// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — packed XMM conversions: CVTPS2PD, CVTDQ2PS, CVTPS2DQ/
// CVTTPS2DQ, CVTDQ2PD, CVTPD2PS and CVTPD2DQ/CVTTPD2DQ.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_conv_packed(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- CVTPS2PD xmm, xmm/m64 ----
    case Opcode::CVTPS2PD_XMM_XMMM64: {
        u8 dst_xmm = op0.reg & 15;
        // Native: FCVTL Vd.2D, Vn.2S widens the low two singles to two
        // doubles, which is the whole instruction (Intel SDM Vol. 2
        // CVTPS2PD). Widening float to double is exact, so the only exception
        // either architecture raises is invalid on a signalling NaN, and both
        // do that identically. Previously a jit_sse_op call.
        //
        // VCVTPS2PD xmm1, xmm2/m64 zeroes DEST[MAXVL-1:128]; VCVTPS2PD ymm1,
        // xmm2/m128 widens all four singles (FCVTL2 for SRC[127:64]). Neither
        // VEX form has an alignment requirement.
        if (instr.prefixes.vex) {
            if (m_vec_lane_split) return false;
            const bool wide = instr.prefixes.vex_l;
            const u32 off_d = xmm_u64_off(dst_xmm, 0);
            const u32 off_dhi = ymm_high_u64_off(dst_xmm, 0);
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_dhi)) {
                return false;
            }
            if (is_register_operand(op1.type)) {
                const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
                if (!fits_ldr_q_offset(off_s)) return false;
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
            } else if (wide) {
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::fmov_d_from_x(1, REG_CALL_TMP));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
            } else {
                emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::fmov_d_from_x(1, REG_SCRATCH0));
            }
            emit(arm64::fcvtl_2d_2s(0, 1));
            if (wide) {
                emit(arm64::fcvtl2_2d_4s(2, 1));
            } else {
                emit(arm64::movi_v16b(2, 0));
            }
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_dhi >> 4));
            break;
        }
        {
            const u32 off_d = vec_off(dst_xmm);
            if (fits_ldr_q_offset(off_d)) {
                if (is_register_operand(op1.type)) {
                    const u32 off_s = vec_off(op1.reg & 15);
                    if (fits_ldr_q_offset(off_s) &&
                        xmm_cache_fits(dst_xmm, op1.reg & 15)) {
                        // XMM register cache: FCVTL reads its whole source
                        // before writing, so the destination may alias it.
                        const u8 vs = xmm_cache_source(op1.reg & 15);
                        const u8 vd = static_cast<u8>(kXmmCacheBase + dst_xmm);
                        emit(arm64::fcvtl_2d_2s(vd, vs));
                        emit(arm64::str_q_uoff(vd, REG_CPU_STATE, off_d >> 4));
                        xmm_cache_publish(dst_xmm);
                        m_xmm_cache_aware = true;
                        break;
                    }
                    if (fits_ldr_q_offset(off_s)) {
                        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
                        emit(arm64::fcvtl_2d_2s(0, 1));
                        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                        break;
                    }
                } else {
                    emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                    emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                    emit(arm64::ins_v_gpr(1, 0, REG_SCRATCH0, arm64::VS_D));
                    emit(arm64::fcvtl_2d_2s(0, 1));
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                    break;
                }
            }
        }
        if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 3));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, op1.reg & 15));
            EMIT_SSE_OP_CALL();
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 4));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // Intel SDM Vol. 2 CVTDQ2PS: 4×i32 → 4×f32 (SCVTF.4S).
    case Opcode::CVTDQ2PS_XMM_XMMM128: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_d = vec_off(dst_xmm);
        if (!fits_ldr_q_offset(off_d)) return false;
        if (is_register_operand(op1.type)) {
            const u32 off_s = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // EA parked in X21 before the first load: emit_guest_load64 / the
            // x64 region load clobbers X4 and may BLR, so the second qword's
            // address must not live there. Same shape as EMIT_SSE_NEON_RR's
            // memory arm. This arm used to `return false` in long mode, so
            // every `cvtdq2ps xmm, [mem]` in an x64 guest was interpreted:
            // 24,463,665 delegated instructions in a browser renderer.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            // CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ take an ALIGNED 16-byte
            // memory operand (Intel SDM Vol. 2, exception class 2 -- the same
            // class as the packed integer forms the probe measured faulting).
            // This arm computed the address and read it with no check, so a
            // misaligned operand silently succeeded here while the interpreter
            // raised. VEX forms are exempt: unaligned unless the opcode says
            // otherwise.
            if (!instr.prefixes.vex && !m_current_original_vex)
                emit_align_check(REG_CALL_TMP, false);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        // SCVTF Vd.4S, Vn.4S — ARM ARM C7.2.232 (Q=1, size=S).
        emit(0x4E21D800u | (1u << 5) | 0u);
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        for (u8 lane = 0; lane < 4; ++lane) {
            emit(arm64::umov_w_s(REG_SCRATCH0, 0, lane));
            emit(arm64::umov_w_s(REG_SCRATCH1, 1, lane));
            emit(arm64::fmov_s_from_w(2, REG_SCRATCH0));
            emit(0x1E380000u | (2u << 5) | static_cast<u32>(REG_SCRATCH2));
            emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH1));
            const u32 b_exact_dq2ps = static_cast<u32>(m_code_buf.size());
            emit(0);
            {
                const u32 mxcsr_off = static_cast<u32>(
                    offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
                emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
                emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
                emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
                emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
            }
            const u32 dq2ps_done = static_cast<u32>(m_code_buf.size());
            m_code_buf[b_exact_dq2ps] = arm64::b_cond(
                arm64::EQ, static_cast<i32>(dq2ps_done - b_exact_dq2ps) * 4);
        }
        break;
    }

    // Intel SDM Vol. 2 CVTPS2DQ / CVTTPS2DQ: 4×f32 → 4×i32 with indefinite
    // 0x80000000 on overflow/NaN (match interpreter; no #IA delivery).
    case Opcode::CVTPS2DQ_XMM_XMMM128:
    case Opcode::CVTTPS2DQ_XMM_XMMM128: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_d = vec_off(dst_xmm);
        if (!fits_ldr_q_offset(off_d)) return false;
        if (is_register_operand(op1.type)) {
            const u32 off_s = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(3, REG_CPU_STATE, off_s >> 4));
        } else {
            // See CVTDQ2PS above: EA parked in X21, and no longer declined in
            // long mode.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            // CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ take an ALIGNED 16-byte
            // memory operand (Intel SDM Vol. 2, exception class 2 -- the same
            // class as the packed integer forms the probe measured faulting).
            // This arm computed the address and read it with no check, so a
            // misaligned operand silently succeeded here while the interpreter
            // raised. VEX forms are exempt: unaligned unless the opcode says
            // otherwise.
            if (!instr.prefixes.vex && !m_current_original_vex)
                emit_align_check(REG_CALL_TMP, false);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(3, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(3, 1, REG_SCRATCH0, arm64::VS_D));
        }
        const bool truncate = (instr.opcode == Opcode::CVTTPS2DQ_XMM_XMMM128);
        auto emit_f32_to_i32 = [&](u8 dst_w, u8 src_w) {
            emit(arm64::fmov_s_from_w(0, src_w));
            emit(0x1E202000u | (0u << 16) | (0u << 5));
            const u32 b_nan = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm32(REG_SCRATCH2, 0x4F000000u);
            emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
            emit(0x1E202000u | (1u << 16) | (0u << 5));
            const u32 b_ge = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm32(REG_SCRATCH2, 0xCF000000u);
            emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
            emit(0x1E202000u | (1u << 16) | (0u << 5));
            const u32 b_lt = static_cast<u32>(m_code_buf.size());
            emit(0);
            if (truncate) {
                     emit(0x1E380000u | static_cast<u32>(dst_w));
                 } else {
                     // CVTSS2SI/CVTSD2SI round per MXCSR.RC; only the
                     // CVTT* forms truncate unconditionally.
                     emit_fcvt_scalar_by_mxcsr(0x1E200000u | static_cast<u32>(dst_w));
                 }
            emit(0x1E220000u | (static_cast<u32>(dst_w) << 5) | 1u);
            emit(0x1E202000u | (1u << 16) | (0u << 5));
            const u32 b_exact = static_cast<u32>(m_code_buf.size());
            emit(0);
            {
                const u32 mxcsr_off = static_cast<u32>(
                    offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
                emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
                emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
                emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
                emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
            }
            const u32 b_done = static_cast<u32>(m_code_buf.size());
            emit(0);
            const u32 indef = static_cast<u32>(m_code_buf.size());
            emit_mov_imm32(dst_w, 0x80000000u);
            const u32 done = static_cast<u32>(m_code_buf.size());
            m_code_buf[b_nan] = arm64::b_cond(
                arm64::VS, static_cast<i32>(indef - b_nan) * 4);
            m_code_buf[b_ge] = arm64::b_cond(
                arm64::GE, static_cast<i32>(indef - b_ge) * 4);
            m_code_buf[b_lt] = arm64::b_cond(
                arm64::LT, static_cast<i32>(indef - b_lt) * 4);
            m_code_buf[b_exact] = arm64::b_cond(
                arm64::EQ, static_cast<i32>(b_done - b_exact) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done - b_done) * 4);
        };
        for (u8 lane = 0; lane < 4; ++lane) {
            emit(arm64::umov_w_s(REG_SCRATCH0, 3, lane));
            emit_f32_to_i32(REG_SCRATCH0, REG_SCRATCH0);
            emit(arm64::ins_v_gpr(2, lane, REG_SCRATCH0, arm64::VS_S));
        }
        emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // Intel SDM Vol. 2 CVTDQ2PD: two packed i32 (low 64) → two f64.
    case Opcode::CVTDQ2PD_XMM_XMMM64: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_lo = xmm_u64_off(dst_xmm, 0);
        const u32 off_hi = xmm_u64_off(dst_xmm, 1);
        if (!fits_ldr_x_offset(off_lo) || !fits_ldr_x_offset(off_hi)) {
            return false;
        }
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) {
                return false;
            }
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 32));
        emit(arm64::scvtf_d_from_w(0, REG_SCRATCH0));
        emit(arm64::scvtf_d_from_w(1, REG_SCRATCH1));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::fmov_x_from_d(REG_SCRATCH1, 1));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_lo >> 3));
        emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_hi >> 3));
        break;
    }

    // Intel SDM Vol. 2 CVTPD2PS: two f64 → two f32 in low 64; high 64 zero.
    // VCVTPD2PS xmm1, ymm2/m256 (VEX.256) converts FOUR doubles into the
    // whole of xmm1 and zeroes YMM1[255:128]. It is a 256->128 narrowing, not
    // two independent 128-bit lanes, so it cannot use the lane split; the
    // VEX bridge hands it here directly with vex/vex_l still set.
    case Opcode::CVTPD2PS_XMM_XMMM128: {
        if (m_vec_lane_split) return false;
        const bool wide = instr.prefixes.vex && instr.prefixes.vex_l;
        if (instr.prefixes.vex && !wide) return false;  // VEX.128: bridge
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_lo = xmm_u64_off(dst_xmm, 0);
        const u32 dhi = ymm_high_u64_off(dst_xmm, 0);
        const u32 scr = jit_vec_scratch_off();
        const bool src_is_reg = is_register_operand(op1.type);
        // Where the source doubles are read from as Q registers: the
        // register's XMM and YMM-high slots, or (m256) the staging scratch.
        const u32 src_lo = src_is_reg ? xmm_u64_off(op1.reg & 15, 0) : scr;
        const u32 src_hi =
            src_is_reg ? ymm_high_u64_off(op1.reg & 15, 0) : scr + 16;
        if (!fits_ldr_q_offset(off_lo)) return false;
        if (wide && (!fits_ldr_x_offset(dhi) || !fits_ldr_x_offset(dhi + 8) ||
                     !fits_ldr_q_offset(src_lo) || !fits_ldr_q_offset(src_hi) ||
                     (!src_is_reg && !fits_ldr_x_offset(scr + 24)))) {
            return false;
        }
        if (src_is_reg && !fits_ldr_q_offset(src_lo)) return false;
        // Every lane at once (ARM ARM FCVTN/FCVTN2, FCVTL/FCVTL2): narrow
        // under FPCR, which carries MXCSR.RC/FTZ/DAZ, widen back and compare
        // with the source. PE is the interpreter's rule (sse_cvt_narrow_pe):
        // only a FINITE source that narrows inexactly sets it. An infinity
        // always narrows exactly, so "not NaN" (FCMEQ of the source with
        // itself) is the finite mask, and it raises Invalid only for an SNaN,
        // which FCVTN signals anyway; x - x would raise Invalid on +-Inf. IE,
        // OE and UE come from the host's sticky accumulator, as they do in the
        // interpreter. FCVTN zeroes V1[127:64], so one STR Q writes the whole
        // legacy destination. Only the memory loads write NZCV.
        const bool hold = !src_is_reg && should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        if (src_is_reg) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_lo >> 4));
            if (wide) emit(arm64::ldr_q_uoff(5, REG_CPU_STATE, src_hi >> 4));
        } else if (wide) {
            // m256, no alignment requirement (VEX). Stage all four qwords
            // memory-to-CpuState before any destination byte is written,
            // so a fault anywhere in the operand leaves DEST untouched, and
            // because a non-linear load's helper clobbers V0-V7. EA parked in
            // X21; each piece's address is an ADD X from it.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            for (u32 piece = 0; piece < 4; ++piece) {
                u8 addr = REG_CALL_TMP;
                if (piece != 0) {
                    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP,
                                          piece * 8));
                    addr = REG_SCRATCH4;
                }
                emit_guest_load64(REG_SCRATCH0, addr);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (scr + piece * 8) >> 3));
            }
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_lo >> 4));
            emit(arm64::ldr_q_uoff(5, REG_CPU_STATE, src_hi >> 4));
        } else {
            // Legacy m128 (16-byte aligned, checked): both halves land in
            // GPRs before V0 is built, for the same helper-clobber reason.
            // The EA is parked in X21 (callee-saved) and EA+8 is formed from
            // it after the first load: in long mode the region probe
            // clobbers X4.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH4);
            emit(arm64::fmov_d_from_x(0, REG_CALL_TMP));
            emit(arm64::ins_v_gpr(0, 1, REG_SCRATCH1, arm64::VS_D));
        }
        emit(arm64::fcvtn_2s_2d(1, 0));
        emit(arm64::fcvtl_2d_2s(2, 1));
        emit(arm64::fcmeq_v(3, 2, 0, arm64::FS_D));  // narrowed exactly
        emit(arm64::fcmeq_v(4, 0, 0, arm64::FS_D));  // not a NaN
        emit(arm64::bic_v16b(4, 4, 3));
        if (wide) {
            emit(arm64::fcvtn2_4s_2d(1, 5));
            emit(arm64::fcvtl2_2d_4s(6, 1));
            emit(arm64::fcmeq_v(7, 6, 5, arm64::FS_D));
            emit(arm64::fcmeq_v(6, 5, 5, arm64::FS_D));
            emit(arm64::bic_v16b(6, 6, 7));
            emit(arm64::orr_v16b(4, 4, 6));
        }
        // W3 is 0 or all ones: OR PE in without a branch. A real narrowing
        // is usually inexact, and a cold arm taken on it would cost a branch
        // out, an XMM-cache reload and a branch back, and mark the
        // instruction as a helper call for the bookkeeping after it.
        static_assert(MXCSR_PE == 1u << 5);
        const u32 mxcsr_off = static_cast<u32>(
            offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
        emit(arm64::umaxv_v(4, 4, arm64::VS_S));
        emit(arm64::fmov_w_from_s(REG_SCRATCH3, 4));
        emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 31));
        emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
        emit(arm64::orr_w_lsl(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3, 5));
        emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, off_lo >> 4));
        if (wide) {
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, dhi >> 3));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, (dhi + 8) >> 3));
        }
        if (hold) emit_nzcv_fill();
        break;
    }

    // Intel SDM Vol. 2 CVTPD2DQ / CVTTPD2DQ: two f64 → two i32 in DEST[63:0],
    // DEST[127:64] := 0; VCVT(T)PD2DQ xmm1, ymm2/m256 converts four doubles
    // into all of xmm1. Every VEX form zeroes DEST[MAXVL-1:128] and has no
    // alignment requirement; the legacy m128 must be 16-byte aligned.
    // Indefinite 0x80000000 on overflow/NaN (match interpreter).
    case Opcode::CVTPD2DQ_XMM_XMMM128:
    case Opcode::CVTTPD2DQ_XMM_XMMM128: {
        if (m_vec_lane_split) return false;
        const bool vex = instr.prefixes.vex;
        const u32 lanes = (vex && instr.prefixes.vex_l) ? 4u : 2u;
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_lo = xmm_u64_off(dst_xmm, 0);
        const u32 off_dhi = ymm_high_u64_off(dst_xmm, 0);
        const u32 scr = jit_vec_scratch_off();
        if (!fits_ldr_q_offset(off_lo) || (vex && !fits_ldr_q_offset(off_dhi))) {
            return false;
        }
        // Where each source double is read from: the register's XMM and
        // YMM-high qwords, or the staging scratch for a memory operand.
        u32 src_off[4] = {};
        const bool src_is_reg = is_register_operand(op1.type);
        for (u32 i = 0; i < lanes; ++i) {
            src_off[i] = !src_is_reg ? scr + i * 8u
                       : i < 2 ? xmm_u64_off(op1.reg & 15, static_cast<u8>(i))
                               : ymm_high_u64_off(op1.reg & 15,
                                                  static_cast<u8>(i - 2));
            if (!fits_ldr_x_offset(src_off[i])) return false;
        }
        if (!src_is_reg) {
            // Stage the whole operand before any destination byte is
            // written, so a fault anywhere in it leaves DEST untouched. EA
            // parked in X21: in long mode the region probe clobbers X4.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            for (u32 piece = 0; piece < lanes; ++piece) {
                u8 addr = REG_CALL_TMP;
                if (piece != 0) {
                    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, piece * 8));
                    addr = REG_SCRATCH4;
                }
                emit_guest_load64(REG_SCRATCH0, addr);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (scr + piece * 8) >> 3));
            }
        }
        const bool truncate = (instr.opcode == Opcode::CVTTPD2DQ_XMM_XMMM128);
        auto emit_f64_to_i32 = [&](u8 dst_w, u8 src_x) {
            emit(arm64::fmov_d_from_x(0, src_x));
            emit(arm64::fcmp_d(0, 0));
            const u32 b_nan = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm64(REG_SCRATCH2, 0x41E0000000000000LL);
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
            emit(arm64::fcmp_d(0, 1));
            const u32 b_ge = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0xC1E0000000000000ULL));
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
            emit(arm64::fcmp_d(0, 1));
            const u32 b_lt = static_cast<u32>(m_code_buf.size());
            emit(0);
            // ARM ARM: FCVTZS Wd, Dn (truncate) / FCVTNS Wd, Dn (nearest-even).
            // FCVTNS Wd,Dn = 0x1E600000; FCVTZS Wd,Dn = 0x1E780000 (Rn=0).
            if (truncate) {
                     emit(0x1E780000u | static_cast<u32>(dst_w));
                 } else {
                     // CVTSS2SI/CVTSD2SI round per MXCSR.RC; only the
                     // CVTT* forms truncate unconditionally.
                     emit_fcvt_scalar_by_mxcsr(0x1E600000u | static_cast<u32>(dst_w));
                 }
            emit(0x1E620000u | (static_cast<u32>(dst_w) << 5) | 1u);
            emit(arm64::fcmp_d(0, 1));
            const u32 b_exact = static_cast<u32>(m_code_buf.size());
            emit(0);
            {
                const u32 mxcsr_off = static_cast<u32>(
                    offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
                emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
                emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
                emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
                emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
            }
            const u32 b_done = static_cast<u32>(m_code_buf.size());
            emit(0);
            const u32 indef = static_cast<u32>(m_code_buf.size());
            emit_mov_imm32(dst_w, 0x80000000u);
            const u32 done = static_cast<u32>(m_code_buf.size());
            m_code_buf[b_nan] = arm64::b_cond(
                arm64::VS, static_cast<i32>(indef - b_nan) * 4);
            m_code_buf[b_ge] = arm64::b_cond(
                arm64::GE, static_cast<i32>(indef - b_ge) * 4);
            m_code_buf[b_lt] = arm64::b_cond(
                arm64::LT, static_cast<i32>(indef - b_lt) * 4);
            m_code_buf[b_exact] = arm64::b_cond(
                arm64::EQ, static_cast<i32>(b_done - b_exact) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done - b_done) * 4);
        };
        // Each result is inserted into V2 lane by lane; the lambda only
        // uses V0/V1 and X0-X3, and no helper call is made from here on.
        emit(arm64::movi_v16b(2, 0));
        for (u32 i = 0; i < lanes; ++i) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, src_off[i] >> 3));
            emit_f64_to_i32(REG_SCRATCH0, REG_SCRATCH1);
            emit(arm64::ins_v_gpr(2, i, REG_SCRATCH0, arm64::VS_S));
        }
        emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_lo >> 4));
        if (vex) {
            emit(arm64::movi_v16b(3, 0));
            emit(arm64::str_q_uoff(3, REG_CPU_STATE, off_dhi >> 4));
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
