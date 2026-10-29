// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Stack instruction decoding (PUSH, POP)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_push(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        case 0x06: case 0x0E: case 0x16: case 0x1E: {
            if (mode == DecodeMode::Mode64) return DecodeError::InvalidOpcode;
            constexpr Opcode opcodes[] = {
                Opcode::PUSH_ES, Opcode::PUSH_CS, Opcode::PUSH_SS, Opcode::PUSH_DS,
            };
            constexpr SegReg segments[] = {
                SegReg::ES, SegReg::CS, SegReg::SS, SegReg::DS,
            };
            const u8 index = byte >> 3;
            out.opcode = opcodes[index];
            out.add_operand(Operand::make_reg(
                OperandType::RegSeg, static_cast<u8>(segments[index])));
            return DecodeError::Success;
        }

        // PUSH r16/64 (50-57)
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: {
            u8 reg = (byte - 0x50) | (prefix.rex_b() ? 8 : 0);
            if (mode == DecodeMode::Mode64 && op_size != 2) {
            // Intel SDM Vol. 2 PUSH/POP: 64-bit mode defaults to a
            // 64-bit operand, but a 66H override still selects 16-bit
            // (2-byte stack slot). Only 32-bit is unencodable here.
                out.opcode = Opcode::PUSH_R64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg));
            } else if (op_size == 2) {
                out.opcode = Opcode::PUSH_R16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg));
            } else {
                out.opcode = Opcode::PUSH_R32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg));
            }
            return DecodeError::Success;
        }
        
        // PUSH imm8 (6A)
        case 0x6A: {
            i8 imm;
            if (!reader.read_i8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::PUSH_IMM8;
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // PUSH imm16/32 (68)
        case 0x68: {
            if ((is_64bit_operand || mode == DecodeMode::Mode64)
                && op_size != 2) {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::PUSH_IMM32;
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                i16 imm;
                if (!reader.read_i16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::PUSH_IMM16;
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::PUSH_IMM32;
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_pop(DecodeContext& ctx) noexcept {
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        case 0x07: case 0x17: case 0x1F: {
            if (mode == DecodeMode::Mode64) return DecodeError::InvalidOpcode;
            Opcode opcode;
            SegReg segment;
            if (byte == 0x07) {
                opcode = Opcode::POP_ES;
                segment = SegReg::ES;
            } else if (byte == 0x17) {
                opcode = Opcode::POP_SS;
                segment = SegReg::SS;
            } else {
                opcode = Opcode::POP_DS;
                segment = SegReg::DS;
            }
            out.opcode = opcode;
            out.add_operand(Operand::make_reg(
                OperandType::RegSeg, static_cast<u8>(segment)));
            return DecodeError::Success;
        }

        // POP r16/64 (58-5F)
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            u8 reg = (byte - 0x58) | (prefix.rex_b() ? 8 : 0);
            if (mode == DecodeMode::Mode64 && op_size != 2) {
                out.opcode = Opcode::POP_R64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg));
            } else if (op_size == 2) {
                out.opcode = Opcode::POP_R16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg));
            } else {
                out.opcode = Opcode::POP_R32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg));
            }
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_pop_rm(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    if (byte != 0x8F) {
        return DecodeError::InvalidOpcode;
    }
    
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    
    u8 reg_field = (modrm >> 3) & 0x07;
    
    // Only /0 is valid for POP r/m
    if (reg_field != 0) {
        return DecodeError::InvalidOpcode;
    }
    
    Operand rm;
    u8 unused_reg;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, unused_reg) < 0) {
        return DecodeError::InvalidModRM;
    }
    
    // In 64-bit mode, default operand size is 64-bit (not 32)
    if (mode == DecodeMode::Mode64 && op_size != 2) {
        out.opcode = Opcode::POP_RM64;
        if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
        else if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
    } else if (op_size == 2) {
        out.opcode = Opcode::POP_RM16;
        if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
        else if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
    } else {
        out.opcode = Opcode::POP_RM32;
        if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
        else if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
    }
    out.add_operand(rm);
    
    return DecodeError::Success;
}

DecodeError decode_group5(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    if (byte != 0xFF) {
        return DecodeError::InvalidOpcode;
    }
    
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    
    u8 reg_field = (modrm >> 3) & 0x07;
    
    Operand rm;
    u8 unused_reg;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, unused_reg) < 0) {
        return DecodeError::InvalidModRM;
    }
    
    switch (reg_field) {
        case 0: // INC r/m16/32/64
            if (is_64bit_operand) {
                out.opcode = Opcode::INC_RM64;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
            } else if (op_size == 2) {
                out.opcode = Opcode::INC_RM16;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            } else {
                out.opcode = Opcode::INC_RM32;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
        
        case 1: // DEC r/m16/32/64
            if (is_64bit_operand) {
                out.opcode = Opcode::DEC_RM64;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
            } else if (op_size == 2) {
                out.opcode = Opcode::DEC_RM16;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            } else {
                out.opcode = Opcode::DEC_RM32;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
        
        case 2: // CALL r/m16/32/64
            if (mode == DecodeMode::Mode64) {
                out.opcode = Opcode::CALL_RM64;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
            } else if (op_size == 2) {
                out.opcode = Opcode::CALL_RM16;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            } else {
                out.opcode = Opcode::CALL_RM32;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            }
            out.add_operand(rm);
            return DecodeError::Success;

        case 3: { // CALL far m16:16/32/64 — Intel SDM Vol. 2 CALL. Memory only.
            if (!is_memory_operand(rm.type)) {
                return DecodeError::InvalidOpcode;
            }
            if (mode == DecodeMode::Mode64) {
                // Default m16:64; 66H → m16:16. REX.W keeps m16:64.
                out.opcode = (prefix.operand_size && !is_64bit_operand)
                    ? Opcode::CALL_M16_16 : Opcode::CALL_M16_64;
            } else if (op_size == 2) {
                out.opcode = Opcode::CALL_M16_16;
            } else {
                out.opcode = Opcode::CALL_M16_32;
            }
            rm.type = OperandType::MemUnknown;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        case 4: // JMP r/m16/32/64
            if (mode == DecodeMode::Mode64) {
                out.opcode = Opcode::JMP_RM64;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
            } else if (op_size == 2) {
                out.opcode = Opcode::JMP_RM16;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            } else {
                out.opcode = Opcode::JMP_RM32;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            }
            out.add_operand(rm);
            return DecodeError::Success;

        case 5: { // JMP far m16:16/32/64 — Intel SDM Vol. 2 JMP. Memory only.
            if (!is_memory_operand(rm.type)) {
                return DecodeError::InvalidOpcode;
            }
            if (mode == DecodeMode::Mode64) {
                out.opcode = (prefix.operand_size && !is_64bit_operand)
                    ? Opcode::JMP_M16_16 : Opcode::JMP_M16_64;
            } else if (op_size == 2) {
                out.opcode = Opcode::JMP_M16_16;
            } else {
                out.opcode = Opcode::JMP_M16_32;
            }
            rm.type = OperandType::MemUnknown;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        case 6: // PUSH r/m16/32/64
            // In 64-bit mode, default operand size is 64-bit (not 32)
            if (mode == DecodeMode::Mode64 && op_size != 2) {
                out.opcode = Opcode::PUSH_RM64;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
            } else if (op_size == 2) {
                out.opcode = Opcode::PUSH_RM16;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            } else {
                out.opcode = Opcode::PUSH_RM32;
                if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                else if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            }
            out.add_operand(rm);
            return DecodeError::Success;
        
        default:
            // Other Group 5 operations (INC /0, DEC /1) handled elsewhere
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
