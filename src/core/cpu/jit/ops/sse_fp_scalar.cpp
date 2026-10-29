// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — scalar FP: ADD/SUB/MUL/DIV/MIN/MAX/SQRT SS and SD.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_fp_scalar(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- Scalar FP (SS/SD) — native NEON reg-reg; helper for mem.
    // Semantics: result lane 0 = op(dst.lane0, src.lane0) (or sqrt(src.lane0)
    // for SQRT*); upper lanes of dst are preserved. Helper op_ids 400-427
    // (14 ops × {reg, mem}).
    case Opcode::ADDSS_XMM_XMMM32:
    case Opcode::ADDSD_XMM_XMMM64:
    case Opcode::SUBSS_XMM_XMMM32:
    case Opcode::SUBSD_XMM_XMMM64:
    case Opcode::MULSS_XMM_XMMM32:
    case Opcode::MULSD_XMM_XMMM64:
    case Opcode::DIVSS_XMM_XMMM32:
    case Opcode::DIVSD_XMM_XMMM64:
    case Opcode::MINSS_XMM_XMMM32:
    case Opcode::MINSD_XMM_XMMM64:
    case Opcode::MAXSS_XMM_XMMM32:
    case Opcode::MAXSD_XMM_XMMM64:
    case Opcode::SQRTSS_XMM_XMMM32:
    case Opcode::SQRTSD_XMM_XMMM64: {
        u8 dst_xmm = op0.reg & 15;
        u32 op_type;      // 0..13
        bool is_sqrt;
        arm64::FSize fs;
        arm64::VSize vs;
        switch (instr.opcode) {
        case Opcode::ADDSS_XMM_XMMM32: op_type=0;  is_sqrt=false; fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::ADDSD_XMM_XMMM64: op_type=1;  is_sqrt=false; fs=arm64::FS_D; vs=arm64::VS_D; break;
        case Opcode::SUBSS_XMM_XMMM32: op_type=2;  is_sqrt=false; fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::SUBSD_XMM_XMMM64: op_type=3;  is_sqrt=false; fs=arm64::FS_D; vs=arm64::VS_D; break;
        case Opcode::MULSS_XMM_XMMM32: op_type=4;  is_sqrt=false; fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::MULSD_XMM_XMMM64: op_type=5;  is_sqrt=false; fs=arm64::FS_D; vs=arm64::VS_D; break;
        case Opcode::DIVSS_XMM_XMMM32: op_type=6;  is_sqrt=false; fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::DIVSD_XMM_XMMM64: op_type=7;  is_sqrt=false; fs=arm64::FS_D; vs=arm64::VS_D; break;
        case Opcode::MINSS_XMM_XMMM32: op_type=8;  is_sqrt=false; fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::MINSD_XMM_XMMM64: op_type=9;  is_sqrt=false; fs=arm64::FS_D; vs=arm64::VS_D; break;
        case Opcode::MAXSS_XMM_XMMM32: op_type=10; is_sqrt=false; fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::MAXSD_XMM_XMMM64: op_type=11; is_sqrt=false; fs=arm64::FS_D; vs=arm64::VS_D; break;
        case Opcode::SQRTSS_XMM_XMMM32: op_type=12; is_sqrt=true;  fs=arm64::FS_S; vs=arm64::VS_S; break;
        case Opcode::SQRTSD_XMM_XMMM64: op_type=13; is_sqrt=true;  fs=arm64::FS_D; vs=arm64::VS_D; break;
        default: return false;
        }
        u16 reg_opid = static_cast<u16>(400 + 2 * op_type);
        u16 mem_opid = static_cast<u16>(reg_opid + 1);
        // ADD/SUB/MUL/DIV SS/SD: host FADD/FMUL/FSUB/FDIV, with x86's NaN
        // selection emitted in a cold arm when the result is a NaN
        // (Intel SDM Vol. 1 §4.8.3.4); DIV gates on its operands for the
        // exactness residual. MIN/MAX are ops/sse_minmax.cpp's. SQRT stays
        // fully native.
        const bool gated_arith = (op_type <= 7);
        const bool is_div = (op_type == 6 || op_type == 7);
        const bool is_sd = (fs == arm64::FS_D);

        if (gated_arith) {
            // VEX (Intel SDM Vol. 2 VADDSS..VDIVSD): SRC1 is VEX.vvvv,
            // DEST[127:32|64] := SRC1[127:32|64] and DEST[MAXVL-1:128] := 0.
            // The legacy form's SRC1 is the destination, whose upper lanes
            // simply stay.
            const bool vex = instr.prefixes.vex;
            const u8 src1_xmm = vex
                ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
            const bool merge_src1 = vex && src1_xmm != dst_xmm;
            const u32 off_d = xmm_u64_off(dst_xmm, 0);
            const u32 off_1 = xmm_u64_off(src1_xmm, 0);
            const u32 off_dhi = ymm_high_u64_off(dst_xmm, 0);
            auto lane_fits = [is_sd](u32 off) {
                return is_sd ? fits_ldr_x_offset(off) : fits_ldr_w_offset(off);
            };
            const bool dst_ok = lane_fits(off_d) && lane_fits(off_1) &&
                (!merge_src1 ||
                 (fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_1))) &&
                (!vex || (fits_ldr_x_offset(off_dhi) &&
                          fits_ldr_x_offset(off_dhi + 8)));
            bool src_ok = false;
            u8 src_xmm = 0;
            if (is_register_operand(op1.type)) {
                src_xmm = op1.reg & 15;
                src_ok = lane_fits(xmm_u64_off(src_xmm, 0));
            } else if (is_memory_operand(op1.type)) {
                src_ok = true;
            }
            // The helper arms below read SRC1 from the destination.
            if (vex && !(dst_ok && src_ok)) return false;

            if (dst_ok && src_ok) {
                const bool hold = should_hold_nzcv();
                const bool ah = math::sse_alternate_nan_handling();
                const bool src_reg = is_register_operand(op1.type);
                const u32 off_s = src_reg ? xmm_u64_off(src_xmm, 0) : 0;

                // Writes lane 0 of `vres` as the architectural result. V3 is
                // its only scratch.
                const auto store_result =
                    [this, merge_src1, vex, is_sd, vs, off_d, off_1,
                     off_dhi](u8 vres) {
                    if (merge_src1) {
                        emit(arm64::ldr_q_uoff(3, REG_CPU_STATE, off_1 >> 4));
                        emit(arm64::ins_v_elem(3, 0, vres, 0, vs));
                        emit(arm64::str_q_uoff(3, REG_CPU_STATE, off_d >> 4));
                    } else {
                        emit(is_sd
                             ? arm64::str_d_uoff(vres, REG_CPU_STATE, off_d >> 3)
                             : arm64::str_s_uoff(vres, REG_CPU_STATE, off_d >> 2));
                    }
                    if (vex) {
                        emit(arm64::str_x_uoff(31, REG_CPU_STATE, off_dhi >> 3));
                        emit(arm64::str_x_uoff(31, REG_CPU_STATE,
                                               (off_dhi + 8) >> 3));
                    }
                };
                // x86's NaN result (Intel SDM Vol. 1 §4.8.3.5 Table 4-7;
                // sse_binop/sse_div in sse_float_semantics.hpp) from SRC1 in
                // V0, SRC2 in V1 and the ARM result in V2, into V6: a NaN SRC1
                // wins, quieted; else a NaN SRC2, quieted; else a NaN result
                // is an invalid operation and becomes the QNaN indefinite,
                // whose sign is set. A non-NaN result is the ARM one. V0-V2
                // hold lane 0 alone (scalar loads and ops zero the rest), so
                // FCMEQ can raise Invalid only for an SNaN operand, which the
                // operation itself already raised. Writes no NZCV.
                const auto emit_nan_fixup = [this, is_sd, vs]() {
                    if (is_sd) {
                        emit_mov_imm64(REG_SCRATCH0,
                                       static_cast<i64>(0xFFF8000000000000ull));
                        emit(arm64::dup_v_gpr(3, REG_SCRATCH0, vs));
                        emit(arm64::movz_x(REG_SCRATCH0, 0x0008, 48));
                    } else {
                        emit_mov_imm32(REG_SCRATCH0, 0xFFC00000u);
                        emit(arm64::dup_v_gpr(3, REG_SCRATCH0, vs));
                        emit(arm64::movz_w_shift(REG_SCRATCH0, 0x0040, 16));
                    }
                    emit(arm64::dup_v_gpr(4, REG_SCRATCH0, vs));  // quiet bit
                    const arm64::FSize f = is_sd ? arm64::FS_D : arm64::FS_S;
                    emit(arm64::orr_v16b(5, 1, 4));
                    emit(arm64::fcmeq_v(6, 1, 1, f));
                    emit(arm64::bsl_v16b(6, 3, 5));   // SRC2 ordered ? indef : qSRC2
                    emit(arm64::orr_v16b(5, 0, 4));
                    emit(arm64::fcmeq_v(7, 0, 0, f));
                    emit(arm64::bsl_v16b(7, 6, 5));   // SRC1 ordered ? that : qSRC1
                    emit(arm64::fcmeq_v(6, 2, 2, f));
                    emit(arm64::bsl_v16b(6, 2, 7));   // result ordered ? result : that
                };

                // ADD/SUB/MUL SS/SD: stay in the FP register file, and gate on
                // the RESULT rather than on the operands.
                //
                // sse_float_semantics.hpp (the interpreter's reference, out of
                // Intel SDM Vol. 1 §4.8.3.4) differs from ARM FADD/FSUB/FMUL
                // only where the result is a NaN: destination-first quieting
                // when both operands are NaN, and the Indefinite QNaN -- whose
                // SIGN IS SET, unlike ARM's default NaN -- for Inf-Inf and
                // 0*Inf. These three ops set no MXCSR sticky bit in the
                // interpreter, so a non-NaN result needs no fixup at all and a
                // single FCMEQ covers the entire divergence set.
                //
                // LDR S/D reads xmm lane 0 straight into V0/V1, so the old
                // shape's three GPR<->FP FMOVs and its two exponent compares
                // (which also wrote NZCV, forcing a spill/fill) all disappear.
                // DIV keeps the operand gate: MXCSR.ZE and the exactness
                // residual are only defined for finite operands.
                if (!is_div) {
                    // XMM register cache, under FPCR.AH only (no NaN arm):
                    // read the operands' lane 0 from their cache registers.
                    // The result is lane 0 alone and a scalar FP op zeroes
                    // Vd above it, so the destination's cache register is
                    // not updated but dropped: keeping it would cost an INS
                    // here, dropping costs at most one reload at its next
                    // use, so dropping is never more words.
                    const bool cached = ah && !vex &&
                        xmm_cache_fits(dst_xmm, src_reg ? src_xmm : 0);
                    u8 va = 0;  // SRC1's lane 0
                    u8 vb = 1;  // SRC2
                    if (cached && src_reg) {
                        vb = xmm_cache_source(src_xmm);
                    }
                    if (cached && (m_xmm_cache & (1u << dst_xmm)) != 0) {
                        va = static_cast<u8>(kXmmCacheBase + dst_xmm);
                    }
                    if (src_reg) {
                        if (va == 0) {
                            emit(is_sd
                                 ? arm64::ldr_d_uoff(0, REG_CPU_STATE, off_1 >> 3)
                                 : arm64::ldr_s_uoff(0, REG_CPU_STATE, off_1 >> 2));
                        }
                        if (vb == 1) {
                            emit(is_sd
                                 ? arm64::ldr_d_uoff(1, REG_CPU_STATE, off_s >> 3)
                                 : arm64::ldr_s_uoff(1, REG_CPU_STATE, off_s >> 2));
                        }
                    } else {
                        // The guest load can BLR (clobbering V0-V7), so it runs
                        // before SRC1 is brought into V0, and its bounds SUBS
                        // is why the memory arm spills NZCV.
                        if (hold) emit_nzcv_spill();
                        emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH4));
                        if (is_sd) {
                            emit_guest_load64(REG_SCRATCH1, REG_CALL_TMP);
                            emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
                            if (va == 0) {
                                emit(arm64::ldr_d_uoff(0, REG_CPU_STATE,
                                                       off_1 >> 3));
                            }
                        } else {
                            emit_guest_load32(REG_SCRATCH1, REG_CALL_TMP);
                            emit(arm64::fmov_s_from_w(1, REG_SCRATCH1));
                            if (va == 0) {
                                emit(arm64::ldr_s_uoff(0, REG_CPU_STATE,
                                                       off_1 >> 2));
                            }
                        }
                    }
                    if (op_type <= 1) {
                        emit(arm64::fadd_s_scalar(2, va, vb, fs));
                    } else if (op_type <= 3) {
                        emit(arm64::fsub_s_scalar(2, va, vb, fs));
                    } else {
                        emit(arm64::fmul_s_scalar(2, va, vb, fs));
                    }
                    // FPCR.AH makes the result x86's already, NaNs included
                    // (math::sse_alternate_nan_handling); store it and done.
                    if (ah) {
                        store_result(2);
                        if (cached) {
                            m_xmm_cache = static_cast<u16>(
                                m_xmm_cache & ~(1u << dst_xmm));
                            m_xmm_cache_aware = true;
                        }
                        if (!src_reg && hold) emit_nzcv_fill();
                        break;
                    }
                    // A scalar FP op zeroes Vd above its own lane and +0.0 is
                    // not a NaN, so a whole-register FCMEQ still answers only
                    // for the architectural lane. FCMEQ/UMINV write no NZCV.
                    emit(arm64::fcmeq_v(3, 2, 2, fs));
                    emit(arm64::uminv_v(3, 3, arm64::VS_B));
                    emit(arm64::umov_w_b(REG_SCRATCH0, 3, 0));
                    const usize nan_br = m_code_buf.size();
                    emit(0);  /* cbz W0 -> NaN arm when the result is a NaN */
                    store_result(2);
                    // The NaN arm runs only on a NaN result, out of line; it
                    // calls nothing and writes no NZCV.
                    std::vector<ColdArmEntry> arm_entries;
                    arm_entries.push_back({nan_br, [](i32 disp) {
                        return arm64::cbz_w(REG_SCRATCH0, disp);
                    }});
                    defer_cold_arm(std::move(arm_entries),
                        [emit_nan_fixup, store_result]() {
                            emit_nan_fixup();
                            store_result(6);
                        }, /*semantic_helper=*/false);
                    if (!src_reg && hold) emit_nzcv_fill();
                    break;
                }
                if (hold) emit_nzcv_spill();
                if (src_reg) {
                    // Reg-reg: no helper BLR between loads — V0/V1 stay live.
                    if (is_sd) {
                        emit(arm64::ldr_x_uoff(REG_EA_TMP, REG_CPU_STATE,
                                              off_1 >> 3));
                        emit(arm64::fmov_d_from_x(0, REG_EA_TMP));
                        emit(arm64::ldr_x_uoff(REG_CALL_TMP, REG_CPU_STATE,
                                              off_s >> 3));
                        emit(arm64::fmov_d_from_x(1, REG_CALL_TMP));
                    } else {
                        emit(arm64::ldr_w_uoff(REG_EA_TMP, REG_CPU_STATE,
                                              off_1 >> 2));
                        emit(arm64::fmov_s_from_w(0, REG_EA_TMP));
                        emit(arm64::ldr_w_uoff(REG_CALL_TMP, REG_CPU_STATE,
                                              off_s >> 2));
                        emit(arm64::fmov_s_from_w(1, REG_CALL_TMP));
                    }
                } else {
                    // Mem: load src first (may BLR / clobber V0-V7 + X0-X18),
                    // then load SRC1 from CpuState so the finite gate and
                    // FDIV see intact operands.
                    emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                    emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                    emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH4));
                    if (is_sd) {
                        emit_guest_load64(REG_SCRATCH1, REG_CALL_TMP);
                        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
                        emit(arm64::ldr_x_uoff(REG_EA_TMP, REG_CPU_STATE,
                                              off_1 >> 3));
                        emit(arm64::fmov_d_from_x(0, REG_EA_TMP));
                    } else {
                        emit_guest_load32(REG_SCRATCH1, REG_CALL_TMP);
                        emit(arm64::fmov_s_from_w(1, REG_SCRATCH1));
                        emit(arm64::ldr_w_uoff(REG_EA_TMP, REG_CPU_STATE,
                                              off_1 >> 2));
                        emit(arm64::fmov_s_from_w(0, REG_EA_TMP));
                    }
                }

                // Operand gate: exp != all-ones (excludes Inf and NaN) and a
                // nonzero divisor bit pattern. SRC1 bits in X5; SRC2 bits in
                // X21 (reg) or X1 (mem).
                const u8 src_bits = src_reg ? REG_CALL_TMP : REG_SCRATCH1;
                if (is_sd) {
                    emit_mov_imm64(REG_SCRATCH2,
                                   static_cast<i64>(0x7FF0000000000000ULL));
                    emit(arm64::and_x(REG_SCRATCH3, REG_EA_TMP, REG_SCRATCH2));
                    emit(arm64::subs_x(31, REG_SCRATCH3, REG_SCRATCH2));
                } else {
                    emit_mov_imm32(REG_SCRATCH2, 0x7F800000u);
                    emit(arm64::and_w(REG_SCRATCH3, REG_EA_TMP, REG_SCRATCH2));
                    emit(arm64::subs_w(31, REG_SCRATCH3, REG_SCRATCH2));
                }
                const usize dst_special = m_code_buf.size();
                emit(0);
                if (is_sd) {
                    emit(arm64::and_x(REG_SCRATCH3, src_bits, REG_SCRATCH2));
                    emit(arm64::subs_x(31, REG_SCRATCH3, REG_SCRATCH2));
                } else {
                    emit(arm64::and_w(REG_SCRATCH3, src_bits, REG_SCRATCH2));
                    emit(arm64::subs_w(31, REG_SCRATCH3, REG_SCRATCH2));
                }
                const usize src_special = m_code_buf.size();
                emit(0);
                emit(is_sd ? arm64::lsl_x_imm(REG_SCRATCH3, src_bits, 1)
                           : arm64::lsl_w_imm(REG_SCRATCH3, src_bits, 1));
                const usize src_zero = m_code_buf.size();
                emit(0);
                emit(arm64::fdiv_s_scalar(0, 0, 1, fs));
                // sse_div raises PE only when the QUOTIENT is finite, and
                // evaluates the residual only then. Finite operands can still
                // overflow (a normal over the smallest denormal is Inf, and
                // under DAZ that divisor is a zero), and the residual a-r*b
                // is then infinite -- read below as "inexact" -- or, for a
                // flushed divisor, Inf*0, which raises a host Invalid the
                // interpreter never raises. REG_SCRATCH2 still holds the
                // exponent mask built by the operand gate.
                emit(arm64::dup_v_gpr(5, REG_SCRATCH2, vs));
                emit(arm64::and_v16b(6, 0, 5));
                emit(arm64::cmeq_v(6, 6, 5, vs));
                emit(arm64::umov_w_s(REG_SCRATCH3, 6, 0));
                const usize pe_inf = m_code_buf.size();
                emit(0);
                if (is_sd) {
                    emit(arm64::fmov_d_from_x(2, REG_EA_TMP));
                } else {
                    emit(arm64::fmov_s_from_w(2, REG_EA_TMP));
                }
                emit(arm64::fmsub_s_scalar(3, 0, 1, 2, fs));
                emit(arm64::movi_v16b(4, 0));
                emit(is_sd ? arm64::fcmp_d(3, 4) : arm64::fcmp_s(3, 4));
                const usize pe_eq = m_code_buf.size();
                emit(0);
                {
                    const u32 mxcsr_off = static_cast<u32>(
                        offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
                    emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                           mxcsr_off >> 2));
                    emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
                    emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
                    emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                           mxcsr_off >> 2));
                }
                m_code_buf[pe_eq] = arm64::b_cond(
                    arm64::EQ, static_cast<i32>(m_code_buf.size() - pe_eq) * 4);
                m_code_buf[pe_inf] = arm64::cbnz_w(
                    REG_SCRATCH3,
                    static_cast<i32>(m_code_buf.size() - pe_inf) * 4);
                store_result(0);
                // Finite operands still divide to a NaN under DAZ: 0 over a
                // denormal is 0/0, an invalid operation. FPCR.AH returns
                // x86's indefinite for it; without AH, ARM's default NaN is
                // positive, so the arm overwrites it.
                usize div_nan = 0;
                if (!ah) {
                    emit(arm64::fcmeq_v(4, 0, 0, fs));
                    emit(arm64::umov_w_s(REG_SCRATCH3, 4, 0));
                    div_nan = m_code_buf.size();
                    emit(0);
                }
                // The special-operand arm (Inf/NaN, or a zero divisor), out
                // of line. sse_div's PE needs finite operands and a nonzero
                // divisor, so it never applies here. ZE is sse_div's explicit
                // rule, on the bits: a +-0 divisor, a finite dividend that is
                // not zero (a denormal one is, under DAZ). IE and the host's
                // own flags come from FDIV as they do from the interpreter's
                // a / b. Then x86's NaN selection. It calls nothing and
                // writes no NZCV; V0/V1 still hold the operands on entry.
                std::vector<ColdArmEntry> arm_entries;
                arm_entries.push_back({dst_special, [](i32 disp) {
                    return arm64::b_cond(arm64::EQ, disp);
                }});
                arm_entries.push_back({src_special, [](i32 disp) {
                    return arm64::b_cond(arm64::EQ, disp);
                }});
                arm_entries.push_back({src_zero, [is_sd](i32 disp) {
                    return is_sd ? arm64::cbz_x(REG_SCRATCH3, disp)
                                 : arm64::cbz_w(REG_SCRATCH3, disp);
                }});
                defer_cold_arm(std::move(arm_entries),
                    [this, ah, fs, vs, is_sd, emit_nan_fixup, store_result]() {
                        const u32 mxcsr_off = static_cast<u32>(
                            offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
                        const auto dup_const = [&](u8 v, u64 value) {
                            if (is_sd) emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(value));
                            else emit_mov_imm32(REG_SCRATCH0, static_cast<u32>(value));
                            emit(arm64::dup_v_gpr(v, REG_SCRATCH0, vs));
                        };
                        // V3/V4 = |a|/|b| images; V5 = a finite.
                        emit(arm64::shl_v_imm(3, 0, 1, vs));
                        emit(arm64::ushr_v_imm(3, 3, 1, vs));
                        emit(arm64::shl_v_imm(4, 1, 1, vs));
                        emit(arm64::ushr_v_imm(4, 4, 1, vs));
                        dup_const(6, is_sd ? 0x7FF0000000000000ull : 0x7F800000ull);
                        emit(arm64::cmhi_v(5, 6, 3, vs));
                        // V6 = all ones under DAZ.
                        emit(arm64::ldr_w_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                               mxcsr_off >> 2));
                        emit(arm64::ubfx_w(REG_SCRATCH0, REG_SCRATCH3,
                            static_cast<u8>(std::countr_zero(u32(MXCSR_DAZ))), 1));
                        emit(arm64::neg_w(REG_SCRATCH0, REG_SCRATCH0));
                        if (is_sd) emit(arm64::sxtw_x(REG_SCRATCH0, REG_SCRATCH0));
                        emit(arm64::dup_v_gpr(6, REG_SCRATCH0, vs));
                        // V3 = a is zero: +-0, or a denormal under DAZ.
                        dup_const(7, is_sd ? 0x0010000000000000ull : 0x00800000ull);
                        emit(arm64::cmhi_v(7, 7, 3, vs));
                        emit(arm64::and_v16b(7, 7, 6));
                        emit(arm64::cmeq_v_zero(3, 3, vs));
                        emit(arm64::orr_v16b(3, 3, 7));
                        // ZE = b is +-0 & a finite & a not zero.
                        emit(arm64::cmeq_v_zero(4, 4, vs));
                        emit(arm64::and_v16b(4, 4, 5));
                        emit(arm64::bic_v16b(4, 4, 3));
                        emit(arm64::umov_w_s(REG_SCRATCH0, 4, 0));
                        emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH0, 31));
                        static_assert(u32(MXCSR_ZE) == 1u << 2);
                        emit(arm64::orr_w_lsl(REG_SCRATCH3, REG_SCRATCH3,
                                              REG_SCRATCH0, 2));
                        emit(arm64::str_w_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                               mxcsr_off >> 2));
                        emit(arm64::fdiv_s_scalar(2, 0, 1, fs));
                        if (ah) {
                            store_result(2);
                        } else {
                            emit_nan_fixup();
                            store_result(6);
                        }
                    }, /*semantic_helper=*/false);
                if (!ah) {
                    std::vector<ColdArmEntry> nan_entries;
                    nan_entries.push_back({div_nan, [](i32 disp) {
                        return arm64::cbz_w(REG_SCRATCH3, disp);
                    }});
                    defer_cold_arm(std::move(nan_entries),
                        [this, is_sd, vs, store_result]() {
                            if (is_sd) {
                                emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(
                                    0xFFF8000000000000ull));
                            } else {
                                emit_mov_imm32(REG_SCRATCH0, 0xFFC00000u);
                            }
                            emit(arm64::dup_v_gpr(6, REG_SCRATCH0, vs));
                            store_result(6);
                        }, /*semantic_helper=*/false);
                }
                if (hold) emit_nzcv_fill();
                break;
            }
        }

        if (is_register_operand(op1.type)) {
            u8 src_xmm = op1.reg & 15;
            u32 off_d = xmm_u64_off(dst_xmm, 0);
            u32 off_s = xmm_u64_off(src_xmm, 0);
            const bool native_ok = is_sqrt
                && fits_ldr_q_offset(off_d) && fits_ldr_q_offset(off_s);
            if (native_ok) {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
                emit(arm64::fsqrt_s_scalar(2, 1, fs));
                // x86's QNaN indefinite for sqrt(negative) has the SIGN BIT
                // SET (Intel SDM Vol. 1 §4.8.3.7); ARM's default NaN does not.
                // A bare FSQRT therefore published +QNaN where hardware and
                // the interpreter both give -QNaN. Corrected the same way as
                // the packed SQRTPS/SQRTPD forms: FCMLT-against-zero is false
                // for NaN (so a propagated input NaN is left alone) and false
                // for -0 (whose root is -0, not an invalid operation), so it
                // selects exactly the invalid-operation case.
                emit(arm64::movi_v16b(3, 0));
                emit(arm64::fcmgt_v(3, 3, 1, fs));
                if (fs == arm64::FS_D) {
                    emit_mov_imm64(REG_SCRATCH0,
                                   static_cast<i64>(0xFFF8000000000000ull));
                    emit(arm64::dup_v_gpr(4, REG_SCRATCH0, arm64::VS_D));
                } else {
                    emit_mov_imm32(REG_SCRATCH0, 0xFFC00000u);
                    emit(arm64::dup_v_gpr(4, REG_SCRATCH0, arm64::VS_S));
                }
                emit(arm64::bsl_v16b(3, 4, 2));
                emit(arm64::ins_v_elem(0, 0, 3, 0, vs));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                break;
            }
            (void)fs;
            (void)vs;
            (void)op_type;
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

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
