// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the VEX.256 lane-split policy and driver: which VEX.256
// forms run their 128-bit lowering once per lane, and the driver that does
// it (compile_vex256_lane_split).
//
// Split out of ops/sse.cpp (ADR-0006 step 3) as pure code motion.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
/// VEX.256 forms that are strictly element-wise across the 128-bit lane
/// boundary AND whose 128-bit lowering reads SRC1 from vvvv (so no merge copy
/// is needed) and has no register-index-taking cold helper arm. For these,
/// running the existing 128-bit lowering twice — once over xmm[], once over
/// ymm_high[] — is exactly the architectural semantics.
///
/// NOT element-wise, deliberately absent: PSLLD/PSRLD/PSRLW (the 256-bit form
/// keeps a single 128-bit count operand, Intel SDM Vol. 2 PSLLD), PMOVMSKB
/// (one 32-bit mask, not two), VPERM*/gathers (lane-crossing).
///
/// Class B — ADDPS/SUBPS/MULPS/DIVPS (EMIT_SSE_NEON_RR_FINITE_FP) and CMPPS —
/// is here too. Both read SRC1 from vvvv now (they used to read it from the
/// destination and rely on the generic bridge's merge copy, which the lane
/// split deliberately suppresses). The FINITE_FP macro additionally has a
/// RUNTIME Inf/NaN/zero-divisor cold arm that calls jit_sse_op with register
/// indices, and those always mean xmm[]; on the high lane that arm physically
/// swaps xmm[]<->ymm_high[] around the helper call (see the macro).
/// Same-build A/B and bisect knob: with VITREO_JIT_NO_VEX256_TRANCHE set the
/// 2026-10-01 tranche delegates to the interpreter exactly as before.
[[nodiscard]] bool vex256_tranche_disabled() noexcept {
    static const bool disabled =
        std::getenv("VITREO_JIT_NO_VEX256_TRANCHE") != nullptr;
    return disabled;
}
} // namespace

[[nodiscard]] bool vex256_lane_split_allowed(Opcode op) noexcept {
    switch (op) {
    case Opcode::PXOR_XMM_XMMM128:
    case Opcode::PAND_XMM_XMMM128:
    case Opcode::POR_XMM_XMMM128:
    case Opcode::ANDPS_XMM_XMMM128:
    case Opcode::PADDD_XMM_XMMM128:
    case Opcode::PSUBD_XMM_XMMM128:
    // Intel SDM Vol. 2B: word interleaves stay within each 128-bit lane;
    // multiply-add combines adjacent signed word products into dwords.
    case Opcode::PUNPCKLWD_XMM_XMMM128:
    case Opcode::PUNPCKHWD_XMM_XMMM128:
    case Opcode::PMADDWD_XMM_XMMM128:
    // Intel SDM Vol. 2A CVTDQ2PS/CVTPS2DQ/CVTTPS2DQ: eight
    // independent 32-bit conversions. Their lowerings use vec_off and no
    // register-index helper, so each half can reuse the 128-bit lowering.
    case Opcode::CVTDQ2PS_XMM_XMMM128:
    case Opcode::CVTPS2DQ_XMM_XMMM128:
    case Opcode::CVTTPS2DQ_XMM_XMMM128:
    // The rest of the EMIT_SSE_NEON_RR family. Each is element-wise within a
    // 128-bit lane in its VEX.256 form (Intel SDM Vol. 2: "VEX.256 encoded
    // version: DEST[255:128] <- SRC1[255:128] OP SRC2[255:128]"), and each is
    // lowered by the same macro as PADDD/PSUBD above -- which reads SRC1 from
    // vvvv, addresses every operand through vec_off(), and declines its
    // register-index helper arm on the high lane. The lane split is therefore
    // correct for them by exactly the argument that already justifies PADDD.
    //
    // Why they are worth listing: a VEX.256 op NOT on this list delegates to
    // the interpreter, one instruction at a time. Measured on a browser
    // renderer with VITREO_JIT_FALLBACK_HIST -- the process on the startup
    // critical path -- 40M instructions were delegated, and this family was a
    // large share (PADDW 4.6% alone, then PSUBW, PADDUSB, PMULLW, PCMPEQW and
    // the min/max ops).
    //
    // The unary members of the macro family (SQRTPS/SQRTPD) are deliberately
    // absent: their VEX form has no SRC1, VEX.vvvv must be 1111b, and the
    // macro would read xmm15 as SRC1.
    case Opcode::PADDB_XMM_XMMM128:
    case Opcode::PADDW_XMM_XMMM128:
    case Opcode::PADDQ_XMM_XMMM128:
    case Opcode::PADDUSB_XMM_XMMM128:
    case Opcode::PADDUSW_XMM_XMMM128:
    case Opcode::PSUBB_XMM_XMMM128:
    case Opcode::PSUBW_XMM_XMMM128:
    case Opcode::PSUBQ_XMM_XMMM128:
    case Opcode::PSUBUSB_XMM_XMMM128:
    case Opcode::PSUBUSW_XMM_XMMM128:
    case Opcode::PANDN_XMM_XMMM128:
    case Opcode::ANDPD_XMM_XMMM128:
    case Opcode::ANDNPS_XMM_XMMM128:
    case Opcode::ANDNPD_XMM_XMMM128:
    case Opcode::ORPS_XMM_XMMM128:
    case Opcode::ORPD_XMM_XMMM128:
    case Opcode::XORPS_XMM_XMMM128:
    case Opcode::XORPD_XMM_XMMM128:
    case Opcode::PAVGB_XMM_XMMM128:
    case Opcode::PAVGW_XMM_XMMM128:
    case Opcode::PCMPEQB_XMM_XMMM128:
    case Opcode::PCMPEQW_XMM_XMMM128:
    case Opcode::PCMPEQD_XMM_XMMM128:
    case Opcode::PCMPEQQ_XMM_XMMM128:
    case Opcode::PCMPGTB_XMM_XMMM128:
    case Opcode::PCMPGTW_XMM_XMMM128:
    case Opcode::PCMPGTD_XMM_XMMM128:
    case Opcode::PCMPGTQ_XMM_XMMM128:
    case Opcode::PMAXSB_XMM_XMMM128:
    case Opcode::PMAXSD_XMM_XMMM128:
    case Opcode::PMAXSW_XMM_XMMM128:
    case Opcode::PMAXUB_XMM_XMMM128:
    case Opcode::PMAXUD_XMM_XMMM128:
    case Opcode::PMAXUW_XMM_XMMM128:
    case Opcode::PMINSB_XMM_XMMM128:
    case Opcode::PMINSD_XMM_XMMM128:
    case Opcode::PMINSW_XMM_XMMM128:
    case Opcode::PMINUB_XMM_XMMM128:
    case Opcode::PMINUD_XMM_XMMM128:
    case Opcode::PMINUW_XMM_XMMM128:
    case Opcode::PMULLD_XMM_XMMM128:
    case Opcode::PMULLW_XMM_XMMM128:
    // Shift-by-imm8. VEX makes these NDD (vvvv is the DESTINATION, ModRM.rm
    // the source), so the bridge below retargets operands[0] to vvvv and
    // copies the source across BEFORE handing the instruction here -- by which
    // point it is an ordinary in-place shift of one register, and the 256-bit
    // form shifts each lane by the same imm8 (Intel SDM Vol. 2 VPSLLW/VPSRLW:
    // "VEX.256 ... DEST[255:128] := SRC1[255:128] << COUNT"). The lowering now
    // addresses through vec_off() and declines its register-index helper arm
    // on the high lane.
    case Opcode::PSLLW_XMM_IMM8:
    case Opcode::PSLLD_XMM_IMM8:
    case Opcode::PSLLQ_XMM_IMM8:
    case Opcode::PSRLW_XMM_IMM8:
    case Opcode::PSRLD_XMM_IMM8:
    case Opcode::PSRLQ_XMM_IMM8:
    case Opcode::PSRAW_XMM_IMM8:
    case Opcode::PSRAD_XMM_IMM8:
    case Opcode::PSLLDQ_XMM_IMM8:
    case Opcode::PSRLDQ_XMM_IMM8:
    case Opcode::MINPS_XMM_XMMM128:
    case Opcode::MAXPS_XMM_XMMM128:
    case Opcode::ADDPS_XMM_XMMM128:
    case Opcode::SUBPS_XMM_XMMM128:
    case Opcode::MULPS_XMM_XMMM128:
    case Opcode::DIVPS_XMM_XMMM128:
    case Opcode::CMPPS_XMM_XMMM128_IMM8:
    // Packed arithmetic and comparisons remain lane-local. Packed MIN/MAX
    // are claimed earlier by compile_sse_minmax: exception collection and
    // destination commitment must cover the entire 256-bit instruction.
    case Opcode::ADDPD_XMM_XMMM128:
    case Opcode::SUBPD_XMM_XMMM128:
    case Opcode::MULPD_XMM_XMMM128:
    case Opcode::DIVPD_XMM_XMMM128:
    case Opcode::MINPD_XMM_XMMM128:
    case Opcode::MAXPD_XMM_XMMM128:
    case Opcode::CMPPD_XMM_XMMM128_IMM8:
    // Lane-local shuffles and the alternating add/sub. SHUFPS repeats imm8 in
    // both lanes; VSHUFPD's high lane takes imm8[3:2], which the driver
    // applies through vex256_high_lane_imm. Both lowerings read SRC1 from
    // vvvv, address through vec_off() and decline their register-index helper
    // arm under the split. ADDSUBPS/PD has no helper arm, and each 128-bit
    // lane begins on an even element, so the per-lane alternation is the
    // 256-bit one (Intel SDM Vol. 2 VADDSUBPS). On a 64-bit title menu,
    // SHUFPS is 21.3%, ADDSUBPS 8.4%, and SHUFPD 3.7% of delegated
    // instructions.
    case Opcode::SHUFPS_XMM_XMMM128_IMM8:
    case Opcode::SHUFPD_XMM_XMMM128_IMM8:
    case Opcode::ADDSUBPS_XMM_XMMM128:
    case Opcode::ADDSUBPD_XMM_XMMM128:
    // Unary, element-wise (EMIT_SSE_UNARY_PS): no SRC1 at all, operands via
    // vec_off(), no helper arm. On a 64-bit title menu, RCPPS is 0.8% of delegations.
    case Opcode::RCPPS_XMM_XMMM128:
    case Opcode::RSQRTPS_XMM_XMMM128:
    // Native FRINT* per element, same imm8 in both lanes (vex256_lane_imm.hpp
    // lists ROUNDPS/PD as per-lane patterns). On a 64-bit title menu, ROUNDPS is 0.8%.
    case Opcode::ROUNDPS_XMM_XMMM128_IMM8:
    case Opcode::ROUNDPD_XMM_XMMM128_IMM8:
    // Pure 128-bit moves, LOAD direction only (register destination):
    // compile_vex256_lane_split requires operands[0] to be Reg128, so a store
    // form (`vmovdqu [rax], ymm0`) is declined by the driver and keeps the
    // delegate. In a 64-bit block both directions are claimed earlier by
    // compile_64bit's dedicated 256-bit arms, so this matters only for 32-bit
    // blocks -- where VEX.256 is legal and a TLS library emits it.
    case Opcode::MOVUPS_XMM_XMMM128:
    case Opcode::MOVUPD_XMM_XMMM128:
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128:
    case Opcode::MOVDQA_XMM_XMMM128:
        return true;

    // The lane-wise integer/FP tranche added 2026-10-01. Each is "VEX.256:
    // DEST[255:128] := op(SRC1[255:128], SRC2[255:128])" in its Intel SDM
    // Vol. 2 operation section, and each lowering was audited to read SRC1
    // from vvvv, address every operand through vec_off(), read both sources
    // before its store, and decline its register-index helper arm under the
    // split. Measured on a browser renderer (VITREO_JIT_FALLBACK_HIST):
    // PUNPCK*/PACK*/PSHUFB ~12%, PMADDWD 3.1%, PMULHW and the saturating
    // adds among the remaining delegated instructions after the first
    // 42-opcode tranche.
    //
    // EMIT_SSE_NEON_RR_REGONLY family (unary members read only r/m):
    case Opcode::PABSB_XMM_XMMM128:
    case Opcode::PABSW_XMM_XMMM128:
    case Opcode::PABSD_XMM_XMMM128:
    case Opcode::PADDSB_XMM_XMMM128:
    case Opcode::PADDSW_XMM_XMMM128:
    case Opcode::PSUBSB_XMM_XMMM128:
    case Opcode::PSUBSW_XMM_XMMM128:
    case Opcode::PHADDW_XMM_XMMM128:
    case Opcode::PHADDD_XMM_XMMM128:
    case Opcode::HADDPS_XMM_XMMM128:
    case Opcode::HADDPD_XMM_XMMM128:
    case Opcode::MOVSLDUP_XMM_XMMM128:
    case Opcode::MOVSHDUP_XMM_XMMM128:
    case Opcode::MOVDDUP_XMM_XMMM64:
    case Opcode::PMULHRSW_XMM_XMMM128:
    // EMIT_SSE_UZP_PAIR family:
    case Opcode::PHSUBW_XMM_XMMM128:
    case Opcode::PHSUBD_XMM_XMMM128:
    case Opcode::PHADDSW_XMM_XMMM128:
    case Opcode::PHSUBSW_XMM_XMMM128:
    case Opcode::HSUBPS_XMM_XMMM128:
    case Opcode::HSUBPD_XMM_XMMM128:
    case Opcode::PSIGNB_XMM_XMMM128:
    case Opcode::PSIGNW_XMM_XMMM128:
    case Opcode::PSIGND_XMM_XMMM128:
    case Opcode::PMULDQ_XMM_XMMM128:
    case Opcode::PMULUDQ_XMM_XMMM128:
    // EMIT_SSE_PACK family:
    case Opcode::PACKSSWB_XMM_XMMM128:
    case Opcode::PACKSSDW_XMM_XMMM128:
    case Opcode::PACKUSWB_XMM_XMMM128:
    case Opcode::PACKUSDW_XMM_XMMM128:
    // Dedicated arms:
    case Opcode::PMULHW_XMM_XMMM128:
    case Opcode::PMULHUW_XMM_XMMM128:
    case Opcode::PMADDUBSW_XMM_XMMM128:
    case Opcode::PSADBW_XMM_XMMM128:
    case Opcode::PSHUFD_XMM_XMMM128_IMM8:
    case Opcode::PSHUFLW_XMM_XMMM128_IMM8:
    case Opcode::PSHUFHW_XMM_XMMM128_IMM8:
    case Opcode::PUNPCKLBW_XMM_XMMM128:
    case Opcode::PUNPCKHBW_XMM_XMMM128:
    case Opcode::PUNPCKLDQ_XMM_XMMM128:
    case Opcode::PUNPCKHDQ_XMM_XMMM128:
    case Opcode::PUNPCKLQDQ_XMM_XMMM128:
    case Opcode::PUNPCKHQDQ_XMM_XMMM128:
    case Opcode::UNPCKLPS_XMM_XMMM128:
    case Opcode::UNPCKHPS_XMM_XMMM128:
    case Opcode::UNPCKLPD_XMM_XMMM128:
    case Opcode::UNPCKHPD_XMM_XMMM128:
    // EMIT_SSE_X86_SQRT is unary (reads r/m only, never vvvv) and already
    // vec_off()-addressed with its helper arm declined on the high lane.
    case Opcode::SQRTPS_XMM_XMMM128:
    case Opcode::SQRTPD_XMM_XMMM128:
    // Blends: VPBLENDW repeats imm8 per lane; VBLENDPS/PD take the high
    // lane's bits from vex256_high_lane_imm (vex256_lane_imm.hpp).
    case Opcode::PBLENDW_XMM_XMMM128_IMM8:
    case Opcode::BLENDPS_XMM_XMMM128_IMM8:
    case Opcode::BLENDPD_XMM_XMMM128_IMM8:
    // Shift by an xmm/m128 count: one count for both lanes, see
    // ops::vex_rm_is_shared_shift_count and the arm.
    case Opcode::PSLLW_XMM_XMMM128:
    case Opcode::PSLLD_XMM_XMMM128:
    case Opcode::PSLLQ_XMM_XMMM128:
    case Opcode::PSRLW_XMM_XMMM128:
    case Opcode::PSRLD_XMM_XMMM128:
    case Opcode::PSRLQ_XMM_XMMM128:
    case Opcode::PSRAW_XMM_XMMM128:
    case Opcode::PSRAD_XMM_XMMM128:
        return !vex256_tranche_disabled();

    // FMA3, packed only. Element-wise across the lane boundary (Intel SDM
    // Vol. 2 VFMADD132PS: the 256-bit form is the 128-bit operation applied to
    // each lane), the lowering reads all three of a/b/c through vec_off(), and
    // it has no helper arm at all -- so no register index ever escapes to a
    // helper that would mean xmm[] on the high lane. The SS/SD forms are
    // deliberately absent: VEX.L=1 on a scalar FMA is reserved (Intel SDM
    // Vol. 2, "FMA Instruction Operand Encoding"), and the scalar lowering's
    // DEST[127:32] merge has no high-lane meaning.
    case Opcode::VFMADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD231PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD231PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB231PD_XMM_XMM_XMMM128:
    // FMA3 alternating (Intel SDM Vol. 2 VFMADDSUB/VFMSUBADD): the 256-bit
    // form alternates over eight (PS) or four (PD) elements starting at an
    // even one, and each 128-bit lane also starts on an even element, so it
    // is the 128-bit operation per lane. The lowering (avx.cpp) addresses
    // a/b/c through vec_off(), has no helper arm, and suppresses its VEX.128
    // upper-half zero under the split.
    case Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128:
        return true;
    default:
        return false;
    }
}

/// Intel SDM Vol. 2: the aligned mnemonics require the memory operand to be
/// aligned to the OPERAND size, so a VEX.256 VMOVAPS wants 32 bytes, not 16.
/// The unaligned mnemonics (and LDDQU) require nothing at any width.
bool vex256_move_requires_align32(Opcode op) {
    switch (op) {
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128:
    case Opcode::MOVDQA_XMM_XMMM128:
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    case Opcode::MOVNTDQ_M128_XMM:
        return true;
    default:
        return false;
    }
}

bool JitCompiler::compile_vex256_lane_split(const DecodedInstruction& instr,
                                            u64 instr_rip, u64 next_rip,
                                            bool is_last) {
    // No nesting: a lane pass must never re-enter the driver.
    if (m_vec_lane_split) return false;
    if (!vex256_lane_split_allowed(instr.opcode)) return false;
    if (instr.operand_count < 1 ||
        instr.operands[0].type != OperandType::Reg128) {
        return false;
    }
    // The register-only restrictions that used to sit here for the converts,
    // PUNPCKLWD/HWD and PMADDWD predate the whole-operand probe below, which
    // is what makes a memory source safe for every allowlisted form: both
    // halves are proven readable before lane 0 writes anything.
    //
    // The shift-by-xmm family keeps ONE 128-bit count for both lanes (Intel
    // SDM Vol. 2 VPSLLD ymm, ymm, xmm/m128), so its r/m operand is neither
    // probed as 32 bytes nor advanced by 16 for the high pass.
    const bool rm_is_shift_count = ops::vex_rm_is_shared_shift_count(instr.opcode);
    // dst == r/m source is lowered: the FINITE_FP cold arm parks the r/m
    // lane in a third register around its helper call (rm_alias), and every
    // other allowlisted lowering reads both sources before its store.

    // vex stays SET so the lowering still takes SRC1 from vvvv; only vex_l is
    // cleared so the 128-bit arms accept it. m_vec_lane_split suppresses the
    // generic bridge (its merge copy and high-lane zero are both wrong here:
    // both lanes are written outright, and a per-lane zero of ymm_high[dst]
    // would destroy the high source of `vpaddd ymm0, ymm0, ymm1`).
    DecodedInstruction lane = instr;
    lane.prefixes.vex_l = false;

    // A memory source is an m256 that the two passes read as two m128
    // halves, lane 0 first. Hardware never writes DEST when any byte of the
    // read faults, but lane 0's result would already be committed when lane
    // 1's read faulted. So read the whole operand once, in the same ascending
    // qword order the two passes use (so the first faulting address is the
    // one they would have reported), BEFORE lane 0 emits anything. After the
    // probe no load in either pass can fault on a page the probe proved
    // readable. The interpreter's bridge restores the destination on a fault
    // for the same reason (avx_sse_bridge.cpp), so the two agree.
    int mem_index = -1;
    for (u8 i = 0; i < lane.operand_count; ++i) {
        if (is_memory_operand(lane.operands[i].type)) mem_index = i;
    }

    const usize mark = m_code_buf.size();
    if (mem_index >= 0) {
        const MemoryOperand& mem = lane.operands[mem_index].mem;
        // disp + 16 must stay an i32 (see the high-lane adjustment below).
        if (mem.disp > std::numeric_limits<i32>::max() - 16) return false;
        const bool hold = should_hold_nzcv();
        if (hold) emit_nzcv_spill();
        emit_compute_ea(REG_CALL_TMP, mem, next_rip);
        // Intel SDM Vol. 2 VMOVAPS/VMOVAPD/VMOVDQA ymm, m256: #GP(0) unless
        // 32-byte aligned. The per-lane arms see a VEX form and skip their
        // 16-byte check, so the whole-operand rule is applied here, once.
        if (vex256_move_requires_align32(instr.opcode)) {
            emit_align_check(REG_CALL_TMP, /*align32=*/true);
        }
        const u32 probe_qwords = rm_is_shift_count ? 2u : 4u;
        for (u32 piece = 0; piece < probe_qwords; ++piece) {
            u8 addr = REG_CALL_TMP;
            if (piece != 0) {
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, piece * 8));
                addr = REG_SCRATCH4;
            }
            emit_guest_load64(REG_SCRATCH0, addr);
        }
        if (hold) emit_nzcv_fill();
    }
    m_vec_lane_split = true;

    // Lane 0 MUST be emitted with is_last=false: the lowering emits the block
    // epilogue on the last instruction, which would land between the lanes and
    // leave the high lane unreachable.
    m_vec_hi_lane = false;
    bool ok = compile_sse(lane, instr_rip, next_rip, /*is_last=*/false);

    if (ok) {
        // Lane 1 addresses YMM[255:128] of every register operand, and the
        // second 16 bytes of a memory operand. The +16 lands on
        // MemoryOperand::disp, so it flows through emit_compute_ea →
        // emit_compute_ea64 and stays a full 64-bit EA.
        // MemoryOperand::disp is i32 (decoder.hpp:114), so a displacement
        // within 16 of INT32_MAX would overflow -- signed UB, and an address
        // that wraps to negative. A disp32 can legitimately reach there, so
        // decline the split rather than emit a wrong EA.
        for (u8 i = 0; i < lane.operand_count; ++i) {
            if (!is_memory_operand(lane.operands[i].type)) continue;
            if (rm_is_shift_count) continue;
            if (lane.operands[i].mem.disp >
                std::numeric_limits<i32>::max() - 16) {
                m_vec_hi_lane = false;
                m_vec_lane_split = false;
                m_code_buf.resize(mark);
                return false;
            }
            lane.operands[i].mem.disp += 16;
        }
        // Cross-lane immediates (VBLENDPS/PD, VSHUFPD, VMPSADBW) give the
        // upper half its own selector bits; see vex256_lane_imm.hpp.
        for (u8 i = 0; i < lane.operand_count; ++i) {
            if (lane.operands[i].type == OperandType::Imm8) {
                lane.operands[i].imm = cpu::vex256_high_lane_imm(
                    lane.opcode, static_cast<u8>(lane.operands[i].imm));
            }
        }
        m_vec_hi_lane = true;
        ok = compile_sse(lane, instr_rip, next_rip, is_last);
    }

    m_vec_hi_lane = false;
    m_vec_lane_split = false;
    if (!ok) {
        m_code_buf.resize(mark);
        return false;
    }
    // A faulting memory source is caught by the whole-operand probe above,
    // before lane 0 writes, so the destination is never half-committed.
    return true;
}

} // namespace vitreo::cpu::jit
