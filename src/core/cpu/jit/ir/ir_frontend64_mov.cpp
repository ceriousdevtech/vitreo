// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_mov.cpp
/// @brief Long-mode data movement: MOVZX/MOVSX/MOVSXD, CMOVcc, SETcc, LEA,
///        moffs MOV and narrow MOV
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_widen(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // MOVZX/MOVSX/MOVSXD. The destination is a WHOLE 32- or 64-bit register,
    // so there is no partial-register merge, and these define no flags.
    // Intel SDM Vol. 2B: a 32-bit destination zero-extends to 64.
    struct Widening { bool sext; Type from; Type to; };
    Widening widen{false, Type::I8, Type::I8};
    bool is_widen = true;
    switch (insn.opcode) {
    case Opcode::MOVZX_R32_RM8:  widen = {false, Type::I8,  Type::I32}; break;
    case Opcode::MOVZX_R64_RM8:  widen = {false, Type::I8,  Type::I64}; break;
    case Opcode::MOVZX_R32_RM16: widen = {false, Type::I16, Type::I32}; break;
    case Opcode::MOVZX_R64_RM16: widen = {false, Type::I16, Type::I64}; break;
    case Opcode::MOVSX_R32_RM8:  widen = {true,  Type::I8,  Type::I32}; break;
    case Opcode::MOVSX_R64_RM8:  widen = {true,  Type::I8,  Type::I64}; break;
    case Opcode::MOVSX_R32_RM16: widen = {true,  Type::I16, Type::I32}; break;
    case Opcode::MOVSX_R64_RM16: widen = {true,  Type::I16, Type::I64}; break;
    case Opcode::MOVSXD_R64_RM32: widen = {true, Type::I32, Type::I64}; break;
    case Opcode::MOVZX_R16_RM8:  widen = {false, Type::I8,  Type::I16}; break;
    case Opcode::MOVSX_R16_RM8:  widen = {true,  Type::I8,  Type::I16}; break;
    default: is_widen = false; break;
    }
    if (is_widen && !px.lock && !px.rep && !px.repne && !px.vex && !px.evex &&
        insn.operand_count == 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        // A 16-bit destination is a PARTIAL write: `movzx ax, bl` leaves
        // EAX[31:16] alone. The 32- and 64-bit destinations are whole
        // registers and need no merge.
        const OperandType dst_type = widen.to == Type::I64 ? OperandType::Reg64
                                   : widen.to == Type::I16 ? OperandType::Reg16
                                                           : OperandType::Reg32;
        const OperandType src_type = widen.from == Type::I8  ? OperandType::Reg8
                                   : widen.from == Type::I16 ? OperandType::Reg16
                                                             : OperandType::Reg32;
        const OperandType mem_type = widen.from == Type::I8  ? OperandType::Mem8
                                   : widen.from == Type::I16 ? OperandType::Mem16
                                                             : OperandType::Mem32;
        if (dst.type == dst_type && dst.reg < 16) {
            ValueId narrow = kInvalidValue;
            if (src.type == src_type && src.reg < 16) {
                // `narrow_reg_index` resolves the REX spelling: with a REX
                // prefix a Reg8 index of 4-7 is SPL/BPL/SIL/DIL, without one
                // it is AH/CH/DH/BH and moves into the high-byte space.
                narrow = builder.gpr_read(
                    GuestReg{narrow_reg_index(insn, src), widen.from});
            } else if (src.type == mem_type) {
                const ValueId address = build_long_mode_ea(builder, src.mem,
                                                           next_rip, px.address_size);
                if (address != kInvalidValue)
                    narrow = builder.load(widen.from, address, kMemFaultState);
            }
            if (narrow != kInvalidValue) {
                const ValueId wide = widen.sext ? builder.sext(widen.to, narrow)
                                                : builder.zext(widen.to, narrow);
                builder.gpr_write(GuestReg{dst.reg, widen.to}, wide);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_acc_sign_extend(
    IrBuilder& builder, const DecodedInstruction& insn) {
    // Intel SDM Vol. 2A CBW/CWDE/CDQE and CWD/CDQ/CQO: the operand size
    // (66 / none / REX.W) already selected the opcode. No flags are affected.
    // CBW and CWD write a 16-bit register and so MERGE; CWDE and CDQ write a
    // 32-bit register and so zero bits 63:32; CWD/CDQ/CQO leave rAX alone.
    Type from = Type::Void;
    bool into_rdx = false;
    switch (insn.opcode) {
    case Opcode::CBW:  from = Type::I8;  break;
    case Opcode::CWDE: from = Type::I16; break;
    case Opcode::CDQE: from = Type::I32; break;
    case Opcode::CWD:  from = Type::I16; into_rdx = true; break;
    case Opcode::CDQ:  from = Type::I32; into_rdx = true; break;
    case Opcode::CQO:  from = Type::I64; into_rdx = true; break;
    default: return std::nullopt;
    }
    const auto& px = insn.prefixes;
    if (px.lock || px.rep || px.repne || px.vex || px.evex)
        return FamilyVerdict{false, DeclineReason::UnsupportedPrefix};
    const ValueId acc = builder.gpr_read(GuestReg{0, from});
    if (!into_rdx) {
        const Type to = from == Type::I8 ? Type::I16
                      : from == Type::I16 ? Type::I32 : Type::I64;
        builder.gpr_write(GuestReg{0, to}, builder.sext(to, acc));
        return FamilyVerdict{true, DeclineReason::None};
    }
    const ValueId wide = from == Type::I64 ? acc : builder.sext(Type::I64, acc);
    const ValueId sign = builder.shift_imm(Op::Sar, Type::I64, wide, 63);
    builder.gpr_write(GuestReg{2, from},
                      from == Type::I64 ? sign : builder.trunc(from, sign));
    return FamilyVerdict{true, DeclineReason::None};
}

FamilyVerdict try_long_mode_cmov(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // CMOVcc. The opcodes run in condition order, three widths per condition.
    //
    // Both arms are always evaluated, which is the architectural requirement,
    // not a shortcut: Intel SDM Vol. 2A says a memory source is READ whether
    // or not the condition holds, so the load must not be made conditional.
    // A 32-bit destination also zeroes bits 63:32 either way, which falls out
    // of writing the selected value through a full-width `GprWrite`.
    {
        const int cmov_base = static_cast<int>(Opcode::CMOVO_R16_RM16);
        const int value = static_cast<int>(insn.opcode);
        static_assert(static_cast<int>(Opcode::CMOVO_R32_RM32) ==
                      static_cast<int>(Opcode::CMOVO_R16_RM16) + 1);
        static_assert(static_cast<int>(Opcode::CMOVO_R64_RM64) ==
                      static_cast<int>(Opcode::CMOVO_R16_RM16) + 2);
        if (value >= cmov_base && value < cmov_base + 48 && !px.lock &&
            !px.rep && !px.repne && !px.vex && !px.evex &&
            insn.operand_count == 2) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const int offset = value - cmov_base;
            const u8 cond = static_cast<u8>(offset / 3);
            const Type w = offset % 3 == 0 ? Type::I16
                         : offset % 3 == 1 ? Type::I32 : Type::I64;
            const auto& dst = insn.operands[0];
            const auto& src = insn.operands[1];
            const OperandType reg_type = w == Type::I16 ? OperandType::Reg16
                                       : w == Type::I32 ? OperandType::Reg32
                                                        : OperandType::Reg64;
            const OperandType mem_type = w == Type::I16 ? OperandType::Mem16
                                       : w == Type::I32 ? OperandType::Mem32
                                                        : OperandType::Mem64;
            if (dst.type == reg_type && dst.reg < 16) {
                ValueId source = kInvalidValue;
                if (src.type == reg_type && src.reg < 16) {
                    source = builder.gpr_read(GuestReg{src.reg, w});
                } else if (src.type == mem_type) {
                    const ValueId address = build_long_mode_ea(
                        builder, src.mem, next_rip, px.address_size);
                    if (address != kInvalidValue)
                        source = builder.load(w, address, kMemFaultState);
                }
                if (source != kInvalidValue) {
                    const GuestReg reg{dst.reg, w};
                    const ValueId original = builder.gpr_read(reg);
                    const ValueId taken =
                        builder.eval_cc(require_flags(builder, live_flags), cond);
                    builder.gpr_write(reg,
                        builder.select(w, taken, source, original));
                    return {true, DeclineReason::None};
                }
            }
            return verdict;
        }
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_setcc(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // SETcc. The 16 opcodes are contiguous in condition order, exactly as the
    // Jcc families are, so the condition is the distance from SETO.
    //
    // The destination is a BYTE, so a register form is a partial write, and
    // SETcc defines no flags at all -- it only reads them.
    {
        const int set_base = static_cast<int>(Opcode::SETO_RM8);
        const int value = static_cast<int>(insn.opcode);
        static_assert(static_cast<int>(Opcode::SETNLE_RM8) ==
                      static_cast<int>(Opcode::SETO_RM8) + 15);
        if (value >= set_base && value <= set_base + 15 && !px.lock &&
            !px.rep && !px.repne && !px.vex && !px.evex &&
            insn.operand_count == 1) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const u8 cond = static_cast<u8>(value - set_base);
            const ValueId bit =
                builder.eval_cc(require_flags(builder, live_flags), cond);
            if (dst.type == OperandType::Reg8 && dst.reg < 16) {
                builder.gpr_write(GuestReg{narrow_reg_index(insn, dst), Type::I8},
                                  bit);
                return {true, DeclineReason::None};
            }
            if (dst.type == OperandType::Mem8) {
                const ValueId address = build_long_mode_ea(builder, dst.mem,
                                                           next_rip, px.address_size);
                if (address != kInvalidValue) {
                    builder.store(Type::I8, address, bit, kMemFaultState);
                    return {true, DeclineReason::None};
                }
            }
            return verdict;
        }
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_lea(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // LEA at all three long-mode widths. It performs NO access -- the address
    // is the result -- so it carries no fault state and touches no flag.
    // The operand-size prefix selects how much of the address is kept, which
    // is the whole difference between the three forms: 64 keeps it all, 32
    // truncates and zero-extends, 16 truncates and MERGES.
    Type lea_width = Type::Void;
    switch (insn.opcode) {
    case Opcode::LEA_R16_M: lea_width = Type::I16; break;
    case Opcode::LEA_R32_M: lea_width = Type::I32; break;
    case Opcode::LEA_R64_M: lea_width = Type::I64; break;
    default: break;
    }
    if (lea_width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const OperandType reg_type = lea_width == Type::I16 ? OperandType::Reg16
                                   : lea_width == Type::I32 ? OperandType::Reg32
                                                            : OperandType::Reg64;
        if (dst.type == reg_type && dst.reg < 16 && is_memory_operand(src.type)) {
            const ValueId address =
                build_long_mode_ea(builder, src.mem, next_rip, px.address_size);
            if (address != kInvalidValue) {
                builder.gpr_write(GuestReg{static_cast<u8>(dst.reg), lea_width},
                                  lea_width == Type::I64
                                      ? address
                                      : builder.trunc(lea_width, address));
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_moffs(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // moffs: MOV between the accumulator and an absolute address. The shared
    // effective-address builder already resolves `has_absolute`.
    Type moffs_width = Type::Void;
    bool moffs_store = false;
    switch (insn.opcode) {
    case Opcode::MOV_AL_MOFFS8:   moffs_width = Type::I8;  break;
    case Opcode::MOV_MOFFS8_AL:   moffs_width = Type::I8;  moffs_store = true; break;
    case Opcode::MOV_AX_MOFFS16:  moffs_width = Type::I16; break;
    case Opcode::MOV_MOFFS16_AX:  moffs_width = Type::I16; moffs_store = true; break;
    default: break;
    }
    if (moffs_width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count == 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& mem = moffs_store ? insn.operands[0] : insn.operands[1];
        const OperandType mem_type = moffs_width == Type::I8 ? OperandType::Mem8
                                                             : OperandType::Mem16;
        if (mem.type == mem_type) {
            const ValueId address = build_long_mode_ea(builder, mem.mem, next_rip,
                                                       px.address_size);
            if (address != kInvalidValue) {
                // The accumulator, always: AL for the byte forms, AX for word.
                const GuestReg accumulator{0, moffs_width};
                if (moffs_store) {
                    builder.store(moffs_width, address,
                                  builder.gpr_read(accumulator), kMemFaultState);
                } else {
                    builder.gpr_write(accumulator,
                                      builder.load(moffs_width, address, kMemFaultState));
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_mov_narrow(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Narrow MOV. A register destination is a PARTIAL write: the emitter
    // merges the field and leaves the rest of the architectural register
    // alone (Intel SDM Vol. 1 3.4.1.1 — only a 32-bit write zero-extends).
    Type narrow_width = Type::Void;
    switch (insn.opcode) {
    case Opcode::MOV_RM8_R8: case Opcode::MOV_R8_RM8:
    case Opcode::MOV_R8_IMM8: case Opcode::MOV_RM8_IMM8:
        narrow_width = Type::I8; break;
    case Opcode::MOV_RM16_R16: case Opcode::MOV_R16_RM16:
    case Opcode::MOV_R16_IMM16: case Opcode::MOV_RM16_IMM16:
        narrow_width = Type::I16; break;
    default: break;
    }
    if (narrow_width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count == 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const OperandType reg_type = narrow_width == Type::I8
            ? OperandType::Reg8 : OperandType::Reg16;
        const OperandType mem_type = narrow_width == Type::I8
            ? OperandType::Mem8 : OperandType::Mem16;
        ValueId value = kInvalidValue;
        if (src.type == reg_type && src.reg < 16)
            value = builder.gpr_read(GuestReg{narrow_reg_index(insn, src), narrow_width});
        else if (is_immediate_operand(src.type))
            value = builder.constant(narrow_width, narrow_width == Type::I8
                ? static_cast<u64>(static_cast<u8>(src.imm))
                : static_cast<u64>(static_cast<u16>(src.imm)));
        else if (src.type == mem_type) {
            const ValueId address = build_long_mode_ea(builder, src.mem, next_rip,
                                                       px.address_size);
            if (address != kInvalidValue)
                value = builder.load(narrow_width, address, kMemFaultState);
        }
        if (value != kInvalidValue) {
            if (dst.type == reg_type && dst.reg < 16) {
                builder.gpr_write(GuestReg{narrow_reg_index(insn, dst), narrow_width},
                                  value);
                return {true, DeclineReason::None};
            }
            if (dst.type == mem_type) {
                const ValueId address = build_long_mode_ea(builder, dst.mem, next_rip,
                                                           px.address_size);
                if (address != kInvalidValue) {
                    builder.store(narrow_width, address, value, kMemFaultState);
                    return {true, DeclineReason::None};
                }
            }
        }
        return verdict;
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir
