// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — PCLMULQDQ / VPCLMULQDQ (VEX.128) on ARMv8 FEAT_PMULL (WS3).

#include "../jit_helpers.hpp"

/// @file direct_armv8_pclmul.cpp
/// @brief Register, memory and VEX.128 forms of PCLMULQDQ as one PMULL .1Q.
///
/// Intel SDM Vol. 2 PCLMULQDQ xmm1, xmm2/m128, imm8:
///   TEMP1 := imm8[0] ? SRC1[127:64] : SRC1[63:0]
///   TEMP2 := imm8[4] ? SRC2[127:64] : SRC2[63:0]
///   DEST  := carry-less product TEMP1 * TEMP2 (128 bits)
/// with the other immediate bits ignored. SRC1 is DEST (legacy) or VEX.vvvv
/// (VEX.128, which also zeroes DEST[MAXVL-1:128]); SRC2 is xmm2/m128 (legacy
/// m128 must be 16-byte aligned, VEX m128 need not be).
///
/// ARM PMULL Vd.1Q, Vn.1D, Vm.1D multiplies the low doublewords, so a
/// selector bit becomes EXT #8 (swap halves) of that operand first. Both
/// operands are in V registers before the single store (dst == src exact).
/// No EFLAGS effect.

namespace vitreo::cpu::jit {

namespace {
// VITREO_SW_PCLMUL forces the checked helper (the existing opt-out in
// ops/sse_crypto.cpp, which reads the same variable).
const bool kForceSwPclmulDirect = std::getenv("VITREO_SW_PCLMUL") != nullptr;
} // namespace

bool JitCompiler::emit_pclmul_direct(const DecodedInstruction& instr,
                                     u64 next_rip) {
    if (instr.opcode != Opcode::PCLMULQDQ_XMM_XMMM128_IMM8) return false;
    if (!host_supports_pmull() || kForceSwPclmulDirect) return false;
    if (m_vec_lane_split) return false;
    const bool vex = instr.prefixes.vex;
    if (vex && instr.prefixes.vex_l) return false;

    const u8 dst_xmm = static_cast<u8>(instr.operands[0].reg & 15);
    const u8 src1_xmm = vex ? static_cast<u8>(instr.prefixes.vex_vvvv & 15)
                            : dst_xmm;
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    const u32 src1_off = xmm_u64_off(src1_xmm, 0);
    const u32 dhi_off = ymm_high_u64_off(dst_xmm, 0);
    if (!fits_ldr_q_offset(dst_off) || !fits_ldr_q_offset(src1_off) ||
        (vex && !fits_ldr_q_offset(dhi_off))) {
        return false;
    }
    const u8 imm8 = static_cast<u8>(instr.operands[2].imm);

    const usize mark = m_code_buf.size();
    if (!emit_crypto_src128(1, instr, instr.operands[1], next_rip)) {
        m_code_buf.resize(mark);
        return false;
    }
    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src1_off >> 4));
    if ((imm8 & 0x01) != 0) emit(arm64::ext_v16b(0, 0, 0, 8));
    if ((imm8 & 0x10) != 0) emit(arm64::ext_v16b(1, 1, 1, 8));
    emit(arm64::pmull_v1q(0, 0, 1));
    emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
    if (vex) {
        emit(arm64::eor_v16b(5, 5, 5));
        emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
    }
    return true;
}

} // namespace vitreo::cpu::jit
