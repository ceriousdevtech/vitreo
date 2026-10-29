// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — packed FP arithmetic: ADD/SUB/MUL/DIV/SQRT PS and PD, and
// ADDSUBPS/ADDSUBPD.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
const bool kDisableAddpsForward =
    std::getenv("VITREO_JIT_SSE_DISABLE_ADDPS_FORWARD") != nullptr;
} // namespace

std::optional<bool> JitCompiler::compile_sse_fp_packed(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- Packed FP ADD/SUB/MUL — finite NEON; Inf/NaN → whole-insn helper.
    // Matches scalar ADDSS/ADDSD gate (Intel SDM Vol. 1 §4.8.3.4 destination-
    // first quieting / indefinite QNaN). Any Inf/NaN lane in either operand
    // falls back to jit_sse_op for the entire instruction.
    #define EMIT_SSE_NEON_RR_FINITE_FP(NEON_ENC, REG_OPID, MEM_OPID, IS_PD, DO_DIV) \
        { \
            /* VEX.256 reaches this only through compile_vex256_lane_split, \
             * which re-emits it once per 128-bit lane with vec_off() naming \
             * the half being written; a raw VEX.L instruction never gets \
             * here (the bridge at the top of compile_sse intercepts it). */ \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            /* SRC1 comes from vvvv for a VEX form (Intel SDM Vol. 2 VADDPS: \
             * DEST := SRC1 OP SRC2). Reading it from the destination — as \
             * this macro used to — only worked because the generic bridge \
             * copies vvvv->dst in front of the 128-bit lowering, and the \
             * lane-split driver deliberately suppresses that bridge. */ \
            u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15) \
                                             : dst_xmm; \
            u32 off_d = vec_off(dst_xmm); \
            u32 off_1 = vec_off(src1_xmm); \
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) \
                return false; \
            /* The cold arm stages lanes through BOTH halves by absolute \
             * offset, so both must be LDR/STR Q reachable. */ \
            if (m_vec_lane_split && \
                (!fits_ldr_q_offset(xmm_u64_off(dst_xmm, 0)) || \
                 !fits_ldr_q_offset(ymm_high_u64_off(dst_xmm, 0)) || \
                 !fits_ldr_q_offset(xmm_u64_off(src1_xmm, 0)) || \
                 !fits_ldr_q_offset(ymm_high_u64_off(src1_xmm, 0)))) { \
                return false; \
            } \
            /* Intel SDM Vol. 2 ADDPS/MULPS: Flags Affected = None.
             * The vector Inf/NaN gate is CMEQ (no NZCV) and FADD/FMUL
             * also leave NZCV alone, so a register-source path can keep
             * a live predecessor CMP in host NZCV (`CMP; ADDPS; JAE`).
             * Memory EA / emit_guest_load64 (x64 region SUBS) still
             * clobber; helper BLR does too. */ \
            const bool hold = should_hold_nzcv(); \
            const bool src_is_reg = is_register_operand(op1.type); \
            /* `vsubps ymm0, ymm1, ymm0`: the r/m source IS the destination \
             * and SRC1 is not. The cold arm below must stage SRC1 into \
             * xmm[dst] for the helper (xmm[dst] := xmm[dst] OP xmm[src]), \
             * which would destroy the r/m operand first. So on that shape \
             * the r/m lane is parked in a third register T (neither dst \
             * nor SRC1), whose own contents are saved in \
             * CpuState::sse.jit_vec_scratch and restored after the call; \
             * nothing vector-typed is held in a V register across the BLR. \
             * Only reachable under m_vec_lane_split (the VEX.256 driver, \
             * or the VEX.128 single pass for this exact aliasing). */ \
            const bool rm_alias = m_vec_lane_split && src_is_reg \
                && (op1.reg & 15) == dst_xmm && src1_xmm != dst_xmm; \
            u8 alias_tmp = 0; \
            if (rm_alias) { \
                while (alias_tmp == dst_xmm || alias_tmp == src1_xmm) \
                    ++alias_tmp; \
                if (!fits_ldr_q_offset(xmm_u64_off(alias_tmp, 0)) || \
                    !fits_ldr_q_offset(jit_vec_scratch_off())) \
                    return false; \
            } \
            /* Extend the PE32 write-through chain through a finite packed-FP \
             * operation only when it actually consumes the prior V0 value. \
             * Publishing every eligible FP result would grow isolated \
             * instructions with a cold-arm reload for no forwarding gain. */ \
            const bool pe32_forward_hit = src_is_reg \
                && !m_block_is_64bit && !m_current_original_vex \
                && !m_vec_lane_split \
                && m_xmm_v0_forward_in == src1_xmm; \
            /* Repeated legacy register ADDPS is a measured x64 hot shape. \
             * Bootstrap the existing one-instruction V0 forwarding contract \
             * for that exact form: every result is still committed to \
             * CpuState, while an adjacent consumer can omit its destination \
             * reload. The helper arm reload below restores the same contract \
             * after NaN/invalid fallback clobbers caller-saved V0. Keep VEX \
             * and lane-split forms out because their merge/high-lane rules \
             * are separate architectural contracts. */ \
            const bool addps_forward_eligible = src_is_reg \
                && instr.opcode == Opcode::ADDPS_XMM_XMMM128 \
                && !m_current_original_vex && !m_vec_lane_split \
                && !kDisableAddpsForward; \
            const bool forward_hit = (pe32_forward_hit \
                                      || addps_forward_eligible) \
                && m_xmm_v0_forward_in == src1_xmm; \
            const bool publish_v0 = pe32_forward_hit \
                || addps_forward_eligible; \
            /* XMM register cache. Only where no NaN gate is needed: under \
             * FPCR.AH the NEON op already IS the x86 result, so the whole \
             * lowering is one instruction on cache registers. */ \
            const bool rm_aligned = \
                !instr.prefixes.vex && !m_current_original_vex; \
            const bool rm_linear128 = m_block_is_64bit && m_cfg.linear64 \
                && m_cfg.linear64_vec128 && m_linear_base_pinned; \
            if (!(DO_DIV) && math::sse_alternate_nan_handling() && !rm_alias) { \
                if (src_is_reg) { \
                    if (xmm_cache_rr((NEON_ENC), dst_xmm, src1_xmm, \
                                     static_cast<u8>(op1.reg & 15))) \
                        break; \
                } else if (rm_linear128 && xmm_cache_fits(dst_xmm, src1_xmm)) { \
                    if (hold) emit_nzcv_spill(); \
                    const u8 ea = emit_ea64(REG_SCRATCH2, op1.mem, next_rip); \
                    if (rm_aligned) emit_align_check(ea, false); \
                    emit_linear64_load128(1, ea, rm_aligned); \
                    xmm_cache_rm((NEON_ENC), dst_xmm, src1_xmm); \
                    if (hold) emit_nzcv_fill(); \
                    break; \
                } \
            } \
            if (hold && !src_is_reg) emit_nzcv_spill(); \
            u8 src_xmm = 0; \
            bool pe32_source_alias_hit = false; \
            if (src_is_reg) { \
                src_xmm = op1.reg & 15; \
                u32 off_s = vec_off(src_xmm); \
                if (m_vec_lane_split && \
                    (!fits_ldr_q_offset(xmm_u64_off(src_xmm, 0)) || \
                     !fits_ldr_q_offset(ymm_high_u64_off(src_xmm, 0)))) { \
                    return false; \
                } \
                if (!fits_ldr_q_offset(off_s)) { \
                    /* Bare helper call: register indices always mean xmm[], \
                     * which cannot express a lane. Let the bridge run it. */ \
                    if (m_vec_lane_split) return false; \
                    if (hold) emit_nzcv_spill(); \
                    emit(arm64::mov_x(0, REG_CPU_STATE)); \
                    emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, \
                                          JIT_CTX_OFF_MEMORY >> 3)); \
                    emit(arm64::movz_w(2, REG_OPID)); \
                    emit(arm64::movz_w(3, dst_xmm)); \
                    emit(arm64::movz_w(4, src_xmm)); \
                    EMIT_SSE_OP_CALL(); \
                    if (hold) emit_nzcv_fill(); \
                    break; \
                } \
                if (!forward_hit) { \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                } \
                pe32_source_alias_hit = forward_hit \
                    && m_xmm_v0_alias_in == src_xmm; \
                if (!pe32_source_alias_hit) { \
                    emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
                } \
            } else { \
                /* Park EA in X21 for the whole insn. emit_x64_region_load
                 * clobbers X4 (slot offset); adding 8 to that leftover made
                 * the high qword load a small guest address (SUBPD #GP in a
                 * 64-bit guest). The Inf/NaN helper recomputes EA so X21 is only
                 * needed as the address, not as parked data. */ \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1); \
                /* Legacy SSE requires a 16-byte ALIGNED memory operand on \
                 * every one of these forms, not only the MOVAPS and MOVDQA \
                 * copies. Measured on silicon rather than read off the \
                 * manual: tools/oracle_probe/sse_align_probe.c misaligns \
                 * the operand by one and reports which forms raise. On a \
                 * real Alder Lake core XORPS, XORPD, PXOR, ANDPS, PAND, \
                 * ORPS and POR all fault exactly as MOVAPS and MOVDQA do, \
                 * while MOVUPS and MOVDQU do not. This path had no check, \
                 * so a misaligned operand silently succeeded. VEX forms are \
                 * exempt -- unaligned unless the opcode says otherwise \
                 * (Intel SDM Vol. 2), the same rule avx.cpp follows. */ \
                if (rm_aligned) \
                    emit_align_check(REG_CALL_TMP, false); \
                if (rm_linear128) { \
                    /* One checked 16-byte access, not two 8-byte ones. */ \
                    emit_linear64_load128(1, REG_CALL_TMP, rm_aligned); \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                } else if (!m_block_is_64bit && m_cfg.linear_jit && m_memory.is_linear()) { \
                    emit(arm64::mov_w(9, REG_CALL_TMP)); \
                    const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE \
                                                        : REG_SCRATCH3; \
                    if (!m_linear_base_pinned) { \
                        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX, \
                                                JIT_CTX_OFF_LINEAR_BASE >> 3)); \
                    } \
                    emit(arm64::add_x_uxtw(REG_SCRATCH3, base, 9)); \
                    emit(arm64::ldr_q_uoff(1, REG_SCRATCH3, 0)); \
                    /* Acquire: the 128-bit guest load has happened, so the \
                     * fence goes after it. The next LDR Q reads CpuState. */ \
                    emit_vector_tso_fence(); \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                } else { \
                    emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP); \
                    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8)); \
                    emit(arm64::mov_x(REG_EA_TMP, REG_SCRATCH0)); \
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4); \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                    emit(arm64::ins_v_gpr(1, 0, REG_EA_TMP, arm64::VS_D)); \
                    emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
                } \
            } \
            const arm64::VSize exp_sz = (IS_PD) ? arm64::VS_D : arm64::VS_S; \
            const u8 src_v = pe32_source_alias_hit ? 0 : 1; \
            /* Every finite macro instantiation encodes Vm=V1 in bits 20:16. \
             * When the one-instruction copy alias proves both operands equal, \
             * select V0 without manufacturing a redundant vector copy. */ \
            const u32 neon_enc = pe32_source_alias_hit \
                                     ? ((NEON_ENC) & ~(0x1Fu << 16)) \
                                     : (NEON_ENC); \
            usize special_br = 0; \
            bool special_br_is_cbz = false; \
            /* With FPCR.AH installed for every SSE mode, ARM ADD/SUB/MUL \
             * already return x86's NaN (first operand quieted; negative \
             * default NaN when invalid): nothing to check, no helper arm \
             * (math::sse_alternate_nan_handling). The block's SSE \
             * environment guard installs it before this runs. */ \
            const bool no_nan_gate = \
                !(DO_DIV) && math::sse_alternate_nan_handling(); \
            if (no_nan_gate) { \
                emit(neon_enc); \
            } else if (!(DO_DIV)) { \
                /* RESULT gate, not an operand gate.
                 *
                 * sse_float_semantics.hpp (the interpreter's reference, from
                 * Intel SDM Vol. 1 §4.8.3.4) diverges from ARM FADD/FSUB/FMUL
                 * in exactly three places, and ALL THREE produce a NaN:
                 *   1. both operands NaN, dst QNaN + src SNaN — x86 takes the
                 *      destination, ARM prefers the signalling one;
                 *   2. an invalid op on non-NaN operands (Inf-Inf, 0*Inf) —
                 *      x86 yields the Indefinite QNaN whose SIGN IS SET, ARM's
                 *      default NaN has it clear;
                 *   3. any NaN operand at all, which lands in 1 or falls out
                 *      identically.
                 * ADD/SUB/MUL touch no MXCSR sticky bit in the interpreter, so
                 * a non-NaN NEON result needs no fixup of any kind. Testing the
                 * result therefore covers the whole divergence set with ONE
                 * FCMEQ, and is strictly tighter than the old operand gate:
                 * Inf+1, Inf*2 and friends are exact on both ISAs and no longer
                 * pay a helper round trip.
                 *
                 * FCMEQ leaves NZCV alone, so the register-source path still
                 * carries a live predecessor CMP (`CMP; ADDPS; JAE`). */ \
                emit(neon_enc); \
                emit(arm64::fcmeq_v(2, 0, 0, \
                                    (IS_PD) ? arm64::FS_D : arm64::FS_S)); \
                /* FCMEQ made every non-NaN lane all-ones. UMINV across bytes \
                 * reduces the predicate to one byte: nonzero iff every lane is \
                 * a number, zero if any lane is NaN. */ \
                emit(arm64::uminv_v(2, 2, arm64::VS_B)); \
                emit(arm64::umov_w_b(REG_SCRATCH0, 2, 0)); \
                special_br = m_code_buf.size(); \
                special_br_is_cbz = true; \
                emit(0); /* cbz W0 → helper if any result lane is NaN */ \
            } else { \
                /* DIV keeps the OPERAND gate: MXCSR.ZE (finite/±0) and the
                 * exactness residual below are only defined for finite
                 * operands, so an Inf or NaN input cannot take this arm.
                 * V4 = exp mask; V2/V3 = per-operand matches. */ \
                if (IS_PD) { \
                    emit_mov_imm64(REG_SCRATCH2, \
                                   static_cast<i64>(0x7FF0000000000000ULL)); \
                } else { \
                    emit_mov_imm32(REG_SCRATCH2, 0x7F800000u); \
                } \
                emit(arm64::dup_v_gpr(4, REG_SCRATCH2, exp_sz)); \
                emit(arm64::and_v16b(2, 0, 4)); \
                emit(arm64::cmeq_v(2, 2, 4, exp_sz)); \
                if (!pe32_source_alias_hit) { \
                    emit(arm64::and_v16b(3, src_v, 4)); \
                    emit(arm64::cmeq_v(3, 3, 4, exp_sz)); \
                    emit(arm64::orr_v16b(2, 2, 3)); \
                } \
                /* ±0 divisor → helper (ZE, and 0/0 indefinite QNaN). */ \
                emit(arm64::shl_v_imm(3, src_v, 1, exp_sz)); \
                emit(arm64::cmeq_v_zero(3, 3, exp_sz)); \
                emit(arm64::orr_v16b(2, 2, 3)); \
                emit(arm64::umaxv_v(2, 2, arm64::VS_B)); \
                emit(arm64::umov_w_b(REG_SCRATCH0, 2, 0)); \
                special_br = m_code_buf.size(); \
                emit(0); /* cbnz X0 → helper if any Inf/NaN/zero-div lane */ \
                emit(arm64::mov_v16b(2, 0)); \
                emit(neon_enc); \
            } \
            if (DO_DIV) { \
                const arm64::FSize dsz = (IS_PD) ? arm64::FS_D : arm64::FS_S; \
                emit(arm64::fneg_v(3, 0, dsz)); \
                /* V6, not V4: V4 still holds the exponent mask the operand \
                 * gate built, and the finite-quotient test below needs it. \
                 * (V6/V7 are only used by the lane-split arm, past the call.) */ \
                emit(arm64::mov_v16b(6, 2)); \
                emit(arm64::fmla_v(6, 3, src_v, dsz)); \
                emit(arm64::eor_v16b(5, 5, 5)); \
                emit(arm64::fcmeq_v(6, 6, 5, dsz)); \
                /* sse_div only raises PE when the QUOTIENT is finite. Finite \
                 * operands can still overflow (pi / smallest-denormal = Inf), \
                 * and the residual a-r*b is then ±Inf, which the compare above \
                 * reads as "inexact" and would set a sticky bit the \
                 * interpreter never sets. Fold Inf/NaN quotient lanes into the \
                 * exact mask. */ \
                emit(arm64::and_v16b(3, 0, 4)); \
                emit(arm64::cmeq_v(3, 3, 4, exp_sz)); \
                emit(arm64::orr_v16b(6, 6, 3)); \
                emit(arm64::mov_w(REG_SCRATCH1, 31)); \
                if (IS_PD) { \
                    for (u8 lane = 0; lane < 2; ++lane) { \
                        emit(arm64::umov_x_d(REG_EA_TMP, 6, lane)); \
                        emit(arm64::mvn_x(REG_EA_TMP, REG_EA_TMP)); \
                        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, \
                                          REG_EA_TMP)); \
                    } \
                } else { \
                    for (u8 lane = 0; lane < 4; ++lane) { \
                        emit(arm64::umov_w_s(REG_EA_TMP, 6, lane)); \
                        emit(arm64::mvn_w(REG_EA_TMP, REG_EA_TMP)); \
                        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, \
                                          REG_EA_TMP)); \
                    } \
                } \
                const usize pe_cbz = m_code_buf.size(); \
                emit(0); \
                { \
                    const u32 mxcsr_off = static_cast<u32>( \
                        offsetof(CpuState, sse) + offsetof(SseState, mxcsr)); \
                    emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE, \
                                           mxcsr_off >> 2)); \
                    emit_mov_imm32(REG_SCRATCH3, MXCSR_PE); \
                    emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, \
                                      REG_SCRATCH3)); \
                    emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE, \
                                           mxcsr_off >> 2)); \
                } \
                m_code_buf[pe_cbz] = arm64::cbz_w( \
                    REG_SCRATCH1, \
                    static_cast<i32>(m_code_buf.size() - pe_cbz) * 4); \
            } \
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            if (no_nan_gate) { \
                if (publish_v0) { \
                    m_xmm_v0_forward_out = dst_xmm; \
                } \
                if (hold && !src_is_reg) emit_nzcv_fill(); \
                break; \
            } \
            if (!m_vec_lane_split && !rm_alias) { \
                /* The helper arm runs only on a NaN result (or, for DIV, \
                 * a special operand), so it goes out of line: inline it \
                 * was ~35 never-executed words in the hot path plus a \
                 * taken B over them on every execution. Same words, \
                 * placed in the cold region (defer_cold_arm). */ \
                const DecodedInstruction arm_instr = instr; \
                const Operand arm_op1 = op1; \
                const bool cbz_arm = special_br_is_cbz; \
                std::vector<ColdArmEntry> arm_entries; \
                arm_entries.push_back({special_br, [cbz_arm](i32 disp) { \
                        return cbz_arm ? arm64::cbz_w(REG_SCRATCH0, disp) \
                                       : arm64::cbnz_x(REG_SCRATCH0, disp); \
                    }}); \
                defer_cold_arm(std::move(arm_entries), \
                    [this, arm_instr, arm_op1, next_rip, src_is_reg, hold, \
                     publish_v0, dst_xmm, src_xmm, off_d]() { \
                        emit(arm64::mov_x(0, REG_CPU_STATE)); \
                        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, \
                                               JIT_CTX_OFF_MEMORY >> 3)); \
                        if (src_is_reg) { \
                            emit(arm64::movz_w(2, REG_OPID)); \
                            emit(arm64::movz_w(3, dst_xmm)); \
                            emit(arm64::movz_w(4, src_xmm)); \
                        } else { \
                            emit_compute_ea(REG_CALL_TMP, arm_op1.mem, next_rip); \
                            emit_m128_align_check_if_required(REG_CALL_TMP, \
                                                              arm_instr, arm_op1); \
                            emit(arm64::movz_w(2, MEM_OPID)); \
                            emit(arm64::movz_w(3, dst_xmm)); \
                            emit(arm64::mov_x(4, REG_CALL_TMP)); \
                        } \
                        if (hold && src_is_reg) emit_nzcv_spill(); \
                        EMIT_SSE_OP_CALL(); \
                        if (hold && src_is_reg) emit_nzcv_fill(); \
                        if (publish_v0) { \
                            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
                        } \
                    }, /*semantic_helper=*/true); \
                if (publish_v0) { \
                    m_xmm_v0_forward_out = dst_xmm; \
                } \
                if (hold && !src_is_reg) emit_nzcv_fill(); \
                break; \
            } \
            const usize native_done = m_code_buf.size(); \
            emit(0); /* b → done */ \
            const usize slow_pos = m_code_buf.size(); \
            m_code_buf[special_br] = special_br_is_cbz \
                ? arm64::cbz_w(REG_SCRATCH0, \
                               static_cast<i32>(slow_pos - special_br) * 4) \
                : arm64::cbnz_x(REG_SCRATCH0, \
                                static_cast<i32>(slow_pos - special_br) * 4); \
            /* jit_sse_op computes xmm[dst] := xmm[dst] OP xmm[src] — register \
             * INDICES, so it is structurally lane-blind. Under a lane split \
             * the operands must therefore be moved into xmm[] first. \
             * \
             * Low lane: xmm[] already holds the right 128 bits; all that is \
             * missing is the merge the suppressed bridge would have done, \
             * xmm[dst] := xmm[SRC1]. xmm[dst] is dead here — VEX dst is a \
             * pure destination, and when dst == r/m src its value was parked \
             * in xmm[alias_tmp] above (rm_alias). \
             * \
             * High lane: physically SWAP the halves around the call, so \
             * nothing is held in a register across the BLR (V6/V7 are \
             * caller-saved, and even v8-v15 preserve only their low 64 bits \
             * under AAPCS64 §6.1.2 — a 128-bit value cannot survive a call \
             * in ANY register). Fused with the merge on the destination: \
             *   xmm[dst]      := ymm_high[SRC1]        (helper's SRC1) \
             *   ymm_high[dst] := old xmm[dst]          (lane-0 result, parked) \
             * When SRC1 == dst that is exactly a swap; when SRC1 != dst, \
             * ymm_high[dst] is dead (the unswap below overwrites it with the \
             * result). The r/m register gets a true swap, undone afterwards. */ \
            if (rm_alias) { \
                emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                       xmm_u64_off(alias_tmp, 0) >> 4)); \
                emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                       jit_vec_scratch_off() >> 4)); \
                emit(arm64::ldr_q_uoff(7, REG_CPU_STATE, \
                                       (m_vec_hi_lane \
                                            ? ymm_high_u64_off(dst_xmm, 0) \
                                            : xmm_u64_off(dst_xmm, 0)) >> 4)); \
                emit(arm64::str_q_uoff(7, REG_CPU_STATE, \
                                       xmm_u64_off(alias_tmp, 0) >> 4)); \
            } \
            if (m_vec_lane_split) { \
                if (!m_vec_hi_lane) { \
                    if (src1_xmm != dst_xmm) { \
                        emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                               xmm_u64_off(src1_xmm, 0) >> 4)); \
                        emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                               xmm_u64_off(dst_xmm, 0) >> 4)); \
                    } \
                } else { \
                    emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                           ymm_high_u64_off(src1_xmm, 0) >> 4)); \
                    emit(arm64::ldr_q_uoff(7, REG_CPU_STATE, \
                                           xmm_u64_off(dst_xmm, 0) >> 4)); \
                    emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                           xmm_u64_off(dst_xmm, 0) >> 4)); \
                    emit(arm64::str_q_uoff(7, REG_CPU_STATE, \
                                           ymm_high_u64_off(dst_xmm, 0) >> 4)); \
                    if (src_is_reg && src_xmm != dst_xmm) { \
                        emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                               xmm_u64_off(src_xmm, 0) >> 4)); \
                        emit(arm64::ldr_q_uoff(7, REG_CPU_STATE, \
                                               ymm_high_u64_off(src_xmm, 0) >> 4)); \
                        emit(arm64::str_q_uoff(7, REG_CPU_STATE, \
                                               xmm_u64_off(src_xmm, 0) >> 4)); \
                        emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                               ymm_high_u64_off(src_xmm, 0) >> 4)); \
                    } \
                } \
            } \
            emit(arm64::mov_x(0, REG_CPU_STATE)); \
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3)); \
            if (is_register_operand(op1.type)) { \
                emit(arm64::movz_w(2, REG_OPID)); \
                emit(arm64::movz_w(3, dst_xmm)); \
                emit(arm64::movz_w(4, rm_alias ? alias_tmp : src_xmm)); \
            } else { \
                /* Dual-load parks the low qword in X21. Recompute EA so \
                 * Inf/NaN helper rereads the guest address, not data \
                 * (Intel SDM Vol. 1 §4.8.3.4 fallback). The lane-split \
                 * driver already added 16 to the displacement on the high \
                 * lane, so this recompute names the right 16 bytes. */ \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1); \
                emit(arm64::movz_w(2, MEM_OPID)); \
                emit(arm64::movz_w(3, dst_xmm)); \
                emit(arm64::mov_x(4, REG_CALL_TMP)); \
            } \
            /* BLR clobbers NZCV. Spill only on the register path (memory
             * already spilled at entry) and fill before the merge so the
             * finite fast path does not execute a fill it never spilled.
             * The spill moved above the helper-call sequence so this site
             * can use the same choke point as every other jit_sse_op call;
             * it only touches X11 and the SP+8 slot, never X16. */ \
            if (hold && src_is_reg) emit_nzcv_spill(); \
            EMIT_SSE_OP_CALL(); \
            if (hold && src_is_reg) emit_nzcv_fill(); \
            /* Undo the high-lane swap: the r/m register goes back verbatim, \
             * and the destination's two halves trade places so the result \
             * lands in ymm_high[dst] and the parked lane-0 result returns to \
             * xmm[dst]. */ \
            if (m_vec_lane_split && m_vec_hi_lane) { \
                if (src_is_reg && src_xmm != dst_xmm) { \
                    emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                           xmm_u64_off(src_xmm, 0) >> 4)); \
                    emit(arm64::ldr_q_uoff(7, REG_CPU_STATE, \
                                           ymm_high_u64_off(src_xmm, 0) >> 4)); \
                    emit(arm64::str_q_uoff(7, REG_CPU_STATE, \
                                           xmm_u64_off(src_xmm, 0) >> 4)); \
                    emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                           ymm_high_u64_off(src_xmm, 0) >> 4)); \
                } \
                emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                       xmm_u64_off(dst_xmm, 0) >> 4)); \
                emit(arm64::ldr_q_uoff(7, REG_CPU_STATE, \
                                       ymm_high_u64_off(dst_xmm, 0) >> 4)); \
                emit(arm64::str_q_uoff(7, REG_CPU_STATE, \
                                       xmm_u64_off(dst_xmm, 0) >> 4)); \
                emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                       ymm_high_u64_off(dst_xmm, 0) >> 4)); \
            } \
            if (rm_alias) { \
                emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, \
                                       jit_vec_scratch_off() >> 4)); \
                emit(arm64::str_q_uoff(6, REG_CPU_STATE, \
                                       xmm_u64_off(alias_tmp, 0) >> 4)); \
            } \
            /* The helper writes the architectural destination but clobbers \
             * caller-saved V0. Reload only on this cold arm so both paths \
             * reach the join with the same forwarding contract. The native \
             * arm's branch below skips this instruction. */ \
            if (publish_v0) { \
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            } \
            const usize done_pos = m_code_buf.size(); \
            m_code_buf[native_done] = arm64::b_imm( \
                static_cast<i32>(done_pos - native_done) * 4); \
            if (publish_v0) { \
                m_xmm_v0_forward_out = dst_xmm; \
            } \
            if (hold && !src_is_reg) emit_nzcv_fill(); \
            break; \
        }

    case Opcode::ADDPS_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fadd_v(0, 0, 1, arm64::FS_S), 300, 301, false, false)
    case Opcode::SUBPS_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fsub_v(0, 0, 1, arm64::FS_S), 302, 303, false, false)
    case Opcode::MULPS_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fmul_v(0, 0, 1, arm64::FS_S), 304, 305, false, false)
    case Opcode::DIVPS_XMM_XMMM128:
        // Finite nonzero: NEON FDIV + FMA residual → MXCSR.PE.
        // Inf/NaN/±0 still use the helper (ZE / indefinite QNaN).
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fdiv_v(0, 0, 1, arm64::FS_S), 306, 307, false, true)
    // SQRTPS / SQRTPD: dst = sqrt(src), lane-wise.
    //
    // Bare FSQRT is NOT the x86 semantics. For a negative operand both
    // architectures signal invalid and return their DEFAULT NaN, and the two
    // defaults differ in the sign bit: ARM's is +QNaN (0x7FC00000), x86's
    // "QNaN floating-point indefinite" has the sign SET (0xFFC00000, Intel SDM
    // Vol. 1 Â§4.8.3.7). NaN inputs are propagated identically by both, so only
    // the invalid-operation lanes need correcting.
    //
    // FCMGT against zero is the right lane predicate: it is false for NaN (so a
    // propagated NaN is left alone) and false for -0, whose square root is -0
    // and not an invalid operation.
    //
    // Found by the ISA matrix sweep; the interpreter and hardware agree against
    // the JIT. This is one member of the recorded "NaN sign/payload wrong on
    // several SSE paths" family, now pinned to a specific instruction.
    #define EMIT_SSE_X86_SQRT(IS_PD, REG_OPID, MEM_OPID) \
        { \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            const arm64::FSize fs = (IS_PD) ? arm64::FS_D : arm64::FS_S; \
            auto emit_x86_sqrt = [&]() { \
                emit(arm64::movi_v16b(2, 0)); \
                emit(arm64::fcmgt_v(2, 2, 1, fs)); \
                if (IS_PD) { \
                    emit_mov_imm64(REG_SCRATCH0, \
                        static_cast<i64>(0xFFF8000000000000ull)); \
                    emit(arm64::dup_v_gpr(3, REG_SCRATCH0, arm64::VS_D)); \
                } else { \
                    emit_mov_imm32(REG_SCRATCH0, 0xFFC00000u); \
                    emit(arm64::dup_v_gpr(3, REG_SCRATCH0, arm64::VS_S)); \
                } \
                emit(arm64::fsqrt_v(0, 1, fs)); \
                emit(arm64::bsl_v16b(2, 3, 0)); \
                emit(arm64::mov_v16b(0, 2)); \
            }; \
            if (is_register_operand(op1.type)) { \
                u8 src_xmm = op1.reg & 15; \
                u32 off_d = vec_off(dst_xmm); \
                u32 off_s = vec_off(src_xmm); \
                if (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_s)) { \
                    emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
                    emit_x86_sqrt(); \
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
                    break; \
                } \
                if (m_vec_hi_lane) return false; \
                emit(arm64::mov_x(0, REG_CPU_STATE)); \
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, \
                                       JIT_CTX_OFF_MEMORY >> 3)); \
                emit(arm64::movz_w(2, REG_OPID)); \
                emit(arm64::movz_w(3, dst_xmm)); \
                emit(arm64::movz_w(4, src_xmm)); \
                EMIT_SSE_OP_CALL(); \
            } else { \
                u32 off_d = vec_off(dst_xmm); \
                if (!fits_ldr_q_offset(off_d)) return false; \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                /* 128-bit memory operand: legacy SSE requires 16-byte \
                 * alignment, measured on silicon -- see the note in \
                 * EMIT_SSE_NEON_RR. VEX is exempt. */ \
                if (!instr.prefixes.vex && !m_current_original_vex) \
                    emit_align_check(REG_CALL_TMP, false); \
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP); \
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8)); \
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0)); \
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4); \
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D)); \
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
                emit_x86_sqrt(); \
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            } \
            break; \
        }

    case Opcode::SQRTPS_XMM_XMMM128:
        EMIT_SSE_X86_SQRT(false, 312, 313)
    case Opcode::ADDPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fadd_v(0, 0, 1, arm64::FS_D), 314, 315, true, false)
    case Opcode::SUBPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fsub_v(0, 0, 1, arm64::FS_D), 316, 317, true, false)
    case Opcode::MULPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fmul_v(0, 0, 1, arm64::FS_D), 318, 319, true, false)
    case Opcode::DIVPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_FINITE_FP(arm64::fdiv_v(0, 0, 1, arm64::FS_D), 320, 321, true, true)
    case Opcode::SQRTPD_XMM_XMMM128:
        EMIT_SSE_X86_SQRT(true, 326, 327)
    #undef EMIT_SSE_X86_SQRT

    // ADDSUBPS/PD — even-indexed lanes subtract, odd-indexed add (Intel SDM
    // Vol. 2 ADDSUBPS: DEST[31:0] := SRC1[31:0] - SRC2[31:0], DEST[63:32] :=
    // SRC1[63:32] + SRC2[63:32], ...). Each lane is an ordinary SSE add or
    // subtract, so it follows the Vol. 1 §4.8.3.4 NaN rules exactly as
    // ADDPS/SUBPS do (sse_float_semantics.hpp): SRC1 NaN wins over SRC2 NaN,
    // an SNaN is quietened, and a generated NaN (inf - inf) is the Indefinite
    // QNaN with its SIGN SET -- none of which ARM FADD/FSUB produce, so the
    // FMA NaN fixup is applied with SRC1 as the highest-priority operand.
    //
    // Lane-split ready: SRC1 is read from vvvv for a VEX form, every operand
    // is addressed through vec_off(), and there is no helper arm, so the
    // VEX.256 form runs this once per 128-bit lane (the operation is
    // element-wise and each lane starts on an even element).
    case Opcode::ADDSUBPS_XMM_XMMM128:
    case Opcode::ADDSUBPD_XMM_XMMM128: {
        if (instr.prefixes.vex_l) return false;
        const bool dbl = instr.opcode == Opcode::ADDSUBPD_XMM_XMMM128;
        const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;
        const u8 dst_xmm = op0.reg & 15;
        const u8 src1_xmm = instr.prefixes.vex
            ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
        const u32 off_d = vec_off(dst_xmm);
        const u32 off_1 = vec_off(src1_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) {
            return false;
        }
        const bool src_is_reg = is_register_operand(op1.type);
        const u32 off_s = src_is_reg ? vec_off(op1.reg & 15) : 0;
        if (src_is_reg && !fits_ldr_q_offset(off_s)) return false;
        const bool hold = !src_is_reg && should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        if (src_is_reg) {
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // Both qwords into GPRs first: the guest-load slow path BLRs a
            // helper that clobbers V0-V7. EA parked in X21, EA+8 by ADD X.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));   // SRC1
        emit(arm64::fsub_v(2, 0, 1, fs));                          // even
        emit(arm64::fadd_v(4, 0, 1, fs));                          // odd
        emit(arm64::ins_v_elem(4, 0, 2, 0, dbl ? arm64::VS_D : arm64::VS_S));
        if (!dbl) emit(arm64::ins_v_elem(4, 2, 2, 2, arm64::VS_S));
        emit_x86_fma_nan_fixup(4, 0, 1, 1, dbl, 2, 3, 5);
        emit(arm64::str_q_uoff(4, REG_CPU_STATE, off_d >> 4));
        if (hold) emit_nzcv_fill();
        break;
    }

    #undef EMIT_SSE_NEON_RR_FINITE_FP

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
