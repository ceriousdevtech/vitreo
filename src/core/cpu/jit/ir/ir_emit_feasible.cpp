// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_emit_feasible.cpp
/// @brief `JitCompiler::ir_emit_feasible`: every reason the Tier 1 backend
///        declines a block, checked before Seam A returns (see "Emission
///        cannot fail" in ir_emit_internal.hpp).

#include "ir_emit_internal.hpp"

namespace vitreo::cpu::jit {

bool JitCompiler::ir_emit_feasible(const ir::IrBlock& block) const noexcept {
    m_ir_emit_decline = ir::DeclineReason::BackendUnsupported;
    // Check lowering and register lifetimes beyond the structural verifier:
    // pure scalar blocks share the width-aware register allocator. Long-mode
    // helper blocks preserve snapshots in bounded invocation-local slots.
    m_ir_plan.snapshot_path = false;
    if (block.is_64bit || ir::requires_snapshot_backing(block)) {
        // A block can lower to NOTHING: a zero-count shift modifies neither
        // its destination nor any flag, so a block containing only one
        // produces no ops. The 32-bit emitter handles that and is tested on
        // it; this one has no op to hang the RIP advance and the terminator
        // on, so it declines and the stencil path takes the block.
        if (block.insts.empty()) return false;
        if (!ir::snapshot_block_allocatable(block)) return false;
        for (const auto& in : block.insts) {
            if (in.may_fault && in.op != ir::Op::Load && in.op != ir::Op::Store &&
                in.op != ir::Op::Divide) return false;
            switch (in.op) {
            case ir::Op::Load:
                if (in.mem.width != ir::Type::I32 && in.mem.width != ir::Type::I64 &&
                    in.mem.width != ir::Type::I8 && in.mem.width != ir::Type::I16 &&
                    in.mem.width != ir::Type::V128) return false;
                if (in.type != in.mem.width) return false;
                break;
            case ir::Op::XmmRead:
            case ir::Op::XmmWrite: {
                // Both ends are a Q-form load/store off CpuState, whose
                // offset is a 16-scaled uimm12. A guest whose CpuState put the
                // XMM array out of that reach would emit a WRONG offset rather
                // than fail, so it is checked here. Long mode has sixteen.
                const u8 idx = in.operands[0].reg_index;
                if (idx >= 16) return false;
                if (!fits_ldr_q_offset(xmm_u64_off(idx, 0))) return false;
                break;
            }
            case ir::Op::VXor: case ir::Op::VAnd: case ir::Op::VOr:
            case ir::Op::VAdd: case ir::Op::VSub:
            case ir::Op::VCmpEq: case ir::Op::VCmpGt:
            case ir::Op::VMinU: case ir::Op::VMaxU:
            case ir::Op::VMinS: case ir::Op::VMaxS:
            case ir::Op::VMulLo: case ir::Op::VAvgU:
            case ir::Op::VQAddS: case ir::Op::VQAddU:
            case ir::Op::VQSubS: case ir::Op::VQSubU:
            case ir::Op::VAbs:
            case ir::Op::VZipLo: case ir::Op::VZipHi:
            case ir::Op::VPackS: case ir::Op::VPackU:
            case ir::Op::VShlImm: case ir::Op::VShrImm: case ir::Op::VSarImm:
            case ir::Op::VInsertLow32:
            case ir::Op::VZext32:
            case ir::Op::VLow32:
                break;
            case ir::Op::Lea: case ir::Op::SegBase:
                if (in.type != ir::Type::I32 && in.type != ir::Type::I64) return false;
                break;
            case ir::Op::Add: case ir::Op::Sub: case ir::Op::And:
            case ir::Op::Or: case ir::Op::Xor: case ir::Op::Not: case ir::Op::Neg:
                // Narrow ALU computes in a W register and leaves the bits
                // above the field undefined: the lazy record masks both
                // payloads to its own width, and the merge and the store take
                // only `width` bits.
                if (in.type != ir::Type::I32 && in.type != ir::Type::I64 &&
                    in.type != ir::Type::I8 && in.type != ir::Type::I16) return false;
                break;
            case ir::Op::Mul: case ir::Op::SMulH:
                // I64 only: the lowering has an X form and no W form, because
                // every caller wants the FULL product of narrower operands,
                // which is an I64 multiply of extended halves, not a W one.
                if (in.type != ir::Type::I64) return false;
                break;
            case ir::Op::Rol: case ir::Op::Ror:
                // 32- and 64-bit rotates are one ARM64 ROR; the narrow ones are
                // lowered by doubling the field into a 32-bit register, so only
                // the widths that fit do.
                if (!ir::is_integer_type(in.type)) return false;
                if (in.operands[1].kind != ir::OperandKind::Imm) return false;
                break;
            case ir::Op::FlagsImage:
            case ir::Op::FlagsInstall:
            case ir::Op::Divide:
            case ir::Op::SegSelector:
                break;
            case ir::Op::SetCf: case ir::Op::SetOf:
                if (in.operands[0].type == ir::Type::Flags) return false;
                break;
            case ir::Op::EvalCc:
                if (in.type != ir::Type::I8) return false;
                break;
            case ir::Op::Select:
                if (in.type != ir::Type::I16 && in.type != ir::Type::I32 &&
                    in.type != ir::Type::I64) return false;
                break;
            case ir::Op::Shl: case ir::Op::Shr: case ir::Op::Sar:
                // By a compile-time count only. Every integer width is
                // lowerable; what `Shl64`/`Shr64`/`Sar64` cannot do is RECORD
                // a carry out, and that is a constraint on the guest form the
                // frontend admits, not on the arithmetic. ADC's packed
                // `flag_src` shifts at I64 and publishes no shift record.
                if (!ir::is_integer_type(in.type)) return false;
                if (in.operands[1].kind != ir::OperandKind::Imm &&
                    !(in.operands[1].kind == ir::OperandKind::Value &&
                      in.operands[1].type == ir::Type::I8)) return false;
                break;
            case ir::Op::Const:
                // A narrow constant is the source of a narrow MOV. It
                // materializes with the same MOV immediate; only the field
                // width the consumer merges differs.
                if (in.type != ir::Type::I32 && in.type != ir::Type::I64 &&
                    in.type != ir::Type::I8 && in.type != ir::Type::I16) return false;
                break;
            case ir::Op::GprRead:
                // A narrow read is admitted as an EXTRACT. Narrow writes are
                // not: a partial-register merge is M11b's next tranche.
                if (in.type != ir::Type::I32 && in.type != ir::Type::I64 &&
                    in.type != ir::Type::I8 && in.type != ir::Type::I16) return false;
                break;
            case ir::Op::Trunc:
                if (!ir::is_integer_type(in.type)) return false;
                break;
            case ir::Op::Zext: case ir::Op::Sext:
                // An I16 result is MOVZX/MOVSX into a 16-bit destination,
                // which the narrow GprWrite then merges.
                if (in.type != ir::Type::I32 && in.type != ir::Type::I64 &&
                    in.type != ir::Type::I16) return false;
                if (in.operands[0].type != ir::Type::I8 &&
                    in.operands[0].type != ir::Type::I16 &&
                    in.operands[0].type != ir::Type::I32) return false;
                break;
            case ir::Op::FlagsIn: case ir::Op::Jmp: case ir::Op::Jcc:
                break;
            case ir::Op::CondExit:
                if (in.branch.cond > 0xF) return false;
                break;
            case ir::Op::JmpIndirect:
                // The target is a value, and at this width it is the whole
                // register -- no zero-extending MOV, and nothing narrower is
                // a legal long-mode target.
                if (in.operands[0].type != ir::Type::I64) return false;
                break;
            case ir::Op::Store:
                if (in.mem.width != ir::Type::I32 && in.mem.width != ir::Type::I64 &&
                    in.mem.width != ir::Type::I8 && in.mem.width != ir::Type::I16 &&
                    in.mem.width != ir::Type::V128) return false;
                if (in.operands[1].type != in.mem.width) return false;
                break;
            case ir::Op::GprWrite:
                if (in.operands[0].type != ir::Type::I32 &&
                    in.operands[0].type != ir::Type::I64 &&
                    in.operands[0].type != ir::Type::I8 &&
                    in.operands[0].type != ir::Type::I16) return false;
                break;
            case ir::Op::AddCarry: case ir::Op::SubBorrow:
                if (in.type != ir::Type::I32 && in.type != ir::Type::I8 &&
                    in.type != ir::Type::I16) return false;
                break;
            case ir::Op::DefFlags:
                // keep_carry_in_pstate's definitions emit nothing; the
                // emitter checks the claim they name is the live one.
                if (in.flags.repr == ir::FlagRepr::InPstate) {
                    if (in.operand_count != 1 ||
                        (in.flags.nzcv != ir::NzcvKind::Add &&
                         in.flags.nzcv != ir::NzcvKind::Sub)) return false;
                    break;
                }
                if ((in.operand_count != 2 && in.operand_count != 3) || in.flags.overlay_live ||
                    in.flags.keep_overlay ||
                    (in.flags.repr != ir::FlagRepr::Dead &&
                     in.flags.repr != ir::FlagRepr::InCpuStateRecord)) return false;
                // A shift record packs its count into `flag_dst`; it needs the
                // three-input publisher, which the emitter selects on this.
                // A by-CL shift brings the count as a third VALUE operand.
                if (in.flags.shift_count != 0 && in.operand_count != 2) return false;
                break;
            default: return false;
            }
            for (u32 k = 0; k < in.operand_count; ++k) {
                const auto& operand = in.operands[k];
                if (operand.kind == ir::OperandKind::Value && operand.type != ir::Type::Flags &&
                    operand.as_value() >= block.next_value) return false;
            }
        }
        // The vector "allocator" is a bump over the V registers with no reuse,
        // so a block defining more vector values than there are registers
        // would wrap into a different register and silently compute with the
        // wrong one. Bounded here rather than clamped at the bump.
        {
            u32 vector_defs = 0;
            for (const ir::Inst& in : block.insts)
                if (in.defines_value() && in.type == ir::Type::V128)
                    ++vector_defs;
            if (vector_defs > 32) return false;
        }

        // A V128 value must not be live across a may-fault op.
        //
        // V0-V7 are caller-saved, so a helper a guest access can call clobbers
        // them -- and unlike a GPR there is nowhere recoverable to put a
        // vector instead: the fault handler harvests `JitPinMap` and saves no
        // vector state at all. Every admitted vector shape loads, computes and
        // stores inside one guest instruction, so this costs nothing today and
        // stops a future form from depending on luck.
        for (usize d = 0; d < block.insts.size(); ++d) {
            const ir::Inst& def = block.insts[d];
            if (!def.defines_value() || def.type != ir::Type::V128) continue;
            usize last_use = d;
            for (usize u = d + 1; u < block.insts.size(); ++u)
                for (u32 k = 0; k < block.insts[u].operand_count; ++k) {
                    const ir::Operand& o = block.insts[u].operands[k];
                    if (o.kind == ir::OperandKind::Value &&
                        o.as_value() == def.result) last_use = u;
                }
            for (usize m = d + 1; m < last_use; ++m)
                if (block.insts[m].may_fault) return false;
        }

        // The plan is the feasibility answer: a block is lowerable exactly
        // when every value it cannot keep in a pin, a constant or a pool
        // register fits the bounded slot backing.
        if (!ir_plan_snapshot_block(block)) {
            m_ir_emit_decline = ir::DeclineReason::BackendRegisterPressure;
            return false;
        }
        m_ir_plan.snapshot_path = true;
        return true;
    }

    const bool block_pool = ir::scalar_block_allocatable(block);
    u16 commit_pin_mask = 0;
    for (u8 reg = 0; reg < 16; ++reg)
        if (pinned_gpr32(reg) != 0xFF) commit_pin_mask |= static_cast<u16>(1u << reg);

    // owner_rip[v] = the guest RIP of the op that defined value v.
    std::vector<u64> owner(block.next_value, ir::kInvalidRip);
    // PRE-PASS: which values are parked in the survivor register.
    //
    // This has to run before the pressure check, not alongside it. A survivor
    // is DISCOVERED at the access it must outlive, but it was DEFINED earlier
    // — for `sub [mem], imm` the result is defined four ops before the store
    // it survives. Counting it against the three-register pool at definition
    // time is what made every read-modify-write decline on register pressure
    // the moment the form started building.
    //
    // `pin_home` for this pass is the simple version: a value read out of a
    // pinned register. The tombstoning the main pass does is irrelevant here,
    // because a tombstoned value cannot be used at all.
    std::vector<u8> pre_pin(block.next_value, 0xFF);
    std::vector<u8> is_survivor(block.next_value, 0);
    // A V128 value must not be live across a may-fault op.
    //
    // V0-V7 are caller-saved, so a helper a guest access can call clobbers
    // them — and unlike a GPR there is nowhere recoverable to put a vector
    // instead: the fault handler harvests `JitPinMap` and saves no vector
    // state at all. The MOVSS and XORPS shapes never do this (a store extracts
    // to a GPR before the access, a load fills the vector after it), so this
    // costs nothing today and stops a future form from depending on luck.
    //
    // A separate pass because it is a def-to-last-use question, which the
    // per-op loop below is the wrong shape to ask.
    for (usize d = 0; d < block.insts.size(); ++d) {
        const ir::Inst& def = block.insts[d];
        if (!def.defines_value() || def.type != ir::Type::V128) continue;
        usize last_use = d;
        for (usize u = d + 1; u < block.insts.size(); ++u) {
            for (u32 k = 0; k < block.insts[u].operand_count; ++k) {
                const ir::Operand& o = block.insts[u].operands[k];
                if (o.kind == ir::OperandKind::Value &&
                    o.as_value() == def.result) {
                    last_use = u;
                }
            }
        }
        for (usize m = d + 1; m < last_use; ++m) {
            if (block.insts[m].may_fault) return false;
        }
    }

    for (const ir::Inst& in : block.insts) {
        if (in.op == ir::Op::GprRead && in.defines_value() &&
            in.result < pre_pin.size()) {
            const u8 p = pinned_gpr32(in.operands[0].reg_index);
            if (p != 0xFF) pre_pin[in.result] = in.operands[0].reg_index;
        }
    }
    for (usize m = 0; m < block.insts.size(); ++m) {
        const ir::Inst& access = block.insts[m];
        if (access.op != ir::Op::Load && access.op != ir::Op::Store) continue;
        for (usize d = 0; d < m; ++d) {
            const ir::Inst& def = block.insts[d];
            if (!def.defines_value() || def.type != ir::Type::I32) continue;
            if (def.guest_rip != access.guest_rip) continue;
            if (def.result >= pre_pin.size()) continue;
            if (pre_pin[def.result] != 0xFF) continue;
            bool used_after = false;
            for (usize j = m + 1;
                 j < block.insts.size() &&
                 block.insts[j].guest_rip == access.guest_rip && !used_after;
                 ++j) {
                for (u32 k = 0; k < block.insts[j].operand_count; ++k) {
                    const ir::Operand& o = block.insts[j].operands[k];
                    if (o.kind == ir::OperandKind::Value &&
                        o.as_value() == def.result) {
                        used_after = true;
                        break;
                    }
                }
            }
            if (!used_after) continue;
            if (rematerializable(block, def.result, pre_pin)) continue;
            is_survivor[def.result] = 1;
        }
    }
    const auto survivor_in_insn = [&](u64 rip) {
        for (const ir::Inst& in : block.insts) {
            if (in.guest_rip != rip) continue;
            if (in.defines_value() && in.result < is_survivor.size() &&
                is_survivor[in.result]) {
                return true;
            }
        }
        return false;
    };

    // Only one survivor register exists, so a guest instruction needing two
    // declines rather than half-building an allocator.
    {
        u64 rip = ir::kInvalidRip;
        u32 count = 0;
        for (const ir::Inst& in : block.insts) {
            if (in.guest_rip != rip) { rip = in.guest_rip; count = 0; }
            if (in.defines_value() && in.result < is_survivor.size() &&
                is_survivor[in.result] && ++count > 1) {
                m_ir_emit_decline =
                    ir::DeclineReason::BackendRegisterPressure;
                return false;
            }
        }
    }
    // pin_home[v] = the guest register whose PIN holds v, or 0xFF. A value
    // read out of a pinned guest register is not copied anywhere: it simply
    // IS that pin. So a later `GprWrite` to the same guest register destroys
    // it, and any use after that would silently read the newly written value.
    //
    // That is not hypothetical -- it is the NEG bug this check was added for.
    // `neg eax` reads EAX (the pin), computes, writes EAX, and then defines
    // flags from the ORIGINAL EAX; with the write emitted first, the flag
    // record received the result where the original belonged. Wrong only for
    // the forms whose record needs the pre-write destination, which is why an
    // ADD-shaped test corpus would never have found it.
    //
    // The frontend now orders flags before the write, but the emitter must not
    // depend on that: a frontend change is a wrong-flags bug, and this turns
    // it into a decline.
    std::vector<u8> pin_home(block.next_value, 0xFF);
    for (const ir::Inst& in : block.insts) {
        switch (in.op) {
            case ir::Op::GprRead:
            case ir::Op::Const:
            case ir::Op::Add:
            case ir::Op::Sub:
            case ir::Op::And:
            case ir::Op::Or:
            case ir::Op::Xor:
            case ir::Op::Not:
            case ir::Op::Neg:
            case ir::Op::Lea:
                break;
            case ir::Op::SegBase: {
                // The base is a 64-bit CpuState field; a 32-bit guest's
                // segment base fits in its low half, and the low half is what
                // a W-load reads on little-endian. Refuse an offset the
                // scaled immediate cannot reach rather than emit a wrong one.
                const u32 off = seg_base_offset(
                    static_cast<SegReg>(in.operands[0].payload));
                if (off == 0 || !fits_ldr_w_offset(off)) return false;
                break;
            }
            case ir::Op::Load:
                // Narrow loads are read-only halves of the 8/16-bit tranche.
                // Narrow STORES are not admitted: writing 8 or 16 bits to
                // memory is fine, but the tranche that needs it also needs
                // partial-register writes, and those are a separate slice.
                if (in.mem.width != ir::Type::I32 &&
                    in.mem.width != ir::Type::I8 &&
                    in.mem.width != ir::Type::I16) {
                    return false;
                }
                break;
            case ir::Op::Store:
                if (in.mem.width != ir::Type::I32 &&
                    in.mem.width != ir::Type::I8 &&
                    in.mem.width != ir::Type::I16) {
                    return false;
                }
                break;
            case ir::Op::GprWrite:
                // A narrow write is lowered as a bitfield insert into the
                // pin. Without a pin there is nothing to insert into that the
                // fault handler would also see, so decline rather than
                // read-modify-write CpuState behind its back.
                if (in.operands[0].type == ir::Type::I8 ||
                    in.operands[0].type == ir::Type::I16) {
                    if (pinned_gpr32(ir::architectural_gpr(
                            in.operands[0].type,
                            in.operands[0].reg_index)) == 0xFF) {
                        return false;
                    }
                }
                break;
            case ir::Op::Shl:
            case ir::Op::Shr:
            case ir::Op::Sar:
                // Immediate counts only, 1-31. The frontend never builds a
                // zero-count shift (it is a no-op that defines no flags), and
                // a runtime count would need the CL form's zero branch.
                if (in.type != ir::Type::I32) return false;
                if (in.operand_count != 2 ||
                    in.operands[1].kind != ir::OperandKind::Imm) {
                    return false;
                }
                if (in.operands[1].payload == 0 ||
                    in.operands[1].payload > 31) {
                    return false;
                }
                break;
            case ir::Op::Zext:
            case ir::Op::Sext:
                if (in.type != ir::Type::I32) return false;
                if (in.operands[0].type != ir::Type::I8 &&
                    in.operands[0].type != ir::Type::I16) {
                    return false;
                }
                break;
            case ir::Op::Jcc:
            case ir::Op::Jmp:
            case ir::Op::JmpIndirect:
                break;
            case ir::Op::FlagsIn:
                // Emits nothing: the block's incoming flags live in CpuState,
                // in whatever representation the previous block left. Every
                // consumer materializes what it needs from there — a Jcc via
                // `emit_eval_lazy_condition`, a CF merge via
                // `emit_lazy_cf_to_call_tmp`. It occupies no register, which
                // is why it costs no pool slot below.
                break;
            case ir::Op::DefFlags:
                // `Dead` (the flag-demand pass proved nothing can observe it)
                // and `InCpuStateRecord` are lowerable. `InPstate` and
                // `InRflags` need representations this emitter does not
                // produce.
                if (in.flags.repr != ir::FlagRepr::InCpuStateRecord &&
                    in.flags.repr != ir::FlagRepr::Dead)
                    return false;
                // A CF MERGE edge is lowered by capturing the incoming carry
                // into REG_CALL_TMP first — which is also the survivor
                // register. An instruction needing both would have to park two
                // values in one place, so it declines.
                if (in.operand_count == 3 && survivor_in_insn(in.guest_rip))
                    return false;
                // A dead definition whose overlay clear is still owed needs an
                // emission this backend does not have yet; decline rather than
                // drop the clear.
                if (in.flags.repr == ir::FlagRepr::Dead &&
                    in.flags.keep_overlay)
                    return false;
                if (in.flags.overlay_live || in.flags.keep_overlay)
                    return false;
                break;
            case ir::Op::XmmRead:
            case ir::Op::XmmWrite: {
                // Both ends are a Q-form load/store off CpuState, and the
                // encoding's offset is a 16-scaled uimm12. A guest whose
                // CpuState put the XMM array out of that reach would emit a
                // wrong offset rather than fail, so check it here.
                const u8 idx = in.operands[0].reg_index;
                if (idx >= 8) return false;   // 32-bit blocks only
                if (!fits_ldr_q_offset(xmm_u64_off(idx, 0))) return false;
                break;
            }
            case ir::Op::VXor: case ir::Op::VAnd: case ir::Op::VOr:
            case ir::Op::VInsertLow32:
            case ir::Op::VZext32:
            case ir::Op::VLow32:
                break;
            default:
                return false;  // shifts, extends: not lowered yet
        }

        // Type discipline: this emitter is 32-bit-only. Anything narrower or
        // wider would need a partial-register merge it does not implement.
        // `V128` is admitted because the vector ops above define it, and it
        // gets its own register pool rather than competing for the GPR one.
        if (in.defines_value() && in.type != ir::Type::I32 &&
            in.type != ir::Type::Flags && in.type != ir::Type::V128 &&
            in.type != ir::Type::I8 && in.type != ir::Type::I16) {
            return false;
        }

        for (u32 k = 0; k < in.operand_count; ++k) {
            const ir::Operand& operand = in.operands[k];
            if (operand.kind != ir::OperandKind::Value) continue;
            const ir::ValueId v = operand.as_value();
            if (v >= owner.size()) return false;

            // RIP-locality applies to values that live in REGISTERS, which is
            // what the per-instruction scratch pool reset would strand. A
            // `Flags` value has no register home at all: its home is the lazy
            // record in `CpuState`, which is architectural state and outlives
            // the instruction by design. That is exactly what lets a `cmp` in
            // one guest instruction feed a `jcc` in the next — the shape every
            // real block ends with.
            if (operand.type == ir::Type::Flags) continue;

            // A use of an integer value defined under a different guest
            // instruction would outlive the scratch pool's reset.
            if (owner[v] != in.guest_rip &&
                (operand.type != ir::Type::I32 ||
                 (pin_home[v] >= 16 && !block_pool))) {
                return false;
            }
            // ...or a use of a pin-resident value after that pin was rewritten.
            if (pin_home[v] == 0xFE) {
                m_ir_emit_decline = ir::DeclineReason::BackendRegisterPressure;
                return false;
            }
        }

        // A write to a pinned guest register kills every value still living in
        // that pin. 0xFE is the tombstone; the loop above rejects any later use.
        if (in.op == ir::Op::GprWrite) {
            // The architectural register, not the encoding index: `mov ah,5`
            // is a write to EAX, and tombstoning index 4 would invalidate
            // ESP's pin while leaving stale EAX values live.
            const u8 written = ir::architectural_gpr(in.operands[0].type,
                                                     in.operands[0].reg_index);
            if (pinned_gpr32(written) != 0xFF) {
                for (usize v = 0; v < pin_home.size(); ++v) {
                    if (pin_home[v] == written) pin_home[v] = 0xFE;
                }
                const auto source = in.operands[1].as_value();
                if (in.operands[0].type == ir::Type::I32 &&
                    source < pin_home.size() && pin_home[source] == 0xFF)
                    pin_home[source] = written;
            }
        }

        // A guest access CLOBBERS the scratch pool. `emit_guest_load32`
        // clobbers W3/W4/W8/W9/X16 and X0 (the helper return); the store
        // clobbers X0/W3/W4/X8/W9/W10/X16. W0-W2 are this emitter's whole
        // temporary pool, so a scratch-resident value still needed after a
        // memory op is destroyed.
        //
        // Pins survive: the region path's `emit_helper_blr` spills and reloads
        // them, and the linear path makes no call at all.
        //
        // This is not a hypothetical shape — it is PUSH. `push eax` computes
        // ESP-4, stores through it, and only THEN commits it to ESP, because
        // the SDM requires ESP to be unchanged if the store faults. The
        // address is therefore live across the very access that clobbers it.
        //
        // Rather than decline (which would cost every stack instruction) or
        // reorder (which would break fault atomicity), a value whose defining
        // op is cheap and pure is REMATERIALIZED after the access. Only
        // `Const` and a pin-based `Lea` qualify: both are a couple of
        // instructions and neither reads anything the access can disturb.
        if (in.op == ir::Op::Load || in.op == ir::Op::Store) {
            const usize here = static_cast<usize>(&in - block.insts.data());
            for (usize v = 0; v < owner.size(); ++v) {
                if (owner[v] != in.guest_rip) continue;   // not live here
                if (pin_home[v] != 0xFF) continue;        // lives in a pin
                bool used_after = false;
                for (usize j = here + 1;
                     j < block.insts.size() &&
                     block.insts[j].guest_rip == in.guest_rip && !used_after;
                     ++j) {
                    for (u32 k = 0; k < block.insts[j].operand_count; ++k) {
                        const ir::Operand& o = block.insts[j].operands[k];
                        if (o.kind == ir::OperandKind::Value &&
                            o.as_value() == v) {
                            used_after = true;
                            break;
                        }
                    }
                }
                if (!used_after) continue;
                if (rematerializable(block, v, pin_home)) continue;
                // Not rematerializable, so it must be parked in the survivor
                // register. The pre-pass already proved at most one per guest
                // instruction claims it.
                if (!is_survivor[v]) {
                    m_ir_emit_decline =
                        ir::DeclineReason::BackendRegisterPressure;
                    return false;
                }
            }
        }

        // Ownership is recorded for EVERY value-defining op, not just the
        // integer ones. The RIP-locality check below reads `owner`, so a value
        // with no entry looks like it came from another guest instruction and
        // the block declines — which is exactly what V128 values did until
        // this stopped being an I32-only assignment.
        if (in.defines_value() && (in.type == ir::Type::V128 ||
                                   in.type == ir::Type::I8 ||
                                   in.type == ir::Type::I16)) {
            if (in.result >= owner.size()) return false;
            owner[in.result] = in.guest_rip;
        }

        if (in.defines_value() && in.type == ir::Type::I32) {
            if (in.result >= owner.size()) return false;
            owner[in.result] = in.guest_rip;
            // A `GprRead` of a pinned register costs no scratch: the value
            // already lives in its pin.
            const bool free_in_pin =
                in.op == ir::Op::GprRead &&
                pinned_gpr32(in.operands[0].reg_index) != 0xFF;
            if (free_in_pin) pin_home[in.result] = in.operands[0].reg_index;
        }
    }
    // Pool pressure, measured as LIVE INTERVALS rather than as a count of
    // definitions. `pin_home` above is the version with tombstoning, but the
    // pre-pass copy is what emission will use, so plan against that to keep
    // the two answers identical.
    {
        if (block_pool) {
            if (!plan_pool_registers(block, 0, block.insts.size(), pre_pin,
                                     is_survivor, nullptr, commit_pin_mask)) {
                m_ir_emit_decline = ir::DeclineReason::BackendRegisterPressure;
                return false;
            }
            return true;
        }
        usize begin = 0;
        while (begin < block.insts.size()) {
            const u64 rip = block.insts[begin].guest_rip;
            usize end = begin;
            while (end < block.insts.size() &&
                   block.insts[end].guest_rip == rip) {
                ++end;
            }
            if (!plan_pool_registers(block, begin, end, pre_pin, is_survivor,
                                     nullptr)) {
                m_ir_emit_decline =
                    ir::DeclineReason::BackendRegisterPressure;
                return false;
            }
            begin = end;
        }
    }
    return true;
}

}  // namespace vitreo::cpu::jit
