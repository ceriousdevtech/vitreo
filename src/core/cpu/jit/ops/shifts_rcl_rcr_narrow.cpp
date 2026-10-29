// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — RCL / RCR r/m8 and r/m16 (imm8 / 1 / CL).
//
// One family of compile_shifts' opcode space, split out of ops/shifts.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_shifts (ops/shifts.cpp) owns the dispatch chain and the shared
// is_last tail; see the declaration in jit_compiler_lowering.hpp for the
// nullopt / true contract.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_shifts_rcl_rcr_narrow(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- RCL / RCR r/m8 and r/m16 (IMM8 / 1) ----
    // Intel SDM Vol. 2: tempCOUNT = (COUNT & 1FH) MOD 9 (8-bit) or MOD 17
    // (16-bit). Rotate through CF in a (bits+1)-bit space. count==0 or
    // tempCOUNT==0 leaves dest/flags unchanged. OF only when masked COUNT==1.
    case Opcode::RCL_RM8_IMM8:
    case Opcode::RCL_RM8_1:
    case Opcode::RCR_RM8_IMM8:
    case Opcode::RCR_RM8_1:
    case Opcode::RCL_RM16_IMM8:
    case Opcode::RCL_RM16_1:
    case Opcode::RCR_RM16_IMM8:
    case Opcode::RCR_RM16_1: {
        const bool is_8 =
            (instr.opcode == Opcode::RCL_RM8_IMM8 ||
             instr.opcode == Opcode::RCL_RM8_1 ||
             instr.opcode == Opcode::RCR_RM8_IMM8 ||
             instr.opcode == Opcode::RCR_RM8_1);
        const bool is_rcl =
            (instr.opcode == Opcode::RCL_RM8_IMM8 ||
             instr.opcode == Opcode::RCL_RM8_1 ||
             instr.opcode == Opcode::RCL_RM16_IMM8 ||
             instr.opcode == Opcode::RCL_RM16_1);
        const bool is_one =
            (instr.opcode == Opcode::RCL_RM8_1 ||
             instr.opcode == Opcode::RCR_RM8_1 ||
             instr.opcode == Opcode::RCL_RM16_1 ||
             instr.opcode == Opcode::RCR_RM16_1);
        const u8 bits = is_8 ? 8 : 16;
        const u8 width = static_cast<u8>(bits + 1);
        const u8 raw =
            is_one ? 1 : static_cast<u8>(op1.imm & 31);
        if (raw == 0) break;
        const u8 shift = static_cast<u8>(raw % width);
        if (shift == 0) break;

        bool is_mem = !is_register_operand(op0.type);

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_CAPTURE_CF >> 3));
        emit_helper_blr();
        emit(arm64::mov_x(REG_CALL_TMP, 0));

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            if (is_8) {
                emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            }
        } else if (is_8) {
            emit_load_gpr8(REG_SCRATCH0, op0.reg,
                           instr.prefixes.has_rex());
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }

        // OF for RCR COUNT==1: MSB(orig) XOR old CF (before mutation).
        if (!is_rcl && raw == 1) {
            if (is_8) {
                emit(0x53071C02u); // UBFX W2, W0, #7, #1
            } else {
                emit(0x530F3C02u); // UBFX W2, W0, #15, #1
            }
            emit(0x4A150042u); // EOR W2, W2, W21
            emit(arm64::mov_w(9, REG_SCRATCH2));
        }

        // ext = value | (cf << bits)
        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_CALL_TMP, bits));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        if (is_rcl) {
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH0, shift));
            emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0,
                                  static_cast<u8>(width - shift)));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        } else {
            emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, shift));
            emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH0,
                                  static_cast<u8>(width - shift)));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        }

        // new_cf = bit `bits`; result = low `bits`
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH0, bits));
        emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH4, 0x1)); // AND #1
        if (is_8) {
            emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        } else {
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        }

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(0x8A210063u); // BIC X3, X3, X1
            emit(0xAA040063u); // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (raw == 1) {
                emit(arm64::movz_w(REG_SCRATCH1, 0x800));
                emit(0x8A210063u); // BIC X3, X3, X1
                if (is_rcl) {
                    if (is_8) {
                        emit(0x53071C02u); // UBFX W2, W0, #7, #1
                    } else {
                        emit(0x530F3C02u); // UBFX W2, W0, #15, #1
                    }
                    emit(0x4A040042u); // EOR W2, W2, W4
                } else {
                    emit(arm64::mov_w(REG_SCRATCH2, 9));
                }
                emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH2, 1));
                emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                      of_overlay_offset()));
                emit(0x53155042u); // LSL W2, W2, #11
                emit(0xAA020063u); // ORR X3, X3, X2
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            if (is_8) {
                emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
            }
        } else if (is_8) {
            emit_store_gpr8(REG_SCRATCH0, op0.reg,
                            instr.prefixes.has_rex());
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::RCL_RM8_CL:
    case Opcode::RCR_RM8_CL:
    case Opcode::RCL_RM16_CL:
    case Opcode::RCR_RM16_CL: {
        const bool is_8 =
            (instr.opcode == Opcode::RCL_RM8_CL ||
             instr.opcode == Opcode::RCR_RM8_CL);
        const bool is_rcl =
            (instr.opcode == Opcode::RCL_RM8_CL ||
             instr.opcode == Opcode::RCL_RM16_CL);
        const u8 bits = is_8 ? 8 : 16;
        const u8 width = static_cast<u8>(bits + 1);
        bool is_mem = !is_register_operand(op0.type);

        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #31
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0); // CBZ W8, end (masked count == 0)

        // tempCOUNT = (COUNT & 31) % width; skip if 0 (full cycles).
        emit(arm64::movz_w(7, width));
        emit(arm64::udiv_x(1, 8, 7));       // X1 = count / width
        emit(arm64::msub_x(8, 1, 7, 8));   // X8 = count - quot*width
        u32 cbz_eff_idx = static_cast<u32>(m_code_buf.size());
        emit(0); // CBZ W8, end (eff == 0)

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_CAPTURE_CF >> 3));
        emit_helper_blr();
        emit(arm64::mov_x(REG_CALL_TMP, 0));

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            if (is_8) {
                emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            }
        } else if (is_8) {
            emit_load_gpr8(REG_SCRATCH0, op0.reg,
                           instr.prefixes.has_rex());
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }

        // Re-read masked COUNT for OF (COUNT==1) and restore eff in W8.
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(10, REG_SCRATCH1, 0x1F)); // AND W10, W1, #31
        emit(arm64::movz_w(7, width));
        emit(arm64::udiv_x(1, 10, 7));
        emit(arm64::msub_x(8, 1, 7, 10)); // W8 = W10 % width

        if (!is_rcl) {
            emit(arm64::cmp_w_imm(10, 1));
            const usize of_pre_skip = m_code_buf.size();
            emit(0u);
            if (is_8) {
                emit(0x53071C02u); // UBFX W2, W0, #7, #1
            } else {
                emit(0x530F3C02u); // UBFX W2, W0, #15, #1
            }
            emit(0x4A150042u); // EOR W2, W2, W21
            emit(arm64::mov_w(9, REG_SCRATCH2));
            const usize of_pre_done = m_code_buf.size();
            m_code_buf[of_pre_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_pre_done - of_pre_skip) * 4);
        }

        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_CALL_TMP, bits));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        emit(arm64::movz_w(7, width));
        emit(arm64::sub_w(7, 7, 8)); // W7 = width - eff

        if (is_rcl) {
            emit(arm64::lslv_x(REG_SCRATCH1, REG_SCRATCH0, 8));
            emit(arm64::lsrv_x(REG_SCRATCH2, REG_SCRATCH0, 7));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        } else {
            emit(arm64::lsrv_x(REG_SCRATCH1, REG_SCRATCH0, 8));
            emit(arm64::lslv_x(REG_SCRATCH2, REG_SCRATCH0, 7));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        }

        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH0, bits));
        emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH4, 0x1));
        if (is_8) {
            emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        } else {
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        }

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(0x8A210063u);
            emit(0xAA040063u);
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            // OF only when masked COUNT == 1 (W10), not when tempCOUNT == 1.
            emit(arm64::cmp_w_imm(10, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);
            emit(arm64::movz_w(REG_SCRATCH1, 0x800));
            emit(0x8A210063u);
            if (is_rcl) {
                if (is_8) {
                    emit(0x53071C02u);
                } else {
                    emit(0x530F3C02u);
                }
                emit(0x4A040042u);
            } else {
                emit(arm64::mov_w(REG_SCRATCH2, 9));
            }
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH2, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(0x53155042u);
            emit(0xAA020063u);
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            if (is_8) {
                emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
            }
        } else if (is_8) {
            emit_store_gpr8(REG_SCRATCH0, op0.reg,
                            instr.prefixes.has_rex());
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
            m_code_buf[cbz_eff_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_eff_idx) * 4);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
