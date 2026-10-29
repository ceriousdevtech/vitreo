// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Logical instruction decoding (AND, OR, NOT)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_and(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // AND AL, imm8 (24)
        case 0x24: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::AND_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // AND eAX, imm16/32 (25)
        case 0x25: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::AND_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::AND_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::AND_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // AND r/m8, r8 (20)
        case 0x20: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::AND_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // AND r/m16/32/64, r16/32/64 (21)
        case 0x21: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::AND_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::AND_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::AND_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // AND r8, r/m8 (22)
        case 0x22: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::AND_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // AND r16/32/64, r/m16/32/64 (23)
        case 0x23: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::AND_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::AND_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::AND_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_or(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // OR AL, imm8 (0C)
        case 0x0C: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::OR_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // OR eAX, imm16/32 (0D)
        case 0x0D: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::OR_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::OR_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::OR_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // OR r/m8, r8 (08)
        case 0x08: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::OR_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // OR r/m16/32/64, r16/32/64 (09)
        case 0x09: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::OR_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::OR_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::OR_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // OR r8, r/m8 (0A)
        case 0x0A: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::OR_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // OR r16/32/64, r/m16/32/64 (0B)
        case 0x0B: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::OR_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::OR_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::OR_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_group3(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    // Read ModR/M byte to get the reg field which encodes the operation
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    
    u8 reg_field = (modrm >> 3) & 0x07;  // Bits 5:3 select operation
    
    Operand rm;
    u8 unused_reg;
    bool is_8bit = (byte == 0xF6);
    
    if (is_8bit) {
        if (decode_modrm(reader, mode, prefix, false, modrm, rm, unused_reg) < 0) {
            return DecodeError::InvalidModRM;
        }
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
    } else {
        if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, unused_reg) < 0) {
            return DecodeError::InvalidModRM;
        }
    }
    
    switch (reg_field) {
        case 0: // TEST r/m, imm
        case 1: // TEST r/m, imm (undocumented alias)
            if (is_8bit) {
                u8 imm;
                if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_RM8_IMM8;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            } else if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_RM64_IMM32;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_RM16_IMM16;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_RM32_IMM32;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
            
        case 2: // NOT r/m
            if (is_8bit) {
                out.opcode = Opcode::NOT_RM8;
            } else if (is_64bit_operand) {
                out.opcode = Opcode::NOT_RM64;
            } else if (op_size == 2) {
                out.opcode = Opcode::NOT_RM16;
            } else {
                out.opcode = Opcode::NOT_RM32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
            
        case 3: // NEG r/m
            if (is_8bit) {
                out.opcode = Opcode::NEG_RM8;
            } else if (is_64bit_operand) {
                out.opcode = Opcode::NEG_RM64;
            } else if (op_size == 2) {
                out.opcode = Opcode::NEG_RM16;
            } else {
                out.opcode = Opcode::NEG_RM32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
            
        case 4: // MUL r/m
            if (is_8bit) {
                out.opcode = Opcode::MUL_RM8;
            } else if (is_64bit_operand) {
                out.opcode = Opcode::MUL_RM64;
            } else if (op_size == 2) {
                out.opcode = Opcode::MUL_RM16;
            } else {
                out.opcode = Opcode::MUL_RM32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
            
        case 5: // IMUL r/m
            if (is_8bit) {
                out.opcode = Opcode::IMUL_RM8;
            } else if (is_64bit_operand) {
                out.opcode = Opcode::IMUL_RM64;
            } else if (op_size == 2) {
                out.opcode = Opcode::IMUL_RM16;
            } else {
                out.opcode = Opcode::IMUL_RM32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
            
        case 6: // DIV r/m
            if (is_8bit) {
                out.opcode = Opcode::DIV_RM8;
            } else if (is_64bit_operand) {
                out.opcode = Opcode::DIV_RM64;
            } else if (op_size == 2) {
                out.opcode = Opcode::DIV_RM16;
            } else {
                out.opcode = Opcode::DIV_RM32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
            
        case 7: // IDIV r/m
            if (is_8bit) {
                out.opcode = Opcode::IDIV_RM8;
            } else if (is_64bit_operand) {
                out.opcode = Opcode::IDIV_RM64;
            } else if (op_size == 2) {
                out.opcode = Opcode::IDIV_RM16;
            } else {
                out.opcode = Opcode::IDIV_RM32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
            
        default:
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
