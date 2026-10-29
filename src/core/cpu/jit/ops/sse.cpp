// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — SSE/SSE2 ops (MOVDQA, MOVUPS, MOVLHPS, MOVHLPS, MOVLPS,
//              CVTPS2PD, MOVD, MOVQ, PMOVMSKB, packed shifts, shuffles,
//              PUNPCK, PINSRW, PEXTRW, MOVSS, and all xmm,xmm/m128 integer ops)
//
// This file keeps compile_sse: the MIN/MAX pre-switch, the VEX.256 and
// VEX.128 bridge prologue, the AES kill switch, CRC32 and the hand-off to
// compile_avx_unique, then the chain over the legacy SSE opcode families
// and the shared tail. Each family is one file (ADR-0006 step 3):
//   ops/sse_data.cpp            128-bit and scalar data movement
//   ops/sse_conv_packed.cpp     packed XMM conversions
//   ops/sse_int.cpp             packed integer and bitwise NEON lowerings
//   ops/sse_fp_packed.cpp       packed FP arithmetic
//   ops/sse_neon_stencils.cpp   Tier-1 reg-reg NEON stencils
//   ops/sse_fp_reciprocal.cpp   RCP/RSQRT estimates
//   ops/sse_fp_scalar.cpp       scalar FP
//   ops/sse_crypto.cpp          AES-NI, SHA, PCLMULQDQ
//   ops/sse_shuffle.cpp         shuffles, unpacks and blends
//   ops/sse_shift.cpp           packed shifts
//   ops/sse_insert_extract.cpp  lane insert/extract
//   ops/sse_round.cpp           ROUNDPS/PD/SS/SD
//   ops/sse_cmp.cpp             FP compares and flag-writing compares
//   ops/sse_mmx_forms.cpp       SSE/SSE2 forms that name an MM register
//   ops/sse_state.cpp           MXCSR, FXSAVE/FXRSTOR, cache-control hints
//   ops/sse4_helper_ops.cpp     SSE4.1/4.2 forms lowered through a helper
//   ops/sse_conv_scalar.cpp     scalar conversions
// Shared pieces: ops/sse_vex256.cpp (the VEX.256 lane-split policy and
// driver), ops/sse_emit.cpp (vector emit primitives), ops/sse_minmax.cpp
// (MIN/MAX) and ops/sse_internal.hpp (the helper-call macros).

#include "../jit_helpers.hpp"
#include "core/math/host_fenv.hpp"
#include "core/cpu/decode/vex256_lane_imm.hpp"
#include "core/cpu/sse_float_semantics.hpp"
#include "../../interpreter/ops/exec_ops.hpp"
#include "sse_internal.hpp"

#include <cstdlib>
#include <limits>

#include <optional>
#include <bit>

namespace vitreo::cpu::jit {

namespace {
const bool kDisableAes = std::getenv("VITREO_JIT_NO_AES") != nullptr;
} // namespace

namespace {

/// The pure 128/256-bit register-or-memory moves, both directions.
///
/// "Full width" distinguishes them from every other VEX.256 form: the whole
/// 256 bits are copied, so the lowering is one wide move rather than a 128-bit
/// operation applied to each lane. Keeping them out of the lane-split driver
/// is not an optimisation -- the driver requires operands[0] to be Reg128 and
/// so cannot express the store direction at all.
bool is_vex256_full_width_move(Opcode op) {
    switch (op) {
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128:
    case Opcode::MOVDQA_XMM_XMMM128:
    case Opcode::MOVUPS_XMM_XMMM128:
    case Opcode::MOVUPD_XMM_XMMM128:
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::LDDQU_XMM_M128:
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVUPD_XMMM128_XMM:
    case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    case Opcode::MOVNTDQ_M128_XMM:
        return true;
    default:
        return false;
    }
}

/// True for the store-direction mnemonics (r/m is the destination).
bool is_full_width_move_store(Opcode op) {
    switch (op) {
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVUPD_XMMM128_XMM:
    case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    case Opcode::MOVNTDQ_M128_XMM:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool JitCompiler::compile_sse(const DecodedInstruction& instr,
                               u64 instr_rip, u64 next_rip,
                               bool is_last) {
    switch (instr.opcode) {
    case Opcode::MINPS_XMM_XMMM128: case Opcode::MAXPS_XMM_XMMM128:
    case Opcode::MINPD_XMM_XMMM128: case Opcode::MAXPD_XMM_XMMM128:
    case Opcode::MINSS_XMM_XMMM32: case Opcode::MAXSS_XMM_XMMM32:
    case Opcode::MINSD_XMM_XMMM64: case Opcode::MAXSD_XMM_XMMM64:
        return compile_sse_minmax(instr,instr_rip,next_rip,is_last);
    default: break;
    }
    // A VEX-encoded SSE instruction reuses the legacy SSE Opcode with
    // prefixes.vex set, but it is NOT the legacy instruction: VEX.NDS makes it
    // three-operand (dst := vvvv OP rm, leaving rm untouched), the 128-bit form
    // zeroes YMM[255:128] of the destination, and VEX.L doubles the work into
    // the high lane (Intel SDM Vol. 2, "VEX Encoding" / 2.3.5). Every lowering
    // below the AVX-unique cases is two-operand and 128-bit only, so claiming a
    // VEX form here silently computes `dst OP rm` into the wrong register and
    // leaves the upper lane stale. The interpreter's AVX/SSE bridge implements
    // the NDS merge, the lane split and the YMM zeroing, so delegate to it.
    // The AVX-unique opcodes (VFMADD*/VPERM*/VPBLEND*/VINSERT*/VPBROADCAST*)
    // are outside the legacy SSE opcode range and keep their lowerings.
    //
    // Because they are outside that range, the bridge below never sees them:
    // a VEX.256 form would otherwise fall straight into a 128-bit-only arm and
    // leave YMM[255:128] stale. Route the ones the lane-split allowlist covers
    // (the packed FMA3 family) into the driver here, before the bridge test.
    // ---- VEX.256 full-width moves -------------------------------------
    //
    // A 256-bit move is one wide copy, not a 128-bit operation applied to each
    // lane, so it does not belong in the lane-split driver -- and the driver
    // cannot express it anyway: compile_vex256_lane_split requires
    // operands[0] to be Reg128, which excludes every store form
    // (`vmovdqu [rax], ymm0`). Handling it here rather than there is what lets
    // one lowering serve both bitnesses; the x64 compiler used to carry a
    // second copy of this for 64-bit blocks only.
    //
    // Intel SDM Vol. 2 VMOVDQA/VMOVDQU/VMOVAPS/VMOVAPD/VMOVUPS/VMOVUPD:
    // DEST[255:0] := SRC[255:0]. The aligned mnemonics require 32-byte
    // alignment at 256-bit width, not 16.
    if (instr.prefixes.vex && instr.prefixes.vex_l && !m_vec_lane_split &&
        is_vex256_full_width_move(instr.opcode)) {
      const auto emit_full_width_move = [&]() -> bool {
        if (instr.operand_count < 2) return false;
        const auto& fw0 = instr.operands[0];
        const auto& fw1 = instr.operands[1];
        const bool store_form = is_full_width_move_store(instr.opcode);
        const u8 reg_xmm = (store_form ? fw1.reg : fw0.reg) & 15;
        const auto& other = store_form ? fw0 : fw1;
        const u32 off_r0 = xmm_u64_off(reg_xmm, 0);
        const u32 off_rh0 = ymm_high_u64_off(reg_xmm, 0);
        if (!fits_ldr_q_offset(off_r0) || !fits_ldr_q_offset(off_rh0)) {
            return false;
        }

        if (is_register_operand(other.type)) {
            const u8 oth_xmm = other.reg & 15;
            const u32 off_o0 = xmm_u64_off(oth_xmm, 0);
            const u32 off_oh0 = ymm_high_u64_off(oth_xmm, 0);
            if (!fits_ldr_q_offset(off_o0) || !fits_ldr_q_offset(off_oh0)) {
                return false;
            }
            const u32 s0 = store_form ? off_r0 : off_o0;
            const u32 sh = store_form ? off_rh0 : off_oh0;
            const u32 d0 = store_form ? off_o0 : off_r0;
            const u32 dh = store_form ? off_oh0 : off_rh0;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s0 >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, sh >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, d0 >> 4));
            emit(arm64::str_q_uoff(1, REG_CPU_STATE, dh >> 4));
            return true;
        }

        // A memory LOAD with no 32-byte guest-base access available is better
        // served by the lane-split driver below, which runs the 128-bit fast
        // path once per lane, than by the four guarded 8-byte accesses this
        // path would fall back to. Decline and let it route. (Stores have no
        // such alternative -- the driver cannot express them -- so they take
        // the fallback and are still strictly better than delegating.)
        // linear64 is a memory-wide setting and a 32-bit linear block pins
        // the base too, but only a long-mode block holds the linear64 check
        // map in X15 (a 32-bit block keeps its ESP frame there).
        const bool have_wide_access = m_block_is_64bit && m_cfg.linear64 &&
                                      m_cfg.linear64_vec128 && m_linear_base_pinned;
        if (!store_form && !have_wide_access) return false;

        if (vex256_move_requires_align32(instr.opcode)) {
            emit_compute_ea(REG_SCRATCH2, other.mem, next_rip);
            emit_align_check(REG_SCRATCH2, /*align32=*/true);
        }
        // One guarded 32-byte guest access instead of four guarded 8-byte
        // ones, and it matches the architecture better: x86 does not update
        // the destination when a later part of the access faults, and the
        // split form would have written the low half first.
        if (have_wide_access) {
            emit_compute_ea(REG_SCRATCH2, other.mem, next_rip);
            if (store_form) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_r0 >> 4));
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_rh0 >> 4));
                emit_linear64_store256(0, 1, REG_SCRATCH2, off_r0, off_rh0);
            } else {
                emit_linear64_load256(0, 1, REG_SCRATCH2, off_r0, off_rh0);
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_r0 >> 4));
                emit(arm64::str_q_uoff(1, REG_CPU_STATE, off_rh0 >> 4));
            }
            return true;
        }
        // The store, with no 32-byte guest-base access: one validated 32-byte
        // write (emit_vec_store_span). Four 8-byte stores performed the
        // pieces before a denied one; Windows leaves every byte of a
        // faulting store untouched (tools/oracle_probe/
        // simd_store_fault_probe.expected).
        const bool hold = should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        emit_compute_ea(REG_SCRATCH2, other.mem, next_rip);
        emit_vec_store_span(REG_SCRATCH2, off_r0, off_rh0, 32, instr_rip);
        if (hold) emit_nzcv_fill();
        return true;
      };
      if (!emit_full_width_move()) {
        // A load with no 32-byte wide access (every 32-bit block; a 64-bit
        // one without linear64): the lane-split driver runs the 128-bit
        // load once per lane. This used to `return false` here, so the
        // "let it route" above never happened and every `vmovdqa ymm,
        // [mem]` in a 32-bit guest was interpreted. The lambda has emitted
        // nothing on this path.
        if (!is_full_width_move_store(instr.opcode) &&
            vex256_lane_split_allowed(instr.opcode)) {
          return compile_vex256_lane_split(instr, instr_rip, next_rip,
                                           is_last);
        }
        return false;
      }
      finish_vector_block(next_rip, is_last);
      return true;
    }

    if (instr.prefixes.vex && instr.prefixes.vex_l &&
        instr.opcode == Opcode::PMADDWD_XMM_XMMM128 &&
        instr.operand_count >= 2 &&
        is_register_operand(instr.operands[0].type) &&
        is_memory_operand(instr.operands[1].type)) {
        const u8 dst = instr.operands[0].reg & 15;
        const u8 src = instr.prefixes.vex_vvvv & 15;
        const u32 scratch = jit_vec_scratch_off();
        const u32 dst_off[] = {xmm_u64_off(dst, 0), ymm_high_u64_off(dst, 0)};
        const u32 src_off[] = {xmm_u64_off(src, 0), ymm_high_u64_off(src, 0)};
        for (u8 half = 0; half < 2; ++half) {
            if (!fits_ldr_q_offset(dst_off[half]) ||
                !fits_ldr_q_offset(src_off[half]) ||
                !fits_ldr_q_offset(scratch + half * 16u)) return false;
        }
        // Stage the entire m256 operand before touching either destination
        // lane. A later read fault must leave the architectural YMM intact.
        // The final scratch qword holds the base until the last load, then
        // becomes data. Compute the effective address only once so address-
        // size wrapping applies to the start, not to each individual piece.
        emit_compute_ea(REG_SCRATCH2, instr.operands[1].mem, next_rip);
        emit(arm64::str_x_uoff(REG_SCRATCH2, REG_CPU_STATE, (scratch + 24) >> 3));
        for (u32 piece = 0; piece < 4; ++piece) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH2, REG_CPU_STATE, (scratch + 24) >> 3));
            if (piece != 0) emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, piece * 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, (scratch + piece * 8) >> 3));
        }
        for (u8 half = 0; half < 2; ++half) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off[half] >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, (scratch + half * 16u) >> 4));
            emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
            emit(arm64::smull2_v(3, 0, 1, arm64::VS_H));
            emit(arm64::addp_v(0, 2, 3, arm64::VS_S));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off[half] >> 4));
        }
        finish_vector_block(next_rip, is_last);
        return true;
    }

    if (instr.prefixes.vex && instr.prefixes.vex_l &&
        instr.opcode == Opcode::PSHUFB_XMM_XMMM128 &&
        instr.operand_count >= 2 &&
        is_register_operand(instr.operands[0].type) &&
        is_register_operand(instr.operands[1].type)) {
        // Intel SDM Vol. 2, VPSHUFB (VEX.256): each 128-bit half shuffles
        // independently; bit 7 zeros a byte and bits 3:0 select its source.
        const u8 dst = instr.operands[0].reg & 15;
        const u8 data = instr.prefixes.vex_vvvv & 15;
        const u8 mask = instr.operands[1].reg & 15;
        const u32 dst_off[] = {xmm_u64_off(dst, 0), ymm_high_u64_off(dst, 0)};
        const u32 data_off[] = {xmm_u64_off(data, 0), ymm_high_u64_off(data, 0)};
        const u32 mask_off[] = {xmm_u64_off(mask, 0), ymm_high_u64_off(mask, 0)};
        for (u8 half = 0; half < 2; ++half) {
            if (!fits_ldr_q_offset(dst_off[half]) ||
                !fits_ldr_q_offset(data_off[half]) ||
                !fits_ldr_q_offset(mask_off[half])) return false;
        }
        emit(arm64::movi_v16b(3, 0x8F));
        for (u8 half = 0; half < 2; ++half) {
            // Read both sources before writing, including when dst aliases mask.
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, data_off[half] >> 4));
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, mask_off[half] >> 4));
            emit(arm64::and_v16b(2, 2, 3));
            emit(arm64::tbl_v16b_1(0, 1, 2));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off[half] >> 4));
        }
        finish_vector_block(next_rip, is_last);
        return true;
    }

    if (instr.prefixes.vex && instr.prefixes.vex_l && !m_vec_lane_split &&
        !ops::opcode_is_vex_sse_family(instr.opcode) &&
        vex256_lane_split_allowed(instr.opcode)) {
        return compile_vex256_lane_split(instr, instr_rip, next_rip, is_last);
    }

    // VEX.256 VCVTPD2PS narrows ymm/m256 into xmm: not a lane-split shape and
    // not a bridge shape (the bridge would re-run it as two 128-bit passes).
    // Its own arm handles it with vex/vex_l set.
    const bool vex256_direct_arm =
        instr.prefixes.vex && instr.prefixes.vex_l &&
        instr.opcode == Opcode::CVTPD2PS_XMM_XMMM128;
    const bool vex_direct = !m_vec_lane_split && sse_vex_lowered_directly(instr);
    if (instr.prefixes.vex && ops::opcode_is_vex_sse_family(instr.opcode) &&
        !m_vec_lane_split && !vex256_direct_arm && !vex_direct) {
        // 256-bit runs the 128-bit lowering once per lane for the allowlisted
        // element-wise forms; everything else stays bridge-only. The NDD
        // shifts put the destination in vvvv, and VMOVSS/VMOVSD merge after
        // the move: both are bridge-only shapes.
        if (instr.prefixes.vex_l &&
            !ops::is_vex_ndd_shift_imm(instr.opcode)) {
            return compile_vex256_lane_split(instr, instr_rip, next_rip,
                                             is_last);
        }
        // VEX shift-by-imm8 is NDD: ModRM.reg is the opcode extension, so the
        // SOURCE is the r/m register and VEX.vvvv is the DESTINATION (Intel
        // SDM Vol. 2, VPSLLW/VPSRLW/... 71-73 /x ib). The legacy lowering
        // shifts its one register in place, which would clobber the source and
        // never write the destination -- so copy source -> destination first,
        // then retarget operands[0] at the destination and let the ordinary
        // path (or the 256-bit lane split) run it.
        //
        // This used to `return false`, so EVERY VEX shift-by-imm went to the
        // interpreter: PSRLW/PSLLD/PSRLD/PSRAD/PSLLW together were 23.3% of
        // all delegated instructions in a browser renderer.
        if (ops::is_vex_ndd_shift_imm(instr.opcode)) {
            if (instr.operand_count < 1 ||
                instr.operands[0].type != OperandType::Reg128) {
                return false;
            }
            const u8 shift_src = instr.operands[0].reg & 15;
            const u8 shift_dst = instr.prefixes.vex_vvvv & 15;
            const u32 src_lo = xmm_u64_off(shift_src, 0);
            const u32 dst_lo = xmm_u64_off(shift_dst, 0);
            const u32 src_hi = ymm_high_u64_off(shift_src, 0);
            const u32 dst_hi = ymm_high_u64_off(shift_dst, 0);
            if (!fits_ldr_q_offset(src_lo) || !fits_ldr_q_offset(dst_lo) ||
                !fits_ldr_q_offset(src_hi) || !fits_ldr_q_offset(dst_hi)) {
                return false;
            }
            const usize shift_mark = m_code_buf.size();
            if (shift_src != shift_dst) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_lo >> 4));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_lo >> 4));
            }
            if (instr.prefixes.vex_l) {
                if (shift_src != shift_dst) {
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_hi >> 4));
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_hi >> 4));
                }
            } else {
                // 128-bit VEX zeroes YMM[255:128] of the destination.
                emit(arm64::eor_v16b(5, 5, 5));
                emit(arm64::str_q_uoff(5, REG_CPU_STATE, dst_hi >> 4));
            }
            DecodedInstruction shifted = instr;
            shifted.operands[0].reg = shift_dst;
            shifted.prefixes.vex = false;
            bool shift_ok = false;
            if (instr.prefixes.vex_l) {
                shift_ok = compile_vex256_lane_split(shifted, instr_rip,
                                                     next_rip, is_last);
            } else {
                shifted.prefixes.vex = false;
                shifted.prefixes.vex_l = false;
                shift_ok = compile_sse(shifted, instr_rip, next_rip, is_last);
            }
            if (!shift_ok) {
                m_code_buf.resize(shift_mark);
                return false;
            }
            return true;
        }
        // VROUNDSS/VROUNDSD: DEST[127:32|64] := SRC1 (vvvv), not a merge
        // copy into DEST -- which is also why the generic bridge below could
        // not take the `vroundsd xmm0, xmm1, xmm0` shape (dst == r/m) and
        // sent it to the interpreter. The lowering reads SRC1 itself.
        if (instr.opcode == Opcode::ROUNDSS_XMM_XMMM32_IMM8 ||
            instr.opcode == Opcode::ROUNDSD_XMM_XMMM64_IMM8) {
            if (instr.prefixes.vex_l) return false;
            const usize round_mark = m_code_buf.size();
            if (!emit_round_scalar(instr, next_rip,
                                   instr.prefixes.vex_vvvv & 15,
                                   /*zero_upper=*/true)) {
                m_code_buf.resize(round_mark);
                return false;
            }
            finish_vector_block(next_rip, is_last);
            return true;
        }
        if (instr.opcode == Opcode::MOVSS_XMM_XMMM32 ||
            instr.opcode == Opcode::MOVSD_XMM_XMMM64) {
            if (instr.prefixes.vex_l || instr.operand_count < 2 ||
                instr.operands[0].type != OperandType::Reg128) return false;
            const auto& source = instr.operands[1];
            const bool reg_source = source.type == OperandType::Reg128;
            if (!reg_source && (!is_memory_operand(source.type) ||
                                instr.prefixes.vex_vvvv != 0)) return false;
            const bool scalar_double =
                instr.opcode == Opcode::MOVSD_XMM_XMMM64;
            const u8 dst = instr.operands[0].reg & 15;
            const u32 dst_lo = xmm_u64_off(dst, 0);
            const u32 dst_hi = xmm_u64_off(dst, 1);
            const u32 upper = ymm_high_u64_off(dst, 0);
            if (!fits_ldr_x_offset(dst_lo) || !fits_ldr_x_offset(dst_hi) ||
                !fits_ldr_x_offset(upper + 8)) return false;

            // Intel SDM Vol. 2B, MOVSD/MOVSS: register forms merge the
            // scalar from r/m with bits 127:scalar_width from VEX.vvvv.
            // Memory loads instead zero everything above the scalar. Both
            // zero YMM[255:128]. Read all aliased sources before any store.
            if (reg_source) {
                const u32 src = xmm_u64_off(source.reg & 15, 0);
                const u32 merge = xmm_u64_off(instr.prefixes.vex_vvvv & 15, 0);
                if (!fits_ldr_x_offset(src) ||
                    !fits_ldr_x_offset(merge + 8)) return false;
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src >> 3));
                if (!scalar_double) {
                    emit(arm64::ldr_x_uoff(REG_SCRATCH2, REG_CPU_STATE, merge >> 3));
                    emit(arm64::bfi_x(REG_SCRATCH2, REG_SCRATCH0, 0, 32));
                    emit(arm64::mov_x(REG_SCRATCH0, REG_SCRATCH2));
                }
                emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, (merge + 8) >> 3));
            } else {
                emit_compute_ea(REG_SCRATCH4, source.mem, next_rip);
                if (scalar_double) emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                else emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::mov_x(REG_SCRATCH1, 31));
            }
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, dst_lo >> 3));
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, dst_hi >> 3));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, upper >> 3));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, (upper + 8) >> 3));
            finish_vector_block(next_rip, is_last);
            return true;
        }

        // COMISS/UCOMISS/PTEST name a SOURCE in operand 0 and write only
        // EFLAGS, so there is no destination whose upper half to zero.
        const bool xmm_dst = instr.operand_count > 0 &&
                             instr.operands[0].type == OperandType::Reg128 &&
                             !sse_writes_only_flags(instr.opcode);
        const u8 vvvv = instr.prefixes.vex_vvvv & 15;
        u8  dst = 0;
        u32 dhi_off = 0;
        if (xmm_dst) {
            dst = instr.operands[0].reg & 15;
            dhi_off = ymm_high_u64_off(dst, 0);
            if (!fits_ldr_q_offset(dhi_off)) return false;
        }

        const bool merge =
            xmm_dst && ops::vex_sse_merges_vvvv(instr) && dst != vvvv;
        if (merge) {
            // A register source that is also the destination would be
            // destroyed by the merge copy (`vsubps xmm0, xmm1, xmm0`).
            // Lowerings on the lane-split allowlist read SRC1 from vvvv
            // themselves and read both sources before their store, so for
            // those run exactly the lane split's LOW pass instead: no merge
            // copy, prefixes.vex kept so SRC1 comes from vvvv, and the
            // 128-bit VEX upper-half zero emitted first (YMM[255:128] of dst
            // is not a source of a 128-bit operation, so zeroing it before is
            // equivalent and keeps it ahead of a last-instruction epilogue).
            // Anything else keeps the bridge.
            if (instr.operand_count > 1 &&
                instr.operands[1].type == OperandType::Reg128 &&
                (instr.operands[1].reg & 15) == dst) {
                if (!vex256_lane_split_allowed(instr.opcode) ||
                    !fits_ldr_q_offset(dhi_off)) {
                    return false;
                }
                const usize alias_mark = m_code_buf.size();
                emit(arm64::eor_v16b(5, 5, 5));
                emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
                DecodedInstruction low = instr;
                low.prefixes.vex_l = false;
                m_vec_lane_split = true;
                m_vec_hi_lane = false;
                const bool ok = compile_sse(low, instr_rip, next_rip, is_last);
                m_vec_lane_split = false;
                if (!ok) {
                    m_code_buf.resize(alias_mark);
                    return false;
                }
                return true;
            }
            if (!fits_ldr_q_offset(xmm_u64_off(dst, 0)) ||
                !fits_ldr_q_offset(xmm_u64_off(vvvv, 0))) {
                return false;
            }
        }

        DecodedInstruction legacy = instr;
        legacy.prefixes.vex = false;
        legacy.prefixes.vex_l = false;

        // Both prologue steps run BEFORE the legacy lowering: on the block's
        // last instruction that lowering emits the block exit, so anything
        // appended after it is unreachable. Neither step can disturb it — the
        // legacy SSE lowerings read and write only the XMM half.
        const usize mark = m_code_buf.size();
        if (merge) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, xmm_u64_off(vvvv, 0) >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, xmm_u64_off(dst, 0) >> 4));
        }
        if (xmm_dst) {
            // Intel SDM Vol. 2: a 128-bit VEX form zeroes YMM[255:128].
            emit(arm64::eor_v16b(5, 5, 5));
            emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
        }
        if (!compile_sse(legacy, instr_rip, next_rip, is_last)) {
            m_code_buf.resize(mark);
            return false;
        }
        return true;
    }

    const bool disable_aes = kDisableAes;
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];


    // Diagnostic: force-bail on AES opcodes so they run through the
    // interpreter, isolating any JIT-specific AES bug.
    if (disable_aes) {
        switch (instr.opcode) {
            case Opcode::AESENC_XMM_XMMM128:
            case Opcode::AESENCLAST_XMM_XMMM128:
            case Opcode::AESDEC_XMM_XMMM128:
            case Opcode::AESDECLAST_XMM_XMMM128:
            case Opcode::AESIMC_XMM_XMMM128:
            case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8:
                return false;
            default: break;
        }
    }

    // SSE4.2 CRC32 uses the Castagnoli polynomial, exactly matching ARMv8's
    // CRC32C family. Load a memory source before the accumulator because a
    // slow guest-memory helper may clobber caller-saved ARM registers.
    switch (instr.opcode) {
    case Opcode::CRC32_R32_RM8:
    case Opcode::CRC32_R64_RM8:
    case Opcode::CRC32_R32_RM16:
    case Opcode::CRC32_R32_RM32:
    case Opcode::CRC32_R64_RM64: {
        const bool src8 = instr.opcode == Opcode::CRC32_R32_RM8
                       || instr.opcode == Opcode::CRC32_R64_RM8;
        if (is_register_operand(op1.type)) {
            if (src8) {
                emit_load_gpr8(REG_SCRATCH1, op1.reg,
                               instr.prefixes.has_rex());
            } else if (instr.opcode == Opcode::CRC32_R32_RM16) {
                emit_load_gpr16(REG_SCRATCH1, op1.reg);
            } else if (instr.opcode == Opcode::CRC32_R64_RM64) {
                emit_load_gpr64(REG_SCRATCH1, op1.reg);
            } else {
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
            }
        } else {
            if (!m_block_is_64bit) emit_update_rip(instr_rip);
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            if (src8) {
                emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
            } else if (instr.opcode == Opcode::CRC32_R32_RM16) {
                emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
            } else if (instr.opcode == Opcode::CRC32_R64_RM64) {
                emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
            } else {
                emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
            }
        }

        emit_load_gpr32(REG_SCRATCH0, op0.reg);
        if (src8) {
            emit(arm64::crc32cb_w(REG_SCRATCH0, REG_SCRATCH0,
                                  REG_SCRATCH1));
        } else if (instr.opcode == Opcode::CRC32_R32_RM16) {
            emit(arm64::crc32ch_w(REG_SCRATCH0, REG_SCRATCH0,
                                  REG_SCRATCH1));
        } else if (instr.opcode == Opcode::CRC32_R64_RM64) {
            emit(arm64::crc32cx_w(REG_SCRATCH0, REG_SCRATCH0,
                                  REG_SCRATCH1));
        } else {
            emit(arm64::crc32cw_w(REG_SCRATCH0, REG_SCRATCH0,
                                  REG_SCRATCH1));
        }
        // r32 writes and CRC32 r64 both zero-extend the 32-bit CRC.
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }
    default:
        break;
    }

    if (instr.opcode == Opcode::CRC32_R32_RM8
        || instr.opcode == Opcode::CRC32_R64_RM8
        || instr.opcode == Opcode::CRC32_R32_RM16
        || instr.opcode == Opcode::CRC32_R32_RM32
        || instr.opcode == Opcode::CRC32_R64_RM64) {
        if (is_last) {
            u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }



    // AVX/AVX2/FMA3/F16C opcodes live outside the legacy SSE range and are
    // lowered separately; nullopt means "not one of those".
    if (auto avx = compile_avx_unique(instr, instr_rip, next_rip, is_last);
        avx.has_value()) {
        return *avx;
    }

    // The legacy SSE opcode space, one family per ops/sse*.cpp (ADR-0006
    // step 3). A family returns nullopt for an opcode outside it, false to
    // decline, and true when its arm reached break; the shared tail below
    // then runs exactly as after the old single switch. Case labels are
    // disjoint across families, so chain order is not observable.
    std::optional<bool> lowered = compile_sse_data(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_conv_packed(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_int(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_fp_packed(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_neon_stencils(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_fp_reciprocal(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_fp_scalar(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_crypto(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_shuffle(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_shift(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_insert_extract(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_round(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_cmp(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_mmx_forms(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_state(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse4_helper_ops(instr, instr_rip, next_rip);
    if (!lowered) lowered = compile_sse_conv_scalar(instr, instr_rip, next_rip);
    if (!lowered.has_value() || !*lowered) return false;

    // An instruction that touches an MM register enters MMX state: the x87 tag
    // word becomes all-Valid and TOP becomes 0 (Intel SDM Vol. 1 §11.6.1 and
    // FpuState::mmx_enter). That applies to instructions which only READ an MM
    // register, not just those that write one.
    //
    // compile_mmx has always done this at its own exit, but the MMX-touching
    // lowerings that live in this file — the CVT*2PI / CVTPI2* family and
    // MOVNTQ — never did, so they left TOP at whatever the x87 stack had and
    // diverged from the interpreter on every one of those forms. Found by the
    // ISA matrix sweep; it is invisible until a guest mixes MMX with x87 or
    // reads the state back through FNSTENV/FXSAVE.
    //
    // Idempotent, so the helper-lowered forms (whose helper already performs
    // the same transition) are unaffected.
    for (u8 i = 0; i < instr.operand_count; ++i) {
        if (instr.operands[i].type == OperandType::RegMM) {
            emit_mmx_enter();
            break;
        }
    }

    finish_vector_block(next_rip, is_last);
    return true;
}


} // namespace vitreo::cpu::jit
