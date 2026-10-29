// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// VEX.0F38 / VEX.0F3A maps: BMI, FMA, AVX2 unique ops, F16C (Intel SDM Vol. 2)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

namespace {

DecodeError decode_gpr_modrm(DecodeContext& ctx, Operand& dest, Operand& rm,
                             bool is64) noexcept {
    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, is64, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }
    dest = Operand::make_reg(is64 ? OperandType::Reg64 : OperandType::Reg32, reg_field);
    if (is_register_operand(rm.type)) {
        rm.type = is64 ? OperandType::Reg64 : OperandType::Reg32;
    } else if (is_memory_operand(rm.type)) {
        rm.type = is64 ? OperandType::Mem64 : OperandType::Mem32;
    }
    return DecodeError::Success;
}

DecodeError decode_xmm_modrm(DecodeContext& ctx, Operand& xmm, Operand& rm,
                             OperandType mem_ty = OperandType::Mem128) noexcept {
    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }
    xmm = Operand::make_reg(OperandType::Reg128, reg_field);
    if (is_register_operand(rm.type)) {
        rm.type = OperandType::Reg128;
    } else if (is_memory_operand(rm.type)) {
        rm.type = mem_ty;
        if (ctx.prefix.vex_l && mem_ty == OperandType::Mem128) {
            rm.type = OperandType::Mem256;
        }
    }
    return DecodeError::Success;
}

DecodeError decode_vsib_modrm(DecodeContext& ctx, Operand& xmm, Operand& rm) noexcept {
    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false, modrm, rm, reg_field,
                     true) < 0) {
        return DecodeError::InvalidModRM;
    }
    if (!is_memory_operand(rm.type) || rm.mem.index == 255) {
        return DecodeError::InvalidOpcode;
    }
    rm.type = ctx.prefix.vex_l ? OperandType::Mem256 : OperandType::Mem128;
    xmm = Operand::make_reg(OperandType::Reg128, reg_field);
    return DecodeError::Success;
}

}  // namespace

DecodeError decode_vex_38(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    const u8 byte = ctx.opcode_byte;
    // VEX.W on the BMI1/BMI2 GPR instructions is an operand-size promotion,
    // exactly like REX.W — and like REX.W it does not exist outside 64-bit
    // mode (Intel SDM Vol. 2A §2.3.5.1: in non-64-bit modes VEX.W is ignored
    // where it would select a 64-bit operand).
    //
    // Honouring it in a 32-bit block made `C4 E2 F8 F5 C1` decode as
    // BZHI r64 there, so a 32-bit guest got 64-bit semantics from the
    // interpreter while the JIT declined the form entirely. Found by the ISA
    // matrix sweep.
    //
    // This is deliberately NOT a blanket clear of VEX.W: on the gathers
    // (0F38 90-93) and the FMA family (0F38 96-BF) W selects ELEMENT size, not
    // operand size, and those forms are valid in 32-bit mode. Only the
    // GPR instructions below consult `is64`.
    const bool is64 = ctx.prefix.rex_w();
    const bool gpr64 = is64 && ctx.mode == DecodeMode::Mode64;
    const bool l = ctx.prefix.vex_l;
    const bool p66 = ctx.prefix.operand_size;
    const bool f3 = ctx.prefix.rep;
    const bool f2 = ctx.prefix.repne;
    Operand dest, rm;

    // BMI1/BMI2 (no 66/F2/F3 unless noted)
    if (byte == 0xF2 && !p66 && !f3 && !f2) {  // ANDN
        auto err = decode_gpr_modrm(ctx, dest, rm, gpr64);
        if (err != DecodeError::Success) return err;
        out.opcode = gpr64 ? Opcode::ANDN_R64_R64_RM64 : Opcode::ANDN_R32_R32_RM32;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0xF3 && !p66 && !f3 && !f2) {  // BLSR /1, BLSMSK /2, BLSI /3
        // Intel SDM Vol. 2: VEX.LZ.0F38.W* F3 /1 BLSR, /2 BLSMSK, /3 BLSI.
        u8 modrm;
        if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
        u8 reg_field;
        if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, gpr64, modrm, rm, reg_field) < 0) {
            return DecodeError::InvalidModRM;
        }
        const u8 ext = (modrm >> 3) & 7;
        if (is_register_operand(rm.type)) {
            rm.type = gpr64 ? OperandType::Reg64 : OperandType::Reg32;
        } else if (is_memory_operand(rm.type)) {
            rm.type = gpr64 ? OperandType::Mem64 : OperandType::Mem32;
        }
        if (ext == 1) out.opcode = gpr64 ? Opcode::BLSR_R64_RM64 : Opcode::BLSR_R32_RM32;
        else if (ext == 2) out.opcode = gpr64 ? Opcode::BLSMSK_R64_RM64 : Opcode::BLSMSK_R32_RM32;
        else if (ext == 3) out.opcode = gpr64 ? Opcode::BLSI_R64_RM64 : Opcode::BLSI_R32_RM32;
        else return DecodeError::InvalidOpcode;
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0xF5) {
        auto err = decode_gpr_modrm(ctx, dest, rm, gpr64);
        if (err != DecodeError::Success) return err;
        if (!p66 && !f3 && !f2) {
            out.opcode = gpr64 ? Opcode::BZHI_R64_RM64_R64 : Opcode::BZHI_R32_RM32_R32;
        } else if (f2 && !p66 && !f3) {
            out.opcode = gpr64 ? Opcode::PDEP_R64_R64_RM64 : Opcode::PDEP_R32_R32_RM32;
        } else if (f3 && !p66 && !f2) {
            out.opcode = gpr64 ? Opcode::PEXT_R64_R64_RM64 : Opcode::PEXT_R32_R32_RM32;
        } else {
            return DecodeError::InvalidOpcode;
        }
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0xF6 && f2 && !p66 && !f3) {  // MULX
        auto err = decode_gpr_modrm(ctx, dest, rm, gpr64);
        if (err != DecodeError::Success) return err;
        out.opcode = gpr64 ? Opcode::MULX_R64_R64_RM64 : Opcode::MULX_R32_R32_RM32;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0xF7) {
        auto err = decode_gpr_modrm(ctx, dest, rm, gpr64);
        if (err != DecodeError::Success) return err;
        if (!p66 && !f3 && !f2) {
            out.opcode = gpr64 ? Opcode::BEXTR_R64_RM64_R64 : Opcode::BEXTR_R32_RM32_R32;
        } else if (p66 && !f3 && !f2) {
            out.opcode = gpr64 ? Opcode::SHLX_R64_RM64_R64 : Opcode::SHLX_R32_RM32_R32;
        } else if (f3 && !p66 && !f2) {
            // Intel SDM Vol. 2: SARX is VEX.LZ.**F3**.0F38 F7, SHRX is
            // VEX.LZ.**F2**.0F38 F7. These two were swapped here, so every
            // guest SARX did a logical shift and every SHRX an arithmetic one.
            // Confirmed on real x86 silicon: with pp=F3, 0x80000000 >> 1
            // yields 0xC0000000 (arithmetic), and with pp=F2, 0x40000000.
            out.opcode = gpr64 ? Opcode::SARX_R64_RM64_R64 : Opcode::SARX_R32_RM32_R32;
        } else if (f2 && !p66 && !f3) {
            out.opcode = gpr64 ? Opcode::SHRX_R64_RM64_R64 : Opcode::SHRX_R32_RM32_R32;
        } else {
            return DecodeError::InvalidOpcode;
        }
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // F16C VCVTPH2PS — VEX.66.0F38.W0 13
    if (byte == 0x13 && p66 && !f3 && !f2 && !is64) {
        auto err = decode_xmm_modrm(ctx, dest, rm, l ? OperandType::Mem128 : OperandType::Mem64);
        if (err != DecodeError::Success) return err;
        out.opcode = l ? Opcode::VCVTPH2PS_YMM_XMMM128 : Opcode::VCVTPH2PS_XMM_XMMM64;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // AVX broadcasts — VEX.66.0F38 18/19/1A
    if (byte == 0x18 && p66 && !f3 && !f2) {  // VBROADCASTSS
        auto err = decode_xmm_modrm(ctx, dest, rm, OperandType::Mem32);
        if (err != DecodeError::Success) return err;
        if (is_memory_operand(rm.type)) {
            out.opcode = l ? Opcode::VBROADCASTSS_YMM_M32 : Opcode::VBROADCASTSS_XMM_M32;
        } else {
            out.opcode = l ? Opcode::VBROADCASTSS_YMM_XMM : Opcode::VBROADCASTSS_XMM_XMM;
        }
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0x19 && p66 && !f3 && !f2 && l) {  // VBROADCASTSD ymm
        auto err = decode_xmm_modrm(ctx, dest, rm, OperandType::Mem64);
        if (err != DecodeError::Success) return err;
        out.opcode = is_memory_operand(rm.type) ? Opcode::VBROADCASTSD_YMM_M64
                                                : Opcode::VBROADCASTSD_YMM_XMM;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0x1A && p66 && !f3 && !f2 && l) {  // VBROADCASTF128
        auto err = decode_xmm_modrm(ctx, dest, rm, OperandType::Mem128);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm.type)) return DecodeError::InvalidOpcode;
        out.opcode = Opcode::VBROADCASTF128_YMM_M128;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0x5A && p66 && !f3 && !f2 && l) {  // VBROADCASTI128
        auto err = decode_xmm_modrm(ctx, dest, rm, OperandType::Mem128);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm.type)) return DecodeError::InvalidOpcode;
        out.opcode = Opcode::VBROADCASTI128_YMM_M128;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // AVX2 VPBROADCAST*
    if (p66 && !f3 && !f2) {
        Opcode pb = Opcode::INVALID;
        OperandType mty = OperandType::Mem8;
        switch (byte) {
            case 0x78: pb = l ? Opcode::VPBROADCASTB_YMM_XMMM8 : Opcode::VPBROADCASTB_XMM_XMMM8; mty = OperandType::Mem8; break;
            case 0x79: pb = l ? Opcode::VPBROADCASTW_YMM_XMMM16 : Opcode::VPBROADCASTW_XMM_XMMM16; mty = OperandType::Mem16; break;
            case 0x58: pb = l ? Opcode::VPBROADCASTD_YMM_XMMM32 : Opcode::VPBROADCASTD_XMM_XMMM32; mty = OperandType::Mem32; break;
            case 0x59: pb = l ? Opcode::VPBROADCASTQ_YMM_XMMM64 : Opcode::VPBROADCASTQ_XMM_XMMM64; mty = OperandType::Mem64; break;
            default: break;
        }
        if (pb != Opcode::INVALID) {
            auto err = decode_xmm_modrm(ctx, dest, rm, mty);
            if (err != DecodeError::Success) return err;
            out.opcode = pb;
            out.add_operand(dest);
            out.add_operand(rm);
            return DecodeError::Success;
        }
    }

    // VPERMD / VPERMPS
    if (byte == 0x36 && p66 && !f3 && !f2 && l) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::VPERMD_YMM_YMM_YMMM256;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }
    if (byte == 0x16 && p66 && !f3 && !f2 && l) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = Opcode::VPERMPS_YMM_YMM_YMMM256;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // Variable shifts AVX2
    if (p66 && !f3 && !f2) {
        Opcode vs = Opcode::INVALID;
        switch (byte) {
            case 0x47: vs = l ? Opcode::VPSLLVD_YMM_YMM_YMMM256 : Opcode::VPSLLVD_XMM_XMM_XMMM128; break;
            case 0x45: vs = l ? Opcode::VPSRLVD_YMM_YMM_YMMM256 : Opcode::VPSRLVD_XMM_XMM_XMMM128; break;
            case 0x46: vs = l ? Opcode::VPSRAVD_YMM_YMM_YMMM256 : Opcode::VPSRAVD_XMM_XMM_XMMM128; break;
            default: break;
        }
        if (byte == 0x47 && is64) {
            vs = l ? Opcode::VPSLLVQ_YMM_YMM_YMMM256 : Opcode::VPSLLVQ_XMM_XMM_XMMM128;
        }
        if (byte == 0x45 && is64) {
            vs = l ? Opcode::VPSRLVQ_YMM_YMM_YMMM256 : Opcode::VPSRLVQ_XMM_XMM_XMMM128;
        }
        if (vs != Opcode::INVALID && (byte == 0x47 || byte == 0x45 || byte == 0x46)) {
            auto err = decode_xmm_modrm(ctx, dest, rm);
            if (err != DecodeError::Success) return err;
            out.opcode = vs;
            out.add_operand(dest);
            out.add_operand(rm);
            return DecodeError::Success;
        }
    }

    // VPERMILPS/PD, variable control — VEX.66.0F38.W0 0C /r, 0D /r.
    // Control vector is the r/m operand, source is VEX.vvvv (Intel SDM Vol. 2
    // VPERMILPS: "DEST := permute(SRC1) using SRC2 as the control").
    if ((byte == 0x0C || byte == 0x0D) && p66 && !f3 && !f2) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = (byte == 0x0C) ? Opcode::VPERMILPS_VAR : Opcode::VPERMILPD_VAR;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // VTESTPS / VTESTPD — VEX.66.0F38.W0 0E /r, 0F /r. Two-operand; writes
    // ZF and CF from element sign bits (Intel SDM Vol. 2 VTESTPS).
    if ((byte == 0x0E || byte == 0x0F) && p66 && !f3 && !f2) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = (byte == 0x0E) ? Opcode::VTESTPS : Opcode::VTESTPD;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // VMASKMOVPS / VMASKMOVPD — VEX.66.0F38.W0 2C/2D (load), 2E/2F (store).
    // Mask is VEX.vvvv; the r/m operand must be memory, mirroring VPMASKMOV*.
    if (byte >= 0x2C && byte <= 0x2F && p66 && !f3 && !f2) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm.type)) return DecodeError::InvalidOpcode;
        const bool store = byte >= 0x2E;
        const bool pd = (byte & 1) != 0;
        if (store) {
            out.opcode = pd ? Opcode::VMASKMOVPD_STORE : Opcode::VMASKMOVPS_STORE;
            out.add_operand(rm);
            out.add_operand(dest);
        } else {
            out.opcode = pd ? Opcode::VMASKMOVPD_LOAD : Opcode::VMASKMOVPS_LOAD;
            out.add_operand(dest);
            out.add_operand(rm);
        }
        return DecodeError::Success;
    }

    // Gathers — VEX.66.0F38 90-93 /r. The opcode byte selects the element
    // width (90/91 integer, 92/93 float) and the index width (90/92 dword
    // indices, 91/93 qword indices); VEX.W selects the element size
    // (W0 dword, W1 qword). Intel SDM Vol. 2 VGATHERDPS/VGATHERQPS.
    if (byte >= 0x90 && byte <= 0x93 && p66 && !f3 && !f2) {
        auto err = decode_vsib_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        const bool qword_index = (byte & 1) != 0;
        const bool fp = byte >= 0x92;
        if (!is64) {
            // W0 — dword elements.
            if (!qword_index) {
                out.opcode = fp
                    ? (l ? Opcode::VGATHERDPS_YMM_VM32Y_YMM : Opcode::VGATHERDPS_XMM_VM32X_XMM)
                    : (l ? Opcode::VPGATHERDD_YMM_VM32Y_YMM : Opcode::VPGATHERDD_XMM_VM32X_XMM);
            } else {
                out.opcode = fp ? Opcode::VGATHERQPS : Opcode::VPGATHERQD;
            }
        } else {
            // W1 — qword elements.
            if (!qword_index) {
                out.opcode = fp ? Opcode::VGATHERDPD : Opcode::VPGATHERDQ;
            } else {
                out.opcode = fp ? Opcode::VGATHERQPD : Opcode::VPGATHERQQ;
            }
        }
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // VPMASKMOVD/Q — VEX.66.0F38.W0/W1 8C (load) / 8E (store). Memory
    // operand only; mask is VEX.vvvv (Intel SDM Vol. 2 VPMASKMOV).
    if ((byte == 0x8C || byte == 0x8E) && p66 && !f3 && !f2) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        if (!is_memory_operand(rm.type)) return DecodeError::InvalidOpcode;
        const bool store = byte == 0x8E;
        if (is64) {
            out.opcode = store ? Opcode::VPMASKMOVQ_STORE : Opcode::VPMASKMOVQ_LOAD;
        } else {
            out.opcode = store ? Opcode::VPMASKMOVD_STORE : Opcode::VPMASKMOVD_LOAD;
        }
        if (store) {
            out.add_operand(rm);
            out.add_operand(dest);
        } else {
            out.add_operand(dest);
            out.add_operand(rm);
        }
        return DecodeError::Success;
    }

    // FMA3 — VEX.66.0F38 96-BF
    if (p66 && !f3 && !f2 && byte >= 0x96 && byte <= 0xBF) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        const bool pd = is64;
        Opcode op = Opcode::INVALID;
        switch (byte) {
            case 0x98: op = pd ? Opcode::VFMADD132PD_XMM_XMM_XMMM128 : Opcode::VFMADD132PS_XMM_XMM_XMMM128; break;
            case 0x99: op = pd ? Opcode::VFMADD132SD_XMM_XMM_XMMM64 : Opcode::VFMADD132SS_XMM_XMM_XMMM32; break;
            case 0xA8: op = pd ? Opcode::VFMADD213PD_XMM_XMM_XMMM128 : Opcode::VFMADD213PS_XMM_XMM_XMMM128; break;
            case 0xA9: op = pd ? Opcode::VFMADD213SD_XMM_XMM_XMMM64 : Opcode::VFMADD213SS_XMM_XMM_XMMM32; break;
            case 0xB8: op = pd ? Opcode::VFMADD231PD_XMM_XMM_XMMM128 : Opcode::VFMADD231PS_XMM_XMM_XMMM128; break;
            case 0xB9: op = pd ? Opcode::VFMADD231SD_XMM_XMM_XMMM64 : Opcode::VFMADD231SS_XMM_XMM_XMMM32; break;
            case 0x9A: op = pd ? Opcode::VFMSUB132PD_XMM_XMM_XMMM128 : Opcode::VFMSUB132PS_XMM_XMM_XMMM128; break;
            case 0x9B: op = pd ? Opcode::VFMSUB132SD_XMM_XMM_XMMM64 : Opcode::VFMSUB132SS_XMM_XMM_XMMM32; break;
            case 0xAA: op = pd ? Opcode::VFMSUB213PD_XMM_XMM_XMMM128 : Opcode::VFMSUB213PS_XMM_XMM_XMMM128; break;
            case 0xAB: op = pd ? Opcode::VFMSUB213SD_XMM_XMM_XMMM64 : Opcode::VFMSUB213SS_XMM_XMM_XMMM32; break;
            case 0xBA: op = pd ? Opcode::VFMSUB231PD_XMM_XMM_XMMM128 : Opcode::VFMSUB231PS_XMM_XMM_XMMM128; break;
            case 0xBB: op = pd ? Opcode::VFMSUB231SD_XMM_XMM_XMMM64 : Opcode::VFMSUB231SS_XMM_XMM_XMMM32; break;
            case 0x9C: op = pd ? Opcode::VFNMADD132PD_XMM_XMM_XMMM128 : Opcode::VFNMADD132PS_XMM_XMM_XMMM128; break;
            case 0x9D: op = pd ? Opcode::VFNMADD132SD_XMM_XMM_XMMM64 : Opcode::VFNMADD132SS_XMM_XMM_XMMM32; break;
            case 0xAC: op = pd ? Opcode::VFNMADD213PD_XMM_XMM_XMMM128 : Opcode::VFNMADD213PS_XMM_XMM_XMMM128; break;
            case 0xAD: op = pd ? Opcode::VFNMADD213SD_XMM_XMM_XMMM64 : Opcode::VFNMADD213SS_XMM_XMM_XMMM32; break;
            case 0xBC: op = pd ? Opcode::VFNMADD231PD_XMM_XMM_XMMM128 : Opcode::VFNMADD231PS_XMM_XMM_XMMM128; break;
            case 0xBD: op = pd ? Opcode::VFNMADD231SD_XMM_XMM_XMMM64 : Opcode::VFNMADD231SS_XMM_XMM_XMMM32; break;
            case 0x9E: op = pd ? Opcode::VFNMSUB132PD_XMM_XMM_XMMM128 : Opcode::VFNMSUB132PS_XMM_XMM_XMMM128; break;
            case 0x9F: op = pd ? Opcode::VFNMSUB132SD_XMM_XMM_XMMM64 : Opcode::VFNMSUB132SS_XMM_XMM_XMMM32; break;
            case 0xAE: op = pd ? Opcode::VFNMSUB213PD_XMM_XMM_XMMM128 : Opcode::VFNMSUB213PS_XMM_XMM_XMMM128; break;
            case 0xAF: op = pd ? Opcode::VFNMSUB213SD_XMM_XMM_XMMM64 : Opcode::VFNMSUB213SS_XMM_XMM_XMMM32; break;
            case 0xBE: op = pd ? Opcode::VFNMSUB231PD_XMM_XMM_XMMM128 : Opcode::VFNMSUB231PS_XMM_XMM_XMMM128; break;
            case 0xBF: op = pd ? Opcode::VFNMSUB231SD_XMM_XMM_XMMM64 : Opcode::VFNMSUB231SS_XMM_XMM_XMMM32; break;
            case 0x96: op = pd ? Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128 : Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128; break;
            case 0xA6: op = pd ? Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128 : Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128; break;
            case 0xB6: op = pd ? Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128 : Opcode::VFMADDSUB231PS_XMM_XMM_XMMM128; break;
            case 0x97: op = pd ? Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128 : Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128; break;
            case 0xA7: op = pd ? Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128 : Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128; break;
            case 0xB7: op = pd ? Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128 : Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128; break;
            default: break;
        }
        if (op == Opcode::INVALID) return DecodeError::InvalidOpcode;
        out.opcode = op;
        out.add_operand(dest);
        out.add_operand(rm);
        return DecodeError::Success;
    }

    return DecodeError::InvalidOpcode;
}

DecodeError decode_vex_3a(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    const u8 byte = ctx.opcode_byte;
    const bool is64 = ctx.prefix.rex_w();
    // RORX is the one GPR instruction in this map: VEX.W there is an
    // operand-size promotion and does not exist outside 64-bit mode. The AVX
    // arms below keep plain `is64`, where W selects element size. See the
    // twin comment in decode_vex_38.
    const bool gpr64 = is64 && ctx.mode == DecodeMode::Mode64;
    const bool l = ctx.prefix.vex_l;
    const bool p66 = ctx.prefix.operand_size;
    const bool f2 = ctx.prefix.repne;
    Operand dest, rm;

    // RORX — VEX.LZ.F2.0F3A F0 /r ib
    if (byte == 0xF0 && f2 && !p66) {
        auto err = decode_gpr_modrm(ctx, dest, rm, gpr64);
        if (err != DecodeError::Success) return err;
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.opcode = gpr64 ? Opcode::RORX_R64_RM64_IMM8
                           : Opcode::RORX_R32_RM32_IMM8;
        out.add_operand(dest);
        out.add_operand(rm);
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    }

    if (!p66) return DecodeError::InvalidOpcode;

    auto read_imm = [&]() -> DecodeError {
        u8 imm;
        if (!ctx.reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
        out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
        return DecodeError::Success;
    };

    // VPERMILPS/PD, immediate control — VEX.66.0F3A.W0 04 /r ib, 05 /r ib.
    // Two operands plus imm8; VEX.vvvv must be 1111b (Intel SDM Vol. 2).
    if (byte == 0x04 || byte == 0x05) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = (byte == 0x04) ? Opcode::VPERMILPS_IMM : Opcode::VPERMILPD_IMM;
        out.add_operand(dest);
        out.add_operand(rm);
        return read_imm();
    }

    // VEXTRACTF128 / VEXTRACTI128 — VEX.256.66.0F3A.W0 19 / 39 /r ib.
    // Intel SDM Vol. 2: #UD if VEX.L = 0 (falls through to InvalidOpcode
    // below) or if encoded VEX.vvvv != 1111b, which PrefixState holds as 0.
    if ((byte == 0x19 || byte == 0x39) && l) {
        if (ctx.prefix.vex_vvvv != 0) return DecodeError::InvalidOpcode;
        auto err = decode_xmm_modrm(ctx, dest, rm, OperandType::Mem128);
        if (err != DecodeError::Success) return err;
        // dest is ymm (reg), rm is xmm/m128 — Intel encoding is dest=r/m, src=ymm
        out.opcode = (byte == 0x19) ? Opcode::VEXTRACTF128_XMMM128_YMM_IMM8
                                    : Opcode::VEXTRACTI128_XMMM128_YMM_IMM8;
        out.add_operand(rm);
        out.add_operand(dest);
        return read_imm();
    }
    // VINSERTF128 / VINSERTI128 — 18 / 38
    if ((byte == 0x18 || byte == 0x38) && l) {
        auto err = decode_xmm_modrm(ctx, dest, rm, OperandType::Mem128);
        if (err != DecodeError::Success) return err;
        out.opcode = (byte == 0x18) ? Opcode::VINSERTF128_YMM_YMM_XMMM128_IMM8
                                    : Opcode::VINSERTI128_YMM_YMM_XMMM128_IMM8;
        out.add_operand(dest);
        out.add_operand(rm);
        return read_imm();
    }
    // VPERM2F128 / VPERM2I128 — 06 / 46
    if ((byte == 0x06 || byte == 0x46) && l) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = (byte == 0x06) ? Opcode::VPERM2F128_YMM_YMM_YMMM256_IMM8
                                    : Opcode::VPERM2I128_YMM_YMM_YMMM256_IMM8;
        out.add_operand(dest);
        out.add_operand(rm);
        return read_imm();
    }
    // VPERMQ / VPERMPD — 00 / 01 W1 L1
    if ((byte == 0x00 || byte == 0x01) && l && is64) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = (byte == 0x00) ? Opcode::VPERMQ_YMM_YMMM256_IMM8
                                    : Opcode::VPERMPD_YMM_YMMM256_IMM8;
        out.add_operand(dest);
        out.add_operand(rm);
        return read_imm();
    }
    // VPBLENDD — 02
    if (byte == 0x02) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        out.opcode = l ? Opcode::VPBLENDD_YMM_YMM_YMMM256_IMM8
                       : Opcode::VPBLENDD_XMM_XMM_XMMM128_IMM8;
        out.add_operand(dest);
        out.add_operand(rm);
        return read_imm();
    }
    // VPBLENDVB — 4C /r /is4 (mask = imm8[7:4]; VEX.W must be 0)
    if (byte == 0x4C && !is64) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        u8 is4;
        if (!ctx.reader.read_u8(is4)) return DecodeError::UnexpectedEnd;
        out.opcode = l ? Opcode::VPBLENDVB_YMM_YMM_YMMM256_YMM
                       : Opcode::VPBLENDVB_XMM_XMM_XMMM128_XMM;
        out.add_operand(dest);
        out.add_operand(rm);
        // Store mask register index as Imm8 (high nibble); bits[3:0] ignored.
        out.add_operand(Operand::make_imm(OperandType::Imm8, is4));
        return DecodeError::Success;
    }
    // VBLENDVPS / VBLENDVPD — 4A / 4B /r /is4 (mask register in imm8[7:4];
    // VEX.W must be 0). Same encoding shape as VPBLENDVB, but the blend is
    // per 32-bit (PS) or per 64-bit (PD) element.
    if ((byte == 0x4A || byte == 0x4B) && !is64) {
        auto err = decode_xmm_modrm(ctx, dest, rm);
        if (err != DecodeError::Success) return err;
        u8 is4;
        if (!ctx.reader.read_u8(is4)) return DecodeError::UnexpectedEnd;
        if (byte == 0x4A) {
            out.opcode = l ? Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM
                           : Opcode::VBLENDVPS_XMM_XMM_XMMM128_XMM;
        } else {
            out.opcode = l ? Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM
                           : Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM;
        }
        out.add_operand(dest);
        out.add_operand(rm);
        // Store mask register index as Imm8 (high nibble); bits[3:0] ignored.
        out.add_operand(Operand::make_imm(OperandType::Imm8, is4));
        return DecodeError::Success;
    }
    // VCVTPS2PH — VEX.66.0F3A.W0 1D /r ib (Intel SDM Vol. 2 VCVTPS2PH).
    if (byte == 0x1D && !is64) {
        auto err = decode_xmm_modrm(ctx, dest, rm, l ? OperandType::Mem128 : OperandType::Mem64);
        if (err != DecodeError::Success) return err;
        out.opcode = l ? Opcode::VCVTPS2PH_XMMM128_YMM_IMM8 : Opcode::VCVTPS2PH_XMMM64_XMM_IMM8;
        out.add_operand(rm);
        out.add_operand(dest);
        return read_imm();
    }

    return DecodeError::InvalidOpcode;
}

}  // namespace vitreo::cpu::decode
