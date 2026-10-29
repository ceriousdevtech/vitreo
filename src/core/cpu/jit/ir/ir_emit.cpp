// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_emit.cpp
/// @brief Tier 1 ARM64 emission from canonical IR (ADR-0002): the
///        dispatcher and the 32-bit pinned-ABI emitter.
///
/// The overview ("Emission cannot fail", "Register discipline") and the
/// helpers every part shares are in ir_emit_internal.hpp. The other parts
/// are ir_emit_plan.cpp, ir_emit_feasible.cpp, ir_emit_snapshot.cpp and
/// ir_runtime_helpers.cpp.

#include "ir_emit_internal.hpp"

namespace vitreo::cpu::jit {

void JitCompiler::ir_emit_block(const BasicBlock& block) {
    const ir::IrBlock& ir_block = m_ir_block;

    if (m_ir_plan.snapshot_path) {
        ir_emit_snapshot_block(block);
        return;
    }

    // Start without a claim about host NZCV. Flag-residency annotations may
    // select S-form ALU lowering below and establish a new claim; inherited
    // compiler state must never authorize a fused branch before that happens.
    m_flags.nzcv = NzcvState::None;
    m_flags.pending_carry_source = CarrySource::Unknown;

    // Value -> ARM register. Pure scalar blocks allocate scratch across the
    // block; helper/memory blocks restart allocation per instruction. Pins
    // retain their architectural values across RIP boundaries.
    std::vector<u8> loc(ir_block.next_value, 0xFF);
    // Value -> NEON register, and the next free one. A separate pool from the
    // GPR scratch: vectors never compete with integers for a slot, and the
    // pool needs no allocator because vector values are RIP-local like every
    // other value here and no admitted form defines more than three.
    //
    // V0-V3 are safe to take: they are caller-saved, and the feasibility pass
    // has already refused any block where a vector is live across a guest
    // memory access (the only thing here that can call a helper).
    std::vector<u8> vloc(ir_block.next_value, 0xFF);
    u8 vnext = 0;
    constexpr u8 kVecPool = 4;
    u64 current_rip = ir::kInvalidRip;

    // Resolve the recovery plans to ARM registers BEFORE lowering, because a
    // cold store stub emitted mid-instruction has to know whether this
    // instruction's record was elided. The per-interval descriptors published
    // to the fault map are filled from the same plans after the loop.
    struct ResolvedPlan {
        u64 first_rip{ir::kInvalidRip};
        u64 last_rip{ir::kInvalidRip};
        StubFlagReplay replay{};
    };
    std::vector<ResolvedPlan> resolved_plans;
    for (const ir::FlagRecoveryPlan& plan : ir_block.flag_recovery) {
        if (!plan.active()) continue;
        ResolvedPlan r;
        r.first_rip = plan.first_rip;
        r.last_rip = plan.last_rip;
        r.replay.active = true;
        r.replay.flag_op = plan.flag_op;
        r.replay.recompute = plan.recompute_dst;
        if (plan.src_is_imm) {
            r.replay.src_imm = static_cast<u32>(plan.src_imm);
        } else {
            r.replay.src_reg = pinned_gpr32(plan.src_reg);
        }
        if (plan.dst_is_imm) {
            r.replay.dst_imm = static_cast<u32>(plan.dst_imm);
        } else {
            r.replay.dst_reg = pinned_gpr32(plan.dst_reg);
        }
        resolved_plans.push_back(r);
    }

    // Publish the guest RIP before an access, but only where the lowering
    // actually needs it.
    //
    // The two fault paths differ, and the difference is a configuration:
    //
    //  * **Linear** (`VITREO_JIT_LINEAR`, the 32-bit production default) — a
    //    bad address is a real SIGSEGV on a PROT_NONE page inside the
    //    reservation. The handler recovers the guest RIP from the host-PC side
    //    table this emitter already populates, so publishing would be dead
    //    work on the hot path.
    //  * **Region cache** (`VITREO_JIT_LINEAR=0`) — the access goes through a
    //    helper that raises via `jit_raise_access_violation`, which
    //    DELIBERATELY does not set RIP: it assumes the caller already did. With
    //    no publication the guest's exception record would carry whatever RIP
    //    the previous memory instruction left behind.
    //
    // The IR says `FaultState::RipAndFlags` on every access, which is a claim
    // that the guest RIP is correct at that point. Honour it in the
    // configuration where nothing else does, rather than letting the claim be
    // true only by accident of the default.
    // Published at INSTRUCTION ENTRY, never at the access itself.
    //
    // `emit_update_rip` leaves the published value in W0 — it is the same
    // register the linked-tail target travels in. W0 is also the first of this
    // emitter's three scratch registers, so publishing between the address
    // computation and the access overwrites the address with the RIP. That is
    // not a hypothetical: `mov eax,[ecx]` loaded from the block's own code
    // address instead of from ECX, and the value it returned was the encoded
    // instruction bytes read back as data.
    //
    // Publishing at instruction entry is also what the stencil path does for
    // its implicit-memory-operand instructions, and it naturally publishes
    // once for an instruction with two accesses.
    const bool region_path = !(m_cfg.linear_jit && m_memory.is_linear());

    // Which values may be computed DIRECTLY into a guest pin, keyed by value.
    // 0xFF means "no, use a scratch". Recomputed per guest instruction.
    std::vector<u8> inplace(ir_block.next_value, 0xFF);
    // Which values currently live in a pin because they were read from one.
    std::vector<u8> home_pin(ir_block.next_value, 0xFF);

    /// True when the value defined at `d` is the RESULT operand of a flag
    /// definition the residency pass marked. Only then is the S-form worth
    /// emitting, and only then may the terminator read PSTATE.
    const auto nzcv_resident_result = [&](usize d) {
        if (!ir_block.insts[d].defines_value()) return false;
        const ir::ValueId v = ir_block.insts[d].result;
        for (const ir::Inst& u : ir_block.insts) {
            if (u.op != ir::Op::DefFlags) continue;
            if (u.flags.nzcv == ir::NzcvKind::None) continue;
            // Operand 1 is `flag_dst`, the result the flags describe.
            if (u.operand_count >= 2 &&
                u.operands[1].kind == ir::OperandKind::Value &&
                u.operands[1].as_value() == v) {
                return true;
            }
        }
        return false;
    };

    const auto reg_of = [&](const ir::Operand& operand) -> u8 {
        return loc[operand.as_value()];
    };
    // The value parked in `kIrSurvivor` for the current guest instruction.
    ir::ValueId survivor = ir::kInvalidValue;

    // Value -> pool register, filled for each supported allocation range by the same
    // interval scan the feasibility check ran. Round-robin allocation is gone:
    // it could hand two simultaneously live values the same register once the
    // pool was allowed to be reused at all.
    std::vector<u8> pool_reg(ir_block.next_value, 0xFF);
    std::vector<u8> survivor_mask(ir_block.next_value, 0);
    ir::ValueId prev_survivor = ir::kInvalidValue;

    const bool block_pool = ir::scalar_block_allocatable(ir_block);
    if (block_pool) {
        for (const auto& in : ir_block.insts) {
            if (in.op == ir::Op::GprRead && in.type == ir::Type::I32 &&
                pinned_gpr32(in.operands[0].reg_index) != 0xFF)
                home_pin[in.result] = in.operands[0].reg_index;
        }
        u16 commit_pin_mask = 0;
        for (u8 reg = 0; reg < 16; ++reg)
            if (pinned_gpr32(reg) != 0xFF) commit_pin_mask |= static_cast<u16>(1u << reg);
        (void)plan_pool_registers(ir_block, 0, ir_block.insts.size(), home_pin,
                                 survivor_mask, &pool_reg, commit_pin_mask);
    }

    const auto alloc_for = [&](ir::ValueId v) -> u8 {
        if (v == survivor) return kIrSurvivor;
        if (inplace[v] != 0xFF) return inplace[v];
        if (pool_reg[v] != 0xFF) return pool_reg[v];
        // Unreachable: `ir_emit_feasible` planned every pool-resident value.
        // Falling back to a fixed register keeps a logic slip contained rather
        // than indexing out of range.
        return kIrScratch[0];
    };

    /// Which value in this guest instruction must be parked in the survivor
    /// register: one that is live across a memory access and cannot be
    /// recomputed. `ir_emit_feasible` has already proved there is at most one.
    const auto plan_survivor = [&](usize begin, usize end) -> ir::ValueId {
        for (usize m = begin; m < end; ++m) {
            const ir::Inst& access = ir_block.insts[m];
            if (access.op != ir::Op::Load && access.op != ir::Op::Store)
                continue;
            for (usize d = begin; d < m; ++d) {
                const ir::Inst& def = ir_block.insts[d];
                if (!def.defines_value() || def.type != ir::Type::I32) continue;
                if (home_pin[def.result] != 0xFF) continue;  // a pin survives
                bool used_after = false;
                for (usize j = m + 1; j < end && !used_after; ++j) {
                    for (u32 k = 0; k < ir_block.insts[j].operand_count; ++k) {
                        const ir::Operand& o = ir_block.insts[j].operands[k];
                        if (o.kind == ir::OperandKind::Value &&
                            o.as_value() == def.result) {
                            used_after = true;
                            break;
                        }
                    }
                }
                if (!used_after) continue;
                if (rematerializable(ir_block, def.result, home_pin)) continue;
                return def.result;
            }
        }
        return ir::kInvalidValue;
    };

    /// Decide, for one guest instruction's op range, which results can be
    /// computed straight into their destination pin.
    ///
    /// Without this, every ALU result lands in a scratch and is then copied to
    /// the pin: `add_w(W0,X23,X24); mov_w(X23,W0)` where the stencil compiler
    /// emits `adds_w(X23,X23,X25)` in place. The MOV is pure overhead and it
    /// was the whole remaining size gap to Tier 0.
    ///
    /// Four conditions, and the third is the one with teeth.
    // The two rematerializable emitters, factored out because each is emitted
    // twice for a stack instruction: once in program order, and again after
    // the memory access that clobbered the scratch holding its result.
    const auto emit_const_op = [&](u8 dst, const ir::Inst& in) {
        emit_mov_imm32(dst, static_cast<u32>(in.operands[0].payload));
    };
    const auto emit_lea_op = [&](u8 dst, const ir::Inst& in) {
        const u8 base = loc[in.operands[0].as_value()];
        if (in.operands[1].kind == ir::OperandKind::Value) {
            emit(arm64::add_w_lsl(dst, base, loc[in.operands[1].as_value()],
                                  scale_shift(in.operands[2].payload)));
        } else if (dst != base) {
            emit(arm64::mov_w(dst, base));
        }
        const i64 disp = static_cast<i64>(in.operands[3].payload);
        if (disp > 0 && fits_uimm12(static_cast<u64>(disp))) {
            emit(arm64::add_w_imm(dst, dst, static_cast<u32>(disp)));
        } else if (disp < 0 && fits_uimm12(static_cast<u64>(-disp))) {
            emit(arm64::sub_w_imm(dst, dst, static_cast<u32>(-disp)));
        } else if (disp != 0) {
            // REG_SCRATCH3 is outside the value pool, so materializing here
            // cannot evict a live value.
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(disp));
            emit(arm64::add_w(dst, dst, REG_SCRATCH3));
        }
    };

    /// Re-emit, after a memory access, every value the access clobbered that is
    /// still needed. See `rematerializable` for why only `Const` and a
    /// pin-based `Lea` are eligible.
    ///
    /// This is what makes PUSH expressible: `push eax` must keep ESP-4 alive
    /// across the very store that clobbers the scratch holding it, because the
    /// SDM requires ESP to be unchanged if that store faults. Recomputing is
    /// two instructions; reordering would be a correctness bug and declining
    /// would cost every stack instruction.
    const auto remat_after_access = [&](usize access, usize end) {
        for (usize d = 0; d < access; ++d) {
            const ir::Inst& def = ir_block.insts[d];
            if (!def.defines_value() || def.type != ir::Type::I32) continue;
            if (def.guest_rip != ir_block.insts[access].guest_rip) continue;
            const u8 home = loc[def.result];
            if (home == 0xFF) continue;
            // A value living in a guest pin survives the access untouched, and
            // so does the one parked in the survivor register.
            if (home_pin[def.result] != 0xFF) continue;
            if (def.result == survivor) continue;
            bool used_after = false;
            for (usize j = access + 1; j < end && !used_after; ++j) {
                for (u32 k = 0; k < ir_block.insts[j].operand_count; ++k) {
                    const ir::Operand& o = ir_block.insts[j].operands[k];
                    if (o.kind == ir::OperandKind::Value &&
                        o.as_value() == def.result) {
                        used_after = true;
                        break;
                    }
                }
            }
            if (!used_after) continue;
            if (def.op == ir::Op::Const) emit_const_op(home, def);
            else if (def.op == ir::Op::Lea) emit_lea_op(home, def);
        }
    };

    const auto plan_inplace = [&](usize begin, usize end) {
        for (usize d = begin; d < end; ++d) {
            const ir::Inst& def = ir_block.insts[d];
            if (!def.defines_value() || def.type != ir::Type::I32) continue;
            if (def.op == ir::Op::GprRead) continue;  // already in its pin

            // 1. The value's home must be a pinned guest register, named by a
            //    `GprWrite` of exactly this value.
            usize w = end;
            for (usize j = d + 1; j < end; ++j) {
                const ir::Inst& use = ir_block.insts[j];
                if (use.op == ir::Op::GprWrite &&
                    use.operands[1].kind == ir::OperandKind::Value &&
                    use.operands[1].as_value() == def.result) {
                    w = j;
                    break;
                }
            }
            if (w == end) continue;
            const u8 guest = ir_block.insts[w].operands[0].reg_index;
            const u8 pin = pinned_gpr32(guest);
            if (pin == 0xFF) continue;

            // 2. Nothing may read the value after that write; the pin is the
            //    architectural home from then on and a later reader would be
            //    reading it by a name we no longer track.
            bool used_after = false;
            for (usize j = w + 1; j < end && !used_after; ++j) {
                const ir::Inst& later = ir_block.insts[j];
                for (u32 k = 0; k < later.operand_count; ++k) {
                    if (later.operands[k].kind == ir::OperandKind::Value &&
                        later.operands[k].as_value() == def.result) {
                        used_after = true;
                        break;
                    }
                }
            }
            if (used_after) continue;

            // 3. **Nothing between the definition and the write may read the
            //    OLD value of that pin.** Writing the result early destroys
            //    the guest register's previous contents, and a flag record can
            //    still need them: `neg eax` publishes `Neg32`, whose
            //    materialization reconstructs flags from the ORIGINAL EAX. Put
            //    the result in X23 at the NEG and the `defflags` that follows
            //    reads the result where the original belonged — the exact
            //    wrong-flags bug this emitter already hit once, reintroduced
            //    by an optimization instead of by an ordering.
            bool reads_old_pin = false;
            for (usize j = d + 1; j <= w && !reads_old_pin; ++j) {
                const ir::Inst& mid = ir_block.insts[j];
                for (u32 k = 0; k < mid.operand_count; ++k) {
                    if (mid.operands[k].kind != ir::OperandKind::Value) continue;
                    const ir::ValueId v = mid.operands[k].as_value();
                    if (v != def.result && home_pin[v] == guest) {
                        reads_old_pin = true;
                        break;
                    }
                }
            }
            if (reads_old_pin) continue;

            // 4. Nothing between may fault. Committing the destination early
            //    would expose a half-executed guest instruction to the fault
            //    handler, which harvests exactly these pins. Vacuous for this
            //    op set, and precisely the assumption a later op set breaks.
            bool faults_between = false;
            for (usize j = d + 1; j <= w; ++j) {
                if (ir_block.insts[j].may_fault) { faults_between = true; break; }
            }
            if (faults_between) continue;

            inplace[def.result] = pin;
        }
    };

    // Walk guest instructions, not IR ops, because every piece of per-
    // instruction bookkeeping below is keyed on the guest instruction and the
    // fault side table requires strictly ascending, non-overlapping host
    // intervals. The IR verifier already guarantees ops form contiguous
    // ascending runs by `guest_rip`, so that requirement is satisfied by
    // construction rather than by care.
    usize op = 0;
    u64 rip = block.start_addr;
    for (u32 i = 0; i < block.instruction_count; ++i) {
        m_diagnostic_instruction_rip = rip;
        const DecodedInstruction& instr = block.instructions[i];

        const u32 arm_before = static_cast<u32>(m_code_buf.size());
        const bool recorded = m_insn_pos_count < kMaxInsnArmPos;
        const u32 recorded_index = m_insn_pos_count;
        if (recorded) {
            m_insn_rips[m_insn_pos_count] = rip;
            m_insn_arm_pos[m_insn_pos_count] = arm_before;
            ++m_insn_pos_count;
        }

        if (current_rip != rip) current_rip = rip;

        // This guest instruction's op range, then the in-place plan over it.
        // `home_pin` must be populated for the range before planning, since
        // condition 3 asks which values live in which pin.
        usize range_end = op;
        while (range_end < ir_block.insts.size() &&
               ir_block.insts[range_end].guest_rip == rip) {
            const ir::Inst& scan = ir_block.insts[range_end];
            // Only a FULL-WIDTH read aliases its pin. A narrow read is an
            // EXTRACT — `al` is bits 7:0 of X23, not X23 — so recording a pin
            // home for it makes `needs_slot` skip it, leaves it with no pool
            // register, and drops every narrow operand into the same fallback
            // scratch. Measured symptom: `cmp al,bl` computed BL-BL and
            // reported ZF, `test al,bl` computed BL&BL.
            if (scan.op == ir::Op::GprRead && scan.defines_value() &&
                scan.type == ir::Type::I32) {
                const u8 p = pinned_gpr32(scan.operands[0].reg_index);
                if (p != 0xFF) home_pin[scan.result] = scan.operands[0].reg_index;
            }
            ++range_end;
        }
        survivor = plan_survivor(op, range_end);
        // One reused mask rather than a fresh vector per guest instruction:
        // this runs inside block compilation, and the previous instruction's
        // survivor must not leak into this one's plan.
        if (prev_survivor != ir::kInvalidValue) survivor_mask[prev_survivor] = 0;
        if (survivor != ir::kInvalidValue) survivor_mask[survivor] = 1;
        prev_survivor = survivor;
        if (!block_pool)
            (void)plan_pool_registers(ir_block, op, range_end, home_pin,
                                      survivor_mask, &pool_reg);
        plan_inplace(op, range_end);

        // A CF MERGE in this instruction captures the incoming carry FIRST,
        // before any operand is materialized.
        //
        // `emit_lazy_cf_to_call_tmp` clobbers X0-X4 — this emitter's entire
        // scratch pool — and may make a helper call. Capturing it after the
        // operands were loaded would destroy them. The stencil path emits it
        // in exactly the same position for exactly this reason
        // (`ops/arithmetic.cpp:1606`: "emit_guest_load32 clobbers W3, so a
        // register +1 would increment by the region base instead of 1").
        //
        // It also honours a live CLC/STC/CMC overlay before any NZCV or
        // FlagOp fast path, which is what makes `ADD; CLC; INC` inherit the
        // forced carry rather than the ADD's.
        [[maybe_unused]] bool captured_cf = false;
        for (usize j = op; j < range_end; ++j) {
            if (ir_block.insts[j].op == ir::Op::DefFlags &&
                ir_block.insts[j].operand_count == 3) {
                const CarrySource saved = m_flags.incoming_carry_source;
                m_flags.incoming_carry_source =
                    in_block_carry_source(ir_block, j);
                emit_lazy_cf_to_call_tmp();
                m_flags.incoming_carry_source = saved;
                captured_cf = true;
                break;
            }
        }

        // Arm the cold-stub record replay for this guest instruction. Cleared
        // and re-armed per instruction so a stub belonging to an uncovered
        // instruction cannot inherit a stale plan.
        // The vector pool is reused per guest instruction. `vloc` needs no
        // clearing: values are SSA and RIP-local, so no entry from a previous
        // instruction can be read.
        vnext = 0;

        m_ir_stub_replay = StubFlagReplay{};
        for (const ResolvedPlan& r : resolved_plans) {
            if (rip >= r.first_rip && rip <= r.last_rip) {
                m_ir_stub_replay = r.replay;
                break;
            }
        }

        // Region-cache lowering raises through a helper that deliberately does
        // NOT set RIP (it assumes the caller already did), so an instruction
        // that can fault publishes its own address first. On the linear path
        // (the 32-bit production default) a bad access is a real signal and
        // the handler recovers the RIP from the side table below, so this is
        // dead work and is skipped.
        if (region_path) {
            for (usize j = op; j < range_end; ++j) {
                if (!ir_block.insts[j].may_fault) continue;
                emit_update_rip(rip);
                break;
            }
        }

        for (; op < range_end; ++op) {
            const ir::Inst& in = ir_block.insts[op];
            switch (in.op) {
            case ir::Op::FlagsIn:
                break;  // architectural state; consumers read CpuState
            case ir::Op::GprRead: {
                if (in.type == ir::Type::I8 || in.type == ir::Type::I16) {
                    // A sub-register read is an EXTRACT, never an alias of the
                    // pin: aliasing would hand the consumer all 32 bits and
                    // `test al, al` would test EAX.
                    //
                    // The frontend already resolved AH/CH/DH/BH into the
                    // high-byte index space, so this asks one shared question
                    // rather than re-deriving the REX rule.
                    const u8 idx = in.operands[0].reg_index;
                    const bool high = ir::is_high_byte(in.type, idx);
                    const u8 guest = ir::architectural_gpr(in.type, idx);
                    const u8 dst = alloc_for(in.result);
                    const u8 pin_src = pinned_gpr32(guest);
                    u8 src = dst;
                    if (pin_src != 0xFF) {
                        src = pin_src;
                    } else {
                        emit_load_gpr32(dst, guest);
                    }
                    emit(arm64::ubfx_w(
                        dst, src, high ? 8 : 0,
                        in.type == ir::Type::I8 ? 8 : 16));
                    loc[in.result] = dst;
                    break;
                }
                const u8 pin = pinned_gpr32(in.operands[0].reg_index);
                if (pin != 0xFF) {
                    loc[in.result] = pin;  // already resident; emit nothing
                } else {
                    const u8 dst = alloc_for(in.result);
                    emit_load_gpr32(dst, in.operands[0].reg_index);
                    loc[in.result] = dst;
                }
                break;
            }
            case ir::Op::Const: {
                const u8 dst = alloc_for(in.result);
                emit_const_op(dst, in);
                loc[in.result] = dst;
                break;
            }
            case ir::Op::Add: case ir::Op::Sub: case ir::Op::And:
            case ir::Op::Or:  case ir::Op::Xor: {
                const u8 a = reg_of(in.operands[0]);
                const u8 b = reg_of(in.operands[1]);
                const u8 dst = alloc_for(in.result);
                // The FLAG-SETTING form when this result's flags were marked
                // resident in PSTATE. Same result either way; the S-form just
                // additionally leaves NZCV meaning what the record says, which
                // is what lets the block's branch be one `B.cond`.
                const bool s_form = nzcv_resident_result(op);
                switch (in.op) {
                    case ir::Op::Add:
                        emit(s_form ? arm64::adds_w(dst, a, b)
                                    : arm64::add_w(dst, a, b));
                        break;
                    case ir::Op::Sub:
                        emit(s_form ? arm64::subs_w(dst, a, b)
                                    : arm64::sub_w(dst, a, b));
                        break;
                    case ir::Op::And:
                        emit(s_form ? arm64::ands_w(dst, a, b)
                                    : arm64::and_w(dst, a, b));
                        break;
                    case ir::Op::Or:  emit(arm64::orr_w(dst, a, b)); break;
                    default:          emit(arm64::eor_w(dst, a, b)); break;
                }
                loc[in.result] = dst;
                break;
            }
            case ir::Op::Not: {
                const u8 dst = alloc_for(in.result);
                emit(arm64::mvn_w(dst, reg_of(in.operands[0])));
                loc[in.result] = dst;
                break;
            }
            case ir::Op::Neg: {
                // No `neg_w` encoder exists; SUB from WZR is what the stencil
                // lowering uses too (ops/arithmetic.cpp).
                const u8 dst = alloc_for(in.result);
                emit(arm64::sub_w(dst, 31, reg_of(in.operands[0])));
                loc[in.result] = dst;
                break;
            }
            case ir::Op::Lea: {
                const u8 dst = alloc_for(in.result);
                emit_lea_op(dst, in);
                loc[in.result] = dst;
                break;
            }
            case ir::Op::Shl:
            case ir::Op::Shr:
            case ir::Op::Sar: {
                const u8 d = alloc_for(in.result);
                const u8 a = reg_of(in.operands[0]);
                const u8 n = static_cast<u8>(in.operands[1].payload);
                switch (in.op) {
                case ir::Op::Shl: emit(arm64::lsl_w_imm(d, a, n)); break;
                case ir::Op::Shr: emit(arm64::lsr_w_imm(d, a, n)); break;
                default:          emit(arm64::asr_w_imm(d, a, n)); break;
                }
                loc[in.result] = d;
                break;
            }
            case ir::Op::Zext:
            case ir::Op::Sext: {
                const u8 d = alloc_for(in.result);
                const u8 src = reg_of(in.operands[0]);
                const bool byte = in.operands[0].type == ir::Type::I8;
                if (in.op == ir::Op::Sext) {
                    emit(byte ? arm64::sxtb_w(d, src) : arm64::sxth_w(d, src));
                } else {
                    emit(byte ? arm64::uxtb_w(d, src) : arm64::uxth_w(d, src));
                }
                loc[in.result] = d;
                break;
            }
            case ir::Op::Load: {
                const u8 addr = reg_of(in.operands[0]);
                const u8 dst = alloc_for(in.result);
                if (in.mem.width == ir::Type::I8 ||
                    in.mem.width == ir::Type::I16) {
                    // Both primitives zero-extend into the destination W
                    // register, which is what the narrow value model wants:
                    // every narrow value here is held zero-extended, so a
                    // 32-bit ALU on it produces the right low bits and the
                    // lazy record's own cast does the rest.
                    if (in.mem.width == ir::Type::I8) {
                        emit_guest_load8(dst, addr);
                    } else {
                        emit_guest_load16(dst, addr);
                    }
                    loc[in.result] = dst;
                    remat_after_access(op, range_end);
                    break;
                }
                // A faulting LDR does not write its destination, so putting
                // the result straight into the guest's pin is safe even here:
                // if the access faults, the pin still holds the architectural
                // value the fault handler will harvest.
                emit_guest_load32(dst, addr);
                loc[in.result] = dst;
                remat_after_access(op, range_end);
                break;
            }
            case ir::Op::Store: {
                const u8 addr = reg_of(in.operands[0]);
                const u8 value = reg_of(in.operands[1]);
                if (in.mem.width == ir::Type::I8 ||
                    in.mem.width == ir::Type::I16) {
                    if (in.mem.width == ir::Type::I8) {
                        emit_guest_store8(value, addr);
                    } else {
                        emit_guest_store16(value, addr);
                    }
                    remat_after_access(op, range_end);
                    break;
                }
                // NZCV is handled INSIDE the primitive: it consults
                // `should_hold_nzcv()` and either brackets the store with the
                // [SP,#8] spill/fill or defers the save into its cold stub.
                // Doing it here as well would nest the depth counter and pay
                // twice.
                emit_guest_store32(value, addr);
                remat_after_access(op, range_end);
                break;
            }
            case ir::Op::Jcc: {
                // FUSED when the residency pass proved this condition's
                // producer also leaves its result in host PSTATE, and the
                // backend can read this particular x86 condition out of that
                // kind. `fused_arm_cond` is where the disagreements live: ARM
                // and x86 differ on the carry sense, ANDS clears C and V, and
                // parity has no NZCV equivalent at all.
                //
                // Fusion is purely an ADDITIONAL fast path. The lazy record is
                // still published — a fault, or the next block, still needs the
                // architectural home — which is exactly what the two-homes
                // lattice (`InCpuStateRecord` + a kind) is for.
                int fused = -1;
                for (const ir::Inst& d : ir_block.insts) {
                    if (d.op != ir::Op::DefFlags) continue;
                    if (d.result != in.operands[0].as_value()) continue;
                    if (d.flags.nzcv == ir::NzcvKind::None) break;
                    fused = fused_arm_cond(
                        static_cast<NzcvState>(d.flags.nzcv),
                        in.branch.cond);
                    break;
                }

                if (fused < 0) {
                    // `emit_eval_lazy_condition` clobbers W0-W4 and NZCV,
                    // which is free at a terminator: the block ends here and
                    // no IR value is live past it.
                    emit_eval_lazy_condition(in.branch.cond);
                }

                const usize skip_pos = m_code_buf.size();
                emit(0);  // patched below: B.cond !cc / CBZ W0

                // IN-BLOCK LOOP: a taken edge back to this block's own start
                // branches straight to the body instead of leaving through the
                // tail.
                //
                // Without this, a tight loop pays a linked hop per iteration,
                // and every backward hop is counted against `kMaxLinkedHops`
                // (4095). Measured on crc32: 16,003 of 16,128 dispatcher
                // returns were `const tail: hop cap`, against ZERO for Tier 0,
                // which has had this since `find_in_block_loop_pos`. The
                // budget exists for the GUI pump and lease bounds, not as a
                // throughput cap -- the right fix is not to spend it on an
                // edge that never has to leave the block.
                //
                // Restricted to `target == block start`, deliberately, where
                // Tier 0 also accepts any recorded in-block RIP. Tier 0's
                // stencils are independent per guest instruction, so entering
                // mid-block is safe there. Tier 1 allocates across
                // instructions (`pool_reg`/`survivor`), so a mid-block entry
                // could read a pool register that this path never wrote. The
                // block-start case re-executes the whole body and needs no
                // such assumption.
                //
                // The cap exit falls into the ordinary taken-edge tail below,
                // which publishes the TARGET rip. Exiting at the Jcc's own rip
                // (as Tier 0 does) would re-execute the branch and so would
                // need its flags still architecturally present -- exactly what
                // the recovery pass is allowed to elide.
                const bool in_block_loop =
                    !m_block_is_64bit &&
                    in.branch.target == m_block_start_addr;
                if (in_block_loop) {
                    static_assert(MAX_IN_BLOCK_LOOP_ITERS == 0xFFFFu);
                    emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                    // Bit 16 set means the counter reached 65536: skip the
                    // backward branch and take the tail.
                    emit(arm64::tbnz(REG_LOOP_CTR, 16, 8));
                    const i32 loop_off =
                        static_cast<i32>(m_body_start_pos -
                                         m_code_buf.size()) * 4;
                    emit(arm64::b_imm(loop_off));
                    // Undo the increment so the published iteration count
                    // stays exact on the exit path.
                    emit(arm64::sub_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                }

                // Taken edge.
                emit_update_rip(in.branch.target);
                if (!emit_linked_tail_continue(in.branch.target,
                                               NzcvState::None)) {
                    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                           jit_iters_offset() >> 2));
                    emit_epilogue(JitExecResult::Continue);
                }

                const usize nt_pos = m_code_buf.size();
                const i32 skip_off = static_cast<i32>(nt_pos - skip_pos) * 4;
                m_code_buf[skip_pos] =
                    fused >= 0
                        ? arm64::b_cond(static_cast<u8>(fused) ^ 1u, skip_off)
                        : arm64::cbz_w(0, skip_off);

                // Fall-through edge.
                emit_update_rip(in.branch.fallthrough);
                if (!emit_linked_tail_continue(in.branch.fallthrough,
                                               NzcvState::None)) {
                    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                           jit_iters_offset() >> 2));
                    emit_epilogue(JitExecResult::Continue);
                }
                break;
            }
            case ir::Op::JmpIndirect: {
                // The target is DATA, so there is no static successor to link
                // to and no `emit_update_rip` to use — that helper
                // materializes a compile-time immediate. The runtime value is
                // published as the guest RIP, which is also the precondition
                // `emit_linked_tail_reg` states.
                //
                // `cpu->rip` is a u64 and a 32-bit guest's EIP is a W value, so
                // the MOV is doing real work: it zero-extends. Storing the X
                // register without it would publish whatever garbage sat in
                // the upper half.
                const u8 target = reg_of(in.operands[0]);
                emit(arm64::mov_w(REG_SCRATCH0, target));
                // Preserve the target before the predictor clobbers X2.
                if (in.branch.return_site != ir::kInvalidRip) {
                    emit_rsb_push(in.branch.return_site);
                }
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       rip_offset() >> 3));
                m_rip_reg_pos = static_cast<usize>(-1);

                // A RET pops the RETURN-ADDRESS PREDICTOR before falling back
                // to the L1 lookup; an indirect JMP has no prediction to make.
                // Lowering both as a plain dispatcher exit — which is what
                // this did before — measured **7.7x slower** than the stencil
                // compiler on a call/ret loop, because every return paid a
                // full round trip. That regression was invisible until the
                // 32-bit bench set grew a `call_ret` kernel.
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
            case ir::Op::Jmp: {
                // Prime the return-address predictor before linking to the
                // callee. Without this every matching RET misses its RSB slot,
                // which is half of the 7.7x call/ret regression above.
                //
                // Emitted BEFORE `emit_update_rip`, because `emit_rsb_push`
                // clobbers X2/X16/X17/W3/W4 and `emit_update_rip` leaves the
                // target in X0 for the tail that follows it.
                if (in.branch.return_site != ir::kInvalidRip) {
                    emit_rsb_push(in.branch.return_site);
                }
                emit_update_rip(in.branch.target);
                if (!emit_linked_tail_continue(in.branch.target,
                                               NzcvState::None)) {
                    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                           jit_iters_offset() >> 2));
                    emit_epilogue(JitExecResult::Continue);
                }
                break;
            }
            case ir::Op::GprWrite: {
                const ir::Type wt = in.operands[0].type;
                if (wt == ir::Type::I8 || wt == ir::Type::I16) {
                    // x86's merge rule: `mov al,5` leaves EAX[31:8] alone and
                    // `mov ah,5` writes bits 15:8. A bitfield insert into the
                    // pin is exactly that, and it is why a narrow write is not
                    // a move — writing the whole pin would silently clear the
                    // upper bits of a register the guest still needs.
                    // Through the primitive, NOT a bare BFI at the pin: the
                    // primitive owns the invalidation a register write implies
                    // (`m_esp_frame_live`), and emitting the insert here
                    // skipped it — a stale ESP-frame cache and a host SIGSEGV.
                    emit_merge_gpr_narrow(
                        reg_of(in.operands[1]),
                        ir::architectural_gpr(wt, in.operands[0].reg_index),
                        ir::subregister_lsb(wt, in.operands[0].reg_index),
                        wt == ir::Type::I8 ? 8 : 16);
                    break;
                }
                // Elides to nothing when the value already lives in that pin.
                emit_store_gpr32(reg_of(in.operands[1]),
                                 in.operands[0].reg_index);
                const auto value = in.operands[1].as_value();
                const u8 pin = pinned_gpr32(in.operands[0].reg_index);
                if (pin != 0xFF && home_pin[value] == 0xFF) {
                    // After the commit this value has an architectural home;
                    // helpers and fault recovery already preserve that pin.
                    home_pin[value] = in.operands[0].reg_index;
                    loc[value] = pin;
                }
                break;
            }
            case ir::Op::SegBase: {
                const u8 d = alloc_for(in.result);
                emit(arm64::ldr_w_uoff(
                    d, REG_CPU_STATE,
                    seg_base_offset(static_cast<SegReg>(
                        in.operands[0].payload)) >> 2));
                loc[in.result] = d;
                break;
            }
            case ir::Op::XmmRead: {
                const u8 v = vnext++;
                vloc[in.result] = v;
                emit(arm64::ldr_q_uoff(
                    v, REG_CPU_STATE,
                    xmm_u64_off(in.operands[0].reg_index, 0) >> 4));
                break;
            }
            case ir::Op::XmmWrite: {
                emit(arm64::str_q_uoff(
                    vloc[in.operands[1].as_value()], REG_CPU_STATE,
                    xmm_u64_off(in.operands[0].reg_index, 0) >> 4));
                break;
            }
            case ir::Op::VXor:
            case ir::Op::VAnd:
            case ir::Op::VOr: {
                const u8 d = vnext++;
                vloc[in.result] = d;
                const u8 a = vloc[in.operands[0].as_value()];
                const u8 b = vloc[in.operands[1].as_value()];
                emit(in.op == ir::Op::VXor ? arm64::eor_v16b(d, a, b)
                   : in.op == ir::Op::VAnd ? arm64::and_v16b(d, a, b)
                                           : arm64::orr_v16b(d, a, b));
                break;
            }
            case ir::Op::VInsertLow32: {
                // MOVSS xmm,xmm: bits 127:32 come from operand 0, bits 31:0
                // from operand 1. Copy then insert, rather than insert in
                // place, because operand 0 is an SSA value that a later op in
                // the same instruction could still read.
                const u8 d = vnext++;
                vloc[in.result] = d;
                emit(arm64::mov_v16b(d, vloc[in.operands[0].as_value()]));
                emit(arm64::ins_v_gpr(d, 0, reg_of(in.operands[1]),
                                      arm64::VS_S));
                break;
            }
            case ir::Op::VZext32: {
                // MOVSS xmm,m32: FMOV Sd,Wn writes lane 0 and zeroes the rest
                // of the register, which is exactly the architectural rule.
                const u8 d = vnext++;
                vloc[in.result] = d;
                emit(arm64::fmov_s_from_w(d, reg_of(in.operands[0])));
                break;
            }
            case ir::Op::VLow32: {
                const u8 d = alloc_for(in.result);
                loc[in.result] = d;
                emit(arm64::fmov_w_from_s(
                    d, vloc[in.operands[0].as_value()]));
                break;
            }
            case ir::Op::DefFlags: {
                // A definition the flag-demand pass proved dead emits nothing.
                // This is the pass's entire payoff: `emit_set_lazy_flags`
                // publishes an op byte plus two 64-bit words, and on a run of
                // ALU instructions all but the last of those stores is written
                // and immediately overwritten.
                if (in.flags.repr == ir::FlagRepr::Dead) break;
                // The IR's FlagDef is the authority on whether this record is
                // published, so Tier 0's liveness side table must not also get
                // a vote: `emit_set_lazy_flags` silently emits nothing when
                // `m_omit_lazy_current` is set, and that flag belongs to the
                // stencil path's analysis, not this one.
                m_omit_lazy_current = false;
                m_omit_lazy_keep_overlay = false;
                // Operand 0 -> flag_src (the x86 source operand), operand 1 ->
                // flag_dst (the result). This is the contract stated in
                // ir_frontend.cpp and it matches emit_set_lazy_flags' own
                // parameter meaning; asserting it here rather than re-deriving
                // it is deliberate, because getting it backwards is wrong only
                // for asymmetric operands.
                if (in.flags.shift_count != 0) {
                    // `flag_dst` is `(count << 32) | result`, so this needs
                    // THREE inputs where every other record needs two. The
                    // count is a compile-time constant on the FlagDef and is
                    // materialized here; `emit_set_shift_lazy_flags` is the
                    // same primitive the stencil path uses, so the two tiers
                    // cannot disagree about the packing.
                    emit_mov_imm32(REG_SCRATCH4, in.flags.shift_count);
                    emit_set_shift_lazy_flags(in.flags.record,
                                              reg_of(in.operands[0]),
                                              REG_SCRATCH4,
                                              reg_of(in.operands[1]));
                    break;
                }
                if (in.operand_count == 3) {
                    // The CF-preserving publisher packs
                    // `flag_src = (CF << 32) | u32(original)`, reading the
                    // original at X width — so it must be zero-extended first.
                    // A 32-bit block's pin carries bits 31:0 only and its
                    // upper half is not guaranteed clean, so passing the pin
                    // directly would OR guest garbage into the carry byte.
                    //
                    // REG_SCRATCH4 is the temp: outside this emitter's pool,
                    // and not REG_SCRATCH3, which the publisher itself uses.
                    // (`emit_lazy_cf_to_call_tmp` clobbered W4 earlier, but it
                    // ran before any of this instruction's operands existed.)
                    emit(arm64::mov_w(REG_SCRATCH4, reg_of(in.operands[0])));
                    emit_set_lazy_flags_preserve_cf(
                        in.flags.record, REG_SCRATCH4,
                        reg_of(in.operands[1]), REG_CALL_TMP);
                } else {
                    emit_set_lazy_flags(in.flags.record,
                                        reg_of(in.operands[0]),
                                        reg_of(in.operands[1]));
                }
                break;
            }
            default:
                break;  // unreachable: ir_emit_feasible rejected it
            }
        }

        // Per-instruction bookkeeping the stencil loop performs inside
        // `compile_instruction`. Dropping any of it is invisible until
        // something needs it: a missing RIP interval is a wrong fault address,
        // a missing coverage record silently lowers the ISA ratchet.
        if (recorded) {
            m_insn_arm_end[recorded_index] = static_cast<u32>(m_code_buf.size());
        }
        ++m_block_native_insns;
        // The IR tier only emits when it has a real lowering for the
        // instruction, so every instruction it commits is Native by
        // construction; it has no helper or delegate path of its own.
        coverage::record_compiled(instr.opcode, form_key(instr),
                                  m_block_is_64bit, LowerKind::Native);
        if (stats::enabled()) {
            const u32 arm_words =
                static_cast<u32>(m_code_buf.size()) - arm_before;
            stats::record_insn(instr.opcode, m_block_is_64bit, true, arm_words);
            m_stats_insns.push_back(stats::InsnSite{
                rip, static_cast<u32>(instr.opcode),
                static_cast<u32>(instr.length), arm_before, arm_words});
        }

        rip += instr.length;
    }

    m_ir_stub_replay = StubFlagReplay{};

    // Translate the pass's guest-level recovery plans into per-instruction
    // descriptors the fault handler can act on.
    //
    // The pass reasons about guest registers because it must — it is deciding
    // an elision from dataflow, and knows nothing about pins. The pin map is
    // the backend's, and it is what turns "EAX" into "X23". Doing the
    // translation HERE, after emission, means it sees the same pin assignment
    // the block was actually compiled against.
    //
    // A guest register with no pin cannot be recovered: the fault handler
    // reads the interrupted register file, and an unpinned register is only in
    // CpuState, whose value at the fault is whatever the block last flushed.
    // Such a plan is dropped, which simply means the record it would have
    // elided is... still elided. So this must NOT silently drop: it is a
    // correctness obligation the pass already discharged on the assumption
    // that this translation succeeds. Every guest index the planner allows is
    // pinned in a 32-bit block (0-7 map to X23-X26/X12/X13/X27/X28), so the
    // drop path is unreachable — and it fails LOUD rather than quiet, because
    // a silent drop here is unpublished flags at a fault.
    for (const ir::FlagRecoveryPlan& plan : ir_block.flag_recovery) {
        if (!plan.active()) continue;
        JitFlagRecovery fr{};
        fr.flag_op = plan.flag_op;
        fr.width = 32;
        fr.mode = plan.recompute_dst ? JitFlagRecovery::RecomputeDst
                                     : JitFlagRecovery::DirectDst;
        bool ok = true;
        const auto place = [&](bool is_imm, u8 guest, u64 imm, u8& out_reg,
                               u64& out_imm) {
            if (is_imm) {
                out_reg = JitFlagRecovery::kImmediate;
                out_imm = imm;
                return;
            }
            const u8 pin = pinned_gpr32(guest);
            if (pin == 0xFF) { ok = false; return; }
            out_reg = pin;
        };
        place(plan.src_is_imm, plan.src_reg, plan.src_imm, fr.src_reg,
              fr.src_imm);
        place(plan.dst_is_imm, plan.dst_reg, plan.dst_imm, fr.dst_reg,
              fr.dst_imm);
        if (!ok) {
            std::fprintf(stderr,
                         "[ir] FATAL: flag-recovery plan names an unpinned "
                         "guest register; block 0x%llx\n",
                         static_cast<unsigned long long>(ir_block.entry_rip));
            std::abort();
        }
        for (u32 i = 0; i < m_insn_pos_count; ++i) {
            if (m_insn_rips[i] >= plan.first_rip &&
                m_insn_rips[i] <= plan.last_rip) {
                m_insn_flag_recovery[i] = fr;
            }
        }
    }

    // Terminate the block. The stencil path does this from inside the LAST
    // instruction's lowering (`is_last`), not after its loop, so skipping the
    // loop skips the terminator too — and a block that falls off the end of
    // its own emitted code runs into whatever the arena holds next. That is a
    // SIGILL if you are lucky.
    //
    // A block whose last op IS a branch already emitted both of its edges, so
    // emitting a fall-through tail after it would be dead code at best and, if
    // the branch's own tail were ever shorter than expected, a second entry
    // point into it.
    if (!ir_block.insts.empty() &&
        ir::is_terminator(ir_block.insts.back().op)) {
        return;
    }

    emit_update_rip(rip);
    if (!emit_linked_tail_continue(rip, m_flags.nzcv)) {
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);
    }
}

}  // namespace vitreo::cpu::jit
