// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — RCPPS/RSQRTPS and RCPSS/RSQRTSS.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_fp_reciprocal(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // Unary packed-single: DEST := f(SRC), no SRC1. V1 = SRC; the body leaves
    // the result in V0. Every operand goes through vec_off() and there is no
    // helper arm, so the VEX.256 form runs it once per 128-bit lane through
    // compile_vex256_lane_split (Intel SDM Vol. 2 VRCPPS/VRSQRTPS: the 256-bit
    // form applies the operation to all eight elements). The VEX forms have
    // no vvvv operand, so nothing here reads it. Memory: both qwords into
    // GPRs before V1 is formed (the guest-load slow path BLRs a helper that
    // clobbers V0-V7); EA parked in X21, EA+8 by ADD X.
    #define EMIT_SSE_UNARY_PS(OP_ENC) \
        { \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            u32 off_d = vec_off(dst_xmm); \
            if (!fits_ldr_q_offset(off_d)) return false; \
            const bool unary_reg = is_register_operand(op1.type); \
            const u32 off_s = unary_reg ? vec_off(op1.reg & 15) : 0; \
            if (unary_reg && !fits_ldr_q_offset(off_s)) return false; \
            const bool unary_hold = !unary_reg && should_hold_nzcv(); \
            if (unary_hold) emit_nzcv_spill(); \
            if (unary_reg) { \
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
            } else { \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1); \
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP); \
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8)); \
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0)); \
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4); \
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D)); \
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
            } \
            OP_ENC; \
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            if (unary_hold) emit_nzcv_fill(); \
            break; \
        }

    // RCPPS — packed 1.0/src.  Interpreter uses exact `1.0f / src`;
    // FMOV V2.4S, #1.0 then FDIV matches bit-exactly.
    case Opcode::RCPPS_XMM_XMMM128:
        if (is_register_operand(op1.type) && !m_block_is_64bit
            && !m_current_original_vex && !m_vec_lane_split) {
            const u8 dst_xmm = op0.reg & 15;
            const u8 src_xmm = op1.reg & 15;
            const u32 off_d = xmm_u64_off(dst_xmm, 0);
            const u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_s)) {
                return false;
            }
            const bool source_in_v0 = m_xmm_v0_forward_in == src_xmm
                || m_xmm_v0_alias_in == src_xmm;
            if (!source_in_v0) {
                emit(arm64::ldr_q_uoff(
                    0, REG_CPU_STATE, off_s >> 4));
            }
            emit(arm64::fmov_v4s_one(2));
            emit(arm64::fdiv_v(0, 2, 0, arm64::FS_S));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            m_xmm_v0_forward_out = dst_xmm;
            break;
        }
        EMIT_SSE_UNARY_PS(({
            emit(arm64::fmov_v4s_one(2));
            emit(arm64::fdiv_v(0, 2, 1, arm64::FS_S));
        }))

    // RSQRTPS — 1.0/sqrt(src).
    case Opcode::RSQRTPS_XMM_XMMM128:
        if (is_register_operand(op1.type) && !m_block_is_64bit
            && !m_current_original_vex && !m_vec_lane_split) {
            const u8 dst_xmm = op0.reg & 15;
            const u8 src_xmm = op1.reg & 15;
            const u32 off_d = xmm_u64_off(dst_xmm, 0);
            const u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_s)) {
                return false;
            }
            const bool source_in_v0 = m_xmm_v0_forward_in == src_xmm
                || m_xmm_v0_alias_in == src_xmm;
            if (!source_in_v0) {
                emit(arm64::ldr_q_uoff(
                    0, REG_CPU_STATE, off_s >> 4));
            }
            emit(arm64::fsqrt_v(0, 0, arm64::FS_S));
            emit(arm64::fmov_v4s_one(2));
            emit(arm64::fdiv_v(0, 2, 0, arm64::FS_S));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            m_xmm_v0_forward_out = dst_xmm;
            break;
        }
        EMIT_SSE_UNARY_PS(({
            emit(arm64::fsqrt_v(0, 1, arm64::FS_S));
            emit(arm64::fmov_v4s_one(2));
            emit(arm64::fdiv_v(0, 2, 0, arm64::FS_S));
        }))

    #undef EMIT_SSE_UNARY_PS

    // Intel SDM Vol. 2 RCPSS / RSQRTSS: exact 1/x and 1/sqrt(x) on lane 0
    // (same as interpreter / RCPPS native FDIV path). Upper lanes kept.
    // Load dest Q after any guest load: emit_guest_load32 helper BLR
    // clobbers V0–V7 (same ordering as PINSRD mem).
    case Opcode::RCPSS_XMM_XMMM32:
    case Opcode::RSQRTSS_XMM_XMMM32: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        if (is_register_operand(op1.type)) {
            const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_w_offset(off_s)) return false;
            emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, off_s >> 2));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        emit(arm64::fmov_s_from_w(1, REG_SCRATCH0));
        if (instr.opcode == Opcode::RSQRTSS_XMM_XMMM32) {
            emit(arm64::fsqrt_s_scalar(1, 1, arm64::FS_S));
        }
        emit(arm64::fmov_v4s_one(2));
        emit(arm64::fdiv_s_scalar(1, 2, 1, arm64::FS_S));
        emit(arm64::ins_v_elem(0, 0, 1, 0, arm64::VS_S));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
