// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE2 integer arithmetic instruction decoding (CPU-037)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_sse_int(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    DecodeMode mode = ctx.mode;

    // Intel SDM Vol. 2B 4-348/349: PMOVMSKB has NP/66 legacy forms,
    // VEX.128/256.66.WIG forms, and reserved encoded vvvv=1111b.
    // The prefix decoder stores vvvv inverted. Silicon also rejects every
    // F2/F3 combination (pmovmskb_encoding_probe.expected, both modes).
    if (byte == 0xD7 &&
        (prefix.rep || prefix.repne ||
         (prefix.vex && (!prefix.operand_size || prefix.vex_vvvv != 0)))) {
        return DecodeError::InvalidOpcode;
    }

    // F3 0F 7E — MOVQ xmm, xmm/m64 (needs F3, not 66)
    if (byte == 0x7E && prefix.rep && !prefix.operand_size) {
        u8 modrm;
        if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
        Operand rm;
        u8 reg_field;
        if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0)
            return DecodeError::InvalidModRM;
        if (prefix.rex_r()) reg_field |= 8;
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem64;
        }
        out.opcode = Opcode::MOVQ_XMM_XMMM64;
        out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
        out.add_operand(rm);
        return DecodeError::Success;
    }

    // SSE2 integer instructions normally require 66 prefix.
    // Without the prefix, they encode MMX variants (operating on mm0-mm7).
    // We map MMX registers to XMM8-XMM15 to avoid collision with XMM0-XMM7
    // used by SSE/SSSE3 instructions (code may mix both register files).
    bool is_mmx = !ctx.prefix.operand_size && !ctx.prefix.rep && !ctx.prefix.repne;

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

    // Helper: map packed register index for MMX mode (offset by 8)
    auto mmx_pack_reg = [&](u8 r) -> u8 {
        return is_mmx ? ((r & 7) | 8) : r;
    };

    // Set rm operand type: register → Reg128, memory → Mem128
    auto set_rm_packed = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
            rm.reg = mmx_pack_reg(rm.reg);
        } else {
            rm.type = OperandType::Mem128;
        }
    };

    set_rm_packed();

    auto finish_mmx_rr = [&](Opcode opc) -> DecodeError {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::RegMM;
            rm.reg &= 7;
        } else {
            rm.type = OperandType::Mem64;
        }
        out.opcode = opc;
        out.add_operand(Operand::reg_mm(reg_field & 7));
        out.add_operand(rm);
        return DecodeError::Success;
    };

    switch (byte) {
        // === PINSRW — Insert Word into XMM ===
        case 0xC4: {
            // NP 0F C4 /r ib — PINSRW mm, r32/m16, imm8 (SSE)
            // 66 0F C4 /r ib — PINSRW xmm, r32/m16, imm8 (SSE2)
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg32;
                rm.reg &= 7;
                if (prefix.rex_b()) rm.reg |= 8;
            } else {
                rm.type = OperandType::Mem16;
            }
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            if (is_mmx) {
                out.opcode = Opcode::PINSRW_MM_R32M16_IMM8;
                out.add_operand(Operand::reg_mm(reg_field & 7));
            } else {
                out.opcode = Opcode::PINSRW_XMM_R32M16_IMM8;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
            }
            out.add_operand(rm);
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }
        case 0xC5: {
            // NP 0F C5 /r ib — PEXTRW r32, mm, imm8; 66 0F C5 — PEXTRW r32, xmm
            if (!is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            if (is_mmx) {
                out.opcode = Opcode::PEXTRW_R32_MM_IMM8;
                out.add_operand(Operand::reg_mm(rm.reg & 7));
            } else {
                out.opcode = Opcode::PEXTRW_R32_XMM_IMM8;
                rm.type = OperandType::Reg128;
                if (prefix.rex_b()) rm.reg |= 8;
                out.add_operand(rm);
            }
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }

        // === Packed add ===
        case 0xFC:
            if (is_mmx) return finish_mmx_rr(Opcode::PADDB_MM_MMM64);
            out.opcode = Opcode::PADDB_XMM_XMMM128;   break;
        case 0xFD:
            if (is_mmx) {
                // MMX: PADDW mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PADDW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PADDW_XMM_XMMM128;   break;
        case 0xFE:
            if (is_mmx) {
                // MMX: PADDD mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PADDD_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PADDD_XMM_XMMM128;   break;
        case 0xD4:
            if (is_mmx) {
                // MMX: PADDQ mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PADDQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PADDQ_XMM_XMMM128;
            break;

        // === Packed subtract ===
        case 0xF8:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBB_MM_MMM64);
            out.opcode = Opcode::PSUBB_XMM_XMMM128;   break;
        case 0xF9:
            if (is_mmx) {
                // NP 0F F9 /r — PSUBW mm, mm/m64. Wraparound per word
                // (Intel SDM Vol. 2: PSUBB/PSUBW/PSUBD). Must land in MM0-7;
                // the XMM8-15 alias leaves MOVQ/MOVD/PCMPEQW looking at the
                // pre-subtract value, so a later DIV r32 can see a 0 divisor.
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PSUBW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PSUBW_XMM_XMMM128;   break;
        case 0xFA:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBD_MM_MMM64);
            out.opcode = Opcode::PSUBD_XMM_XMMM128;   break;
        case 0xFB:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBQ_MM_MMM64);
            out.opcode = Opcode::PSUBQ_XMM_XMMM128;   break;

        // === Packed add/sub with unsigned saturation ===
        case 0xDC:
            if (is_mmx) return finish_mmx_rr(Opcode::PADDUSB_MM_MMM64);
            out.opcode = Opcode::PADDUSB_XMM_XMMM128; break;
        case 0xDD:
            if (is_mmx) return finish_mmx_rr(Opcode::PADDUSW_MM_MMM64);
            out.opcode = Opcode::PADDUSW_XMM_XMMM128; break;
        case 0xD8:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBUSB_MM_MMM64);
            out.opcode = Opcode::PSUBUSB_XMM_XMMM128; break;
        case 0xD9:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBUSW_MM_MMM64);
            out.opcode = Opcode::PSUBUSW_XMM_XMMM128; break;

        // === Packed add/sub with signed saturation ===
        case 0xEC:
            if (is_mmx) return finish_mmx_rr(Opcode::PADDSB_MM_MMM64);
            out.opcode = Opcode::PADDSB_XMM_XMMM128;  break;
        case 0xED:
            if (is_mmx) return finish_mmx_rr(Opcode::PADDSW_MM_MMM64);
            out.opcode = Opcode::PADDSW_XMM_XMMM128;  break;
        case 0xE8:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBSB_MM_MMM64);
            out.opcode = Opcode::PSUBSB_XMM_XMMM128;  break;
        case 0xE9:
            if (is_mmx) return finish_mmx_rr(Opcode::PSUBSW_MM_MMM64);
            out.opcode = Opcode::PSUBSW_XMM_XMMM128;  break;

        // === Pack with saturation ===
        case 0x63:
            if (is_mmx) return finish_mmx_rr(Opcode::PACKSSWB_MM_MMM64);
            out.opcode = Opcode::PACKSSWB_XMM_XMMM128; break;
        case 0x6B:
            if (is_mmx) {
                // NP 0F 6B /r — PACKSSDW mm, mm/m64. Source is 64-bit, not
                // the 128-bit XMM memory operand the alias path would use.
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PACKSSDW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PACKSSDW_XMM_XMMM128; break;
        case 0x67:
            if (is_mmx) {
                // MMX: PACKUSWB mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PACKUSWB_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PACKUSWB_XMM_XMMM128; break;

        // === Packed multiply ===
        case 0xD5:
            if (is_mmx) return finish_mmx_rr(Opcode::PMULLW_MM_MMM64);
            out.opcode = Opcode::PMULLW_XMM_XMMM128;  break;
        case 0xE5:
            if (is_mmx) return finish_mmx_rr(Opcode::PMULHW_MM_MMM64);
            out.opcode = Opcode::PMULHW_XMM_XMMM128;  break;
        case 0xE4:
            if (is_mmx) return finish_mmx_rr(Opcode::PMULHUW_MM_MMM64);
            out.opcode = Opcode::PMULHUW_XMM_XMMM128; break;
        case 0xF4:
            if (is_mmx) {
                // MMX: PMULUDQ mm, mm/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PMULUDQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PMULUDQ_XMM_XMMM128; break;
        case 0xF5:
            if (is_mmx) return finish_mmx_rr(Opcode::PMADDWD_MM_MMM64);
            out.opcode = Opcode::PMADDWD_XMM_XMMM128; break;

        // === Packed min/max ===
        case 0xDA:
            if (is_mmx) return finish_mmx_rr(Opcode::PMINUB_MM_MMM64);
            out.opcode = Opcode::PMINUB_XMM_XMMM128;  break;
        case 0xDE:
            if (is_mmx) return finish_mmx_rr(Opcode::PMAXUB_MM_MMM64);
            out.opcode = Opcode::PMAXUB_XMM_XMMM128;  break;
        case 0xEA:
            if (is_mmx) {
                // NP 0F EA /r — PMINSW mm, mm/m64 (SSE on MMX).
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PMINSW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PMINSW_XMM_XMMM128;  break;
        case 0xEE:
            if (is_mmx) {
                // NP 0F EE /r — PMAXSW mm, mm/m64 (SSE on MMX).
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::RegMM;
                    rm.reg &= 7;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::PMAXSW_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PMAXSW_XMM_XMMM128;  break;

        // === Packed average / SAD ===
        case 0xE0:
            if (is_mmx) return finish_mmx_rr(Opcode::PAVGB_MM_MMM64);
            out.opcode = Opcode::PAVGB_XMM_XMMM128;   break;
        case 0xE3:
            if (is_mmx) return finish_mmx_rr(Opcode::PAVGW_MM_MMM64);
            out.opcode = Opcode::PAVGW_XMM_XMMM128;   break;
        case 0xF6:
            if (is_mmx) return finish_mmx_rr(Opcode::PSADBW_MM_MMM64);
            out.opcode = Opcode::PSADBW_XMM_XMMM128;  break;

        // === Packed bitwise ===
        case 0xDB:
            if (is_mmx) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PAND_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PAND_XMM_XMMM128; break;
        case 0xDF:
            if (is_mmx) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PANDN_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PANDN_XMM_XMMM128; break;
        case 0xEB:
            if (is_mmx) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::POR_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::POR_XMM_XMMM128; break;
        case 0xEF:
            if (is_mmx) {
                if (is_register_operand(rm.type)) { rm.type = OperandType::RegMM; rm.reg &= 7; }
                else rm.type = OperandType::Mem64;
                out.opcode = Opcode::PXOR_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            out.opcode = Opcode::PXOR_XMM_XMMM128; break;

        // === MOVD / MOVQ data movement ===
        case 0x6E: {
            // 66 0F 6E — MOVD xmm, r/m32; NP 0F 6E — MOVD mm, r/m32
            // 66 REX.W 0F 6E — MOVQ xmm, r/m64
            if (prefix.rex_w() && !is_mmx) {
                // REX.W: 64-bit MOVQ xmm, r/m64
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::Reg64;
                    if (prefix.rex_b()) rm.reg |= 8;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.opcode = Opcode::MOVQ_XMM_RM64;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg32;
                rm.reg &= 7;  // Undo mmx_pack_reg offset — rm is a GPR, not MMX
                if (prefix.rex_b()) rm.reg |= 8;
            } else {
                rm.type = OperandType::Mem32;
            }
            if (is_mmx) {
                out.opcode = Opcode::MOVD_MM_RM32;
                out.add_operand(Operand::reg_mm(reg_field & 7));
            } else {
                out.opcode = Opcode::MOVD_XMM_RM32;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        case 0x7E: {
            // 66 0F 7E — MOVD r/m32, xmm; NP 0F 7E — MOVD r/m32, mm
            // 66 REX.W 0F 7E — MOVQ r/m64, xmm
            if (prefix.rex_w() && !is_mmx) {
                // REX.W: 64-bit MOVQ r/m64, xmm
                if (is_register_operand(rm.type)) {
                    rm.type = OperandType::Reg64;
                    if (prefix.rex_b()) rm.reg |= 8;
                } else {
                    rm.type = OperandType::Mem64;
                }
                out.add_operand(rm);
                out.opcode = Opcode::MOVQ_RM64_XMM;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
                return DecodeError::Success;
            }
            if (is_register_operand(rm.type)) {
                rm.type = OperandType::Reg32;
                rm.reg &= 7;  // Undo mmx_pack_reg offset — rm is a GPR, not MMX
                if (prefix.rex_b()) rm.reg |= 8;
            } else {
                rm.type = OperandType::Mem32;
            }
            out.add_operand(rm);
            if (is_mmx) {
                out.opcode = Opcode::MOVD_RM32_MM;
                out.add_operand(Operand::reg_mm(reg_field & 7));
            } else {
                out.opcode = Opcode::MOVD_RM32_XMM;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
            }
            return DecodeError::Success;
        }
        case 0xD6: {
            // F3 0F D6 /r — MOVQ2DQ xmm, mm (register only)
            // F2 0F D6 /r — MOVDQ2Q mm, xmm (register only)
            // 66 0F D6 /r — MOVQ xmm/m64, xmm
            // NP 0F D6 — #UD
            if (prefix.rep && prefix.repne) return DecodeError::InvalidOpcode;
            if (prefix.rep) {
                if (!is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
                out.opcode = Opcode::MOVQ2DQ_XMM_MM;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
                out.add_operand(Operand::reg_mm(rm.reg & 7));
                return DecodeError::Success;
            }
            if (prefix.repne) {
                if (!is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
                out.opcode = Opcode::MOVDQ2Q_MM_XMM;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                rm.type = OperandType::Reg128;
                out.add_operand(rm);
                return DecodeError::Success;
            }
            if (!prefix.operand_size) return DecodeError::InvalidOpcode;
            if (rm.type == OperandType::Mem128) rm.type = OperandType::Mem64;
            out.opcode = Opcode::MOVQ_XMMM64_XMM;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
            return DecodeError::Success;
        }
        case 0xD7: {
            // NP 0F D7 — PMOVMSKB r32, mm; 66 0F D7 — PMOVMSKB r32, xmm
            // All forms require a register source. An encoded memory source
            // is #UD, not a load that may fault or an accidental XMM0 read.
            if (!is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            if (is_mmx) {
                out.opcode = Opcode::PMOVMSKB_R32_MM;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
                out.add_operand(Operand::reg_mm(rm.reg & 7));
                return DecodeError::Success;
            }
            out.opcode = Opcode::PMOVMSKB_R32_XMM;
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            out.add_operand(rm); // already Reg128 from set_rm_packed()
            return DecodeError::Success;
        }

        // === Packed shift by immediate ===
        case 0x71: case 0x72: case 0x73: {
            // 66 0F 71/72/73 modrm imm8
            // r/m field = XMM register, reg field = shift operation
            u8 imm;
            if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
            u8 op_sel = reg_field & 7;
            // Intel SDM Vol. 2 PSRLW/PSRAW/PSLLW/PSRLD/PSRAD/PSLLD/PSRLQ/
            // PSLLQ/PSRLDQ/PSLLDQ by imm8 (groups 12/13/14): the operand is
            // "mm/xmm, imm8" -- ModRM.r/m names a register only, so a memory
            // encoding (mod != 11) is #UD. Silicon raises on both
            // `66 0F 73 3B 04` and its VEX form
            // (tools/oracle_probe/vex_scalar_round_probe.c); Vitreo used to
            // decode a memory-destination shift that does not exist.
            if (!is_register_operand(rm.type)) return DecodeError::InvalidOpcode;

            // MMX mode for 0x71 (PSRLW/PSRAW/PSLLW imm8), 0x72
            // (PSRLD/PSRAD/PSLLD imm8) and 0x73 (PSLLQ/PSRLQ imm8).
            if (is_mmx && (byte == 0x71 || byte == 0x72 || byte == 0x73)) {
                if (byte == 0x71) {
                    switch (op_sel) {
                        case 2: out.opcode = Opcode::PSRLW_MM_IMM8; break;
                        case 4: out.opcode = Opcode::PSRAW_MM_IMM8; break;
                        case 6: out.opcode = Opcode::PSLLW_MM_IMM8; break;
                        default: return DecodeError::InvalidOpcode;
                    }
                    out.add_operand(Operand::reg_mm(rm.reg & 7));
                    out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
                    return DecodeError::Success;
                }
                if (byte == 0x72) {
                    switch (op_sel) {
                        case 2: out.opcode = Opcode::PSRLD_MM_IMM8; break;
                        case 4: out.opcode = Opcode::PSRAD_MM_IMM8; break;
                        case 6: out.opcode = Opcode::PSLLD_MM_IMM8; break;
                        default: return DecodeError::InvalidOpcode;
                    }
                } else {
                    switch (op_sel) {
                        case 2: out.opcode = Opcode::PSRLQ_MM_IMM8; break;
                        case 6: out.opcode = Opcode::PSLLQ_MM_IMM8; break;
                        default: return DecodeError::InvalidOpcode;
                    }
                }
                // rm is XMM, extract MM reg index
                out.add_operand(Operand::reg_mm(rm.reg & 7));
                out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
                return DecodeError::Success;
            }
            
            if (byte == 0x71) {
                switch (op_sel) {
                    case 2: out.opcode = Opcode::PSRLW_XMM_IMM8; break;
                    case 4: out.opcode = Opcode::PSRAW_XMM_IMM8; break;
                    case 6: out.opcode = Opcode::PSLLW_XMM_IMM8; break;
                    default: return DecodeError::InvalidOpcode;
                }
            } else if (byte == 0x72) {
                switch (op_sel) {
                    case 2: out.opcode = Opcode::PSRLD_XMM_IMM8; break;
                    case 4: out.opcode = Opcode::PSRAD_XMM_IMM8; break;
                    case 6: out.opcode = Opcode::PSLLD_XMM_IMM8; break;
                    default: return DecodeError::InvalidOpcode;
                }
            } else { // 0x73
                switch (op_sel) {
                    case 2: out.opcode = Opcode::PSRLQ_XMM_IMM8; break;
                    case 3: out.opcode = Opcode::PSRLDQ_XMM_IMM8; break;
                    case 6: out.opcode = Opcode::PSLLQ_XMM_IMM8; break;
                    case 7: out.opcode = Opcode::PSLLDQ_XMM_IMM8; break;
                    default: return DecodeError::InvalidOpcode;
                }
            }
            // Operand 0: XMM register (from r/m field)
            out.add_operand(rm);
            // Operand 1: immediate
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
            return DecodeError::Success;
        }

        // === Packed shift by register ===
        case 0xD1:
            if (is_mmx) return finish_mmx_rr(Opcode::PSRLW_MM_MMM64);
            out.opcode = Opcode::PSRLW_XMM_XMMM128; break;
        case 0xD2:
            if (is_mmx) return finish_mmx_rr(Opcode::PSRLD_MM_MMM64);
            out.opcode = Opcode::PSRLD_XMM_XMMM128; break;
        case 0xD3:
            if (is_mmx) {
                out.opcode = Opcode::PSRLQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                if (is_register_operand(rm.type)) {
                    out.add_operand(Operand::reg_mm(rm.reg & 7));
                } else {
                    rm.type = OperandType::Mem64;
                    out.add_operand(rm);
                }
                return DecodeError::Success;
            }
            out.opcode = Opcode::PSRLQ_XMM_XMMM128;
            break;
        case 0xE1:
            if (is_mmx) return finish_mmx_rr(Opcode::PSRAW_MM_MMM64);
            out.opcode = Opcode::PSRAW_XMM_XMMM128; break;
        case 0xE2:
            if (is_mmx) return finish_mmx_rr(Opcode::PSRAD_MM_MMM64);
            out.opcode = Opcode::PSRAD_XMM_XMMM128; break;
        case 0xF1:
            if (is_mmx) return finish_mmx_rr(Opcode::PSLLW_MM_MMM64);
            out.opcode = Opcode::PSLLW_XMM_XMMM128; break;
        case 0xF2:
            if (is_mmx) return finish_mmx_rr(Opcode::PSLLD_MM_MMM64);
            out.opcode = Opcode::PSLLD_XMM_XMMM128; break;
        case 0xF3:
            if (is_mmx) {
                out.opcode = Opcode::PSLLQ_MM_MMM64;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                if (is_register_operand(rm.type)) {
                    out.add_operand(Operand::reg_mm(rm.reg & 7));
                } else {
                    rm.type = OperandType::Mem64;
                    out.add_operand(rm);
                }
                return DecodeError::Success;
            }
            out.opcode = Opcode::PSLLQ_XMM_XMMM128;
            break;

        // NP 0F E7 /r — MOVNTQ m64, mm (dest must be memory)
        // 66 0F E7 /r — MOVNTDQ m128, xmm (16-byte aligned dest)
        // F2/F3 0F E7 — #UD
        case 0xE7: {
            if (prefix.rep || prefix.repne) return DecodeError::InvalidOpcode;
            if (is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            if (is_mmx) {
                rm.type = OperandType::Mem64;
                out.opcode = Opcode::MOVNTQ_M64_MM;
                out.add_operand(rm);
                out.add_operand(Operand::reg_mm(reg_field & 7));
            } else if (prefix.operand_size) {
                rm.type = OperandType::Mem128;
                out.opcode = Opcode::MOVNTDQ_M128_XMM;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
            } else {
                return DecodeError::InvalidOpcode;
            }
            return DecodeError::Success;
        }

        // NP 0F F7 /r — MASKMOVQ mm1, mm2 (both registers; store to DS:EDI/RDI)
        // 66 0F F7 /r — MASKMOVDQU xmm1, xmm2
        // F2/F3 0F F7 — #UD; r/m memory — #UD
        case 0xF7: {
            if (prefix.rep || prefix.repne) return DecodeError::InvalidOpcode;
            if (!is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            if (is_mmx) {
                out.opcode = Opcode::MASKMOVQ_MM_MM;
                out.add_operand(Operand::reg_mm(reg_field & 7));
                out.add_operand(Operand::reg_mm(rm.reg & 7));
            } else if (prefix.operand_size) {
                out.opcode = Opcode::MASKMOVDQU_XMM_XMM;
                out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
                rm.type = OperandType::Reg128;
                out.add_operand(rm);
            } else {
                return DecodeError::InvalidOpcode;
            }
            return DecodeError::Success;
        }

        default:
            return DecodeError::InvalidOpcode;
    }

    // The is_mmx forms that reach here have no MM-register implementation yet
    // and fall back to the XMM8-15 alias, so their results are invisible to the
    // real MM register file (MOVQ/MOVD/PADDD/... use FpuState::get_mm).
    // Operand 0: destination XMM register (from reg field)
    out.add_operand(Operand::make_reg(OperandType::Reg128, mmx_pack_reg(reg_field)));
    // Operand 1: source XMM/memory (from r/m field)
    out.add_operand(rm);

    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
