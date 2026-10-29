// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_mul_div.cpp
/// @brief Long-mode DIV/IDIV and MUL/IMUL at the narrow widths, and the
///        two- and three-operand IMUL forms
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_divide(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // DIV / IDIV at every width.
    //
    // One op with a helper lowering, not arithmetic the frontend assembles.
    // A zero divisor, or a quotient that does not fit the destination, raises
    // #DE and must commit NOTHING -- a conditional fault with a conditional
    // commit, which the IR has no in-instruction control flow to express any
    // other way. `Op::Divide` makes the decision and the two register writes
    // indivisible, and the emitted code branches on its status.
    //
    // Flags are UNDEFINED after both (Intel SDM Vol. 2 DIV/IDIV), so no record
    // is published and any pending one survives -- which is what the reference
    // does too.
    struct DivForm { ir::DivideKind kind; Type width; };
    DivForm divf{ir::DivideKind::Div8, Type::Void};
    switch (insn.opcode) {
    case Opcode::DIV_RM8:   divf = {ir::DivideKind::Div8,   Type::I8};  break;
    case Opcode::IDIV_RM8:  divf = {ir::DivideKind::Idiv8,  Type::I8};  break;
    case Opcode::DIV_RM16:  divf = {ir::DivideKind::Div16,  Type::I16}; break;
    case Opcode::IDIV_RM16: divf = {ir::DivideKind::Idiv16, Type::I16}; break;
    case Opcode::DIV_RM32:  divf = {ir::DivideKind::Div32,  Type::I32}; break;
    case Opcode::IDIV_RM32: divf = {ir::DivideKind::Idiv32, Type::I32}; break;
    case Opcode::DIV_RM64:  divf = {ir::DivideKind::Div64,  Type::I64}; break;
    case Opcode::IDIV_RM64: divf = {ir::DivideKind::Idiv64, Type::I64}; break;
    default: break;
    }
    if (divf.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& src = insn.operands[0];
        const Type w = divf.width;
        const OperandType reg_type = w == Type::I8  ? OperandType::Reg8
                                   : w == Type::I16 ? OperandType::Reg16
                                   : w == Type::I32 ? OperandType::Reg32
                                                    : OperandType::Reg64;
        const OperandType mem_type = w == Type::I8  ? OperandType::Mem8
                                   : w == Type::I16 ? OperandType::Mem16
                                   : w == Type::I32 ? OperandType::Mem32
                                                    : OperandType::Mem64;
        ValueId divisor = kInvalidValue;
        if (src.type == reg_type && src.reg < 16) {
            divisor = builder.gpr_read(GuestReg{narrow_reg_index(insn, src), w});
        } else if (src.type == mem_type) {
            const ValueId address =
                build_long_mode_ea(builder, src.mem, next_rip, px.address_size);
            if (address != kInvalidValue)
                divisor = builder.load(w, address, kMemFaultState);
        }
        if (divisor != kInvalidValue) {
            builder.divide(divisor, static_cast<u8>(divf.kind), kMemFaultState);
            return {true, DeclineReason::None};
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_mul_accumulator(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // MUL / IMUL, one-operand: the forms that write the IMPLICIT accumulator
    // pair. At 8 bits the whole product lands in AX; at 16 it is split DX:AX.
    // Like the other multiply forms they define CF and OF only, and both mean
    // the same thing -- "the high half is not just an extension of the low
    // one" -- so they go through the overlay rather than a record.
    //
    // Only the 8- and 16-bit widths: at 32 and 64 the product needs the high
    // half of a 64x64 (or 128-bit) multiply into a SECOND implicit register,
    // and `Mul32`'s record convention already owns that path in the stencil.
    struct WideMul { bool sign; Type width; };
    WideMul acc{false, Type::Void};
    switch (insn.opcode) {
    case Opcode::MUL_RM8:   acc = {false, Type::I8};  break;
    case Opcode::MUL_RM16:  acc = {false, Type::I16}; break;
    case Opcode::IMUL_RM8:  acc = {true,  Type::I8};  break;
    case Opcode::IMUL_RM16: acc = {true,  Type::I16}; break;
    default: break;
    }
    if (acc.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& src = insn.operands[0];
        const Type w = acc.width;
        const u32 bits = static_cast<u32>(type_bits(w));
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                                   : OperandType::Reg16;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                                   : OperandType::Mem16;
        const bool from_memory = src.type == mem_type;
        if ((src.type == reg_type && src.reg < 16) || from_memory) {
            ValueId factor = kInvalidValue;
            if (from_memory) {
                const ValueId address =
                    build_long_mode_ea(builder, src.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    factor = builder.load(w, address, kMemFaultState);
            } else {
                factor = builder.gpr_read(GuestReg{narrow_reg_index(insn, src), w});
            }
            if (factor != kInvalidValue) {
                const auto widen = [&](ValueId v) {
                    return acc.sign ? builder.sext(Type::I64, v)
                                    : builder.zext(Type::I64, v);
                };
                // AL for the byte form, AX for the word form.
                const ValueId accumulator = builder.gpr_read(GuestReg{0, w});
                const ValueId product = builder.binary(Op::Mul, Type::I64,
                    widen(accumulator), widen(factor));
                // The high half. Unsigned asks whether it is nonzero; signed
                // asks whether it is the low half's sign extension.
                const ValueId high =
                    builder.shift_imm(Op::Shr, Type::I64, product,
                                      static_cast<u8>(bits));
                const ValueId differs = acc.sign
                    ? builder.binary(Op::Xor, Type::I64, product,
                                     builder.sext(Type::I64,
                                                  builder.trunc(w, product)))
                    : high;
                const ValueId flag = builder.select(Type::I64, differs,
                                                    builder.constant(Type::I64, 1),
                                                    builder.constant(Type::I64, 0));
                if (w == Type::I8) {
                    // The byte form's whole product is AX; there is no DX half.
                    builder.gpr_write(GuestReg{0, Type::I16},
                                      builder.trunc(Type::I16, product));
                } else {
                    builder.gpr_write(GuestReg{0, Type::I16},
                                      builder.trunc(Type::I16, product));
                    builder.gpr_write(GuestReg{2, Type::I16},
                                      builder.trunc(Type::I16, high));
                }
                builder.set_flag_overlay(Op::SetCf, flag, Type::I64);
                builder.set_flag_overlay(Op::SetOf, flag, Type::I64);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_imul(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // IMUL, two- and three-operand forms. Not the one-operand form: that one
    // writes the implicit DX:AX pair, which is a different instruction wearing
    // the same mnemonic.
    //
    // These write ONLY CF and OF -- both set when the product does not fit the
    // destination -- and leave ZF/SF/PF/AF untouched (Intel SDM Vol. 2 IMUL;
    // `exec_imul` agrees). So they are partial flag writers and use the
    // overlay, NOT the `Imul16`/`Imul32` records: a record is the whole flag
    // state, and publishing one here would discard a pending record that a
    // preceding instruction still owed ZF/SF/PF for.
    //
    // "Does it fit" is asked by computing the full product and comparing it
    // with its own truncation sign-extended back. Below 64 bits the full
    // product fits an I64 multiply of sign-extended halves; at 64 it does not,
    // which is what `SMulH` is for.
    struct MulForm { Type width; bool from_imm; };
    MulForm mulf{Type::Void, false};
    switch (insn.opcode) {
    case Opcode::IMUL_R16_RM16: mulf = {Type::I16, false}; break;
    case Opcode::IMUL_R32_RM32: mulf = {Type::I32, false}; break;
    case Opcode::IMUL_R64_RM64: mulf = {Type::I64, false}; break;
    case Opcode::IMUL_R16_RM16_IMM8:
    case Opcode::IMUL_R16_RM16_IMM16: mulf = {Type::I16, true}; break;
    case Opcode::IMUL_R32_RM32_IMM8:
    case Opcode::IMUL_R32_RM32_IMM32: mulf = {Type::I32, true}; break;
    case Opcode::IMUL_R64_RM64_IMM8:
    case Opcode::IMUL_R64_RM64_IMM32: mulf = {Type::I64, true}; break;
    default: break;
    }
    if (mulf.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex &&
        insn.operand_count >= (mulf.from_imm ? 3u : 2u)) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const Type w = mulf.width;
        const u32 bits = static_cast<u32>(type_bits(w));
        const OperandType reg_type = w == Type::I16 ? OperandType::Reg16
                                   : w == Type::I32 ? OperandType::Reg32
                                                    : OperandType::Reg64;
        const OperandType mem_type = w == Type::I16 ? OperandType::Mem16
                                   : w == Type::I32 ? OperandType::Mem32
                                                    : OperandType::Mem64;
        const bool from_memory = src.type == mem_type;
        if (dst.type == reg_type && dst.reg < 16 &&
            ((src.type == reg_type && src.reg < 16) || from_memory)) {
            const GuestReg reg{static_cast<u8>(dst.reg), w};
            ValueId factor = kInvalidValue;
            if (from_memory) {
                const ValueId address =
                    build_long_mode_ea(builder, src.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    factor = builder.load(w, address, kMemFaultState);
            } else {
                factor = builder.gpr_read(GuestReg{static_cast<u8>(src.reg), w});
            }
            if (factor != kInvalidValue) {
                // Both factors as SIGNED 64-bit values. A narrow operand is
                // sign-extended because IMUL is a signed multiply: zero-
                // extending it would give the right low half and the wrong
                // answer to the overflow question.
                const auto widen = [&](ValueId v) {
                    return w == Type::I64 ? v : builder.sext(Type::I64, v);
                };
                const ValueId a = widen(factor);
                const ValueId b = mulf.from_imm
                    ? builder.constant(Type::I64,
                                       static_cast<u64>(insn.operands[2].imm))
                    : widen(builder.gpr_read(reg));
                const ValueId low = builder.binary(Op::Mul, Type::I64, a, b);
                ValueId overflow_bits = kInvalidValue;
                ValueId written = kInvalidValue;
                if (w == Type::I64) {
                    // The product is 128 bits. It fits when its high half is
                    // exactly the sign of its low half.
                    written = low;
                    overflow_bits = builder.binary(Op::Xor, Type::I64,
                        builder.smulh(a, b),
                        builder.shift_imm(Op::Sar, Type::I64, low, 63));
                } else {
                    const ValueId narrow = builder.trunc(w, low);
                    written = narrow;
                    overflow_bits = builder.binary(Op::Xor, Type::I64, low,
                                                   builder.sext(Type::I64, narrow));
                }
                (void)bits;
                // Any difference at all means it did not fit. `Select` turns
                // "nonzero" into the single bit the overlay writes.
                const ValueId flag = builder.select(Type::I64, overflow_bits,
                                                    builder.constant(Type::I64, 1),
                                                    builder.constant(Type::I64, 0));
                builder.gpr_write(reg, written);
                builder.set_flag_overlay(Op::SetCf, flag, Type::I64);
                builder.set_flag_overlay(Op::SetOf, flag, Type::I64);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir
