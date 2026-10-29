// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: XMM data moves. MOVSD, MOVD
// r/m32,xmm, MOVLPS/MOVLPD/MOVHPS/MOVHPD, the MOVDQA/MOVDQU/MOVAPS/...
// loads and stores (including the VEX.256 arms that ops64.cpp's
// vex256_move_lowered_natively relies on), and MOVQ.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_sse_move64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // MOVSD xmm,xmm/m64 and xmm/m64,xmm — the legacy scalar move copies the
    // low qword. A register destination preserves its upper qword for a
    // register source but clears it for a memory source; a memory destination
    // writes exactly eight bytes. EFLAGS are unchanged (Intel SDM Vol. 2,
    // MOVSD).
    case Opcode::MOVSD_XMM_XMMM64: {
        const u32 dst_off = xmm_u64_off(op0.reg & 15, 0);
        if (!fits_ldr_x_offset(dst_off) ||
            !fits_ldr_x_offset(dst_off + 8)) return false;
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) return false;
            if (emit_xmm_cache_store_lane0(op0.reg & 15, op1.reg & 15, true))
                break;
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   src_off >> 3));
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   dst_off >> 3));
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            const u8 dst_xmm = op0.reg & 15;
            if (xmm_cache_fits(dst_xmm) && fits_ldr_q_offset(dst_off)) {
                // FMOV D zeroes bits 127:64, exactly the memory form's
                // upper-qword clear, and leaves the value cached.
                const u8 vd = static_cast<u8>(kXmmCacheBase + dst_xmm);
                emit(arm64::fmov_d_from_x(vd, REG_SCRATCH0));
                emit(arm64::str_q_uoff(vd, REG_CPU_STATE, dst_off >> 4));
                xmm_cache_publish(dst_xmm);
                m_xmm_cache_aware = true;
                break;
            }
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   dst_off >> 3));
            emit(arm64::str_x_uoff(31, REG_CPU_STATE,
                                   (dst_off + 8) >> 3));
        }
        break;
    }
    case Opcode::MOVSD_XMMM64_XMM: {
        const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
        if (!fits_ldr_x_offset(src_off)) return false;
        if (is_register_operand(op0.type) &&
            emit_xmm_cache_store_lane0(op0.reg & 15, op1.reg & 15, true))
            break;
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                               src_off >> 3));
        if (is_register_operand(op0.type)) {
            const u32 dst_off = xmm_u64_off(op0.reg & 15, 0);
            if (!fits_ldr_x_offset(dst_off)) return false;
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   dst_off >> 3));
        } else {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_CALL_TMP, REG_SCRATCH2);
        }
        break;
    }

    // MOVD r/m32, xmm — copy the low doubleword of the source XMM. A GPR
    // destination is an architectural 32-bit write and therefore clears its
    // upper 32 bits in long mode; a memory destination writes exactly four
    // bytes through the full-width effective address. EFLAGS are unchanged.
    // Intel SDM Vol. 2B, MOVD/MOVQ.
    case Opcode::MOVD_RM32_XMM: {
        const u8 src_xmm = op1.reg & 15;
        const u32 src_off = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_w_offset(src_off)) return false;
        emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 2));
        if (is_register_operand(op0.type)) {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    // ========================================================================
    // MOVLPS/MOVLPD/MOVHPS/MOVHPD xmm, m64 — load 64 bits from guest memory
    // into one 64-bit lane of an XMM register, preserving the other lane.
    //   MOVLPS/MOVLPD: low lane (xmm[63:0]).   MOVHPS/MOVHPD: high lane.
    // These were a top self-extract fallback (MOVLPS_XMM_M64 ~8%). The
    // existing SSE fast paths skip memory operands; here we use the 64-bit-
    // address-safe emit_guest_load64 so addresses above 4 GB are handled.
    // No EFLAGS effect.
    // ========================================================================
    case Opcode::MOVLPS_XMM_M64:
    case Opcode::MOVLPD_XMM_M64:
    case Opcode::MOVHPS_XMM_M64:
    case Opcode::MOVHPD_XMM_M64: {
        if (!is_mem(op1)) return false;
        const bool high = (instr.opcode == Opcode::MOVHPS_XMM_M64 ||
                           instr.opcode == Opcode::MOVHPD_XMM_M64);
        u8 dst_xmm = op0.reg & 15;
        u32 off = xmm_u64_off(dst_xmm, high ? 1u : 0u);
        if (!fits_ldr_x_offset(off)) return false;
        emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);     // X = [mem]
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
        break;
    }

    // ========================================================================
    // MOVDQA/MOVDQU xmm, xmm/m128 — 128-bit integer move. MOVDQU has no
    // alignment requirement. MOVDQA / MOVAPS / MOVAPD require 16-byte
    // alignment (#GP otherwise) and use jit_require_align16 on memory forms.
    // No EFLAGS effect. Intel SDM Vol. 1 §3.3.7: long-mode EA is 64-bit.
    // ========================================================================
    // MOVAPS/MOVUPS/MOVAPD/MOVUPD are bit-for-bit identical 128-bit moves to
    // MOVDQA/MOVDQU (packed-float vs packed-int is irrelevant for a pure move).
    case Opcode::MOVDQA_XMM_XMMM128:
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::LDDQU_XMM_M128:
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVUPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128:
    case Opcode::MOVUPD_XMM_XMMM128: {
        // load: xmm/ymm(op0) <- xmm/m128 or ymm/m256(op1)
        u8 dst_xmm = op0.reg & 15;
        if (instr.prefixes.vex_l) {
            // Intel SDM Vol. 2 VMOVDQU/VMOVDQA (VEX.256):
            // DEST[255:0] := SRC[255:0]. High half lives in ymm_high.
            const u32 off_d0 = xmm_u64_off(dst_xmm, 0);
            const u32 off_h0 = ymm_high_u64_off(dst_xmm, 0);
            if (!fits_ldr_x_offset(off_d0) || !fits_ldr_x_offset(off_d0 + 8) ||
                !fits_ldr_x_offset(off_h0) || !fits_ldr_x_offset(off_h0 + 8))
                return false;
            if (is_register_operand(op1.type)) {
                const u8 src_xmm = op1.reg & 15;
                const u32 off_s0 = xmm_u64_off(src_xmm, 0);
                const u32 off_sh0 = ymm_high_u64_off(src_xmm, 0);
                if (!fits_ldr_x_offset(off_s0) || !fits_ldr_x_offset(off_s0 + 8) ||
                    !fits_ldr_x_offset(off_sh0) || !fits_ldr_x_offset(off_sh0 + 8))
                    return false;
                // ymm := ymm as two 16-byte Q moves (four words, was eight).
                if (fits_ldr_q_offset(off_s0) && fits_ldr_q_offset(off_sh0) &&
                    fits_ldr_q_offset(off_d0) && fits_ldr_q_offset(off_h0)) {
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s0 >> 4));
                    emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_sh0 >> 4));
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d0 >> 4));
                    emit(arm64::str_q_uoff(1, REG_CPU_STATE, off_h0 >> 4));
                    break;
                }
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_s0 >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d0 >> 3));
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_s0 + 8) >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_d0 + 8) >> 3));
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_sh0 >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_h0 >> 3));
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_sh0 + 8) >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_h0 + 8) >> 3));
            } else {
                const bool checked_aligned =
                    instr.opcode == Opcode::MOVDQA_XMM_XMMM128 ||
                    instr.opcode == Opcode::MOVAPS_XMM_XMMM128 ||
                    instr.opcode == Opcode::MOVAPD_XMM_XMMM128;
                if (checked_aligned) {
                    emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                    emit_align_check(REG_SCRATCH2, /*align32=*/true);
                }
                // One guarded 32-byte guest-base access instead of four
                // guarded 8-byte ones (and four EA recomputes).
                if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned &&
                    fits_ldr_q_offset(off_d0) && fits_ldr_q_offset(off_h0)) {
                    emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                    emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                    emit_linear64_load256(0, 1, REG_SCRATCH2, off_d0, off_h0);
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d0 >> 4));
                    emit(arm64::str_q_uoff(1, REG_CPU_STATE, off_h0 >> 4));
                    break;
                }
                const u32 dest_off[4] = {off_d0, off_d0 + 8, off_h0, off_h0 + 8};
                for (u32 i = 0; i < 4; ++i) {
                    emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                    emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                    if (i != 0) {
                        emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, i * 8u));
                    }
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                    emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                           dest_off[i] >> 3));
                }
            }
            break;
        }
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_s))
                return false;
            if (emit_xmm_cache_copy(dst_xmm, src_xmm)) break;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            // Intel SDM Vol. 1 §3.3.7: long-mode EA is 64-bit. Returning
            // false here used to fall through to compile_sse, whose 128-bit
            // fast path zero-extends through W and aliases 0x14000xxxx onto
            // 0x4000xxxx. Keep #GP-on-misalign via jit_require_align16.
            const bool checked_aligned =
                instr.opcode == Opcode::MOVDQA_XMM_XMMM128 ||
                instr.opcode == Opcode::MOVAPS_XMM_XMMM128 ||
                instr.opcode == Opcode::MOVAPD_XMM_XMMM128;
            if (!fits_ldr_x_offset(off_d) || !fits_ldr_x_offset(off_d + 8))
                return false;
            // One guarded 16-byte guest-base access instead of two guarded
            // 8-byte ones (and two EA recomputes). It is also closer to the
            // architectural contract: x86 does not update the destination
            // register when the second half of a 128-bit load faults, and the
            // split form writes xmm[63:0] before the fault on [addr+8].
            // The EA is formed once (a lone pinned base is used in place) and
            // checked once; a 16-byte-aligned access cannot cross a host
            // page, so the guard drops its crossing test.
            if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned &&
                fits_ldr_q_offset(off_d)) {
                const u8 ea = emit_ea64(REG_SCRATCH2, op1.mem, next_rip);
                if (checked_aligned) {
                    emit_align_check(ea, /*align32=*/false);
                } else {
                    emit_m128_align_check_if_required(ea, instr, op1);
                }
                if (xmm_cache_fits(dst_xmm)) {
                    // Straight into the destination's cache register.
                    const u8 vd = static_cast<u8>(kXmmCacheBase + dst_xmm);
                    emit_linear64_load128(vd, ea, checked_aligned);
                    emit(arm64::str_q_uoff(vd, REG_CPU_STATE, off_d >> 4));
                    xmm_cache_publish(dst_xmm);
                    m_xmm_cache_aware = true;
                    break;
                }
                emit_linear64_load128(0, ea, /*aligned16=*/checked_aligned);
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                break;
            }
            if (checked_aligned) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_align_check(REG_SCRATCH2, /*align32=*/false);
            }
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d >> 3));
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8u));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, (off_d + 8) >> 3));
        }
        break;
    }

    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVUPD_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    case Opcode::MOVNTDQ_M128_XMM: {
        // store: xmm/m128 or ymm/m256(op0) <- xmm/ymm(op1)
        u8 src_xmm = op1.reg & 15;
        if (instr.prefixes.vex_l) {
            // Intel SDM Vol. 2 VMOVDQU/VMOVDQA store-form (VEX.256):
            // DEST[255:0] := SRC[255:0].
            const u32 off_s0 = xmm_u64_off(src_xmm, 0);
            const u32 off_sh0 = ymm_high_u64_off(src_xmm, 0);
            if (!fits_ldr_x_offset(off_s0) || !fits_ldr_x_offset(off_s0 + 8) ||
                !fits_ldr_x_offset(off_sh0) || !fits_ldr_x_offset(off_sh0 + 8))
                return false;
            if (is_register_operand(op0.type)) {
                const u8 dst_xmm = op0.reg & 15;
                const u32 off_d0 = xmm_u64_off(dst_xmm, 0);
                const u32 off_h0 = ymm_high_u64_off(dst_xmm, 0);
                if (!fits_ldr_x_offset(off_d0) || !fits_ldr_x_offset(off_d0 + 8) ||
                    !fits_ldr_x_offset(off_h0) || !fits_ldr_x_offset(off_h0 + 8))
                    return false;
                if (fits_ldr_q_offset(off_s0) && fits_ldr_q_offset(off_sh0) &&
                    fits_ldr_q_offset(off_d0) && fits_ldr_q_offset(off_h0)) {
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s0 >> 4));
                    emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_sh0 >> 4));
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d0 >> 4));
                    emit(arm64::str_q_uoff(1, REG_CPU_STATE, off_h0 >> 4));
                    break;
                }
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_s0 >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d0 >> 3));
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_s0 + 8) >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_d0 + 8) >> 3));
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_sh0 >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_h0 >> 3));
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_sh0 + 8) >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (off_h0 + 8) >> 3));
            } else {
                const bool checked_aligned =
                    instr.opcode == Opcode::MOVDQA_XMMM128_XMM ||
                    instr.opcode == Opcode::MOVAPS_XMMM128_XMM ||
                    instr.opcode == Opcode::MOVAPD_XMMM128_XMM ||
                    instr.opcode == Opcode::MOVNTPS_M128_XMM ||
                    instr.opcode == Opcode::MOVNTPD_M128_XMM ||
                    instr.opcode == Opcode::MOVNTDQ_M128_XMM;
                if (checked_aligned) {
                    emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
                    emit_align_check(REG_SCRATCH2, /*align32=*/true);
                }
                // One guarded 32-byte guest-base store; see the load form.
                if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned &&
                    fits_ldr_q_offset(off_s0) && fits_ldr_q_offset(off_sh0)) {
                    emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
                    emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s0 >> 4));
                    emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_sh0 >> 4));
                    emit_linear64_store256(0, 1, REG_SCRATCH2, off_s0, off_sh0);
                    break;
                }
                // One validated 32-byte write: four 8-byte stores performed
                // the pieces before a denied one, and Windows leaves every
                // byte of a faulting store untouched (tools/oracle_probe/
                // simd_store_fault_probe.expected).
                const bool hold = should_hold_nzcv();
                if (hold) emit_nzcv_spill();
                emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
                emit_vec_store_span(REG_SCRATCH2, off_s0, off_sh0, 32,
                                    next_rip - instr.length);
                if (hold) emit_nzcv_fill();
            }
            break;
        }
        u32 off_s = xmm_u64_off(src_xmm, 0);
        if (is_register_operand(op0.type)) {
            u8 dst_xmm = op0.reg & 15;
            u32 off_d = xmm_u64_off(dst_xmm, 0);
            if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_s))
                return false;
            if (emit_xmm_cache_copy(dst_xmm, src_xmm)) break;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            const bool checked_aligned =
                instr.opcode == Opcode::MOVDQA_XMMM128_XMM ||
                instr.opcode == Opcode::MOVAPS_XMMM128_XMM ||
                instr.opcode == Opcode::MOVAPD_XMMM128_XMM ||
                instr.opcode == Opcode::MOVNTPS_M128_XMM ||
                instr.opcode == Opcode::MOVNTPD_M128_XMM ||
                instr.opcode == Opcode::MOVNTDQ_M128_XMM;
            if (!fits_ldr_x_offset(off_s) || !fits_ldr_x_offset(off_s + 8))
                return false;
            // One guarded 16-byte guest-base store; see the load form above.
            // The write-watch and SMC obligations are identical and are
            // enforced inside emit_linear64_store128. EA formed and checked
            // once, as in the load form.
            if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned &&
                fits_ldr_q_offset(off_s)) {
                const u8 ea = emit_ea64(REG_SCRATCH2, op0.mem, next_rip);
                if (checked_aligned) {
                    emit_align_check(ea, /*align32=*/false);
                } else {
                    emit_m128_align_check_if_required(ea, instr, op0);
                }
                if (xmm_cache_fits(src_xmm)) {
                    const u8 vs = xmm_cache_source(src_xmm);
                    emit_linear64_store128(vs, ea, checked_aligned);
                    m_xmm_cache_aware = true;  // writes no XMM
                    break;
                }
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
                emit_linear64_store128(0, ea, /*aligned16=*/checked_aligned);
                break;
            }
            if (checked_aligned) {
                emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
                emit_align_check(REG_SCRATCH2, /*align32=*/false);
            }
            // One validated 16-byte write; two 8-byte stores performed the
            // first half of a store whose second half is denied.
            const bool hold = should_hold_nzcv();
            if (hold) emit_nzcv_spill();
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_vec_store_span(REG_SCRATCH2, off_s, 0, 16,
                                next_rip - instr.length);
            if (hold) emit_nzcv_fill();
        }
        break;
    }

    // MOVQ xmm, r/m64 — zero the upper 64 bits of the XMM. Flag-neutral.
    case Opcode::MOVQ_XMM_RM64: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 dst_off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_x_offset(dst_off) || !fits_ldr_x_offset(dst_off + 8))
            return false;
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, dst_off >> 3));
        emit(arm64::movz_x(REG_SCRATCH1, 0, 0));
        emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, (dst_off + 8) >> 3));
        break;
    }

    // MOVQ r/m64, xmm — copy the low qword. GPR dest is a full 64-bit write.
    case Opcode::MOVQ_RM64_XMM: {
        const u8 src_xmm = op1.reg & 15;
        const u32 src_off = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_x_offset(src_off)) return false;
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        if (is_register_operand(op0.type)) {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
