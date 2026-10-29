// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE shuffle and unpack instruction decoding (CPU-038)
//
// Integration checklist (modifications to existing files):
//
// 1. opcodes.hpp — add to Opcode enum (before _COUNT):
//      // SSE Shuffle & Unpack Instructions (CPU-038)
//      SHUFPS_XMM_XMMM128_IMM8, SHUFPD_XMM_XMMM128_IMM8,
//      PSHUFD_XMM_XMMM128_IMM8, PSHUFLW_XMM_XMMM128_IMM8, PSHUFHW_XMM_XMMM128_IMM8,
//      UNPCKLPS_XMM_XMMM128, UNPCKHPS_XMM_XMMM128,
//      UNPCKLPD_XMM_XMMM128, UNPCKHPD_XMM_XMMM128,
//      PUNPCKLBW_XMM_XMMM128, PUNPCKLWD_XMM_XMMM128,
//      PUNPCKLDQ_XMM_XMMM128, PUNPCKLQDQ_XMM_XMMM128,
//      PUNPCKHBW_XMM_XMMM128, PUNPCKHWD_XMM_XMMM128,
//      PUNPCKHDQ_XMM_XMMM128, PUNPCKHQDQ_XMM_XMMM128,
//
// 2. decode_ops.hpp — add declaration:
//      [[nodiscard]] DecodeError decode_sse_shuffle(DecodeContext& ctx) noexcept;
//
// 3. two_byte.cpp — add cases in decode_two_byte switch (before default):
//      case 0xC6: case 0x70:
//      case 0x14: case 0x15:
//      case 0x60: case 0x61: case 0x62: case 0x6C:
//      case 0x68: case 0x69: case 0x6A: case 0x6D:
//          return decode_sse_shuffle(ctx);
//
// 4. src/CMakeLists.txt — add to vitreo_cpu_decode sources:
//      cpu/decode/opcodes/sse_shuffle.cpp

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_sse_shuffle(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;

    // Read ModR/M byte
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, ctx.mode, prefix, false, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Apply REX.R to reg field for XMM8-XMM15
    if (prefix.rex_r()) reg_field |= 8;

    // Determine mandatory prefix
    bool has_f3 = prefix.rep;
    bool has_f2 = prefix.repne;
    bool has_66 = prefix.operand_size;

    // Helper: set r/m operand type for packed 128-bit
    auto set_rm_packed = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem128;
        }
    };

    // Determine if this instruction needs an imm8 trailing byte
    bool needs_imm8 = false;

    switch (byte) {
        // 0xC6: SHUFPS (NP) / SHUFPD (66)
        case 0xC6:
            set_rm_packed();
            if (has_66) { out.opcode = Opcode::SHUFPD_XMM_XMMM128_IMM8; }
            else        { out.opcode = Opcode::SHUFPS_XMM_XMMM128_IMM8; }
            needs_imm8 = true;
            break;

        // 0x70: PSHUFD (66) / PSHUFLW (F2) / PSHUFHW (F3) / PSHUFW (NP)
        case 0x70:
            if (has_66)      { set_rm_packed(); out.opcode = Opcode::PSHUFD_XMM_XMMM128_IMM8; needs_imm8 = true; break; }
            if (has_f2)      { set_rm_packed(); out.opcode = Opcode::PSHUFLW_XMM_XMMM128_IMM8; needs_imm8 = true; break; }
            if (has_f3)      { set_rm_packed(); out.opcode = Opcode::PSHUFHW_XMM_XMMM128_IMM8; needs_imm8 = true; break; }
            // NP 0F 70 /r ib — PSHUFW mm1, mm2/m64, imm8. Destination is an
            // MMX register, not XMM0 (Intel SDM Vol. 2: PSHUFW).
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::RegMM;
                rm.reg &= 7;
            } else {
                rm.type = OperandType::Mem64;
            }
            {
                u8 imm;
                if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = Opcode::PSHUFW_MM_MMM64_IMM8;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            }
            return DecodeError::Success;

        // 0x14: UNPCKLPS (NP) / UNPCKLPD (66)
        case 0x14:
            set_rm_packed();
            if (has_66) { out.opcode = Opcode::UNPCKLPD_XMM_XMMM128; }
            else        { out.opcode = Opcode::UNPCKLPS_XMM_XMMM128; }
            break;

        // 0x15: UNPCKHPS (NP) / UNPCKHPD (66)
        case 0x15:
            set_rm_packed();
            if (has_66) { out.opcode = Opcode::UNPCKHPD_XMM_XMMM128; }
            else        { out.opcode = Opcode::UNPCKHPS_XMM_XMMM128; }
            break;

        // 0x60: PUNPCKLBW (66 = XMM, NP = MMX)
        case 0x60:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKLBW_XMM_XMMM128;
            } else {
                // MMX version: mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKLBW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;  // Return early for MMX
            }
            break;

        // 0x61: PUNPCKLWD (66 = XMM, NP = MMX)
        case 0x61:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKLWD_XMM_XMMM128;
            } else {
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKLWD_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;  // Return early for MMX
            }
            break;

        // 0x62: PUNPCKLDQ (66 = XMM, NP = MMX)
        case 0x62:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKLDQ_XMM_XMMM128;
            } else {
                // MMX version: mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKLDQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;  // Return early for MMX
            }
            break;

        // 0x6C: PUNPCKLQDQ (66)
        case 0x6C:
            if (!has_66) return DecodeError::InvalidOpcode;
            set_rm_packed();
            out.opcode = Opcode::PUNPCKLQDQ_XMM_XMMM128;
            break;

        // 0x68: PUNPCKHBW (66 = XMM, NP = MMX)
        case 0x68:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKHBW_XMM_XMMM128;
            } else {
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKHBW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;  // Return early for MMX
            }
            break;

        // 0x69: PUNPCKHWD (66 = XMM, NP = MMX)
        case 0x69:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKHWD_XMM_XMMM128;
            } else {
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKHWD_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;  // Return early for MMX
            }
            break;

        // 0x6A: PUNPCKHDQ (66 = XMM, NP = MMX)
        case 0x6A:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKHDQ_XMM_XMMM128;
            } else {
                // MMX version: mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKHDQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;  // Return early for MMX
            }
            break;

        // 0x6D: PUNPCKHQDQ — 66: xmm,xmm/m128; NP: mm,mm/m64 (Vitreo MM form;
        // only one qword exists, so interpreter/JIT treat it as dst ← src).
        case 0x6D:
            if (has_66) {
                set_rm_packed();
                out.opcode = Opcode::PUNPCKHQDQ_XMM_XMMM128;
            } else {
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PUNPCKHQDQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            break;

        default:
            return DecodeError::InvalidOpcode;
    }

    // Operand 0: destination XMM register (from reg field)
    out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
    // Operand 1: source XMM/memory (from r/m field)
    out.add_operand(rm);

    // Operand 2: imm8 (for shuffle instructions)
    if (needs_imm8) {
        u8 imm;
        if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
    }

    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
