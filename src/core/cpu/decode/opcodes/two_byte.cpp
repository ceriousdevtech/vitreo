// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Two-byte opcode decoding (0F prefix)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_two_byte(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;
    
    switch (byte) {
        // Jcc rel32 (0F 80-8F)
        case 0x80: out.opcode = Opcode::JO_REL32; goto read_rel32;
        case 0x81: out.opcode = Opcode::JNO_REL32; goto read_rel32;
        case 0x82: out.opcode = Opcode::JB_REL32; goto read_rel32;
        case 0x83: out.opcode = Opcode::JNB_REL32; goto read_rel32;
        case 0x84: out.opcode = Opcode::JZ_REL32; goto read_rel32;
        case 0x85: out.opcode = Opcode::JNZ_REL32; goto read_rel32;
        case 0x86: out.opcode = Opcode::JBE_REL32; goto read_rel32;
        case 0x87: out.opcode = Opcode::JNBE_REL32; goto read_rel32;
        case 0x88: out.opcode = Opcode::JS_REL32; goto read_rel32;
        case 0x89: out.opcode = Opcode::JNS_REL32; goto read_rel32;
        case 0x8A: out.opcode = Opcode::JP_REL32; goto read_rel32;
        case 0x8B: out.opcode = Opcode::JNP_REL32; goto read_rel32;
        case 0x8C: out.opcode = Opcode::JL_REL32; goto read_rel32;
        case 0x8D: out.opcode = Opcode::JNL_REL32; goto read_rel32;
        case 0x8E: out.opcode = Opcode::JLE_REL32; goto read_rel32;
        case 0x8F: out.opcode = Opcode::JNLE_REL32; goto read_rel32;
        read_rel32: {
            i32 disp;
            if (!reader.read_i32(disp)) return DecodeError::UnexpectedEnd;
            out.add_operand(Operand::make_imm(OperandType::Rel32, disp));
            return DecodeError::Success;
        }
        
        // NOP r/m (0F 1F) - Multi-byte NOP
        // PREFETCHW (0F 0D /r) - 3DNow! prefetch, AMD/Intel modern - hint only
        // PREFETCH* (0F 18 /r) - PREFETCHNTA/T0/T1/T2 - hint only
        // 0F 19-1E: reserved NOP / hint NOP (Intel SDM Vol. 2 NOP). ENDBR32/64
        // occupy F3 0F 1E FB/FA and execute as NOPs when CET is off.
        case 0x0D:
        case 0x18:
        case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
        case 0x1E:
        case 0x1F: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (byte == 0x1E && ctx.prefix.rep) {
                if (modrm == 0xFA) {
                    out.opcode = Opcode::ENDBR64;
                    return DecodeError::Success;
                }
                if (modrm == 0xFB) {
                    out.opcode = Opcode::ENDBR32;
                    return DecodeError::Success;
                }
            }
            out.opcode = Opcode::NOP_RM;
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // UD2 (0F 0B)
        case 0x0B:
            out.opcode = Opcode::UD2;
            return DecodeError::Success;

        // UD1 (0F B9 /r) and UD0 (0F FF /r) — Intel SDM Vol. 2 "UD".
        //
        // Both raise #UD, which is also what an InvalidOpcode decode failure
        // produces, so this looks redundant. It is not: these forms take a
        // ModRM byte (and so may carry SIB and displacement), and the
        // exception must report the length of the WHOLE instruction. Failing
        // the decode instead reports the escape byte alone, so a debugger or
        // an SEH handler that resumes past the faulting instruction resumes
        // into the middle of it.
        //
        // UD0's operand form is the version Intel documents from SDM rev. 060
        // onward; earlier processors encoded 0F FF with no ModRM. Decoding the
        // ModRM matches current architecture and the assemblers that emit it.
        case 0xB9:
        case 0xFF: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm,
                             reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            out.opcode = (byte == 0xB9) ? Opcode::UD1_R_RM : Opcode::UD0_R_RM;
            out.add_operand(Operand::make_reg(
                is_64bit_operand ? OperandType::Reg64 : OperandType::Reg32,
                reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }

        // WBINVD (0F 09) is privileged. Decode it distinctly from UD so the
        // user-mode executor can raise Windows' privileged-instruction status.
        case 0x09:
            out.opcode = Opcode::WBINVD;
            return DecodeError::Success;

        // SYSCALL (0F 05)
        case 0x05:
            out.opcode = Opcode::SYSCALL;
            return DecodeError::Success;
        // SYSENTER (0F 34) / SYSEXIT (0F 35) — SEP
        case 0x34:
            out.opcode = Opcode::SYSENTER;
            return DecodeError::Success;
        case 0x35:
            out.opcode = Opcode::SYSEXIT;
            return DecodeError::Success;
        
        // CPUID (0F A2)
        case 0xA2:
            out.opcode = Opcode::CPUID;
            return DecodeError::Success;
        
        // RDTSC (0F 31)
        case 0x31:
            out.opcode = Opcode::RDTSC;
            return DecodeError::Success;

        // Group 7 (0F 01). RDTSCP is 0F 01 F9; XGETBV is 0F 01 D0.
        // Other 0F 01 encodings stay #UD in this user-mode runtime.
        case 0x01: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            if (modrm == 0xF9) {
                out.opcode = Opcode::RDTSCP;
                return DecodeError::Success;
            }
            if (modrm == 0xD0) {
                out.opcode = Opcode::XGETBV;
                return DecodeError::Success;
            }
            return DecodeError::InvalidOpcode;
        }
        
        // WRMSR (0F 30), RDMSR (0F 32), RDPMC (0F 33) - privileged
        case 0x30:
            out.opcode = Opcode::WRMSR;
            return DecodeError::Success;
        case 0x32:
            out.opcode = Opcode::RDMSR;
            return DecodeError::Success;
        case 0x33:
            out.opcode = Opcode::RDPMC;
            return DecodeError::Success;
        
        // MOVZX r16/32/64, r/m8 (0F B6)
        case 0xB6: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            
            if (is_64bit_operand) {
                out.opcode = Opcode::MOVZX_R64_RM8;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::MOVZX_R16_RM8;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::MOVZX_R32_RM8;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // MOVZX r32/64, r/m16 (0F B7)
        case 0xB7: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
            
            if (is_64bit_operand) {
                out.opcode = Opcode::MOVZX_R64_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else {
                out.opcode = Opcode::MOVZX_R32_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // MOVSX r16/32/64, r/m8 (0F BE)
        case 0xBE: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
            
            if (is_64bit_operand) {
                out.opcode = Opcode::MOVSX_R64_RM8;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::MOVSX_R16_RM8;
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::MOVSX_R32_RM8;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // MOVSX r32/64, r/m16 (0F BF)
        case 0xBF: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
            
            if (is_64bit_operand) {
                out.opcode = Opcode::MOVSX_R64_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else {
                out.opcode = Opcode::MOVSX_R32_RM16;
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // CMPXCHG r/m8, r8 (0F B0)
        case 0xB0:
        // CMPXCHG r/m16/32/64, r16/32/64 (0F B1)
        case 0xB1:
            return decode_cmpxchg(ctx);
        
        // XADD r/m8, r8 (0F C0)
        case 0xC0:
        // XADD r/m16/32/64, r16/32/64 (0F C1)
        case 0xC1:
            return decode_xadd(ctx);
        
        // CMPXCHG8B m64 (0F C7 /1)  /  CMPXCHG16B m128 (REX.W 0F C7 /1)
        case 0xC7: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            // reg_field must be 1 for CMPXCHG8B / CMPXCHG16B
            if (reg_field != 1) return DecodeError::InvalidOpcode;
            // Must be memory operand (mod != 11)
            if (is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            if (prefix.rex_w()) {
                // REX.W → CMPXCHG16B m128
                rm.type = OperandType::Mem128;
                out.opcode = Opcode::CMPXCHG16B_M128;
            } else {
                rm.type = OperandType::Mem64;
                out.opcode = Opcode::CMPXCHG8B_M64;
            }
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        // CMOVcc r16/32/64, r/m16/32/64 (0F 40-4F)
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            return decode_cmov(ctx);
        
        // SETcc r/m8 (0F 90-9F)
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            return decode_setcc(ctx);
        
        // POPCNT (F3 0F B8)
        case 0xB8:
            if (ctx.prefix.rep) {
                ctx.prefix.rep = false;
                return decode_popcnt(ctx);
            }
            return DecodeError::InvalidOpcode;

        // BSF (0F BC)
        case 0xBC:
        // BSR (0F BD)
        case 0xBD:
            return decode_bsf_bsr(ctx);
        
        // BT (0F A3)
        case 0xA3:
        // BTS (0F AB)
        case 0xAB:
        // BTR (0F B3)
        case 0xB3:
        // BTC (0F BB)
        case 0xBB:
            return decode_bt(ctx);

        // PUSH FS (0F A0) / POP FS (0F A1) / PUSH GS (0F A8) / POP GS (0F A9)
        case 0xA0:
            out.opcode = Opcode::PUSH_FS;
            out.add_operand(Operand::make_reg(OperandType::RegSeg,
                                             static_cast<u8>(SegReg::FS)));
            return DecodeError::Success;
        case 0xA1:
            out.opcode = Opcode::POP_FS;
            out.add_operand(Operand::make_reg(OperandType::RegSeg,
                                             static_cast<u8>(SegReg::FS)));
            return DecodeError::Success;
        case 0xA8:
            out.opcode = Opcode::PUSH_GS;
            out.add_operand(Operand::make_reg(OperandType::RegSeg,
                                             static_cast<u8>(SegReg::GS)));
            return DecodeError::Success;
        case 0xA9:
            out.opcode = Opcode::POP_GS;
            out.add_operand(Operand::make_reg(OperandType::RegSeg,
                                             static_cast<u8>(SegReg::GS)));
            return DecodeError::Success;

        // MOVNTI m32, r32 / MOVNTI m64, r64 (0F C3 /r) — dest must be memory
        case 0xC3: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0)
                return DecodeError::InvalidModRM;
            if (prefix.rex_r()) reg_field |= 8;
            if (is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            if (prefix.rex_w()) {
                rm.type = OperandType::Mem64;
                out.opcode = Opcode::MOVNTI_M64_R64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else {
                rm.type = OperandType::Mem32;
                out.opcode = Opcode::MOVNTI_M32_R32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            return DecodeError::Success;
        }

        // BT/BTS/BTR/BTC imm8 (0F BA)
        case 0xBA:
            return decode_bt_imm(ctx);
        
        // IMUL r, r/m (0F AF)
        case 0xAF:
            return decode_imul_two_byte(ctx);
        
        // BSWAP (0F C8-CF)
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            return decode_bswap(ctx);
        
        // SSE data transfer (0F 10/11/12/13/16/17/28/29/2B/50/54-57/6F/7F/AE/F0)
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x16: case 0x17: case 0x28: case 0x29:
        case 0x2B:  // MOVNTPS / MOVNTPD
        case 0x50: case 0x54: case 0x55: case 0x56:
        case 0x57: case 0x6F: case 0x7F: case 0xAE:
        case 0xF0:  // SSE3 LDDQU (F2 0F F0)
            return decode_sse_data(ctx);
        
        // SSE arithmetic (0F 2E/2F/51/52/53/58/59/5C/5D/5E/5F, SSE3: 7C/7D/D0)
        case 0x2E: case 0x2F:
        case 0x51: case 0x52: case 0x53:
        case 0x58: case 0x59:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x7C: case 0x7D: case 0xD0:
            return decode_sse_arith(ctx);
        
        // SSE conversion (0F 2A/2C/2D/5A/5B/E6)
        case 0x2A: case 0x2C: case 0x2D:
        case 0x5A: case 0x5B: case 0xE6:
            return decode_sse_conv(ctx);
        
        // SSE comparison instructions (CPU-036)
        case 0xC2:
        case 0x74: case 0x75: case 0x76:
        case 0x64: case 0x65: case 0x66:
            return decode_sse_cmp(ctx);
        
        // SSE2 integer arithmetic (CPU-037)
        case 0x77: // EMMS, or VZEROUPPER / VZEROALL when VEX-encoded
            if (ctx.prefix.vex) {
                out.opcode = ctx.prefix.vex_l ? Opcode::VZEROALL : Opcode::VZEROUPPER;
                return DecodeError::Success;
            }
            out.opcode = Opcode::EMMS;
            return DecodeError::Success;
        case 0xFC: case 0xFD: case 0xFE: case 0xD4: // PADDB/W/D/Q
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: // PSUBB/W/D/Q
        case 0xDC: case 0xDD: case 0xD8: case 0xD9: // PADDUSB/W, PSUBUSB/W
        case 0xEC: case 0xED: case 0xE8: case 0xE9: // PADDSB/W, PSUBSB/W
        case 0x63: case 0x6B: case 0x67:             // PACKSSWB, PACKSSDW, PACKUSWB
        case 0xD5: case 0xE5: case 0xE4: case 0xF5: case 0xF4: // PMULLW, PMULHW, PMULHUW, PMADDWD, PMULUDQ
        case 0xDA: case 0xDE: case 0xEA: case 0xEE: // PMINUB, PMAXUB, PMINSW, PMAXSW
        case 0xE0: case 0xE3: case 0xF6:             // PAVGB, PAVGW, PSADBW
        case 0xDB: case 0xDF: case 0xEB: case 0xEF: // PAND, PANDN, POR, PXOR
        case 0x6E: case 0x7E:                         // MOVD
        case 0xD6:                                     // MOVQ store / MOVQ2DQ / MOVDQ2Q
        case 0xD7:                                     // PMOVMSKB
        case 0x71: case 0x72: case 0x73:               // packed shifts by imm
        case 0xD1: case 0xD2: case 0xD3:               // PSRLW/D/Q by register
        case 0xE1: case 0xE2:                           // PSRAW/D by register
        case 0xF1: case 0xF2: case 0xF3:               // PSLLW/D/Q by register
        case 0xC4: case 0xC5:                             // PINSRW, PEXTRW
        case 0xE7: case 0xF7:                             // MOVNTQ/DQ, MASKMOVQ/DQU
            return decode_sse_int(ctx);
        
        // SSE shuffle & unpack (0F C6/70/14/15/60-62/68-6A/6C/6D)
        case 0xC6: case 0x70:
        case 0x14: case 0x15:
        case 0x60: case 0x61: case 0x62: case 0x6C:
        case 0x68: case 0x69: case 0x6A: case 0x6D:
            return decode_sse_shuffle(ctx);
        
        // SHLD r/m, r, imm8 (0F A4)
        case 0xA4: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            u8 imm8;
            if (!reader.read_u8(imm8)) return DecodeError::UnexpectedEnd;
            if (is_64bit_operand) {
                out.opcode = Opcode::SHLD_RM64_R64_IMM8;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SHLD_RM16_R16_IMM8;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SHLD_RM32_R32_IMM8;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm8));
            return DecodeError::Success;
        }
        
        // SHLD r/m, r, CL (0F A5)
        case 0xA5: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::SHLD_RM64_R64_CL;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SHLD_RM16_R16_CL;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SHLD_RM32_R32_CL;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(Operand::make_reg(OperandType::Reg8, 1)); // CL
            return DecodeError::Success;
        }
        
        // SHRD r/m, r, imm8 (0F AC)
        case 0xAC: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            u8 imm8;
            if (!reader.read_u8(imm8)) return DecodeError::UnexpectedEnd;
            if (is_64bit_operand) {
                out.opcode = Opcode::SHRD_RM64_R64_IMM8;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SHRD_RM16_R16_IMM8;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SHRD_RM32_R32_IMM8;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(Operand::make_imm(OperandType::Imm8, imm8));
            return DecodeError::Success;
        }
        
        // SHRD r/m, r, CL (0F AD)
        case 0xAD: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_64bit_operand) {
                out.opcode = Opcode::SHRD_RM64_R64_CL;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
            } else if (op_size == 2) {
                out.opcode = Opcode::SHRD_RM16_R16_CL;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
            } else {
                out.opcode = Opcode::SHRD_RM32_R32_CL;
                if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
                else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
                out.add_operand(rm);
                out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
            }
            out.add_operand(Operand::make_reg(OperandType::Reg8, 1)); // CL
            return DecodeError::Success;
        }

        // LSS / LFS / LGS — 0F B2 / B4 / B5. Memory far-pointer load.
        case 0xB2: case 0xB4: case 0xB5: {
            u8 modrm;
            if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;
            Operand rm;
            u8 reg_field;
            if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) return DecodeError::InvalidOpcode;
            const bool is64 = is_64bit_operand && mode == DecodeMode::Mode64;
            const bool word = (op_size == 2) && !is64;
            rm.type = OperandType::MemUnknown;
            Opcode op;
            if (byte == 0xB2) {
                op = is64 ? Opcode::LSS_R64_M80
                    : (word ? Opcode::LSS_R16_M32 : Opcode::LSS_R32_M48);
            } else if (byte == 0xB4) {
                op = is64 ? Opcode::LFS_R64_M80
                    : (word ? Opcode::LFS_R16_M32 : Opcode::LFS_R32_M48);
            } else {
                op = is64 ? Opcode::LGS_R64_M80
                    : (word ? Opcode::LGS_R16_M32 : Opcode::LGS_R32_M48);
            }
            out.opcode = op;
            OperandType rty = is64 ? OperandType::Reg64
                : (word ? OperandType::Reg16 : OperandType::Reg32);
            out.add_operand(Operand::make_reg(rty, reg_field));
            out.add_operand(rm);
            return DecodeError::Success;
        }
        
        default:
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
