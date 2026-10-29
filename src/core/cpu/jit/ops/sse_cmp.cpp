// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — FP compares and flag-writing compares: CMPPS/CMPPD,
// CMPSS/CMPSD, COMISS/UCOMISS/COMISD/UCOMISD and PTEST.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

// ---------------------------------------------------------------------------
// Promoted out of compile_sse (ADR-0006). These were `auto x = [&](...)`
// lambdas declared 150 lines into a 6,863-line function, which is why that
// function could not be broken up: its case arms closed over them. Verified
// to capture nothing from the enclosing scope -- only `this` and their own
// parameters -- so promotion is mechanical.
// ---------------------------------------------------------------------------

void JitCompiler::emit_fp_cmp_predicate(u8 d, u8 pred, bool dbl, u8 a,
                                        u8 b, bool scalar) {
    // d := (a PRED b) per lane. Every predicate reads a and b before its
    // first write of d, so d may be either operand. V2/V3 are scratch.
    //
    // MXCSR.IE (Intel SDM Vol. 2 CMPPS Table 3-1, kSseCmpSignalsOnQnan): every
    // predicate signals invalid for an SNaN operand, and the signalling ones
    // (_OS/_US, and FALSE_OS/TRUE_US) for a QNaN too. It reaches MXCSR through
    // the host FPSR (IOC, harvested at STMXCSR/FXSAVE), so it is decided by
    // WHICH compare runs (Arm ARM FPCompareEQ/GE/GT): FCMEQ signals only for
    // an SNaN, FCMGE/FCMGT for any NaN. Where a predicate's mask and its
    // signalling need different instructions, they are built apart:
    //   * a quiet ordering predicate (LT_OQ, LE_OQ, NLT_UQ, NLE_UQ, NGE_UQ,
    //     NGT_UQ, GE_OQ, GT_OQ) compares operands whose unordered lanes are
    //     replaced by +0 under the quiet ORD mask, so FCMGE/FCMGT never see a
    //     NaN, and takes the unordered result from the mask;
    //   * a signalling equality/ordered predicate (EQ_OS, UNORD_S, NEQ_US,
    //     ORD_S, EQ_US, NEQ_OS) runs a throwaway FCMGE first;
    //   * FALSE/TRUE run a throwaway FCMEQ (_OQ/_UQ) or FCMGE (_OS/_US).
    // Scalar forms use the scalar encodings, which compare element 0 alone:
    // CMPSS/CMPSD neither compare nor signal on the upper elements.
    const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;
    const auto eq = [&](u8 o, u8 x, u8 y) {
        emit(scalar ? arm64::fcmeq_scalar(o, x, y, fs) : arm64::fcmeq_v(o, x, y, fs));
    };
    const auto ge = [&](u8 o, u8 x, u8 y) {  // o := x >= y
        emit(scalar ? arm64::fcmge_scalar(o, x, y, fs) : arm64::fcmge_v(o, x, y, fs));
    };
    const auto gt = [&](u8 o, u8 x, u8 y) {  // o := x > y
        emit(scalar ? arm64::fcmgt_scalar(o, x, y, fs) : arm64::fcmgt_v(o, x, y, fs));
    };
    // ARM FCMxx are FALSE for any NaN operand, i.e. they are the ORDERED
    // relations; the unordered ones are their complements.
    auto ordered = [&](u8 out) {  // out = ORD(a, b), quiet
        eq(2, a, a);
        eq(3, b, b);
        emit(arm64::and_v16b(out, 2, 3));
    };
    const u8 relation = pred & 15;
    const bool signals = cpu::kSseCmpSignalsOnQnan[pred & 31];
    const bool ordering = relation == 1 || relation == 2 || relation == 5 ||
                          relation == 6 || relation == 9 || relation == 10 ||
                          relation == 13 || relation == 14;
    if (relation == 11 || relation == 15) {  // FALSE / TRUE
        if (signals) ge(2, a, b); else eq(2, a, b);
        emit(arm64::movi_v16b(d, relation == 11 ? 0x00 : 0xFF));
        return;
    }
    if (ordering && !signals) {
        ordered(2);                         // V2 = ORD; an SNaN signals here
        emit(arm64::and_v16b(3, b, 2));     // V3 = b, unordered lanes +0
        emit(arm64::and_v16b(d, a, 2));     // d  = a, unordered lanes +0
        switch (relation) {
        case 1:  gt(d, 3, d); break;                                       // LT_OQ
        case 2:  ge(d, 3, d); emit(arm64::and_v16b(d, d, 2)); break;       // LE_OQ
        case 5:  gt(d, 3, d); emit(arm64::not_v16b(d, d)); break;          // NLT_UQ
        case 6:  ge(d, 3, d); emit(arm64::and_v16b(d, d, 2));
                 emit(arm64::not_v16b(d, d)); break;                       // NLE_UQ
        case 9:  ge(d, d, 3); emit(arm64::and_v16b(d, d, 2));
                 emit(arm64::not_v16b(d, d)); break;                       // NGE_UQ
        case 10: gt(d, d, 3); emit(arm64::not_v16b(d, d)); break;          // NGT_UQ
        case 13: ge(d, d, 3); emit(arm64::and_v16b(d, d, 2)); break;       // GE_OQ
        case 14: gt(d, d, 3); break;                                       // GT_OQ
        }
        return;
    }
    // The ordering relations below are their signalling forms (FCMGE/FCMGT
    // signal for a QNaN by themselves); the equality/ordered relations are
    // quiet unless the predicate signals, which the throwaway FCMGE supplies.
    if (!ordering && signals) ge(2, a, b);
    switch (relation) {
    case 0: eq(d, a, b); break;                                          // EQ
    case 1: gt(d, b, a); break;                                          // LT_OS
    case 2: ge(d, b, a); break;                                          // LE_OS
    case 3: ordered(d); emit(arm64::not_v16b(d, d)); break;              // UNORD
    case 4: eq(d, a, b); emit(arm64::not_v16b(d, d)); break;             // NEQ_U
    case 5: gt(d, b, a); emit(arm64::not_v16b(d, d)); break;             // NLT_US
    case 6: ge(d, b, a); emit(arm64::not_v16b(d, d)); break;             // NLE_US
    case 7: ordered(d); break;                                           // ORD
    case 8:  // EQ_U: EQ or unordered = NOT(ORD AND NOT EQ)
        ordered(2);
        eq(3, a, b);
        emit(arm64::bic_v16b(d, 2, 3));
        emit(arm64::not_v16b(d, d));
        break;
    case 9:  ge(d, a, b); emit(arm64::not_v16b(d, d)); break;            // NGE_US
    case 10: gt(d, a, b); emit(arm64::not_v16b(d, d)); break;            // NGT_US
    case 12:  // NEQ_O: ordered and not equal
        ordered(2);
        eq(3, a, b);
        emit(arm64::bic_v16b(d, 2, 3));
        break;
    case 13: ge(d, a, b); break;                                         // GE_OS
    case 14: gt(d, a, b); break;                                         // GT_OS
    default: break;
    }
}

std::optional<bool> JitCompiler::compile_sse_cmp(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- CMPPS / CMPPD xmm, xmm/m128, imm8 (8 predicates) ----
    case Opcode::CMPPS_XMM_XMMM128_IMM8:
    case Opcode::CMPPD_XMM_XMMM128_IMM8: {
        // VEX.256 reaches this only via compile_vex256_lane_split, which
        // re-emits it once per 128-bit lane with vec_off() naming the half.
        if (instr.prefixes.vex_l) return false;
        u8 dst_xmm = op0.reg & 15;
        // Intel SDM Vol. 2 VCMPPS: DEST := SRC1 CMP SRC2, SRC1 = vvvv. The
        // legacy form has SRC1 == DEST, and the 128-bit VEX bridge copies
        // vvvv->dst in front of this, so reading vvvv is right in all three
        // cases and is the only one that works for a suppressed bridge.
        u8 src1_xmm = instr.prefixes.vex ? (instr.prefixes.vex_vvvv & 15)
                                         : dst_xmm;
        u32 off_d = vec_off(dst_xmm);
        u32 off_1 = vec_off(src1_xmm);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1)) return false;
        arm64::FSize fs = (instr.opcode == Opcode::CMPPD_XMM_XMMM128_IMM8)
                              ? arm64::FS_D : arm64::FS_S;
        // Legacy SSE uses imm8[2:0]; VEX uses imm8[4:0] (32 predicates).
        const u8 pred = static_cast<u8>(instr.operands[2].imm) &
                        ((instr.prefixes.vex || m_current_original_vex) ? 0x1F : 0x07);
        // XMM register cache: compare cache registers into the destination's.
        if (is_register_operand(op1.type) &&
            xmm_cache_fits(dst_xmm, src1_xmm, op1.reg & 15) &&
            fits_ldr_q_offset(vec_off(op1.reg & 15))) {
            const u8 va = xmm_cache_source(src1_xmm);
            const u8 vb = xmm_cache_source(static_cast<u8>(op1.reg & 15));
            const u8 vd = static_cast<u8>(kXmmCacheBase + dst_xmm);
            emit_fp_cmp_predicate(vd, pred, fs == arm64::FS_D, va, vb);
            emit(arm64::str_q_uoff(vd, REG_CPU_STATE, off_d >> 4));
            xmm_cache_publish(dst_xmm);
            m_xmm_cache_aware = true;
            break;
        }
        // SRC1 goes into V0 AFTER SRC2 is in V1: the memory path below reaches
        // emit_guest_load64, which is a helper call on the non-linear region
        // path, and v0-v7 are caller-saved. Loading SRC1 first left the
        // comparison reading whatever the helper returned — every lane came
        // back "equal". Same defect as CMPSS/INSERTPS/FCOM.
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = vec_off(src_xmm);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        }
        // Memory SRC2, both bitnesses. The EA is parked in X21
        // (callee-saved) and EA+8 is recomputed from it AFTER the first load:
        // in long mode the region probe inside emit_guest_load64 clobbers X4,
        // which is why this path used to hold the EA in X4 and decline every
        // 64-bit block. Both qwords are in GPRs before V1 is formed, and the
        // loads' SUBS/BLR clobber NZCV, so a live predecessor result is
        // spilled around them.
        const bool hold_mem = !is_register_operand(op1.type) &&
                              should_hold_nzcv();
        if (hold_mem) emit_nzcv_spill();
        if (!is_register_operand(op1.type)) {
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
        emit_fp_cmp_predicate(0, pred, fs == arm64::FS_D);
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        if (hold_mem) emit_nzcv_fill();
        break;
    }

    // ---- CMPSS / CMPSD xmm, xmm/m32|m64, imm8 — scalar compare, lane 0 only ----
    case Opcode::CMPSS_XMM_XMMM32_IMM8:
    case Opcode::CMPSD_XMM_XMMM64_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        bool is_pd = (instr.opcode == Opcode::CMPSD_XMM_XMMM64_IMM8);
        arm64::FSize fs = is_pd ? arm64::FS_D : arm64::FS_S;
        arm64::VSize lane_sz = is_pd ? arm64::VS_D : arm64::VS_S;
        const u8 pred = static_cast<u8>(instr.operands[2].imm) &
                        ((instr.prefixes.vex || m_current_original_vex) ? 0x1F : 0x07);
        // Load the SOURCE before the destination. emit_guest_load* may BLR on
        // a region-cache miss, and AAPCS64 makes v0-v7 caller-saved, so a
        // destination already staged in V0 does not survive the call — the
        // same hazard the COMISS arm below documents.
        //
        // CMPSS/CMPSD write only lane 0 and preserve DEST[127:32] (Intel SDM
        // Vol. 2), and those preserved bits are exactly what was being lost:
        // the JIT returned a destination whose upper 96 bits were whatever the
        // helper left in V0. Found by the ISA matrix sweep.
        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_s = xmm_u64_off(src_xmm, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else if (m_block_is_64bit) {
            return false;
        } else if (is_pd) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_SCRATCH0, arm64::VS_D));
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_SCRATCH0, arm64::VS_S));
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        }
        // Element-0 compare into V4. The scalar encodings matter for
        // MXCSR.IE: a packed compare would also signal for a NaN in the
        // upper elements, which CMPSS/CMPSD do not compare (Intel SDM Vol. 2).
        emit_fp_cmp_predicate(4, pred, is_pd, 0, 1, /*scalar=*/true);
        // INS V0.<lane>[0] from V4.<lane>[0] — preserves upper lanes of dst.
        emit(arm64::ins_v_elem(0, 0, 4, 0, lane_sz));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ---- COMISS / UCOMISS / COMISD / UCOMISD — native FCMP → EFLAGS. ----
    // Intel SDM Vol. 2: ZF/PF/CF encode unordered/gt/lt/eq; OF/SF/AF cleared.
    // Flag results are identical for ordered vs unordered; only #IA on QNaN
    // differs (interpreter/helper also omit MXCSR exception delivery here).
    // ARM FCMP NZCV: V=unordered, Z=equal, N=less — same mapping as FCOMI.
    case Opcode::COMISS_XMM_XMMM32:
    case Opcode::UCOMISS_XMM_XMMM32:
    case Opcode::COMISD_XMM_XMMM64:
    case Opcode::UCOMISD_XMM_XMMM64: {
        const bool is_sd =
            (instr.opcode == Opcode::COMISD_XMM_XMMM64 ||
             instr.opcode == Opcode::UCOMISD_XMM_XMMM64);
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_d = xmm_u64_off(dst_xmm, 0);
        // Load src first. emit_guest_load* may BLR on a region-cache miss;
        // AAPCS64 clobbers v0–v7, which would drop a dest already in d0.
        if (is_register_operand(op1.type)) {
            const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            // Load straight into the FP register file: LDR W + FMOV S puts a
            // cross-domain transfer in the FCMP's dependency chain for no
            // reason.
            if (is_sd) {
                if (!fits_ldr_x_offset(off_s)) return false;
                emit(arm64::ldr_d_uoff(1, REG_CPU_STATE, off_s >> 3));
            } else {
                if (!fits_ldr_w_offset(off_s)) return false;
                emit(arm64::ldr_s_uoff(1, REG_CPU_STATE, off_s >> 2));
            }
        } else if (is_memory_operand(op1.type)) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            if (is_sd) {
                emit_guest_load64(REG_SCRATCH1, REG_SCRATCH4);
                emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
            } else {
                emit_guest_load32(REG_SCRATCH1, REG_SCRATCH4);
                emit(arm64::fmov_s_from_w(1, REG_SCRATCH1));
            }
        } else {
            return false;
        }
        if (is_sd) {
            if (!fits_ldr_x_offset(off_d)) return false;
            emit(arm64::ldr_d_uoff(0, REG_CPU_STATE, off_d >> 3));
        } else {
            if (!fits_ldr_w_offset(off_d)) return false;
            emit(arm64::ldr_s_uoff(0, REG_CPU_STATE, off_d >> 2));
        }
        // Intel SDM Vol. 2 COMISS: "Invalid (if SNaN or QNaN operands)";
        // UCOMISS: "Invalid (if SNaN Operands)". ARM FCMPE (signaling) raises
        // Invalid Operation for any NaN and FCMP (quiet) only for an SNaN
        // (Arm ARM, FCMP and FCMPE), so the host FPSR the SSE flags are
        // harvested from carries exactly x86's IE. FCMPE is FCMP with
        // opc<1> (bit 4) set.
        const bool signaling =
            instr.opcode == Opcode::COMISS_XMM_XMMM32 ||
            instr.opcode == Opcode::COMISD_XMM_XMMM64;
        const u32 cmp = is_sd ? arm64::fcmp_d(0, 1) : arm64::fcmp_s(0, 1);
        emit(signaling ? (cmp | 0x10u) : cmp);
        // Branch-free NZCV -> EFLAGS, no materialize helper.
        //
        // COMISS writes ALL six arithmetic flags (ZF/PF/CF from the compare,
        // OF/SF/AF cleared — Intel SDM Vol. 2). Any pending lazy FlagOp is
        // therefore dead: materializing it first only to overwrite every bit
        // it produces is pure cost. Discard the lazy record instead (the
        // CpuState::discard_lazy_flags contract) — flag_op = None, both
        // CF/OF overlays cleared, nzcv_kind cleared.
        //
        // ARM FCMP NZCV (0=false): less N=1,Z=0,C=0,V=0; equal N=0,Z=1,C=1;
        // greater N=0,Z=0,C=1; unordered N=0,Z=0,C=1,V=1. So
        //   CF = less || unordered      == cond LT  (N != V)
        //   PF = unordered              == cond VS  (V)
        //   ZF = equal || unordered     == EQ | VS
        // and greater leaves all three 0.
        emit(arm64::cset_w(REG_SCRATCH0, arm64::LT));   // CF
        emit(arm64::cset_w(REG_SCRATCH1, arm64::VS));   // PF
        emit(arm64::cset_w(REG_SCRATCH2, arm64::EQ));   // Z
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));  // ZF
        const u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        // Clear ZF/PF/CF and OF/SF/AF before encoding the compare result.
        static_assert((FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF |
                       FLAG_OF) == 0x8D5u);
        emit(arm64::movz_w(REG_SCRATCH4, 0x8D5));
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        static_assert(FLAG_CF == 1u && FLAG_PF == (1u << 2) &&
                      FLAG_ZF == (1u << 6));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH0));
        emit(arm64::orr_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH1, 2));
        emit(arm64::orr_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 6));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        static_assert(static_cast<u8>(CpuState::FlagOp::None) == 0);
        emit(arm64::strb_uoff(31, REG_CPU_STATE, flag_op_offset()));
        note_lazy_kind_publish(CpuState::FlagOp::None);
        emit_clear_cf_overlay();
        // COMISS writes EFLAGS. A prior CMP's nzcv_kind must not survive
        // into a Jcc-first successor (Intel SDM Vol. 2 COMISS / Jcc).
        emit_store_nzcv_kind(NzcvState::None);
        // Nothing above writes PSTATE, so NZCV still holds the FCMP. An
        // adjacent Jcc/SETcc/CMOVcc whose condition FCMP can express branches
        // on it directly instead of re-reading the EFLAGS just stored.
        m_flags.nzcv = NzcvState::FpCmp;
        break;
    }

#define EMIT_SSE_XMM_XMMM128_LOCAL(REG_OPID, MEM_OPID) \
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
    // ---- PTEST xmm1, xmm2/m128 ----
    // Intel SDM Vol. 2 PTEST: ZF = ((SRC2 AND SRC1) == 0);
    // CF = ((SRC2 AND NOT SRC1) == 0); OF, AF, SF and PF are cleared.
    // AND / BIC give the two products; UMAXV over bytes reduces each to zero
    // or non-zero. Flags are published the same way COMISS does it above --
    // a full write, so the lazy FlagOp is cleared rather than left pending,
    // which is what the partial-flag landmine requires of a full writer.
    case Opcode::PTEST_XMM_XMMM128: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_d = vec_off(dst_xmm);
        if (!fits_ldr_q_offset(off_d)) {
            EMIT_SSE_XMM_XMMM128_LOCAL(508, 509);
        }
        if (is_register_operand(op1.type)) {
            const u32 off_s = vec_off(op1.reg & 15);
            if (!fits_ldr_q_offset(off_s)) {
                EMIT_SSE_XMM_XMMM128_LOCAL(508, 509);
            }
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            // Source first: emit_guest_load64 can BLR and v0-v7 are
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
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));   // SRC1
        emit(arm64::and_v16b(2, 1, 0));                          // SRC2 & SRC1
        emit(arm64::bic_v16b(3, 1, 0));                          // SRC2 & ~SRC1
        emit(arm64::umaxv_v(2, 2, arm64::VS_B));
        emit(arm64::umaxv_v(3, 3, arm64::VS_B));
        emit(arm64::umov_w_b(REG_SCRATCH0, 2, 0));
        emit(arm64::umov_w_b(REG_SCRATCH1, 3, 0));
        // ZF = (and == 0), CF = (andn == 0).
        emit(arm64::cmp_w_imm(REG_SCRATCH0, 0));
        emit(arm64::cset_w(REG_SCRATCH0, arm64::EQ));
        emit(arm64::cmp_w_imm(REG_SCRATCH1, 0));
        emit(arm64::cset_w(REG_SCRATCH1, arm64::EQ));
        {
            const u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
            emit(arm64::movz_w(REG_SCRATCH4, 0x8D5));
            emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
            static_assert(FLAG_CF == 1u && FLAG_ZF == (1u << 6));
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH1));
            emit(arm64::orr_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                                  REG_SCRATCH0, 6));
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
            emit(arm64::strb_uoff(31, REG_CPU_STATE, flag_op_offset()));
            emit_clear_cf_overlay();
            emit_store_nzcv_kind(NzcvState::None);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
