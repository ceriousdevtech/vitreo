// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Three-byte opcode decoding: 0F 38 XX
// SSSE3 (PSHUFB), SSE4.1 (PMULLD), AES-NI, SHA extensions

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

namespace {

/// Decode ModR/M for SSE instruction: xmm reg, xmm/m128
DecodeError decode_sse_modrm_38(DecodeContext& ctx, Operand& xmm_reg, Operand& rm_op) noexcept {
    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm_op, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (is_register_operand(rm_op.type)) {
        rm_op.type = OperandType::Reg128;
    } else if (is_memory_operand(rm_op.type)) {
        rm_op.type = OperandType::Mem128;
    }

    xmm_reg = Operand::make_reg(OperandType::Reg128, reg_field);
    return DecodeError::Success;
}

/// Decode ModR/M for SSSE3-on-MMX: mm0-7, mm/m64. REX.R would name MM8-15 (#UD).
DecodeError decode_mmx_modrm_38(DecodeContext& ctx, Operand& mm_reg, Operand& rm_op) noexcept {
    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm_op, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (ctx.prefix.rex_r()) return DecodeError::InvalidOpcode;

    if (is_register_operand(rm_op.type)) {
        rm_op.type = OperandType::RegMM;
        rm_op.reg &= 7;
    } else if (is_memory_operand(rm_op.type)) {
        rm_op.type = OperandType::Mem64;
    }

    mm_reg = Operand::reg_mm(reg_field & 7);
    return DecodeError::Success;
}

} // anonymous namespace

DecodeError decode_three_byte_38(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool has_66 = ctx.prefix.operand_size;

    Operand xmm_reg, rm_op;

    switch (byte) {

    // =================================================================
    // PSHUFB — NP 0F 38 00 /r (MMX) or 66 0F 38 00 /r (XMM). F2/F3 = #UD.
    // =================================================================
    case 0x00: {
        if (ctx.prefix.rep || ctx.prefix.repne) return DecodeError::InvalidOpcode;
        if (has_66) {
            auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = Opcode::PSHUFB_XMM_XMMM128;
            out.add_operand(xmm_reg);
            out.add_operand(rm_op);
            return DecodeError::Success;
        }
        Operand mm_reg;
        auto err = decode_mmx_modrm_38(ctx, mm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PSHUFB_MM_MMM64;
        out.add_operand(mm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSSE3 horizontal / multiply / sign — NP (MMX) or 66 (XMM) 0F 38 01..0B
    // =================================================================
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B: {
        if (ctx.prefix.rep || ctx.prefix.repne) return DecodeError::InvalidOpcode;
        static constexpr Opcode ssse3_xmm[] = {
            Opcode::PHADDW_XMM_XMMM128,    // 01
            Opcode::PHADDD_XMM_XMMM128,    // 02
            Opcode::PHADDSW_XMM_XMMM128,   // 03
            Opcode::PMADDUBSW_XMM_XMMM128, // 04
            Opcode::PHSUBW_XMM_XMMM128,    // 05
            Opcode::PHSUBD_XMM_XMMM128,    // 06
            Opcode::PHSUBSW_XMM_XMMM128,   // 07
            Opcode::PSIGNB_XMM_XMMM128,    // 08
            Opcode::PSIGNW_XMM_XMMM128,    // 09
            Opcode::PSIGND_XMM_XMMM128,    // 0A
            Opcode::PMULHRSW_XMM_XMMM128,  // 0B
        };
        static constexpr Opcode ssse3_mmx[] = {
            Opcode::PHADDW_MM_MMM64,
            Opcode::PHADDD_MM_MMM64,
            Opcode::PHADDSW_MM_MMM64,
            Opcode::PMADDUBSW_MM_MMM64,
            Opcode::PHSUBW_MM_MMM64,
            Opcode::PHSUBD_MM_MMM64,
            Opcode::PHSUBSW_MM_MMM64,
            Opcode::PSIGNB_MM_MMM64,
            Opcode::PSIGNW_MM_MMM64,
            Opcode::PSIGND_MM_MMM64,
            Opcode::PMULHRSW_MM_MMM64,
        };
        if (has_66) {
            auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = ssse3_xmm[byte - 0x01];
            out.add_operand(xmm_reg);
            out.add_operand(rm_op);
            return DecodeError::Success;
        }
        Operand mm_reg;
        auto err = decode_mmx_modrm_38(ctx, mm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = ssse3_mmx[byte - 0x01];
        out.add_operand(mm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSSE3 PABSB/PABSW/PABSD — NP (MMX) or 66 (XMM) 0F 38 1C..1E
    // =================================================================
    case 0x1C: case 0x1D: case 0x1E: {
        if (ctx.prefix.rep || ctx.prefix.repne) return DecodeError::InvalidOpcode;
        static constexpr Opcode pabs_xmm[] = {
            Opcode::PABSB_XMM_XMMM128, Opcode::PABSW_XMM_XMMM128, Opcode::PABSD_XMM_XMMM128
        };
        static constexpr Opcode pabs_mmx[] = {
            Opcode::PABSB_MM_MMM64, Opcode::PABSW_MM_MMM64, Opcode::PABSD_MM_MMM64
        };
        if (has_66) {
            auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            out.opcode = pabs_xmm[byte - 0x1C];
            out.add_operand(xmm_reg);
            out.add_operand(rm_op);
            return DecodeError::Success;
        }
        Operand mm_reg;
        auto err = decode_mmx_modrm_38(ctx, mm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = pabs_mmx[byte - 0x1C];
        out.add_operand(mm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // PMULDQ — 66 0F 38 28 /r
    // =================================================================
    case 0x28: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PMULDQ_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.1 — 66 0F 38 10/14/15 (variable blend, implicit XMM0)
    // =================================================================
    case 0x10: { // PBLENDVB
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PBLENDVB_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0x14: { // BLENDVPS
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::BLENDVPS_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0x15: { // BLENDVPD
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::BLENDVPD_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // PTEST — 66 0F 38 17 /r
    // =================================================================
    case 0x17: {
        if (!has_66) return DecodeError::InvalidOpcode;
        // Intel SDM Vol. 2B PTEST: encoded VEX.vvvv must be 1111b.
        // PrefixState holds its one's complement, hence zero here.
        if (ctx.prefix.vex && ctx.prefix.vex_vvvv != 0)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PTEST_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.1 sign-extend (PMOVSX*) — 66 0F 38 20-25
    // =================================================================
    case 0x20: case 0x21: case 0x22:
    case 0x23: case 0x24: case 0x25: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        static constexpr Opcode sx_table[] = {
            Opcode::PMOVSXBW_XMM_XMMM64, Opcode::PMOVSXBD_XMM_XMMM32,
            Opcode::PMOVSXBQ_XMM_XMMM16, Opcode::PMOVSXWD_XMM_XMMM64,
            Opcode::PMOVSXWQ_XMM_XMMM32, Opcode::PMOVSXDQ_XMM_XMMM64
        };
        // PMOVSX* read a narrow source (m64/m32/m16), not a full m128.
        // decode_sse_modrm_38 defaults memory operands to Mem128, so correct
        // the width here — a blanket 16-byte read faults on a source at the
        // tail of a mapped section.
        static constexpr OperandType sx_mem[] = {
            OperandType::Mem64, OperandType::Mem32, OperandType::Mem16,
            OperandType::Mem64, OperandType::Mem32, OperandType::Mem64
        };
        if (is_memory_operand(rm_op.type)) rm_op.type = sx_mem[byte - 0x20];
        out.opcode = sx_table[byte - 0x20];
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // PCMPEQQ — 66 0F 38 29
    // =================================================================
    case 0x29: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PCMPEQQ_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // MOVNTDQA — 66 0F 38 2A
    // =================================================================
    case 0x2A: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        // Intel SDM Vol. 2 MOVNTDQA xmm1, m128 / VMOVNTDQA: the source is
        // a memory operand only; ModRM.mod = 11 is #UD (silicon raises on
        // `66 0F 38 2A C1`, tools/oracle_probe/vex_scalar_round_probe.c).
        if (is_register_operand(rm_op.type)) return DecodeError::InvalidOpcode;
        out.opcode = Opcode::MOVNTDQA_XMM_M128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // PACKUSDW — 66 0F 38 2B
    // =================================================================
    case 0x2B: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PACKUSDW_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.1 zero-extend (PMOVZX*) — 66 0F 38 30-35
    // =================================================================
    case 0x30: case 0x31: case 0x32:
    case 0x33: case 0x34: case 0x35: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        static constexpr Opcode zx_table[] = {
            Opcode::PMOVZXBW_XMM_XMMM64, Opcode::PMOVZXBD_XMM_XMMM32,
            Opcode::PMOVZXBQ_XMM_XMMM16, Opcode::PMOVZXWD_XMM_XMMM64,
            Opcode::PMOVZXWQ_XMM_XMMM32, Opcode::PMOVZXDQ_XMM_XMMM64
        };
        // PMOVZX* read a narrow source (m64/m32/m16), not a full m128 — correct
        // the Mem128 default so read_pack does not over-read past the operand
        // (the observed AV was a pmovzxdq RIP-relative m64 constant load
        // whose 16-byte read crossed a section boundary).
        static constexpr OperandType zx_mem[] = {
            OperandType::Mem64, OperandType::Mem32, OperandType::Mem16,
            OperandType::Mem64, OperandType::Mem32, OperandType::Mem64
        };
        if (is_memory_operand(rm_op.type)) rm_op.type = zx_mem[byte - 0x30];
        out.opcode = zx_table[byte - 0x30];
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.2 — PCMPGTQ — 66 0F 38 37
    // =================================================================
    case 0x37: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PCMPGTQ_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.1 min/max — 66 0F 38 38-3F
    // =================================================================
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        static constexpr Opcode mm_table[] = {
            Opcode::PMINSB_XMM_XMMM128, Opcode::PMINSD_XMM_XMMM128,
            Opcode::PMINUW_XMM_XMMM128, Opcode::PMINUD_XMM_XMMM128,
            Opcode::PMAXSB_XMM_XMMM128, Opcode::PMAXSD_XMM_XMMM128,
            Opcode::PMAXUW_XMM_XMMM128, Opcode::PMAXUD_XMM_XMMM128
        };
        out.opcode = mm_table[byte - 0x38];
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // PMULLD — 66 0F 38 40 /r
    // =================================================================
    case 0x40: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PMULLD_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // PHMINPOSUW — 66 0F 38 41
    // =================================================================
    case 0x41: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::PHMINPOSUW_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // 0F 38 F0/F1 — CRC32 (F2 prefix) or MOVBE (no F2 prefix).
    //   F2 0F 38 F0 /r          : CRC32 r32, r/m8
    //   F2 REX.W 0F 38 F0 /r    : CRC32 r64, r/m8
    //   F2 0F 38 F1 /r          : CRC32 r32, r/m32
    //   66 F2 0F 38 F1 /r       : CRC32 r32, r/m16
    //   F2 REX.W 0F 38 F1 /r    : CRC32 r64, r/m64
    //       0F 38 F0 /r         : MOVBE r32, m32
    //    66 0F 38 F0 /r         : MOVBE r16, m16
    //       0F 38 F1 /r         : MOVBE m32, r32
    //    66 0F 38 F1 /r         : MOVBE m16, r16
    // =================================================================
    case 0xF0: case 0xF1: {
        const bool is_f1 = (byte == 0xF1);
        if (ctx.prefix.repne) {
            // ---- CRC32 ----
            auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            xmm_reg.type = OperandType::Reg32;
            if (!is_f1) {
                // F0 — r/m8 source (66 prefix has no effect on r/m8 form).
                if (is_register_operand(rm_op.type)) rm_op.type = OperandType::Reg8;
                else if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem8;
                if (ctx.prefix.rex_w()) {
                    xmm_reg.type = OperandType::Reg64;
                    out.opcode = Opcode::CRC32_R64_RM8;
                } else {
                    out.opcode = Opcode::CRC32_R32_RM8;
                }
            } else if (ctx.prefix.rex_w()) {
                xmm_reg.type = OperandType::Reg64;
                if (is_register_operand(rm_op.type)) {
                    rm_op.type = OperandType::Reg64;
                } else if (is_memory_operand(rm_op.type)) {
                    rm_op.type = OperandType::Mem64;
                }
                out.opcode = Opcode::CRC32_R64_RM64;
            } else if (has_66) {
                // 66 F2 — r/m16 source.
                if (is_register_operand(rm_op.type)) rm_op.type = OperandType::Reg16;
                else if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem16;
                out.opcode = Opcode::CRC32_R32_RM16;
            } else {
                if (is_register_operand(rm_op.type)) rm_op.type = OperandType::Reg32;
                else if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem32;
                out.opcode = Opcode::CRC32_R32_RM32;
            }
            out.add_operand(xmm_reg);
            out.add_operand(rm_op);
            return DecodeError::Success;
        }

        // ---- MOVBE ---- (memory operand required; register form is #UD)
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm_op.type)) return DecodeError::InvalidOpcode;
        const bool qword = ctx.prefix.rex_w();
        const bool word = has_66 && !qword;
        OperandType reg_t = qword ? OperandType::Reg64
            : (word ? OperandType::Reg16 : OperandType::Reg32);
        OperandType mem_t = qword ? OperandType::Mem64
            : (word ? OperandType::Mem16 : OperandType::Mem32);
        xmm_reg.type = reg_t;
        rm_op.type = mem_t;
        if (!is_f1) {
            out.opcode = qword ? Opcode::MOVBE_R64_M64
                : (word ? Opcode::MOVBE_R16_M16 : Opcode::MOVBE_R32_M32);
            out.add_operand(xmm_reg);
            out.add_operand(rm_op);
        } else {
            out.opcode = qword ? Opcode::MOVBE_M64_R64
                : (word ? Opcode::MOVBE_M16_R16 : Opcode::MOVBE_M32_R32);
            out.add_operand(rm_op);
            out.add_operand(xmm_reg);
        }
        return DecodeError::Success;
    }

    // ADCX (66 0F 38 F6) / ADOX (F3 0F 38 F6) — Intel SDM Vol. 2 ADX
    case 0xF6: {
        if (ctx.prefix.vex) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        const bool qword = ctx.prefix.rex_w();
        xmm_reg.type = qword ? OperandType::Reg64 : OperandType::Reg32;
        if (is_register_operand(rm_op.type)) {
            rm_op.type = qword ? OperandType::Reg64 : OperandType::Reg32;
        } else if (is_memory_operand(rm_op.type)) {
            rm_op.type = qword ? OperandType::Mem64 : OperandType::Mem32;
        }
        if (has_66 && !ctx.prefix.rep && !ctx.prefix.repne) {
            out.opcode = qword ? Opcode::ADCX_R64_RM64 : Opcode::ADCX_R32_RM32;
        } else if (ctx.prefix.rep && !has_66 && !ctx.prefix.repne) {
            out.opcode = qword ? Opcode::ADOX_R64_RM64 : Opcode::ADOX_R32_RM32;
        } else {
            return DecodeError::InvalidOpcode;
        }
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // SHA extensions — no 66 prefix
    // =================================================================
    case 0xC8: { // SHA1NEXTE
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::SHA1NEXTE_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xC9: { // SHA1MSG1
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::SHA1MSG1_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xCA: { // SHA1MSG2
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::SHA1MSG2_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xCB: { // SHA256RNDS2 (implicit XMM0)
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::SHA256RNDS2_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xCC: { // SHA256MSG1
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::SHA256MSG1_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xCD: { // SHA256MSG2
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::SHA256MSG2_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    // =================================================================
    // AES-NI — 66 prefix required
    // =================================================================
    case 0xDB: { // AESIMC
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::AESIMC_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xDC: { // AESENC
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::AESENC_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xDD: { // AESENCLAST
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::AESENCLAST_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xDE: { // AESDEC
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::AESDEC_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }
    case 0xDF: { // AESDECLAST
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_38(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::AESDECLAST_XMM_XMMM128;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        return DecodeError::Success;
    }

    default:
        return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
