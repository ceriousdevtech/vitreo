// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — scalar conversions: CVTSI2SS/SD, CVT(T)SS2SI and
// CVT(T)SD2SI in r32 and r64 forms, CVTSS2SD and CVTSD2SS.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_conv_scalar(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // Intel SDM Vol. 2 CVTSI2SS xmm, r/m32: i32 → f32 lane 0; upper 96 kept.
    // Match interpreter (C++ cast / SCVTF); default MXCSR.RC.
    case Opcode::CVTSI2SS_XMM_RM32: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_w_offset(off)) return false;
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        // SCVTF Sd, Wn — ARM ARM C6.2.244 (same encoding as CVTPI2PS).
        emit(0x1E220000u | (static_cast<u32>(REG_SCRATCH0) << 5) | 0u);
        emit(0x1E380000u | (0u << 5) | static_cast<u32>(REG_SCRATCH1));
        emit(arm64::subs_w(31, REG_SCRATCH1, REG_SCRATCH0));
        const u32 b_exact_si2ss = static_cast<u32>(m_code_buf.size());
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
        const u32 si2ss_done = static_cast<u32>(m_code_buf.size());
        m_code_buf[b_exact_si2ss] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(si2ss_done - b_exact_si2ss) * 4);
        emit(arm64::fmov_w_from_s(REG_SCRATCH0, 0));
        emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 2));
        break;
    }

    // Intel SDM Vol. 2 CVTSI2SD xmm, r/m32: i32 → f64 low qword; high kept.
    case Opcode::CVTSI2SD_XMM_RM32: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_x_offset(off)) return false;
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::scvtf_d_from_w(0, REG_SCRATCH0));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
        break;
    }

    // Intel SDM Vol. 2 CVTSS2SI / CVTTSS2SI r32, xmm/m32.
    // Round (FCVTNS) or truncate (FCVTZS); indefinite 0x80000000 on
    // overflow/NaN — same contract as CVTPS2PI / interpreter.
    case Opcode::CVTSS2SI_R32_XMMM32:
    case Opcode::CVTTSS2SI_R32_XMMM32: {
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_w_offset(src_off)) return false;
            emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 2));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        const bool truncate = (instr.opcode == Opcode::CVTTSS2SI_R32_XMMM32);
        emit(arm64::fmov_s_from_w(0, REG_SCRATCH0));
        emit(0x1E202000u | (0u << 16) | (0u << 5));  // FCMP S0, S0
        const u32 b_nan = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm32(REG_SCRATCH2, 0x4F000000u);  // 2^31 as f32
        emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
        emit(0x1E202000u | (1u << 16) | (0u << 5));
        const u32 b_ge = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm32(REG_SCRATCH2, 0xCF000000u);  // INT32_MIN as f32
        emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
        emit(0x1E202000u | (1u << 16) | (0u << 5));
        const u32 b_lt = static_cast<u32>(m_code_buf.size());
        emit(0);
        if (truncate) {
                 emit(0x1E380000u | static_cast<u32>(REG_SCRATCH0));
             } else {
                 // CVTSS2SI/CVTSD2SI round per MXCSR.RC; only the
                 // CVTT* forms truncate unconditionally.
                 emit_fcvt_scalar_by_mxcsr(0x1E200000u | static_cast<u32>(REG_SCRATCH0));
             }
        // Intel SDM Vol. 2: sticky PE when the in-range conversion is inexact.
        emit(0x1E220000u | (static_cast<u32>(REG_SCRATCH0) << 5) | 1u);  // SCVTF S1, W
        emit(0x1E202000u | (1u << 16) | (0u << 5));  // FCMP S0, S1
        const u32 b_exact = static_cast<u32>(m_code_buf.size());
        emit(0);
        {
            const u32 mxcsr_off = static_cast<u32>(
                offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
            emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
            emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
            emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
            emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
        }
        const u32 b_done = static_cast<u32>(m_code_buf.size());
        emit(0);
        const u32 indef = static_cast<u32>(m_code_buf.size());
        emit_mov_imm32(REG_SCRATCH0, 0x80000000u);
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
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // Intel SDM Vol. 2 CVTSD2SI / CVTTSD2SI r32, xmm/m64.
    case Opcode::CVTSD2SI_R32_XMMM64:
    case Opcode::CVTTSD2SI_R32_XMMM64: {
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) return false;
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        }
        const bool truncate = (instr.opcode == Opcode::CVTTSD2SI_R32_XMMM64);
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fcmp_d(0, 0));
        const u32 b_nan = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm64(REG_SCRATCH2, 0x41E0000000000000LL);  // 2^31
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
        emit(arm64::fcmp_d(0, 1));
        const u32 b_ge = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0xC1E0000000000000ULL));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
        emit(arm64::fcmp_d(0, 1));
        const u32 b_lt = static_cast<u32>(m_code_buf.size());
        emit(0);
        if (truncate) {
                 emit(0x1E780000u | static_cast<u32>(REG_SCRATCH0));
             } else {
                 // CVTSS2SI/CVTSD2SI round per MXCSR.RC; only the
                 // CVTT* forms truncate unconditionally.
                 emit_fcvt_scalar_by_mxcsr(0x1E600000u | static_cast<u32>(REG_SCRATCH0));
             }
        emit(arm64::scvtf_d_from_w(1, REG_SCRATCH0));
        emit(arm64::fcmp_d(0, 1));
        const u32 b_exact = static_cast<u32>(m_code_buf.size());
        emit(0);
        {
            const u32 mxcsr_off = static_cast<u32>(
                offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
            emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
            emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
            emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
            emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
        }
        const u32 b_done = static_cast<u32>(m_code_buf.size());
        emit(0);
        const u32 indef = static_cast<u32>(m_code_buf.size());
        emit_mov_imm32(REG_SCRATCH0, 0x80000000u);
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
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // Intel SDM Vol. 2 CVTSS2SI / CVTTSS2SI r64, xmm/m32 (REX.W form).
    // Same compare tree as the r32 arm, with three differences that are the
    // whole point of a separate case:
    //   * the in-range window is ±2^63, not ±2^31, so a 1.0e10 source is a
    //     legal conversion here and indefinite there;
    //   * the FCVT/SCVTF encodings carry sf=1 (X destination);
    //   * the indefinite value is 0x8000000000000000 and is stored with
    //     emit_store_gpr64 — emit_store_gpr32 would zero-extend it into
    //     0x0000000080000000 and also miss the 64-bit register pin.
    // The bare ARM FCVT* saturate (NaN→0, +overflow→INT64_MAX), which is not
    // the x86 contract, so the compare tree is mandatory.
    // Contract: cvt_f32_to_i64_round / cvt_f32_to_i64_trunc in
    // interpreter/ops/sse_conv.cpp.
    case Opcode::CVTSS2SI_R64_XMMM32:
    case Opcode::CVTTSS2SI_R64_XMMM32: {
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_w_offset(src_off)) return false;
            emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 2));
        } else {
            // REG_SCRATCH4 must be dead before the convert below:
            // emit_fcvt_scalar_by_mxcsr uses W3/W4 as its own scratch.
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        const bool truncate = (instr.opcode == Opcode::CVTTSS2SI_R64_XMMM32);
        emit(arm64::fmov_s_from_w(0, REG_SCRATCH0));
        emit(arm64::fcmp_s(0, 0));  // NaN sets V
        const u32 b_nan = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm32(REG_SCRATCH2, 0x5F000000u);  // +2^63 as f32
        emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
        emit(arm64::fcmp_s(0, 1));
        const u32 b_ge = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm32(REG_SCRATCH2, 0xDF000000u);  // -2^63 as f32
        emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
        emit(arm64::fcmp_s(0, 1));
        const u32 b_lt = static_cast<u32>(m_code_buf.size());
        emit(0);
        if (truncate) {
            // FCVTZS Xd, S0
            emit(0x9E380000u | static_cast<u32>(REG_SCRATCH0));
        } else {
            // CVTSS2SI rounds per MXCSR.RC; only the CVTT* form truncates.
            emit_fcvt_scalar_by_mxcsr(0x9E200000u
                                      | static_cast<u32>(REG_SCRATCH0));
        }
        // Intel SDM Vol. 2: sticky PE when the in-range conversion is inexact.
        // SCVTF S1, Xd — exact for every value that reaches here.
        emit(0x9E220000u | (static_cast<u32>(REG_SCRATCH0) << 5) | 1u);
        emit(arm64::fcmp_s(0, 1));
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
        // The interpreter deliberately sets NO MXCSR bit on this path (not IE,
        // not PE). A JIT-only IE would break parity; leave it alone.
        emit_mov_imm64(REG_SCRATCH0,
                       static_cast<i64>(0x8000000000000000ull));
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
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // Intel SDM Vol. 2 CVTSD2SI / CVTTSD2SI r64, xmm/m64 (REX.W form).
    // Contract: cvt_f64_to_i64_round / cvt_f64_to_i64_trunc.
    case Opcode::CVTSD2SI_R64_XMMM64:
    case Opcode::CVTTSD2SI_R64_XMMM64: {
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) return false;
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        }
        const bool truncate = (instr.opcode == Opcode::CVTTSD2SI_R64_XMMM64);
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fcmp_d(0, 0));  // NaN sets V
        const u32 b_nan = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm64(REG_SCRATCH2, 0x43E0000000000000LL);  // +2^63
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
        emit(arm64::fcmp_d(0, 1));
        const u32 b_ge = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit_mov_imm64(REG_SCRATCH2,
                       static_cast<i64>(0xC3E0000000000000ull));  // -2^63
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
        emit(arm64::fcmp_d(0, 1));
        const u32 b_lt = static_cast<u32>(m_code_buf.size());
        emit(0);
        if (truncate) {
            // FCVTZS Xd, D0
            emit(0x9E780000u | static_cast<u32>(REG_SCRATCH0));
        } else {
            emit_fcvt_scalar_by_mxcsr(0x9E600000u
                                      | static_cast<u32>(REG_SCRATCH0));
        }
        // SCVTF D1, Xd — exact for every value that reaches here.
        emit(arm64::scvtf_d_from_x(1, REG_SCRATCH0));
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
        emit_mov_imm64(REG_SCRATCH0,
                       static_cast<i64>(0x8000000000000000ull));
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
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // Intel SDM Vol. 2 CVTSS2SD: f32 → f64 in low qword; high qword kept.
    case Opcode::CVTSS2SD_XMM_XMMM32: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_x_offset(off)) return false;
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_w_offset(src_off)) return false;
            emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 2));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::fmov_s_from_w(0, REG_SCRATCH0));
        emit(arm64::fcvt_d_from_s(0, 0));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
        break;
    }

    // Intel SDM Vol. 2 CVTSD2SS: f64 → f32 lane 0; upper 96 bits kept.
    case Opcode::CVTSD2SS_XMM_XMMM64: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_w_offset(off)) return false;
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) return false;
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fcvt_s_from_d(1, 0));
        emit(arm64::fcvt_d_from_s(1, 1));
        emit(arm64::fcmp_d(0, 1));
        const u32 b_exact_sd2ss = static_cast<u32>(m_code_buf.size());
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
        const u32 sd2ss_done = static_cast<u32>(m_code_buf.size());
        m_code_buf[b_exact_sd2ss] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(sd2ss_done - b_exact_sd2ss) * 4);
        emit(arm64::fcvt_s_from_d(0, 0));
        emit(arm64::fmov_w_from_s(REG_SCRATCH0, 0));
        emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 2));
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
