// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: SSE packed, shuffle, crypto and
// conversion fast paths. PADDD, XORPS/XORPD/PXOR, PSRLDQ, PALIGNR,
// PCLMULQDQ, SHA1*, BLENDPD, PMOVMSKB, PSHUFB, PSHUFD and
// CVTSI2SS/CVTSI2SD.
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
std::optional<bool> JitCompiler::compile_sse64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // SSE — register-only fast paths for hot SHA-NI / packed ops.
    // Memory operands fall back to the delegate (32-bit EA fast-path would
    // truncate >4GB addresses).
    // ========================================================================

    // PADDD xmm, xmm/m128 — pure NEON ADD.4S. Memory source loads both
    // qwords through the long-mode EA path (same shape as XORPS mem).
    // VEX.128 VPADDD: DEST := SRC1(vvvv) + SRC2 (Intel SDM Vol. 2 PADDD).
    case Opcode::PADDD_XMM_XMMM128: {
        if (instr.prefixes.vex_l) return false;
        u8 dst_xmm = op0.reg & 15;
        u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15)
                                        : dst_xmm;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        u32 off_1 = xmm_u64_off(src1_xmm, 0);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) return false;
        if (emit_xmm_cache_binop(instr, op1, next_rip,
                                 arm64::add_v(0, 0, 1, arm64::VS_S), dst_xmm,
                                 src1_xmm, false))
            break;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else if (is_mem(op1)) {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned) {
                // One checked 16-byte access, not two 8-byte ones. The
                // alignment rule is the one just emitted above.
                emit_linear64_load128(1, REG_SCRATCH2, /*aligned16=*/false);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
            } else {
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
            }
        } else {
            return false;
        }
        emit(arm64::add_v(0, 0, 1, arm64::VS_S));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // XORPS / XORPD / PXOR xmm, xmm/m128 — all three legacy encodings have
    // the same guest-visible operation: bitwise XOR of all 128 bits, with no
    // EFLAGS effect. The packed element type does not alter the operation
    // (Intel SDM Vol. 2D, XORPS/XORPD; Vol. 2C, PXOR).
    // VEX.128: DEST := SRC1(vvvv) XOR SRC2 (Intel SDM Vol. 2 VPXOR/VXORPS).
    case Opcode::XORPS_XMM_XMMM128:
    case Opcode::XORPD_XMM_XMMM128:
    case Opcode::PXOR_XMM_XMMM128: {
        if (instr.prefixes.vex_l) return false;
        const u8 dst_xmm = op0.reg & 15;
        const u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15)
                                              : dst_xmm;
        const u32 dst_off = xmm_u64_off(dst_xmm, 0);
        const u32 src1_off = xmm_u64_off(src1_xmm, 0);
        if (!fits_ldr_q_offset(dst_off) || !fits_ldr_q_offset(src1_off))
            return false;
        if (emit_xmm_cache_binop(instr, op1, next_rip, arm64::eor_v16b(0, 0, 1),
                                 dst_xmm, src1_xmm, /*xor_zero_idiom=*/true))
            break;
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_q_offset(src_off)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src1_off >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, src_off >> 4));
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            // Legacy SSE requires a 16-byte ALIGNED 128-bit memory operand.
            // Measured on silicon, not read off the manual:
            // tools/oracle_probe/sse_align_probe.c, recorded output beside it.
            // VEX is exempt -- unaligned unless the opcode says otherwise.
            // This long-mode lowering shadows the `EMIT_SSE_NEON_RR` one in
            // ops/sse.cpp, so fixing that macro alone left 64-bit blocks
            // silently permissive.
            //
            // `m_current_original_vex` is NOT redundant with
            // `instr.prefixes.vex`: compile_sse's VEX.128 bridge recursively
            // presents a LEGACY-SHAPED instruction to this lowering with
            // `prefixes.vex` already cleared, so testing that field alone
            // reports a VEX instruction as legacy and raises #GP on an operand
            // VEX explicitly permits to be unaligned. That is not theoretical:
            // it is a deterministic `VPXOR xmm2, xmm1, [rdi]` fault at
            // image+0x16ddc with rdi 8-byte aligned, inside AES-NI/PCLMULQDQ
            // startup, which kills the process before it can spawn its UI.
            const bool rm_aligned =
                !instr.prefixes.vex && !m_current_original_vex;
            if (rm_aligned)
                emit_align_check(REG_SCRATCH2, false);
            if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned) {
                // One checked 16-byte access, not two 8-byte ones.
                emit_linear64_load128(1, REG_SCRATCH2, rm_aligned);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src1_off >> 4));
                emit(arm64::eor_v16b(0, 0, 1));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
                break;
            }
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            // The checked loads call C helpers, so preserve the first half in
            // callee-saved X21 rather than a caller-saved vector register.
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src1_off >> 4));
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        emit(arm64::eor_v16b(0, 0, 1));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
        break;
    }

    // PSRLDQ xmm, imm8 — shift the full 128-bit value right by imm8 bytes,
    // filling the high bytes with zero. Counts greater than 15 produce zero;
    // count zero is a no-op. EFLAGS are unchanged (Intel SDM Vol. 2C,
    // PSLLDQ/PSRLDQ). ARM EXT over {old, zero} has the same byte ordering.
    case Opcode::PSRLDQ_XMM_IMM8: {
        const u8 dst_xmm = op0.reg & 15;
        const u8 count = static_cast<u8>(op1.imm);
        if (count == 0) break;
        const u32 dst_off = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(dst_off)) return false;
        if (count > 15) {
            emit(arm64::eor_v16b(0, 0, 0));
        } else {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
            emit(arm64::eor_v16b(1, 1, 1));
            emit(arm64::ext_v16b(0, 0, 1, count));
        }
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
        break;
    }

    // PALIGNR xmm, xmm/m128, imm8 — concat-shift-extract via EXT.
    case Opcode::PALIGNR_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        if (imm >= 32) {
            emit(arm64::eor_v16b(0, 0, 0));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            break;
        }
        if (imm == 16) break;  // dst unchanged
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            if (imm < 16) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
                emit(arm64::ext_v16b(0, 1, 0, imm));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            } else {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                emit(arm64::eor_v16b(2, 2, 2));
                emit(arm64::ext_v16b(0, 0, 2, static_cast<u8>(imm - 16)));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            }
        } else if (is_mem(op1)) {
            if (imm < 16) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                emit(arm64::ext_v16b(0, 1, 0, imm));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            } else {
                // imm in 17..31: the result is the DESTINATION shifted right
                // with zero fill, so the source contributes nothing and the
                // arithmetic below never reads it. x86 still ACCESSES the
                // memory operand, though -- eliding the read also elides its
                // #GP on a misaligned address and its #PF on an unmapped one,
                // so a guest that would have faulted runs on instead. Perform
                // the access and discard it.
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                emit(arm64::eor_v16b(2, 2, 2));
                emit(arm64::ext_v16b(0, 0, 2, static_cast<u8>(imm - 16)));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            }
        } else {
            return false;
        }
        break;
    }

    // SHA256RNDS2 / SHA256MSG1 / SHA256MSG2 — invoke jit_sse_op helper.
    // Register-source only (mem variant needs 64-bit EA path).
    // The SHA arms that used to live here have been deleted.
    //
    // compile_sse owns them, is already in the 64-bit dispatch chain, and its
    // versions are strictly better: this copy declined the memory form
    // outright (`if (!is_register_operand(op1.type)) return false;`) while
    // compile_sse handles it, and both carried the same emit_sha*_direct
    // logic, so a fix to one silently missed the other.
    // PCLMULQDQ xmm, xmm, imm8 — register form is independent of guest
    // address size in long mode. Intel SDM Vol. 2B, PCLMULQDQ selects one
    // 64-bit lane from each XMM operand using imm8[0] and imm8[4], then
    // writes the 128-bit carry-less product to the destination. ARM PMULL
    // implements that same polynomial product directly.
    case Opcode::PCLMULQDQ_XMM_XMMM128_IMM8: {
        if (!is_register_operand(op1.type)) return false;
        const u8 dst_xmm = op0.reg & 15;
        const u8 src_xmm = op1.reg & 15;
        const u8 imm8 = static_cast<u8>(instr.operands[2].imm);
        const u32 dst_off = xmm_u64_off(dst_xmm, 0);
        const u32 src_off = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(dst_off) || !fits_ldr_q_offset(src_off))
            return false;
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

    // SHA1NEXTE / SHA1MSG1 / SHA1MSG2 / SHA1RNDS4 — native on FEAT_SHA1
    // (emit_sha1_native). They were a C++ helper call each: ~3.4M calls/s
    // on a 64-bit title menu. The memory forms read one aligned m128 (SDM
    // exception type 4: legacy SSE, 16-byte alignment).
    case Opcode::SHA1NEXTE_XMM_XMMM128:
    case Opcode::SHA1MSG1_XMM_XMMM128:
    case Opcode::SHA1MSG2_XMM_XMMM128:
    case Opcode::SHA1RNDS4_XMM_XMMM128_IMM8: {
        const u8 dst_xmm = op0.reg & 15;
        if (!fits_ldr_q_offset(xmm_u64_off(dst_xmm, 0))) return false;
        const u8 imm8 = instr.opcode == Opcode::SHA1RNDS4_XMM_XMMM128_IMM8
            ? static_cast<u8>(instr.operands[2].imm) : u8{0};
        if (is_register_operand(op1.type)) {
            const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else if (is_mem(op1) && m_cfg.linear64 && m_cfg.linear64_vec128 &&
                   m_linear_base_pinned) {
            const u8 ea = emit_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_align_check(ea, /*align32=*/false);
            emit_linear64_load128(1, ea, /*aligned16=*/true);
        } else {
            return false;
        }
        emit_sha1_native(instr.opcode, dst_xmm, imm8);
        break;
    }

    // ========================================================================
    // BLENDPD xmm, xmm/m128, imm8 — per-lane select of packed doubles:
    //   dst.q[i] = (imm & (1<<i)) ? src.q[i] : dst.q[i],  i = 0,1.
    // Only the selected lanes are written, so it reduces to 64-bit lane
    // copies (register src) or 64-bit loads from [EA + i*8] (memory src).
    // No EFLAGS effect.
    // ========================================================================
    case Opcode::BLENDPD_XMM_XMMM128_IMM8: {
        u8 imm = static_cast<u8>(instr.operands[2].imm) & 3u;
        if (imm == 0) break;                       // no lanes selected
        u8 dst_xmm = op0.reg & 15;
        u32 off_d0 = xmm_u64_off(dst_xmm, 0);
        u32 off_d1 = xmm_u64_off(dst_xmm, 1);
        if (!fits_ldr_x_offset(off_d0) || !fits_ldr_x_offset(off_d1))
            return false;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s0 = xmm_u64_off(src_xmm, 0);
            u32 off_s1 = xmm_u64_off(src_xmm, 1);
            if (!fits_ldr_x_offset(off_s0) || !fits_ldr_x_offset(off_s1))
                return false;
            if (imm & 1) {
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_s0 >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d0 >> 3));
            }
            if (imm & 2) {
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_s1 >> 3));
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d1 >> 3));
            }
        } else {
            if (imm & 1) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d0 >> 3));
            }
            if (imm & 2) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
                emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8u));  // EA+8
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_d1 >> 3));
            }
        }
        break;
    }

    // PMOVMSKB r32,xmm / VPMOVMSKB r32,ymm — Intel SDM Vol. 2, "PMOVMSKB".
    // Extract each byte's MSB into the corresponding result bit. VEX.256
    // writes a 32-bit mask (high lane in bits [31:16]); r32 zeros [63:32].
    case Opcode::PMOVMSKB_R32_XMM: {
        const u8 src_xmm = op1.reg & 15;
        const u32 off_s = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_x_offset(off_s) || !fits_ldr_x_offset(off_s + 8))
            return false;
        const bool ymm = instr.prefixes.vex_l;
        u32 off_h = 0;
        if (ymm) {
            off_h = ymm_high_u64_off(src_xmm, 0);
            if (!fits_ldr_x_offset(off_h) || !fits_ldr_x_offset(off_h + 8))
                return false;
        }
        auto emit_mask16 = [&](u32 off) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                   (off + 8) >> 3));
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
        };
        emit_mask16(off_s);
        if (ymm) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_mask16(off_h);
            emit(arm64::orr_x_lsl(REG_SCRATCH0, REG_CALL_TMP,
                                  REG_SCRATCH0, 16));
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // PSHUFB xmm, xmm/m128 — Intel SDM Vol. 2, "PSHUFB". Each mask byte's
    // low nibble selects from the original destination, while mask bit 7
    // zeros that output byte. ARM TBL already zeros indices >= 16, so ANDing
    // the mask with 0x8F maps both behaviors exactly. Memory masks load both
    // qwords through the long-mode checked-read path.
    case Opcode::PSHUFB_XMM_XMMM128: {
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        if (is_register_operand(op1.type)) {
            u8 mask_xmm = op1.reg & 15;
            u32 off_m = xmm_u64_off(mask_xmm, 0);
            if (!fits_ldr_q_offset(off_m)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_d >> 4));
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, off_m >> 4));
        } else if (is_mem(op1)) {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_d >> 4));
            emit(arm64::ins_v_gpr(2, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(2, 1, REG_SCRATCH0, arm64::VS_D));
        } else {
            return false;
        }
        emit(arm64::movi_v16b(3, 0x8F));
        emit(arm64::and_v16b(2, 2, 3));
        emit(arm64::tbl_v16b_1(0, 1, 2));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ========================================================================
    // PSHUFD xmm, xmm/m128, imm8 — dst.d[i] = src.d[(imm>>(2i))&3].
    // All four source dwords are read before any destination write so
    // src==dst aliasing is safe. Memory sources assemble a temporary V1.
    // No EFLAGS effect.
    // ========================================================================
    case Opcode::PSHUFD_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        // XMM register cache: shuffle register to register. The source is
        // read whole before the result is written, so a destination that is
        // also the source is built in V0 and its cache entry dropped.
        const bool reg_src = is_register_operand(op1.type);
        const bool mem_linear = is_mem(op1) && m_cfg.linear64 &&
                                m_cfg.linear64_vec128 && m_linear_base_pinned;
        if ((reg_src && xmm_cache_fits(dst_xmm, op1.reg & 15)) ||
            (mem_linear && xmm_cache_fits(dst_xmm))) {
            u8 vs = 1;
            if (reg_src) {
                vs = xmm_cache_source(static_cast<u8>(op1.reg & 15));
            } else {
                const bool aligned =
                    !instr.prefixes.vex && !m_current_original_vex;
                const u8 ea = emit_ea64(REG_SCRATCH2, op1.mem, next_rip);
                if (aligned) emit_align_check(ea, false);
                emit_linear64_load128(1, ea, aligned);
            }
            const u8 vd = static_cast<u8>(kXmmCacheBase + dst_xmm);
            const u8 t = (reg_src && vs == vd) ? 0 : vd;
            u8 sel[4];
            u32 moved = 0;
            for (u8 i = 0; i < 4; ++i) {
                sel[i] = static_cast<u8>((imm >> (2 * i)) & 3);
                moved += sel[i] != i ? 1u : 0u;
            }
            if (sel[0] == sel[1] && sel[1] == sel[2] && sel[2] == sel[3]) {
                emit(arm64::dup_v_elem_s(t, vs, sel[0]));
            } else if (moved < 4) {
                // Copy, then move only the lanes that change.
                emit(arm64::mov_v16b(t, vs));
                for (u8 i = 0; i < 4; ++i) {
                    if (sel[i] != i)
                        emit(arm64::ins_v_elem(t, i, vs, sel[i], arm64::VS_S));
                }
            } else {
                for (u8 i = 0; i < 4; ++i)
                    emit(arm64::ins_v_elem(t, i, vs, sel[i], arm64::VS_S));
            }
            emit(arm64::str_q_uoff(t, REG_CPU_STATE, off_d >> 4));
            if (t == vd) {
                xmm_cache_publish(dst_xmm);
            } else {
                m_xmm_cache = static_cast<u16>(m_xmm_cache & ~(1u << dst_xmm));
            }
            m_xmm_cache_aware = true;
            break;
        }
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_w_offset(off_s + 12) || !fits_ldr_w_offset(off_d + 12))
                return false;
            u8 s0 = (imm >> 0) & 3, s1 = (imm >> 2) & 3,
               s2 = (imm >> 4) & 3, s3 = (imm >> 6) & 3;
            emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, (off_s + s0 * 4u) >> 2));
            emit(arm64::ldr_w_uoff(REG_SCRATCH1, REG_CPU_STATE, (off_s + s1 * 4u) >> 2));
            emit(arm64::ldr_w_uoff(REG_SCRATCH3, REG_CPU_STATE, (off_s + s2 * 4u) >> 2));
            emit(arm64::ldr_w_uoff(REG_SCRATCH4, REG_CPU_STATE, (off_s + s3 * 4u) >> 2));
            emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, (off_d + 0u) >> 2));
            emit(arm64::str_w_uoff(REG_SCRATCH1, REG_CPU_STATE, (off_d + 4u) >> 2));
            emit(arm64::str_w_uoff(REG_SCRATCH3, REG_CPU_STATE, (off_d + 8u) >> 2));
            emit(arm64::str_w_uoff(REG_SCRATCH4, REG_CPU_STATE, (off_d + 12u) >> 2));
        } else if (is_mem(op1)) {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            // Legacy SSE requires a 16-byte ALIGNED 128-bit memory operand;
            // measured on silicon (tools/oracle_probe/sse_align_probe.c).
            // This long-mode lowering shadows the one in ops/sse.cpp, which
            // is why fixing that file alone left 64-bit blocks permissive.
            // Same VEX-bridge rule as the bitwise trio above: the bridge
            // clears `prefixes.vex`, so `m_current_original_vex` is what
            // actually distinguishes VPSHUFD from PSHUFD here.
            if (!instr.prefixes.vex && !m_current_original_vex)
                emit_align_check(REG_SCRATCH2, false);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
            for (int i = 0; i < 4; ++i) {
                u8 lane = static_cast<u8>((imm >> (2 * i)) & 3);
                emit(arm64::ins_v_elem(0, static_cast<u8>(i),
                                       1, lane, arm64::VS_S));
            }
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            return false;
        }
        break;
    }

    // CVTSI2SS xmm, r/m64 — signed i64 → scalar single in lane 0.
    case Opcode::CVTSI2SS_XMM_RM64: {
        const u8 dst_xmm = op0.reg & 15;
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH2, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op1);
            emit_guest_load64(REG_SCRATCH2, REG_SCRATCH3);
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm32(1, dst_xmm);
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_cvtsi2ss_r64)));
        emit_helper_blr();
        break;
    }

    // CVTSI2SD xmm, r/m32 — signed i32 → scalar double in low qword.
    case Opcode::CVTSI2SD_XMM_RM32: {
        const u8 dst_xmm = op0.reg & 15;
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH2, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op1);
            emit_guest_load32_64(REG_SCRATCH2, REG_SCRATCH3);
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm32(1, dst_xmm);
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_cvtsi2sd_r32)));
        emit_helper_blr();
        break;
    }

    // CVTSI2SD xmm, r/m64 — signed i64 → scalar double in low qword.
    case Opcode::CVTSI2SD_XMM_RM64: {
        const u8 dst_xmm = op0.reg & 15;
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH2, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op1);
            emit_guest_load64(REG_SCRATCH2, REG_SCRATCH3);
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm32(1, dst_xmm);
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_cvtsi2sd_r64)));
        emit_helper_blr();
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
