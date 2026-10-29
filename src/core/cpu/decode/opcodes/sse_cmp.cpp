// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE comparison instruction decoding (CPU-036)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_sse_cmp(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    DecodeMode mode = ctx.mode;

    // Read ModR/M byte
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Apply REX.R to reg field for XMM8-XMM15
    if (prefix.rex_r()) reg_field |= 8;

    // Determine mandatory prefix
    bool has_f3 = prefix.rep;
    bool has_f2 = prefix.repne;
    bool has_66 = prefix.operand_size;

    // Helper lambdas for operand type setup
    auto set_rm_scalar_ss = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem32;
        }
    };

    auto set_rm_scalar_sd = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem64;
        }
    };

    auto set_rm_packed = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem128;
        }
    };

    switch (byte) {
        // 0xC2: CMPPS / CMPSS / CMPPD / CMPSD (+ imm8)
        case 0xC2: {
            if (has_f3)      { out.opcode = Opcode::CMPSS_XMM_XMMM32_IMM8;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::CMPSD_XMM_XMMM64_IMM8;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::CMPPD_XMM_XMMM128_IMM8; set_rm_packed(); }
            else             { out.opcode = Opcode::CMPPS_XMM_XMMM128_IMM8; set_rm_packed(); }

            // Operand 0: destination XMM register
            out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
            // Operand 1: source XMM/memory
            out.add_operand(rm);

            // Operand 2: imm8 predicate
            u8 imm8;
            if (!reader.read_u8(imm8)) return DecodeError::UnexpectedEnd;
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm8));

            return DecodeError::Success;
        }

        // 0x74: PCMPEQB (66 = XMM, NP = MMX)
        case 0x74:
            if (!has_66 && !has_f2 && !has_f3) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PCMPEQB_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!has_66) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::PCMPEQB_XMM_XMMM128;
            set_rm_packed();
            break;

        // 0x75: PCMPEQW (66 = XMM, NP = MMX)
        case 0x75:
            if (!has_66 && !has_f2 && !has_f3) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PCMPEQW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!has_66) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::PCMPEQW_XMM_XMMM128;
            set_rm_packed();
            break;

        // 0x76: PCMPEQD (66 = XMM, NP = MMX)
        case 0x76:
            if (!has_66 && !has_f2 && !has_f3) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PCMPEQD_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!has_66) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::PCMPEQD_XMM_XMMM128;
            set_rm_packed();
            break;

        // 0x64: PCMPGTB (66 = XMM, NP = MMX)
        case 0x64:
            if (!has_66 && !has_f2 && !has_f3) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PCMPGTB_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!has_66) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::PCMPGTB_XMM_XMMM128;
            set_rm_packed();
            break;

        // 0x65: PCMPGTW (66 = XMM, NP = MMX)
        case 0x65:
            if (!has_66 && !has_f2 && !has_f3) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PCMPGTW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!has_66) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::PCMPGTW_XMM_XMMM128;
            set_rm_packed();
            break;

        // 0x66: PCMPGTD (66 = XMM, NP = MMX)
        case 0x66:
            if (!has_66 && !has_f2 && !has_f3) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PCMPGTD_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!has_66) return DecodeError::InvalidOpcode;
            out.opcode = Opcode::PCMPGTD_XMM_XMMM128;
            set_rm_packed();
            break;

        default:
            return DecodeError::InvalidOpcode;
    }

    // Operand 0: destination XMM register (from reg field)
    out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
    // Operand 1: source XMM/memory (from r/m field)
    out.add_operand(rm);

    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
