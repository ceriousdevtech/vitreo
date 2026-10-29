// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — shuffles, unpacks and blends: PSHUFB, PSHUFD/PSHUFLW/
// PSHUFHW/SHUFPS/SHUFPD, PUNPCK*/UNPCK*, PALIGNR, BLENDPS/BLENDPD/PBLENDW
// and PBLENDVB/BLENDVPS/BLENDVPD.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_shuffle(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- SSSE3 PSHUFB — native NEON TBL reg-reg, helper for mem (254/255).
    // x86: dst[i] = (mask[i]&0x80) ? 0 : dst[mask[i]&0x0F]
    // ARM TBL: dst[i] = (idx[i]>=16) ? 0 : src[idx[i]]
    // Translate: idx = mask AND 0x8F; bytes with bit7 set become 0x80..0x8F (>=16).
    case Opcode::PSHUFB_XMM_XMMM128: {
        u8 dst_xmm = op0.reg & 15;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_d = xmm_u64_off(dst_xmm, 0);
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_s)) {
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_d >> 4)); // V1 = data (orig dst)
                emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, off_s >> 4)); // V2 = mask
                emit(arm64::movi_v16b(3, 0x8F));
                emit(arm64::and_v16b(2, 2, 3));                        // V2 &= 0x8F
                emit(arm64::tbl_v16b_1(0, 1, 2));                      // V0 = TBL(V1, V2)
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                break;
            }
        }
        // Fallback to helper.
        if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 254));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, static_cast<u16>(op1.reg & 15)));
            EMIT_SSE_OP_CALL();
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 255));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- Shuffles (op_ids 140-149) ----
    // PSHUFD / SHUFPS / SHUFPD / PSHUFLW / PSHUFHW: native NEON via per-lane
    // INS for register and memory sources. Helper op_ids stay as fallback.
    case Opcode::PSHUFD_XMM_XMMM128_IMM8:
    case Opcode::PSHUFLW_XMM_XMMM128_IMM8:
    case Opcode::PSHUFHW_XMM_XMMM128_IMM8:
    case Opcode::SHUFPS_XMM_XMMM128_IMM8:
    case Opcode::SHUFPD_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u8 imm8 = static_cast<u8>(instr.operands[2].imm);
        u16 base_opid;
        switch (instr.opcode) {
        case Opcode::PSHUFD_XMM_XMMM128_IMM8:  base_opid = 140; break;
        case Opcode::PSHUFLW_XMM_XMMM128_IMM8: base_opid = 142; break;
        case Opcode::PSHUFHW_XMM_XMMM128_IMM8: base_opid = 144; break;
        case Opcode::SHUFPS_XMM_XMMM128_IMM8:  base_opid = 146; break;
        case Opcode::SHUFPD_XMM_XMMM128_IMM8:  base_opid = 148; break;
        default: return false;
        }

        // vec_off(): under the VEX.256 lane split (SHUFPS/SHUFPD only) the
        // second pass names YMM[255:128] of every register. SHUFPS/SHUFPD are
        // lane-local (Intel SDM Vol. 2 VSHUFPS/VSHUFPD: "DEST[255:128] <-
        // shuffle of SRC1[255:128], SRC2[255:128]"), and the driver hands the
        // high pass imm8 already adjusted by vex256_high_lane_imm.
        const u32 off_d = vec_off(dst_xmm);
        // SRC1 comes from vvvv for a VEX form (VSHUFPS: DEST := shuffle of
        // SRC1, SRC2). The VEX.128 bridge clears prefixes.vex after its merge
        // copy, so only the lane split reaches here with vex still set.
        const bool shuf_fp =
            instr.opcode == Opcode::SHUFPS_XMM_XMMM128_IMM8
            || instr.opcode == Opcode::SHUFPD_XMM_XMMM128_IMM8;
        const u8 src1_xmm = (shuf_fp && instr.prefixes.vex)
            ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
        const u32 off_1 = vec_off(src1_xmm);
        const bool native_op =
            instr.opcode == Opcode::PSHUFD_XMM_XMMM128_IMM8
            || instr.opcode == Opcode::SHUFPS_XMM_XMMM128_IMM8
            || instr.opcode == Opcode::SHUFPD_XMM_XMMM128_IMM8
            || instr.opcode == Opcode::PSHUFLW_XMM_XMMM128_IMM8
            || instr.opcode == Opcode::PSHUFHW_XMM_XMMM128_IMM8;
        bool src_ready = false;
        // XMM register cache: the sources are read from their cache
        // registers. The result is still built in V0 -- lane-by-lane INS in
        // place would read a lane it already overwrote when the destination
        // is a source -- so the destination's entry is dropped after.
        const bool cached = xmm_cache_fits(
            dst_xmm, src1_xmm,
            is_register_operand(op1.type) ? (op1.reg & 15) : 0);
        u8 vs = 1;  // the r/m source
        if (native_op && fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_1)) {
            if (is_register_operand(op1.type)) {
                const u32 off_s = vec_off(op1.reg & 15);
                if (fits_ldr_q_offset(off_s)) {
                    if (cached) {
                        vs = xmm_cache_source(static_cast<u8>(op1.reg & 15));
                    } else {
                        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
                    }
                    src_ready = true;
                }
            } else {
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                /* 128-bit memory operand: legacy SSE requires 16-byte
                 * alignment, measured on silicon -- see the note in
                 * EMIT_SSE_NEON_RR. VEX is exempt. */
                const bool rm_aligned =
                    !instr.prefixes.vex && !m_current_original_vex;
                if (rm_aligned)
                    emit_align_check(REG_CALL_TMP, false);
                if (m_block_is_64bit && m_cfg.linear64 &&
                    m_cfg.linear64_vec128 && m_linear_base_pinned) {
                    // One checked 16-byte access, not two 8-byte ones.
                    emit_linear64_load128(1, REG_CALL_TMP, rm_aligned);
                } else {
                    emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                    emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                    emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                    emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
                }
                src_ready = true;
            }
        }
        if (src_ready) {
            // vs = src. For SHUFPS/SHUFPD also SRC1 (V2, or its cache
            // register), read after the source so every aliasing is safe.
            const auto src1_reg = [&]() -> u8 {
                if (cached) return xmm_cache_source(src1_xmm);
                emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, off_1 >> 4));
                return 2;
            };
            if (instr.opcode == Opcode::PSHUFD_XMM_XMMM128_IMM8) {
                    // V0.S[i] = src.S[(imm8 >> (2*i)) & 3]
                    for (int i = 0; i < 4; ++i) {
                        u8 lane = static_cast<u8>((imm8 >> (2 * i)) & 3);
                        emit(arm64::ins_v_elem(0, static_cast<u8>(i),
                                               vs, lane, arm64::VS_S));
                    }
                } else if (instr.opcode == Opcode::SHUFPS_XMM_XMMM128_IMM8) {
                    // V0 (dst) lanes 0,1 from SRC1; lanes 2,3 from src.
                    const u8 va = src1_reg();
                    emit(arm64::ins_v_elem(0, 0, va,
                                           static_cast<u8>(imm8 & 3),
                                           arm64::VS_S));
                    emit(arm64::ins_v_elem(0, 1, va,
                                           static_cast<u8>((imm8 >> 2) & 3),
                                           arm64::VS_S));
                    emit(arm64::ins_v_elem(0, 2, vs,
                                           static_cast<u8>((imm8 >> 4) & 3),
                                           arm64::VS_S));
                    emit(arm64::ins_v_elem(0, 3, vs,
                                           static_cast<u8>((imm8 >> 6) & 3),
                                           arm64::VS_S));
                } else if (instr.opcode == Opcode::SHUFPD_XMM_XMMM128_IMM8) {
                    // SHUFPD imm2: V0.D[0] from SRC1[imm&1];
                    //              V0.D[1] from src[(imm>>1)&1].
                    const u8 va = src1_reg();
                    emit(arm64::ins_v_elem(0, 0, va,
                                           static_cast<u8>(imm8 & 1),
                                           arm64::VS_D));
                    emit(arm64::ins_v_elem(0, 1, vs,
                                           static_cast<u8>((imm8 >> 1) & 1),
                                           arm64::VS_D));
                } else {
                    // PSHUFLW/PSHUFHW: V0 = src (copy), then shuffle 4 halves
                    // in the low (imm_off=0) or high (imm_off=4) 64 bits.
                    emit(arm64::mov_v16b(0, vs));
                    u8 off_lane = (instr.opcode
                        == Opcode::PSHUFHW_XMM_XMMM128_IMM8) ? 4 : 0;
                    for (int i = 0; i < 4; ++i) {
                        u8 lane = static_cast<u8>((imm8 >> (2 * i)) & 3);
                        emit(arm64::ins_v_elem(
                            0, static_cast<u8>(off_lane + i),
                            vs, static_cast<u8>(off_lane + lane),
                            arm64::VS_H));
                    }
                }
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                if (cached) {
                    m_xmm_cache = static_cast<u16>(
                        m_xmm_cache & ~(1u << dst_xmm));
                    m_xmm_cache_aware = true;
                }
                break;
        }

        // The helper takes register INDICES, which always mean xmm[], and
        // computes from xmm[dst] rather than vvvv: it cannot express a lane
        // pass or a VEX SRC1. Nothing has been emitted yet on this path.
        if (m_vec_lane_split || instr.prefixes.vex) return false;
        if (is_register_operand(op1.type)) {
            u16 packed_src = static_cast<u16>((op1.reg & 15) | (imm8 << 8));
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, base_opid));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, packed_src));
            EMIT_SSE_OP_CALL();
        } else {
            u16 packed_dst = static_cast<u16>(dst_xmm | (imm8 << 8));
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            /* 128-bit memory operand: legacy SSE requires 16-byte
             * alignment, measured on silicon -- see the note in
             * EMIT_SSE_NEON_RR. VEX is exempt. */
            if (!instr.prefixes.vex && !m_current_original_vex)
                emit_align_check(REG_SCRATCH4, false);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, static_cast<u16>(base_opid + 1)));
            emit(arm64::movz_w(3, packed_dst));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- PUNPCK*/UNPCK* — native NEON ZIP1/ZIP2 for reg and mem ----
    case Opcode::PUNPCKLBW_XMM_XMMM128:
    case Opcode::PUNPCKHBW_XMM_XMMM128:
    case Opcode::PUNPCKLWD_XMM_XMMM128:
    case Opcode::PUNPCKHWD_XMM_XMMM128:
    case Opcode::PUNPCKLDQ_XMM_XMMM128:
    case Opcode::PUNPCKHDQ_XMM_XMMM128:
    case Opcode::PUNPCKLQDQ_XMM_XMMM128:
    case Opcode::PUNPCKHQDQ_XMM_XMMM128:
    case Opcode::UNPCKLPS_XMM_XMMM128:
    case Opcode::UNPCKHPS_XMM_XMMM128:
    case Opcode::UNPCKLPD_XMM_XMMM128:
    case Opcode::UNPCKHPD_XMM_XMMM128: {
        u8 dst_xmm = op0.reg & 15;
        u16 base_opid;
        bool hi;           // ZIP2 variant
        arm64::VSize vsz;
        switch (instr.opcode) {
        case Opcode::PUNPCKLBW_XMM_XMMM128:  base_opid = 150; hi = false; vsz = arm64::VS_B; break;
        case Opcode::PUNPCKHBW_XMM_XMMM128:  base_opid = 152; hi = true;  vsz = arm64::VS_B; break;
        case Opcode::PUNPCKLWD_XMM_XMMM128:  base_opid = 154; hi = false; vsz = arm64::VS_H; break;
        case Opcode::PUNPCKHWD_XMM_XMMM128:  base_opid = 156; hi = true;  vsz = arm64::VS_H; break;
        case Opcode::PUNPCKLDQ_XMM_XMMM128:  base_opid = 158; hi = false; vsz = arm64::VS_S; break;
        case Opcode::PUNPCKHDQ_XMM_XMMM128:  base_opid = 160; hi = true;  vsz = arm64::VS_S; break;
        case Opcode::PUNPCKLQDQ_XMM_XMMM128: base_opid = 162; hi = false; vsz = arm64::VS_D; break;
        case Opcode::PUNPCKHQDQ_XMM_XMMM128: base_opid = 164; hi = true;  vsz = arm64::VS_D; break;
        case Opcode::UNPCKLPS_XMM_XMMM128:   base_opid = 166; hi = false; vsz = arm64::VS_S; break;
        case Opcode::UNPCKHPS_XMM_XMMM128:   base_opid = 168; hi = true;  vsz = arm64::VS_S; break;
        case Opcode::UNPCKLPD_XMM_XMMM128:   base_opid = 162; hi = false; vsz = arm64::VS_D; break;
        case Opcode::UNPCKHPD_XMM_XMMM128:   base_opid = 164; hi = true;  vsz = arm64::VS_D; break;
        default: return false;
        }
        u32 off_d = vec_off(dst_xmm);
        // SRC1 is vvvv for a VEX form -- in BOTH operand shapes. The memory
        // branch used to read it from the destination, which the VEX.128
        // bridge's merge copy hid and the lane split (no merge) would not.
        u32 off_a = vec_off(instr.prefixes.vex
            ? instr.prefixes.vex_vvvv : dst_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_a)) return false;
        const u8 src1_xmm = static_cast<u8>(
            (instr.prefixes.vex ? instr.prefixes.vex_vvvv : dst_xmm) & 15);
        const u32 zip = hi ? arm64::zip2_v(0, 0, 1, vsz)
                           : arm64::zip1_v(0, 0, 1, vsz);
        const bool rm_aligned = !instr.prefixes.vex && !m_current_original_vex;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = vec_off(src_xmm);
            if (!fits_ldr_q_offset(off_s)) return false;
            if (emit_xmm_cache_binop(instr, op1, next_rip, zip, dst_xmm,
                                     src1_xmm, false))
                break;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_a >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
            emit(hi ? arm64::zip2_v(0, 0, 1, vsz)
                    : arm64::zip1_v(0, 0, 1, vsz));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            // Park EA in X21: emit_guest_load64 / x64 region load clobbers
            // X4 (and may BLR). Low qword then reuses X21.
            if (emit_xmm_cache_binop(instr, op1, next_rip, zip, dst_xmm,
                                     src1_xmm, false))
                break;
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            /* 128-bit memory operand: legacy SSE requires 16-byte
             * alignment, measured on silicon -- see the note in
             * EMIT_SSE_NEON_RR. VEX is exempt. */
            if (rm_aligned)
                emit_align_check(REG_CALL_TMP, false);
            if (m_block_is_64bit && m_cfg.linear64 && m_cfg.linear64_vec128 &&
                m_linear_base_pinned) {
                // One checked 16-byte access, not two 8-byte ones.
                emit_linear64_load128(1, REG_CALL_TMP, rm_aligned);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_a >> 4));
            } else {
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_a >> 4));
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
            }
            emit(zip);
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        }
        (void)base_opid;
        break;
    }

    // ---- PALIGNR xmm, xmm/m128, imm8 — concat-shift-extract ----
    case Opcode::PALIGNR_XMM_XMMM128_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else if (m_block_is_64bit) {
            return false;
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH4, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        if (imm >= 32) {
            // Result = 0.
            emit(arm64::eor_v16b(0, 0, 0));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            break;
        }
        if (imm == 16) {
            // Result = dst unchanged.
            break;
        }
        if (imm < 16) {
            // EXT Vd.16B, Vsrc, Vdst, #imm.
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            emit(arm64::ext_v16b(0, 1, 0, imm));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            // imm in (16..32): result = dst >> (imm-16) bytes (high zeros).
            // EXT Vd.16B, Vdst, Vzero, #(imm-16).
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            emit(arm64::eor_v16b(2, 2, 2));
            emit(arm64::ext_v16b(0, 0, 2, static_cast<u8>(imm - 16)));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        }
        break;
    }

    // ---- BLENDPS / BLENDPD / PBLENDW xmm, xmm/m128, imm8 ----
    // mask V2: lane = all-ones if imm bit set, else 0; BSL V2,Vsrc,Vdst.
    case Opcode::BLENDPS_XMM_XMMM128_IMM8:
    case Opcode::BLENDPD_XMM_XMMM128_IMM8:
    case Opcode::PBLENDW_XMM_XMMM128_IMM8: {
        if (instr.prefixes.vex_l) return false;
        u8 dst_xmm = op0.reg & 15;
        // SRC1 from vvvv (VEX) and vec_off(): under the VEX.256 lane split
        // VPBLENDW uses the same imm8 for both lanes and VBLENDPS/PD get
        // the high lane's selector bits from vex256_high_lane_imm.
        const u8 src1_xmm = instr.prefixes.vex
            ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
        u32 off_d = vec_off(dst_xmm);
        const u32 off_1 = vec_off(src1_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) return false;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = vec_off(src_xmm);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // EA parked in X21: emit_guest_load64 / the x64 region load
            // clobbers X4 and may BLR. This arm used to decline every
            // memory form in long mode (`vpblendw ymm, ymm, [mem]`).
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 lane_count;
        u8 imm_mask;
        arm64::VSize vsz;
        switch (instr.opcode) {
        case Opcode::BLENDPS_XMM_XMMM128_IMM8: lane_count = 4; imm_mask = 0x0F; vsz = arm64::VS_S; break;
        case Opcode::BLENDPD_XMM_XMMM128_IMM8: lane_count = 2; imm_mask = 0x03; vsz = arm64::VS_D; break;
        case Opcode::PBLENDW_XMM_XMMM128_IMM8: lane_count = 8; imm_mask = 0xFF; vsz = arm64::VS_H; break;
        default: return false;
        }
        u8 eff_imm = imm & imm_mask;
        if (eff_imm == 0) {
            // Result = SRC1: a no-op for the legacy form, a copy for VEX.
            if (off_1 != off_d) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            }
            break;
        }
        if (eff_imm == imm_mask) {
            // Result = src.
            emit(arm64::str_q_uoff(1, REG_CPU_STATE, off_d >> 4));
            break;
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
        if (instr.opcode == Opcode::PBLENDW_XMM_XMMM128_IMM8) {
            // Modified-immediate forms materialize the two alternating word
            // masks in one instruction instead of MOVI plus four dependent
            // lane clears. Other masks retain the generic exact lowering.
            if (eff_imm == 0x55) {
                emit(arm64::movi_v4s_ff_msl8(2));
                emit(arm64::bsl_v16b(2, 1, 0));
                emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_d >> 4));
                break;
            }
            if (eff_imm == 0xAA) {
                emit(arm64::mvni_v4s_ff_msl8(2));
                emit(arm64::bsl_v16b(2, 1, 0));
                emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_d >> 4));
                break;
            }
        }
        // Build mask V2: all ones, then zero lanes where bit clear.
        emit(arm64::movi_v16b(2, 0xFF));
        for (u8 i = 0; i < lane_count; ++i) {
            if ((eff_imm & (1u << i)) == 0) {
                // INS V2.<T>[i], W31 (WZR) — zeros that lane.
                emit(arm64::ins_v_gpr(2, i, 31, vsz));
            }
        }
        // BSL V2, V1, V0: V2 = (V2 & V1) | (~V2 & V0) = src where mask, dst else.
        emit(arm64::bsl_v16b(2, 1, 0));
        emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // Intel SDM Vol. 2 PBLENDVB / BLENDVPS / BLENDVPD: implicit XMM0 mask.
    // Per-lane MSB (byte / dword / qword) selects src vs dest. CMLT #0
    // expands that sign bit to an all-ones BSL mask.
    case Opcode::PBLENDVB_XMM_XMMM128:
    case Opcode::BLENDVPS_XMM_XMMM128:
    case Opcode::BLENDVPD_XMM_XMMM128: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_d = xmm_u64_off(dst_xmm, 0);
        const u32 off_m = xmm_u64_off(0, 0);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_m)) {
            return false;
        }
        arm64::VSize vsz;
        switch (instr.opcode) {
        case Opcode::PBLENDVB_XMM_XMMM128: vsz = arm64::VS_B; break;
        case Opcode::BLENDVPS_XMM_XMMM128: vsz = arm64::VS_S; break;
        default: vsz = arm64::VS_D; break;
        }
        if (is_register_operand(op1.type)) {
            const u8 src_xmm = op1.reg & 15;
            const u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // Two 64-bit guest loads. A helper BLR clobbers V0–V7 and
            // caller-saved GPRs, so park EA then the low qword in X21
            // and reload dst only after both loads complete.
            if (!m_block_is_64bit) emit_update_rip(instr_rip);
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, off_m >> 4));
        emit(arm64::cmlt_v_zero(3, 2, vsz));
        emit(arm64::bsl_v16b(3, 1, 0));
        emit(arm64::str_q_uoff(3, REG_CPU_STATE, off_d >> 4));
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
