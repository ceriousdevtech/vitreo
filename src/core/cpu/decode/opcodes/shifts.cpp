// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Shift instruction decoding (SHL, SHR, SAR) - Group 2

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_shifts(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    // Determine operand width category from opcode byte
    // C0, D0, D2 = 8-bit operand;  C1, D1, D3 = 16/32/64
    bool is_8bit = (byte == 0xC0 || byte == 0xD0 || byte == 0xD2);
    
    // Determine shift-count source
    // C0/C1 = imm8,  D0/D1 = 1,  D2/D3 = CL
    enum class CountSrc { Imm8, One, CL };
    CountSrc count_src;
    switch (byte) {
        case 0xC0: case 0xC1: count_src = CountSrc::Imm8; break;
        case 0xD0: case 0xD1: count_src = CountSrc::One;  break;
        case 0xD2: case 0xD3: count_src = CountSrc::CL;   break;
        default:
            return DecodeError::InvalidOpcode;
    }
    
    // Read ModR/M byte
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    
    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_8bit ? false : is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }
    
    // Determine the opcode from reg_field and width / count source
    // reg_field: 0=ROL, 1=ROR, 2=RCL, 3=RCR, 4/6=SHL, 5=SHR, 7=SAR.
    // /4 is the canonical legacy encoding. Existing x86 processors also
    // execute the otherwise-unassigned /6 form as the same SHL operation.
    if (is_8bit) {
        // Coerce rm to 8-bit type
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
        
        switch (reg_field) {
            case 0: // ROL
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROL_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROL_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROL_RM8_CL;   break;
                }
                break;
            case 1: // ROR
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROR_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROR_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROR_RM8_CL;   break;
                }
                break;
            case 2: // RCL
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCL_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCL_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCL_RM8_CL;   break;
                }
                break;
            case 3: // RCR
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCR_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCR_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCR_RM8_CL;   break;
                }
                break;
            case 4: // SHL
            case 6: // Legacy SHL alias
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHL_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHL_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHL_RM8_CL;   break;
                }
                break;
            case 5: // SHR
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHR_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHR_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHR_RM8_CL;   break;
                }
                break;
            case 7: // SAR
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SAR_RM8_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SAR_RM8_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SAR_RM8_CL;   break;
                }
                break;
            default:
                return DecodeError::InvalidOpcode;
        }
    } else if (is_64bit_operand) {
        // 64-bit operand
        switch (reg_field) {
            case 0:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROL_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROL_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROL_RM64_CL;   break;
                }
                break;
            case 1:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROR_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROR_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROR_RM64_CL;   break;
                }
                break;
            case 2:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCL_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCL_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCL_RM64_CL;   break;
                }
                break;
            case 3:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCR_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCR_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCR_RM64_CL;   break;
                }
                break;
            case 4:
            case 6:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHL_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHL_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHL_RM64_CL;   break;
                }
                break;
            case 5:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHR_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHR_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHR_RM64_CL;   break;
                }
                break;
            case 7:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SAR_RM64_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SAR_RM64_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SAR_RM64_CL;   break;
                }
                break;
            default:
                return DecodeError::InvalidOpcode;
        }
    } else if (op_size == 2) {
        // 16-bit operand
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
        
        switch (reg_field) {
            case 0:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROL_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROL_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROL_RM16_CL;   break;
                }
                break;
            case 1:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROR_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROR_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROR_RM16_CL;   break;
                }
                break;
            case 2:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCL_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCL_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCL_RM16_CL;   break;
                }
                break;
            case 3:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCR_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCR_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCR_RM16_CL;   break;
                }
                break;
            case 4:
            case 6:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHL_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHL_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHL_RM16_CL;   break;
                }
                break;
            case 5:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHR_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHR_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHR_RM16_CL;   break;
                }
                break;
            case 7:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SAR_RM16_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SAR_RM16_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SAR_RM16_CL;   break;
                }
                break;
            default:
                return DecodeError::InvalidOpcode;
        }
    } else {
        // 32-bit operand (default)
        switch (reg_field) {
            case 0:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROL_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROL_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROL_RM32_CL;   break;
                }
                break;
            case 1:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::ROR_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::ROR_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::ROR_RM32_CL;   break;
                }
                break;
            case 2:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCL_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCL_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCL_RM32_CL;   break;
                }
                break;
            case 3:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::RCR_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::RCR_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::RCR_RM32_CL;   break;
                }
                break;
            case 4:
            case 6:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHL_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHL_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHL_RM32_CL;   break;
                }
                break;
            case 5:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SHR_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SHR_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SHR_RM32_CL;   break;
                }
                break;
            case 7:
                switch (count_src) {
                    case CountSrc::Imm8: out.opcode = Opcode::SAR_RM32_IMM8; break;
                    case CountSrc::One:  out.opcode = Opcode::SAR_RM32_1;    break;
                    case CountSrc::CL:   out.opcode = Opcode::SAR_RM32_CL;   break;
                }
                break;
            default:
                return DecodeError::InvalidOpcode;
        }
    }
    
    // First operand: the r/m destination
    out.add_operand(rm);
    
    // Second operand: the shift count
    switch (count_src) {
        case CountSrc::Imm8: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            break;
        }
        case CountSrc::One: {
            out.add_operand(Operand::make_imm(OperandType::Imm8, 1));
            break;
        }
        case CountSrc::CL: {
            out.add_operand(Operand::make_reg(OperandType::Reg8, 1)); // CL = register index 1
            break;
        }
    }
    
    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
