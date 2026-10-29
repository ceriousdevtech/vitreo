// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: memory stores. FST/FSTP m64real and m32real,
// FSTP m80real and FIST/FISTP/FISTTP (Intel SDM Vol. 1 8.3.3).
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

std::optional<bool> JitCompiler::compile_fpu_store(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];

    // FST/FSTP m64 — Intel SDM Vol. 2: store ST(0) as m64real (FSTP pops).
    //
    // Same contract as FST/FSTP m32 (see that lowering): a store narrows,
    // narrowing obeys RC and raises PE, and FpuState::store_m64 through
    // jit_fpu_peek_m64 is the reference. The fast path here does no rounding
    // either -- it proves the value is already exactly a binary64 normal
    // (explicit integer bit set, low 11 significand bits clear, exponent in
    // the binary64 normal range) and then just re-encodes it.
    if ((instr.opcode == Opcode::FST_M64 ||
         instr.opcode == Opcode::FSTP_M64) &&
        is_memory_operand(op0.type)) {
        // Intel SDM Vol. 2 FST/FSTP: EFLAGS unchanged. The old gate used SUBS
        // with no spill at all, so it destroyed a pending compare's flags.
        const bool hold = should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        constexpr u16 double_exp_bias_delta = 16383u - 1023u;
        const bool consume_d0 = m_x87_d0_forward_in &&
                                instr.opcode == Opcode::FSTP_M64;

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_uoff(REG_EA_TMP, REG_SCRATCH3, 0));
        // C1 is the rounding-direction indicator and is ASSIGNED, not sticky
        // (Intel SDM Vol. 1 8.1.3.2): a store that rounds UP sets it and any
        // other store clears it, an EXACT store included. The fast path below
        // never rounds, so it would otherwise leave a previous operation's C1
        // standing. Clearing it here costs one BFI on the status word this
        // body already has in hand; the helper path re-assigns it through
        // FpuState::record_soft_status.
        emit(arm64::bfi_w(REG_EA_TMP, 31, 9, 1));
        emit(arm64::strh_uoff(REG_EA_TMP, REG_SCRATCH3, 0));
        emit(arm64::ubfx_w(REG_EA_TMP, REG_EA_TMP, 11, 3));  // TOP

        // Intel SDM Vol. 1 8.5.1.1 stack underflow (#IS): checked here rather
        // than in compile_fpu's shared guard, off the TOP this body loads
        // anyway.
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_uoff(REG_SCRATCH2, REG_SCRATCH3, 0));
        emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_EA_TMP, 1));
        emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
        emit(arm64::mvn_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 0, 2));
        const usize empty_branch = m_code_buf.size();
        emit(0);  // CBZ -> slow (tag is Empty)

        static_assert(sizeof(Float80) == 10, "st[] stride");
        static_assert(offsetof(Float80, exponent_sign) == 8, "st[] layout");
        emit(arm64::add_x_lsl(REG_SCRATCH2, REG_EA_TMP, REG_EA_TMP, 2));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 1));
        usize forwarded_branch = 0;
        if (consume_d0) {
            forwarded_branch = m_code_buf.size();
            emit(0);  // CBNZ W6 -> exact binary64 result already in D0
        }
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::ldrh_uoff(REG_SCRATCH1, REG_SCRATCH3, 4));

        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 0, 15));
        const usize zero_branch = m_code_buf.size();
        emit(0);  // CBZ -> zero / denormal
        emit(arm64::movz_w(REG_SCRATCH4, double_exp_bias_delta));
        emit(arm64::sub_w(REG_SCRATCH4, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4, 11));
        const usize range_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (outside the 11-bit exponent field)
        const usize low_branch = m_code_buf.size();
        emit(0);  // CBZ -> slow (field 0: binary64 subnormal or underflow)
        emit_mov_imm32(REG_SCRATCH2, 0x7FF);
        emit(arm64::eor_w(REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH2));
        const usize high_branch = m_code_buf.size();
        emit(0);  // CBZ -> slow (field 2047: Inf/NaN or overflow)
        emit(arm64::ubfx_x(REG_SCRATCH2, REG_SCRATCH0, 0, 11));
        const usize inexact_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (more than 53 significant bits)
        const usize unnormal_branch = m_code_buf.size();
        emit(0);  // TBZ bit 63 -> slow (unnormal: no explicit integer bit)

        emit(arm64::ubfx_x(REG_CALL_TMP, REG_SCRATCH0, 11, 52));
        emit(arm64::bfi_x(REG_CALL_TMP, REG_SCRATCH4, 52, 11));
        emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH1, 15));
        emit(arm64::bfi_x(REG_CALL_TMP, REG_SCRATCH2, 63, 1));
        const usize native_done_branch = m_code_buf.size();
        emit(0);

        const usize zero_pos = m_code_buf.size();
        const usize pseudo_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (exponent 0, significand nonzero: denormal)
        emit(arm64::lsr_w_imm(REG_CALL_TMP, REG_SCRATCH1, 15));
        emit(arm64::lsl_x_imm(REG_CALL_TMP, REG_CALL_TMP, 63));
        const usize zero_done_branch = m_code_buf.size();
        emit(0);

        usize forwarded_pos = 0;
        usize forwarded_done_branch = 0;
        if (consume_d0) {
            forwarded_pos = m_code_buf.size();
            emit(arm64::fmov_x_from_d(REG_CALL_TMP, 0));
            forwarded_done_branch = m_code_buf.size();
            emit(0);  // B -> authorized guest store
        }

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                              static_cast<u32>(offsetof(JitContext, fpu_peek_m64)) >> 3));
        emit_helper_blr();
        emit(arm64::mov_x(REG_CALL_TMP, 0));
        const usize suppress_store_branch = m_code_buf.size();
        emit(0);  // CBZ W1 -> no memory commit and no FSTP pop

        const usize store_pos = m_code_buf.size();
        m_code_buf[empty_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(slow_pos - empty_branch) * 4);
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(zero_pos - zero_branch) * 4);
        m_code_buf[range_branch] = arm64::cbnz_w(
            REG_SCRATCH2, static_cast<i32>(slow_pos - range_branch) * 4);
        m_code_buf[low_branch] = arm64::cbz_w(
            REG_SCRATCH4, static_cast<i32>(slow_pos - low_branch) * 4);
        m_code_buf[high_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(slow_pos - high_branch) * 4);
        m_code_buf[inexact_branch] = arm64::cbnz_x(
            REG_SCRATCH2, static_cast<i32>(slow_pos - inexact_branch) * 4);
        m_code_buf[unnormal_branch] = arm64::tbz(
            REG_SCRATCH0, 63, static_cast<i32>(slow_pos - unnormal_branch) * 4);
        m_code_buf[pseudo_branch] = arm64::cbnz_x(
            REG_SCRATCH0, static_cast<i32>(slow_pos - pseudo_branch) * 4);
        m_code_buf[native_done_branch] = arm64::b_imm(
            static_cast<i32>(store_pos - native_done_branch) * 4);
        m_code_buf[zero_done_branch] = arm64::b_imm(
            static_cast<i32>(store_pos - zero_done_branch) * 4);
        if (consume_d0) {
            m_code_buf[forwarded_branch] = arm64::cbnz_w(
                kX87D0Valid,
                static_cast<i32>(forwarded_pos - forwarded_branch) * 4);
            m_code_buf[forwarded_done_branch] = arm64::b_imm(
                static_cast<i32>(store_pos - forwarded_done_branch) * 4);
        }

        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_store64(REG_CALL_TMP, REG_SCRATCH2);

        if (instr.opcode == Opcode::FSTP_M64) {
            emit_fpu_pop();
        }
        const usize done_pos = m_code_buf.size();
        m_code_buf[suppress_store_branch] = arm64::cbz_w(
            1, static_cast<i32>(done_pos - suppress_store_branch) * 4);
        if (hold) emit_nzcv_fill();

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // FST/FSTP m32 — Intel SDM Vol. 2: store ST(0) as m32real (FSTP pops).
    //
    // A store NARROWS, and narrowing is a rounding step that obeys RC and
    // raises PE when it is inexact; FpuState::store_m32 (reached through
    // jit_fpu_peek_m32) is the reference. This lowering therefore does not
    // try to round at all. It proves the ST(0) value is ALREADY exactly a
    // binary32 normal -- explicit integer bit set, low 40 significand bits
    // clear, exponent inside the binary32 normal range -- and in that case
    // the narrowing is the identity: no rounding, no PE, and the answer does
    // not depend on RC or on precision control. Everything else is the
    // helper's.
    //
    // That value test replaces the old control-word guard in compile_fpu,
    // which asked the wrong question. `PC == 53` does not imply the register
    // holds at most 53 significant bits -- FLD m80 and FILD m64 both load 64
    // -- so the guard let full-precision values through to a lowering that
    // TRUNCATED them, and it ignored RC and PE entirely.
    //
    // Pop only after the store: a fault must not commit TOP.
    if ((instr.opcode == Opcode::FST_M32 ||
         instr.opcode == Opcode::FSTP_M32) &&
        is_memory_operand(op0.type)) {
        // Intel SDM Vol. 2 FST/FSTP: EFLAGS unchanged. Every gate below is
        // CBZ/CBNZ/TBZ so the fast path writes no flags at all; the spill is
        // for the guest store's bounds check and the cold helper call.
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

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_uoff(REG_EA_TMP, REG_SCRATCH3, 0));
        // C1 is the rounding-direction indicator and is ASSIGNED, not sticky
        // (Intel SDM Vol. 1 8.1.3.2): a store that rounds UP sets it and any
        // other store clears it, an EXACT store included. The fast path below
        // never rounds, so it would otherwise leave a previous operation's C1
        // standing. Clearing it here costs one BFI on the status word this
        // body already has in hand; the helper path re-assigns it through
        // FpuState::record_soft_status.
        emit(arm64::bfi_w(REG_EA_TMP, 31, 9, 1));
        emit(arm64::strh_uoff(REG_EA_TMP, REG_SCRATCH3, 0));
        emit(arm64::ubfx_w(REG_EA_TMP, REG_EA_TMP, 11, 3));  // TOP

        // Intel SDM Vol. 1 8.5.1.1: sourcing a register the tag word marks
        // Empty is a stack UNDERFLOW (#IS). compile_fpu's shared guard used
        // to do this in 16 instructions through the same TOP this body reads
        // anyway; doing it here costs seven and lets FST/FSTP skip the guard
        // block entirely.
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_uoff(REG_SCRATCH2, REG_SCRATCH3, 0));
        emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_EA_TMP, 1));
        emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
        emit(arm64::mvn_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 0, 2));
        const usize empty_branch = m_code_buf.size();
        emit(0);  // CBZ -> slow (tag is Empty)

        static_assert(sizeof(Float80) == 10, "st[] stride");
        static_assert(offsetof(Float80, exponent_sign) == 8, "st[] layout");
        emit(arm64::add_x_lsl(REG_SCRATCH2, REG_EA_TMP, REG_EA_TMP, 2));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 1));
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::ldrh_uoff(REG_SCRATCH1, REG_SCRATCH3, 4));

        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 0, 15));
        const usize zero_branch = m_code_buf.size();
        emit(0);  // CBZ -> zero / denormal
        emit(arm64::movz_w(REG_SCRATCH4, single_exp_bias_delta));
        emit(arm64::sub_w(REG_SCRATCH4, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH4, 8));
        const usize range_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (outside the 8-bit exponent field)
        const usize low_branch = m_code_buf.size();
        emit(0);  // CBZ -> slow (field 0: binary32 subnormal or underflow)
        emit(arm64::movz_w(REG_SCRATCH2, 0xFF));
        emit(arm64::eor_w(REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH2));
        const usize high_branch = m_code_buf.size();
        emit(0);  // CBZ -> slow (field 255: Inf/NaN or overflow)
        emit(arm64::ubfx_x(REG_SCRATCH2, REG_SCRATCH0, 0, 40));
        const usize inexact_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow (more than 24 significant bits)
        const usize unnormal_branch = m_code_buf.size();
        emit(0);  // TBZ bit 63 -> slow (unnormal: no explicit integer bit)

        // Exact: reassemble the binary32 encoding directly.
        emit(arm64::ubfx_x(REG_CALL_TMP, REG_SCRATCH0, 40, 23));
        emit(arm64::bfi_w(REG_CALL_TMP, REG_SCRATCH4, 23, 8));
        emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH1, 15));
        emit(arm64::bfi_w(REG_CALL_TMP, REG_SCRATCH2, 31, 1));
        const usize native_done_branch = m_code_buf.size();
        emit(0);

        const usize zero_pos = m_code_buf.size();
        // Exponent 0 with a clear significand is +-0, which stores exactly.
        // A nonzero significand there is an 80-bit denormal: helper.
        const usize pseudo_branch = m_code_buf.size();
        emit(0);  // CBNZ -> slow
        emit(arm64::lsr_w_imm(REG_CALL_TMP, REG_SCRATCH1, 15));
        emit(arm64::lsl_w_imm(REG_CALL_TMP, REG_CALL_TMP, 31));
        const usize zero_done_branch = m_code_buf.size();
        emit(0);

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                              JIT_CTX_OFF_FPU_PEEK_M32 >> 3));
        emit_helper_blr();
        emit(arm64::mov_w(REG_CALL_TMP, 0));
        const usize suppress_store_branch = m_code_buf.size();
        emit(0);  // CBZ W1 -> no memory commit and no FSTP pop

        const usize store_pos = m_code_buf.size();
        m_code_buf[empty_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(slow_pos - empty_branch) * 4);
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(zero_pos - zero_branch) * 4);
        m_code_buf[range_branch] = arm64::cbnz_w(
            REG_SCRATCH2, static_cast<i32>(slow_pos - range_branch) * 4);
        m_code_buf[low_branch] = arm64::cbz_w(
            REG_SCRATCH4, static_cast<i32>(slow_pos - low_branch) * 4);
        m_code_buf[high_branch] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(slow_pos - high_branch) * 4);
        m_code_buf[inexact_branch] = arm64::cbnz_x(
            REG_SCRATCH2, static_cast<i32>(slow_pos - inexact_branch) * 4);
        m_code_buf[unnormal_branch] = arm64::tbz(
            REG_SCRATCH0, 63, static_cast<i32>(slow_pos - unnormal_branch) * 4);
        m_code_buf[pseudo_branch] = arm64::cbnz_x(
            REG_SCRATCH0, static_cast<i32>(slow_pos - pseudo_branch) * 4);
        m_code_buf[native_done_branch] = arm64::b_imm(
            static_cast<i32>(store_pos - native_done_branch) * 4);
        m_code_buf[zero_done_branch] = arm64::b_imm(
            static_cast<i32>(store_pos - zero_done_branch) * 4);

        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_store32(REG_CALL_TMP, REG_SCRATCH2);

        if (instr.opcode == Opcode::FSTP_M32) {
            emit_fpu_pop();
        }
        const usize done_pos = m_code_buf.size();
        m_code_buf[suppress_store_branch] = arm64::cbz_w(
            1, static_cast<i32>(done_pos - suppress_store_branch) * 4);
        if (hold) emit_nzcv_fill();

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FSTP_M80 && is_memory_operand(op0.type)) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));

        // &fpu.st[TOP] -> X3. Re-emitted between the two guest stores rather
        // than kept in a register: X0-X5 and X17 are all caller-saved, and the
        // one callee-saved temp (X21) is holding the effective address.
        const auto emit_st_top_ptr_x3 = [&] {
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
            emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 11, 3));
            emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
            emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
            emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 0));
        };

        // The effective address must be computed BEFORE anything is captured
        // out of the x87 stack: emit_compute_ea / emit_compute_ea64 claim
        // REG_EA_TMP (X5) as their own scratch for any indexed operand, for a
        // displacement that does not fit an imm12, and for a segment base
        // (jit_compiler.cpp:2373, 64_bit_emit_primitives_used.cpp:246). The
        // old order captured exponent_sign into X5 first, so `fstp tbyte
        // [rax+rcx*8]` wrote the index register as the exponent, in both
        // bitnesses. X21 is callee-saved, so the address also survives the
        // helper BLR that either store takes on a cold region-cache slot.
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));

        emit_st_top_ptr_x3();
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::mov_x(17, REG_SCRATCH0));
        emit(arm64::mov_x(REG_SCRATCH2, REG_CALL_TMP));
        emit_guest_store64(17, REG_SCRATCH2);

        // Re-read exponent+sign only now. emit_guest_store64's slow path calls
        // a C helper (emit_helper_blr restores just the pins and the linear
        // base), so no caller-saved register carries a value across it. TOP has
        // not moved -- emit_fpu_pop() runs below -- so st[TOP] is still the
        // source register.
        emit_st_top_ptr_x3();
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, 8));
        emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        emit_fpu_pop();
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if ((instr.opcode == Opcode::FIST_M16 ||
         instr.opcode == Opcode::FIST_M32 ||
         instr.opcode == Opcode::FISTP_M16 ||
         instr.opcode == Opcode::FISTP_M32 ||
         instr.opcode == Opcode::FISTP_M64 ||
         instr.opcode == Opcode::FISTTP_M16 ||
         instr.opcode == Opcode::FISTTP_M32 ||
         instr.opcode == Opcode::FISTTP_M64) &&
        is_memory_operand(op0.type)) {
        const bool trunc =
            instr.opcode == Opcode::FISTTP_M16 ||
            instr.opcode == Opcode::FISTTP_M32 ||
            instr.opcode == Opcode::FISTTP_M64;
        const u32 width =
            (instr.opcode == Opcode::FIST_M16 ||
             instr.opcode == Opcode::FISTP_M16 ||
             instr.opcode == Opcode::FISTTP_M16) ? 2u
            : (instr.opcode == Opcode::FISTP_M64 ||
               instr.opcode == Opcode::FISTTP_M64) ? 8u : 4u;
        const bool do_pop =
            instr.opcode == Opcode::FISTP_M16 ||
            instr.opcode == Opcode::FISTP_M32 ||
            instr.opcode == Opcode::FISTP_M64 ||
            trunc;
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm32(1, width | (trunc ? 0x100u : 0u));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                              static_cast<u32>(offsetof(JitContext, fpu_peek_int)) >> 3));
        emit_helper_blr();
        emit(arm64::mov_x(REG_CALL_TMP, 0));
        const usize suppress_store_branch = m_code_buf.size();
        emit(0);  // CBZ W1 -> no guest write and no pop
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        if (width == 2) {
            emit_guest_store16(REG_CALL_TMP, REG_SCRATCH2);
        } else if (width == 4) {
            emit_guest_store32(REG_CALL_TMP, REG_SCRATCH2);
        } else {
            emit_guest_store64(REG_CALL_TMP, REG_SCRATCH2);
        }
        if (do_pop) {
            emit_fpu_pop();
        }
        const usize done_pos = m_code_buf.size();
        m_code_buf[suppress_store_branch] = arm64::cbz_w(
            1, static_cast<i32>(done_pos - suppress_store_branch) * 4);
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
