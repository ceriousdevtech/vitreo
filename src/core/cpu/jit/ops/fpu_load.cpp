// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: memory loads that push. FLD m64real,
// FLD m32real, FILD m16int/m32int and FLD m80real (Intel SDM Vol. 1 8.3.3).
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

std::optional<bool> JitCompiler::compile_fpu_load(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const bool native_fpu_arith = native_arith_allowed(instr.opcode);

    const auto& op0 = instr.operands[0];

    // Intel SDM Vol. 2 FLD m64real: converting a finite normal binary64 to
    // double-extended is exact (no rounding). Keep Inf/NaN/subnormals on the
    // helper so Float80(double) remains the semantic reference. Default on:
    // this is a format conversion, not the opt-in binary64 arithmetic path.
    if (instr.opcode == Opcode::FLD_M64 && is_memory_operand(op0.type)) {
        // FLD m64 — Intel SDM Vol. 2: push m64real onto the x87 stack.
        //
        // Same shape as FLD m32: binary64 -> double-extended is exact for
        // every finite normal, so the fraction moves to significand bits
        // 62:11 under an explicit integer bit and the exponent is rebiased by
        // 16383 - 1023. Zero inline, everything else to the helper.
        //
        // Intel SDM Vol. 2 FLD: EFLAGS unchanged. The old lowering gated on
        // SUBS and never spilled, so it silently destroyed the flags of a
        // preceding compare; every gate here is CBZ/CBNZ.
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        constexpr u16 double_exp_bias_delta = 16383u - 1023u;
        const bool next_consumes_d0 = m_next_opcode == Opcode::FADD_M64 ||
                                      m_next_opcode == Opcode::FMUL_M64;
        const bool publish_d0 = !kDisableX87D0Forward && next_consumes_d0 &&
                                native_arith_allowed(m_next_opcode);
        m_x87_d0_forward_out = publish_d0;

        const bool hold = should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        // emit_guest_load64 may BLR a helper and clobber caller-saved X17,
        // so the slow path's EA lives in the callee-saved X21.
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        if (publish_d0) {
            // Write-through only: ST(0) is still committed below. D0 mirrors
            // the already-authorized m64 load for one adjacent FADD/FMUL.
            emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
            emit(arm64::movz_w(kX87D0Valid, 1));
        }

        emit(arm64::ubfx_x(REG_SCRATCH2, REG_SCRATCH0, 52, 11));
        const usize zero_branch = m_code_buf.size();
        emit(0);  // CBZ -> zero / subnormal
        emit_mov_imm32(REG_SCRATCH4, 0x7FF);
        emit(arm64::eor_w(REG_SCRATCH4, REG_SCRATCH2, REG_SCRATCH4));
        const usize special_branch = m_code_buf.size();
        emit(0);  // CBZ -> Inf/NaN slow path

        emit(arm64::ubfx_x(REG_SCRATCH1, REG_SCRATCH0, 0, 52));
        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 11));
        emit(arm64::movz_x(REG_SCRATCH4, 0x8000, 48));
        emit(arm64::orr_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, double_exp_bias_delta));
        emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH0, 63));
        emit(arm64::bfi_w(REG_SCRATCH2, REG_SCRATCH4, 15, 1));
        emit(arm64::movz_w(REG_SCRATCH3, static_cast<u16>(FpuTag::Valid)));
        const usize value_ready_branch = m_code_buf.size();
        emit(0);  // B -> commit

        const usize zero_pos = m_code_buf.size();
        emit(arm64::ubfx_x(REG_SCRATCH4, REG_SCRATCH0, 0, 52));
        const usize subnormal_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (binary64 subnormal normalises in x87)
        emit(arm64::movz_x(REG_SCRATCH1, 0, 0));
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH2, 15));
        emit(arm64::movz_w(REG_SCRATCH3, static_cast<u16>(FpuTag::Zero)));

        // Commit: X1 = significand, W2 = exponent/sign, W3 = tag, X21 = EA.
        const usize commit_pos = m_code_buf.size();

        // Intel SDM Vol. 1 8.5.1.1: pushing onto a register the tag word does
        // not mark Empty is a stack OVERFLOW (#IS); FpuState::push raises it
        // and substitutes the indefinite QNaN. Check before committing TOP.
        static_assert(sizeof(Float80) == 10, "st[] stride");
        static_assert(offsetof(Float80, exponent_sign) == 8, "st[] layout");
        emit(arm64::add_x_imm(17, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_uoff(REG_SCRATCH4, 17, 0));
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH4, 11, 3));
        emit(arm64::sub_w_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH4, 0, 3));  // new TOP
        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_SCRATCH4, 1));    // tag shift
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_uoff(17, REG_SCRATCH0, 0));
        emit(arm64::lsrv_w(17, 17, REG_EA_TMP));
        emit(arm64::mvn_w(17, 17));
        emit(arm64::ubfx_w(17, 17, 0, 2));
        const usize overflow_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (destination is not Empty)

        emit(arm64::add_x_imm(17, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_uoff(REG_SCRATCH0, 17, 0));
        emit(arm64::bfi_w(REG_SCRATCH0, REG_SCRATCH4, 11, 3));
        emit(arm64::strh_uoff(REG_SCRATCH0, 17, 0));

        emit(arm64::add_x_lsl(17, REG_SCRATCH4, REG_SCRATCH4, 2));
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH0, REG_SCRATCH0, 17, 1));
        emit(arm64::str_x_uoff(REG_SCRATCH1, REG_SCRATCH0, 0));
        emit(arm64::strh_uoff(REG_SCRATCH2, REG_SCRATCH0, 4));

        emit(arm64::add_x_imm(REG_SCRATCH0, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_uoff(17, REG_SCRATCH0, 0));
        emit(arm64::movz_w(REG_SCRATCH1, 3));
        emit(arm64::lslv_w(REG_SCRATCH1, REG_SCRATCH1, REG_EA_TMP));
        emit(arm64::bic_w(17, 17, REG_SCRATCH1));
        emit(arm64::lslv_w(REG_SCRATCH3, REG_SCRATCH3, REG_EA_TMP));
        emit(arm64::orr_w(17, 17, REG_SCRATCH3));
        emit(arm64::strh_uoff(17, REG_SCRATCH0, 0));
        const usize native_done_branch = m_code_buf.size();
        emit(0);

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        const u32 packed = (kFpuKindMem << 28) |
            (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);
        emit_mov_imm32(2, packed);
        emit(arm64::mov_x(3, REG_CALL_TMP));  // full 64-bit EA
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
        emit_semantic_helper_blr(); // blr x16
        if (publish_d0) emit(arm64::movz_w(kX87D0Valid, 0));

        const usize done_pos = m_code_buf.size();
        if (hold) emit_nzcv_fill();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(zero_pos - zero_branch) * 4);
        m_code_buf[special_branch] = arm64::cbz_w(
            REG_SCRATCH4, static_cast<i32>(slow_pos - special_branch) * 4);
        m_code_buf[value_ready_branch] = arm64::b_imm(
            static_cast<i32>(commit_pos - value_ready_branch) * 4);
        m_code_buf[subnormal_branch] = arm64::cbnz_x(
            REG_SCRATCH4, static_cast<i32>(slow_pos - subnormal_branch) * 4);
        m_code_buf[overflow_branch] = arm64::cbnz_w(
            17, static_cast<i32>(slow_pos - overflow_branch) * 4);
        m_code_buf[native_done_branch] = arm64::b_imm(
            static_cast<i32>(done_pos - native_done_branch) * 4);

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (native_fpu_arith &&
        instr.opcode == Opcode::FLD_M32 && is_memory_operand(op0.type)) {
        // FLD m32 — Intel SDM Vol. 2: push m32real onto the x87 stack.
        //
        // binary32 -> double-extended is EXACT for every finite normal, so
        // this is pure bit reassembly: the 23-bit fraction moves to
        // significand bits 62:40 under an explicit integer bit, and the 8-bit
        // exponent field is rebiased by 16383 - 127. The previous lowering
        // widened through the FP unit (FMOV S, FCVT D, FMOV X) and then took
        // the binary64 form apart again -- three crossings of the
        // integer/FP register boundary, each of them pure latency, on a path
        // that never needs an FP operation at all.
        //
        // Zero is handled inline. Subnormals, infinities and NaNs go to the
        // interpreter-backed helper: rare, and its result is the reference.
        //
        // Every gate here is CBZ/CBNZ/TBZ, never SUBS. Intel SDM Vol. 2 FLD:
        // EFLAGS unchanged, and the JIT keeps pending lazy flags live in
        // NZCV, so `CMP; FLD; FSTP; JNE` must still answer the CMP. The
        // guest load's bounds check is the only flag writer left, which is
        // what `hold` covers.
        const bool hold = should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        // binary32 bias 127 -> x87 bias 16383.
        constexpr u16 single_exp_bias_delta = 16383u - 127u;

        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_load32(REG_EA_TMP, REG_SCRATCH2);
        // emit_guest_load32 may BLR a helper, which clobbers caller-saved
        // X17; take the copy the slow path needs only after it returns.
        emit(arm64::mov_w(17, REG_EA_TMP));

        emit(arm64::ubfx_w(REG_SCRATCH0, REG_EA_TMP, 23, 8));
        const usize zero_branch = m_code_buf.size();
        emit(0);  // CBZ -> zero / subnormal
        emit(arm64::movz_w(REG_SCRATCH4, 0xFF));
        emit(arm64::eor_w(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH4));
        const usize special_branch = m_code_buf.size();
        emit(0);  // CBZ -> Inf/NaN slow path

        // Finite normal: significand = 1.fraction, exponent rebiased.
        emit(arm64::ubfx_x(REG_SCRATCH1, REG_EA_TMP, 0, 23));
        emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 40));
        emit(arm64::movz_x(REG_SCRATCH4, 0x8000, 48));
        emit(arm64::orr_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, single_exp_bias_delta));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::lsr_w_imm(REG_SCRATCH4, REG_EA_TMP, 31));
        emit(arm64::bfi_w(REG_SCRATCH0, REG_SCRATCH4, 15, 1));
        emit(arm64::movz_w(REG_SCRATCH3, static_cast<u16>(FpuTag::Valid)));
        const usize value_ready_branch = m_code_buf.size();
        emit(0);  // B -> commit

        const usize zero_pos = m_code_buf.size();
        // Exponent field 0: +-0 when the fraction is clear, otherwise a
        // binary32 subnormal, which normalises into the 80-bit format and is
        // left to the helper.
        emit(arm64::ubfx_x(REG_SCRATCH4, REG_EA_TMP, 0, 23));
        const usize subnormal_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow
        emit(arm64::movz_x(REG_SCRATCH1, 0, 0));
        emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_EA_TMP, 31));
        emit(arm64::lsl_w_imm(REG_SCRATCH0, REG_SCRATCH0, 15));
        emit(arm64::movz_w(REG_SCRATCH3, static_cast<u16>(FpuTag::Zero)));

        // Commit: X1 = significand, W0 = exponent/sign, W3 = tag.
        const usize commit_pos = m_code_buf.size();

        // Intel SDM Vol. 1 8.5.1.1: a push onto a register the tag word does
        // not mark Empty is a stack OVERFLOW (#IS) -- IE | SF | C1, and with
        // #I masked the destination takes the indefinite QNaN, not the loaded
        // value. FpuState::push implements that; this lowering used to move
        // TOP and store unconditionally, so an overflowing FLD wrote the
        // wrong value and set no status bits. Test the destination tag BEFORE
        // anything is committed and hand the whole instruction to the helper.
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_uoff(REG_SCRATCH4, REG_CALL_TMP, 0));
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH4, 11, 3));
        emit(arm64::sub_w_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH4, 0, 3));  // new TOP
        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_SCRATCH4, 1));    // tag shift
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_uoff(REG_SCRATCH2, REG_CALL_TMP, 0));  // tag word
        emit(arm64::lsrv_w(REG_CALL_TMP, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::mvn_w(REG_CALL_TMP, REG_CALL_TMP));
        emit(arm64::ubfx_w(REG_CALL_TMP, REG_CALL_TMP, 0, 2));
        const usize overflow_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (destination is not Empty)

        // X17's copy of the loaded bits is dead from here: every branch that
        // needs it has been emitted.
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_uoff(17, REG_CALL_TMP, 0));
        emit(arm64::bfi_w(17, REG_SCRATCH4, 11, 3));
        emit(arm64::strh_uoff(17, REG_CALL_TMP, 0));

        // &st[TOP]: Float80 is 10 bytes, so the index scales by 5 then 2.
        static_assert(sizeof(Float80) == 10, "st[] stride");
        static_assert(offsetof(Float80, exponent_sign) == 8, "st[] layout");
        emit(arm64::add_x_lsl(17, REG_SCRATCH4, REG_SCRATCH4, 2));
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_CALL_TMP, REG_CALL_TMP, 17, 1));
        emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CALL_TMP, 0));
        emit(arm64::strh_uoff(REG_SCRATCH0, REG_CALL_TMP, 4));

        // Tag word: W2 still holds it, W5 the shift, W3 the new tag.
        emit(arm64::movz_w(17, 3));
        emit(arm64::lslv_w(17, 17, REG_EA_TMP));
        emit(arm64::bic_w(REG_SCRATCH2, REG_SCRATCH2, 17));
        emit(arm64::lslv_w(REG_SCRATCH3, REG_SCRATCH3, REG_EA_TMP));
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CPU_STATE, tag_offset));
        emit(arm64::strh_uoff(REG_SCRATCH2, REG_CALL_TMP, 0));
        const usize native_done_branch = m_code_buf.size();
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
        if (hold) emit_nzcv_fill();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH0, static_cast<i32>(zero_pos - zero_branch) * 4);
        m_code_buf[special_branch] = arm64::cbz_w(
            REG_SCRATCH4, static_cast<i32>(slow_pos - special_branch) * 4);
        m_code_buf[value_ready_branch] = arm64::b_imm(
            static_cast<i32>(commit_pos - value_ready_branch) * 4);
        m_code_buf[subnormal_branch] = arm64::cbnz_x(
            REG_SCRATCH4, static_cast<i32>(slow_pos - subnormal_branch) * 4);
        m_code_buf[overflow_branch] = arm64::cbnz_w(
            REG_CALL_TMP, static_cast<i32>(slow_pos - overflow_branch) * 4);
        m_code_buf[native_done_branch] = arm64::b_imm(
            static_cast<i32>(done_pos - native_done_branch) * 4);

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // FILD m64 must stay exact for |n| > 2^53 (SDM 80-bit integer load).
    // scvtf_d loses those bits; the helper uses Float80::from_i64.
    if ((instr.opcode == Opcode::FILD_M16 ||
         instr.opcode == Opcode::FILD_M32) &&
        is_memory_operand(op0.type)) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        constexpr u32 double_exp_bias_delta = 16383u - 1023u;

        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));
        if (instr.opcode == Opcode::FILD_M16) {
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::sxth_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::scvtf_d_from_w(0, REG_SCRATCH0));
        } else if (instr.opcode == Opcode::FILD_M32) {
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::scvtf_d_from_w(0, REG_SCRATCH0));
        } else {
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::scvtf_d_from_x(0, REG_SCRATCH0));
        }
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 11, 3));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::mov_w(REG_EA_TMP, REG_SCRATCH2));
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_TOP_MASK);
        emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));

        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 0));

        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0, 52));
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        const usize zero_branch = m_code_buf.size();
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
        emit(arm64::movz_w(REG_SCRATCH0, static_cast<u16>(FpuTag::Valid)));
        const usize value_ready = m_code_buf.size();
        emit(0);
        const usize zero_pos = m_code_buf.size();
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::str_x_uoff(31, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        emit(arm64::movz_w(REG_SCRATCH0, static_cast<u16>(FpuTag::Zero)));
        const usize tag_pos = m_code_buf.size();
        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
        emit(arm64::movz_w(REG_SCRATCH2, 3));
        emit(arm64::lslv_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::mvn_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH0, REG_EA_TMP));
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH2,
            static_cast<i32>(zero_pos - zero_branch) * 4);
        m_code_buf[value_ready] = arm64::b_imm(
            static_cast<i32>(tag_pos - value_ready) * 4);

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FLD_M80 && is_memory_operand(op0.type)) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        // Both halves are read before any FPU state is touched, so a fault on
        // either leaves the x87 stack untouched.
        //
        // The mantissa CANNOT stay in X0 across the 16-bit read: the narrow
        // read has its own region-cache slot (JIT_CTX_OFF_X64_READ16_REGION*)
        // separate from the hashed slot table the 64-bit read probes, so it
        // misses independently, and its slow path returns the helper's result
        // in X0 (emit_guest_load16_64, jit_compiler.cpp:2136; the 32-bit
        // non-linear path does the same). Park it in REG_CALL_TMP, the only
        // callee-saved temp, and re-compute the address instead -- a load
        // cannot change the guest GPRs emit_compute_ea reads, and a stack
        // spill is not usable because emit_exit_if_pending_raise() can leave
        // the block from inside the helper tail.
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
        emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
        emit(arm64::mov_x(REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_EA_TMP, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_EA_TMP, 11, 3));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_TOP_MASK);
        emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::and_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH4, REG_SCRATCH2, 11));
        emit(arm64::orr_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::strh_reg(REG_EA_TMP, REG_SCRATCH3, 31));
        emit(arm64::mov_w(REG_EA_TMP, REG_SCRATCH2));
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 0));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FFF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH1, static_cast<u16>(FpuTag::Valid)));
        const usize mant_nz = m_code_buf.size();
        emit(0);
        const usize exp_nz = m_code_buf.size();
        emit(0);
        emit(arm64::movz_w(REG_SCRATCH1, static_cast<u16>(FpuTag::Zero)));
        const usize tag_ready = m_code_buf.size();
        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
        emit(arm64::movz_w(REG_SCRATCH2, 3));
        emit(arm64::lslv_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::mvn_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
        emit(arm64::lslv_w(REG_SCRATCH1, REG_SCRATCH1, REG_EA_TMP));
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        m_code_buf[mant_nz] = arm64::cbnz_x(
            REG_SCRATCH0, static_cast<i32>(tag_ready - mant_nz) * 4);
        m_code_buf[exp_nz] = arm64::cbnz_w(
            REG_SCRATCH2, static_cast<i32>(tag_ready - exp_nz) * 4);
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
