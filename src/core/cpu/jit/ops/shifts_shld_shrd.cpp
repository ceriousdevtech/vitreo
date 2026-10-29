// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — SHLD / SHRD r/m16 and r/m32 (imm8 / CL).
//
// One family of compile_shifts' opcode space, split out of ops/shifts.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_shifts (ops/shifts.cpp) owns the dispatch chain and the shared
// is_last tail; see the declaration in jit_compiler_lowering.hpp for the
// nullopt / true contract.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_shifts_shld_shrd(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- SHLD / SHRD r/m16 ----
    // Intel SDM Vol. 2: COUNT &= 1Fh. count==0 ⇒ no-op. For 16-bit operands
    // count may be 16..31 (undefined in SDM); match interpreter exec_shld/
    // exec_shrd: result = src <<</>> (count-16), CF from dest for count==16
    // and 0 for count>16 (bits-count < 0 under C++ int promotion).
    case Opcode::SHLD_RM16_R16_IMM8:
    case Opcode::SHRD_RM16_R16_IMM8:
    case Opcode::SHLD_RM16_R16_CL:
    case Opcode::SHRD_RM16_R16_CL: {
        const bool is_shld =
            (instr.opcode == Opcode::SHLD_RM16_R16_IMM8 ||
             instr.opcode == Opcode::SHLD_RM16_R16_CL);
        const bool is_cl =
            (instr.opcode == Opcode::SHLD_RM16_R16_CL ||
             instr.opcode == Opcode::SHRD_RM16_R16_CL);
        const bool is_mem = !is_register_operand(op0.type);

        u8 imm_count = 0;
        if (!is_cl) {
            imm_count = static_cast<u8>(instr.operands[2].imm) & 31;
            if (imm_count == 0) break;
        }

        u32 cbz_idx = 0;
        if (is_cl) {
            emit_load_gpr32(REG_SCRATCH1,
                             static_cast<u8>(RegIndex::RCX));
            // AND W8, W1, #0x1F
            emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));
            cbz_idx = static_cast<u32>(m_code_buf.size());
            emit(0); // CBZ W8, skip
        }

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH4, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH4, op0.reg);
        }
        emit_load_gpr16(REG_SCRATCH1, op1.reg);

        if (is_cl) {
            emit_load_gpr32(REG_SCRATCH3,
                             static_cast<u8>(RegIndex::RCX));
            // AND W8, W3, #0x1F
            emit(arm64::and_w_imm(8, REG_SCRATCH3, 0x1F));

            // count >= 16 → interpreter wide-count path
            emit(arm64::cmp_w_imm(8, 16));
            usize bhs_idx = m_code_buf.size();
            emit(0); // B.CS ge16

            // count < 16: fill = 16 - count
            emit(arm64::movz_w(REG_SCRATCH2, 16));
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, 8));

            if (is_shld) {
                emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH4, 8));
                emit(arm64::lsrv_w(REG_SCRATCH3, REG_SCRATCH1,
                                    REG_SCRATCH2));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH3));
                emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4,
                                    REG_SCRATCH2));
            } else {
                emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH4, 8));
                emit(arm64::lslv_w(REG_SCRATCH3, REG_SCRATCH1,
                                    REG_SCRATCH2));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH3));
                emit(arm64::movz_w(REG_SCRATCH2, 1));
                emit(arm64::sub_w(REG_SCRATCH2, 8, REG_SCRATCH2));
                emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4,
                                    REG_SCRATCH2));
            }
            emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1)); // AND #1
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
            usize b_done_idx = m_code_buf.size();
            emit(0); // B flags_common

            // ge16: result = src <</>> (count-16);
            // CF = (count==16) ? (dest & 1) : 0
            usize ge16_pos = m_code_buf.size();
            m_code_buf[bhs_idx] = arm64::b_cond(
                arm64::CS, static_cast<i32>(ge16_pos - bhs_idx) * 4);
            emit(arm64::movz_w(REG_SCRATCH3, 16));
            emit(arm64::sub_w(REG_SCRATCH3, 8, REG_SCRATCH3)); // count-16
            if (is_shld) {
                emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH3));
            } else {
                emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH3));
            }
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
            // CF: SHLD count==16 → dest[0]; SHRD → (dest>>(count-1))&1
            //     (for count>16 both yield 0 with 16-bit dest except SHRD@16).
            if (is_shld) {
                emit(arm64::cmp_w_imm(8, 16));
                emit(arm64::cset_w(REG_SCRATCH2, arm64::EQ));
                emit(arm64::and_w_imm(REG_SCRATCH3, REG_SCRATCH4, 0x1)); // AND W3,W4,#1
                emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
            } else {
                emit(arm64::movz_w(REG_SCRATCH2, 1));
                emit(arm64::sub_w(REG_SCRATCH2, 8, REG_SCRATCH2)); // count-1
                emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH2));
                emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1));
            }

            usize flags_pos = m_code_buf.size();
            m_code_buf[b_done_idx] = arm64::b_imm(
                static_cast<i32>(flags_pos - b_done_idx) * 4);
        } else if (imm_count >= 16) {
            const u8 excess = static_cast<u8>(imm_count - 16);
            if (is_shld) {
                if (excess == 0) {
                    emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH1));
                } else {
                    emit(arm64::lsl_w_imm(REG_SCRATCH0, REG_SCRATCH1, excess));
                }
            } else {
                if (excess == 0) {
                    emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH1));
                } else {
                    emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH1, excess));
                }
            }
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
            if (is_shld) {
                if (imm_count == 16) {
                    emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH4, 0x1)); // dest&1
                } else {
                    emit(arm64::movz_w(REG_SCRATCH2, 0));
                }
            } else {
                // CF = (dest >> (count-1)) & 1
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(imm_count - 1)));
                emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1));
            }
        } else {
            if (is_shld) {
                emit(arm64::lsl_w_imm(REG_SCRATCH0, REG_SCRATCH4,
                                       imm_count));
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH1,
                                       static_cast<u8>(16 - imm_count)));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH2));
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(16 - imm_count)));
            } else {
                emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH4,
                                       imm_count));
                emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH1,
                                       static_cast<u8>(16 - imm_count)));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH2));
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(imm_count - 1)));
            }
            emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1)); // AND #1
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        }

        // Eager CF/ZSP/(OF) — 16-bit SF/OF use bit 15.
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
            emit_mov_imm32(REG_SCRATCH1, 0xC5u);
            emit(0x8A210063u); // BIC X3, X3, X1
            emit(0xAA020063u); // ORR X3, X3, X2 (CF)

            emit(arm64::cmp_w_imm(REG_SCRATCH0, 0));
            emit(arm64::cset_w(REG_SCRATCH1, arm64::EQ));
            emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 6));
            emit(0xAA010063u);

            emit(0x530F3C01u); // UBFX W1, W0, #15, #1
            emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 7));
            emit(0xAA010063u);

            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_CALL_TMP, REG_SCRATCH1, 4));
            emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1, REG_CALL_TMP));
            emit_mov_imm32(REG_CALL_TMP, 0xFu);
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_CALL_TMP));
            emit_mov_imm32(REG_CALL_TMP, 0x9669u);
            emit(arm64::lsrv_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH1));
            emit_mov_imm32(REG_SCRATCH1, 1);
            emit(arm64::and_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH1));
            emit(arm64::lsl_w_imm(REG_CALL_TMP, REG_CALL_TMP, 2));
            emit(0xAA150063u);

            if (is_cl || imm_count == 1) {
                emit(0x530F3C01u); // UBFX W1, W0, #15, #1 (result)
                emit(0x530F3C82u); // UBFX W2, W4, #15, #1 (orig)
                emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1,
                                   REG_SCRATCH2));
                emit_mov_imm32(REG_SCRATCH2, 0x800u);
                emit(0x8A220063u);
                emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 11));
                emit(0xAA010063u);
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }

        if (is_cl) {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }
        break;
    }

    // ---- SHLD / SHRD r/m32 ----
    // Intel SDM Vol. 2: COUNT := COUNT & 1Fh. count==0 ⇒ no op / flags
    // unchanged. Else result combines DEST with SRC; CF = last bit shifted
    // out of DEST; ZSP from result; OF defined for count==1 (sign change),
    // and for CL forms the interpreter also updates OF on any nonzero count.
    case Opcode::SHLD_RM32_R32_IMM8:
    case Opcode::SHRD_RM32_R32_IMM8:
    case Opcode::SHLD_RM32_R32_CL:
    case Opcode::SHRD_RM32_R32_CL: {
        const bool is_shld =
            (instr.opcode == Opcode::SHLD_RM32_R32_IMM8 ||
             instr.opcode == Opcode::SHLD_RM32_R32_CL);
        const bool is_cl =
            (instr.opcode == Opcode::SHLD_RM32_R32_CL ||
             instr.opcode == Opcode::SHRD_RM32_R32_CL);
        const bool is_mem = !is_register_operand(op0.type);

        u8 imm_count = 0;
        if (!is_cl) {
            imm_count = static_cast<u8>(instr.operands[2].imm) & 31;
            if (imm_count == 0) break;
        }

        u32 cbz_idx = 0;
        if (is_cl) {
            emit_load_gpr32(REG_SCRATCH1,
                             static_cast<u8>(RegIndex::RCX));
            // AND W8, W1, #0x1F
            emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));
            cbz_idx = static_cast<u32>(m_code_buf.size());
            emit(0); // CBZ W8, skip
        }

        // Discard the pending FlagOp before the eager rflags write below.
        //
        // Intel SDM Vol. 2B (SHLD/SHRD): for a non-zero count CF is the last
        // bit shifted out and SF/ZF/PF come from the result; AF is undefined,
        // and OF is defined only for count == 1. Every architecturally-defined
        // flag is rewritten below, so a pending lazy op contributes nothing
        // that survives it -- only AF, and OF when count > 1, could carry
        // over, and the SDM leaves both undefined. Calling the materialize
        // helper to compute flags that are then overwritten is pure cost.
        //
        // Measured on one load: this one emission site was 9,871,370 of
        // 15.4M materialize calls (64% of all flag materialization), because
        // SHLD sits in the inner loop of a table-driven hash
        // (movzx/shld/shr/xor/table-xor).
        //
        // The overlays must still be dropped: they hold a pending CF/OF that
        // would otherwise be applied on top of the rflags written below, and
        // CF here is architectural, not undefined. nzcv_kind must be cleared
        // for the same reason -- it advertises a carried ARM NZCV that a later
        // Jcc would consult in preference to rflags. nzcv_bits needs no store;
        // it is only read when nzcv_kind != 0.
        emit(arm64::strb_uoff(31, REG_CPU_STATE, flag_op_offset()));
        emit_clear_cf_overlay();
        emit_store_nzcv_kind(NzcvState::None);

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH4, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH4, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, op1.reg);

        if (is_cl) {
            emit_load_gpr32(REG_SCRATCH3,
                             static_cast<u8>(RegIndex::RCX));
            // AND W8, W3, #0x1F (count); W3 reused below for rflags.
            emit(arm64::and_w_imm(8, REG_SCRATCH3, 0x1F));

            // fill = 32 - count
            emit(arm64::movz_w(REG_SCRATCH2, 32));
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, 8));

            if (is_shld) {
                // result = (dest << count) | (src >> (32-count))
                emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH4, 8));
                emit(arm64::lsrv_w(REG_SCRATCH3, REG_SCRATCH1,
                                    REG_SCRATCH2));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH3));
                // CF = (dest >> (32-count)) & 1
                emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4,
                                    REG_SCRATCH2));
            } else {
                // result = (dest >> count) | (src << (32-count))
                emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH4, 8));
                emit(arm64::lslv_w(REG_SCRATCH3, REG_SCRATCH1,
                                    REG_SCRATCH2));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH3));
                // CF = (dest >> (count-1)) & 1
                emit(arm64::movz_w(REG_SCRATCH2, 1));
                emit(arm64::sub_w(REG_SCRATCH2, 8, REG_SCRATCH2));
                emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4,
                                    REG_SCRATCH2));
            }
            emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1)); // AND #1
        } else {
            if (is_shld) {
                emit(arm64::lsl_w_imm(REG_SCRATCH0, REG_SCRATCH4,
                                       imm_count));
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH1,
                                       static_cast<u8>(32 - imm_count)));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH2));
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(32 - imm_count)));
            } else {
                emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH4,
                                       imm_count));
                emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH1,
                                       static_cast<u8>(32 - imm_count)));
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH2));
                emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(imm_count - 1)));
            }
            emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1)); // AND #1
        }

        // Eager CF/ZSP/(OF) into rflags — mirrors jit_shld32/jit_shrd32 and
        // interpreter exec_shld/exec_shrd (OF policy differs for CL).
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
            // Clear CF|PF|ZF|SF (= 0xC5). Leave OF unless we rewrite it.
            emit_mov_imm32(REG_SCRATCH1, 0xC5u);
            emit(0x8A210063u); // BIC X3, X3, X1
            emit(0xAA020063u); // ORR X3, X3, X2 (CF)

            // ZF
            emit(arm64::cmp_w_imm(REG_SCRATCH0, 0));
            emit(arm64::cset_w(REG_SCRATCH1, arm64::EQ));
            emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 6));
            emit(0xAA010063u); // ORR X3, X3, X1

            // SF = result bit 31
            emit(0x531F7C01u); // UBFX W1, W0, #31, #1
            emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 7));
            emit(0xAA010063u);

            // PF = even parity of result[7:0] via 0x9669 nibble LUT
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_CALL_TMP, REG_SCRATCH1, 4));
            emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1, REG_CALL_TMP));
            emit_mov_imm32(REG_CALL_TMP, 0xFu);
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_CALL_TMP));
            emit_mov_imm32(REG_CALL_TMP, 0x9669u);
            emit(arm64::lsrv_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH1));
            emit_mov_imm32(REG_SCRATCH1, 1);
            emit(arm64::and_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH1));
            emit(arm64::lsl_w_imm(REG_CALL_TMP, REG_CALL_TMP, 2));
            emit(0xAA150063u); // ORR X3, X3, X21 (REG_CALL_TMP)

            // OF: IMM only when count==1; CL for any nonzero count (interpreter).
            if (is_cl || imm_count == 1) {
                // OF = MSB(orig) XOR MSB(result)
                emit(0x531F7C01u); // UBFX W1, W0, #31, #1 (result)
                emit(0x531F7C82u); // UBFX W2, W4, #31, #1 (orig)
                emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1,
                                   REG_SCRATCH2));
                emit_mov_imm32(REG_SCRATCH2, 0x800u);
                emit(0x8A220063u); // BIC X3, X3, X2
                emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 11));
                emit(0xAA010063u); // ORR X3, X3, X1
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }

        if (is_cl) {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
