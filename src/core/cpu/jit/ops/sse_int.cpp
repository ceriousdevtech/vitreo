// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — packed integer and bitwise NEON lowerings: PXOR/PAND/POR/
// PANDN and the FP bitwise aliases, PADD*/PSUB*, PCMPEQ/PCMPGT, the SSE4.1/
// 4.2 compare/multiply/min-max forms, PMOVSX/PMOVZX, the multiplies,
// PMIN/PMAX/PAVG/PSADBW and PADDUS/PSUBUS.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_int(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- Bitwise (integer) — native NEON for register and memory sources ----
    // NEON_ENC operates on V0 (src1) and V1 (src2). Legacy SSE loads src1 from
    // dest; VEX.128 NDS loads src1 from vvvv (Intel SDM Vol. 2 VPAND/VPADDD:
    // DEST := SRC1 OP SRC2). VEX.256 reaches this only via the lane-split
    // driver, which re-emits it once per 128-bit lane with vec_off() naming
    // the half being written.
    #define EMIT_SSE_NEON_RR(NEON_ENC, REG_OPID, MEM_OPID) \
        { \
            if (instr.prefixes.vex_l) return false; \
            u8 dst_xmm = op0.reg & 15; \
            u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15) \
                                            : dst_xmm; \
            if (is_register_operand(op1.type)) { \
                u8 src_xmm = op1.reg & 15; \
                u32 off_d = vec_off(dst_xmm); \
                u32 off_s = vec_off(src_xmm); \
                u32 off_1 = vec_off(src1_xmm); \
                if (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_s) \
                    && fits_ldr_q_offset(off_1)) { \
                    /* XMM register cache; XOR of a register with itself \
                     * is the zero idiom. */ \
                    if (emit_xmm_cache_binop(instr, op1, next_rip, (NEON_ENC), \
                            dst_xmm, src1_xmm, \
                            instr.opcode == Opcode::PXOR_XMM_XMMM128 \
                            || instr.opcode == Opcode::XORPS_XMM_XMMM128 \
                            || instr.opcode == Opcode::XORPD_XMM_XMMM128)) \
                        break; \
                    /* Intel SDM Vol. 2 PXOR/XORPS/XORPD: legacy register \
                     * self-XOR is an exception-free all-zero result. PE32 \
                     * can therefore skip both architectural operand loads. \
                     * Keep VEX/NDS, lane-split, and long-mode shapes intact. */ \
                    const bool pe32_xor_self = \
                        !m_block_is_64bit && !m_current_original_vex \
                        && !m_vec_lane_split && src_xmm == dst_xmm \
                        && (instr.opcode == Opcode::PXOR_XMM_XMMM128 \
                            || instr.opcode == Opcode::XORPS_XMM_XMMM128 \
                            || instr.opcode == Opcode::XORPD_XMM_XMMM128); \
                    /* One-instruction write-through forwarding: an \
                     * immediately prior whitelisted RR lowering left its \
                     * stored result in V0.  Preserve the established PE32 \
                     * set; long mode initially admits only the measured \
                     * repeated-PADDQ shape.  VEX/lane-split stay excluded. */ \
                    const bool v0_forward_enabled = \
                        !m_current_original_vex && !m_vec_lane_split \
                        && (!m_block_is_64bit \
                            || instr.opcode == Opcode::PADDQ_XMM_XMMM128); \
                    if (!pe32_xor_self) { \
                        if (!v0_forward_enabled \
                            || m_xmm_v0_forward_in != src1_xmm) { \
                            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, \
                                                  off_1 >> 4)); \
                        } \
                        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4)); \
                    } \
                    emit(pe32_xor_self ? arm64::eor_v16b(0, 0, 0) \
                                       : (NEON_ENC)); \
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
                    if (v0_forward_enabled) { \
                        m_xmm_v0_forward_out = dst_xmm; \
                    } \
                    break; \
                } \
                /* jit_sse_op and emit_sse_reg_copy take REGISTER INDICES and \
                 * always mean xmm[]; they cannot express the high lane. The \
                 * SRC1 staging copy would also destroy an r/m source that is \
                 * the destination. Nothing has been emitted on this path. */ \
                if (m_vec_hi_lane) return false; \
                if (src1_xmm != dst_xmm && src_xmm == dst_xmm) return false; \
                if (src1_xmm != dst_xmm) { \
                    emit_sse_reg_copy(dst_xmm, src1_xmm, 0); \
                } \
                emit(arm64::mov_x(0, REG_CPU_STATE)); \
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3)); \
                emit(arm64::movz_w(2, REG_OPID)); \
                emit(arm64::movz_w(3, dst_xmm)); \
                emit(arm64::movz_w(4, src_xmm)); \
                EMIT_SSE_OP_CALL(); \
            } else { \
                u32 off_d = vec_off(dst_xmm); \
                u32 off_1 = vec_off(src1_xmm); \
                if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) \
                    return false; \
                /* Legacy forms need a 16-byte-aligned operand (the \
                 * silicon measurement quoted below); VEX forms do not. */ \
                const bool rm_aligned = \
                    !instr.prefixes.vex && !m_current_original_vex; \
                const bool rm_linear128 = m_block_is_64bit && m_cfg.linear64 \
                    && m_cfg.linear64_vec128 && m_linear_base_pinned; \
                if (emit_xmm_cache_binop(instr, op1, next_rip, (NEON_ENC), \
                                         dst_xmm, src1_xmm, false)) \
                    break; \
                /* Park EA in X21: emit_guest_load64 / x64 region load \
                 * clobbers X4 (and may BLR). Low qword then reuses X21. \
                 * Replaces the prior jit_sse_op dual-BLR path that lost the \
                 * high half of long-mode addresses (PCMPEQW #AV in a \
                 * 64-bit guest). */ \
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip); \
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1); \
                /* Legacy SSE requires a 16-byte ALIGNED memory operand on \
                 * every one of these forms, not only the MOVAPS and MOVDQA \
                 * copies. Measured on silicon rather than read off the \
                 * manual: tools/oracle_probe/sse_align_probe.c misaligns \
                 * the operand by one and reports which forms raise. On a \
                 * real Alder Lake core XORPS, XORPD, PXOR, ANDPS, PAND, \
                 * ORPS and POR all fault exactly as MOVAPS and MOVDQA do, \
                 * while MOVUPS and MOVDQU do not. This path had no check, \
                 * so a misaligned operand silently succeeded. VEX forms are \
                 * exempt -- unaligned unless the opcode says otherwise \
                 * (Intel SDM Vol. 2), the same rule avx.cpp follows. */ \
                if (rm_aligned) \
                    emit_align_check(REG_CALL_TMP, false); \
                if (rm_linear128) { \
                    /* One checked 16-byte access, not two 8-byte ones. */ \
                    emit_linear64_load128(1, REG_CALL_TMP, rm_aligned); \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                } else { \
                    emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP); \
                    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8)); \
                    emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0)); \
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4); \
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4)); \
                    emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D)); \
                    emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D)); \
                } \
                emit(NEON_ENC); \
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4)); \
            } \
            break; \
        }

    case Opcode::PXOR_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::eor_v16b(0, 0, 1), 20, 21)
    case Opcode::PAND_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::and_v16b(0, 0, 1), 22, 23)
    case Opcode::POR_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::orr_v16b(0, 0, 1), 24, 25)
    // PANDN: x86 dst = ~dst & src; ARM BIC Vd,Vn,Vm = Vn & ~Vm.
    // So BIC V0,V1,V0 = src & ~dst = PANDN result in V0.
    case Opcode::PANDN_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::bic_v16b(0, 1, 0), 26, 27)

    // ---- Bitwise (floating-point alias) — same NEON encodings ----
    case Opcode::XORPS_XMM_XMMM128:
    case Opcode::XORPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::eor_v16b(0, 0, 1), 88, 89)
    case Opcode::ANDPS_XMM_XMMM128:
    case Opcode::ANDPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::and_v16b(0, 0, 1), 90, 91)
    case Opcode::ORPS_XMM_XMMM128:
    case Opcode::ORPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::orr_v16b(0, 0, 1), 92, 93)
    case Opcode::ANDNPS_XMM_XMMM128:
    case Opcode::ANDNPD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::bic_v16b(0, 1, 0), 94, 95)

    // ---- Packed add ----
    case Opcode::PADDB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::add_v(0, 0, 1, arm64::VS_B), 28, 29)
    case Opcode::PADDW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::add_v(0, 0, 1, arm64::VS_H), 30, 31)
    case Opcode::PADDD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::add_v(0, 0, 1, arm64::VS_S), 32, 33)
    case Opcode::PADDQ_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::add_v(0, 0, 1, arm64::VS_D), 34, 35)

    // ---- Packed sub ----
    case Opcode::PSUBB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::sub_v(0, 0, 1, arm64::VS_B), 36, 37)
    case Opcode::PSUBW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::sub_v(0, 0, 1, arm64::VS_H), 38, 39)
    case Opcode::PSUBD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::sub_v(0, 0, 1, arm64::VS_S), 40, 41)
    case Opcode::PSUBQ_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::sub_v(0, 0, 1, arm64::VS_D), 42, 43)

    // ---- Comparison ----
    case Opcode::PCMPEQB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmeq_v(0, 0, 1, arm64::VS_B), 46, 47)
    case Opcode::PCMPEQW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmeq_v(0, 0, 1, arm64::VS_H), 48, 49)
    case Opcode::PCMPEQD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmeq_v(0, 0, 1, arm64::VS_S), 50, 51)
    case Opcode::PCMPGTB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmgt_v(0, 0, 1, arm64::VS_B), 52, 53)
    case Opcode::PCMPGTW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmgt_v(0, 0, 1, arm64::VS_H), 54, 55)
    case Opcode::PCMPGTD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmgt_v(0, 0, 1, arm64::VS_S), 56, 57)

    // ---- SSE4.1/4.2 integer compare/multiply/min-max (native NEON) ----
    case Opcode::PCMPEQQ_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmeq_v(0, 0, 1, arm64::VS_D), 330, 331)
    case Opcode::PCMPGTQ_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::cmgt_v(0, 0, 1, arm64::VS_D), 332, 333)
    case Opcode::PMULLD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::mul_v(0, 0, 1, arm64::VS_S), 334, 335)
    case Opcode::PMINSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::smin_v(0, 0, 1, arm64::VS_B), 336, 337)
    case Opcode::PMINSD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::smin_v(0, 0, 1, arm64::VS_S), 338, 339)
    case Opcode::PMINUW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::umin_v(0, 0, 1, arm64::VS_H), 340, 341)
    case Opcode::PMINUD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::umin_v(0, 0, 1, arm64::VS_S), 342, 343)
    case Opcode::PMAXSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::smax_v(0, 0, 1, arm64::VS_B), 344, 345)
    case Opcode::PMAXSD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::smax_v(0, 0, 1, arm64::VS_S), 346, 347)
    case Opcode::PMAXUW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::umax_v(0, 0, 1, arm64::VS_H), 348, 349)
    case Opcode::PMAXUD_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::umax_v(0, 0, 1, arm64::VS_S), 350, 351)

    // ---- PMOVSX / PMOVZX — native NEON via SXTL/UXTL chain (reg-reg);
    // helper for mem form (op_ids 360-383). 12 ops: 6 SX + 6 ZX variants.
    case Opcode::PMOVSXBW_XMM_XMMM64:
    case Opcode::PMOVSXBD_XMM_XMMM32:
    case Opcode::PMOVSXBQ_XMM_XMMM16:
    case Opcode::PMOVSXWD_XMM_XMMM64:
    case Opcode::PMOVSXWQ_XMM_XMMM32:
    case Opcode::PMOVSXDQ_XMM_XMMM64:
    case Opcode::PMOVZXBW_XMM_XMMM64:
    case Opcode::PMOVZXBD_XMM_XMMM32:
    case Opcode::PMOVZXBQ_XMM_XMMM16:
    case Opcode::PMOVZXWD_XMM_XMMM64:
    case Opcode::PMOVZXWQ_XMM_XMMM32:
    case Opcode::PMOVZXDQ_XMM_XMMM64: {
        u8 dst_xmm = op0.reg & 15;
        bool is_signed;
        u32 kind;   // 0=BW,1=BD,2=BQ,3=WD,4=WQ,5=DQ
        switch (instr.opcode) {
        case Opcode::PMOVSXBW_XMM_XMMM64: is_signed=true;  kind=0; break;
        case Opcode::PMOVSXBD_XMM_XMMM32: is_signed=true;  kind=1; break;
        case Opcode::PMOVSXBQ_XMM_XMMM16: is_signed=true;  kind=2; break;
        case Opcode::PMOVSXWD_XMM_XMMM64: is_signed=true;  kind=3; break;
        case Opcode::PMOVSXWQ_XMM_XMMM32: is_signed=true;  kind=4; break;
        case Opcode::PMOVSXDQ_XMM_XMMM64: is_signed=true;  kind=5; break;
        case Opcode::PMOVZXBW_XMM_XMMM64: is_signed=false; kind=0; break;
        case Opcode::PMOVZXBD_XMM_XMMM32: is_signed=false; kind=1; break;
        case Opcode::PMOVZXBQ_XMM_XMMM16: is_signed=false; kind=2; break;
        case Opcode::PMOVZXWD_XMM_XMMM64: is_signed=false; kind=3; break;
        case Opcode::PMOVZXWQ_XMM_XMMM32: is_signed=false; kind=4; break;
        case Opcode::PMOVZXDQ_XMM_XMMM64: is_signed=false; kind=5; break;
        default: return false;
        }
        // reg_opid = 360 + 2*op_type; mem_opid = 361 + 2*op_type
        u16 op_type = static_cast<u16>((is_signed ? 0 : 6) + kind);
        u16 reg_opid = static_cast<u16>(360 + 2 * op_type);
        u16 mem_opid = static_cast<u16>(reg_opid + 1);

        // Intel SDM Vol. 2 PMOVSX/PMOVZX: the source is the low 8/4/2 bytes
        // of xmm/m64/m32/m16 (BW,WD,DQ / BD,WQ / BQ), widened element-wise.
        // VEX.128 zeroes DEST[MAXVL-1:128]; VEX.256 reads twice as many bytes
        // (xmm/m128/m64/m32) and fills the whole ymm. The memory operand has
        // no alignment requirement at any width. vvvv is 1111b (checked by
        // sse_vex_lowered_directly).
        static constexpr u8 kSrcBytes[6] = {8, 4, 2, 8, 4, 8};
        const bool vex = instr.prefixes.vex;
        const bool wide = vex && instr.prefixes.vex_l;
        const u32 half_bytes = kSrcBytes[kind];
        const u32 src_bytes = wide ? half_bytes * 2 : half_bytes;
        const u32 off_d = xmm_u64_off(dst_xmm, 0);
        const u32 off_dhi = ymm_high_u64_off(dst_xmm, 0);
        const bool native_ok = fits_ldr_q_offset(off_d) &&
            (!vex || fits_ldr_q_offset(off_dhi)) &&
            (!is_register_operand(op1.type) ||
             fits_ldr_q_offset(xmm_u64_off(op1.reg & 15, 0)));
        if (native_ok) {
            if (is_register_operand(op1.type)) {
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE,
                                       xmm_u64_off(op1.reg & 15, 0) >> 4));
            } else if (src_bytes == 16) {
                // Both qwords are read before V1 is formed: guest-load
                // helpers may clobber V0-V7.
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::fmov_d_from_x(1, REG_CALL_TMP));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
            } else {
                emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                if (src_bytes == 8) {
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                    emit(arm64::fmov_d_from_x(1, REG_SCRATCH0));
                } else {
                    if (src_bytes == 4) {
                        emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
                    } else {
                        emit_guest_load16(REG_SCRATCH0, REG_SCRATCH4);
                    }
                    emit(arm64::fmov_s_from_w(1, REG_SCRATCH0));
                }
            }
            // SXTL/UXTL chain from Vn into Vd.
            auto ext = [&](u8 d, u8 n, arm64::VSize dst_sz) {
                emit(is_signed ? arm64::sxtl_v(d, n, dst_sz)
                               : arm64::uxtl_v(d, n, dst_sz));
            };
            auto widen = [&](u8 d, u8 n) {
                switch (kind) {
                case 0: ext(d, n, arm64::VS_H); break;                       // BW
                case 1: ext(d, n, arm64::VS_H); ext(d, d, arm64::VS_S); break; // BD
                case 2: ext(d, n, arm64::VS_H); ext(d, d, arm64::VS_S);
                        ext(d, d, arm64::VS_D); break;                       // BQ
                case 3: ext(d, n, arm64::VS_S); break;                       // WD
                case 4: ext(d, n, arm64::VS_S); ext(d, d, arm64::VS_D); break; // WQ
                case 5: ext(d, n, arm64::VS_D); break;                       // DQ
                }
            };
            widen(0, 1);
            if (wide) {
                emit(arm64::ext_v16b(3, 1, 1, half_bytes));
                widen(2, 3);
            } else if (vex) {
                emit(arm64::movi_v16b(2, 0));
            }
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            if (vex) emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_dhi >> 4));
            break;
        }
        if (vex) return false;

        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, reg_opid));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, src_xmm));
            EMIT_SSE_OP_CALL();
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, mem_opid));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- More multiply ----
    case Opcode::PMULLW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::mul_v(0, 0, 1, arm64::VS_H), 58, 59)
    case Opcode::PMULHW_XMM_XMMM128:
    case Opcode::PMULHUW_XMM_XMMM128: {
        // Intel SDM Vol. 2 PMULHW/PMULHUW: each dest word is the high 16 bits
        // of the 32-bit product. SMULL/UMULL + SMULL2/UMULL2 + UZP2.8H, same
        // pattern as the MMX lowering. Helper op_ids 60-63 stay as fallback.
        if (instr.prefixes.vex_l) return false;
        const bool is_signed = (instr.opcode == Opcode::PMULHW_XMM_XMMM128);
        const u16 reg_opid = is_signed ? 60 : 62;
        const u16 mem_opid = static_cast<u16>(reg_opid + 1);
        const u8 dst_xmm = op0.reg & 15;
        // SRC1 from vvvv (VEX) and vec_off() so the VEX.256 lane split can
        // run this body per lane (Intel SDM Vol. 2 VPMULHW ymm: lane-wise).
        const u8 src1_xmm = instr.prefixes.vex
            ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
        const u32 off_d = vec_off(dst_xmm);
        const u32 off_1 = vec_off(src1_xmm);
        // The helper arms take register indices (xmm[] only) and read SRC1
        // from the destination: neither a lane pass nor a VEX form.
        const bool helper_ok = !m_vec_lane_split && !instr.prefixes.vex;
        auto emit_pmulh_helper = [&](u16 opid, u64 src_or_ea, bool src_is_reg) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, opid));
            emit(arm64::movz_w(3, dst_xmm));
            if (src_is_reg) {
                emit(arm64::movz_w(4, static_cast<u16>(src_or_ea)));
            } else {
                emit(arm64::mov_x(4, src_or_ea));
            }
            EMIT_SSE_OP_CALL();
        };
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) {
            if (!helper_ok) return false;
            if (is_register_operand(op1.type)) {
                emit_pmulh_helper(reg_opid, op1.reg & 15, true);
            } else {
                emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                emit_pmulh_helper(mem_opid, REG_SCRATCH4, false);
            }
            break;
        }
        if (is_register_operand(op1.type)) {
            const u32 off_s = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(off_s)) {
                if (!helper_ok) return false;
                emit_pmulh_helper(reg_opid, op1.reg & 15, true);
                break;
            }
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // Load src first: guest-load helpers may clobber V0-V7.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        if (is_signed) {
            emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
            emit(arm64::smull2_v(3, 0, 1, arm64::VS_H));
        } else {
            emit(arm64::umull_v(2, 0, 1, arm64::VS_H));
            emit(arm64::umull2_v(3, 0, 1, arm64::VS_H));
        }
        emit(arm64::uzp2_v(0, 2, 3, arm64::VS_H));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }
    case Opcode::PMADDWD_XMM_XMMM128: {
        // Intel SDM Vol.2 PMADDWD: each dword is the wrapping sum of two
        // adjacent signed word products. SMULL/SMULL2 + ADDP.4S matches.
        u8 dst_xmm = op0.reg & 15;
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_d = vec_off(dst_xmm);
            u32 off_s = vec_off(src_xmm);
            u32 off_a = vec_off(instr.prefixes.vex
                ? instr.prefixes.vex_vvvv : dst_xmm);
            if (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_s) &&
                fits_ldr_q_offset(off_a)) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_a >> 4));
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
                emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
                emit(arm64::smull2_v(3, 0, 1, arm64::VS_H));
                emit(arm64::addp_v(0, 2, 3, arm64::VS_S));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                break;
            }
            if (m_vec_lane_split) return false;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 64));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::movz_w(4, src_xmm));
            EMIT_SSE_OP_CALL();
        } else {
            const u32 off_d = vec_off(dst_xmm);
            const u32 off_a = vec_off(instr.prefixes.vex
                ? instr.prefixes.vex_vvvv : dst_xmm);
            if (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_a)) {
                // Same NEON as the register form; only the source differs.
                // Load it BEFORE SRC1: emit_guest_load64 can BLR and v0-v7
                // are caller-saved. SRC1 is vvvv under the lane split.
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_a >> 4));
                emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
                emit(arm64::smull2_v(3, 0, 1, arm64::VS_H));
                emit(arm64::addp_v(0, 2, 3, arm64::VS_S));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                break;
            }
            if (m_vec_lane_split || instr.prefixes.vex) return false;
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 65));
            emit(arm64::movz_w(3, dst_xmm));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // Intel SDM Vol. 2 PMADDUBSW: unsigned dst bytes × signed src bytes;
    // adjacent pairs horizontally add with signed saturate to i16.
    // Mirrors the MMX NEON path (UXTL/SXTL/MUL/SADDLP/SQXTN), widened to 128.
    case Opcode::PMADDUBSW_XMM_XMMM128: {
        if (instr.prefixes.vex_l) return false;
        const u8 dst_xmm = op0.reg & 15;
        // SRC1 from vvvv (VEX) and vec_off(): lane-wise in its VEX.256 form
        // (Intel SDM Vol. 2 VPMADDUBSW ymm), no helper arm.
        const u8 src1_xmm = instr.prefixes.vex
            ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
        const u32 off_d = vec_off(dst_xmm);
        const u32 off_1 = vec_off(src1_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) return false;
        if (is_register_operand(op1.type)) {
            const u32 off_s = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // Park the EA in X21 before the first load: emit_guest_load64 /
            // the x64 region load clobbers X4 and may BLR, so the second
            // qword's address must not live there. Same shape as
            // EMIT_SSE_NEON_RR's memory arm. This arm used to return false in
            // long mode, sending every `cvtdq2ps xmm, [mem]` in an x64 guest
            // to the interpreter.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        // Checked guest loads may call a host helper, whose ABI permits it to
        // clobber V0-V7. Load SRC1 only after the complete memory source is
        // resident in V1.
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
        // UXTL2/SXTL2 = UXTL/SXTL with Q=1 (bit 30).
        emit(arm64::uxtl_v(2, 0, arm64::VS_H));
        emit(arm64::uxtl_v(3, 0, arm64::VS_H) | (1u << 30));
        emit(arm64::sxtl_v(4, 1, arm64::VS_H));
        emit(arm64::sxtl_v(5, 1, arm64::VS_H) | (1u << 30));
        emit(arm64::mul_v(2, 2, 4, arm64::VS_H));
        emit(arm64::mul_v(3, 3, 5, arm64::VS_H));
        emit(arm64::saddlp_v(2, 2, arm64::VS_H));
        emit(arm64::saddlp_v(3, 3, arm64::VS_H));
        emit(arm64::sqxtn_v(0, 2, arm64::VS_S));
        emit(arm64::sqxtn2_v(0, 3, arm64::VS_S));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ---- Min/max/avg/sad ----
    case Opcode::PMINUB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::umin_v(0, 0, 1, arm64::VS_B), 66, 67)
    case Opcode::PMAXUB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::umax_v(0, 0, 1, arm64::VS_B), 68, 69)
    case Opcode::PMINSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::smin_v(0, 0, 1, arm64::VS_H), 70, 71)
    case Opcode::PMAXSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::smax_v(0, 0, 1, arm64::VS_H), 72, 73)
    case Opcode::PAVGB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::urhadd_v(0, 0, 1, arm64::VS_B), 74, 75)
    case Opcode::PAVGW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::urhadd_v(0, 0, 1, arm64::VS_H), 76, 77)
    // ---- PSADBW xmm, xmm/m128 ----
    // Intel SDM Vol. 2 PSADBW: within EACH 64-bit half, sum the eight absolute
    // byte differences and place that sum in the half's low word, zeroing the
    // rest. UABD gives the per-byte differences; three pairwise widening adds
    // (16B→8H→4S→2D) reduce each half independently, which lands the two sums
    // exactly where the architecture wants them — the maximum is 8*255 = 2040,
    // so it occupies the low word and the upper bits of each 64-bit lane are
    // already zero. Previously a jit_sse_op call.
    case Opcode::PSADBW_XMM_XMMM128: {
        if (instr.prefixes.vex_l) return false;
        const u8 dst_xmm = op0.reg & 15;
        const u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15)
                                               : dst_xmm;
        const u32 off_d = vec_off(dst_xmm);
        const u32 off_1 = vec_off(src1_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) return false;
        if (is_register_operand(op1.type)) {
            const u32 off_s = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // Source before SRC1: emit_guest_load64 can BLR, and v0-v7 are
            // caller-saved.
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
        emit(arm64::uabd_v(2, 0, 1, arm64::VS_B));
        emit(arm64::uaddlp_v(2, 2, arm64::VS_B));
        emit(arm64::uaddlp_v(2, 2, arm64::VS_H));
        emit(arm64::uaddlp_v(2, 2, arm64::VS_S));
        emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ---- Saturating add/sub ----
    case Opcode::PADDUSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::uqadd_v(0, 0, 1, arm64::VS_B), 80, 81)
    case Opcode::PADDUSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::uqadd_v(0, 0, 1, arm64::VS_H), 82, 83)
    case Opcode::PSUBUSB_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::uqsub_v(0, 0, 1, arm64::VS_B), 84, 85)
    case Opcode::PSUBUSW_XMM_XMMM128:
        EMIT_SSE_NEON_RR(arm64::uqsub_v(0, 0, 1, arm64::VS_H), 86, 87)

    #undef EMIT_SSE_NEON_RR

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
