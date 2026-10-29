// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_stack.cpp
/// @brief Long-mode stack and control transfer: PUSH/POP, CALL, RET,
///        indirect JMP/CALL, ENTER, PUSH FS/GS, PUSHF/POPF and LEAVE
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_stack(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Long-mode stack: PUSH, POP, CALL rel32 and LEAVE (M12).
    //
    // The fault ordering is the contract here, and it is not uniform across
    // the family. What a fault commits is Windows' (tools/oracle_probe/
    // stack_access_fault_probe.expected), and the same rules hold at 64:
    //
    //   PUSH  writes the slot FIRST and commits RSP only if the store
    //         retired, so a faulting push leaves RSP untouched. The source is
    //         read before the decrement, which is what makes `push rsp` store
    //         the value RSP held on entry without a special case.
    //   POP   commits RSP BEFORE writing the destination, so `pop rsp` ends
    //         up holding the popped value rather than the increment.
    //   LEAVE is ONE instruction: RSP <- RBP and the pop commit together,
    //         so a faulting pop leaves RSP and RBP as they were (`leave`
    //         ctxsp=+0 in the probe; an earlier reading, measured against
    //         the interpreter rather than hardware, had RSP already changed).
    //
    // Slot width follows the OPERAND size, not the mode: a 66h push moves RSP
    // by two and writes exactly two bytes (Intel SDM Vol. 2 PUSH). Promoting
    // it to the 64-bit default slot drifts RSP by six per instruction.
    {
        const GuestReg rsp{kEspIndex, Type::I64};   // index 4 is RSP at every width
        const auto slot_lea = [&](ValueId base, i64 delta) {
            return builder.lea(Type::I64, base, kInvalidValue, 1, delta);
        };

        u8 push_slot = 0;          // 0 = not a push
        bool push_narrow = false;
        switch (insn.opcode) {
        case Opcode::PUSH_R64: case Opcode::PUSH_RM64:
        case Opcode::PUSH_IMM8: case Opcode::PUSH_IMM32:
            push_slot = 8; break;
        case Opcode::PUSH_R16: case Opcode::PUSH_RM16: case Opcode::PUSH_IMM16:
            push_slot = 2; push_narrow = true; break;
        default: break;
        }
        if (push_slot != 0 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 1) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& src = insn.operands[0];
            const Type w = push_narrow ? Type::I16 : Type::I64;
            const OperandType reg_type = push_narrow ? OperandType::Reg16
                                                     : OperandType::Reg64;
            const OperandType mem_type = push_narrow ? OperandType::Mem16
                                                     : OperandType::Mem64;
            ValueId value = kInvalidValue;
            if (src.type == reg_type && src.reg < 16) {
                value = builder.gpr_read(GuestReg{static_cast<u8>(src.reg), w});
            } else if (is_immediate_operand(src.type)) {
                // imm8 and imm32 sign-extend to the slot; a 66h immediate
                // keeps only its low sixteen bits.
                value = builder.constant(w, static_cast<u64>(src.imm));
            } else if (src.type == mem_type) {
                const ValueId address =
                    build_long_mode_ea(builder, src.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    value = builder.load(w, address, kMemFaultState);
            }
            if (value != kInvalidValue) {
                const ValueId slot =
                    slot_lea(builder.gpr_read(rsp), -static_cast<i64>(push_slot));
                builder.store(w, slot, value, kMemFaultState);
                builder.gpr_write(rsp, slot);
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        u8 pop_slot = 0;
        bool pop_narrow = false;
        switch (insn.opcode) {
        case Opcode::POP_R64: case Opcode::POP_RM64: pop_slot = 8; break;
        case Opcode::POP_R16: case Opcode::POP_RM16:
            pop_slot = 2; pop_narrow = true; break;
        default: break;
        }
        if (pop_slot != 0 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 1) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const Type w = pop_narrow ? Type::I16 : Type::I64;
            const OperandType reg_type = pop_narrow ? OperandType::Reg16
                                                    : OperandType::Reg64;
            // Register destinations only. A memory destination computes its
            // effective address AFTER the increment has been committed, so
            // `pop [rsp+8]` reads a different address than it looks like --
            // a memory contract worth its own measurement.
            if (dst.type == reg_type && dst.reg < 16) {
                const ValueId rsp0 = builder.gpr_read(rsp);
                const ValueId value = builder.load(w, rsp0, kMemFaultState);
                builder.gpr_write(rsp, slot_lea(rsp0, pop_slot));
                // The destination LAST: `pop rsp` must keep the popped value,
                // not the increment written a line above.
                builder.gpr_write(GuestReg{static_cast<u8>(dst.reg), w}, value);
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        if (insn.opcode == Opcode::CALL_REL32 && !px.lock && !px.rep &&
            !px.repne && !px.vex && !px.evex && insn.operand_count >= 1 &&
            (insn.operands[0].type == OperandType::Rel32 ||
             insn.operands[0].type == OperandType::Rel8)) {
            const ValueId slot = slot_lea(builder.gpr_read(rsp), -8);
            builder.store(Type::I64, slot,
                          builder.constant(Type::I64, next_rip), kMemFaultState);
            builder.gpr_write(rsp, slot);
            builder.jmp(next_rip + static_cast<u64>(insn.operands[0].imm));
            builder.set_call_return_site(next_rip);
            return {true, DeclineReason::None};
        }

        // RET near, with and without a stack adjustment. The return address
        // is a FULL 64-bit value -- a 32-bit read would land the guest
        // somewhere plausible-looking and only surface much later.
        if ((insn.opcode == Opcode::RET_NEAR ||
             insn.opcode == Opcode::RET_NEAR_IMM16) &&
            !px.lock && !px.rep && !px.repne && !px.vex && !px.evex) {
            i64 extra = 0;
            if (insn.opcode == Opcode::RET_NEAR_IMM16) {
                if (insn.operand_count < 1 ||
                    !is_immediate_operand(insn.operands[0].type))
                    return {false, DeclineReason::UnsupportedOperand};
                // The adjustment is an UNSIGNED 16-bit count of argument
                // bytes, not a sign-extended immediate.
                extra = static_cast<i64>(static_cast<u64>(insn.operands[0].imm)
                                         & 0xFFFFull);
            }
            const ValueId rsp0 = builder.gpr_read(rsp);
            const ValueId target = builder.load(Type::I64, rsp0, kMemFaultState);
            builder.gpr_write(rsp, slot_lea(rsp0, 8 + extra));
            builder.jmp_indirect(target, /*is_return=*/true);
            return {true, DeclineReason::None};
        }

        // FF /4 and FF /2: indirect JMP and CALL. The target is read FIRST,
        // before anything commits -- for the register form so that
        // `call rsp` branches to the value RSP held on entry, and for the
        // MEMORY form (which is what an import thunk is) because a faulting
        // target load must leave RSP untouched and push nothing.
        const bool indirect_call = insn.opcode == Opcode::CALL_RM64;
        if ((indirect_call || insn.opcode == Opcode::JMP_RM64) &&
            !px.lock && !px.rep && !px.repne && !px.vex && !px.evex &&
            insn.operand_count >= 1) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& tgt = insn.operands[0];
            ValueId target = kInvalidValue;
            if (tgt.type == OperandType::Reg64 && tgt.reg < 16) {
                target = builder.gpr_read(GuestReg{static_cast<u8>(tgt.reg),
                                                   Type::I64});
            } else if (tgt.type == OperandType::Mem64) {
                const ValueId address =
                    build_long_mode_ea(builder, tgt.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    target = builder.load(Type::I64, address, kMemFaultState);
            }
            if (target != kInvalidValue) {
                if (indirect_call) {
                    const ValueId slot = slot_lea(builder.gpr_read(rsp), -8);
                    builder.store(Type::I64, slot,
                                  builder.constant(Type::I64, next_rip),
                                  kMemFaultState);
                    builder.gpr_write(rsp, slot);
                }
                builder.jmp_indirect(target);
                if (indirect_call) builder.set_call_return_site(next_rip);
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        // C8 iw ib: ENTER. Nesting levels 0 and 1 only -- a deeper level
        // copies `level - 1` display pointers in a RUNTIME loop, which is
        // control flow inside an instruction. Level 0 is what every compiler
        // that still emits ENTER uses.
        if (insn.opcode == Opcode::ENTER && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 2 &&
            is_immediate_operand(insn.operands[0].type) &&
            is_immediate_operand(insn.operands[1].type)) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const u64 frame = static_cast<u64>(insn.operands[0].imm) & 0xFFFFull;
            const u8 nesting = static_cast<u8>(insn.operands[1].imm) & 0x1Fu;
            if (nesting <= 1) {
                const GuestReg rbp{5, Type::I64};
                const ValueId rbp0 = builder.gpr_read(rbp);
                const ValueId slot = slot_lea(builder.gpr_read(rsp), -8);
                builder.store(Type::I64, slot, rbp0, kMemFaultState);
                builder.gpr_write(rsp, slot);
                // FrameTemp is RSP right after that push, and it is what RBP
                // ends up holding -- not the post-allocation RSP.
                ValueId frame_temp = slot;
                if (nesting == 1) {
                    const ValueId second = slot_lea(slot, -8);
                    builder.store(Type::I64, second, frame_temp, kMemFaultState);
                    builder.gpr_write(rsp, second);
                    frame_temp = slot;
                    builder.gpr_write(rbp, frame_temp);
                    builder.gpr_write(rsp,
                        slot_lea(second, -static_cast<i64>(frame)));
                } else {
                    builder.gpr_write(rbp, frame_temp);
                    builder.gpr_write(rsp,
                        slot_lea(slot, -static_cast<i64>(frame)));
                }
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        // 0F A0 / 0F A8: PUSH FS and PUSH GS. The slot follows the operand
        // size exactly as every other push does -- eight bytes by default in
        // long mode, two with 66h -- and what is STORED is the 16-bit
        // selector, zero-extended into an 8-byte slot or written as two bytes
        // into a 2-byte one (Intel SDM Vol. 2 PUSH).
        //
        // The matching POPs are NOT here: loading a segment register reloads
        // its descriptor, which is a contract this IR does not model.
        if ((insn.opcode == Opcode::PUSH_FS || insn.opcode == Opcode::PUSH_GS) &&
            !px.lock && !px.rep && !px.repne && !px.vex && !px.evex) {
            const bool narrow = px.operand_size;
            const u8 slot = narrow ? 2 : 8;
            const auto which = insn.opcode == Opcode::PUSH_FS ? SegReg::FS
                                                              : SegReg::GS;
            const ValueId selector =
                builder.seg_selector(static_cast<u8>(which));
            const ValueId value = narrow
                ? builder.trunc(Type::I16, selector) : selector;
            const ValueId at =
                slot_lea(builder.gpr_read(rsp), -static_cast<i64>(slot));
            builder.store(narrow ? Type::I16 : Type::I64, at, value,
                          kMemFaultState);
            builder.gpr_write(rsp, at);
            return {true, DeclineReason::None};
        }

        // 9C / 9D: PUSHF and POPF. In long mode the default slot is EIGHT
        // bytes (PUSHFQ/POPFQ) and 66h selects two, exactly as for the other
        // stack forms -- the operand size picks the slot, not the mode.
        //
        // The flag masks are NOT emitted here. `FlagsImage`/`FlagsInstall`
        // call the same rules the interpreter applies, so the two tiers cannot
        // drift on which bits a PUSHF reads or a POPF may write.
        if ((insn.opcode == Opcode::PUSHF || insn.opcode == Opcode::POPF) &&
            !px.lock && !px.rep && !px.repne && !px.vex && !px.evex) {
            const bool narrow = px.operand_size;
            const u8 slot = narrow ? 2 : 8;
            if (insn.opcode == Opcode::PUSHF) {
                const ValueId image =
                    builder.flags_image(require_flags(builder, live_flags));
                const ValueId value = narrow
                    ? builder.trunc(Type::I16, image) : image;
                const ValueId at =
                    slot_lea(builder.gpr_read(rsp), -static_cast<i64>(slot));
                builder.store(narrow ? Type::I16 : Type::I64, at, value,
                              kMemFaultState);
                builder.gpr_write(rsp, at);
                return {true, DeclineReason::None};
            }
            const ValueId rsp0 = builder.gpr_read(rsp);
            const ValueId popped =
                builder.load(narrow ? Type::I16 : Type::I64, rsp0,
                             kMemFaultState);
            builder.gpr_write(rsp, slot_lea(rsp0, slot));
            builder.flags_install(narrow ? builder.zext(Type::I64, popped)
                                         : popped);
            // Whatever value named the block's flags is dead: POPF replaced
            // the architectural state wholesale, so a later consumer has to
            // read it back rather than reuse a definition from before.
            live_flags = kInvalidValue;
            return {true, DeclineReason::None};
        }

        if (insn.opcode == Opcode::LEAVE && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex) {
            const GuestReg rbp{5, Type::I64};
            const ValueId rbp0 = builder.gpr_read(rbp);
            // Nothing is written before the load retires (see above).
            const ValueId value = builder.load(Type::I64, rbp0, kMemFaultState);
            // Computed while RBP's old value is still readable: the write
            // below overwrites the pin `rbp0` lives in.
            const ValueId restored = slot_lea(rbp0, 8);
            builder.gpr_write(rbp, value);
            builder.gpr_write(rsp, restored);
            return {true, DeclineReason::None};
        }
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir
