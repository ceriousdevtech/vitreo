// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — direct ARMv8 FEAT_SHA1 lowering for x86 SHA1RNDS4 / SHA1NEXTE /
// SHA1MSG1 / SHA1MSG2, every block width and memory configuration (WS3).

#include "../jit_helpers.hpp"

/// @file direct_armv8_sha1.cpp
/// @brief Stages xmm2/m128 into V1 with emit_crypto_src128 and runs the
///        existing emit_sha1_native lowering (ops/sse_crypto.cpp), whose
///        derivation from Intel SDM Vol. 2 SHA1RNDS4/SHA1NEXTE/SHA1MSG1/
///        SHA1MSG2 and the ARM ARM SHA1C/P/M / SHA1SU1 pseudocode lives there.
///
/// Before WS3 a 32-bit block reached it only for the SHA1RNDS4 register form
/// and every other form dispatched `jit_sse_op`; ops/sse64.cpp lowers the
/// 64-bit register and linear64 memory forms itself and declines the
/// x64-region memory form, which then lands here. emit_sha1_native reads both
/// operands into V registers before its single store, so dst == src is exact,
/// and none of the four touches EFLAGS.

namespace vitreo::cpu::jit {

bool JitCompiler::emit_sha1_direct(const DecodedInstruction& instr,
                                   u64 next_rip) {
    const Opcode op = instr.opcode;
    if (op != Opcode::SHA1RNDS4_XMM_XMMM128_IMM8 &&
        op != Opcode::SHA1NEXTE_XMM_XMMM128 &&
        op != Opcode::SHA1MSG1_XMM_XMMM128 &&
        op != Opcode::SHA1MSG2_XMM_XMMM128) {
        return false;
    }
    if (!host_supports_sha1()) return false;

    const u8 dst_xmm = static_cast<u8>(instr.operands[0].reg & 15);
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    // SHA1NEXTE reads DEST[127:96] with LDR W at dst_off + 12.
    if (!fits_ldr_q_offset(dst_off) || !fits_ldr_x_offset(dst_off + 8)) {
        return false;
    }
    const u8 imm8 = op == Opcode::SHA1RNDS4_XMM_XMMM128_IMM8
                        ? static_cast<u8>(instr.operands[2].imm)
                        : u8{0};

    const usize mark = m_code_buf.size();
    if (!emit_crypto_src128(1, instr, instr.operands[1], next_rip)) {
        m_code_buf.resize(mark);
        return false;
    }
    emit_sha1_native(op, dst_xmm, imm8);
    return true;
}

} // namespace vitreo::cpu::jit
