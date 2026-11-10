// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — SSE/SSE2 ops (MOVDQA, MOVUPS, MOVLHPS, MOVHLPS, MOVLPS,
//              CVTPS2PD, MOVD, MOVQ, PMOVMSKB, packed shifts, shuffles,
//              PUNPCK, PINSRW, PEXTRW, MOVSS, and all xmm,xmm/m128 integer ops)

#include "../jit_helpers.hpp"

/// @file direct_armv8_sha_256_message.cpp
/// @brief Direct ARMv8 SHA-256 message schedule.
///
/// Closed cut from sse.cpp — verified with plan_extraction.py.

namespace vitreo::cpu::jit {

// ---------------------------------------------------------------------------
// Direct ARMv8 SHA-256 message schedule.
//
// An x64 self-extract profile (VITREO_JIT_SSE_HIST=1) showed the SHA-256
// register forms as the entire remaining jit_sse_op traffic: SHA256RNDS2 57%,
// SHA256MSG1 21.4%, SHA256MSG2 21.4% of 67M helper dispatches. The two message
// forms have exact ARMv8 equivalents, so they no longer need an ABI crossing.
//
// SHA256MSG1 dst, src
//   x86:  dst[i] += sigma0(w[i+1]) with w = {dst[0..3], src[0]}
//   ARM:  SHA256SU0 Vd.4S, Vn.4S is that operation verbatim.
//
// SHA256MSG2 dst, src
//   x86:  dst[0] += sigma1(src[2]);      dst[1] += sigma1(src[3]);
//         dst[2] += sigma1(dst[0]new);   dst[3] += sigma1(dst[1]new)
//   ARM:  SHA256SU1 Vd.4S, Vn.4S, Vm.4S additionally adds
//         T0 = Vn.S[1..3]:Vm.S[0] into Vd before the sigma1 chain, and takes
//         its sigma1 inputs from Vm.S[2..3]. Feeding Vn = 0 and
//         Vm = {0, _, src[2], src[3]} makes T0 zero and leaves exactly the
//         x86 result. Vm is built by zeroing a register and copying only the
//         upper 64-bit lane of the source into it.
//
// Neither instruction reads or writes x86 flags, and neither touches guest
// memory, so this is a pure register-to-register substitution. Vectors are
// loaded before the operation, so dst == src keeps x86 read-before-write
// behavior.
bool JitCompiler::emit_sha256_msg_direct(Opcode opcode, u8 dst_xmm,
                                          u8 src_xmm) {
    if (opcode != Opcode::SHA256MSG1_XMM_XMMM128 &&
        opcode != Opcode::SHA256MSG2_XMM_XMMM128) {
        return false;
    }

    // Opt-out for the control arm of a before/after measurement, and for
    // bisecting a suspected SHA mismatch against the helper path.
    static const bool disabled =
        std::getenv("VITREO_JIT_NO_SHA256_DIRECT") != nullptr;
    if (disabled) {
        return false;
    }

    const u32 off_d = xmm_u64_off(dst_xmm, 0);
    const u32 off_s = xmm_u64_off(src_xmm, 0);
    if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_s)) {
        return false;
    }

    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));   // V0 = dst
    emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));   // V1 = src

    if (opcode == Opcode::SHA256MSG1_XMM_XMMM128) {
        emit(arm64::sha256su0(0, 1));
    } else {
        emit(arm64::eor_v16b(2, 2, 2));                      // V2 = 0 (Vn)
        emit(arm64::eor_v16b(3, 3, 3));                      // V3 = 0
        emit(arm64::ins_v_d1_from_d1(3, 1));                 // V3 = {0,0,s2,s3}
        emit(arm64::sha256su1(0, 2, 3));
    }

    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
    return true;
}

// ---------------------------------------------------------------------------
// WS3: SHA256RNDS2 (register and memory) and the SHA256MSG1/2 memory forms.
//
// SHA256RNDS2 xmm1, xmm2/m128, <XMM0> (Intel SDM Vol. 2):
//   A = SRC2[127:96] B = SRC2[95:64]  C = SRC1[127:96] D = SRC1[95:64]
//   E = SRC2[63:32]  F = SRC2[31:0]   G = SRC1[63:32]  H = SRC1[31:0]
//   two rounds with WK0 = XMM0[31:0], WK1 = XMM0[63:32];
//   DEST := {A2, B2, E2, F2} (A2 at [127:96]).
// So in lane order (lane 0 first) SRC2 = {F,E,B,A} and SRC1 = {H,G,D,C}.
//
// ARM ARM SHA256hash(X, Y, W, part1): X = {a,b,c,d}, Y = {e,f,g,h}, lane 0
// first. Each of its FOUR rounds computes the new a into Y[3] and the new e
// into X[3], then rotates Y:X left by one lane, so after round k
// X = {a_k, a_(k-1), a_(k-2), a_(k-3)} and likewise for Y. After all four,
// X' = {a4,a3,a2,a1} and Y' = {e4,e3,e2,e1}; lanes 2-3 are the values after
// two rounds and depend only on W[0] and W[1] (XMM0[127:64] is irrelevant,
// as it is to x86). SHA256H Qd=X, Qn=Y returns X'; SHA256H2 Qd=Y, Qn=X returns
// Y' (both from the ORIGINAL X and Y, hence the copy).
//
//   stage SRC2 -> V1 = {F,E,B,A}         ; first: a memory stub clobbers V0-V7
//   V0 = SRC1 = {H,G,D,C} ; V2 = XMM0 = {WK0,WK1,_,_}
//   ZIP2.2D V3,V1,V0 = {B,A,D,C} ; REV64.4S -> X = {A,B,C,D}
//   ZIP1.2D V4,V1,V0 = {F,E,H,G} ; REV64.4S -> Y = {E,F,G,H}
//   MOV V5,V3 ; SHA256H Q3,Q4,V2 (X') ; SHA256H2 Q4,Q5,V2 (Y')
//   ZIP2.2D V0,V4,V3 = {e2,e1,a2,a1} ; REV64.4S -> {e1,e2,a1,a2}
//                                     = {F2,E2,B2,A2}  -> DEST
// Every source (including the implicit XMM0, which may be DEST or SRC2) is
// read before the single store. No EFLAGS effect.
//
// The MSG1/MSG2 memory forms stage SRC2 the same way and then run
// emit_sha256_msg_direct's SU0 / SU1 sequence.
// ---------------------------------------------------------------------------
bool JitCompiler::emit_sha256_direct(const DecodedInstruction& instr,
                                     u64 next_rip) {
    const Opcode op = instr.opcode;
    if (op != Opcode::SHA256RNDS2_XMM_XMMM128 &&
        op != Opcode::SHA256MSG1_XMM_XMMM128 &&
        op != Opcode::SHA256MSG2_XMM_XMMM128) {
        return false;
    }
    if (!host_supports_sha256()) return false;
    // The register MSG1/MSG2 forms keep their own (pre-WS3) emitter and its
    // VITREO_JIT_NO_SHA256_DIRECT opt-out.
    if (op != Opcode::SHA256RNDS2_XMM_XMMM128 &&
        is_register_operand(instr.operands[1].type)) {
        return false;
    }

    const u8 dst_xmm = static_cast<u8>(instr.operands[0].reg & 15);
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    const u32 wk_off = xmm_u64_off(0, 0);
    if (!fits_ldr_q_offset(dst_off) || !fits_ldr_q_offset(wk_off)) return false;

    const usize mark = m_code_buf.size();
    if (!emit_crypto_src128(1, instr, instr.operands[1], next_rip)) {
        m_code_buf.resize(mark);
        return false;
    }
    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
    if (op == Opcode::SHA256RNDS2_XMM_XMMM128) {
        emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, wk_off >> 4));
        emit(arm64::zip2_v(3, 1, 0, arm64::VS_D));
        emit(arm64::rev64_v4s(3, 3));
        emit(arm64::zip1_v(4, 1, 0, arm64::VS_D));
        emit(arm64::rev64_v4s(4, 4));
        emit(arm64::mov_v16b(5, 3));
        emit(arm64::sha256h(3, 4, 2));
        emit(arm64::sha256h2(4, 5, 2));
        emit(arm64::zip2_v(0, 4, 3, arm64::VS_D));
        emit(arm64::rev64_v4s(0, 0));
    } else if (op == Opcode::SHA256MSG1_XMM_XMMM128) {
        emit(arm64::sha256su0(0, 1));
    } else {
        emit(arm64::eor_v16b(2, 2, 2));                      // Vn = 0
        emit(arm64::eor_v16b(3, 3, 3));
        emit(arm64::ins_v_d1_from_d1(3, 1));                 // {0,0,s2,s3}
        emit(arm64::sha256su1(0, 2, 3));
    }
    emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
    return true;
}

} // namespace vitreo::cpu::jit
