// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Miscellaneous instruction decoding (LEA, INT, NOP, HLT, etc.)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_lea(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    if (byte != 0x8D) {
        return DecodeError::InvalidOpcode;
    }
    
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }
    
    // LEA requires memory operand
    if (!is_memory_operand(rm.type)) {
        return DecodeError::InvalidOperand;
    }
    
    if (is_64bit_operand) {
        out.opcode = Opcode::LEA_R64_M;
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
    } else if (op_size == 2) {
        out.opcode = Opcode::LEA_R16_M;
        out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
    } else {
        out.opcode = Opcode::LEA_R32_M;
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
    }
    out.add_operand(rm);
    return DecodeError::Success;
}

DecodeError decode_int(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    
    switch (byte) {
        // INT 3 (CC)
        case 0xCC:
            out.opcode = Opcode::INT_3;
            return DecodeError::Success;
        
        // INT imm8 (CD)
        case 0xCD: {
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::INT_IMM8;
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }

        // INTO (CE) — Intel SDM INT n/INTO/INT3: #OF if OF=1.
        // Invalid in 64-bit long mode (#UD).
        case 0xCE:
            if (ctx.mode == DecodeMode::Mode64) {
                return DecodeError::InvalidOpcode;
            }
            out.opcode = Opcode::INTO;
            return DecodeError::Success;
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_nop_xchg(DecodeContext& ctx) noexcept {
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 reg_idx = (byte - 0x90) | (prefix.rex_b() ? 8 : 0);
    
    if (byte == 0x90 && !prefix.rex_b()) {
        // F3 90 is PAUSE (Intel SDM PAUSE); plain 90 is NOP.
        // REX.B 90 remains XCHG r8, eax/rax.
        out.opcode = prefix.rep ? Opcode::PAUSE : Opcode::NOP;
    } else if (is_64bit_operand) {
        out.opcode = Opcode::XCHG_RAX_R64;
        out.add_operand(Operand::make_reg(OperandType::Reg64, 0));
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_idx));
    } else if (ctx.op_size == 2) {
        // 66 90+r is XCHG AX, r16 -- a SIXTEEN-bit exchange, which leaves the
        // upper half of both registers alone. Decoding it as the 32-bit form
        // swapped the full registers and zero-extended them, which is a wrong
        // RESULT rather than a missing form, and silent: the 16-bit halves it
        // produces are correct.
        out.opcode = Opcode::XCHG_AX_R16;
        out.add_operand(Operand::make_reg(OperandType::Reg16, 0));
        out.add_operand(Operand::make_reg(OperandType::Reg16, reg_idx));
    } else {
        out.opcode = Opcode::XCHG_EAX_R32;
        out.add_operand(Operand::make_reg(OperandType::Reg32, 0));
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_idx));
    }
    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
