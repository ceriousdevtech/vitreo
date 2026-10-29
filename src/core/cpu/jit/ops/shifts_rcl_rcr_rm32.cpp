// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — RCL / RCR r/m32 (imm8 / 1 / CL).
//
// One family of compile_shifts' opcode space, split out of ops/shifts.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_shifts (ops/shifts.cpp) owns the dispatch chain and the shared
// is_last tail; see the declaration in jit_compiler_lowering.hpp for the
// nullopt / true contract.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_shifts_rcl_rcr_rm32(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- RCL / RCR r/m32 ----
    // Intel SDM Vol. 2: rotate through CF in a 33-bit space. Count = count & 31;
    // count==0 leaves dest and flags unchanged. CF via write_cf_only overlay so
    // a pending FlagOp's ZF/SF/PF survive (matches Interpreter::exec_rcl/rcr).
    case Opcode::RCL_RM32_IMM8:
    case Opcode::RCL_RM32_1:
    case Opcode::RCR_RM32_IMM8:
    case Opcode::RCR_RM32_1: {
        const bool is_rcl =
            (instr.opcode == Opcode::RCL_RM32_IMM8 ||
             instr.opcode == Opcode::RCL_RM32_1);
        u8 shift =
            (instr.opcode == Opcode::RCL_RM32_1 ||
             instr.opcode == Opcode::RCR_RM32_1)
                ? 1
                : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;

        bool is_mem = !is_register_operand(op0.type);

        // lazy_cf() → W0, preserve in X21 across loads.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_CAPTURE_CF >> 3));
        emit_helper_blr();
        emit(arm64::mov_x(REG_CALL_TMP, 0));

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }

        // OF for RCR count==1: MSB(orig) XOR old CF (before mutation).
        // Stash in W9 — rotate temps clobber W2.
        if (!is_rcl && shift == 1) {
            emit(0x531F7C02u); // UBFX W2, W0, #31, #1
            emit(0x4A150042u); // EOR W2, W2, W21
            emit(arm64::mov_w(9, REG_SCRATCH2));
        }

        // ext = value | (cf << 32) in X0
        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_CALL_TMP, 32));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        if (is_rcl) {
            // rot = (ext << n) | (ext >> (33-n))
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH0, shift));
            emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0,
                                  static_cast<u8>(33 - shift)));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        } else {
            emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, shift));
            emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH0,
                                  static_cast<u8>(33 - shift)));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        }

        // new_cf = bit 32; result = low 32
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH0, 32));
        emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH4, 0x1)); // AND #1
        emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH0));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(0x8A210063u); // BIC X3, X3, X1
            emit(0xAA040063u); // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                emit(arm64::movz_w(REG_SCRATCH1, 0x800));
                emit(0x8A210063u); // BIC X3, X3, X1
                if (is_rcl) {
                    // OF = MSB(result) XOR new CF
                    emit(0x531F7C02u); // UBFX W2, W0, #31, #1
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
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::RCL_RM32_CL:
    case Opcode::RCR_RM32_CL: {
        const bool is_rcl = (instr.opcode == Opcode::RCL_RM32_CL);
        bool is_mem = !is_register_operand(op0.type);

        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #31
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0); // CBZ W8, end

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_CAPTURE_CF >> 3));
        emit_helper_blr();
        emit(arm64::mov_x(REG_CALL_TMP, 0));

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }

        // Re-read masked count (helper may have clobbered W8).
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));

        // OF for RCR count==1 before mutation.
        if (!is_rcl) {
            emit(arm64::cmp_w_imm(8, 1));
            const usize of_pre_skip = m_code_buf.size();
            emit(0u);
            emit(0x531F7C02u); // UBFX W2, W0, #31, #1
            emit(0x4A150042u); // EOR W2, W2, W21
            // Stash OF candidate in W9 (caller-saved but we don't call again).
            emit(arm64::mov_w(9, REG_SCRATCH2));
            const usize of_pre_done = m_code_buf.size();
            m_code_buf[of_pre_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_pre_done - of_pre_skip) * 4);
        }

        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_CALL_TMP, 32));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        // 33 - count in W7 for the complementary shift.
        emit(arm64::movz_w(7, 33));
        emit(arm64::sub_w(7, 7, 8));

        if (is_rcl) {
            emit(arm64::lslv_x(REG_SCRATCH1, REG_SCRATCH0, 8));
            emit(arm64::lsrv_x(REG_SCRATCH2, REG_SCRATCH0, 7));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        } else {
            emit(arm64::lsrv_x(REG_SCRATCH1, REG_SCRATCH0, 8));
            emit(arm64::lslv_x(REG_SCRATCH2, REG_SCRATCH0, 7));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2));
        }

        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH0, 32));
        emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH4, 0x1));
        emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH0));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(0x8A210063u);
            emit(0xAA040063u);
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);
            emit(arm64::movz_w(REG_SCRATCH1, 0x800));
            emit(0x8A210063u);
            if (is_rcl) {
                emit(0x531F7C02u);
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
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        {
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
