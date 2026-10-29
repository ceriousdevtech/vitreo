// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — packed shifts: PSLL/PSRL/PSRA by imm8 (with PSLLDQ/PSRLDQ)
// and by an xmm/m128 count.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_shift(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- Packed shifts by imm8 — native NEON SHL/USHR/SSHR when shift
    // fits ARM's [0..esize-1] / [1..esize] range; else helper (op_ids 100-109).
    case Opcode::PSLLW_XMM_IMM8:
    case Opcode::PSLLD_XMM_IMM8:
    case Opcode::PSLLQ_XMM_IMM8:
    case Opcode::PSLLDQ_XMM_IMM8:
    case Opcode::PSRLW_XMM_IMM8:
    case Opcode::PSRLD_XMM_IMM8:
    case Opcode::PSRLQ_XMM_IMM8:
    case Opcode::PSRLDQ_XMM_IMM8:
    case Opcode::PSRAW_XMM_IMM8:
    case Opcode::PSRAD_XMM_IMM8: {
        u8 xmm_reg = op0.reg & 15;
        u8 imm8 = static_cast<u8>(op1.imm);
        u16 shift_opid;

        // Attempt native NEON for the W/D/Q per-lane shifts (not the
        // byte-granularity PSLLDQ/PSRLDQ which shift the whole register).
        bool use_native = false;
        u32 native_enc = 0;
        arm64::VSize vsz = arm64::VS_B;
        u32 esize = 0;
        enum { SHL_, USHR_, SSHR_ } kind = SHL_;

        switch (instr.opcode) {
        case Opcode::PSLLW_XMM_IMM8:  shift_opid = 100; vsz = arm64::VS_H; esize = 16; kind = SHL_;  break;
        case Opcode::PSLLD_XMM_IMM8:  shift_opid = 101; vsz = arm64::VS_S; esize = 32; kind = SHL_;  break;
        case Opcode::PSLLQ_XMM_IMM8:  shift_opid = 102; vsz = arm64::VS_D; esize = 64; kind = SHL_;  break;
        case Opcode::PSLLDQ_XMM_IMM8: shift_opid = 103; break;
        case Opcode::PSRLW_XMM_IMM8:  shift_opid = 104; vsz = arm64::VS_H; esize = 16; kind = USHR_; break;
        case Opcode::PSRLD_XMM_IMM8:  shift_opid = 105; vsz = arm64::VS_S; esize = 32; kind = USHR_; break;
        case Opcode::PSRLQ_XMM_IMM8:  shift_opid = 106; vsz = arm64::VS_D; esize = 64; kind = USHR_; break;
        case Opcode::PSRLDQ_XMM_IMM8: shift_opid = 107; break;
        case Opcode::PSRAW_XMM_IMM8:  shift_opid = 108; vsz = arm64::VS_H; esize = 16; kind = SSHR_; break;
        case Opcode::PSRAD_XMM_IMM8:  shift_opid = 109; vsz = arm64::VS_S; esize = 32; kind = SSHR_; break;
        default: return false;
        }

        if (esize != 0) {
            bool in_range = (kind == SHL_)
                ? (imm8 < esize)
                : (imm8 >= 1 && imm8 <= esize);
            if (kind != SHL_ && imm8 == 0) {
                // ARM has no USHR/SSHR #0 encoding; x86 defines count zero
                // as identity, which the VEX source-to-destination copy has
                // already established (and legacy in-place form already is).
                use_native = true;
            } else if (in_range) {
                switch (kind) {
                case SHL_:  native_enc = arm64::shl_v_imm(0, 0, imm8, vsz); break;
                case USHR_: native_enc = arm64::ushr_v_imm(0, 0, imm8, vsz); break;
                case SSHR_: native_enc = arm64::sshr_v_imm(0, 0, imm8, vsz); break;
                }
                u32 off = vec_off(xmm_reg);
                if (fits_ldr_q_offset(off)) {
                    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off >> 4));
                    emit(native_enc);
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
                    use_native = true;
                }
            } else {
                // x86 saturates an oversized packed shift count: logical
                // shifts produce zero, while arithmetic right shifts produce
                // the sign bit in every lane. Keep this native so a 256-bit
                // lane split never needs the xmm-index-only helper.
                const u32 off = vec_off(xmm_reg);
                if (fits_ldr_q_offset(off)) {
                    if (kind == SSHR_) {
                        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off >> 4));
                        emit(arm64::sshr_v_imm(0, 0, esize, vsz));
                    } else {
                        emit(arm64::movi_v16b(0, 0));
                    }
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
                    use_native = true;
                }
            }
        }
        if (use_native) break;

        // PSLLDQ / PSRLDQ: whole-register byte shift. EXT with a zero vector.
        if (instr.opcode == Opcode::PSRLDQ_XMM_IMM8
            || instr.opcode == Opcode::PSLLDQ_XMM_IMM8) {
            const u32 off = vec_off(xmm_reg);
            if (fits_ldr_q_offset(off)) {
                if (imm8 == 0) {
                    break;
                }
                if (imm8 >= 16) {
                    emit(arm64::movi_v16b(0, 0));
                    emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
                    break;
                }
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off >> 4));
                emit(arm64::movi_v16b(1, 0));
                if (instr.opcode == Opcode::PSRLDQ_XMM_IMM8) {
                    emit(arm64::ext_v16b(0, 0, 1, imm8));
                } else {
                    emit(arm64::ext_v16b(0, 1, 0,
                                        static_cast<u8>(16u - imm8)));
                }
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off >> 4));
                break;
            }
        }

        // jit_sse_op takes a REGISTER INDEX and always means xmm[]; it cannot
        // express the high lane. Decline so a lane split cannot silently shift
        // the low half twice.
        if (m_vec_hi_lane) return false;
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::movz_w(2, shift_opid));
        emit(arm64::movz_w(3, xmm_reg));
        emit(arm64::movz_w(4, imm8));
        EMIT_SSE_OP_CALL();
        break;
    }

    // ---- Packed shifts by register (op_ids 120-135) ----
    case Opcode::PSLLW_XMM_XMMM128:
    case Opcode::PSLLD_XMM_XMMM128:
    case Opcode::PSLLQ_XMM_XMMM128:
    case Opcode::PSRLW_XMM_XMMM128:
    case Opcode::PSRLD_XMM_XMMM128:
    case Opcode::PSRLQ_XMM_XMMM128:
    case Opcode::PSRAW_XMM_XMMM128:
    case Opcode::PSRAD_XMM_XMMM128: {
        u8 dst_xmm = op0.reg & 15;
        u16 base_opid;
        switch (instr.opcode) {
        case Opcode::PSLLW_XMM_XMMM128: base_opid = 120; break;
        case Opcode::PSLLD_XMM_XMMM128: base_opid = 122; break;
        case Opcode::PSLLQ_XMM_XMMM128: base_opid = 124; break;
        case Opcode::PSRLW_XMM_XMMM128: base_opid = 126; break;
        case Opcode::PSRLD_XMM_XMMM128: base_opid = 128; break;
        case Opcode::PSRLQ_XMM_XMMM128: base_opid = 130; break;
        case Opcode::PSRAW_XMM_XMMM128: base_opid = 132; break;
        case Opcode::PSRAD_XMM_XMMM128: base_opid = 134; break;
        default: return false;
        }
        // Intel SDM Vol. 2 PSLL*/PSRL*/PSRA*: count is the low 64 bits of
        // xmm/m128. Count > lane width → 0 (logical) or sign-fill (arith).
        // USHL/SSHL with a clamped, possibly negated count matches that.
        //
        // VEX: DEST := SRC1 (vvvv) shifted by the count in r/m. The VEX.256
        // form shifts both lanes by the ONE count in xmm/m128 (Intel SDM
        // Vol. 2 VPSLLD: "COUNT := SRC2[63:0]" for every lane), so under the
        // lane split the data goes through vec_off() while the count always
        // comes from the low 128 bits -- xmm[] for a register, the first
        // qword for memory (compile_vex256_lane_split neither bumps the
        // displacement nor probes 32 bytes for these opcodes).
        if (instr.prefixes.vex_l) return false;
        const u8 src1_xmm = instr.prefixes.vex
            ? static_cast<u8>(instr.prefixes.vex_vvvv & 15) : dst_xmm;
        const u32 off_d = vec_off(dst_xmm);
        const u32 off_1 = vec_off(src1_xmm);
        // The helper arms take register indices and shift the destination in
        // place: neither a lane pass nor a VEX form.
        const bool helper_ok = !m_vec_lane_split && !instr.prefixes.vex;
        // Under the lane split the low pass writes xmm[dst] before the high
        // pass reads its count; when the count register IS the destination
        // (`vpslld ymm0, ymm1, xmm0`) the original count is parked in
        // CpuState::sse.jit_vec_scratch by the low pass and read back by the
        // high one. Nothing between the two passes touches that slot.
        const bool stage_count = m_vec_lane_split &&
            is_register_operand(op1.type) && (op1.reg & 15) == dst_xmm;
        const u32 count_scr = jit_vec_scratch_off();
        if (stage_count && !fits_ldr_x_offset(count_scr)) return false;
        const bool word =
            instr.opcode == Opcode::PSLLW_XMM_XMMM128 ||
            instr.opcode == Opcode::PSRLW_XMM_XMMM128 ||
            instr.opcode == Opcode::PSRAW_XMM_XMMM128;
        const bool quad =
            instr.opcode == Opcode::PSLLQ_XMM_XMMM128 ||
            instr.opcode == Opcode::PSRLQ_XMM_XMMM128;
        const bool arith =
            instr.opcode == Opcode::PSRAW_XMM_XMMM128 ||
            instr.opcode == Opcode::PSRAD_XMM_XMMM128;
        const bool left =
            instr.opcode == Opcode::PSLLW_XMM_XMMM128 ||
            instr.opcode == Opcode::PSLLD_XMM_XMMM128 ||
            instr.opcode == Opcode::PSLLQ_XMM_XMMM128;
        const u32 width = word ? 16u : quad ? 64u : 32u;
        const auto vsz = word ? arm64::VS_H : quad ? arm64::VS_D : arm64::VS_S;

        auto emit_shift_helper = [&](u16 opid, bool src_is_reg, u64 src_or_ea) {
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
                emit_shift_helper(base_opid, true, op1.reg & 15);
            } else {
                emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                emit_shift_helper(static_cast<u16>(base_opid + 1), false,
                                  REG_SCRATCH4);
            }
            break;
        }

        if (is_register_operand(op1.type)) {
            // The count register's LOW 128 bits, on both lane passes.
            const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(off_s)) {
                if (!helper_ok) return false;
                emit_shift_helper(base_opid, true, op1.reg & 15);
                break;
            }
            if (stage_count && m_vec_hi_lane) {
                emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, count_scr >> 3));
            } else {
                emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_s >> 3));
                if (stage_count) {
                    emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                           count_scr >> 3));
                }
            }
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
        } else {
            // Full m128 access (Intel SDM: count is still only the low qword).
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::mov_x(REG_SCRATCH1, REG_CALL_TMP));
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
        }

        emit(arm64::movz_w(REG_SCRATCH2, static_cast<u16>(width)));
        emit(arm64::cmp_x_imm(REG_SCRATCH1, width));
        emit(arm64::csel_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1,
                           arm64::CS));
        if (!left)
            emit(arm64::neg_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::dup_v_gpr(1, REG_SCRATCH2, vsz));
        if (arith)
            emit(arm64::sshl_v(0, 0, 1, vsz));
        else
            emit(arm64::ushl_v(0, 0, 1, vsz));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
