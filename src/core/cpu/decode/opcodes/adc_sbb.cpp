// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// ADC/SBB/Group4 instruction decoding (CPU-020)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

// ============================================================================
// ADC (opcodes 10-15)
// ============================================================================

DecodeError decode_adc(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    switch (byte) {
        // ADC r/m8, r8 (10)
        case 0x10: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::ADC_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }

        // ADC r/m16/32/64, r16/32/64 (11)
        case 0x11: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::ADC_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::ADC_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::ADC_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }

        // ADC r8, r/m8 (12)
        case 0x12: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::ADC_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // ADC r16/32/64, r/m16/32/64 (13)
        case 0x13: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::ADC_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::ADC_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::ADC_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // ADC AL, imm8 (14)
        case 0x14: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::ADC_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }

        // ADC eAX/rAX, imm16/32 (15)
        case 0x15: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::ADC_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::ADC_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::ADC_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }

        default:
            return DecodeError::InvalidOpcode;
    }
}

// ============================================================================
// SBB (opcodes 18-1D)
// ============================================================================

DecodeError decode_sbb(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    switch (byte) {
        // SBB r/m8, r8 (18)
        case 0x18: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::SBB_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }

        // SBB r/m16/32/64, r16/32/64 (19)
        case 0x19: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::SBB_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SBB_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SBB_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }

        // SBB r8, r/m8 (1A)
        case 0x1A: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::SBB_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // SBB r16/32/64, r/m16/32/64 (1B)
        case 0x1B: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::SBB_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SBB_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SBB_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // SBB AL, imm8 (1C)
        case 0x1C: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::SBB_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }

        // SBB eAX/rAX, imm16/32 (1D)
        case 0x1D: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::SBB_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::SBB_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::SBB_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }

        default:
            return DecodeError::InvalidOpcode;
    }
}

// ============================================================================
// Group 4 (opcode FE) - INC/DEC r/m8
// ============================================================================

DecodeError decode_group4(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    DecodeMode mode = ctx.mode;

    if (byte != 0xFE) {
        return DecodeError::InvalidOpcode;
    }

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    u8 reg_field = (modrm >> 3) & 0x07;

    Operand rm;
    u8 unused_reg;
    if (decode_modrm(reader, mode, prefix, false, modrm, rm, unused_reg) < 0) {
        return DecodeError::InvalidModRM;
    }
    if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
    else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;

    switch (reg_field) {
        case 0: // INC r/m8
            out.opcode = Opcode::INC_RM8;
            out.add_operand(rm);
            return DecodeError::Success;

        case 1: // DEC r/m8
            out.opcode = Opcode::DEC_RM8;
            out.add_operand(rm);
            return DecodeError::Success;

        default:
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
