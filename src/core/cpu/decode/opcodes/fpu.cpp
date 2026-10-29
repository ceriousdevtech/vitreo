// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// x87 FPU instruction decoding (CPU-030)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_fpu(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte; // D8-DF escape byte
    DecodeMode mode = ctx.mode;
    bool is_64bit_operand = ctx.is_64bit_operand;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    u8 mod = (modrm >> 6) & 3;
    u8 reg = (modrm >> 3) & 7;
    u8 rm  = modrm & 7;

    // Helper lambda: decode memory operand via modrm infrastructure.
    // Creates a new DecodeContext pointing at the already-parsed modrm byte
    // and calls the shared decode_modrm routine.
    auto decode_mem_operand = [&](OperandType mem_type) -> DecodeError {
        Operand rm_op;
        u8 reg_field;
        if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm_op, reg_field) < 0) {
            return DecodeError::InvalidModRM;
        }
        // Force the operand to the correct memory size
        rm_op.type = mem_type;
        out.add_operand(rm_op);
        return DecodeError::Success;
    };

    // mod == 3 → register form (ST(rm)), mod != 3 → memory form
    if (mod == 3) {
        // ---- Register forms (modrm >= 0xC0) ----
        switch (byte) {
            case 0xD8: {
                // D8 C0+i: FADD ST(0),ST(i)       reg=0, rm=i
                // D8 C8+i: FMUL ST(0),ST(i)       reg=1, rm=i
                // D8 D0+i: FCOM ST(i)              reg=2, rm=i
                // D8 D8+i: FCOMP ST(i)             reg=3, rm=i
                // D8 E0+i: FSUB ST(0),ST(i)        reg=4, rm=i
                // D8 E8+i: FSUBR ST(0),ST(i)       reg=5, rm=i
                // D8 F0+i: FDIV ST(0),ST(i)        reg=6, rm=i
                // D8 F8+i: FDIVR ST(0),ST(i)       reg=7, rm=i
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FADD_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 1:
                        out.opcode = Opcode::FMUL_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 2:
                        out.opcode = Opcode::FCOM_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 3:
                        out.opcode = Opcode::FCOMP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 4:
                        out.opcode = Opcode::FSUB_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 5:
                        out.opcode = Opcode::FSUBR_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 6:
                        out.opcode = Opcode::FDIV_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 7:
                        out.opcode = Opcode::FDIVR_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xD9: {
                // D9 C0+i (reg=0): FLD ST(i)
                // D9 C8+i (reg=1): FXCH ST(i)
                // D9 D0   (modrm=0xD0): FNOP
                // D9 E0   (modrm=0xE0): FCHS
                // D9 E1   (modrm=0xE1): FABS
                // D9 E4   (modrm=0xE4): FTST
                // D9 E8   (modrm=0xE8): FLD1
                // D9 EE   (modrm=0xEE): FLDZ
                // D9 F6   (modrm=0xF6): FDECSTP
                // D9 F7   (modrm=0xF7): FINCSTP
                switch (modrm) {
                    case 0xD0:
                        out.opcode = Opcode::FNOP;
                        return DecodeError::Success;
                    case 0xE0:
                        out.opcode = Opcode::FCHS;
                        return DecodeError::Success;
                    case 0xE1:
                        out.opcode = Opcode::FABS;
                        return DecodeError::Success;
                    case 0xE4:
                        out.opcode = Opcode::FTST;
                        return DecodeError::Success;
                    case 0xE8:
                        out.opcode = Opcode::FLD1;
                        return DecodeError::Success;
                    case 0xEE:
                        out.opcode = Opcode::FLDZ;
                        return DecodeError::Success;
                    case 0xF6:
                        out.opcode = Opcode::FDECSTP;
                        return DecodeError::Success;
                    case 0xF7:
                        out.opcode = Opcode::FINCSTP;
                        return DecodeError::Success;
                    case 0xE5:
                        out.opcode = Opcode::FXAM;
                        return DecodeError::Success;
                    case 0xE9:
                        out.opcode = Opcode::FLDL2T;
                        return DecodeError::Success;
                    case 0xEA:
                        out.opcode = Opcode::FLDL2E;
                        return DecodeError::Success;
                    case 0xEB:
                        out.opcode = Opcode::FLDPI;
                        return DecodeError::Success;
                    case 0xEC:
                        out.opcode = Opcode::FLDLG2;
                        return DecodeError::Success;
                    case 0xED:
                        out.opcode = Opcode::FLDLN2;
                        return DecodeError::Success;
                    // D9 F0..F9 — the x87 transcendental block. Missing
                    // FYL2X in particular breaks log()/pow() in guest CRTs.
                    case 0xF0:
                        out.opcode = Opcode::F2XM1;
                        return DecodeError::Success;
                    case 0xF1:
                        out.opcode = Opcode::FYL2X;
                        return DecodeError::Success;
                    case 0xF2:
                        out.opcode = Opcode::FPTAN;
                        return DecodeError::Success;
                    case 0xF3:
                        out.opcode = Opcode::FPATAN;
                        return DecodeError::Success;
                    case 0xF4:
                        out.opcode = Opcode::FXTRACT;
                        return DecodeError::Success;
                    case 0xF5:
                        out.opcode = Opcode::FPREM1;
                        return DecodeError::Success;
                    case 0xF8:
                        out.opcode = Opcode::FPREM;
                        return DecodeError::Success;
                    case 0xF9:
                        out.opcode = Opcode::FYL2XP1;
                        return DecodeError::Success;
                    case 0xFA:
                        out.opcode = Opcode::FSQRT;
                        return DecodeError::Success;
                    case 0xFB:
                        out.opcode = Opcode::FSINCOS;
                        return DecodeError::Success;
                    case 0xFC:
                        out.opcode = Opcode::FRNDINT;
                        return DecodeError::Success;
                    case 0xFD:
                        out.opcode = Opcode::FSCALE;
                        return DecodeError::Success;
                    case 0xFE:
                        out.opcode = Opcode::FSIN;
                        return DecodeError::Success;
                    case 0xFF:
                        out.opcode = Opcode::FCOS;
                        return DecodeError::Success;
                    default:
                        break;
                }
                if (reg == 0) {
                    // D9 C0+i: FLD ST(i)
                    out.opcode = Opcode::FLD_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                if (reg == 1) {
                    // D9 C8+i: FXCH ST(i)
                    out.opcode = Opcode::FXCH_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                return DecodeError::InvalidOpcode;
            }

            case 0xDA: {
                // DA E9: FUCOMPP (compare ST(0)/ST(1), pop both)
                if (modrm == 0xE9) {
                    out.opcode = Opcode::FUCOMPP;
                    return DecodeError::Success;
                }
                // DA C0+i / C8+i / D0+i / D8+i: FCMOVB/E/BE/U ST(0),ST(i)
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FCMOVB_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 1:
                        out.opcode = Opcode::FCMOVE_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 2:
                        out.opcode = Opcode::FCMOVBE_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 3:
                        out.opcode = Opcode::FCMOVU_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDB: {
                // DB E2: FNCLEX
                if (modrm == 0xE2) {
                    out.opcode = Opcode::FNCLEX;
                    return DecodeError::Success;
                }
                // DB E3: FNINIT
                if (modrm == 0xE3) {
                    out.opcode = Opcode::FNINIT;
                    return DecodeError::Success;
                }
                // DB C0+i / C8+i / D0+i / D8+i: FCMOVNB/NE/NBE/NU
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FCMOVNB_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 1:
                        out.opcode = Opcode::FCMOVNE_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 2:
                        out.opcode = Opcode::FCMOVNBE_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 3:
                        out.opcode = Opcode::FCMOVNU_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    default:
                        break;
                }
                // DB E8+i: FUCOMI ST, ST(i)
                if (reg == 5) {
                    out.opcode = Opcode::FUCOMI_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DB F0+i: FCOMI ST, ST(i)
                if (reg == 6) {
                    out.opcode = Opcode::FCOMI_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                return DecodeError::InvalidOpcode;
            }

            case 0xDC: {
                // DC C0+i: FADD ST(i), ST(0)   reg=0
                // DC C8+i: FMUL ST(i), ST(0)   reg=1
                // DC E0+i: FSUBR ST(i), ST(0)  reg=4
                // DC E8+i: FSUB ST(i), ST(0)   reg=5
                // DC F0+i: FDIVR ST(i), ST(0)  reg=6
                // DC F8+i: FDIV ST(i), ST(0)   reg=7
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FADD_TO_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 1:
                        out.opcode = Opcode::FMUL_TO_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 4:
                        out.opcode = Opcode::FSUBR_TO_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 5:
                        out.opcode = Opcode::FSUB_TO_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 6:
                        out.opcode = Opcode::FDIVR_TO_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 7:
                        out.opcode = Opcode::FDIV_TO_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDD: {
                // DD C0+i (reg=0): FFREE ST(i)
                if (reg == 0) {
                    out.opcode = Opcode::FFREE_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DD D0+i (reg=2): FST ST(i)
                if (reg == 2) {
                    out.opcode = Opcode::FST_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DD D8+i (reg=3): FSTP ST(i)
                if (reg == 3) {
                    out.opcode = Opcode::FSTP_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DD E0+i (reg=4): FUCOM ST(i)
                if (reg == 4) {
                    out.opcode = Opcode::FUCOM_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DD E8+i (reg=5): FUCOMP ST(i)
                if (reg == 5) {
                    out.opcode = Opcode::FUCOMP_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                return DecodeError::InvalidOpcode;
            }

            case 0xDE: {
                // DE C0+i (reg=0): FADDP ST(i),ST(0)
                // DE C8+i (reg=1): FMULP ST(i),ST(0)
                // DE D9   (modrm=0xD9): FCOMPP
                // DE E0+i (reg=4): FSUBRP ST(i),ST(0)
                // DE E8+i (reg=5): FSUBP ST(i),ST(0)
                // DE F0+i (reg=6): FDIVRP ST(i),ST(0)
                // DE F8+i (reg=7): FDIVP ST(i),ST(0)
                if (modrm == 0xD9) {
                    out.opcode = Opcode::FCOMPP;
                    return DecodeError::Success;
                }
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FADDP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 1:
                        out.opcode = Opcode::FMULP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 4:
                        out.opcode = Opcode::FSUBRP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 5:
                        out.opcode = Opcode::FSUBP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 6:
                        out.opcode = Opcode::FDIVRP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    case 7:
                        out.opcode = Opcode::FDIVP_STI;
                        out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                        return DecodeError::Success;
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDF: {
                // DF E0: FNSTSW AX
                if (modrm == 0xE0) {
                    out.opcode = Opcode::FNSTSW_AX;
                    return DecodeError::Success;
                }
                // DF C0+i: FFREEP ST(i) — free ST(i), then pop.
                // Undocumented by Intel but implemented by every x86 CPU and
                // emitted by real toolchains (it is the shortest way to pop
                // the x87 stack without touching a value). Distinct from
                // FFREE (DD C0+i), which does NOT pop: sharing the FFREE
                // lowering would leave TOP one slot low for the rest of the
                // block, which corrupts every subsequent ST(i) reference
                // rather than faulting.
                if (reg == 0) {
                    out.opcode = Opcode::FFREEP_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DF E8+i: FUCOMIP ST, ST(i)
                if (reg == 5) {
                    out.opcode = Opcode::FUCOMIP_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                // DF F0+i: FCOMIP ST, ST(i)
                if (reg == 6) {
                    out.opcode = Opcode::FCOMIP_STI;
                    out.add_operand(Operand::make_imm(OperandType::Imm8, rm));
                    return DecodeError::Success;
                }
                return DecodeError::InvalidOpcode;
            }

            default:
                return DecodeError::InvalidOpcode;
        }
    } else {
        // ---- Memory forms (mod != 3) ----
        switch (byte) {
            case 0xD8: {
                // D8 /0: FADD m32fp
                // D8 /1: FMUL m32fp
                // D8 /2: FCOM m32fp
                // D8 /3: FCOMP m32fp
                // D8 /4: FSUB m32fp
                // D8 /5: FSUBR m32fp
                // D8 /6: FDIV m32fp
                switch (reg) {
                    case 0: {
                        out.opcode = Opcode::FADD_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 1: {
                        out.opcode = Opcode::FMUL_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 2: {
                        out.opcode = Opcode::FCOM_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 3: {
                        out.opcode = Opcode::FCOMP_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 4: {
                        out.opcode = Opcode::FSUB_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 5: {
                        out.opcode = Opcode::FSUBR_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 6: {
                        out.opcode = Opcode::FDIV_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 7: {
                        out.opcode = Opcode::FDIVR_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xD9: {
                // D9 /0: FLD m32fp
                // D9 /3: FSTP m32fp
                // D9 /4: FLDENV m28
                // D9 /5: FLDCW m16
                // D9 /6: FNSTENV m28
                // D9 /7: FNSTCW m16
                switch (reg) {
                    case 0: {
                        out.opcode = Opcode::FLD_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 2: {
                        out.opcode = Opcode::FST_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 3: {
                        out.opcode = Opcode::FSTP_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 4: {
                        out.opcode = Opcode::FLDENV_M28;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 5: {
                        out.opcode = Opcode::FLDCW_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    case 6: {
                        out.opcode = Opcode::FNSTENV_M28;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 7: {
                        out.opcode = Opcode::FNSTCW_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDA: {
                // DA /0..7: FI* m32int (Intel SDM Vol. 2 — FIADD/FIMUL/FICOM/
                // FICOMP/FISUB/FISUBR/FIDIV/FIDIVR). Signed 32-bit integer src.
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FIADD_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 1:
                        out.opcode = Opcode::FIMUL_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 2:
                        out.opcode = Opcode::FICOM_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 3:
                        out.opcode = Opcode::FICOMP_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 4:
                        out.opcode = Opcode::FISUB_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 5:
                        out.opcode = Opcode::FISUBR_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 6:
                        out.opcode = Opcode::FIDIV_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    case 7:
                        out.opcode = Opcode::FIDIVR_M32;
                        return decode_mem_operand(OperandType::Mem32);
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDB: {
                // DB /0: FILD m32int
                // DB /1: FISTTP m32int (SSE3)
                // DB /2: FIST m32int
                // DB /3: FISTP m32int
                // DB /5: FLD m80fp
                // DB /7: FSTP m80fp
                switch (reg) {
                    case 0: {
                        out.opcode = Opcode::FILD_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 1: {
                        out.opcode = Opcode::FISTTP_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 2: {
                        out.opcode = Opcode::FIST_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 3: {
                        out.opcode = Opcode::FISTP_M32;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 5: {
                        out.opcode = Opcode::FLD_M80;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 7: {
                        out.opcode = Opcode::FSTP_M80;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDC: {
                // DC /0: FADD m64fp
                // DC /1: FMUL m64fp
                // DC /2: FCOM m64fp
                // DC /3: FCOMP m64fp
                // DC /4: FSUB m64fp
                // DC /5: FSUBR m64fp
                // DC /6: FDIV m64fp
                // DC /7: FDIVR m64fp
                switch (reg) {
                    case 0: {
                        out.opcode = Opcode::FADD_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 1: {
                        out.opcode = Opcode::FMUL_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 2: {
                        out.opcode = Opcode::FCOM_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 3: {
                        out.opcode = Opcode::FCOMP_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 4: {
                        out.opcode = Opcode::FSUB_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 5: {
                        out.opcode = Opcode::FSUBR_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 6: {
                        out.opcode = Opcode::FDIV_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 7: {
                        out.opcode = Opcode::FDIVR_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDD: {
                // DD /0: FLD m64fp
                // DD /1: FISTTP m64int (SSE3)
                // DD /2: FST m64fp
                // DD /3: FSTP m64fp
                // DD /4: FRSTOR m108byte
                // DD /6: FNSAVE m108byte
                switch (reg) {
                    case 4: {
                        out.opcode = Opcode::FRSTOR_M108;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 6: {
                        out.opcode = Opcode::FNSAVE_M108;
                        auto err = decode_mem_operand(OperandType::Mem32);
                        return err;
                    }
                    case 0: {
                        out.opcode = Opcode::FLD_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 1: {
                        out.opcode = Opcode::FISTTP_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 2: {
                        out.opcode = Opcode::FST_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    case 3: {
                        out.opcode = Opcode::FSTP_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    // DD /7: FNSTSW m2byte. The memory form is easy to miss
                    // next to the far more common FNSTSW AX (DF E0), but MSVC
                    // emits it for float comparisons as
                    //   fnstsw [ebp-4] ; movzx ecx, word [ebp-4]
                    case 7: {
                        out.opcode = Opcode::FNSTSW_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDE: {
                // DE /0..7: FI* m16int (same ops as DA, 16-bit signed integer).
                switch (reg) {
                    case 0:
                        out.opcode = Opcode::FIADD_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 1:
                        out.opcode = Opcode::FIMUL_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 2:
                        out.opcode = Opcode::FICOM_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 3:
                        out.opcode = Opcode::FICOMP_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 4:
                        out.opcode = Opcode::FISUB_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 5:
                        out.opcode = Opcode::FISUBR_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 6:
                        out.opcode = Opcode::FIDIV_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    case 7:
                        out.opcode = Opcode::FIDIVR_M16;
                        return decode_mem_operand(OperandType::Mem16);
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            case 0xDF: {
                // DF /0: FILD m16int
                // DF /1: FISTTP m16int (SSE3)
                // DF /2: FIST m16int
                // DF /3: FISTP m16int
                // DF /5: FILD m64int
                // DF /7: FISTP m64int
                switch (reg) {
                    case 0: {
                        out.opcode = Opcode::FILD_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    case 1: {
                        out.opcode = Opcode::FISTTP_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    case 2: {
                        out.opcode = Opcode::FIST_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    case 3: {
                        out.opcode = Opcode::FISTP_M16;
                        auto err = decode_mem_operand(OperandType::Mem16);
                        return err;
                    }
                    case 5: {
                        out.opcode = Opcode::FILD_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    // DF /4: FBLD m80bcd
                    case 4: {
                        out.opcode = Opcode::FBLD_M80;
                        auto err = decode_mem_operand(OperandType::Mem80);
                        return err;
                    }
                    // DF /6: FBSTP m80bcd
                    case 6: {
                        out.opcode = Opcode::FBSTP_M80;
                        auto err = decode_mem_operand(OperandType::Mem80);
                        return err;
                    }
                    case 7: {
                        out.opcode = Opcode::FISTP_M64;
                        auto err = decode_mem_operand(OperandType::Mem64);
                        return err;
                    }
                    default:
                        return DecodeError::InvalidOpcode;
                }
            }

            default:
                return DecodeError::InvalidOpcode;
        }
    }
}

} // namespace vitreo::cpu::decode
