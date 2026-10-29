// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the SSE/SSE2 forms that name an MM register: PAND/POR/PXOR/
// PADDQ/PUNPCKLDQ/PUNPCKHDQ mm, MOVNTQ, MOVQ2DQ, MOVDQ2Q, CVTPI2PS,
// CVT(T)PS2PI, CVTPI2PD and CVT(T)PD2PI. compile_sse's shared tail enters
// MMX state for them.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_mmx_forms(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- MMX bitwise / PADDQ / PUNPCK*DQ — helpers 510..521. ----
    case Opcode::PAND_MM_MMM64:
    case Opcode::POR_MM_MMM64:
    case Opcode::PXOR_MM_MMM64:
    case Opcode::PADDQ_MM_MMM64:
    case Opcode::PUNPCKLDQ_MM_MMM64:
    case Opcode::PUNPCKHDQ_MM_MMM64: {
        u32 base_id;
        switch (instr.opcode) {
        case Opcode::PAND_MM_MMM64:      base_id = 510; break;
        case Opcode::POR_MM_MMM64:       base_id = 512; break;
        case Opcode::PXOR_MM_MMM64:      base_id = 514; break;
        case Opcode::PADDQ_MM_MMM64:     base_id = 516; break;
        case Opcode::PUNPCKLDQ_MM_MMM64: base_id = 518; break;
        case Opcode::PUNPCKHDQ_MM_MMM64: base_id = 520; break;
        default: base_id = 0; break;
        }
        u8 mm_dst = op0.reg & 7;
        if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, base_id));
            emit(arm64::movz_w(3, mm_dst));
            emit(arm64::movz_w(4, op1.reg & 7));
            EMIT_SSE_OP_CALL();
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, base_id + 1));
            emit(arm64::movz_w(3, mm_dst));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        // These are MM-form opcodes, so they enter MMX state too. The host
        // helper only touches the register file, not the x87 tag word.
        emit_mmx_enter();
        break;
    }

    // Intel SDM Vol. 2 MOVNTQ: 64-bit store; NT hint ignored (same as MOVQ).
    case Opcode::MOVNTQ_M64_MM: {
        if (!is_memory_operand(op0.type)) {
            return false;
        }
        emit_load_mm(REG_SCRATCH0, op1.reg);
        emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
        emit_guest_store64(REG_SCRATCH0, REG_SCRATCH4);
        break;
    }

    // Intel SDM Vol. 2 MOVQ2DQ: DEST[63:0] := MM; DEST[127:64] := 0.
    case Opcode::MOVQ2DQ_XMM_MM: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_lo = xmm_u64_off(dst_xmm, 0);
        const u32 off_hi = xmm_u64_off(dst_xmm, 1);
        if (!fits_ldr_x_offset(off_lo) || !fits_ldr_x_offset(off_hi)) {
            return false;
        }
        emit_load_mm(REG_SCRATCH0, op1.reg);
        // Do NOT store the MM source back: emit_store_mm forces its
        // exponent/sign to 0xFFFF, rewriting a register this instruction only
        // reads. Entering MMX state is emit_mmx_enter()'s job, not a bogus
        // write's. Mirrors the interpreter fix in ops/mmx.cpp.
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_lo >> 3));
        emit(arm64::str_x_uoff(31, REG_CPU_STATE, off_hi >> 3));
        emit_mmx_enter();
        break;
    }

    // Intel SDM Vol. 2 MOVDQ2Q: MM := XMM[63:0].
    case Opcode::MOVDQ2Q_MM_XMM: {
        const u32 off_lo = xmm_u64_off(op1.reg & 15, 0);
        if (!fits_ldr_x_offset(off_lo)) {
            return false;
        }
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_lo >> 3));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        emit_mmx_enter();
        break;
    }

    // Intel SDM Vol. 2 CVTPI2PS: two packed i32 (mm/m64) → two f32 in low
    // 64 of XMM; high 64 of dest unmodified. Match interpreter (C++ cast /
    // SCVTF; no MMX tag write on the source read).
    case Opcode::CVTPI2PS_XMM_MMM64: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_lo = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_x_offset(off_lo)) return false;
        if (op1.type == OperandType::RegMM) {
            emit_load_mm(REG_SCRATCH0, op1.reg);
        } else if (is_memory_operand(op1.type)) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            return false;
        }
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 32));
        // SCVTF Sd, Wn — ARM ARM C6.2.244 (single from signed 32).
        emit(0x1E220000u | (static_cast<u32>(REG_SCRATCH0) << 5) | 0u);
        emit(0x1E220000u | (static_cast<u32>(REG_SCRATCH1) << 5) | 1u);
        // FMOV Wd, Sn
        emit(0x1E260000u | (0u << 5) | static_cast<u32>(REG_SCRATCH0));
        emit(0x1E260000u | (1u << 5) | static_cast<u32>(REG_SCRATCH1));
        emit(arm64::orr_x_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1, 32));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_lo >> 3));
        break;
    }

    // Intel SDM Vol. 2 CVTPS2PI / CVTTPS2PI: two f32 (xmm/m64 low) → two i32
    // in MM. Indefinite 0x80000000 on overflow/NaN (match interpreter).
    case Opcode::CVTPS2PI_MM_XMMM64:
    case Opcode::CVTTPS2PI_MM_XMMM64: {
        if (op0.type != OperandType::RegMM) return false;
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_x_offset(src_off)) return false;
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        } else if (is_memory_operand(op1.type)) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            return false;
        }
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 32));
        const bool truncate = (instr.opcode == Opcode::CVTTPS2PI_MM_XMMM64);
        auto emit_f32_to_i32 = [&](u8 dst_w, u8 src_w) {
            emit(arm64::fmov_s_from_w(0, src_w));
            // FCMP S0, S0 — unordered (NaN) sets V.
            emit(0x1E202000u | (0u << 16) | (0u << 5));
            const u32 b_nan = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm32(REG_SCRATCH2, 0x4F000000u);  // 2^31 as f32
            emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
            emit(0x1E202000u | (1u << 16) | (0u << 5));
            const u32 b_ge = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm32(REG_SCRATCH2, 0xCF000000u);  // INT32_MIN as f32
            emit(arm64::fmov_s_from_w(1, REG_SCRATCH2));
            emit(0x1E202000u | (1u << 16) | (0u << 5));
            const u32 b_lt = static_cast<u32>(m_code_buf.size());
            emit(0);
            // FCVTZS Wd, Sn (truncate) / FCVTNS Wd, Sn (nearest-even).
            // ARM: FCVTNS=0x1E200000, FCVTMS=0x1E300000, FCVTZS=0x1E380000 (Rn=0).
            if (truncate) {
                     emit(0x1E380000u | static_cast<u32>(dst_w));
                 } else {
                     // CVTSS2SI/CVTSD2SI round per MXCSR.RC; only the
                     // CVTT* forms truncate unconditionally.
                     emit_fcvt_scalar_by_mxcsr(0x1E200000u | static_cast<u32>(dst_w));
                 }
            emit(0x1E220000u | (static_cast<u32>(dst_w) << 5) | 1u);
            emit(0x1E202000u | (1u << 16) | (0u << 5));
            const u32 b_exact = static_cast<u32>(m_code_buf.size());
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
            const u32 b_done = static_cast<u32>(m_code_buf.size());
            emit(0);
            const u32 indef = static_cast<u32>(m_code_buf.size());
            emit_mov_imm32(dst_w, 0x80000000u);
            const u32 done = static_cast<u32>(m_code_buf.size());
            m_code_buf[b_nan] = arm64::b_cond(
                arm64::VS, static_cast<i32>(indef - b_nan) * 4);
            m_code_buf[b_ge] = arm64::b_cond(
                arm64::GE, static_cast<i32>(indef - b_ge) * 4);
            m_code_buf[b_lt] = arm64::b_cond(
                arm64::LT, static_cast<i32>(indef - b_lt) * 4);
            m_code_buf[b_exact] = arm64::b_cond(
                arm64::EQ, static_cast<i32>(b_done - b_exact) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done - b_done) * 4);
        };
        emit_f32_to_i32(REG_SCRATCH0, REG_SCRATCH0);
        emit_f32_to_i32(REG_SCRATCH1, REG_SCRATCH1);
        emit(arm64::orr_x_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1, 32));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    // Intel SDM Vol. 2 CVTPI2PD: two packed i32 (mm/m64) → two f64 in XMM
    // (writes all 128 bits). Match interpreter (SCVTF; no MMX tag write on
    // the source read).
    case Opcode::CVTPI2PD_XMM_MMM64: {
        const u8 dst_xmm = op0.reg & 15;
        const u32 off_lo = xmm_u64_off(dst_xmm, 0);
        const u32 off_hi = xmm_u64_off(dst_xmm, 1);
        if (!fits_ldr_x_offset(off_lo) || !fits_ldr_x_offset(off_hi)) {
            return false;
        }
        if (op1.type == OperandType::RegMM) {
            emit_load_mm(REG_SCRATCH0, op1.reg);
        } else if (is_memory_operand(op1.type)) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            return false;
        }
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 32));
        emit(arm64::scvtf_d_from_w(0, REG_SCRATCH0));
        emit(arm64::scvtf_d_from_w(1, REG_SCRATCH1));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::fmov_x_from_d(REG_SCRATCH1, 1));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off_lo >> 3));
        emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off_hi >> 3));
        break;
    }

    // Intel SDM Vol. 2 CVTPD2PI / CVTTPD2PI: two f64 (xmm/m128) → two i32
    // in MM. Indefinite 0x80000000 on overflow/NaN (match interpreter).
    case Opcode::CVTPD2PI_MM_XMMM128:
    case Opcode::CVTTPD2PI_MM_XMMM128: {
        if (op0.type != OperandType::RegMM) return false;
        if (is_register_operand(op1.type)) {
            const u32 src_lo = xmm_u64_off(op1.reg & 15, 0);
            const u32 src_hi = xmm_u64_off(op1.reg & 15, 1);
            if (!fits_ldr_x_offset(src_lo) || !fits_ldr_x_offset(src_hi)) {
                return false;
            }
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_lo >> 3));
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, src_hi >> 3));
        } else if (m_block_is_64bit) {
            return false;
        } else if (is_memory_operand(op1.type)) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH4, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::mov_x(REG_SCRATCH0, REG_CALL_TMP));
        } else {
            return false;
        }
        const bool truncate = (instr.opcode == Opcode::CVTTPD2PI_MM_XMMM128);
        auto emit_f64_to_i32 = [&](u8 dst_w, u8 src_x) {
            emit(arm64::fmov_d_from_x(0, src_x));
            emit(arm64::fcmp_d(0, 0));
            const u32 b_nan = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm64(REG_SCRATCH2, 0x41E0000000000000LL);
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
            emit(arm64::fcmp_d(0, 1));
            const u32 b_ge = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0xC1E0000000000000ULL));
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH2));
            emit(arm64::fcmp_d(0, 1));
            const u32 b_lt = static_cast<u32>(m_code_buf.size());
            emit(0);
            if (truncate) {
                     emit(0x1E780000u | static_cast<u32>(dst_w));
                 } else {
                     // CVTSS2SI/CVTSD2SI round per MXCSR.RC; only the
                     // CVTT* forms truncate unconditionally.
                     emit_fcvt_scalar_by_mxcsr(0x1E600000u | static_cast<u32>(dst_w));
                 }
            const u32 b_done = static_cast<u32>(m_code_buf.size());
            emit(0);
            const u32 indef = static_cast<u32>(m_code_buf.size());
            emit_mov_imm32(dst_w, 0x80000000u);
            const u32 done = static_cast<u32>(m_code_buf.size());
            m_code_buf[b_nan] = arm64::b_cond(
                arm64::VS, static_cast<i32>(indef - b_nan) * 4);
            m_code_buf[b_ge] = arm64::b_cond(
                arm64::GE, static_cast<i32>(indef - b_ge) * 4);
            m_code_buf[b_lt] = arm64::b_cond(
                arm64::LT, static_cast<i32>(indef - b_lt) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done - b_done) * 4);
        };
        emit_f64_to_i32(REG_SCRATCH0, REG_SCRATCH0);
        emit_f64_to_i32(REG_SCRATCH1, REG_SCRATCH1);
        emit(arm64::orr_x_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1, 32));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
