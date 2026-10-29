// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — CF lowering shared by the 32-bit and 64-bit paths:
//              emit_lazy_cf_to_call_tmp (leave x86 CF in REG_CALL_TMP)
//              and emit_carry_flag_control (CLC/STC/CMC).
//
// Split out of ops/arithmetic.cpp (ADR-0006 step 3) as pure code motion:
// both definitions are verbatim and in their original order. Both were
// already out-of-line members called from other translation units.

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

namespace vitreo::cpu::jit {

// Leave x86 CF in REG_CALL_TMP.
//
// Shared by the 32-bit path (ops/arithmetic.cpp) and the 64-bit path
// (ops/ops64.cpp). It used to be a lambda local to compile_arithmetic, so
// only 32-bit blocks got its fast paths; the 64-bit SBB case called the
// CAPTURE_CF helper unconditionally and paid ~136M helper calls on an
// ADC/SBB carry chain. Keep this a single definition -- the two paths
// already drifted once.
//
// Order matters: the CLC/STC/CMC overlay is honoured before any NZCV or
// FlagOp fast path, because those instructions force CF without retiring a
// pending FlagOp (ADD;CLC;ADC must not recompute ADD's carry).
void JitCompiler::emit_lazy_cf_to_call_tmp() {
    // Nothing known at compile time about where CF lives: one call to the
    // shared stub (materialize_capture_cf_stub), which asks the same questions
    // in the same order as the walk below, instead of ~90 words inline.
    // VITREO_JIT_INLINE_CF_DISPATCH=1 restores the inline walk.
    static const bool inline_dispatch =
        std::getenv("VITREO_JIT_INLINE_CF_DISPATCH") != nullptr;
    if (!inline_dispatch &&
        m_flags.incoming_nzcv != NzcvState::Sub &&
        m_flags.incoming_nzcv != NzcvState::Sub8 &&
        m_flags.incoming_nzcv != NzcvState::Add &&
        m_flags.incoming_nzcv != NzcvState::And &&
        m_flags.incoming_carry_source == CarrySource::Unknown &&
        materialize_capture_cf_stub() != nullptr) {
        diagnostics::perf_note_cf_emit_dispatch();
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                               JIT_CTX_OFF_CAPTURE_CF_STUB >> 3));
        emit(0xD63F0200u);  // BLR X16
        return;
    }
    // CLC/STC/CMC force CF without retiring a pending FlagOp. Honor that
    // overlay before NZCV or the FlagOp fast paths — otherwise ADD;CLC;ADC
    // would recompute ADD's carry.
    emit(arm64::ldrb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                          cf_overlay_offset()));
    const usize ov_cbz = m_code_buf.size();
    emit(0u);
    emit(arm64::movz_w(REG_CALL_TMP, 1));
    emit(arm64::sub_w(REG_CALL_TMP, REG_SCRATCH3, REG_CALL_TMP));
    const usize ov_done = m_code_buf.size();
    emit(0u);
    const usize ov_rest = m_code_buf.size();
    m_code_buf[ov_cbz] = arm64::cbz_w(
        REG_SCRATCH3, static_cast<i32>(ov_rest - ov_cbz) * 4);
    auto finish_overlay = [&] {
        const usize end = m_code_buf.size();
        m_code_buf[ov_done] = arm64::b_imm(
            static_cast<i32>(end - ov_done) * 4);
    };

    // A directly preceding register CMP/SUB leaves ARM NZCV live until
    // this instruction. x86 borrow is the inverse of ARM's subtraction
    // carry, so capture it without crossing the JIT/helper boundary.
    if (m_flags.incoming_nzcv == NzcvState::Sub ||
        m_flags.incoming_nzcv == NzcvState::Sub8) {
        emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
        diagnostics::perf_note_cf_emit_nzcv();
        finish_overlay();
        return;
    }
    // A directly preceding register ADD leaves ARM NZCV live. For addition
    // ARM64's carry flag equals x86 CF directly (no inversion), so the
    // carry-out is just CSET CS. This is the add/adc bignum fast path.
    if (m_flags.incoming_nzcv == NzcvState::Add) {
        emit(arm64::cset_w(REG_CALL_TMP, arm64::CS));
        diagnostics::perf_note_cf_emit_nzcv();
        finish_overlay();
        return;
    }
    // TEST/AND clears x86 CF. The fused ANDS state is likewise known for
    // the immediately following carry consumer.
    if (m_flags.incoming_nzcv == NzcvState::And) {
        emit(arm64::movz_w(REG_CALL_TMP, 0));
        diagnostics::perf_note_cf_emit_nzcv();
        finish_overlay();
        return;
    }

    auto emit_known_carry = [&](CarrySource source) {
        const u32 src_off = flag_src_offset();
        const u32 dst_off = flag_dst_offset();

        switch (source) {
        case CarrySource::Add32:
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH1, REG_CPU_STATE, src_off >> 2));
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH2, REG_CPU_STATE, dst_off >> 2));
            emit(arm64::sub_w(
                REG_SCRATCH3, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::subs_w(
                31, REG_SCRATCH2, REG_SCRATCH3));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
            break;
        case CarrySource::Adc32:
            emit(arm64::ldr_x_uoff(
                REG_SCRATCH1, REG_CPU_STATE, src_off >> 3));
            emit(arm64::lsr_x_imm(
                REG_SCRATCH2, REG_SCRATCH1, 32));
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH1));
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH3, REG_CPU_STATE, dst_off >> 2));
            emit(arm64::sub_w(
                REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH1));
            emit(arm64::sub_w(
                REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));
            emit(arm64::subs_w(
                31, REG_SCRATCH3, REG_SCRATCH4));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
            emit(arm64::cset_w(REG_SCRATCH0, arm64::EQ));
            emit(arm64::and_w(
                REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
            emit(arm64::orr_w(
                REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH0));
            break;
        case CarrySource::Sub32:
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH1, REG_CPU_STATE, src_off >> 2));
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH2, REG_CPU_STATE, dst_off >> 2));
            emit(arm64::add_w(
                REG_SCRATCH3, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::subs_w(
                31, REG_SCRATCH3, REG_SCRATCH1));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
            break;
        case CarrySource::Sbb32:
            emit(arm64::ldr_x_uoff(
                REG_SCRATCH1, REG_CPU_STATE, src_off >> 3));
            emit(arm64::lsr_x_imm(
                REG_SCRATCH2, REG_SCRATCH1, 32));
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH1));
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH3, REG_CPU_STATE, dst_off >> 2));
            emit(arm64::add_w(
                REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH1));
            emit(arm64::add_w(
                REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));
            emit(arm64::subs_w(
                31, REG_SCRATCH3, REG_SCRATCH4));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::HI));
            emit(arm64::cset_w(REG_SCRATCH0, arm64::EQ));
            emit(arm64::and_w(
                REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
            emit(arm64::orr_w(
                REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH0));
            break;
        case CarrySource::Mul32:
            emit(arm64::ldr_w_uoff(
                REG_SCRATCH1, REG_CPU_STATE, dst_off >> 2));
            emit(arm64::cmp_w_imm(REG_SCRATCH1, 0));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::NE));
            break;
        case CarrySource::Logic32:
            emit(arm64::movz_w(REG_CALL_TMP, 0));
            break;

        // --- 64-bit blocks. Same records, X-width. ---
        //
        // Intel SDM Vol. 1 3.4.3.1 (CF): unsigned overflow. For ADD the
        // record is flag_src = b, flag_dst = r = a + b, and unsigned
        // overflow is exactly r < b, so the whole formula is CMP + CSET —
        // the 32-bit arm reconstructs `a` first only because it predates
        // this identity being written down.
        case CarrySource::Add64:
            emit(arm64::ldr_x_uoff(
                REG_SCRATCH1, REG_CPU_STATE, src_off >> 3));
            emit(arm64::ldr_x_uoff(
                REG_SCRATCH2, REG_CPU_STATE, dst_off >> 3));
            emit(arm64::subs_x(31, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
            break;
        // SUB records flag_src = b, flag_dst = r = a - b. The borrow is
        // a < b, and a is recovered as r + b. CC (unsigned lower) is the
        // borrow; ARM's C is its inverse, which is why this is CSET CC and
        // not a bare read of the flag.
        case CarrySource::Sub64:
            emit(arm64::ldr_x_uoff(
                REG_SCRATCH1, REG_CPU_STATE, src_off >> 3));
            emit(arm64::ldr_x_uoff(
                REG_SCRATCH2, REG_CPU_STATE, dst_off >> 3));
            emit(arm64::add_x(
                REG_SCRATCH3, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::subs_x(31, REG_SCRATCH3, REG_SCRATCH1));
            emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
            break;
        case CarrySource::Logic64:
            emit(arm64::movz_w(REG_CALL_TMP, 0));
            break;

        case CarrySource::Unknown:
            break;
        }
    };

    if (m_flags.incoming_carry_source != CarrySource::Unknown) {
        emit_known_carry(m_flags.incoming_carry_source);
        diagnostics::perf_note_cf_emit_known();
        finish_overlay();
        return;
    }

    diagnostics::perf_note_cf_emit_dispatch();

    // Carry-heavy bignum code commonly separates its producer and
    // consumer with register moves, so NZCV is no longer available even
    // though the pending lazy operation is one of a few simple 32-bit
    // cases. Evaluate those directly in generated code and retain the
    // helper for every uncommon flag state.
    const u32 op_off = flag_op_offset();
    emit(arm64::ldrb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));

    auto emit_case_branch = [&](CpuState::FlagOp op) {
        emit(arm64::cmp_w_imm(
            REG_SCRATCH3, static_cast<u32>(op)));
        const usize branch = m_code_buf.size();
        emit(0u);
        return branch;
    };

    const usize add_branch =
        emit_case_branch(CpuState::FlagOp::Add32);
    const usize adc_branch =
        emit_case_branch(CpuState::FlagOp::Adc32);
    const usize sub_branch =
        emit_case_branch(CpuState::FlagOp::Sub32);
    const usize sbb_branch =
        emit_case_branch(CpuState::FlagOp::Sbb32);
    const usize mul_branch =
        emit_case_branch(CpuState::FlagOp::Mul32);
    const usize and_branch =
        emit_case_branch(CpuState::FlagOp::And32);
    const usize or_branch =
        emit_case_branch(CpuState::FlagOp::Or32);
    const usize xor_branch =
        emit_case_branch(CpuState::FlagOp::Xor32);

    auto emit_done_branch = [&] {
        const usize branch = m_code_buf.size();
        emit(0u);
        return branch;
    };

    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(
        16, REG_JIT_CTX, JIT_CTX_OFF_CAPTURE_CF >> 3));
    emit_helper_blr();
    emit(arm64::mov_w(REG_CALL_TMP, 0));
    const usize fallback_done = emit_done_branch();

    const usize add_pos = m_code_buf.size();
    emit_known_carry(CarrySource::Add32);
    const usize add_done = emit_done_branch();

    const usize adc_pos = m_code_buf.size();
    emit_known_carry(CarrySource::Adc32);
    const usize adc_done = emit_done_branch();

    const usize sub_pos = m_code_buf.size();
    emit_known_carry(CarrySource::Sub32);
    const usize sub_done = emit_done_branch();

    const usize sbb_pos = m_code_buf.size();
    emit_known_carry(CarrySource::Sbb32);
    const usize sbb_done = emit_done_branch();

    const usize mul_pos = m_code_buf.size();
    emit_known_carry(CarrySource::Mul32);
    const usize mul_done = emit_done_branch();

    const usize logic_pos = m_code_buf.size();
    emit_known_carry(CarrySource::Logic32);

    const usize done_pos = m_code_buf.size();
    auto patch_cond = [&](usize branch, usize target) {
        m_code_buf[branch] = arm64::b_cond(
            arm64::EQ,
            static_cast<i32>(target - branch) * 4);
    };
    auto patch_done = [&](usize branch) {
        m_code_buf[branch] = arm64::b_imm(
            static_cast<i32>(done_pos - branch) * 4);
    };

    patch_cond(add_branch, add_pos);
    patch_cond(adc_branch, adc_pos);
    patch_cond(sub_branch, sub_pos);
    patch_cond(sbb_branch, sbb_pos);
    patch_cond(mul_branch, mul_pos);
    patch_cond(and_branch, logic_pos);
    patch_cond(or_branch, logic_pos);
    patch_cond(xor_branch, logic_pos);
    patch_done(fallback_done);
    patch_done(add_done);
    patch_done(adc_done);
    patch_done(sub_done);
    patch_done(sbb_done);
    patch_done(mul_done);
    finish_overlay();
}

// Intel SDM Vol. 1, 7.3.13.1: CLC/STC/CMC modify CF directly. Share
// the identical carry-overlay lowering across guest modes; preserve the
// remaining lazy status flags and the existing helper clobber contract.
void JitCompiler::emit_carry_flag_control(Opcode opcode) {
    u32 rf_off = cpu_rflags_offset();
    u32 ov_off = cf_overlay_offset();
    if (opcode == Opcode::CMC) {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(
            16, REG_JIT_CTX, JIT_CTX_OFF_CAPTURE_CF >> 3));
        emit_helper_blr();  // BLR X16 — W0 = current CF
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(arm64::eor_w(0, 0, REG_SCRATCH4));
        emit(arm64::add_w_imm(REG_SCRATCH3, 0, 1));
        emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, ov_off));
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, 0));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        return;
    }
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::movz_w(REG_SCRATCH4, 1));
    if (opcode == Opcode::CLC) {
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
    } else {
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 2));
    }
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::strb_uoff(REG_SCRATCH4, REG_CPU_STATE, ov_off));
}

} // namespace vitreo::cpu::jit
