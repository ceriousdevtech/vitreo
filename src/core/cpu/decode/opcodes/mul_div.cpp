// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Multiply/Divide instruction decoding (IMUL two/three-operand, sign-extend)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

// ============================================================================
// IMUL two-operand: 0F AF /r  (IMUL r, r/m)
// ============================================================================

DecodeError decode_imul_two_byte(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (is_64bit_operand) {
        out.opcode = Opcode::IMUL_R64_RM64;
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
    } else if (op_size == 2) {
        out.opcode = Opcode::IMUL_R16_RM16;
        out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
    } else {
        out.opcode = Opcode::IMUL_R32_RM32;
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
    }
    out.add_operand(rm);
    return DecodeError::Success;
}

// ============================================================================
// IMUL three-operand: 0x69 /r iw/id and 0x6B /r ib
// ============================================================================

DecodeError decode_imul_three_op(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (byte == 0x6B) {
        // IMUL r, r/m, imm8 (sign-extended)
        i8 imm;
        if (!reader.read_i8(imm)) return DecodeError::UnexpectedEnd;

        if (is_64bit_operand) {
            out.opcode = Opcode::IMUL_R64_RM64_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        } else if (op_size == 2) {
            out.opcode = Opcode::IMUL_R16_RM16_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        } else {
            out.opcode = Opcode::IMUL_R32_RM32_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        }
    } else {
        // byte == 0x69: IMUL r, r/m, imm16/32
        if (is_64bit_operand) {
            i32 imm;
            if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::IMUL_R64_RM64_IMM32;
            out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
        } else if (op_size == 2) {
            i16 imm;
            if (!reader.read_i16(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::IMUL_R16_RM16_IMM16;
            out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
        } else {
            i32 imm;
            if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::IMUL_R32_RM32_IMM32;
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
        }
    }

    return DecodeError::Success;
}

// ============================================================================
// Sign Extension: 0x98 (CBW/CWDE/CDQE) and 0x99 (CWD/CDQ/CQO)
// ============================================================================

DecodeError decode_sign_extend(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;

    if (byte == 0x98) {
        if (is_64bit_operand) {
            out.opcode = Opcode::CDQE;
        } else if (op_size == 2) {
            out.opcode = Opcode::CBW;
        } else {
            out.opcode = Opcode::CWDE;
        }
    } else {
        // byte == 0x99
        if (is_64bit_operand) {
            out.opcode = Opcode::CQO;
        } else if (op_size == 2) {
            out.opcode = Opcode::CWD;
        } else {
            out.opcode = Opcode::CDQ;
        }
    }

    // No operands (implicit registers)
    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
