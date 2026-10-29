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

} // namespace vitreo::cpu::jit
