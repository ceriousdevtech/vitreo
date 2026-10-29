// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — AES-NI, SHA and PCLMULQDQ.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
const bool kForceSwPclmul = std::getenv("VITREO_SW_PCLMUL") != nullptr;
} // namespace

// Macro: standard xmm, xmm/m128 pattern (op_id even=reg, odd=mem)
#define EMIT_SSE_XMM_XMMM128(REG_OPID, MEM_OPID) \
    { \
        u8 dst_xmm = op0.reg & 15; \
        if (is_register_operand(op1.type)) { \
            emit(arm64::mov_x(0, REG_CPU_STATE)); \
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3)); \
            emit(arm64::movz_w(2, REG_OPID)); \
            emit(arm64::movz_w(3, dst_xmm)); \
            emit(arm64::movz_w(4, op1.reg & 15)); \
            EMIT_SSE_OP_CALL(); \
        } else { \
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip); \
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1); \
            emit(arm64::mov_x(0, REG_CPU_STATE)); \
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3)); \
            emit(arm64::movz_w(2, MEM_OPID)); \
            emit(arm64::movz_w(3, dst_xmm)); \
            emit(arm64::mov_x(4, REG_SCRATCH4)); \
            EMIT_SSE_OP_CALL(); \
        } \
        break; \
    }

void JitCompiler::emit_sha1_native(Opcode op, u8 dst, u8 imm8) {
    // x86 keeps a SHA-1 state or schedule word quad with its FIRST word in
    // bits 127:96 (Intel SDM Vol. 2 SHA1RNDS4: A = SRC1[127:96]); ARM's
    // SHA1C/P/M and SHA1SU1 want it in lane 0. Reversing the four lanes is
    // REV64 (swap within each half) then EXT #8 (swap the halves).
    const auto reverse = [&](u8 v) {
        emit(arm64::rev64_v4s(v, v));
        emit(arm64::ext_v16b(v, v, v, 8));
    };
    const u32 off = xmm_u64_off(dst, 0);
    switch (op) {
    case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8: {
        // Four rounds, f and K chosen by imm8[1:0]: 0 choose, 1 and 3
        // parity, 2 majority. x86 adds K inside the instruction and has the
        // round-0 E folded into W0 already (SHA1NEXTE); ARM takes E as Sn
        // and K pre-added to W. So W + K, and E = 0. Round by round the two
        // then agree exactly: ARM's SHA1C round is a := ROL(a,5) + f(b,c,d)
        // + e + W, with e := d after it, the SDM's A_(i+1) and E_(i+1).
        static constexpr u32 kK[4] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu,
                                      0xCA62C1D6u};
        static constexpr u8 kHashOp[4] = {0, 1, 2, 1};
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off >> 4));
        reverse(0);
        reverse(1);
        emit_mov_imm32(REG_SCRATCH0, kK[imm8 & 3]);
        emit(arm64::dup_v_gpr(2, REG_SCRATCH0, arm64::VS_S));
        emit(arm64::add_v(1, 1, 2, arm64::VS_S));
        emit(arm64::movi_v16b(3, 0));
        emit(arm64::sha1_hash(kHashOp[imm8 & 3], 0, 3, 1));
        reverse(0);
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
        break;
    }
    case Opcode::SHA1NEXTE_XMM_XMMM128:
        // DEST[127:96] := ROL(SRC1[127:96], 30) + SRC2[127:96];
        // DEST[95:0] := SRC2[95:0].
        emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, (off + 12) >> 2));
        emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, 2));
        emit(arm64::umov_w_s(REG_SCRATCH1, 1, 3));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::ins_v_gpr(1, 3, REG_SCRATCH0, arm64::VS_S));
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, off >> 4));
        break;
    case Opcode::SHA1MSG1_XMM_XMMM128:
        // Lanes 3..0 := SRC1 XOR {SRC1[1], SRC1[0], SRC2[3], SRC2[2]},
        // the SDM's W2^W0, W3^W1, W4^W2, W5^W3. EXT of SRC2:SRC1 by 8 bytes
        // is exactly that second operand.
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off >> 4));
        emit(arm64::ext_v16b(2, 1, 0, 8));
        emit(arm64::eor_v16b(0, 0, 2));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
        break;
    case Opcode::SHA1MSG2_XMM_XMMM128:
        // With both operands lane-reversed, ARM SHA1SU1's T = X ^ (Y >> 32)
        // is {W12^W13 .. } and its four outputs are W16..W19 (the last one's
        // ROL(T0, 2) is ROL1 of W16, the SDM's dependency), in lane order.
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off >> 4));
        reverse(0);
        reverse(1);
        emit(arm64::sha1su1_v(0, 1));
        reverse(0);
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
        break;
    default:
        // Callers pass the four SHA-1 opcodes only.
        break;
    }
}

std::optional<bool> JitCompiler::compile_sse_crypto(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const bool s_force_sw_pclmul = kForceSwPclmul;
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- AES-NI (op_ids 240-249) ----
    // The round transforms map onto ARMv8 FEAT_AES and are emitted inline —
    // see ops/direct_armv8_aes.cpp for the derivation. emit_aes_direct emits
    // nothing when it declines, so the helper sequence below stays exact.
    case Opcode::AESENC_XMM_XMMM128:
        if (emit_aes_direct(instr, next_rip)) break;
        EMIT_SSE_XMM_XMMM128(240, 241)
    case Opcode::AESENCLAST_XMM_XMMM128:
        if (emit_aes_direct(instr, next_rip)) break;
        EMIT_SSE_XMM_XMMM128(242, 243)
    case Opcode::AESDEC_XMM_XMMM128:
        if (emit_aes_direct(instr, next_rip)) break;
        EMIT_SSE_XMM_XMMM128(244, 245)
    case Opcode::AESDECLAST_XMM_XMMM128:
        if (emit_aes_direct(instr, next_rip)) break;
        EMIT_SSE_XMM_XMMM128(246, 247)
    case Opcode::AESIMC_XMM_XMMM128:
        if (emit_aes_direct(instr, next_rip)) break;
        EMIT_SSE_XMM_XMMM128(248, 249)

    // ---- SHA-256 (op_ids 260-265) ----
    // The register-source message-schedule forms map onto ARMv8 SHA-256 and
    // are emitted directly. Memory-source forms keep the helper, which owns
    // the two checked guest reads and their fault behavior.
    case Opcode::SHA256RNDS2_XMM_XMMM128: EMIT_SSE_XMM_XMMM128(260, 261)
    case Opcode::SHA256MSG1_XMM_XMMM128:
        if (is_register_operand(op1.type) &&
            emit_sha256_msg_direct(instr.opcode, op0.reg & 15, op1.reg & 15)) {
            break;
        }
        EMIT_SSE_XMM_XMMM128(262, 263)
    case Opcode::SHA256MSG2_XMM_XMMM128:
        if (is_register_operand(op1.type) &&
            emit_sha256_msg_direct(instr.opcode, op0.reg & 15, op1.reg & 15)) {
            break;
        }
        EMIT_SSE_XMM_XMMM128(264, 265)

    // ---- SHA-1 (op_ids 268-273) ----
    case Opcode::SHA1NEXTE_XMM_XMMM128:   EMIT_SSE_XMM_XMMM128(268, 269)
    case Opcode::SHA1MSG1_XMM_XMMM128:    EMIT_SSE_XMM_XMMM128(270, 271)
    case Opcode::SHA1MSG2_XMM_XMMM128:    EMIT_SSE_XMM_XMMM128(272, 273)

#undef EMIT_SSE_XMM_XMMM128

    // ---- AESKEYGENASSIST xmm, xmm/m128, imm8 (op_id 250/251) ----
    case Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u8 imm8 = static_cast<u8>(instr.operands[2].imm);
        if (is_register_operand(op1.type)) {
            u16 packed_src = static_cast<u16>((op1.reg & 15) | (imm8 << 8));
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 250));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, packed_src));
            EMIT_SSE_OP_CALL();
        } else {
            u16 packed_dst = static_cast<u16>(dst_xmm | (imm8 << 8));
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 251));
            emit(arm64::movz_w(3, packed_dst));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- PCLMULQDQ xmm, xmm/m128, imm8 (op_id 252/253) ----
    case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u8 imm8 = static_cast<u8>(instr.operands[2].imm);
        const bool force_software = s_force_sw_pclmul;
        if (is_register_operand(op1.type) && !force_software) {
            const u8 src_xmm = op1.reg & 15;
            const u32 dst_off = xmm_u64_off(dst_xmm, 0);
            const u32 src_off = xmm_u64_off(src_xmm, 0);
            if (fits_ldr_q_offset(dst_off) && fits_ldr_q_offset(src_off)) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, src_off >> 4));
                if ((imm8 & 0x01) != 0)
                    emit(arm64::ext_v16b(0, 0, 0, 8));
                if ((imm8 & 0x10) != 0)
                    emit(arm64::ext_v16b(1, 1, 1, 8));
                emit(arm64::pmull_v1q(0, 0, 1));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
                break;
            }
        }

        if (is_register_operand(op1.type)) {
            u16 packed_src = static_cast<u16>((op1.reg & 15) | (imm8 << 8));
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 252));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, packed_src));
            EMIT_SSE_OP_CALL();
        } else {
            u16 packed_dst = static_cast<u16>(dst_xmm | (imm8 << 8));
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 253));
            emit(arm64::movz_w(3, packed_dst));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- SHA1RNDS4 xmm, xmm/m128, imm8 (op_ids 266/267) ----
    case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u8 imm8 = static_cast<u8>(instr.operands[2].imm);
        if (is_register_operand(op1.type) &&
            fits_ldr_q_offset(xmm_u64_off(dst_xmm, 0)) &&
            fits_ldr_q_offset(xmm_u64_off(op1.reg & 15, 0))) {
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE,
                                   xmm_u64_off(op1.reg & 15, 0) >> 4));
            emit_sha1_native(instr.opcode, dst_xmm, imm8);
            break;
        }
        if (is_register_operand(op1.type)) {
            u16 packed_src = static_cast<u16>((op1.reg & 15) | (imm8 << 8));
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 266));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, packed_src));
            EMIT_SSE_OP_CALL();
        } else {
            u16 packed_dst = static_cast<u16>(dst_xmm | (imm8 << 8));
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 267));
            emit(arm64::movz_w(3, packed_dst));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
