// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — legacy SSE 128-bit and scalar data movement: MOVDQA/MOVAPS/
// MOVAPD, MOVNTDQA, MOVUPS/MOVUPD/MOVDQU/LDDQU, the 128-bit stores, MOVLHPS,
// MOVHLPS, MOVLPS/MOVLPD/MOVHPS/MOVHPD, MOVD, MOVQ, PMOVMSKB, MOVSS/MOVSD,
// MOVMSKPS/MOVMSKPD and MASKMOVDQU.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
const bool kDisableMovdqu =
    std::getenv("VITREO_JIT_SSE_DISABLE_MOVDQU") != nullptr;
} // namespace

std::optional<bool> JitCompiler::compile_sse_data(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const bool disable_movdqu = kDisableMovdqu;
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- MOVDQA/MOVAPS/MOVAPD xmm, xmm/m128 — aligned 128-bit load ----

    case Opcode::MOVDQA_XMM_XMMM128:
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128: {
        if (instr.prefixes.vex_l) return false;
        u8 dst_xmm = op0.reg & 15;
        if (m_vec_lane_split) {
            // emit_sse_reg_copy and emit_sse_load_mem128_fast take register
            // INDICES and always address xmm[] -- the LOW lane. On the
            // high-lane pass they would rewrite the low lane and leave
            // ymm[255:128] stale, silently. Emit a lane-addressed move
            // instead. A memory source is two guarded 8-byte loads with the
            // EA parked in X21 (the loads may BLR): the driver has already
            // probed the whole m256 and, for the aligned mnemonics, checked
            // its 32-byte alignment. This used to decline the memory form,
            // so every `vmovdqa ymm, [mem]` in a 32-bit guest -- which has
            // no linear64 wide-access path -- ran in the interpreter.
            const u32 d_off = vec_off(dst_xmm);
            if (!fits_ldr_q_offset(d_off)) return false;
            if (!is_register_operand(op1.type)) {
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
                emit(arm64::str_q_uoff(1, REG_CPU_STATE, d_off >> 4));
                break;
            }
            const u32 s_off = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(s_off)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s_off >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_off >> 4));
            break;
        }
        if (is_register_operand(op1.type)) {
            emit_sse_reg_copy(dst_xmm, op1.reg & 15, 0);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            // Intel SDM Vol. 2 MOVAPS/MOVAPD/MOVDQA: a memory operand that is
            // not 16-byte aligned raises #GP(0). The interpreter models it
            // (load_xmm_aligned / require_align16) and this did not, in either
            // bitness -- the mem_op_id 1-vs-15 split below looks like it
            // carries the distinction but jit_sse_op treats both identically.
            // Two hot words; the raise lives in a cold stub.
            emit_align_check(REG_SCRATCH4, /*align32=*/false);
            emit_sse_load_mem128_fast(dst_xmm, REG_SCRATCH4, 1);
        }
        break;
    }

    // Intel SDM Vol. 2 MOVNTDQA: 128-bit load; NT hint ignored. Interpreter
    // does not #GP on unaligned sources — match that (no jit_require_align16).
    case Opcode::MOVNTDQA_XMM_M128: {
        if (!is_memory_operand(op1.type)) {
            return false;
        }
        emit_update_rip(instr_rip);
        emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
        emit_sse_load_mem128_fast(op0.reg & 15, REG_SCRATCH4, 1);
        break;
    }

    // ---- MOVUPS/MOVUPD/MOVDQU/LDDQU xmm, xmm/m128 — unaligned 128-bit load ----
    case Opcode::MOVUPS_XMM_XMMM128:
    case Opcode::MOVUPD_XMM_XMMM128:
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::LDDQU_XMM_M128: {
        if (instr.prefixes.vex_l) return false;
        if (disable_movdqu &&
            (instr.opcode == Opcode::MOVDQU_XMM_XMMM128 ||
             instr.opcode == Opcode::LDDQU_XMM_M128)) {
            return false;
        }
        u8 dst_xmm = op0.reg & 15;
        if (m_vec_lane_split) {
            // emit_sse_reg_copy and emit_sse_load_mem128_fast take register
            // INDICES and always address xmm[] -- the LOW lane. On the
            // high-lane pass they would rewrite the low lane and leave
            // ymm[255:128] stale, silently. Emit a lane-addressed move
            // instead. A memory source is two guarded 8-byte loads with the
            // EA parked in X21 (the loads may BLR): the driver has already
            // probed the whole m256 and, for the aligned mnemonics, checked
            // its 32-byte alignment. This used to decline the memory form,
            // so every `vmovdqa ymm, [mem]` in a 32-bit guest -- which has
            // no linear64 wide-access path -- ran in the interpreter.
            const u32 d_off = vec_off(dst_xmm);
            if (!fits_ldr_q_offset(d_off)) return false;
            if (!is_register_operand(op1.type)) {
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
                emit(arm64::str_q_uoff(1, REG_CPU_STATE, d_off >> 4));
                break;
            }
            const u32 s_off = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(s_off)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s_off >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_off >> 4));
            break;
        }
        if (is_register_operand(op1.type)) {
            emit_sse_reg_copy(dst_xmm, op1.reg & 15, 14);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_sse_load_mem128_fast(dst_xmm, REG_SCRATCH4, 15);
        }
        break;
    }

    // ---- 128-bit stores: MOVUPS/MOVUPD/MOVDQU/MOVAPS/MOVAPD/MOVDQA/MOVNT* ----
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVUPD_XMMM128_XMM:
    case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    // Intel SDM Vol. 2 MOVNTDQ: 128-bit store; NT hint ignored (same as MOVDQA).
    case Opcode::MOVNTDQ_M128_XMM: {
        if (instr.prefixes.vex_l) return false;
        if (disable_movdqu && instr.opcode == Opcode::MOVDQU_XMMM128_XMM) return false;
        u8 src_xmm = op1.reg & 15;
        if (is_register_operand(op0.type)) {
            emit_sse_reg_copy(op0.reg & 15, src_xmm, 16);
        } else {
            emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            // Same #GP(0)-on-misalign rule as the aligned load above. This arm
            // carries both the aligned and unaligned stores, so the check is
            // conditional -- MOVUPS/MOVUPD/MOVDQU have no alignment
            // requirement. The set matches the interpreter's exactly (see the
            // MOVAPS/MOVNT* store arm of exec_sse_data).
            switch (instr.opcode) {
            case Opcode::MOVAPS_XMMM128_XMM:
            case Opcode::MOVAPD_XMMM128_XMM:
            case Opcode::MOVDQA_XMMM128_XMM:
            case Opcode::MOVNTPS_M128_XMM:
            case Opcode::MOVNTPD_M128_XMM:
            case Opcode::MOVNTDQ_M128_XMM:
                emit_align_check(REG_SCRATCH4, /*align32=*/false);
                break;
            default:
                break;
            }
            emit_sse_store_mem128_fast(src_xmm, REG_SCRATCH4, instr_rip);
        }
        break;
    }

    // ---- MOVLHPS xmm, xmm / VMOVLHPS xmm, xmm, xmm ----
    // Intel SDM Vol.2: DEST[63:0] := SRC1[63:0]; DEST[127:64] := SRC2[63:0].
    // SRC1 is the destination for the legacy form and VEX.vvvv for the VEX
    // form, which also zeroes DEST[MAXVL-1:128].
    // ---- MOVHLPS xmm, xmm / VMOVHLPS xmm, xmm, xmm ----
    // Intel SDM Vol.2: DEST[63:0] := SRC2[127:64]; DEST[127:64] := SRC1[127:64].
    case Opcode::MOVLHPS_XMM_XMM:
    case Opcode::MOVHLPS_XMM_XMM: {
        const bool lh = instr.opcode == Opcode::MOVLHPS_XMM_XMM;
        const bool vex = instr.prefixes.vex;
        u8 dst_xmm = op0.reg & 15;
        u8 src_xmm = op1.reg & 15;
        const u8 src1_xmm = vex ? (instr.prefixes.vex_vvvv & 15) : dst_xmm;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        u32 off_s = xmm_u64_off(src_xmm, 0);
        const u32 off_1 = xmm_u64_off(src1_xmm, 0);
        const u32 off_dhi = ymm_high_u64_off(dst_xmm, 0);
        if (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_s) &&
            fits_ldr_q_offset(off_1) && (!vex || fits_ldr_q_offset(off_dhi))) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
            if (lh) {
                emit(arm64::ins_v_elem(0, 1, 1, 0, arm64::VS_D));
            } else {
                emit(arm64::ins_v_elem(0, 0, 1, 1, arm64::VS_D));
            }
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            if (vex) {
                emit(arm64::movi_v16b(2, 0));
                emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_dhi >> 4));
            }
            break;
        }
        if (vex) return false;
        if (!lh) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 18));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, src_xmm));
            EMIT_SSE_OP_CALL();
            break;
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::movz_w(2, 13));
        emit(arm64::movz_w(3, dst_xmm));
        emit(arm64::movz_w(4, src_xmm));
        EMIT_SSE_OP_CALL();
        break;
    }

    // ---- MOVLPS/MOVLPD xmm, m64 ----
    // Intel SDM Vol. 2: DEST[63:0] := SRC; DEST[127:64] unmodified.
    case Opcode::MOVLPS_XMM_M64:
    case Opcode::MOVLPD_XMM_M64: {
        u8 dst_xmm = op0.reg & 15;
        const u32 off = xmm_u64_off(dst_xmm, 0);
        emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
        if (fits_ldr_x_offset(off)) {
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
            break;
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::movz_w(2, 2));
        emit(arm64::movz_w(3, dst_xmm));
        emit(arm64::mov_x(4, REG_SCRATCH4));
        EMIT_SSE_OP_CALL();
        break;
    }

    // ---- MOVLPS/MOVLPD m64, xmm ----
    case Opcode::MOVLPS_M64_XMM:
    case Opcode::MOVLPD_M64_XMM: {
        if (!is_memory_operand(op0.type)) {
            return false;
        }

        if (!emit_xmm_qword_store(op1.reg & 15, 0, instr, op0, next_rip))
            return false;
        break;
    }

    // ---- MOVHPS/MOVHPD xmm, m64 ----
    // Intel SDM Vol. 2: DEST[127:64] := SRC; DEST[63:0] unmodified.
    case Opcode::MOVHPS_XMM_M64:
    case Opcode::MOVHPD_XMM_M64: {
        u8 dst_xmm = op0.reg & 15;
        const u32 off = xmm_u64_off(dst_xmm, 1);
        if (!fits_ldr_x_offset(off)) return false;
        emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
        break;
    }

    // ---- MOVHPS/MOVHPD m64, xmm ----
    case Opcode::MOVHPS_M64_XMM:
    case Opcode::MOVHPD_M64_XMM: {
        if (!is_memory_operand(op0.type)) {
            return false;
        }
        if (!emit_xmm_qword_store(op1.reg & 15, 1, instr, op0, next_rip))
            return false;
        break;
    }

    // ---- MOVD xmm, r/m32 ----
    // Intel SDM Vol. 2 MOVD: DEST[31:0] ← SRC; DEST[127:32] ← 0.
    case Opcode::MOVD_XMM_RM32: {
        u8 dst_xmm = op0.reg & 15;
        const u32 dst_off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_x_offset(dst_off) || !fits_ldr_x_offset(dst_off + 8))
            return false;
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        // Zero-extended dword in X0 becomes the low qword; clear high qword.
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, dst_off >> 3));
        emit(arm64::str_x_uoff(31, REG_CPU_STATE, (dst_off + 8) >> 3));
        break;
    }

    // ---- MOVD r/m32, xmm ----
    // Intel SDM Vol. 2 MOVD: DEST ← SRC[31:0] (GPR write zero-extends).
    case Opcode::MOVD_RM32_XMM: {
        u8 src_xmm = op1.reg & 15;
        const u32 src_off = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_w_offset(src_off)) return false;
        emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 2));
        if (is_register_operand(op0.type)) {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH4);
        }
        break;
    }

    // ---- MOVQ xmm, xmm/m64 ----
    // Intel SDM Vol. 2 MOVQ: DEST[63:0] ← SRC; DEST[127:64] ← 0.
    case Opcode::MOVQ_XMM_XMMM64: {
        u8 dst_xmm = op0.reg & 15;
        const u32 dst_off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_x_offset(dst_off) || !fits_ldr_x_offset(dst_off + 8))
            return false;
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) return false;
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, dst_off >> 3));
        emit(arm64::str_x_uoff(31, REG_CPU_STATE, (dst_off + 8) >> 3));
        break;
    }

    // ---- MOVQ xmm/m64, xmm ----
    // Reg dest clears upper 64; mem dest stores exactly eight bytes.
    case Opcode::MOVQ_XMMM64_XMM: {
        u8 src_xmm = op1.reg & 15;
        const u32 src_off = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_x_offset(src_off)) return false;
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        if (is_register_operand(op0.type)) {
            const u32 dst_off = xmm_u64_off(op0.reg & 15, 0);
            if (!fits_ldr_x_offset(dst_off) || !fits_ldr_x_offset(dst_off + 8))
                return false;
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, dst_off >> 3));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, (dst_off + 8) >> 3));
        } else {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store64(REG_CALL_TMP, REG_SCRATCH4);
        }
        break;
    }

    // ---- PMOVMSKB r32, xmm ----
    case Opcode::PMOVMSKB_R32_XMM: {
        // VEX.256 VPMOVMSKB is a 32-bit mask (Intel SDM Vol. 2). The 32-bit
        // JIT does not lower the high lane; fall back to the interpreter.
        if (instr.prefixes.vex_l) return false;
        const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
        if (!fits_ldr_x_offset(off_s) || !fits_ldr_x_offset(off_s + 8))
            return false;
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_s >> 3));
        emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                               (off_s + 8) >> 3));
        emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0, 7));
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH1, 7));
        emit_mov_imm64(REG_SCRATCH2, 0x0101010101010101ull);
        emit(arm64::and_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
        emit(arm64::and_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit_mov_imm64(REG_SCRATCH2, 0x0102040810204080ull);
        emit(arm64::mul_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
        emit(arm64::mul_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0, 56));
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH1, 56));
        emit(arm64::orr_x_lsl(REG_SCRATCH0, REG_SCRATCH0,
                              REG_SCRATCH1, 8));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- MOVSS xmm, xmm/m32 ----
    // Intel SDM Vol. 2 MOVSS: reg-reg copies only DEST[31:0]; mem zeros
    // DEST[127:32]. Same store shape as MOVD xmm, r/m32 for the mem form.
    case Opcode::MOVSS_XMM_XMMM32: {
        u8 dst_xmm = op0.reg & 15;
        const u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (is_register_operand(op1.type)) {
            const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            if (fits_ldr_w_offset(off_d) && fits_ldr_w_offset(off_s)) {
                if (emit_xmm_cache_store_lane0(dst_xmm, op1.reg & 15, false))
                    break;
                emit(arm64::ldr_w_uoff(REG_SCRATCH1, REG_CPU_STATE, off_s >> 2));
                emit(arm64::str_w_uoff(REG_SCRATCH1, REG_CPU_STATE, off_d >> 2));
                break;
            }
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 19));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, op1.reg & 15));
            EMIT_SSE_OP_CALL();
        } else if (fits_ldr_x_offset(off_d) && fits_ldr_x_offset(off_d + 8)) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
            if (xmm_cache_fits(dst_xmm) && fits_ldr_q_offset(off_d)) {
                // FMOV S zeroes bits 127:32, exactly MOVSS's memory form
                // (Intel SDM Vol. 2), and leaves the value cached.
                const u8 vd = static_cast<u8>(kXmmCacheBase + dst_xmm);
                emit(arm64::fmov_s_from_w(vd, REG_SCRATCH0));
                emit(arm64::str_q_uoff(vd, REG_CPU_STATE, off_d >> 4));
                xmm_cache_publish(dst_xmm);
                m_xmm_cache_aware = true;
                break;
            }
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d >> 3));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, (off_d + 8) >> 3));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 200));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- MOVSS m32, xmm — store low 32 bits to memory ----
    case Opcode::MOVSS_XMMM32_XMM: {
        if (is_register_operand(op0.type)) {
            // reg-reg form: dst.u32_[0] = src.u32_[0]
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 19));
            emit(arm64::movz_w(3, op0.reg & 15));
            emit(arm64::movz_w(4, op1.reg & 15));
            EMIT_SSE_OP_CALL();
        } else {
            emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            u8 src_xmm = op1.reg & 15;
            u32 off = xmm_u64_off(src_xmm, 0);
            emit(arm64::ldr_w_uoff(REG_SCRATCH1, REG_CPU_STATE, off >> 2));
            emit_guest_store32(REG_SCRATCH1, REG_SCRATCH4);
        }
        break;
    }

    // ---- MOVSD xmm, xmm/m64 — load low 64 from src; mem zero-extends ----
    case Opcode::MOVSD_XMM_XMMM64: {
        u8 dst_xmm = op0.reg & 15;
        if (is_register_operand(op1.type)) {
            // reg-reg: dst.u64_[0] = src.u64_[0]; dst.u64_[1] preserved
            u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            u32 off_d = xmm_u64_off(dst_xmm, 0);
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_s >> 3));
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_d >> 3));
        } else {
            // mem load: dst.u64_[0] = mem; dst.u64_[1] = 0
            // emit_guest_load32 clobbers x3,x4,x8,x9 inline AND x0-x17 via
            // its slow-path BLR (caller-saved per AArch64 PCS). Preserve
            // address in a callee-saved register (REG_CALL_TMP = x21).
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH4));
            u32 off_lo = xmm_u64_off(dst_xmm, 0);
            u32 off_hi = xmm_u64_off(dst_xmm, 1);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH4);
            emit(arm64::str_w_uoff(REG_SCRATCH1, REG_CPU_STATE, off_lo >> 2));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 4));
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH4);
            emit(arm64::str_w_uoff(REG_SCRATCH1, REG_CPU_STATE, (off_lo + 4) >> 2));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE, off_hi >> 3));
        }
        break;
    }

    // ---- MOVSD m64, xmm — store low 64 bits to memory (or reg-reg) ----
    case Opcode::MOVSD_XMMM64_XMM: {
        if (is_register_operand(op0.type)) {
            u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            u32 off_d = xmm_u64_off(op0.reg & 15, 0);
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_s >> 3));
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_d >> 3));
        } else {
            if (!emit_xmm_qword_store(op1.reg & 15, 0, instr, op0, next_rip))
                return false;
        }
        break;
    }

    // ---- MOVMSKPS r32, xmm — gather sign bit of each S lane ----
    case Opcode::MOVMSKPS_R32_XMM: {
        u8 src_xmm = op1.reg & 15;
        u32 off_s = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(off_s)) return false;
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        emit(arm64::ushr_v_imm(0, 1, 31, arm64::VS_S));   // each S = 0 or 1 (low bit = sign)
        emit(arm64::umov_w_s(REG_SCRATCH0, 0, 0));        // W0 = lane0
        emit(arm64::umov_w_s(REG_SCRATCH3, 0, 1));
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3, 1));
        emit(arm64::umov_w_s(REG_SCRATCH3, 0, 2));
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3, 2));
        emit(arm64::umov_w_s(REG_SCRATCH3, 0, 3));
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3, 3));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- MOVMSKPD r32, xmm — gather sign bit of each D lane ----
    case Opcode::MOVMSKPD_R32_XMM: {
        u8 src_xmm = op1.reg & 15;
        u32 off_s = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(off_s)) return false;
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        emit(arm64::ushr_v_imm(0, 1, 63, arm64::VS_D));   // each D = 0 or 1
        emit(arm64::umov_w_s(REG_SCRATCH0, 0, 0));        // S0 of D[0] holds the bit
        emit(arm64::umov_w_s(REG_SCRATCH3, 0, 2));        // S2 of D[1] holds the bit
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3, 1));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // Intel SDM Vol. 2 MASKMOVDQU: store xmm1 bytes to DS:[EDI]/[RDI] where
    // the MSB of the corresponding xmm2 byte is 1. EDI/RDI is not incremented.
    // Same byte-mask store loop as MASKMOVQ (16 lanes). Prefix fallback
    // matches maskmov_ds_edi_address in the interpreter.
    case Opcode::MASKMOVDQU_XMM_XMM: {
        if (op0.type != OperandType::Reg128 || op1.type != OperandType::Reg128)
            return false;
        if (instr.prefixes.address_size) return false;
        if (instr.prefixes.segment != SegReg::Default) return false;

        const u8 data_xmm = op0.reg & 15;
        const u8 mask_xmm = op1.reg & 15;
        const u32 data_lo = xmm_u64_off(data_xmm, 0);
        const u32 data_hi = xmm_u64_off(data_xmm, 1);
        const u32 mask_lo = xmm_u64_off(mask_xmm, 0);
        const u32 mask_hi = xmm_u64_off(mask_xmm, 1);
        if (!fits_ldr_x_offset(data_lo) || !fits_ldr_x_offset(data_hi)
            || !fits_ldr_x_offset(mask_lo) || !fits_ldr_x_offset(mask_hi)) {
            return false;
        }

        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        const u8 rdi = static_cast<u8>(RegIndex::RDI);
        for (int lane = 0; lane < 16; ++lane) {
            const u32 mask_off = (lane < 8) ? mask_lo : mask_hi;
            const u32 data_off = (lane < 8) ? data_lo : data_hi;
            const u8 bit_in_qword = static_cast<u8>((lane & 7) * 8 + 7);
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, mask_off >> 3));
            const u32 tbz_idx = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, data_off >> 3));
            const u8 shift = static_cast<u8>((lane & 7) * 8);
            if (shift != 0) {
                emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0, shift));
            }
            if (m_block_is_64bit)
                emit_load_gpr64(REG_SCRATCH2, rdi);
            else
                emit_load_gpr32(REG_SCRATCH2, rdi);
            if (lane != 0) {
                emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2,
                                      static_cast<u32>(lane)));
            }
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
            const u32 skip = static_cast<u32>(m_code_buf.size());
            m_code_buf[tbz_idx] = arm64::tbz(
                REG_SCRATCH1, bit_in_qword,
                static_cast<i32>(skip - tbz_idx) * 4);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
