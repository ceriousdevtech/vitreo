// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Control flow instruction decoding (JMP, CALL, RET, Jcc)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_jmp(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // JMP rel8 (EB)
        case 0xEB: {
            i8 disp;
            if (!reader.read_i8(disp)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::JMP_REL8;
            out.add_operand(Operand::make_imm(OperandType::Rel8, disp));
            return DecodeError::Success;
        }
        
        // JMP rel16/32 (E9)
        case 0xE9: {
            if (op_size == 2 && mode != DecodeMode::Mode64) {
                i16 disp;
                if (!reader.read_i16(disp)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::JMP_REL16;
                out.add_operand(Operand::make_imm(OperandType::Rel16, disp));
            } else {
                i32 disp;
                if (!reader.read_i32(disp)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::JMP_REL32;
                out.add_operand(Operand::make_imm(OperandType::Rel32, disp));
            }
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_jcc_rel8(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    
    switch (byte) {
        case 0x70: out.opcode = Opcode::JO_REL8; break;
        case 0x71: out.opcode = Opcode::JNO_REL8; break;
        case 0x72: out.opcode = Opcode::JB_REL8; break;
        case 0x73: out.opcode = Opcode::JNB_REL8; break;
        case 0x74: out.opcode = Opcode::JZ_REL8; break;
        case 0x75: out.opcode = Opcode::JNZ_REL8; break;
        case 0x76: out.opcode = Opcode::JBE_REL8; break;
        case 0x77: out.opcode = Opcode::JNBE_REL8; break;
        case 0x78: out.opcode = Opcode::JS_REL8; break;
        case 0x79: out.opcode = Opcode::JNS_REL8; break;
        case 0x7A: out.opcode = Opcode::JP_REL8; break;
        case 0x7B: out.opcode = Opcode::JNP_REL8; break;
        case 0x7C: out.opcode = Opcode::JL_REL8; break;
        case 0x7D: out.opcode = Opcode::JNL_REL8; break;
        case 0x7E: out.opcode = Opcode::JLE_REL8; break;
        case 0x7F: out.opcode = Opcode::JNLE_REL8; break;
        default:
            return DecodeError::InvalidOpcode;
    }
    
    i8 disp;
    if (!reader.read_i8(disp)) return DecodeError::UnexpectedEnd;
    out.add_operand(Operand::make_imm(OperandType::Rel8, disp));
    return DecodeError::Success;
}

DecodeError decode_loop_jcxz(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    DecodeMode mode = ctx.mode;

    switch (byte) {
        case 0xE0: out.opcode = Opcode::LOOPNE_REL8; break;
        case 0xE1: out.opcode = Opcode::LOOPE_REL8; break;
        case 0xE2: out.opcode = Opcode::LOOP_REL8; break;
        case 0xE3:
            // In 64-bit mode, E3 encodes JRCXZ.
            // In 16/32-bit modes, the address-size override selects JCXZ vs JECXZ.
            if (mode == DecodeMode::Mode64) {
                out.opcode = Opcode::JRCXZ_REL8;
            } else {
                out.opcode = ctx.prefix.address_size ? Opcode::JCXZ_REL8 : Opcode::JECXZ_REL8;
            }
            break;
        default:
            return DecodeError::InvalidOpcode;
    }

    i8 disp;
    if (!reader.read_i8(disp)) return DecodeError::UnexpectedEnd;
    out.add_operand(Operand::make_imm(OperandType::Rel8, disp));
    return DecodeError::Success;
}

DecodeError decode_call_ret(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // CALL rel16/32 (E8)
        case 0xE8: {
            if (op_size == 2 && mode != DecodeMode::Mode64) {
                i16 disp;
                if (!reader.read_i16(disp)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::CALL_REL16;
                out.add_operand(Operand::make_imm(OperandType::Rel16, disp));
            } else {
                i32 disp;
                if (!reader.read_i32(disp)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::CALL_REL32;
                out.add_operand(Operand::make_imm(OperandType::Rel32, disp));
            }
            return DecodeError::Success;
        }
        
        // RET (C3)
        case 0xC3:
            out.opcode = Opcode::RET_NEAR;
            return DecodeError::Success;
        
        // RET imm16 (C2)
        case 0xC2: {
            u16 imm;
            if (!reader.read_u16(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::RET_NEAR_IMM16;
            out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
