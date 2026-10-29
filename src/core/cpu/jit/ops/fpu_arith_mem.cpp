// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: memory arithmetic. FADD/FMUL/FSUB/FSUBR/
// FDIV/FDIVR with an m32real or m64real source (Intel SDM Vol. 1 8.3.5).
//
// One family of compile_fpu_native's native lowerings, split out of
// ops/fpu.cpp (ADR-0006) as pure code motion: the blocks are verbatim and in
// their original order. compile_fpu_native (ops/fpu.cpp) owns the dispatch
// chain and the jit_fpu_op helper fallback; the declaration in
// jit_compiler.hpp states what the nullopt return means.

#include "../jit_helpers.hpp"
#include "fpu_internal.hpp"

#include <optional>

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_fpu_arith_mem(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const bool native_fpu_arith = native_arith_allowed(instr.opcode);

    const auto& op0 = instr.operands[0];

    // Same finite-normal gate as STI arith.
    if (native_fpu_arith &&
        (instr.opcode == Opcode::FADD_M32 ||
         instr.opcode == Opcode::FMUL_M32 ||
         instr.opcode == Opcode::FSUB_M32 ||
         instr.opcode == Opcode::FSUBR_M32 ||
         instr.opcode == Opcode::FDIV_M32 ||
         instr.opcode == Opcode::FDIVR_M32) &&
        is_memory_operand(op0.type)) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 double_exp_bias_delta = 16383u - 1023u;
        constexpr u32 min_double_x87_exp = double_exp_bias_delta + 1u;
        constexpr u32 max_double_x87_exp = double_exp_bias_delta + 0x7FEu;

        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_load32(REG_EA_TMP, REG_SCRATCH2); // W5 = m32 bits
        emit(arm64::mov_w(17, REG_EA_TMP));

        // X3 = &fpu.st[fpu.top()]. Float80 has a packed 10-byte stride.
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 11, 3));
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                             REG_SCRATCH2, 0));

        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));

        // Extract the x87 exponent. Zero maps to signed binary64 zero; only
        // normal values representable by Float80::to_double stay native.
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FFF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH1, REG_SCRATCH4));
        const usize st_zero_branch = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, min_double_x87_exp);
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize st_below_branch = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, max_double_x87_exp);
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize st_above_branch = m_code_buf.size();
        emit(0);

        // Normal Float80 -> binary64 bits.
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 12));
        emit_mov_imm32(REG_EA_TMP, double_exp_bias_delta);
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 52));
        emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 63));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH2));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        const usize st_ready_branch = m_code_buf.size();
        emit(0);

        const usize st_zero_pos = m_code_buf.size();
        emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH1, 63));
        const usize st_ready_pos = m_code_buf.size();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_s_from_w(1, 17));
        emit(arm64::fcvt_d_from_s(1, 1));
        if (instr.opcode == Opcode::FADD_M32) {
            emit(arm64::fadd_s_scalar(0, 0, 1, arm64::FS_D));
        } else if (instr.opcode == Opcode::FMUL_M32) {
            emit(arm64::fmul_s_scalar(0, 0, 1, arm64::FS_D));
        } else if (instr.opcode == Opcode::FSUBR_M32) {
            emit(arm64::fsub_s_scalar(0, 1, 0, arm64::FS_D));
        } else if (instr.opcode == Opcode::FDIV_M32) {
            emit(arm64::fdiv_s_scalar(0, 0, 1, arm64::FS_D));
        } else if (instr.opcode == Opcode::FDIVR_M32) {
            emit(arm64::fdiv_s_scalar(0, 1, 0, arm64::FS_D));
        } else {
            emit(arm64::fsub_s_scalar(0, 0, 1, arm64::FS_D));
        }

        // binary64 -> canonical Float80. Float80(double) flushes binary64
        // subnormals to signed zero; infinities/NaNs use the helper fallback.
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0, 52));
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        // A biased exponent of 0 means +-0 OR A SUBNORMAL. Treating the whole
        // class as zero flushed every subnormal RESULT -- measured against
        // Windows, log(1e-320) returned -inf instead of -736.827 because the
        // preceding FADD wrote its subnormal sum as 0. The FLD_M64 gate already
        // distinguishes the two; do the same here and let a subnormal fall
        // through to the helper, which handles it exactly.
        const usize result_nonzero_exp_branch = m_code_buf.size();
        emit(0);   // CBNZ SCRATCH2 -> ordinary exponent check
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));  // drop sign
        const usize result_zero_branch = m_code_buf.size();
        emit(0);   // CBZ SCRATCH4 -> a true +-0
        const usize result_subnormal_branch = m_code_buf.size();
        emit(0);   // B -> slow path (subnormal)
        const usize result_exp_check_pos = m_code_buf.size();
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));  // reload: clobbered above
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize result_special_branch = m_code_buf.size();
        emit(0);

        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 12));
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit_mov_imm64(REG_SCRATCH1,
                       static_cast<i64>(0x8000000000000000ULL));
        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH1, double_exp_bias_delta);
        emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
        emit(arm64::str_x_uoff(REG_SCRATCH4, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH2, REG_SCRATCH4, 31));
        const usize normal_done_branch = m_code_buf.size();
        emit(0);

        const usize result_zero_pos = m_code_buf.size();
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::str_x_uoff(31, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        const usize zero_done_branch = m_code_buf.size();
        emit(0);

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        const u32 packed = (kFpuKindM32Value << 28) |
            (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);
        emit_mov_imm32(2, packed);
        emit(arm64::mov_w(3, 17));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
        emit_semantic_helper_blr(); // blr x16

        const usize done_pos = m_code_buf.size();
        const auto patch_cond = [&](usize branch, usize target, u8 cond) {
            m_code_buf[branch] = arm64::b_cond(
                cond, static_cast<i32>(target - branch) * 4);
        };
        const auto patch_b = [&](usize branch, usize target) {
            m_code_buf[branch] = arm64::b_imm(
                static_cast<i32>(target - branch) * 4);
        };
        m_code_buf[st_zero_branch] = arm64::cbz_w(
            REG_SCRATCH2,
            static_cast<i32>(st_zero_pos - st_zero_branch) * 4);
        patch_cond(st_below_branch, slow_pos, arm64::CC);
        patch_cond(st_above_branch, slow_pos, arm64::HI);
        patch_b(st_ready_branch, st_ready_pos);
        m_code_buf[result_nonzero_exp_branch] = arm64::cbnz_w(
            REG_SCRATCH2,
            static_cast<i32>(result_exp_check_pos -
                             result_nonzero_exp_branch) * 4);
        m_code_buf[result_zero_branch] = arm64::cbz_x(
            REG_SCRATCH4,
            static_cast<i32>(result_zero_pos - result_zero_branch) * 4);
        m_code_buf[result_subnormal_branch] = arm64::b_imm(
            static_cast<i32>(slow_pos - result_subnormal_branch) * 4);
        patch_cond(result_special_branch, slow_pos, arm64::EQ);
        patch_b(normal_done_branch, done_pos);
        patch_b(zero_done_branch, done_pos);

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Intel SDM Vol. 2 FADD/FMUL/FSUB/FDIV m64fp: same finite-normal gate,
    // but the memory operand is already binary64.
    if (native_fpu_arith &&
        (instr.opcode == Opcode::FADD_M64 ||
         instr.opcode == Opcode::FMUL_M64 ||
         instr.opcode == Opcode::FSUB_M64 ||
         instr.opcode == Opcode::FSUBR_M64 ||
         instr.opcode == Opcode::FDIV_M64 ||
         instr.opcode == Opcode::FDIVR_M64) &&
        is_memory_operand(op0.type)) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 double_exp_bias_delta = 16383u - 1023u;
        constexpr u32 min_double_x87_exp = double_exp_bias_delta + 1u;
        constexpr u32 max_double_x87_exp = double_exp_bias_delta + 0x7FEu;
        const bool forwards_d0 = instr.opcode == Opcode::FADD_M64 ||
                                 instr.opcode == Opcode::FMUL_M64;
        const bool consume_d0 = m_x87_d0_forward_in && forwards_d0;
        const bool publish_d0 = m_x87_d0_forward_out &&
                                forwards_d0;

        if (consume_d0) {
            // The m64 operand read may enter a C helper. AAPCS makes D0/W6
            // caller-saved, so stage the adjacent mirror in Vitreo's internal
            // two-qword vector scratch across that read. The operand is still
            // read before any arithmetic/state commit, preserving x86 fault
            // ordering.
            const u32 scratch = jit_vec_scratch_off();
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, scratch >> 4));
            emit(arm64::str_w_uoff(kX87D0Valid, REG_CPU_STATE,
                                   (scratch + 16) >> 2));
        }
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        emit(arm64::mov_x(17, REG_SCRATCH0));
        if (consume_d0) {
            const u32 scratch = jit_vec_scratch_off();
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, scratch >> 4));
            emit(arm64::ldr_w_uoff(kX87D0Valid, REG_CPU_STATE,
                                   (scratch + 16) >> 2));
        }

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 11, 3));
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                             REG_SCRATCH2, 0));

        usize forwarded_branch = 0;
        if (consume_d0) {
            forwarded_branch = m_code_buf.size();
            emit(0);  // CBNZ W6 -> D0 already mirrors architectural ST(0)
        }

        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));

        emit(arm64::movz_w(REG_SCRATCH4, 0x7FFF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH1, REG_SCRATCH4));
        const usize st_zero_branch = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, min_double_x87_exp);
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize st_below_branch = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, max_double_x87_exp);
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize st_above_branch = m_code_buf.size();
        emit(0);

        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 12));
        emit_mov_imm32(REG_EA_TMP, double_exp_bias_delta);
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 52));
        emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 63));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH2));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        const usize st_ready_branch = m_code_buf.size();
        emit(0);

        const usize st_zero_pos = m_code_buf.size();
        emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH1, 63));
        const usize st_ready_pos = m_code_buf.size();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        const usize forwarded_ready_pos = m_code_buf.size();
        emit(arm64::fmov_d_from_x(1, 17));
        if (instr.opcode == Opcode::FADD_M64) {
            emit(arm64::fadd_s_scalar(0, 0, 1, arm64::FS_D));
        } else if (instr.opcode == Opcode::FMUL_M64) {
            emit(arm64::fmul_s_scalar(0, 0, 1, arm64::FS_D));
        } else if (instr.opcode == Opcode::FSUBR_M64) {
            emit(arm64::fsub_s_scalar(0, 1, 0, arm64::FS_D));
        } else if (instr.opcode == Opcode::FDIV_M64) {
            emit(arm64::fdiv_s_scalar(0, 0, 1, arm64::FS_D));
        } else if (instr.opcode == Opcode::FDIVR_M64) {
            emit(arm64::fdiv_s_scalar(0, 1, 0, arm64::FS_D));
        } else {
            emit(arm64::fsub_s_scalar(0, 0, 1, arm64::FS_D));
        }

        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0, 52));
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        // A biased exponent of 0 means +-0 OR A SUBNORMAL. Treating the whole
        // class as zero flushed every subnormal RESULT -- measured against
        // Windows, log(1e-320) returned -inf instead of -736.827 because the
        // preceding FADD wrote its subnormal sum as 0. The FLD_M64 gate already
        // distinguishes the two; do the same here and let a subnormal fall
        // through to the helper, which handles it exactly.
        const usize result_nonzero_exp_branch = m_code_buf.size();
        emit(0);   // CBNZ SCRATCH2 -> ordinary exponent check
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));  // drop sign
        const usize result_zero_branch = m_code_buf.size();
        emit(0);   // CBZ SCRATCH4 -> a true +-0
        const usize result_subnormal_branch = m_code_buf.size();
        emit(0);   // B -> slow path (subnormal)
        const usize result_exp_check_pos = m_code_buf.size();
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));  // reload: clobbered above
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize result_special_branch = m_code_buf.size();
        emit(0);

        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 12));
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit_mov_imm64(REG_SCRATCH1,
                       static_cast<i64>(0x8000000000000000ULL));
        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH1, double_exp_bias_delta);
        emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
        emit(arm64::str_x_uoff(REG_SCRATCH4, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH2, REG_SCRATCH4, 31));
        if (publish_d0) emit(arm64::movz_w(kX87D0Valid, 1));
        const usize normal_done_branch = m_code_buf.size();
        emit(0);

        const usize result_zero_pos = m_code_buf.size();
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::str_x_uoff(31, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        if (publish_d0) emit(arm64::movz_w(kX87D0Valid, 1));
        const usize zero_done_branch = m_code_buf.size();
        emit(0);

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        const u32 packed = (kFpuKindMem << 28) |
            (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);
        emit_mov_imm32(2, packed);
        emit(arm64::mov_x(3, REG_CALL_TMP));  // full 64-bit EA
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
        emit_semantic_helper_blr();
        if (publish_d0) emit(arm64::movz_w(kX87D0Valid, 0));

        const usize done_pos = m_code_buf.size();
        const auto patch_cond = [&](usize branch, usize target, u8 cond) {
            m_code_buf[branch] = arm64::b_cond(
                cond, static_cast<i32>(target - branch) * 4);
        };
        const auto patch_b = [&](usize branch, usize target) {
            m_code_buf[branch] = arm64::b_imm(
                static_cast<i32>(target - branch) * 4);
        };
        if (consume_d0) {
            m_code_buf[forwarded_branch] = arm64::cbnz_w(
                kX87D0Valid,
                static_cast<i32>(forwarded_ready_pos - forwarded_branch) * 4);
        }
        m_code_buf[st_zero_branch] = arm64::cbz_w(
            REG_SCRATCH2,
            static_cast<i32>(st_zero_pos - st_zero_branch) * 4);
        patch_cond(st_below_branch, slow_pos, arm64::CC);
        patch_cond(st_above_branch, slow_pos, arm64::HI);
        patch_b(st_ready_branch, st_ready_pos);
        m_code_buf[result_nonzero_exp_branch] = arm64::cbnz_w(
            REG_SCRATCH2,
            static_cast<i32>(result_exp_check_pos -
                             result_nonzero_exp_branch) * 4);
        m_code_buf[result_zero_branch] = arm64::cbz_x(
            REG_SCRATCH4,
            static_cast<i32>(result_zero_pos - result_zero_branch) * 4);
        m_code_buf[result_subnormal_branch] = arm64::b_imm(
            static_cast<i32>(slow_pos - result_subnormal_branch) * 4);
        patch_cond(result_special_branch, slow_pos, arm64::EQ);
        patch_b(normal_done_branch, done_pos);
        patch_b(zero_done_branch, done_pos);

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    return std::nullopt;
}

} // namespace vitreo::cpu::jit
