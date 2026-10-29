// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// MOV instruction decoding

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_mov(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // MOV r8, imm8 (B0-B7)
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            u8 reg = (byte - 0xB0) | (prefix.rex_b() ? 8 : 0);
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::MOV_R8_IMM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg));
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // MOV r16/32/64, imm16/32/64 (B8-BF)
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            u8 reg = (byte - 0xB8) | (prefix.rex_b() ? 8 : 0);
            if (is_64bit_operand) {
                u64 imm;
                if (!reader.read_u64(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::MOV_R64_IMM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg));
                out.add_operand(Operand::make_imm(OperandType::Imm64, static_cast<i64>(imm)));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::MOV_R16_IMM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg));
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::MOV_R32_IMM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg));
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // MOV r/m8, r8 (88)
        case 0x88: {
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
            out.opcode = Opcode::MOV_RM8_R8;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            return DecodeError::Success;
        }
        
        // MOV r/m16/32/64, r16/32/64 (89)
        case 0x89: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::MOV_RM64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::MOV_RM16_R16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::MOV_RM32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }
        
        // MOV r8, r/m8 (8A)
        case 0x8A: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            out.opcode = Opcode::MOV_R8_RM8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // MOV r16/32/64, r/m16/32/64 (8B)
        case 0x8B: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::MOV_R64_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::MOV_R16_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::MOV_R32_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // MOV r/m16, Sreg (8C /r)
        // The reg field of ModR/M selects a segment register (0=ES,1=CS,
        // 2=SS,3=DS,4=FS,5=GS). Sreg is always 16 bits; r/m may be a 16-bit
        // memory location or a register (in which case high bits of the
        // 32/64-bit GPR are zero-extended on real CPUs — we just write 16).
        case 0x8C: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg16;
            } else if (is_memory_operand(rm.type)) {
                rm.type = OperandType::Mem16;
            }
            if (reg_field > 5) return DecodeError::InvalidOpcode; // only ES/CS/SS/DS/FS/GS
            out.opcode = Opcode::MOV_RM16_SREG;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::RegSeg, reg_field));
            return DecodeError::Success;
        }

        // MOV Sreg, r/m16 (8E /r)
        case 0x8E: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg16;
            } else if (is_memory_operand(rm.type)) {
                rm.type = OperandType::Mem16;
            }
            if (reg_field > 5) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::MOV_SREG_RM16;
            out.add_operand(Operand::make_reg(OperandType::RegSeg, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // MOV r/m8, imm8 (C6 /0)
        case 0xC6: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            
            // Check reg field is 0 (MOV variant)
            u8 reg_field_raw = (modrm >> 3) & 0x07;
            if (reg_field_raw != 0) {
                return DecodeError::InvalidOpcode; // Other C6 variants not MOV
            }
            
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
            
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            
            out.opcode = Opcode::MOV_RM8_IMM8;
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        
        // MOV r/m16/32/64, imm16/32 (C7 /0)
        case 0xC7: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            
            // Check reg field is 0 (MOV variant)
            u8 reg_field_raw = (modrm >> 3) & 0x07;
            if (reg_field_raw != 0) {
                return DecodeError::InvalidOpcode; // Other C7 variants not MOV
            }
            
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            
            if (is_64bit_operand) {
                // 64-bit mode: imm32 sign-extended to 64-bit
                i32 imm;
                if (!reader.read_i32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::MOV_RM64_IMM32;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            } else if (op_size == 2) {
                u16 imm;
                if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::MOV_RM16_IMM16;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            } else {
                u32 imm;
                if (!reader.read_u32(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::MOV_RM32_IMM32;
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm32, imm));
            }
            return DecodeError::Success;
        }
        
        // MOV AL, moffs8 (A0)
        case 0xA0: {
            // In 64-bit mode with 67h prefix, address is 32-bit; otherwise 64-bit
            MemoryOperand mem;
            mem.segment = prefix.segment;
            if (mode == DecodeMode::Mode64 && !prefix.address_size) {
                u64 addr;
                if (!reader.read_u64(addr)) return DecodeError::UnexpectedEnd;
                mem.absolute = addr;
                mem.has_absolute = true;
                mem.base = 255; // No base
            } else {
                u32 addr;
                if (!reader.read_u32(addr)) return DecodeError::UnexpectedEnd;
                mem.disp = static_cast<i32>(addr);
                mem.base = 255;
            }
            out.opcode = Opcode::MOV_AL_MOFFS8;
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0)); // AL
            out.add_operand(Operand::make_mem(OperandType::Mem8, mem));
            return DecodeError::Success;
        }
        
        // MOV AX/EAX/RAX, moffs16/32/64 (A1)
        case 0xA1: {
            MemoryOperand mem;
            mem.segment = prefix.segment;
            if (mode == DecodeMode::Mode64 && !prefix.address_size) {
                u64 addr;
                if (!reader.read_u64(addr)) return DecodeError::UnexpectedEnd;
                mem.absolute = addr;
                mem.has_absolute = true;
                mem.base = 255;
            } else {
                u32 addr;
                if (!reader.read_u32(addr)) return DecodeError::UnexpectedEnd;
                mem.disp = static_cast<i32>(addr);
                mem.base = 255;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::MOV_RAX_MOFFS64;
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
                out.add_operand(Operand::make_mem(OperandType::Mem64, mem));
            } else if (op_size == 2) {
                out.opcode = Opcode::MOV_AX_MOFFS16;
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
                out.add_operand(Operand::make_mem(OperandType::Mem16, mem));
            } else {
                out.opcode = Opcode::MOV_EAX_MOFFS32;
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
                out.add_operand(Operand::make_mem(OperandType::Mem32, mem));
            }
            return DecodeError::Success;
        }
        
        // MOV moffs8, AL (A2)
        case 0xA2: {
            MemoryOperand mem;
            mem.segment = prefix.segment;
            if (mode == DecodeMode::Mode64 && !prefix.address_size) {
                u64 addr;
                if (!reader.read_u64(addr)) return DecodeError::UnexpectedEnd;
                mem.absolute = addr;
                mem.has_absolute = true;
                mem.base = 255;
            } else {
                u32 addr;
                if (!reader.read_u32(addr)) return DecodeError::UnexpectedEnd;
                mem.disp = static_cast<i32>(addr);
                mem.base = 255;
            }
            out.opcode = Opcode::MOV_MOFFS8_AL;
            out.add_operand(Operand::make_mem(OperandType::Mem8, mem));
            out.add_operand(Operand::make_reg(OperandType::Reg8, 0)); // AL
            return DecodeError::Success;
        }
        
        // MOV moffs16/32/64, AX/EAX/RAX (A3)
        case 0xA3: {
            MemoryOperand mem;
            mem.segment = prefix.segment;
            if (mode == DecodeMode::Mode64 && !prefix.address_size) {
                u64 addr;
                if (!reader.read_u64(addr)) return DecodeError::UnexpectedEnd;
                mem.absolute = addr;
                mem.has_absolute = true;
                mem.base = 255;
            } else {
                u32 addr;
                if (!reader.read_u32(addr)) return DecodeError::UnexpectedEnd;
                mem.disp = static_cast<i32>(addr);
                mem.base = 255;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::MOV_MOFFS64_RAX;
                out.add_operand(Operand::make_mem(OperandType::Mem64, mem));
                out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
            } else if (op_size == 2) {
                out.opcode = Opcode::MOV_MOFFS16_AX;
                out.add_operand(Operand::make_mem(OperandType::Mem16, mem));
                out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
            } else {
                out.opcode = Opcode::MOV_MOFFS32_EAX;
                out.add_operand(Operand::make_mem(OperandType::Mem32, mem));
                out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
            }
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
