// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the VEX forms whose legacy-family lowering implements the VEX
// encoding itself, so compile_sse hands them over with prefixes.vex (and
// vex_l) still set instead of running its generic bridge.
//
// The bridge copies VEX.vvvv into the destination and then runs the
// two-operand legacy lowering. That is exact only when the r/m register is
// not the destination: `vdivsd xmm0, xmm1, xmm0` would have its SRC2
// destroyed by the copy, so the bridge declines it and the instruction is
// interpreted -- and so does every unary form whose source is its own
// destination (`vpmovsxwd xmm1, xmm1`, `vcvtps2pd xmm3, xmm3`), because the
// unary forms have no vvvv operand and the bridge still copies it. Nor can
// the bridge express the VEX.256 widen/narrow forms, which are not two
// independent 128-bit lanes.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

bool sse_vex_lowered_directly(const DecodedInstruction& instr) noexcept {
    if (!instr.prefixes.vex || instr.prefixes.evex) return false;
    switch (instr.opcode) {
    // NDS: DEST := SRC1 (vvvv) merged with the result, DEST[MAXVL-1:128] := 0
    // (Intel SDM Vol. 2 VADDSS..VDIVSD, VINSERTPS, VMOVLHPS, VMOVHLPS). The
    // scalar forms are VEX.LIG (the decoder clears L); the other three are
    // VEX.128-only and L=1 is rejected at decode (vex_l_is_reserved).
    case Opcode::ADDSS_XMM_XMMM32: case Opcode::ADDSD_XMM_XMMM64:
    case Opcode::SUBSS_XMM_XMMM32: case Opcode::SUBSD_XMM_XMMM64:
    case Opcode::MULSS_XMM_XMMM32: case Opcode::MULSD_XMM_XMMM64:
    case Opcode::DIVSS_XMM_XMMM32: case Opcode::DIVSD_XMM_XMMM64:
    case Opcode::INSERTPS_XMM_XMMM32_IMM8:
    case Opcode::MOVLHPS_XMM_XMM: case Opcode::MOVHLPS_XMM_XMM:
        return !instr.prefixes.vex_l;
    // Unary, VEX.128 and VEX.256 (Intel SDM Vol. 2 VPMOVSX/VPMOVZX,
    // VCVTPS2PD, VCVTPD2DQ, VCVTTPD2DQ). VEX.vvvv is reserved and must be
    // 1111b (decoded as register 0); anything else is #UD, which stays with
    // the interpreter.
    case Opcode::PMOVSXBW_XMM_XMMM64: case Opcode::PMOVSXBD_XMM_XMMM32:
    case Opcode::PMOVSXBQ_XMM_XMMM16: case Opcode::PMOVSXWD_XMM_XMMM64:
    case Opcode::PMOVSXWQ_XMM_XMMM32: case Opcode::PMOVSXDQ_XMM_XMMM64:
    case Opcode::PMOVZXBW_XMM_XMMM64: case Opcode::PMOVZXBD_XMM_XMMM32:
    case Opcode::PMOVZXBQ_XMM_XMMM16: case Opcode::PMOVZXWD_XMM_XMMM64:
    case Opcode::PMOVZXWQ_XMM_XMMM32: case Opcode::PMOVZXDQ_XMM_XMMM64:
    case Opcode::CVTPS2PD_XMM_XMMM64:
    case Opcode::CVTPD2DQ_XMM_XMMM128: case Opcode::CVTTPD2DQ_XMM_XMMM128:
        return (instr.prefixes.vex_vvvv & 15) == 0;
    default:
        return false;
    }
}

} // namespace vitreo::cpu::jit
