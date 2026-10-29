// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE conversion instruction decoding (CPU-033)
//
// Integration checklist (modifications to existing files):
//
// 1. opcodes.hpp — add to Opcode enum (before _COUNT):
//      // SSE Conversion Instructions
//      CVTSI2SS_XMM_RM32, CVTSI2SS_XMM_RM64,
//      CVTSI2SD_XMM_RM32, CVTSI2SD_XMM_RM64,
//      CVTSS2SI_R32_XMMM32, CVTSS2SI_R64_XMMM32,
//      CVTSD2SI_R32_XMMM64, CVTSD2SI_R64_XMMM64,
//      CVTTSS2SI_R32_XMMM32, CVTTSS2SI_R64_XMMM32,
//      CVTTSD2SI_R32_XMMM64, CVTTSD2SI_R64_XMMM64,
//      CVTSS2SD_XMM_XMMM32, CVTSD2SS_XMM_XMMM64,
//      CVTPS2DQ_XMM_XMMM128, CVTDQ2PS_XMM_XMMM128,
//      CVTPD2DQ_XMM_XMMM128, CVTDQ2PD_XMM_XMMM64,
//      CVTPS2PD_XMM_XMMM64, CVTPD2PS_XMM_XMMM128,
//      CVTTPD2DQ_XMM_XMMM128, CVTTPS2DQ_XMM_XMMM128,
//
// 2. decode_ops.hpp — add declaration:
//      [[nodiscard]] DecodeError decode_sse_conv(DecodeContext& ctx) noexcept;
//
// 3. two_byte.cpp — add cases in decode_two_byte switch (before default):
//      case 0x2A: case 0x2C: case 0x2D:
//      case 0x5A: case 0x5B: case 0xE6:
//          return decode_sse_conv(ctx);
//
// 4. src/CMakeLists.txt — add to vitreo_cpu_decode sources:
//      cpu/decode/opcodes/sse_conv.cpp

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_sse_conv(DecodeContext& ctx) noexcept {
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

    // Apply REX.R to reg field for XMM8-XMM15 or R8-R15
    if (prefix.rex_r()) reg_field |= 8;

    // Determine mandatory prefix
    bool has_f3 = prefix.rep;
    bool has_f2 = prefix.repne;
    bool has_66 = prefix.operand_size;
    bool is_64  = prefix.rex_w();

    // Helpers to set r/m operand type
    auto set_rm_xmm_or_mem32 = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem32;
        }
    };

    auto set_rm_xmm_or_mem64 = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem64;
        }
    };

    auto set_rm_xmm_or_mem128 = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem128;
        }
    };

    auto set_rm_gp32_or_mem32 = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg32;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem32;
        }
    };

    auto set_rm_gp64_or_mem64 = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg64;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem64;
        }
    };

    auto set_rm_gp_or_mem = [&]() {
        if (is_64) {
            set_rm_gp64_or_mem64();
        } else {
            set_rm_gp32_or_mem32();
        }
    };

    // MMX source/destination: MM0-MM7 have no REX extension, and the memory
    // form is always a quadword.
    auto set_rm_mm_or_mem64 = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::RegMM;
            rm.reg &= 7;
        } else {
            rm.type = OperandType::Mem64;
        }
    };

    switch (byte) {

    // =====================================================================
    // 0x2A: CVTSI2SS (F3) / CVTSI2SD (F2)
    //   operand[0] = XMM reg, operand[1] = r/m32 or r/m64 (REX.W)
    // =====================================================================
    case 0x2A: {
        if (has_f3) {
            set_rm_gp_or_mem();
            out.opcode = is_64 ? Opcode::CVTSI2SS_XMM_RM64
                               : Opcode::CVTSI2SS_XMM_RM32;
        } else if (has_f2) {
            set_rm_gp_or_mem();
            out.opcode = is_64 ? Opcode::CVTSI2SD_XMM_RM64
                               : Opcode::CVTSI2SD_XMM_RM32;
        } else {
            // NP: CVTPI2PS xmm, mm/m64 — 66: CVTPI2PD xmm, mm/m64.
            set_rm_mm_or_mem64();
            out.opcode = has_66 ? Opcode::CVTPI2PD_XMM_MMM64
                                : Opcode::CVTPI2PS_XMM_MMM64;
        }
        out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // =====================================================================
    // 0x2D: CVTSS2SI (F3) / CVTSD2SI (F2)
    //   operand[0] = GP reg (Reg32/Reg64 per REX.W), operand[1] = XMM/mem
    // =====================================================================
    case 0x2D: {
        OperandType dst_type = is_64 ? OperandType::Reg64 : OperandType::Reg32;
        if (has_f3) {
            set_rm_xmm_or_mem32();
            out.opcode = is_64 ? Opcode::CVTSS2SI_R64_XMMM32
                               : Opcode::CVTSS2SI_R32_XMMM32;
        } else if (has_f2) {
            set_rm_xmm_or_mem64();
            out.opcode = is_64 ? Opcode::CVTSD2SI_R64_XMMM64
                               : Opcode::CVTSD2SI_R32_XMMM64;
        } else {
            // NP: CVTPS2PI mm, xmm/m64 — 66: CVTPD2PI mm, xmm/m128.
            if (has_66) {
                set_rm_xmm_or_mem128();
                out.opcode = Opcode::CVTPD2PI_MM_XMMM128;
            } else {
                set_rm_xmm_or_mem64();
                out.opcode = Opcode::CVTPS2PI_MM_XMMM64;
            }
            out.add_operand(Operand::reg_mm(reg_field & 7));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        out.add_operand(Operand::make_reg(dst_type, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // =====================================================================
    // 0x2C: CVTTSS2SI (F3) / CVTTSD2SI (F2)
    //   operand[0] = GP reg, operand[1] = XMM/mem
    // =====================================================================
    case 0x2C: {
        OperandType dst_type = is_64 ? OperandType::Reg64 : OperandType::Reg32;
        if (has_f3) {
            set_rm_xmm_or_mem32();
            out.opcode = is_64 ? Opcode::CVTTSS2SI_R64_XMMM32
                               : Opcode::CVTTSS2SI_R32_XMMM32;
        } else if (has_f2) {
            set_rm_xmm_or_mem64();
            out.opcode = is_64 ? Opcode::CVTTSD2SI_R64_XMMM64
                               : Opcode::CVTTSD2SI_R32_XMMM64;
        } else {
            // NP: CVTTPS2PI mm, xmm/m64 — 66: CVTTPD2PI mm, xmm/m128.
            if (has_66) {
                set_rm_xmm_or_mem128();
                out.opcode = Opcode::CVTTPD2PI_MM_XMMM128;
            } else {
                set_rm_xmm_or_mem64();
                out.opcode = Opcode::CVTTPS2PI_MM_XMMM64;
            }
            out.add_operand(Operand::reg_mm(reg_field & 7));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        out.add_operand(Operand::make_reg(dst_type, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // =====================================================================
    // 0x5A: CVTSS2SD (F3) / CVTSD2SS (F2) / CVTPS2PD (NP) / CVTPD2PS (66)
    //   All: operand[0] = XMM reg, operand[1] = XMM/mem
    // =====================================================================
    case 0x5A: {
        if (has_f3) {
            set_rm_xmm_or_mem32();
            out.opcode = Opcode::CVTSS2SD_XMM_XMMM32;
        } else if (has_f2) {
            set_rm_xmm_or_mem64();
            out.opcode = Opcode::CVTSD2SS_XMM_XMMM64;
        } else if (has_66) {
            set_rm_xmm_or_mem128();
            out.opcode = Opcode::CVTPD2PS_XMM_XMMM128;
        } else {
            // NP: CVTPS2PD — reads 64 bits (2×f32)
            set_rm_xmm_or_mem64();
            out.opcode = Opcode::CVTPS2PD_XMM_XMMM64;
        }
        out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // =====================================================================
    // 0x5B: CVTPS2DQ (66) / CVTDQ2PS (NP) / CVTTPS2DQ (F3)
    //   All: operand[0] = XMM reg, operand[1] = XMM/m128
    // =====================================================================
    case 0x5B: {
        set_rm_xmm_or_mem128();
        if (has_66) {
            out.opcode = Opcode::CVTPS2DQ_XMM_XMMM128;
        } else if (has_f3) {
            out.opcode = Opcode::CVTTPS2DQ_XMM_XMMM128;
        } else {
            out.opcode = Opcode::CVTDQ2PS_XMM_XMMM128;
        }
        out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // =====================================================================
    // 0xE6: CVTPD2DQ (F2) / CVTDQ2PD (F3) / CVTTPD2DQ (66)
    //   operand[0] = XMM reg, operand[1] = XMM/m128, except CVTDQ2PD:
    //   Intel SDM Vol. 2 "F3 0F E6 /r CVTDQ2PD xmm1, xmm2/m64" reads only
    //   two i32 (exception Type 5: no alignment rule), like CVTPS2PD.
    // =====================================================================
    case 0xE6: {
        if (has_f2) {
            set_rm_xmm_or_mem128();
            out.opcode = Opcode::CVTPD2DQ_XMM_XMMM128;
        } else if (has_f3) {
            set_rm_xmm_or_mem64();
            out.opcode = Opcode::CVTDQ2PD_XMM_XMMM64;
        } else if (has_66) {
            set_rm_xmm_or_mem128();
            out.opcode = Opcode::CVTTPD2DQ_XMM_XMMM128;
        } else {
            return DecodeError::InvalidOpcode;
        }
        out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    default:
        return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
