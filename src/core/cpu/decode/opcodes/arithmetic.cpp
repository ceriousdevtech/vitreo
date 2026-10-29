// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Arithmetic instruction decoding (ADD, SUB, XOR, CMP, TEST)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_add(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // ADD AL, imm8 (04)
        case 0x04: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::ADD_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // ADD eAX, imm16/32 (05)
        case 0x05: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::ADD_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::ADD_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::ADD_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // ADD r/m8, r8 (00)
        case 0x00: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::ADD_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // ADD r/m16/32/64, r16/32/64 (01)
        case 0x01: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::ADD_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::ADD_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::ADD_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // ADD r8, r/m8 (02)
        case 0x02: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::ADD_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // ADD r16/32/64, r/m16/32/64 (03)
        case 0x03: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::ADD_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::ADD_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::ADD_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_sub(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // SUB r/m8, r8 (28)
        case 0x28: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::SUB_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // SUB r8, r/m8 (2A)
        case 0x2A: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::SUB_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // SUB AL, imm8 (2C)
        case 0x2C: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::SUB_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // SUB eAX, imm16/32 (2D)
        case 0x2D: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::SUB_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::SUB_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::SUB_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // SUB r/m16/32/64, r16/32/64 (29)
        case 0x29: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::SUB_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SUB_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SUB_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // SUB r16/32/64, r/m16/32/64 (2B)
        case 0x2B: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::SUB_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SUB_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SUB_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_xor(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // XOR r/m8, r8 (30)
        case 0x30: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            // Adjust for 8-bit operand
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg8;
            } else if (is_memory_operand(rm.type)) {
                rm.type = OperandType::Mem8;
            }
            out.opcode = Opcode::XOR_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }

        // XOR r8, r/m8 (32)
        case 0x32: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            // Adjust for 8-bit operand
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg8;
            } else if (is_memory_operand(rm.type)) {
                rm.type = OperandType::Mem8;
            }
            out.opcode = Opcode::XOR_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // XOR AL, imm8 (34)
        case 0x34: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::XOR_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // XOR eAX, imm16/32 (35)
        case 0x35: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::XOR_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::XOR_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::XOR_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // XOR r/m16/32/64, r16/32/64 (31)
        case 0x31: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::XOR_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::XOR_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::XOR_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // XOR r16/32/64, r/m16/32/64 (33)
        case 0x33: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::XOR_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::XOR_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::XOR_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_cmp(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // CMP r/m8, r8 (38)
        case 0x38: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::CMP_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // CMP r8, r/m8 (3A)
        case 0x3A: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::CMP_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // CMP AL, imm8 (3C)
        case 0x3C: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::CMP_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // CMP eAX, imm16/32 (3D)
        case 0x3D: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::CMP_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::CMP_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::CMP_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // CMP r/m16/32/64, r16/32/64 (39)
        case 0x39: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::CMP_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::CMP_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::CMP_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // CMP r16/32/64, r/m16/32/64 (3B)
        case 0x3B: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::CMP_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::CMP_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::CMP_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_test(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // TEST AL, imm8 (A8)
        case 0xA8: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::TEST_AL_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // TEST eAX, imm16/32 (A9)
        case 0xA9: {
            if (is_64bit_operand) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_RAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_AX_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::TEST_EAX_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // TEST r/m8, r8 (84)
        case 0x84: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::TEST_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // TEST r/m16/32/64, r16/32/64 (85)
        case 0x85: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::TEST_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::TEST_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::TEST_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_group1(DecodeContext& ctx) noexcept {
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
    bool is_8bit = (byte == 0x80);
    
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
    
    // Read immediate
    i64 imm_value;
    OperandType imm_type;
    
    if (byte == 0x80) {
        // r/m8, imm8
        u8 imm;
        if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        imm_value = imm;
        imm_type = OperandType::Imm8;
    } else if (byte == 0x81) {
        // r/m16/32/64, imm16/32
        if (op_size == 2) {
            u16 imm;
            if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
            imm_value = imm;
            imm_type = OperandType::Imm16;
        } else {
            i32 imm;
            if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
            imm_value = imm;
            imm_type = OperandType::Imm32;
        }
    } else if (byte == 0x82) {
        // Same as 0x80 (only valid in 32-bit mode, but we decode it)
        u8 imm;
        if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        imm_value = imm;
        imm_type = OperandType::Imm8;
    } else if (byte == 0x83) {
        // r/m16/32/64, imm8 (sign-extended)
        i8 imm;
        if (!reader.read_i8(imm)) return DecodeError::UnexpectedEnd;
        imm_value = imm;
        imm_type = OperandType::Imm8;
    } else {
        return DecodeError::InvalidOpcode;
    }
    
    // Select opcode based on reg field
    switch (reg_field) {
        case 0: // ADD
            if (is_8bit) {
                out.opcode = Opcode::ADD_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::ADD_RM64_IMM8 : Opcode::ADD_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::ADD_RM16_IMM8 : Opcode::ADD_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::ADD_RM32_IMM8 : Opcode::ADD_RM32_IMM32;
            }
            break;
            
        case 1: // OR
            if (is_8bit) {
                out.opcode = Opcode::OR_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::OR_RM64_IMM8 : Opcode::OR_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::OR_RM16_IMM8 : Opcode::OR_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::OR_RM32_IMM8 : Opcode::OR_RM32_IMM32;
            }
            break;
            
        case 2: // ADC
            if (is_8bit) {
                out.opcode = Opcode::ADC_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::ADC_RM64_IMM8 : Opcode::ADC_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::ADC_RM16_IMM8 : Opcode::ADC_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::ADC_RM32_IMM8 : Opcode::ADC_RM32_IMM32;
            }
            break;
            
        case 3: // SBB
            if (is_8bit) {
                out.opcode = Opcode::SBB_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::SBB_RM64_IMM8 : Opcode::SBB_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::SBB_RM16_IMM8 : Opcode::SBB_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::SBB_RM32_IMM8 : Opcode::SBB_RM32_IMM32;
            }
            break;
            
        case 4: // AND
            if (is_8bit) {
                out.opcode = Opcode::AND_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::AND_RM64_IMM8 : Opcode::AND_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::AND_RM16_IMM8 : Opcode::AND_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::AND_RM32_IMM8 : Opcode::AND_RM32_IMM32;
            }
            break;
            
        case 5: // SUB
            if (is_8bit) {
                out.opcode = Opcode::SUB_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::SUB_RM64_IMM8 : Opcode::SUB_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::SUB_RM16_IMM8 : Opcode::SUB_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::SUB_RM32_IMM8 : Opcode::SUB_RM32_IMM32;
            }
            break;
            
        case 6: // XOR
            if (is_8bit) {
                out.opcode = Opcode::XOR_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::XOR_RM64_IMM8 : Opcode::XOR_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::XOR_RM16_IMM8 : Opcode::XOR_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::XOR_RM32_IMM8 : Opcode::XOR_RM32_IMM32;
            }
            break;
            
        case 7: // CMP
            if (is_8bit) {
                out.opcode = Opcode::CMP_RM8_IMM8;
            } else if (is_64bit_operand) {
                out.opcode = (byte == 0x83) ? Opcode::CMP_RM64_IMM8 : Opcode::CMP_RM64_IMM32;
            } else if (op_size == 2) {
                out.opcode = (byte == 0x83) ? Opcode::CMP_RM16_IMM8 : Opcode::CMP_RM16_IMM16;
            } else {
                out.opcode = (byte == 0x83) ? Opcode::CMP_RM32_IMM8 : Opcode::CMP_RM32_IMM32;
            }
            break;
            
        default:
            return DecodeError::InvalidOpcode;
    }
    
    out.add_operand(rm);
    out.add_operand(Operand::make_imm(imm_type, imm_value));
    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
