// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_emit_snapshot.cpp
/// @brief The long-mode / snapshot-backed half of `ir_emit_block`, taken
///        when `m_ir_plan.snapshot_path` is set. Overview and shared
///        helpers are in ir_emit_internal.hpp.

#include "ir_emit_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
// ARM condition encodings (Arm ARM C1.2.4).
constexpr int kArmCs = 0x2;
constexpr int kArmCc = 0x3;
}  // namespace

void JitCompiler::ir_emit_snapshot_block(const BasicBlock& block) {
    const ir::IrBlock& ir_block = m_ir_block;

    // Snapshot scalar inputs into scratch registers before committing any
    // pin. This preserves the original source for aliased ADD rax,rax and
    // CMP/TEST flag records. `ir_plan_snapshot_block` already decided,
    // at Seam A and under the final pin ABI, where every value lives.
    m_flags.nzcv = NzcvState::None;
    m_flags.pending_carry_source = CarrySource::Unknown;
    // Block entry assumes a linked predecessor's NZCV may still be live
    // (pred_nzcv_live), for the stencil Jcc's carried-kind probe. Nothing
    // in this emitter reads a predecessor's NZCV -- its branches use its
    // own S-form claims or the lazy record, and every exit clears the
    // kind byte -- so holding it only made every store's cold stub save
    // and restore NZCV around its helper.
    m_flags.pred_nzcv_live = false;
    m_exit_kind_cleared = false;
    // The block's recovery descriptors are this tier's plans, not the ones
    // the stencil planner made for code that is not being emitted.
    // Finalization translates them to per-instruction descriptors under the
    // block's pin layout, and an active plan makes the raise exit name the
    // block, so execute() rebuilds an elided record after a SOFTWARE raise
    // as the fault handler does after a hardware fault.
    if (ir_block.is_64bit) {
        m_stencil_flag_recovery.clear();
        for (const ir::FlagRecoveryPlan& p : ir_block.flag_recovery) {
            if (!p.active()) continue;
            StencilFlagRecovery s{};
            s.flag_op = p.flag_op;
            s.recompute_dst = p.recompute_dst;
            s.src_is_imm = p.src_is_imm;
            s.src_reg = p.src_reg;
            s.src_imm = p.src_imm;
            s.dst_is_imm = p.dst_is_imm;
            s.dst_reg = p.dst_reg;
            s.dst_imm = p.dst_imm;
            s.def_rip = p.def_rip;
            s.first_rip = p.first_rip;
            s.last_rip = p.last_rip;
            s.width = p.width;
            s.merge_cf = p.merge_cf;
            m_stencil_flag_recovery.push_back(s);
        }
    }
    const IrSnapshotPlan& plan = m_ir_plan;
    const usize op_count = ir_block.insts.size();
    const bool pooled = plan.pooled;
    std::vector<u8>& loc = m_ir_plan.loc;
    loc.assign(ir_block.next_value, 0xFF);
    std::vector<u32>& mat_seg = m_ir_plan.mat_seg;
    mat_seg.assign(ir_block.next_value, 0xFFFFFFFFu);
    // Vector values get their own map and their own bump allocator. They
    // never compete for the GPR pool, never take a spill slot and never
    // live across a may-fault op -- feasibility proved the last part --
    // so a bump over the caller-saved V registers is the whole allocator.
    std::vector<u8> vloc(ir_block.next_value, 0xFF);
    u8 vnext = 0;
    // CondExit taken edges, emitted after the body so the trace's hot
    // fall-through stays sequential (as the stencil path's cold Jcc
    // exits do). `arm` < 0: a CBNZ on the evaluated W0.
    // `carry`: the exit's NZCV claim, Carried once the hot body cleared the
    // kind byte ahead of its branch (emit_mid_exit_kind_clear).
    // `record`: an exit-deferred flag record (FlagDef::exit_deferred) the
    // stub publishes before it leaves -- the taken edge is the only path on
    // which the record is architecturally live. Its operands are still in
    // `src` / `dst`, untouched between the B.cond and the stub.
    struct DeferredRecord {
        bool valid{false};
        FlagState::FlagOp op{FlagState::FlagOp::None};
        u8 src{0};
        u8 dst{0};
        bool wide{false};
        ir::ValueId flags{ir::kInvalidValue};
    };
    struct ColdExit {
        usize branch_pos; u64 target; int arm; NzcvState carry;
        DeferredRecord record{};
    };
    std::vector<ColdExit> cold_exits;
    // The exit-deferred record of the definition just emitted, until the
    // exit that reads it (or anything the deferral did not plan for) is
    // reached. Publishing it is always correct; deferring is the saving.
    DeferredRecord pending_record{};
    // An exit-deferred record's operand must sit where nothing between its
    // definition and the exit (full-width commits only) can take it: the
    // zero register, a guest pin, or the value's own pool register. A
    // use-time register (a remat or reload target, a non-pooled scratch) is
    // reused by the next op's operands, so the record is published at once.
    const auto exit_record_operand_safe = [&](const ir::Inst& def, u32 k) {
        const ir::Operand& o = def.operands[k];
        if (o.kind != ir::OperandKind::Value || o.type == ir::Type::Flags ||
            o.type == ir::Type::V128) return false;
        const ir::ValueId v = o.as_value();
        const u8 r = loc[v];
        if (r == 31) return true;
        if (r == REG_SCRATCH3 || r == REG_SCRATCH4 || r == kIrSurvivor)
            return false;
        for (u8 g = 0; g < 16; ++g) {
            if (pinned_gpr64(g) == r) return true;
        }
        return pooled && plan.assignment[v] == r;
    };
    const auto publish_record = [&](const DeferredRecord& r) {
        m_omit_lazy_current = false;
        m_omit_lazy_keep_overlay = false;
        if (r.wide) emit_set_lazy_flags64(r.op, r.src, r.dst);
        else emit_set_lazy_flags(r.op, r.src, r.dst);
    };
    const auto flush_pending_record = [&] {
        if (!pending_record.valid) return;
        publish_record(pending_record);
        pending_record = DeferredRecord{};
    };
    const auto emit_remat = [&](u8 dst, ir::ValueId value) {
        if (plan.remat_kind[value] == 1) {
            if (plan.remat_wide[value])
                emit_mov_imm64(dst, static_cast<i64>(plan.remat_bits[value]));
            else emit_mov_imm32(dst, static_cast<u32>(plan.remat_bits[value]));
        } else {
            if (plan.remat_wide[value]) emit_load_gpr64(dst, plan.remat_reg[value]);
            else emit_load_gpr32(dst, plan.remat_reg[value]);
        }
    };
    const auto spill_base = [&]() {
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX, JIT_CTX_OFF_IR_SPILL_FRAME >> 3));
    };
    // The record kind a flag reader will find, for the lazy-condition
    // evaluator's predicted arm (m_flags.static_lazy_kind). The reader
    // names its definition, and when that definition published a record
    // in this block the kind byte is that record's: the generic walk
    // tests ~20 kinds inline where one is needed. Advisory, exactly as on
    // the stencil path -- the arm stays behind the runtime kind and
    // overlay guards, so a by-CL shift that skipped its publication, or a
    // helper that rewrote the record, takes the helper and is still
    // right.
    // A RIP-relative, moffs or displacement-only address is a 64-bit
    // constant, and the register about to be handed to the access holds
    // exactly it (a constant has no other definition). Telling the linear64
    // emitters so lets them decide the reservation and page-crossing
    // tests at compile time, as the stencil path's emit_compute_ea64 does
    // (m_const_ea); `take_const_ea` ignores it unless nothing has been
    // emitted since, and the register matches.
    const auto note_const_address = [&](const ir::Inst& access) {
        const ir::Operand& a = access.operands[0];
        if (a.kind != ir::OperandKind::Value) return;
        const ir::ValueId v = a.as_value();
        if (plan.remat_kind[v] != 1 || !plan.remat_wide[v]) return;
        m_const_ea = ConstEa{true, loc[v], plan.remat_bits[v],
                             m_code_buf.size()};
    };
    const auto predict_lazy_kind = [&](usize reader_at) {
        m_flags.static_lazy_kind = kLazyKindUnknown;
        const ir::Inst& reader = ir_block.insts[reader_at];
        if (reader.operand_count == 0 ||
            reader.operands[0].kind != ir::OperandKind::Value ||
            reader.operands[0].type != ir::Type::Flags) return;
        const ir::ValueId flags = reader.operands[0].as_value();
        for (usize j = reader_at; j-- > 0;) {
            const ir::Inst& d = ir_block.insts[j];
            if (d.op != ir::Op::DefFlags || d.result != flags) continue;
            if (d.flags.repr == ir::FlagRepr::InCpuStateRecord &&
                d.flags.record != FlagState::FlagOp::None) {
                m_flags.static_lazy_kind = static_cast<int>(d.flags.record);
            }
            return;
        }
    };

    // ---- Zero constants need no register ---------------------------
    //
    // ARM64 reads register 31 as WZR/XZR in every data-processing operand
    // position, so a zero constant can be bound there instead of being
    // materialized -- removing both the MOV and the pool slot. NOT in an
    // ADDRESS position: register 31 is SP there, not zero, so a zero
    // constant that reaches a Load/Store address or a Lea base is
    // materialized normally.
    std::vector<u8> bind_zero(ir_block.next_value, 0);
    // ---- Results consumed only by a flag definition ------------------
    //
    // `cmp` and `test` discard their result; only the lazy record wants
    // it. That alone does not let the ARM result register go away -- the
    // record stores the value, and recovers the first operand from it --
    // but it does when the SECOND operand is zero, because the result is
    // then the first operand unchanged. That is the one-instruction
    // compare shape, and the same special case the stencil compiler has.
    std::vector<u8> flags_only(ir_block.next_value, 0);
    // Both vectors exist only to serve a ZERO constant: `flags_only` is
    // consulted only where an operand already resolved to the zero
    // register. A block with no zero constant -- most of them -- pays one
    // linear scan instead of two nested ones.
    bool any_zero_const = false;
    for (const ir::Inst& in : ir_block.insts) {
        if (in.op == ir::Op::Const && in.operands[0].payload == 0 &&
            in.type != ir::Type::Flags) {
            any_zero_const = true;
            break;
        }
    }
    if (any_zero_const) {
        std::vector<u8> unsafe(ir_block.next_value, 0);
        std::vector<u8> non_flag_use(ir_block.next_value, 0);
        std::vector<u8> any_use(ir_block.next_value, 0);
        for (const ir::Inst& u : ir_block.insts) {
            for (u32 k = 0; k < u.operand_count; ++k) {
                const auto& operand = u.operands[k];
                if (operand.kind != ir::OperandKind::Value ||
                    operand.type == ir::Type::Flags) continue;
                const ir::ValueId v = operand.as_value();
                any_use[v] = 1;
                if (u.op != ir::Op::DefFlags) non_flag_use[v] = 1;
                const bool address_position =
                    (u.op == ir::Op::Load && k == 0) ||
                    (u.op == ir::Op::Store && k == 0) ||
                    (u.op == ir::Op::Lea) ||
                    (u.op == ir::Op::SegBase);
                if (address_position) unsafe[v] = 1;
            }
        }
        for (const ir::Inst& d : ir_block.insts) {
            if (!d.defines_value() || d.type == ir::Type::Flags) continue;
            if (d.op == ir::Op::Const && d.operands[0].payload == 0 &&
                !unsafe[d.result] && any_use[d.result]) {
                bind_zero[d.result] = 1;
            }
            if (any_use[d.result] && !non_flag_use[d.result])
                flags_only[d.result] = 1;
        }
    }

    // ---- Results whose flags the terminal Jcc may read from PSTATE ----
    //
    // `assign_nzcv_residency` annotates the flag definition the block's
    // terminal Jcc reads when nothing after it can destroy host NZCV.
    // The producing ADD/SUB/AND then emits its S-form and claims the kind;
    // the claim survives only ops that provably leave PSTATE alone (see
    // `nzcv_before` below), so a store or helper between the producer and
    // the branch drops it and the branch falls back to the lazy record.
    // CF-merging (three-operand) definitions are excluded: their carry
    // capture is a helper that clobbers NZCV.
    std::vector<ir::NzcvKind> resident_kind(ir_block.next_value,
                                            ir::NzcvKind::None);
    for (const ir::Inst& d : ir_block.insts) {
        if (d.op != ir::Op::DefFlags) continue;
        // A PSTATE-only definition names its producer directly.
        const bool pstate = d.flags.repr == ir::FlagRepr::InPstate &&
                            d.operand_count == 1;
        // INC/DEC's CF merge: the capture runs at the START of the
        // instruction, before the producer, so it cannot disturb the claim.
        const bool merge = ir_is_cf_merge(d);
        if (!pstate && ((d.operand_count != 2 && !merge) ||
                        d.flags.repr != ir::FlagRepr::InCpuStateRecord)) continue;
        if (d.flags.shift_count != 0) continue;
        const ir::NzcvKind kind = d.flags.nzcv;
        if (merge ? kind != ir::NzcvKind::IncDec
                  : (kind != ir::NzcvKind::Sub && kind != ir::NzcvKind::Add &&
                     kind != ir::NzcvKind::And && kind != ir::NzcvKind::Sub8))
            continue;
        const ir::Operand& r = d.operands[pstate ? 0 : 1];
        if (r.kind != ir::OperandKind::Value) continue;
        ir::ValueId v = r.as_value();
        // ADC/SBB publish their result widened for the record; the S-form
        // is the carry op under the widening.
        if (const ir::Inst& w = ir_block.insts[plan.def_at[v]];
            w.result == v && w.op == ir::Op::Zext &&
            w.operands[0].kind == ir::OperandKind::Value) {
            const ir::ValueId inner = w.operands[0].as_value();
            const ir::Inst& c = ir_block.insts[plan.def_at[inner]];
            if (c.result == inner &&
                (c.op == ir::Op::AddCarry || c.op == ir::Op::SubBorrow))
                v = inner;
        }
        resident_kind[v] = kind;
    }
    // `Sub8` is a 32-bit SUBS of ZERO-EXTENDED bytes: Z and C then match
    // x86 ZF/CF (nzcv_kind_for). Claimed only when both operands are
    // zero-extended by construction -- a byte load (LDRB), a byte
    // register extract (UBFX) or a constant that fits a byte.
    const auto zero_extended_byte = [&](const ir::Operand& o) {
        if (o.kind != ir::OperandKind::Value) return false;
        const ir::Inst& d = ir_block.insts[plan.def_at[o.as_value()]];
        if (d.result != o.as_value()) return false;
        if (d.op == ir::Op::Load) return d.mem.width == ir::Type::I8;
        if (d.op == ir::Op::GprRead) return d.type == ir::Type::I8;
        if (d.op == ir::Op::Const) return d.operands[0].payload <= 0xFFu;
        return false;
    };
    const auto claim_kind = [&](const ir::Inst& producer) {
        const ir::NzcvKind kind = resident_kind[producer.result];
        if (kind == ir::NzcvKind::Sub8) {
            return producer.type == ir::Type::I8 &&
                           zero_extended_byte(producer.operands[0]) &&
                           zero_extended_byte(producer.operands[1])
                       ? kind : ir::NzcvKind::None;
        }
        // A narrow AND claims And through the width-shifted ANDS below.
        if (kind == ir::NzcvKind::And && producer.op == ir::Op::And &&
            (producer.type == ir::Type::I8 || producer.type == ir::Type::I16))
            return kind;
        if (producer.type != ir::Type::I32 && producer.type != ir::Type::I64)
            return ir::NzcvKind::None;
        return kind;
    };

    // EvalCc results taken off a live claim (the ARM condition, or -1),
    // and EvalCc results read only by a Select of their own guest
    // instruction -- those need no CSET when the Select fuses too.
    std::vector<i8> fused_select_cc(ir_block.next_value, -1);
    std::vector<u8> select_only(ir_block.next_value, 0);
    {
        std::vector<u8> other_use(ir_block.next_value, 0);
        std::vector<u8> select_use(ir_block.next_value, 0);
        for (const ir::Inst& u : ir_block.insts) {
            for (u32 k = 0; k < u.operand_count; ++k) {
                if (u.operands[k].kind != ir::OperandKind::Value) continue;
                const ir::ValueId v = u.operands[k].as_value();
                if (v >= ir_block.next_value) continue;
                const ir::Inst& d = ir_block.insts[plan.def_at[v]];
                if (u.op == ir::Op::Select && k == 0 && d.op == ir::Op::EvalCc &&
                    d.result == v && d.guest_rip == u.guest_rip) {
                    select_use[v] = 1;
                } else {
                    other_use[v] = 1;
                }
            }
        }
        for (ir::ValueId v = 0; v < ir_block.next_value; ++v)
            select_only[v] = select_use[v] && !other_use[v];
    }
    // EvalCc results whose one reader is the very next op, an ADC/SBB taking
    // its carry: a fused CS/CC condition is already host C (inverted for CC),
    // so the carry op reads C and no CSET is emitted. `pstate_flags` marks
    // keep_carry_in_pstate's definitions, which have no record to fall back
    // to.
    std::vector<u8> carry_only(ir_block.next_value, 0);
    std::vector<u8> pstate_flags(ir_block.next_value, 0);
    {
        std::vector<u32> uses(ir_block.next_value, 0);
        for (const ir::Inst& u : ir_block.insts) {
            if (u.op == ir::Op::DefFlags && u.flags.repr == ir::FlagRepr::InPstate)
                pstate_flags[u.result] = 1;
            for (u32 k = 0; k < u.operand_count; ++k) {
                if (u.operands[k].kind == ir::OperandKind::Value &&
                    u.operands[k].as_value() < ir_block.next_value)
                    ++uses[u.operands[k].as_value()];
            }
        }
        for (usize at = 1; at < op_count; ++at) {
            const ir::Inst& u = ir_block.insts[at];
            if (u.op != ir::Op::AddCarry && u.op != ir::Op::SubBorrow) continue;
            const ir::Inst& cc = ir_block.insts[at - 1];
            if (cc.op == ir::Op::EvalCc && cc.result == u.operands[2].as_value() &&
                uses[cc.result] == 1) {
                carry_only[cc.result] = 1;
            }
        }
    }

    // ---- `test r, r` of the value the previous ALU op just computed ----
    //
    // Intel SDM Vol. 2B TEST: SF and ZF of r AND r are r's sign bit and zero
    // test, which the producing ADD/SUB/AND's S-form already leaves in N and
    // Z. C and V are the producer's, not TEST's cleared ones, so the claim is
    // NzOnly: JZ/JNZ/JS/JNS fuse, every other reader takes TEST's lazy
    // record, which is still published unchanged. Only a flag-neutral,
    // emission-free gap (register reads and writes, constants, killed flag
    // definitions) may separate the two.
    std::vector<u8> selftest_src(ir_block.next_value, 0);
    std::vector<u8> selftest_and(ir_block.next_value, 0);
    {
        std::vector<u8> value_use(ir_block.next_value, 0);
        for (const ir::Inst& u : ir_block.insts) {
            for (u32 k = 0; k < u.operand_count; ++k) {
                if (u.operands[k].kind != ir::OperandKind::Value) continue;
                const ir::ValueId v = u.operands[k].as_value();
                if (v >= ir_block.next_value) continue;
                const bool record_dst = u.op == ir::Op::DefFlags && k == 1 &&
                                        u.operand_count == 2;
                if (!record_dst && value_use[v] < 2) value_use[v] = 2;
                else if (record_dst && value_use[v] == 0) value_use[v] = 1;
            }
        }
        const auto emitted_here = [&](ir::ValueId v) {
            return !bind_zero[v] && plan.pin_home[v] == 0xFF &&
                   plan.remat_kind[v] == 0;
        };
        for (usize t = 0; t < op_count; ++t) {
            const ir::Inst& test = ir_block.insts[t];
            if (test.op != ir::Op::And || test.operand_count != 2 ||
                (test.type != ir::Type::I32 && test.type != ir::Type::I64) ||
                test.operands[0].kind != ir::OperandKind::Value ||
                test.operands[1].kind != ir::OperandKind::Value ||
                test.operands[0].as_value() != test.operands[1].as_value() ||
                resident_kind[test.result] != ir::NzcvKind::And ||
                value_use[test.result] != 1 || !emitted_here(test.result)) {
                continue;
            }
            // TEST reads the register the producer committed, usually as a
            // fresh read of its pin: look through that read to the value the
            // last same-width write of the register stored.
            ir::ValueId x = test.operands[0].as_value();
            {
                const usize read_at = plan.def_at[x];
                const ir::Inst& read = ir_block.insts[read_at];
                if (read.op == ir::Op::GprRead && read.result == x) {
                    const u8 reg = read.operands[0].reg_index;
                    for (usize j = read_at; j-- > 0;) {
                        const ir::Inst& w = ir_block.insts[j];
                        if (w.op != ir::Op::GprWrite ||
                            ir::architectural_gpr(w.operands[0].type,
                                                  w.operands[0].reg_index) != reg)
                            continue;
                        if (w.operands[0].type == read.type &&
                            w.operands[1].kind == ir::OperandKind::Value)
                            x = w.operands[1].as_value();
                        break;
                    }
                }
            }
            const usize p = plan.def_at[x];
            const ir::Inst& producer = ir_block.insts[p];
            if (p >= t || producer.result != x || producer.type != test.type ||
                producer.operand_count != 2 ||
                (producer.op != ir::Op::Add && producer.op != ir::Op::Sub &&
                 producer.op != ir::Op::And) ||
                resident_kind[x] != ir::NzcvKind::None || !emitted_here(x)) {
                continue;
            }
            bool neutral_gap = true;
            for (usize j = p + 1; j < t && neutral_gap; ++j) {
                const ir::Inst& mid = ir_block.insts[j];
                neutral_gap = mid.op == ir::Op::GprWrite ||
                    mid.op == ir::Op::GprRead || mid.op == ir::Op::Const ||
                    (mid.op == ir::Op::DefFlags &&
                     mid.flags.repr == ir::FlagRepr::Dead);
            }
            if (!neutral_gap) continue;
            selftest_src[x] = 1;
            // The elided AND's result aliases the producer's register, so its
            // one reader must find it resident there: pooled, never backed or
            // carried as a survivor, and read by the very next op, in the
            // same segment. Otherwise the AND is still emitted (an ANDS), and
            // only the fusion is kept.
            const ir::ValueId r = test.result;
            const bool aliasable = pooled && t + 1 < op_count &&
                plan.assignment[r] != 0xFF && !plan.needs_backing[r] &&
                !plan.survivor[r] && plan.commit_home[r] == 0xFF &&
                plan.commit_pin[r] == 0xFF &&
                plan.seg_in[t + 1] == plan.seg_out[t] &&
                ir_block.insts[t + 1].op == ir::Op::DefFlags &&
                ir_block.insts[t + 1].operand_count == 2 &&
                ir_block.insts[t + 1].operands[1].kind == ir::OperandKind::Value &&
                ir_block.insts[t + 1].operands[1].as_value() == r;
            if (aliasable) selftest_and[r] = 1;
        }
    }
    usize op = 0;
    u64 rip = block.start_addr;
    for (u32 index = 0; index < block.instruction_count; ++index) {
        m_diagnostic_instruction_rip = rip;
        const auto& instr = block.instructions[index];
        const u32 arm_before = static_cast<u32>(m_code_buf.size());
        const u32 recorded = m_insn_pos_count;
        if (recorded < kMaxInsnArmPos) {
            m_insn_rips[recorded] = rip;
            m_insn_arm_pos[m_insn_pos_count++] = arm_before;
        }
        // Helpers that raise report the already-published guest RIP; do
        // this before any address/value enters X0-X2. Not for a long-mode
        // Load/Store: as on the stencil x64 path, its cold memory and
        // alignment stubs publish this instruction's RIP themselves
        // (emit_flush_cold_mem_stubs / emit_flush_cold_align_checks find it
        // through m_insn_arm_pos/end), and a hardware fault recovers it
        // from the published host-PC map. Publishing it on the hot path
        // cost four instructions and a store per memory instruction.
        for (usize scan = op; scan < ir_block.insts.size() &&
             ir_block.insts[scan].guest_rip == rip; ++scan) {
            const ir::Inst& site = ir_block.insts[scan];
            if (!site.may_fault) continue;
            if (ir_block.is_64bit &&
                (site.op == ir::Op::Load || site.op == ir::Op::Store)) {
                continue;
            }
            emit_update_rip(rip);
            break;
        }
        // INC/DEC preserve CF (Intel SDM Vol. 2A): capture it into X21
        // at the start of the instruction, before any of its values is
        // live (the planner ends a segment here). The helper clobbers
        // X0-X4 and NZCV.
        for (usize scan = op; scan < ir_block.insts.size() &&
             ir_block.insts[scan].guest_rip == rip; ++scan) {
            if (!ir_is_cf_merge(ir_block.insts[scan])) continue;
            const CarrySource saved = m_flags.incoming_carry_source;
            const NzcvState saved_nzcv = m_flags.incoming_nzcv;
            m_flags.incoming_carry_source =
                in_block_carry_source(ir_block, scan);
            // A live claim here is the latest definition's -- the one the
            // merge inherits from: a published definition without a claim
            // clears it, and only a dead one (which nothing reads) restores
            // an older one.
            m_flags.incoming_nzcv = m_flags.nzcv;
            emit_lazy_cf_to_call_tmp();
            m_flags.incoming_carry_source = saved;
            m_flags.incoming_nzcv = saved_nzcv;
            m_flags.nzcv = NzcvState::None;
            break;
        }
        while (op < ir_block.insts.size() && ir_block.insts[op].guest_rip == rip) {
            const auto& in = ir_block.insts[op++];
            const bool wide = in.type == ir::Type::I64;
            const usize at = op - 1;
            if (pending_record.valid) {
                const bool commit = in.op == ir::Op::GprWrite &&
                    (in.operands[0].type == ir::Type::I32 ||
                     in.operands[0].type == ir::Type::I64);
                const bool reader = in.op == ir::Op::CondExit &&
                    in.operands[0].as_value() == pending_record.flags;
                // A commit into the register holding an operand overwrites
                // it before the exit -- unless the value already IS that
                // register (computed into its pin, or a 64-bit read of it),
                // when the commit emits nothing there.
                bool hazard = !commit && !reader;
                if (commit) {
                    const u8 pin = pinned_gpr64(in.operands[0].reg_index);
                    const ir::ValueId v = in.operands[1].as_value();
                    const bool loaded =
                        ir_block.insts[plan.def_at[v]].op == ir::Op::Load;
                    const bool in_place = !loaded &&
                        (plan.commit_pin[v] == pin ||
                         (in.operands[0].type == ir::Type::I64 &&
                          plan.pin_home[v] == pin));
                    hazard = pin != 0xFF && !in_place &&
                             (pin == pending_record.src ||
                              pin == pending_record.dst);
                }
                if (hazard) flush_pending_record();
            }
            if (in.defines_value() && in.type != ir::Type::Flags &&
                bind_zero[in.result]) {
                loc[in.result] = 31;   // WZR/XZR, materialized by nobody
                continue;
            }
            if (in.defines_value() && in.type != ir::Type::Flags &&
                (plan.pin_home[in.result] != 0xFF || plan.remat_kind[in.result] != 0)) {
                // No definition site: the pin already holds it, or every
                // use recomputes it.
                continue;
            }
            // ADD/SUB of a constant that fits the 12-bit immediate takes
            // the immediate form and never materializes the constant (a
            // later use, such as the flag record, still does). A claimed
            // S-form uses the constant only as given: `adds x, #-k` and
            // `subs x, #k` agree on the result but not on C and V.
            bool imm1 = false;
            bool imm1_negated = false;
            u32 imm1_bits = 0;
            if ((in.op == ir::Op::Add || in.op == ir::Op::Sub) &&
                in.operand_count == 2 &&
                in.operands[1].kind == ir::OperandKind::Value &&
                (in.type == ir::Type::I32 || in.type == ir::Type::I64)) {
                const ir::ValueId c = in.operands[1].as_value();
                // In the immediate forms register 31 as the FIRST operand
                // is SP, so a zero bound there must keep the register form.
                const bool lhs_zero =
                    in.operands[0].kind == ir::OperandKind::Value &&
                    bind_zero[in.operands[0].as_value()];
                if (plan.remat_kind[c] == 1 && !bind_zero[c] && !lhs_zero &&
                    plan.pin_home[c] == 0xFF) {
                    const u64 raw = plan.remat_bits[c];
                    const u64 v = wide ? raw : (raw & 0xFFFFFFFFull);
                    const u64 neg = wide ? (0ull - raw)
                        : static_cast<u64>(0u - static_cast<u32>(raw));
                    if (v < 4096u) {
                        imm1 = true;
                        imm1_bits = static_cast<u32>(v);
                    } else if (claim_kind(in) == ir::NzcvKind::None &&
                               neg < 4096u) {
                        imm1 = true;
                        imm1_negated = true;
                        imm1_bits = static_cast<u32>(neg);
                    }
                }
            }
            // A definition the demand pass killed publishes nothing, so its
            // operands are never read: materializing them would be a dead
            // MOVZ (or reload) per eliminated flag record.
            const bool emits_nothing = in.op == ir::Op::DefFlags &&
                                       (in.flags.repr == ir::FlagRepr::Dead ||
                                        in.flags.repr == ir::FlagRepr::InPstate);
            for (u32 k = 0; k < in.operand_count && !emits_nothing; ++k) {
                if (imm1 && k == 1) continue;   // encoded in the op
                const auto& operand = in.operands[k];
                if (operand.kind != ir::OperandKind::Value ||
                    operand.type == ir::Type::Flags) continue;
                if (operand.type == ir::Type::V128) continue;   // in vloc
                const ir::ValueId value = operand.as_value();
                if (bind_zero[value]) { loc[value] = 31; continue; }
                if (plan.pin_home[value] != 0xFF) { loc[value] = plan.pin_home[value]; continue; }
                if (plan.commit_home[value] != 0xFF && at > plan.commit_at[value]) {
                    loc[value] = plan.commit_home[value];
                    continue;
                }
                if (plan.survivor[value]) { loc[value] = kIrSurvivor; continue; }
                if (plan.commit_pin[value] != 0xFF &&
                    ir_block.insts[plan.def_at[value]].op != ir::Op::Load) {
                    loc[value] = plan.commit_pin[value];
                    continue;
                }
                if (plan.remat_kind[value] != 0 &&
                    (!pooled || plan.assignment[value] == 0xFF)) {
                    // Evicted from the pool: recomputed for this use, in a
                    // register outside it; never resident across ops.
                    const u8 home = kIrRematAt[k < 3 ? k : 2];
                    emit_remat(home, value);
                    loc[value] = home;
                    mat_seg[value] = 0xFFFFFFFFu;
                    continue;
                }
                const bool spilled = pooled && plan.assignment[value] == 0xFF;
                const bool resident = pooled && !spilled &&
                                      mat_seg[value] == plan.seg_in[at];
                if (resident) continue;  // still in its pool register
                // A spilled value reloads for every use into a use-time
                // register outside the pool, and is never resident.
                const u8 home = spilled ? kIrRematAt[k < 3 ? k : 2]
                    : pooled ? plan.assignment[value] : kIrScratch[k];
                if (plan.remat_kind[value] != 0) {
                    emit_remat(home, value);   // in its pool register
                } else {
                    spill_base();
                    emit(arm64::ldr_x_uoff(home, REG_SCRATCH3,
                        plan.spill_slot[value] * 2));
                }
                loc[value] = home;
                mat_seg[value] = spilled ? 0xFFFFFFFFu : plan.seg_in[at];
            }
            u8 dst = 0;
            if (in.defines_value() && in.type != ir::Type::Flags &&
                in.type != ir::Type::V128) {
                // A spilled result is written to X5 and stored to its slot
                // right after (needs_backing); X5 is outside the pool.
                dst = pooled ? (plan.assignment[in.result] != 0xFF
                                    ? plan.assignment[in.result] : u8{5})
                             : REG_SCRATCH2;
                // A MOV load may write its destination pin directly: the
                // existing memory primitive commits the load only after a
                // successful access (including its cold helper path). Not
                // when the pin is also the address: the cold path still
                // needs the address it was given.
                if (in.op == ir::Op::Load && op < ir_block.insts.size()) {
                    const auto& commit = ir_block.insts[op];
                    // Only a FULL-width commit may take this shortcut. A
                    // narrow one is a bitfield merge, and letting the load
                    // write the pin directly would zero-extend over the
                    // bits x86 requires it to preserve — `mov dx,[rcx]`
                    // would clear RDX[63:16].
                    const ir::Type commit_type = commit.operands[0].type;
                    if (commit.op == ir::Op::GprWrite && commit.guest_rip == rip &&
                        commit_type != ir::Type::I8 && commit_type != ir::Type::I16 &&
                        commit.operands[1].as_value() == in.result) {
                        const u8 pin = wide ? pinned_gpr64(commit.operands[0].reg_index)
                                            : pinned_gpr32(commit.operands[0].reg_index);
                        if (pin != 0xFF && pin != loc[in.operands[0].as_value()])
                            dst = pin;
                    }
                }
                if (plan.commit_pin[in.result] != 0xFF &&
                    !(in.op == ir::Op::Load &&
                      loc[in.operands[0].as_value()] == plan.commit_pin[in.result])) {
                    dst = plan.commit_pin[in.result];
                    if (const i8 undo = plan.commit_undo[in.result]; undo != 0) {
                        if (recorded >= kMaxInsnArmPos) {
                            std::fprintf(stderr,
                                         "[ir] FATAL: early RSP commit on an "
                                         "instruction with no fault site; "
                                         "block 0x%llx\n",
                                         static_cast<unsigned long long>(
                                             ir_block.entry_rip));
                            std::abort();
                        }
                        m_insn_rsp_undo[recorded] = undo;
                    }
                }
                if (plan.survivor[in.result]) dst = kIrSurvivor;
                loc[in.result] = dst;
                // A spilled result (X5) is reloaded at every use.
                mat_seg[in.result] = pooled && plan.assignment[in.result] == 0xFF &&
                                             dst == 5
                    ? 0xFFFFFFFFu : plan.seg_out[at];
            }
            const auto source = [&](u32 k) { return loc[in.operands[k].as_value()]; };
            // `edge` is the constant-successor class, as the stencil
            // lowering of the same branch passes it, so the
            // VITREO_JIT_LINK_EDGES mask means the same thing on both
            // tiers (every class but CallRel is enabled by default).
            const auto exit_to = [&](u64 target, LinkEdge edge) {
                emit_update_rip(target);
                if (!emit_linked_tail_continue(
                        target,
                        exit_carry_after_kind_clear(target, NzcvState::None),
                        edge)) {
                    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                           jit_iters_offset() >> 2));
                    emit_epilogue(JitExecResult::Continue);
                }
            };
            // Host NZCV is invalidated by DEFAULT and re-claimed only by
            // the op that publishes it. An op that provably leaves PSTATE
            // alone restores this; anything that can emit an S-form, a
            // bounds-check SUBS or a helper BLR must not. Defaulting the
            // other way is how a fused branch reads a condition some
            // intervening lowering already destroyed.
            const NzcvState nzcv_before = m_flags.nzcv;
            m_flags.nzcv = NzcvState::None;
            // Set by a lowering that emits a flag-setting instruction
            // without making it a claim (the `cmp reg, 0` form of Sub).
            bool flags_written = false;
            switch (in.op) {
            case ir::Op::FlagsIn:
                m_flags.nzcv = nzcv_before;
                break;
            case ir::Op::Jmp:
                if (in.branch.return_site != ir::kInvalidRip &&
                    ir_block.is_64bit) {
                    // CALL rel32, as the x64 stencil lowers it: the native
                    // call continuation when it applies, otherwise a
                    // CallRel tail. No return-address predictor push --
                    // ops64.cpp CALL_REL32 measured the RSB probe and its
                    // pushes as pure added latency in front of an L1
                    // lookup that already hits 97-100% on x64.
                    if (emit_linked_call_continue(in.branch.target,
                                                  in.branch.return_site)) {
                        break;
                    }
                    exit_to(in.branch.target, LinkEdge::CallRel);
                    break;
                }
                // Prime the return-address predictor before linking to the
                // callee, and BEFORE `exit_to` -- `emit_rsb_push` clobbers
                // X2/X16/X17/W3/W4, and the tail wants the target in X0.
                // Without it every matching RET misses its RSB slot, which
                // is most of the 7.7x call/ret regression the 32-bit path
                // already measured.
                if (in.branch.return_site != ir::kInvalidRip)
                    emit_rsb_push(in.branch.return_site);
                exit_to(in.branch.target, in.branch.return_site != ir::kInvalidRip
                                              ? LinkEdge::CallRel
                                              : LinkEdge::JmpRel);
                break;
            case ir::Op::JmpIndirect: {
                // The target is DATA, so there is no static successor and
                // no compile-time RIP to publish. Unlike the 32-bit path
                // this needs no zero-extending MOV: a long-mode target is
                // already the full 64-bit value.
                if (source(0) != REG_SCRATCH0)
                    emit(arm64::mov_x(REG_SCRATCH0, source(0)));
                if (ir_block.is_64bit) {
                    // As the x64 stencil lowers RET and CALL/JMP r/m
                    // (ops64.cpp): no return-address predictor on either
                    // side, and RIP stays register-only on a linked hop --
                    // the probe tail publishes it on its miss path, and the
                    // fallback below publishes it before the epilogue.
                    m_rip_reg_pos = static_cast<usize>(-1);
                    if (!emit_linked_tail_reg(REG_SCRATCH0, NzcvState::None,
                                              /*try_rsb_first=*/false,
                                              /*native_ret=*/false,
                                              /*api_call_tail=*/!in.branch.is_return)) {
                        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                               rip_offset() >> 3));
                        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                               jit_iters_offset() >> 2));
                        emit_epilogue(JitExecResult::Continue);
                    }
                    break;
                }
                // Intel SDM Vol. 2A CALL: DEST survives the return-address
                // push. Prediction bookkeeping also clobbers X2, which
                // may be the IR value's home. X0 survives that bookkeeping.
                if (in.branch.return_site != ir::kInvalidRip)
                    emit_rsb_push(in.branch.return_site);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       rip_offset() >> 3));
                m_rip_reg_pos = static_cast<usize>(-1);
                // A RET pops the return-address predictor before falling
                // back to the L1 lookup; an indirect JMP has no prediction
                // to make.
                const bool linked =
                    in.branch.is_return
                        ? emit_linked_tail_ret(REG_SCRATCH0, NzcvState::None)
                        : emit_linked_tail_reg(REG_SCRATCH0, NzcvState::None,
                                               /*try_rsb_first=*/false,
                                               /*native_ret=*/false,
                                               /*api_call_tail=*/true);
                if (!linked) {
                    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                           jit_iters_offset() >> 2));
                    emit_epilogue(JitExecResult::Continue);
                }
                break;
            }
            case ir::Op::CondExit: {
                // A mid-trace branch: taken leaves through the ordinary
                // linked exit, not-taken falls through into the next op.
                // Every guest register write before it is committed, and
                // the flags it leaves with were published by the
                // definition it names (eliminate_dead_flags keeps it).
                // `emit_eval_lazy_condition` clobbers W0-W4 and NZCV and
                // may call its helper, which is why the plan ends a
                // segment here; m_flags.nzcv stays None.
                // The producer's S-form claim survived to here
                // (assign_nzcv_residency) when this maps; otherwise the
                // condition is evaluated into W0. Either way the taken
                // edge branches to an exit emitted after the body.
                const int arm = fused_arm_cond(nzcv_before, in.branch.cond,
                                               KindGuarded{});
                DeferredRecord exit_record{};
                if (pending_record.valid) {
                    // The evaluator reads the record, so an unfused branch
                    // needs it published first; a fused one leaves it to
                    // the taken edge's stub.
                    if (arm < 0) flush_pending_record();
                    else exit_record = pending_record;
                    pending_record = DeferredRecord{};
                }
                if (arm < 0) {
                    predict_lazy_kind(at);
                    emit_eval_lazy_condition(in.branch.cond);
                    m_flags.static_lazy_kind = kLazyKindUnknown;
                }
                emit_mid_exit_kind_clear(in.branch.target);
                cold_exits.push_back(ColdExit{
                    m_code_buf.size(), in.branch.target, arm,
                    exit_carry_after_kind_clear(in.branch.target,
                                                NzcvState::None),
                    exit_record});
                emit(0);  // B.cond / CBNZ W0 -> cold exit
                break;
            }
            case ir::Op::Jcc: {
                // IN-BLOCK LOOP, as on the 32-bit IR path and Tier 0: a
                // taken edge back to this block's own start branches to
                // the body instead of leaving through the tail, up to
                // MAX_IN_BLOCK_LOOP_ITERS. Re-entering at the body start is
                // sound here: every architectural value is committed at
                // the terminator, and the body re-derives everything else
                // from pins, CpuState and its own definitions. The cap
                // exit falls into the ordinary taken-edge tail, which
                // publishes the TARGET rip.
                const auto loop_back = [&] {
                    if (in.branch.target != m_block_start_addr) return;
                    static_assert(MAX_IN_BLOCK_LOOP_ITERS == 0xFFFFu);
                    emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                    emit(arm64::tbnz(REG_LOOP_CTR, 16, 8));
                    emit(arm64::b_imm(static_cast<i32>(
                        m_body_start_pos - m_code_buf.size()) * 4));
                    // Undo the increment so the published iteration count
                    // stays exact on the exit path.
                    emit(arm64::sub_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                };
                // The architectural lazy record stays authoritative for
                // every condition the residency claim does not cover;
                // `fused_arm_cond` is what decides which those are.
                const int arm = fused_arm_cond(nzcv_before, in.branch.cond,
                                               KindGuarded{});
                if (arm >= 0) {
                    // Branch AROUND the taken exit on the inverted
                    // condition. ARM64 condition codes invert by toggling
                    // their low bit.
                    const usize skip = m_code_buf.size();
                    emit(0);
                    loop_back();
                    exit_to(in.branch.target, LinkEdge::TermJcc);
                    m_code_buf[skip] = arm64::b_cond(
                        static_cast<u8>(arm ^ 1),
                        static_cast<i32>(m_code_buf.size() - skip) * 4);
                    exit_to(in.branch.fallthrough, LinkEdge::TermFall);
                    vitreo::diagnostics::perf_note_jcc_emit_fused();
                    break;
                }
                predict_lazy_kind(at);
                emit_eval_lazy_condition(in.branch.cond);
                m_flags.static_lazy_kind = kLazyKindUnknown;
                const usize skip = m_code_buf.size();
                emit(0);
                loop_back();
                exit_to(in.branch.target, LinkEdge::TermJcc);
                m_code_buf[skip] = arm64::cbz_w(0,
                    static_cast<i32>(m_code_buf.size() - skip) * 4);
                exit_to(in.branch.fallthrough, LinkEdge::TermFall);
                break;
            }
            case ir::Op::Const:
                if (wide) emit_mov_imm64(dst, static_cast<i64>(in.operands[0].payload));
                else emit_mov_imm32(dst, static_cast<u32>(in.operands[0].payload));
                break;
            case ir::Op::GprRead: {
                if (in.type == ir::Type::I8 || in.type == ir::Type::I16) {
                    // Extract, never alias. Without REX, Reg8 index 4-7 is
                    // the HIGH byte of GPR 0-3; `architectural_gpr` and
                    // `subregister_lsb` are the single answer to which
                    // register and which field, shared with the verifier.
                    const u8 index = in.operands[0].reg_index;
                    const u8 guest = ir::architectural_gpr(in.type, index);
                    const u8 pin = ir_block.is_64bit ? pinned_gpr64(guest)
                                                     : pinned_gpr32(guest);
                    u8 field_src = dst;
                    if (pin != 0xFF) field_src = pin;
                    else if (ir_block.is_64bit) emit_load_gpr64(dst, guest);
                    else emit_load_gpr32(dst, guest);
                    emit(arm64::ubfx_w(dst, field_src,
                                       ir::subregister_lsb(in.type, index),
                                       in.type == ir::Type::I8 ? 8 : 16));
                    break;
                }
                if (wide) emit_load_gpr64(dst, in.operands[0].reg_index);
                else emit_load_gpr32(dst, in.operands[0].reg_index);
                break;
            }
            case ir::Op::Lea: {
                // Intel SDM Vol. 1 3.3.7: EA wraps at its own width,
                // before Zext and full-width FS/GS addition.
                // base + index << scale in one shifted-register ADD, then
                // the displacement as an immediate when it fits either way
                // round (at a W width the subtraction wraps exactly as
                // the addition would).
                u8 acc = source(0);
                if (in.operands[1].kind == ir::OperandKind::Value) {
                    const u8 shift = scale_shift(in.operands[2].payload);
                    emit(wide ? arm64::add_x_lsl(dst, source(0), source(1), shift)
                              : arm64::add_w_lsl(dst, source(0), source(1), shift));
                    acc = dst;
                }
                const u64 raw = in.operands[3].payload;
                const u64 disp = wide ? raw : (raw & 0xFFFFFFFFull);
                const u64 neg = wide ? (0ull - raw)
                    : static_cast<u64>(0u - static_cast<u32>(raw));
                if (disp == 0) {
                    if (acc != dst) {
                        emit(wide ? arm64::mov_x(dst, acc) : arm64::mov_w(dst, acc));
                    }
                } else if (disp < 4096u) {
                    emit(wide ? arm64::add_x_imm(dst, acc, static_cast<u32>(disp))
                              : arm64::add_w_imm(dst, acc, static_cast<u32>(disp)));
                } else if (neg < 4096u) {
                    emit(wide ? arm64::sub_x_imm(dst, acc, static_cast<u32>(neg))
                              : arm64::sub_w_imm(dst, acc, static_cast<u32>(neg)));
                } else {
                    if (wide) emit_mov_imm64(REG_SCRATCH3, static_cast<i64>(raw));
                    else emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(raw));
                    emit(wide ? arm64::add_x(dst, acc, REG_SCRATCH3)
                              : arm64::add_w(dst, acc, REG_SCRATCH3));
                }
                break;
            }
            case ir::Op::Trunc:
                // Keep the low bits. A W-form move already drops bits
                // 63:32; below that the field has to be masked, because a
                // narrow consumer reads the value as a whole.
                if (in.type == ir::Type::I8) emit(arm64::uxtb_w(dst, source(0)));
                else if (in.type == ir::Type::I16) emit(arm64::uxth_w(dst, source(0)));
                else emit(arm64::mov_w(dst, source(0)));
                break;
            case ir::Op::Zext: {
                // A W-form result zeroes bits 63:32, so every zero
                // extension — including to I64 — is a W instruction.
                const ir::Type from = in.operands[0].type;
                // A narrow source already zero-extended by construction --
                // the Trunc above, an extracting GprRead, a narrow load --
                // needs no second mask (`movzx ecx, cl`).
                bool extended = false;
                if ((from == ir::Type::I8 || from == ir::Type::I16) &&
                    in.operands[0].kind == ir::OperandKind::Value) {
                    const ir::Inst& d = ir_block.insts[plan.def_at[in.operands[0].as_value()]];
                    extended = d.result == in.operands[0].as_value() && d.type == from &&
                        (d.op == ir::Op::Trunc || d.op == ir::Op::GprRead ||
                         d.op == ir::Op::Load);
                }
                if (extended) {
                    if (dst != source(0)) emit(arm64::mov_w(dst, source(0)));
                } else if (from == ir::Type::I8) emit(arm64::uxtb_w(dst, source(0)));
                else if (from == ir::Type::I16) emit(arm64::uxth_w(dst, source(0)));
                else emit(arm64::mov_w(dst, source(0)));
                break;
            }
            case ir::Op::Sext: {
                const ir::Type from = in.operands[0].type;
                if (wide) {
                    if (from == ir::Type::I8) emit(arm64::sxtb_x(dst, source(0)));
                    else if (from == ir::Type::I16) emit(arm64::sxth_x(dst, source(0)));
                    else emit(arm64::sxtw_x(dst, source(0)));
                } else {
                    emit(from == ir::Type::I8 ? arm64::sxtb_w(dst, source(0))
                                              : arm64::sxth_w(dst, source(0)));
                }
                break;
            }
            case ir::Op::SegBase:
                if (wide) emit(arm64::ldr_x_uoff(dst, REG_CPU_STATE,
                    seg_base_offset(static_cast<SegReg>(in.operands[0].payload)) >> 3));
                else emit(arm64::ldr_w_uoff(dst, REG_CPU_STATE,
                    seg_base_offset(static_cast<SegReg>(in.operands[0].payload)) >> 2));
                break;
            case ir::Op::Load:
                // A 128-bit load fills a VECTOR register, so it allocates
                // out of `vloc` rather than taking a GPR destination. The
                // aligned spellings check first: two words here and an
                // out-of-line stub that raises, which is why the check is
                // never a reason to decline the form.
                if (in.mem.width == ir::Type::V128) {
                    if (in.mem.align16) emit_align_check(source(0), false);
                    const u8 v = vnext++;
                    vloc[in.result] = v;
                    emit_guest_load128(v, source(0));
                    break;
                }
                // Narrow loads zero-extend into the destination register;
                // a following Sext re-signs it, which is what the guest
                // form asked for.
                note_const_address(in);
                if (in.mem.width == ir::Type::I8) emit_guest_load8(dst, source(0));
                else if (in.mem.width == ir::Type::I16)
                    emit_guest_load16_64(dst, source(0));
                else if (wide) emit_guest_load64(dst, source(0));
                else if (ir_block.is_64bit) emit_guest_load32_64(dst, source(0));
                else emit_guest_load32(dst, source(0));
                m_const_ea.valid = false;
                break;
            case ir::Op::Store:
                if (in.mem.width == ir::Type::V128) {
                    if (in.mem.align16) emit_align_check(source(0), false);
                    emit_guest_store128(vloc[in.operands[1].as_value()],
                                        source(0));
                    break;
                }
                // A scalar store carries a live NZCV claim through
                // (assign_nzcv_residency): with the claim set, the store
                // primitive's should_hold_nzcv() preserves the flags --
                // in its cold stub where the check is NZCV-clean, around
                // the access where it is not -- so the claim still holds
                // after it.
                m_flags.nzcv = nzcv_before;
                note_const_address(in);
                if (in.mem.width == ir::Type::I8) emit_guest_store8(source(1), source(0));
                else if (in.mem.width == ir::Type::I16)
                    emit_guest_store16_64(source(1), source(0));
                else if (in.mem.width == ir::Type::I64)
                    emit_guest_store64(source(1), source(0));
                else if (ir_block.is_64bit) emit_guest_store32_64(source(1), source(0));
                else emit_guest_store32(source(1), source(0));
                m_const_ea.valid = false;
                break;
            case ir::Op::Add:
                if (selftest_src[in.result]) {
                    if (imm1 && imm1_negated)
                        emit(wide ? arm64::subs_x_imm(dst, source(0), imm1_bits)
                                  : arm64::subs_w_imm(dst, source(0), imm1_bits));
                    else if (imm1)
                        emit(wide ? arm64::adds_x_imm(dst, source(0), imm1_bits)
                                  : arm64::adds_w_imm(dst, source(0), imm1_bits));
                    else
                        emit(wide ? arm64::adds_x(dst, source(0), source(1))
                                  : arm64::adds_w(dst, source(0), source(1)));
                    m_flags.nzcv = NzcvState::NzOnly;
                    break;
                }
                // INC claims IncDec off the same ADDS: N, Z and V are INC's
                // SF/ZF/OF; fused_arm_cond keeps CF readers off it.
                if (imm1) {
                    if (const ir::NzcvKind kind = claim_kind(in);
                        kind == ir::NzcvKind::Add || kind == ir::NzcvKind::IncDec) {
                        emit(wide ? arm64::adds_x_imm(dst, source(0), imm1_bits)
                                  : arm64::adds_w_imm(dst, source(0), imm1_bits));
                        m_flags.nzcv = static_cast<NzcvState>(kind);
                    } else if (imm1_negated) {
                        emit(wide ? arm64::sub_x_imm(dst, source(0), imm1_bits)
                                  : arm64::sub_w_imm(dst, source(0), imm1_bits));
                    } else {
                        emit(wide ? arm64::add_x_imm(dst, source(0), imm1_bits)
                                  : arm64::add_w_imm(dst, source(0), imm1_bits));
                    }
                    break;
                }
                if (const ir::NzcvKind kind = claim_kind(in);
                    kind == ir::NzcvKind::Add || kind == ir::NzcvKind::IncDec) {
                    emit(wide ? arm64::adds_x(dst, source(0), source(1))
                              : arm64::adds_w(dst, source(0), source(1)));
                    m_flags.nzcv = static_cast<NzcvState>(kind);
                    break;
                }
                emit(wide ? arm64::add_x(dst, source(0), source(1))
                          : arm64::add_w(dst, source(0), source(1))); break;
            case ir::Op::Sub:
                if (selftest_src[in.result]) {
                    if (imm1 && imm1_negated)
                        emit(wide ? arm64::adds_x_imm(dst, source(0), imm1_bits)
                                  : arm64::adds_w_imm(dst, source(0), imm1_bits));
                    else if (imm1)
                        emit(wide ? arm64::subs_x_imm(dst, source(0), imm1_bits)
                                  : arm64::subs_w_imm(dst, source(0), imm1_bits));
                    else
                        emit(wide ? arm64::subs_x(dst, source(0), source(1))
                                  : arm64::subs_w(dst, source(0), source(1)));
                    m_flags.nzcv = NzcvState::NzOnly;
                    break;
                }
                if (imm1) {
                    if (const ir::NzcvKind kind = claim_kind(in);
                        kind == ir::NzcvKind::Sub || kind == ir::NzcvKind::IncDec) {
                        emit(wide ? arm64::subs_x_imm(dst, source(0), imm1_bits)
                                  : arm64::subs_w_imm(dst, source(0), imm1_bits));
                        m_flags.nzcv = static_cast<NzcvState>(kind);
                    } else if (imm1_negated) {
                        emit(wide ? arm64::add_x_imm(dst, source(0), imm1_bits)
                                  : arm64::add_w_imm(dst, source(0), imm1_bits));
                    } else {
                        emit(wide ? arm64::sub_x_imm(dst, source(0), imm1_bits)
                                  : arm64::sub_w_imm(dst, source(0), imm1_bits));
                    }
                    break;
                }
                // `cmp reg, 0` in one instruction. Subtracting zero leaves
                // the first operand unchanged, so when nothing but the
                // flag definition wants the result there is nothing to
                // compute: the result IS the first operand, and all that
                // remains is publishing NZCV. Rd is the zero register,
                // which discards.
                if (source(1) == 31 && flags_only[in.result]) {
                    emit(wide ? arm64::subs_x(31, source(0), 31)
                              : arm64::subs_w(31, source(0), 31));
                    flags_written = true;
                    loc[in.result] = source(0);
                    const ir::NzcvKind kind = claim_kind(in);
                    if (kind == ir::NzcvKind::Sub || kind == ir::NzcvKind::Sub8)
                        m_flags.nzcv = static_cast<NzcvState>(kind);
                    break;
                }
                if (const ir::NzcvKind kind = claim_kind(in);
                    kind == ir::NzcvKind::Sub || kind == ir::NzcvKind::Sub8 ||
                    kind == ir::NzcvKind::IncDec) {
                    emit(wide ? arm64::subs_x(dst, source(0), source(1))
                              : arm64::subs_w(dst, source(0), source(1)));
                    m_flags.nzcv = static_cast<NzcvState>(kind);
                    break;
                }
                emit(wide ? arm64::sub_x(dst, source(0), source(1))
                          : arm64::sub_w(dst, source(0), source(1))); break;
            case ir::Op::And:
                if (selftest_and[in.result] && nzcv_before == NzcvState::NzOnly) {
                    loc[in.result] = source(0);
                    m_flags.nzcv = NzcvState::NzOnly;
                    break;
                }
                if (selftest_src[in.result]) {
                    emit(wide ? arm64::ands_x(dst, source(0), source(1))
                              : arm64::ands_w(dst, source(0), source(1)));
                    m_flags.nzcv = NzcvState::NzOnly;
                    break;
                }
                if (claim_kind(in) == ir::NzcvKind::And) {
                    if (in.type == ir::Type::I8 || in.type == ir::Type::I16) {
                        // Narrow: the result (operands are zero-extended,
                        // so it is too), then the flags of the operands
                        // shifted to the top of the word -- N is bit 7 or
                        // 15 of the result, Z its zero test, C and V clear.
                        // X17 is a free scratch between ALU ops.
                        const u8 shift = in.type == ir::Type::I8 ? 24 : 16;
                        emit(arm64::and_w(dst, source(0), source(1)));
                        emit(arm64::lsl_w_imm(17, source(0), shift));
                        emit(arm64::ands_w_lsl(31, 17, source(1), shift));
                    } else {
                        emit(wide ? arm64::ands_x(dst, source(0), source(1))
                                  : arm64::ands_w(dst, source(0), source(1)));
                    }
                    m_flags.nzcv = NzcvState::And;
                    break;
                }
                emit(wide ? arm64::and_x(dst, source(0), source(1))
                          : arm64::and_w(dst, source(0), source(1))); break;
            case ir::Op::Or:
                emit(wide ? arm64::orr_x(dst, source(0), source(1))
                          : arm64::orr_w(dst, source(0), source(1))); break;
            case ir::Op::Xor:
                emit(wide ? arm64::eor_x(dst, source(0), source(1))
                          : arm64::eor_w(dst, source(0), source(1))); break;
            case ir::Op::Shl:
            case ir::Op::Shr:
            case ir::Op::Sar: {
                const bool by_value =
                    in.operands[1].kind == ir::OperandKind::Value;
                const u8 n = by_value
                    ? 0 : static_cast<u8>(in.operands[1].payload);
                // SAR shifts the field SIGN-extended, or the replicated
                // bit would be the host word's rather than the operand's.
                // SHL and SHR take the zero-extended field as it stands.
                u8 value = source(0);
                if (in.op == ir::Op::Sar && in.type == ir::Type::I8) {
                    emit(arm64::sxtb_w(dst, source(0)));
                    value = dst;
                } else if (in.op == ir::Op::Sar && in.type == ir::Type::I16) {
                    emit(arm64::sxth_w(dst, source(0)));
                    value = dst;
                }
                if (by_value) {
                    const u8 count = source(1);
                    if (in.op == ir::Op::Shl)
                        emit(wide ? arm64::lslv_x(dst, value, count)
                                  : arm64::lslv_w(dst, value, count));
                    else if (in.op == ir::Op::Shr)
                        emit(wide ? arm64::lsrv_x(dst, value, count)
                                  : arm64::lsrv_w(dst, value, count));
                    else
                        emit(wide ? arm64::asrv_x(dst, value, count)
                                  : arm64::asrv_w(dst, value, count));
                    break;
                }
                // The W form only encodes shifts of 0..31: an I64 shift
                // by 32 or more is not a wider-than-needed encoding, it is
                // an ILLEGAL one, and the block faults with SIGILL. RCL/RCR
                // at 32 bits rotates a 33-bit span and reaches exactly 32.
                if (in.op == ir::Op::Shl)
                    emit(wide ? arm64::lsl_x_imm(dst, value, n)
                              : arm64::lsl_w_imm(dst, value, n));
                else if (in.op == ir::Op::Shr)
                    emit(wide ? arm64::lsr_x_imm(dst, value, n)
                              : arm64::lsr_w_imm(dst, value, n));
                else
                    emit(wide ? arm64::asr_x_imm(dst, value, n)
                              : arm64::asr_w_imm(dst, value, n));
                break;
            }
            case ir::Op::XmmRead: {
                // Forwarding: this register may already be in a vector
                // register from a write earlier in the same block, which
                // is the repeated-operation shape (`paddq xmm8,xmm9` four
                // times) where the reload is pure waste.
                //
                // The scan stops at the first may-fault op: V0-V7 are
                // caller-saved, so a helper a guest access can call
                // destroys them, and reusing a value across one would be
                // reading a register the callee owned.
                const u8 idx = in.operands[0].reg_index;
                ir::ValueId forwarded = ir::kInvalidValue;
                for (usize back = at; back-- > 0; ) {
                    const ir::Inst& prev = ir_block.insts[back];
                    if (prev.may_fault) break;
                    if (prev.op == ir::Op::XmmWrite &&
                        prev.operands[0].reg_index == idx) {
                        forwarded = prev.operands[1].as_value();
                        break;
                    }
                }
                if (forwarded != ir::kInvalidValue &&
                    vloc[forwarded] != 0xFF) {
                    vloc[in.result] = vloc[forwarded];
                    break;
                }
                const u8 v = vnext++;
                vloc[in.result] = v;
                emit(arm64::ldr_q_uoff(
                    v, REG_CPU_STATE, xmm_u64_off(idx, 0) >> 4));
                break;
            }
            case ir::Op::XmmWrite:
                emit(arm64::str_q_uoff(
                    vloc[in.operands[1].as_value()], REG_CPU_STATE,
                    xmm_u64_off(in.operands[0].reg_index, 0) >> 4));
                break;
            case ir::Op::VXor:
            case ir::Op::VAnd:
            case ir::Op::VOr: {
                const u8 v = vnext++;
                vloc[in.result] = v;
                const u8 a = vloc[in.operands[0].as_value()];
                const u8 b = vloc[in.operands[1].as_value()];
                emit(in.op == ir::Op::VXor ? arm64::eor_v16b(v, a, b)
                   : in.op == ir::Op::VAnd ? arm64::and_v16b(v, a, b)
                                           : arm64::orr_v16b(v, a, b));
                break;
            }
            case ir::Op::VAbs: {
                const u8 v = vnext++;
                vloc[in.result] = v;
                const u8 a = vloc[in.operands[0].as_value()];
                emit(arm64::abs_v(v, a,
                                  static_cast<arm64::VSize>(in.vec_elem)));
                break;
            }
            case ir::Op::VAdd: case ir::Op::VSub:
            case ir::Op::VCmpEq: case ir::Op::VCmpGt:
            case ir::Op::VMinU: case ir::Op::VMaxU:
            case ir::Op::VMinS: case ir::Op::VMaxS:
            case ir::Op::VMulLo: case ir::Op::VAvgU:
            case ir::Op::VQAddS: case ir::Op::VQAddU:
            case ir::Op::VQSubS: case ir::Op::VQSubU:
            case ir::Op::VZipLo: case ir::Op::VZipHi: {
                const u8 v = vnext++;
                vloc[in.result] = v;
                const u8 a = vloc[in.operands[0].as_value()];
                const u8 b = vloc[in.operands[1].as_value()];
                const auto sz = static_cast<arm64::VSize>(in.vec_elem);
                switch (in.op) {
                case ir::Op::VAdd:   emit(arm64::add_v(v, a, b, sz)); break;
                case ir::Op::VSub:   emit(arm64::sub_v(v, a, b, sz)); break;
                case ir::Op::VCmpEq: emit(arm64::cmeq_v(v, a, b, sz)); break;
                case ir::Op::VCmpGt: emit(arm64::cmgt_v(v, a, b, sz)); break;
                case ir::Op::VMinU:  emit(arm64::umin_v(v, a, b, sz)); break;
                case ir::Op::VMaxU:  emit(arm64::umax_v(v, a, b, sz)); break;
                case ir::Op::VMinS:  emit(arm64::smin_v(v, a, b, sz)); break;
                case ir::Op::VMaxS:  emit(arm64::smax_v(v, a, b, sz)); break;
                case ir::Op::VMulLo: emit(arm64::mul_v(v, a, b, sz)); break;
                case ir::Op::VAvgU:  emit(arm64::urhadd_v(v, a, b, sz)); break;
                case ir::Op::VQAddS: emit(arm64::sqadd_v(v, a, b, sz)); break;
                case ir::Op::VQAddU: emit(arm64::uqadd_v(v, a, b, sz)); break;
                case ir::Op::VQSubS: emit(arm64::sqsub_v(v, a, b, sz)); break;
                case ir::Op::VQSubU: emit(arm64::uqsub_v(v, a, b, sz)); break;
                case ir::Op::VZipLo: emit(arm64::zip1_v(v, a, b, sz)); break;
                default:             emit(arm64::zip2_v(v, a, b, sz)); break;
                }
                break;
            }
            case ir::Op::VPackS: case ir::Op::VPackU: {
                // Two narrowing instructions: the destination's elements
                // become the LOW half and the source's the high half,
                // which is exactly SQXTN then SQXTN2 (Intel SDM Vol. 2
                // PACKSSWB/PACKUSWB).
                const u8 v = vnext++;
                vloc[in.result] = v;
                const u8 a = vloc[in.operands[0].as_value()];
                const u8 b = vloc[in.operands[1].as_value()];
                const auto sz = static_cast<arm64::VSize>(in.vec_elem);
                if (in.op == ir::Op::VPackS) {
                    emit(arm64::sqxtn_v(v, a, sz));
                    emit(arm64::sqxtn2_v(v, b, sz));
                } else {
                    // PACKUSWB saturates a SIGNED source into an UNSIGNED
                    // destination, which is SQXTUN, not UQXTN.
                    emit(arm64::sqxtun_v(v, a, sz));
                    emit(arm64::sqxtun2_v(v, b, sz));
                }
                break;
            }
            case ir::Op::VShlImm: case ir::Op::VShrImm:
            case ir::Op::VSarImm: {
                const u8 v = vnext++;
                vloc[in.result] = v;
                const u8 a = vloc[in.operands[0].as_value()];
                const auto sz = static_cast<arm64::VSize>(in.vec_elem);
                const u8 n = static_cast<u8>(in.operands[1].payload);
                emit(in.op == ir::Op::VShlImm ? arm64::shl_v_imm(v, a, n, sz)
                   : in.op == ir::Op::VShrImm ? arm64::ushr_v_imm(v, a, n, sz)
                                              : arm64::sshr_v_imm(v, a, n, sz));
                break;
            }
            case ir::Op::VInsertLow32: {
                // MOVSS xmm,xmm: bits 127:32 from operand 0, bits 31:0
                // from operand 1. Copy then insert rather than insert in
                // place -- operand 0 is an SSA value a later op in the
                // same instruction could still read.
                const u8 v = vnext++;
                vloc[in.result] = v;
                emit(arm64::mov_v16b(v, vloc[in.operands[0].as_value()]));
                emit(arm64::ins_v_gpr(v, 0, source(1), arm64::VS_S));
                break;
            }
            case ir::Op::VZext32: {
                // MOVSS xmm,m32: FMOV Sd,Wn writes lane 0 and zeroes the
                // rest, which is exactly the architectural rule.
                const u8 v = vnext++;
                vloc[in.result] = v;
                emit(arm64::fmov_s_from_w(v, source(0)));
                break;
            }
            case ir::Op::VLow32:
                emit(arm64::fmov_w_from_s(
                    dst, vloc[in.operands[0].as_value()]));
                break;
            case ir::Op::Mul:
                emit(arm64::mul_x(dst, source(0), source(1)));
                break;
            case ir::Op::SMulH:
                emit(arm64::smulh(dst, source(0), source(1)));
                break;
            case ir::Op::Rol:
            case ir::Op::Ror: {
                // ARM64 rotates a whole register, so a narrow rotate needs
                // the field DOUBLED first: `v | (v << width)` puts the bits
                // that leave the top back at the bottom, and a plain right
                // shift then lands them. ROL by n is ROR by (width - n).
                const u32 bits = ir::type_bits(in.type);
                const u8 raw = static_cast<u8>(in.operands[1].payload);
                const u8 right = in.op == ir::Op::Ror
                    ? raw : static_cast<u8>((bits - raw) % bits);
                if (bits == 64) {
                    emit(arm64::ror_x_imm(dst, source(0), right));
                    break;
                }
                if (bits == 32) {
                    emit(arm64::ror_w_imm(dst, source(0), right));
                    break;
                }
                emit(arm64::orr_w_lsl(dst, source(0), source(0),
                                      static_cast<u8>(bits)));
                emit(arm64::lsr_w_imm(dst, dst, right));
                // CF and OF are read out of the result, so the bits above
                // the field must not be there.
                emit(bits == 8 ? arm64::uxtb_w(dst, dst)
                               : arm64::uxth_w(dst, dst));
                break;
            }
            case ir::Op::Select:
                // A condition this instruction's EvalCc took off a live
                // claim is still in NZCV -- nothing between the two in one
                // guest instruction writes it -- so CSEL on it directly.
                if (in.operands[0].kind == ir::OperandKind::Value &&
                    fused_select_cc[in.operands[0].as_value()] >= 0 &&
                    nzcv_before != NzcvState::None &&
                    ir_block.insts[plan.def_at[in.operands[0].as_value()]]
                            .guest_rip == in.guest_rip) {
                    const u8 arm = static_cast<u8>(
                        fused_select_cc[in.operands[0].as_value()]);
                    emit(wide ? arm64::csel_x(dst, source(1), source(2), arm)
                              : arm64::csel_w(dst, source(1), source(2), arm));
                    m_flags.nzcv = nzcv_before;
                    break;
                }
                // The condition is a VALUE, not host NZCV, so compare it
                // against zero first -- at the CONDITION's width, which is
                // not necessarily the result's. This destroys any NZCV
                // residency claim, which is why Select is not on the
                // preserving list below (and clobbers_nzcv names it).
                emit(in.operands[0].type == ir::Type::I64
                         ? arm64::subs_x(31, source(0), 31)
                         : arm64::subs_w(31, source(0), 31));
                emit(wide ? arm64::csel_x(dst, source(1), source(2), 1)
                          : arm64::csel_w(dst, source(1), source(2), 1));
                break;
            case ir::Op::Divide: {
                // Status in W0: zero continues, nonzero raises #DE at this
                // instruction's RIP and leaves the block. The helper wrote
                // nothing on that path, which is the contract. It leaves
                // through the block's raise exit like every other software
                // raise: a flag record elided across this divide under a
                // recovery plan is rebuilt there (raise_block_pc), and the
                // #DE context carries the EFLAGS before the DIV.
                emit(arm64::mov_x(1, source(0)));
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit_mov_imm32(2, static_cast<u32>(in.operands[1].payload));
                emit_host_addr_imm64(16, static_cast<i64>(
                    reinterpret_cast<uintptr_t>(&jit_ir_divide)));
                emit_helper_blr();
                const usize to_error = m_code_buf.size();
                emit(0);
                const usize to_done = m_code_buf.size();
                emit(0);
                const usize error_at = m_code_buf.size();
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit_mov_imm64(1, static_cast<i64>(rip));
                emit_host_addr_imm64(16, static_cast<i64>(
                    reinterpret_cast<uintptr_t>(&jit_raise_divide_error)));
                emit_helper_blr();
                m_direct_raise_branch_pos.push_back(m_code_buf.size());
                emit(0);  // B .raise -- patched by emit_flush_raise_exit_stub()
                const usize done_at = m_code_buf.size();
                m_code_buf[to_error] = arm64::cbnz_w(
                    0, static_cast<i32>(error_at - to_error) * 4);
                m_code_buf[to_done] = arm64::b_imm(
                    static_cast<i32>(done_at - to_done) * 4);
                break;
            }
            case ir::Op::SegSelector:
                // LDRH zero-extends into the whole X register, which is
                // exactly what a 64-bit slot receives.
                emit(arm64::ldrh_uoff(
                    dst, REG_CPU_STATE,
                    seg_selector_offset(static_cast<SegReg>(
                        in.operands[0].payload)) >> 1));
                break;
            case ir::Op::FlagsImage:
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit_host_addr_imm64(16, static_cast<i64>(
                    reinterpret_cast<uintptr_t>(&jit_ir_flags_image)));
                emit_helper_blr();
                if (dst != 0) emit(arm64::mov_x(dst, 0));
                break;
            case ir::Op::FlagsInstall:
                emit(arm64::mov_x(1, source(0)));
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit_host_addr_imm64(16, static_cast<i64>(
                    reinterpret_cast<uintptr_t>(&jit_ir_flags_install)));
                emit_helper_blr();
                break;
            case ir::Op::EvalCc: {
                // Off the producer's S-form claim when it survived to here
                // (assign_nzcv_residency): one CSET, or nothing at all
                // when the only reader is this instruction's Select,
                // which then CSELs on the condition itself.
                const u8 cc = static_cast<u8>(in.operands[1].payload);
                if (const int arm = fused_arm_cond(nzcv_before, cc, KindGuarded{});
                    arm >= 0) {
                    fused_select_cc[in.result] = static_cast<i8>(arm);
                    const bool via_c = carry_only[in.result] &&
                                       (arm == kArmCs || arm == kArmCc);
                    if (!select_only[in.result] && !via_c)
                        emit(arm64::cset_w(dst, static_cast<u8>(arm)));
                    m_flags.nzcv = nzcv_before;   // CSET reads, never writes
                    break;
                }
                if (pstate_flags[in.operands[0].as_value()]) {
                    std::fprintf(stderr,
                                 "[ir] FATAL: a PSTATE-only flag definition's "
                                 "reader found no live claim; block 0x%llx op %zu\n",
                                 static_cast<unsigned long long>(ir_block.entry_rip),
                                 at);
                    std::abort();
                }
                // Leaves 0 or 1 in W0, and may BLR the condition helper --
                // which is why this op ends a segment.
                predict_lazy_kind(at);
                emit_eval_lazy_condition(cc);
                m_flags.static_lazy_kind = kLazyKindUnknown;
                if (dst != 0) emit(arm64::mov_w(dst, 0));
                break;
            }
            case ir::Op::SetCf:
            case ir::Op::SetOf: {
                // `CpuState::write_cf_only`, emitted: the overlay byte AND
                // the RFLAGS bit, leaving the pending lazy record intact so
                // ZF/SF/PF/AF still materialize from it. A later full-flag
                // writer calls `set_flag_op`, which clears the overlays.
                const bool carry = in.op == ir::Op::SetCf;
                const u32 mask = carry ? 0x1u : 0x800u;
                const u8 lsb = carry ? 0 : 11;
                // UBFX Wd, Wn, #0, #1 -- isolate bit 0, so a caller may
                // hand over any value whose low bit is the flag.
                emit(arm64::ubfx_w(REG_SCRATCH4, source(0), 0, 1));
                const u32 rflags = cpu_rflags_offset();
                emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rflags >> 3));
                emit_mov_imm32(REG_EA_TMP, mask);
                emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_EA_TMP));
                if (lsb != 0)
                    emit(arm64::lsl_w_imm(REG_EA_TMP, REG_SCRATCH4, lsb));
                else
                    emit(arm64::mov_w(REG_EA_TMP, REG_SCRATCH4));
                emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_EA_TMP));
                emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rflags >> 3));
                emit(arm64::add_w_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
                emit(arm64::strb_uoff(REG_SCRATCH4, REG_CPU_STATE,
                                      carry ? cf_overlay_offset()
                                            : of_overlay_offset()));
                // Every instruction above is a non-S form: the overlay
                // write does not disturb a residency claim made before it.
                m_flags.nzcv = nzcv_before;
                break;
            }
            case ir::Op::AddCarry:
            case ir::Op::SubBorrow: {
                // ARM ADC adds C; SBC subtracts NOT C (Rn + NOT Rm + C). So
                // ADC needs C == CF and SBC needs C == NOT CF. A fused CS
                // condition left C == CF, a fused CC one C == NOT CF.
                const bool sub = in.op == ir::Op::SubBorrow;
                const ir::ValueId cf = in.operands[2].as_value();
                const int arm = fused_select_cc[cf];
                // Fused by the op right before this one, so nothing between
                // has touched PSTATE (a CSET only reads it).
                const bool adjacent = at > 0 &&
                    ir_block.insts[at - 1].op == ir::Op::EvalCc &&
                    ir_block.insts[at - 1].result == cf;
                if (adjacent && (arm == kArmCs || arm == kArmCc)) {
                    if ((arm == kArmCs) == sub) emit(arm64::cfinv());
                } else if (sub) {
                    emit(arm64::subs_w(31, 31, source(2)));      // C = (0 >= cf)
                } else {
                    emit(arm64::subs_w_imm(31, source(2), 1));   // C = (cf >= 1)
                }
                emit(sub ? arm64::sbcs_w(dst, source(0), source(1))
                         : arm64::adcs_w(dst, source(0), source(1)));
                const ir::NzcvKind kind = claim_kind(in);
                if (kind == (sub ? ir::NzcvKind::Sub : ir::NzcvKind::Add))
                    m_flags.nzcv = static_cast<NzcvState>(kind);
                break;
            }
            case ir::Op::Not:
                emit(wide ? arm64::mvn_x(dst, source(0)) : arm64::mvn_w(dst, source(0)));
                break;
            case ir::Op::Neg:
                emit(wide ? arm64::sub_x(dst, 31, source(0)) : arm64::sub_w(dst, 31, source(0)));
                break;
            case ir::Op::DefFlags:
                if (in.flags.repr == ir::FlagRepr::Dead) break;
                if (in.flags.repr == ir::FlagRepr::InPstate) {
                    // No record: PSTATE is the only home, so the producer's
                    // claim must be the one this definition names.
                    if (nzcv_before != static_cast<NzcvState>(in.flags.nzcv)) {
                        std::fprintf(stderr,
                                     "[ir] FATAL: PSTATE-only flag definition "
                                     "without its producer's claim; block "
                                     "0x%llx op %zu\n",
                                     static_cast<unsigned long long>(ir_block.entry_rip),
                                     at);
                        std::abort();
                    }
                    m_flags.nzcv = nzcv_before;
                    break;
                }
                m_omit_lazy_current = false;
                m_omit_lazy_keep_overlay = false;
                // A CF-merge edge is ALSO a third value operand; the two
                // are told apart by its type, `Flags` versus the count's
                // `I8`. Getting that wrong publishes a rotate's count as
                // an inherited carry.
                if (in.operand_count == 3 &&
                    in.operands[2].kind == ir::OperandKind::Value &&
                    in.operands[2].type == ir::Type::I8) {
                    // A by-CL shift. Intel SDM Vol. 2: a masked count of
                    // ZERO modifies no flags at all, and with a runtime
                    // count that is a runtime decision -- so the skip is
                    // emitted here. The IR has no in-instruction control
                    // flow, and the result needs no skip: a shift by zero
                    // already leaves the destination alone.
                    const u8 count = source(2);
                    const usize skip = m_code_buf.size();
                    emit(0);
                    emit_set_shift_lazy_flags(in.flags.record, source(0),
                                              count, source(1));
                    m_code_buf[skip] = arm64::cbz_w(count,
                        static_cast<i32>(m_code_buf.size() - skip) * 4);
                    break;
                }
                if (in.flags.shift_count != 0) {
                    // `flag_dst` is `(count << 32) | result`, so this needs
                    // THREE inputs. Same primitive the stencil path uses,
                    // so the two tiers cannot disagree about the packing.
                    emit_mov_imm32(REG_SCRATCH4, in.flags.shift_count);
                    emit_set_shift_lazy_flags(in.flags.record, source(0),
                                              REG_SCRATCH4, source(1));
                } else if (in.operand_count == 3 &&
                           in.operands[1].type == ir::Type::I64) {
                    // Inc64/Dec64: flag_src carries only the incoming CF at
                    // bit 32 (a 64-bit original has no room beside it; the
                    // materializer derives it from the result), flag_dst
                    // the full 64-bit result -- the stencil x64 record.
                    // emit_set_lazy_flags_preserve_cf is the 32-bit packer
                    // and would truncate the result.
                    emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_CALL_TMP, 32));
                    emit_set_lazy_flags64(in.flags.record, REG_SCRATCH4,
                                          source(1));
                } else if (in.operand_count == 3) {
                    emit(arm64::mov_w(REG_SCRATCH4, source(0)));
                    emit_set_lazy_flags_preserve_cf(in.flags.record, REG_SCRATCH4,
                                                   source(1), REG_CALL_TMP);
                } else if (in.flags.exit_deferred &&
                           exit_record_operand_safe(in, 0) &&
                           exit_record_operand_safe(in, 1)) {
                    pending_record = DeferredRecord{
                        true, in.flags.record, source(0), source(1),
                        in.operands[0].type == ir::Type::I64, in.result};
                } else if (in.operands[0].type == ir::Type::I64)
                    emit_set_lazy_flags64(in.flags.record, source(0), source(1));
                else emit_set_lazy_flags(in.flags.record, source(0), source(1));
                // The record store is MOVs and STRs, so the producer's
                // S-form claim (resident_kind) is still what PSTATE holds.
                // That includes a CF merge's (LSL/ORR/MOV/STR).
                if (in.flags.shift_count == 0 &&
                    (in.operand_count == 2 || ir_is_cf_merge(in)) &&
                    in.flags.nzcv != ir::NzcvKind::None &&
                    in.flags.nzcv != ir::NzcvKind::NzOnly &&
                    nzcv_before == static_cast<NzcvState>(in.flags.nzcv)) {
                    m_flags.nzcv = nzcv_before;
                }
                if (nzcv_before == NzcvState::NzOnly && in.operand_count == 2 &&
                    in.operands[1].kind == ir::OperandKind::Value &&
                    selftest_and[in.operands[1].as_value()]) {
                    m_flags.nzcv = nzcv_before;
                }
                // A residency claim costs a TST, and only a branch that can
                // actually use it pays that back -- an unconsumed claim
                // taxes every following memory op with an NZCV spill. So it
                // is materialized only when the block really ends in a Jcc
                // this kind can fuse.
                if (in.flags.nzcv == ir::NzcvKind::NzOnly &&
                    in.operand_count >= 2 &&
                    !ir_block.insts.empty() &&
                    ir_block.insts.back().op == ir::Op::Jcc &&
                    fused_arm_cond(NzcvState::NzOnly,
                                   ir_block.insts.back().branch.cond,
                                   KindGuarded{}) >= 0) {
                    emit(arm64::ands_x(31, source(1), source(1)));
                    m_flags.nzcv = NzcvState::NzOnly;
                }
                break;
            case ir::Op::GprWrite: {
                const ir::Type write_type = in.operands[0].type;
                if (write_type == ir::Type::I8 || write_type == ir::Type::I16) {
                    // A PARTIAL write merges the field and leaves the rest
                    // of the architectural register alone. Through the
                    // shared primitive, never a bare BFI at the pin: that
                    // primitive owns the invalidation a register write
                    // implies, and open-coding the insert skipped it once
                    // already (a stale ESP-frame cache and a host SIGSEGV).
                    emit_merge_gpr_narrow(
                        source(1),
                        ir::architectural_gpr(write_type, in.operands[0].reg_index),
                        ir::subregister_lsb(write_type, in.operands[0].reg_index),
                        write_type == ir::Type::I8 ? 8 : 16);
                    break;
                }
                // Intel SDM Vol. 1 3.4.1.1: W-width writes zero the high
                // half, including CpuState-resident (unpinned) registers.
                if (write_type == ir::Type::I64) {
                    emit_store_gpr64(source(1), in.operands[0].reg_index);
                } else if (ir_block.is_64bit &&
                           source(1) == pinned_gpr64(in.operands[0].reg_index)) {
                    // The value is this register's own low half, still in
                    // its pin (a read homed there: `mov eax, eax`,
                    // `shl ecx, 0`). emit_store_gpr32 drops a self-move,
                    // which would keep bits 63:32 the write must clear --
                    // unless the value was COMPUTED into the pin
                    // (commit_pin) by a W-form op, which already did.
                    const u8 pin = source(1);
                    if (plan.commit_pin[in.operands[1].as_value()] != pin)
                        emit(arm64::mov_w(pin, pin));
                } else {
                    emit_store_gpr32(source(1), in.operands[0].reg_index);
                }
                break;
            }
            default: break; // excluded before emission
            }
            if (in.op == ir::Op::GprWrite) {
                // A guest register commit is a MOV, a BFI or a STR to the
                // context -- never an S-form and never a call -- so it does
                // not disturb a residency claim the flag definition before
                // it made. (A scalar Store keeps its claim in its own case.)
                m_flags.nzcv = nzcv_before;
            }
            // The other lowerings here that never set host flags and never
            // call a helper keep a live claim too -- the address and
            // operand arithmetic between a compare and its branch. This
            // list is what makes assign_nzcv_residency's "does not
            // clobber" true in emitted code; anything not on it resets the
            // claim, which only ever costs a fusion.
            if (m_flags.nzcv == NzcvState::None && !flags_written) {
                switch (in.op) {
                case ir::Op::Const: case ir::Op::GprRead: case ir::Op::Lea:
                case ir::Op::Trunc: case ir::Op::Zext: case ir::Op::Sext:
                case ir::Op::SegBase:
                case ir::Op::Add: case ir::Op::Sub: case ir::Op::And:
                case ir::Op::Or: case ir::Op::Xor:
                case ir::Op::Not: case ir::Op::Neg:
                case ir::Op::Shl: case ir::Op::Shr: case ir::Op::Sar:
                case ir::Op::Rol: case ir::Op::Ror:
                case ir::Op::Mul: case ir::Op::SMulH:
                    m_flags.nzcv = nzcv_before;
                    break;
                case ir::Op::DefFlags:
                    if (emits_nothing) m_flags.nzcv = nzcv_before;
                    break;
                default: break;
                }
            }
            if (in.defines_value() && in.type != ir::Type::Flags &&
                in.type != ir::Type::V128 && plan.needs_backing[in.result]) {
                spill_base();
                emit(arm64::str_x_uoff(dst, REG_SCRATCH3, plan.spill_slot[in.result] * 2));
            }
        }
        if (recorded < kMaxInsnArmPos)
            m_insn_arm_end[recorded] = static_cast<u32>(m_code_buf.size());
        ++m_block_native_insns;
        coverage::record_compiled(instr.opcode, form_key(instr), true, LowerKind::Native);
        if (stats::enabled()) {
            const u32 words = static_cast<u32>(m_code_buf.size()) - arm_before;
            stats::record_insn(instr.opcode, true, true, words);
            m_stats_insns.push_back(stats::InsnSite{
                rip, static_cast<u32>(instr.opcode),
                static_cast<u32>(instr.length), arm_before, words});
        }
        rip += instr.length;
    }
    const auto flush_cold_exits = [&] {
        for (const ColdExit& e : cold_exits) {
            const i32 disp = cold_branch_disp(e.branch_pos, m_code_buf.size(),
                                              19, "IR cold exit");
            m_code_buf[e.branch_pos] = e.arm >= 0
                ? arm64::b_cond(static_cast<u8>(e.arm), disp)
                : arm64::cbnz_w(0, disp);
            if (e.record.valid) publish_record(e.record);
            emit_update_rip(e.target);
            // Every forward taken edge carries a direct-link patch slot,
            // exactly as the stencil path's mid-block Jcc exits do
            // (emit_flush_mid_jcc_exits): without it every CondExit taken
            // edge pays the L1 probe. Not gated on the block's other
            // slots -- these exits are flushed after the terminal tail,
            // so such a gate takes the slot away whenever the terminal
            // has one (a CALL rel with CallRel links on).
            const bool slot = e.target > m_block_start_addr;
            if (!emit_linked_tail_continue(e.target, e.carry,
                                           slot ? LinkEdge::MidJcc
                                                : LinkEdge::None)) {
                emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                       jit_iters_offset() >> 2));
                emit_epilogue(JitExecResult::Continue);
            }
        }
    };
    if (!ir_block.insts.empty() && ir::is_terminator(ir_block.insts.back().op)) {
        flush_cold_exits();
        return;
    }
    flush_pending_record();
    emit_update_rip(rip);
    if (!emit_linked_tail_continue(
            rip, exit_carry_after_kind_clear(rip, m_flags.nzcv))) {
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);
    }
    flush_cold_exits();
    return;
}

}  // namespace vitreo::cpu::jit
