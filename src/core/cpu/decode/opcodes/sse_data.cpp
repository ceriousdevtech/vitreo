// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE data transfer instruction decoding (CPU-031)
//
// Integration checklist (modifications to existing files):
//
// 1. decoder.hpp — add to OperandType enum (between Reg64 and RegSeg):
//      Reg128,      // 128-bit register (XMM)
//
// 2. opcodes.hpp — add to Opcode enum (before _COUNT):
//      // SSE Data Transfer
//      MOVAPS_XMM_XMMM128, MOVAPS_XMMM128_XMM,
//      MOVUPS_XMM_XMMM128, MOVUPS_XMMM128_XMM,
//      MOVAPD_XMM_XMMM128, MOVAPD_XMMM128_XMM,
//      MOVUPD_XMM_XMMM128, MOVUPD_XMMM128_XMM,
//      MOVDQA_XMM_XMMM128, MOVDQA_XMMM128_XMM,
//      MOVDQU_XMM_XMMM128, MOVDQU_XMMM128_XMM,
//      MOVSS_XMM_XMMM32, MOVSS_XMMM32_XMM,
//      MOVSD_XMM_XMMM64, MOVSD_XMMM64_XMM,
//      MOVLPS_XMM_M64, MOVLPS_M64_XMM,
//      MOVHPS_XMM_M64, MOVHPS_M64_XMM,
//      MOVLPD_XMM_M64, MOVLPD_M64_XMM,
//      MOVHPD_XMM_M64, MOVHPD_M64_XMM,
//      MOVMSKPS_R32_XMM, MOVMSKPD_R32_XMM,
//      XORPS_XMM_XMMM128, XORPD_XMM_XMMM128,
//      ANDPS_XMM_XMMM128, ANDPD_XMM_XMMM128,
//      ORPS_XMM_XMMM128, ORPD_XMM_XMMM128,
//      ANDNPS_XMM_XMMM128, ANDNPD_XMM_XMMM128,
//      LDMXCSR_M32, STMXCSR_M32,
//
// 3. decode_ops.hpp — add declaration:
//      [[nodiscard]] DecodeError decode_sse_data(DecodeContext& ctx) noexcept;
//
// 4. two_byte.cpp — add cases in decode_two_byte switch (before default):
//      case 0x10: case 0x11: case 0x12: case 0x13:
//      case 0x16: case 0x17: case 0x28: case 0x29:
//      case 0x50: case 0x54: case 0x55: case 0x56:
//      case 0x57: case 0x6F: case 0x7F: case 0xAE:
//          return decode_sse_data(ctx);
//
// 5. src/CMakeLists.txt — add to vitreo_cpu_decode sources:
//      cpu/decode/opcodes/sse_data.cpp

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

namespace {

/// Decode ModR/M for a standard SSE instruction (xmm reg, xmm/mem).
/// Returns the xmm register operand and the r/m operand with corrected types.
DecodeError decode_sse_modrm(DecodeContext& ctx, Operand& xmm_reg, Operand& rm_op,
                              OperandType mem_type = OperandType::Mem128) noexcept {
    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm_op, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Override operand types for SSE context
    if (is_register_operand(rm_op.type)) {
        rm_op.type = OperandType::Reg128;
    } else if (is_memory_operand(rm_op.type)) {
        rm_op.type = mem_type;
    }

    xmm_reg = Operand::make_reg(OperandType::Reg128, reg_field);
    return DecodeError::Success;
}

} // anonymous namespace

DecodeError decode_sse_data(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool has_66 = ctx.prefix.operand_size;
    bool has_f3 = ctx.prefix.rep;
    bool has_f2 = ctx.prefix.repne;

    Operand xmm_reg, rm_op;

    switch (byte) {

    // =================================================================
    // 0x10: MOVUPS / MOVUPD / MOVSS / MOVSD  (load: xmm ← xmm/m)
    // =================================================================
    case 0x10: {
        if (has_f3) {
            // F3 0F 10 — MOVSS xmm, xmm/m32
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, OperandType::Mem32);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVSS_XMM_XMMM32;
        } else if (has_f2) {
            // F2 0F 10 — MOVSD xmm, xmm/m64
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, OperandType::Mem64);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVSD_XMM_XMMM64;
        } else if (has_66) {
            // 66 0F 10 — MOVUPD xmm, xmm/m128
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVUPD_XMM_XMMM128;
        } else {
            // 0F 10 — MOVUPS xmm, xmm/m128
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVUPS_XMM_XMMM128;
        }
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x11: MOVUPS / MOVUPD / MOVSS / MOVSD  (store: xmm/m ← xmm)
    // =================================================================
    case 0x11: {
        if (has_f3) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, OperandType::Mem32);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVSS_XMMM32_XMM;
        } else if (has_f2) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, OperandType::Mem64);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVSD_XMMM64_XMM;
        } else if (has_66) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVUPD_XMMM128_XMM;
        } else {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVUPS_XMMM128_XMM;
        }
        // Store: operand[0] = destination (rm), operand[1] = source (xmm)
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x12: MOVLPS / MOVLPD / MOVHLPS / MOVSLDUP (F3) / MOVDDUP (F2)
    // =================================================================
    case 0x12: {
        // Determine memory operand size based on prefix
        auto mem_size = has_f3 ? OperandType::Mem128
                               : has_f2 ? OperandType::Mem64
                               : OperandType::Mem64;
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, mem_size);
        if (err != DecodeError::Success) return err;
        if (has_f3) {
            // F3 0F 12: MOVSLDUP xmm, xmm/m128
            out.opcode = Opcode::MOVSLDUP_XMM_XMMM128;
        } else if (has_f2) {
            // F2 0F 12: MOVDDUP xmm, xmm/m64
            out.opcode = Opcode::MOVDDUP_XMM_XMMM64;
        } else if (rm_op.type == OperandType::Reg128) {
            // no prefix, mod=11: MOVHLPS xmm, xmm
            out.opcode = Opcode::MOVHLPS_XMM_XMM;
        } else {
            out.opcode = has_66 ? Opcode::MOVLPD_XMM_M64 : Opcode::MOVLPS_XMM_M64;
        }
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x13: MOVLPS / MOVLPD  (store low qword)
    // =================================================================
    case 0x13: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, OperandType::Mem64);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::MOVLPD_M64_XMM : Opcode::MOVLPS_M64_XMM;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x16: MOVHPS / MOVHPD / MOVLHPS / MOVSHDUP (F3)
    // =================================================================
    case 0x16: {
        auto mem_size = has_f3 ? OperandType::Mem128 : OperandType::Mem64;
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, mem_size);
        if (err != DecodeError::Success) return err;
        if (has_f3) {
            // F3 0F 16: MOVSHDUP xmm, xmm/m128
            out.opcode = Opcode::MOVSHDUP_XMM_XMMM128;
        } else if (rm_op.type == OperandType::Reg128) {
            // mod=11: MOVLHPS xmm, xmm (no prefix)
            out.opcode = Opcode::MOVLHPS_XMM_XMM;
        } else {
            out.opcode = has_66 ? Opcode::MOVHPD_XMM_M64 : Opcode::MOVHPS_XMM_M64;
        }
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x17: MOVHPS / MOVHPD  (store high qword)
    // =================================================================
    case 0x17: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op, OperandType::Mem64);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::MOVHPD_M64_XMM : Opcode::MOVHPS_M64_XMM;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x28: MOVAPS / MOVAPD  (aligned load: xmm ← xmm/m128)
    // =================================================================
    case 0x28: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::MOVAPD_XMM_XMMM128
                            : Opcode::MOVAPS_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x29: MOVAPS / MOVAPD  (aligned store: xmm/m128 ← xmm)
    // =================================================================
    case 0x29: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::MOVAPD_XMMM128_XMM
                            : Opcode::MOVAPS_XMMM128_XMM;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x2B: MOVNTPS / MOVNTPD  (aligned non-temporal store: m128 ← xmm)
    // Intel SDM Vol. 2: NP 0F 2B /r, 66 0F 2B /r. Destination is memory
    // only (ModRM.mod != 11b); #UD for register form. Hint is advisory —
    // architectural result matches MOVAPS/MOVAPD store.
    // =================================================================
    case 0x2B: {
        if (has_f3 || has_f2) {
            return DecodeError::InvalidOpcode;
        }
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm_op.type)) {
            return DecodeError::InvalidOpcode;
        }
        out.opcode = has_66 ? Opcode::MOVNTPD_M128_XMM
                            : Opcode::MOVNTPS_M128_XMM;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x50: MOVMSKPS / MOVMSKPD  (r32 ← sign-bit mask of xmm)
    // =================================================================
    case 0x50: {
        u8 modrm;
        if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
        u8 reg_field;
        Operand rm;
        if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm, reg_field) < 0) {
            return DecodeError::InvalidModRM;
        }
        // MOVMSKPS/PD requires register form (mod == 3)
        if (!is_register_operand(rm.type)) return DecodeError::InvalidOperand;
        out.opcode = has_66 ? Opcode::MOVMSKPD_R32_XMM
                            : Opcode::MOVMSKPS_R32_XMM;
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
        out.add_operand(Operand::make_reg(OperandType::Reg128, rm.reg));
        return DecodeError::Success;
    }

    // =================================================================
    // 0x54: ANDPS / ANDPD
    // =================================================================
    case 0x54: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::ANDPD_XMM_XMMM128
                            : Opcode::ANDPS_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x55: ANDNPS / ANDNPD
    // =================================================================
    case 0x55: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::ANDNPD_XMM_XMMM128
                            : Opcode::ANDNPS_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x56: ORPS / ORPD
    // =================================================================
    case 0x56: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::ORPD_XMM_XMMM128
                            : Opcode::ORPS_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x57: XORPS / XORPD
    // =================================================================
    case 0x57: {
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = has_66 ? Opcode::XORPD_XMM_XMMM128
                            : Opcode::XORPS_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x6F: MOVDQA / MOVDQU / MOVQ  (integer load)
    // =================================================================
    case 0x6F: {
        if (has_66) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVDQA_XMM_XMMM128;
        } else if (has_f3) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVDQU_XMM_XMMM128;
        } else {
            // No prefix: MMX MOVQ mm, mm/m64 — 64-bit move
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVQ_MM_MMM64;
            if (rm_op.type == OperandType::Mem128) rm_op.type = OperandType::Mem64;
            out.add_operand(Operand::reg_mm(xmm_reg.reg & 7));
            if (is_register_operand(rm_op.type)) {
                out.add_operand(Operand::reg_mm(rm_op.reg & 7));
            } else {
                out.add_operand(rm_op);
            }
            return DecodeError::Success;
        }
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0x7F: MOVDQA / MOVDQU / MOVQ  (integer store)
    // =================================================================
    case 0x7F: {
        if (has_66) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVDQA_XMMM128_XMM;
        } else if (has_f3) {
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVDQU_XMMM128_XMM;
        } else {
            // No prefix: MMX MOVQ mm/m64, mm — 64-bit store
            auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::MOVQ_MMM64_MM;
            if (rm_op.type == OperandType::Mem128) rm_op.type = OperandType::Mem64;
            if (is_register_operand(rm_op.type)) {
                out.add_operand(Operand::reg_mm(rm_op.reg & 7));
            } else {
                out.add_operand(rm_op);
            }
            out.add_operand(Operand::reg_mm(xmm_reg.reg & 7));
            return DecodeError::Success;
        }
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        return DecodeError::Success;
    }

    // =================================================================
    // 0xF0: LDDQU xmm, m128  (SSE3; F2 prefix required)
    // Intel SDM Vol. 2: F2 0F F0 /r — functionally same as MOVDQU load
    // from memory; #UD if source is a register (ModRM.mod == 11b).
    // =================================================================
    case 0xF0: {
        if (!has_f2 || has_f3 || has_66) {
            return DecodeError::InvalidOpcode;
        }
        auto err = decode_sse_modrm(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm_op.type)) {
            return DecodeError::InvalidOpcode;
        }
        out.opcode = Opcode::LDDQU_XMM_M128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0xAE: FXSAVE (/0), FXRSTOR (/1), LDMXCSR (/2), STMXCSR (/3),
    // CLFLUSH (/7 mem). Register /7 is SFENCE.
    // =================================================================
    case 0xAE: {
        u8 modrm;
        if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
        u8 reg_field;
        Operand rm;
        if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm, reg_field) < 0) {
            return DecodeError::InvalidModRM;
        }

        // Extension opcode from raw modrm reg field (without REX.R)
        u8 ext = (modrm >> 3) & 7;

        // Register form: the architectural fence encodings require r/m=000.
        if (is_register_operand(rm.type)) {
            if ((modrm & 7) != 0) return DecodeError::InvalidOperand;
            if (ext == 5) out.opcode = Opcode::LFENCE;
            else if (ext == 6) out.opcode = Opcode::MFENCE;
            else if (ext == 7) out.opcode = Opcode::SFENCE;
            else return DecodeError::InvalidOperand;
            return DecodeError::Success;
        }

        // Memory form
        if (ext == 0) {
            rm.type = OperandType::MemUnknown;
            out.opcode = Opcode::FXSAVE_M512;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        if (ext == 1) {
            rm.type = OperandType::MemUnknown;
            out.opcode = Opcode::FXRSTOR_M512;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        if (ext == 7) {
            rm.type = OperandType::Mem8;
            out.opcode = Opcode::CLFLUSH_M8;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        if (ext == 4) {
            rm.type = OperandType::MemUnknown;
            out.opcode = Opcode::XSAVE_M;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        if (ext == 5) {
            rm.type = OperandType::MemUnknown;
            out.opcode = Opcode::XRSTOR_M;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        if (ext == 6) {
            rm.type = OperandType::MemUnknown;
            out.opcode = Opcode::XSAVEOPT_M;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        rm.type = OperandType::Mem32;
        if (ext == 2) {
            out.opcode = Opcode::LDMXCSR_M32;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        if (ext == 3) {
            out.opcode = Opcode::STMXCSR_M32;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        return DecodeError::InvalidOpcode;
    }

    default:
        return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
