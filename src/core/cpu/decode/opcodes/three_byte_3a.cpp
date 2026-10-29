// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Three-byte opcode decoding: 0F 3A XX
// SSSE3 (PALIGNR), PCLMULQDQ, AES-NI (AESKEYGENASSIST), SHA (SHA1RNDS4)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

namespace {

/// Decode ModR/M for SSE instruction: xmm reg, xmm/m128
DecodeError decode_sse_modrm_3a(DecodeContext& ctx, Operand& xmm_reg, Operand& rm_op) noexcept {
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
DecodeError decode_mmx_modrm_3a(DecodeContext& ctx, Operand& mm_reg, Operand& rm_op) noexcept {
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

DecodeError decode_three_byte_3a(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool has_66 = ctx.prefix.operand_size;

    Operand xmm_reg, rm_op;

    switch (byte) {

    // =================================================================
    // SSE4.1 ROUND* — 66 0F 3A 08-0B /r ib
    // =================================================================
    case 0x08: case 0x09: case 0x0A: case 0x0B: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        static constexpr Opcode round_table[] = {
            Opcode::ROUNDPS_XMM_XMMM128_IMM8,
            Opcode::ROUNDPD_XMM_XMMM128_IMM8,
            Opcode::ROUNDSS_XMM_XMMM32_IMM8,
            Opcode::ROUNDSD_XMM_XMMM64_IMM8
        };
        out.opcode = round_table[byte - 0x08];
        if (byte == 0x0A) {
            if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem32;
        } else if (byte == 0x0B) {
            if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem64;
        }
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.1 BLEND* — 66 0F 3A 0C-0E /r ib
    // =================================================================
    case 0x0C: case 0x0D: case 0x0E: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        static constexpr Opcode blend_table[] = {
            Opcode::BLENDPS_XMM_XMMM128_IMM8,
            Opcode::BLENDPD_XMM_XMMM128_IMM8,
            Opcode::PBLENDW_XMM_XMMM128_IMM8
        };
        out.opcode = blend_table[byte - 0x0C];
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PALIGNR — NP 0F 3A 0F /r ib (MMX) or 66 0F 3A 0F /r ib (XMM). F2/F3 = #UD.
    // =================================================================
    case 0x0F: {
        if (ctx.prefix.rep || ctx.prefix.repne) return DecodeError::InvalidOpcode;
        u8 imm;
        if (has_66) {
            auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
            if (err != DecodeError::Success) return err;
            if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.opcode = Opcode::PALIGNR_XMM_XMMM128_IMM8;
            out.add_operand(xmm_reg);
            out.add_operand(rm_op);
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        Operand mm_reg;
        auto err = decode_mmx_modrm_3a(ctx, mm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::PALIGNR_MM_MMM64_IMM8;
        out.add_operand(mm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PEXTRB — 66 0F 3A 14 /r ib
    // =================================================================
    case 0x14: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (is_register_operand(rm_op.type)) rm_op.type = OperandType::Reg32;
        else if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem8;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::PEXTRB_RM8_XMM_IMM8;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PEXTRW — 66 0F 3A 15 /r ib (SSE4.1 form: extract word to r32/m16)
    // Register dest zero-extends to r32; memory dest writes 2 bytes.
    // =================================================================
    case 0x15: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (is_register_operand(rm_op.type)) {
            rm_op.type = OperandType::Reg32;
        } else if (is_memory_operand(rm_op.type)) {
            rm_op.type = OperandType::Mem16;
        }
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::PEXTRW_R32_XMM_IMM8;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PEXTRD — 66 0F 3A 16 /r ib
    // Extract dword from XMM at position imm8[1:0] → r/m32
    // REX.W promotes this to PEXTRQ, extracting a qword at imm8[0].
    // =================================================================
    case 0x16: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        // W selects the 64-bit GPR operand, which is an operand-size
        // promotion and so does not exist outside 64-bit mode — whether it
        // arrives as REX.W (impossible in 32-bit mode) or as VEX.W, which the
        // decoder folds into the same bit. `C4 E3 F9 16 C1 11` decoded as
        // VPEXTRQ r64 inside a 32-bit block. Same rule as the BMI families in
        // this map and map 2; see decode_vex_3a.
        const bool is_qword =
            ctx.prefix.rex_w() && ctx.mode == DecodeMode::Mode64;
        if (is_register_operand(rm_op.type)) {
            rm_op.type = is_qword ? OperandType::Reg64 : OperandType::Reg32;
        } else if (is_memory_operand(rm_op.type)) {
            rm_op.type = is_qword ? OperandType::Mem64 : OperandType::Mem32;
        }
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = is_qword ? Opcode::PEXTRQ_RM64_XMM_IMM8
                              : Opcode::PEXTRD_RM32_XMM_IMM8;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // EXTRACTPS — 66 0F 3A 17 /r ib
    // =================================================================
    case 0x17: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (is_register_operand(rm_op.type)) rm_op.type = OperandType::Reg32;
        else if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem32;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::EXTRACTPS_RM32_XMM_IMM8;
        out.add_operand(rm_op);
        out.add_operand(xmm_reg);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PINSRB — 66 0F 3A 20 /r ib
    // =================================================================
    case 0x20: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (is_register_operand(rm_op.type)) rm_op.type = OperandType::Reg32;
        else if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem8;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::PINSRB_XMM_RM8_IMM8;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // INSERTPS — 66 0F 3A 21 /r ib
    // =================================================================
    case 0x21: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        if (is_memory_operand(rm_op.type)) rm_op.type = OperandType::Mem32;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::INSERTPS_XMM_XMMM32_IMM8;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PINSRD — 66 0F 3A 22 /r ib
    // Insert dword from r/m32 into XMM at position imm8[1:0]
    // =================================================================
    case 0x22: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        // W selects the 64-bit GPR operand, which is an operand-size
        // promotion and so does not exist outside 64-bit mode — whether it
        // arrives as REX.W (impossible in 32-bit mode) or as VEX.W, which the
        // decoder folds into the same bit. `C4 E3 F9 16 C1 11` decoded as
        // VPEXTRQ r64 inside a 32-bit block. Same rule as the BMI families in
        // this map and map 2; see decode_vex_3a.
        const bool is_qword =
            ctx.prefix.rex_w() && ctx.mode == DecodeMode::Mode64;
        if (is_register_operand(rm_op.type)) {
            rm_op.type = is_qword ? OperandType::Reg64 : OperandType::Reg32;
        } else if (is_memory_operand(rm_op.type)) {
            rm_op.type = is_qword ? OperandType::Mem64 : OperandType::Mem32;
        }
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = is_qword ? Opcode::PINSRQ_XMM_RM64_IMM8
                              : Opcode::PINSRD_XMM_RM32_IMM8;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // DPPS/DPPD/MPSADBW — 66 0F 3A 40-42 /r ib
    // =================================================================
    case 0x40: case 0x41: case 0x42: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        static constexpr Opcode dp_table[] = {
            Opcode::DPPS_XMM_XMMM128_IMM8,
            Opcode::DPPD_XMM_XMMM128_IMM8,
            Opcode::MPSADBW_XMM_XMMM128_IMM8
        };
        out.opcode = dp_table[byte - 0x40];
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // PCLMULQDQ — 66 0F 3A 44 /r ib
    // =================================================================
    case 0x44: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::PCLMULQDQ_XMM_XMMM128_IMM8;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // SHA1RNDS4 — 0F 3A CC /r ib (no 66 prefix)
    // =================================================================
    case 0xCC: {
        if (has_66 || ctx.prefix.rep || ctx.prefix.repne)
            return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::SHA1RNDS4_XMM_XMMM128_IMM8;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // AESKEYGENASSIST — 66 0F 3A DF /r ib
    // =================================================================
    case 0xDF: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8;
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    // =================================================================
    // SSE4.2 string comparison — 66 0F 3A 60-63 /r ib
    // =================================================================
    case 0x60: case 0x61: case 0x62: case 0x63: {
        if (!has_66) return DecodeError::InvalidOpcode;
        auto err = decode_sse_modrm_3a(ctx, xmm_reg, rm_op);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        static constexpr Opcode str_table[] = {
            Opcode::PCMPESTRM_XMM_XMMM128_IMM8,
            Opcode::PCMPESTRI_XMM_XMMM128_IMM8,
            Opcode::PCMPISTRM_XMM_XMMM128_IMM8,
            Opcode::PCMPISTRI_XMM_XMMM128_IMM8
        };
        out.opcode = str_table[byte - 0x60];
        out.add_operand(xmm_reg);
        out.add_operand(rm_op);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    default:
        return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
