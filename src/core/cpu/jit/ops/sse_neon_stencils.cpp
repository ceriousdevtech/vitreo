// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the Tier-1 reg-reg NEON stencils: PABS, PADDS/PSUBS, the
// horizontal adds and subtracts, MOVDDUP/MOVSLDUP/MOVSHDUP, PMULHRSW,
// PSIGN*, PMULDQ/PMULUDQ and the PACK family.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_neon_stencils(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- Tier-1 native stencils (no helper op_id — reg-reg only).
    // Memory-operand variants return false and are picked up by the
    // generic interpreter delegate in compile_instruction.  Each op is
    // a direct 1:1 ARM64 NEON mapping verified against the oracle.
    #define EMIT_SSE_NEON_RR_REGONLY(NEON_ENC) \
        { \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15) \
                                            : dst_xmm; \
            /* vec_off(): the VEX.256 lane split re-runs this body over \
             * YMM[255:128]; every operand goes through it and there is no \
             * register-index helper arm, so the high pass is exact. */ \
            u32 off_d = vec_off(dst_xmm); \
            u32 off_1 = vec_off(src1_xmm); \
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) \
                return false; \
            if (is_register_operand(op1.type)) { \
                u8 src_xmm = op1.reg & 15; \
                u32 off_s = vec_off(src_xmm); \
                if (!fits_ldr_q_offset(off_s)) return false; \
                if (m_block_is_64bit || m_current_original_vex \
                    || m_vec_lane_split \
                    || m_xmm_v0_forward_in != src1_xmm) { \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                } \
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
                emit(NEON_ENC); \
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
                if (!m_block_is_64bit && !m_current_original_vex \
                    && !m_vec_lane_split) { \
                    m_xmm_v0_forward_out = dst_xmm; \
                } \
            } else { \
                /* Park EA in X21: emit_guest_load64 / x64 region load \
                 * clobbers X4 (and may BLR). Low qword then reuses X21. */ \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1); \
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP); \
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8)); \
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0)); \
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4); \
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D)); \
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
                emit(NEON_ENC); \
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            } \
            break; \
        }

    // PABSB/W/D — NEON ABS. x86 INT_MIN → INT_MIN (no saturation); NEON
    // ABS wraps identically.
    case Opcode::PABSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::abs_v(0, 1, arm64::VS_B))
    case Opcode::PABSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::abs_v(0, 1, arm64::VS_H))
    case Opcode::PABSD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::abs_v(0, 1, arm64::VS_S))

    // PADDSB/W — signed saturating add → NEON SQADD.
    case Opcode::PADDSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::sqadd_v(0, 0, 1, arm64::VS_B))
    case Opcode::PADDSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::sqadd_v(0, 0, 1, arm64::VS_H))

    // PSUBSB/W — signed saturating sub → NEON SQSUB.
    case Opcode::PSUBSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::sqsub_v(0, 0, 1, arm64::VS_B))
    case Opcode::PSUBSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::sqsub_v(0, 0, 1, arm64::VS_H))

    // PHADDW/PHADDD — horizontal pairwise add (SSSE3). NEON ADDP
    // produces exactly the x86 layout: low 64b = pairs of src0, high
    // 64b = pairs of src1.
    case Opcode::PHADDW_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::addp_v(0, 0, 1, arm64::VS_H))
    case Opcode::PHADDD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::addp_v(0, 0, 1, arm64::VS_S))

    // HADDPS — horizontal FP pairwise add → NEON FADDP.4S.
    case Opcode::HADDPS_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::faddp_v(0, 0, 1, arm64::FS_S))
    // HADDPD — same, double precision. 2D lanes: [A0+A1, B0+B1].
    case Opcode::HADDPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::faddp_v(0, 0, 1, arm64::FS_D))

    // MOVDDUP — dst[63:0] = src[63:0]; dst[127:64] = src[63:0].
    // Exactly NEON DUP Vd.2D, Vn.D[0].  Only the reg-reg form is
    // claimed here; MOVDDUP_XMM_M64 (memory) takes the delegate.
    case Opcode::MOVDDUP_XMM_XMMM64: {
        if (instr.prefixes.vex_l) return false;
        u8 dst_xmm = op0.reg & 15;
        // vec_off(): VMOVDDUP ymm duplicates SRC[63:0] into the low lane and
        // SRC[191:128] into the high lane (Intel SDM Vol. 2), i.e. this body
        // once per lane, the memory form reading the second qword at +16.
        u32 off_d = vec_off(dst_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_x_offset(off_d + 8)) return false;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = vec_off(src_xmm);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
            emit(arm64::dup_v_d0_to_2d(0, 1));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d >> 3));
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                  (off_d + 8) >> 3));
        }
        break;
    }

    // MOVSLDUP — dst lanes = [src[0], src[0], src[2], src[2]] (32-bit).
    // NEON TRN1.4S(src, src) produces exactly this pattern.
    case Opcode::MOVSLDUP_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::trn1_v(0, 1, 1, arm64::VS_S))

    // MOVSHDUP — dst lanes = [src[1], src[1], src[3], src[3]].
    // NEON TRN2.4S(src, src).
    case Opcode::MOVSHDUP_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(arm64::trn2_v(0, 1, 1, arm64::VS_S))

    // PMULHRSW — signed rounding multiply, high half. x86 does NOT saturate.
    //
    // SQRDMULH.8H agrees on every input except INT16_MIN x INT16_MIN, where
    // NEON saturates to 0x7FFF but x86 yields 0x8000. That corner is cheap to
    // correct exactly, because 0x7FFF is UNREACHABLE as a genuine PMULHRSW
    // result: the value is ((a*b >> 14) + 1) >> 1, so producing 0x7FFF needs
    // a*b >= 0x3FFFC000, and the largest product of two non-INT16_MIN operands
    // is 32767*32767 = 0x3FFF0001 (which yields 0x7FFE). So SQRDMULH emits
    // 0x7FFF if and only if it saturated -- exactly the case to rewrite.
    //
    // Build the 0x7FFF compare constant in-register (MOVI gives one repeated
    // byte, so 0xFFFF >> 1 per H lane), then flip the matching lanes with EOR:
    // 0x7FFF ^ 0xFFFF = 0x8000, and non-matching lanes XOR 0 are untouched.
    // Clobbers only V0/V1, which this stencil already owns.
    case Opcode::PMULHRSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR_REGONLY(({
            emit(arm64::sqrdmulh_v(0, 0, 1, arm64::VS_H));
            emit(arm64::movi_v16b(1, 0xFF));                  // 0xFFFF/lane
            emit(arm64::ushr_v_imm(1, 1, 1, arm64::VS_H));    // 0x7FFF/lane
            emit(arm64::cmeq_v(1, 0, 1, arm64::VS_H));        // saturated mask
            arm64::eor_v16b(0, 0, 1);                         // 0x7FFF -> 0x8000
        }))

    #undef EMIT_SSE_NEON_RR_REGONLY

    // ---- Tier-1 three-instruction stencils (UZP1+UZP2+op).
    // Reg-reg only; memory form delegates. Clobbers V0, V1, V2.
    #define EMIT_SSE_UZP_PAIR(PAIR_OP_ENC) \
        { \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            /* SRC1 from vvvv for a VEX form (only the lane split reaches \
             * here with vex set; the VEX.128 bridge merges and clears it), \
             * every operand through vec_off() for the high-lane pass. */ \
            const u8 src1_xmm = instr.prefixes.vex \
                ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm; \
            u32 off_d = vec_off(dst_xmm); \
            const u32 off_1 = vec_off(src1_xmm); \
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) \
                return false; \
            if (is_register_operand(op1.type)) { \
                u8 src_xmm = op1.reg & 15; \
                u32 off_s = vec_off(src_xmm); \
                if (!fits_ldr_q_offset(off_s)) return false; \
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
            } else { \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1); \
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP); \
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8)); \
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0)); \
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4); \
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D)); \
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
            } \
            PAIR_OP_ENC; \
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            break; \
        }

    // PHSUBW/PHSUBD — horizontal subtract pairs (result = even - odd).
    case Opcode::PHSUBW_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 1, arm64::VS_H));  // V2 = even lanes
            emit(arm64::uzp2_v(0, 0, 1, arm64::VS_H));  // V0 = odd lanes
            emit(arm64::sub_v(0, 2, 0, arm64::VS_H));   // V0 = even - odd
        }))
    case Opcode::PHSUBD_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 1, arm64::VS_S));
            emit(arm64::uzp2_v(0, 0, 1, arm64::VS_S));
            emit(arm64::sub_v(0, 2, 0, arm64::VS_S));
        }))

    // PHADDSW — saturating pairwise add on words.
    case Opcode::PHADDSW_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 1, arm64::VS_H));
            emit(arm64::uzp2_v(0, 0, 1, arm64::VS_H));
            emit(arm64::sqadd_v(0, 2, 0, arm64::VS_H));
        }))

    // PHSUBSW — saturating pairwise sub on words.
    case Opcode::PHSUBSW_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 1, arm64::VS_H));
            emit(arm64::uzp2_v(0, 0, 1, arm64::VS_H));
            emit(arm64::sqsub_v(0, 2, 0, arm64::VS_H));
        }))

    // HSUBPS/HSUBPD — horizontal FP subtract pairs.
    case Opcode::HSUBPS_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 1, arm64::VS_S));
            emit(arm64::uzp2_v(0, 0, 1, arm64::VS_S));
            emit(arm64::fsub_v(0, 2, 0, arm64::FS_S));
        }))
    case Opcode::HSUBPD_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 1, arm64::VS_D));
            emit(arm64::uzp2_v(0, 0, 1, arm64::VS_D));
            emit(arm64::fsub_v(0, 2, 0, arm64::FS_D));
        }))

    // PSIGNB/W/D — for each lane: if src<0 negate dst; if src=0 zero dst;
    // else dst unchanged.  5 NEON ops: cmlt0, cmeq0, neg, bsl, bic.
    case Opcode::PSIGNB_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::cmlt_v_zero(2, 1, arm64::VS_B));
            emit(arm64::cmeq_v_zero(3, 1, arm64::VS_B));
            emit(arm64::neg_v(4, 0, arm64::VS_B));
            emit(arm64::bsl_v16b(2, 4, 0));
            emit(arm64::bic_v16b(0, 2, 3));
        }))
    case Opcode::PSIGNW_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::cmlt_v_zero(2, 1, arm64::VS_H));
            emit(arm64::cmeq_v_zero(3, 1, arm64::VS_H));
            emit(arm64::neg_v(4, 0, arm64::VS_H));
            emit(arm64::bsl_v16b(2, 4, 0));
            emit(arm64::bic_v16b(0, 2, 3));
        }))
    case Opcode::PSIGND_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::cmlt_v_zero(2, 1, arm64::VS_S));
            emit(arm64::cmeq_v_zero(3, 1, arm64::VS_S));
            emit(arm64::neg_v(4, 0, arm64::VS_S));
            emit(arm64::bsl_v16b(2, 4, 0));
            emit(arm64::bic_v16b(0, 2, 3));
        }))

    // PMULDQ — signed 32x32→64 multiply on even-indexed S lanes.
    // UZP1.4S packs even lanes into low 64 of V2/V3 (.2S); SMULL.2D
    // produces 2 sign-extended 64-bit products.
    case Opcode::PMULDQ_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 0, arm64::VS_S));
            emit(arm64::uzp1_v(3, 1, 1, arm64::VS_S));
            emit(arm64::smull_v(0, 2, 3, arm64::VS_S));
        }))

    // PMULUDQ — unsigned 32x32→64 on even-indexed S lanes (Intel SDM Vol. 2).
    // Same UZP1 pack as PMULDQ; UMULL.2D yields the two 64-bit products.
    case Opcode::PMULUDQ_XMM_XMMM128:
        EMIT_SSE_UZP_PAIR(({
            emit(arm64::uzp1_v(2, 0, 0, arm64::VS_S));
            emit(arm64::uzp1_v(3, 1, 1, arm64::VS_S));
            emit(arm64::umull_v(0, 2, 3, arm64::VS_S));
        }))

    #undef EMIT_SSE_UZP_PAIR

    // ---- PACK family: dst low = narrow(dst), dst high = narrow(src).
    // Two-instruction stencils (SQXTN + SQXTN2 or SQXTUN variants).
    #define EMIT_SSE_PACK(NARROW_LO, NARROW_HI, SRC_SZ) \
        { \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            /* SRC1 from vvvv (VEX), vec_off() for the lane split: Intel SDM \
             * Vol. 2 VPACKSSWB ymm: DEST[191:128] from SRC1[255:128], \
             * DEST[255:192] from SRC2[255:128] -- lane-local. */ \
            const u8 src1_xmm = instr.prefixes.vex \
                ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm; \
            u32 off_d = vec_off(dst_xmm); \
            const u32 off_1 = vec_off(src1_xmm); \
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) \
                return false; \
            if (is_register_operand(op1.type)) { \
                u8 src_xmm = op1.reg & 15; \
                u32 off_s = vec_off(src_xmm); \
                if (!fits_ldr_q_offset(off_s)) return false; \
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
            } else { \
                /* X21 dual-load: same PE32/x64 pattern as PUNPCK/PCMPEQ. */ \
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
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D)); \
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
            } \
            emit(NARROW_LO(0, 0, SRC_SZ));   /* V0.low  = narrow(src1) */ \
            emit(NARROW_HI(0, 1, SRC_SZ));   /* V0.high = narrow(src) */ \
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            break; \
        }

    case Opcode::PACKSSWB_XMM_XMMM128:
        EMIT_SSE_PACK(arm64::sqxtn_v, arm64::sqxtn2_v, arm64::VS_H)
    case Opcode::PACKSSDW_XMM_XMMM128:
        EMIT_SSE_PACK(arm64::sqxtn_v, arm64::sqxtn2_v, arm64::VS_S)
    case Opcode::PACKUSWB_XMM_XMMM128:
        EMIT_SSE_PACK(arm64::sqxtun_v, arm64::sqxtun2_v, arm64::VS_H)
    case Opcode::PACKUSDW_XMM_XMMM128:
        EMIT_SSE_PACK(arm64::sqxtun_v, arm64::sqxtun2_v, arm64::VS_S)

    #undef EMIT_SSE_PACK

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
