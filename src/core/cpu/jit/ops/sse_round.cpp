// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — ROUNDPS/ROUNDPD/ROUNDSS/ROUNDSD and their helpers.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

// Intel SDM Vol. 2 ROUNDPS/ROUNDPD/ROUNDSS/ROUNDSD, including MXCSR.PE.
// packed: kind[1:0] dst_xmm[5:2] imm8[15:8] is_mem[16]
// kinds: 0 ROUNDPS, 1 ROUNDPD, 2 ROUNDSS, 3 ROUNDSD
extern "C" void jit_round_op(CpuState* cpu, GuestMemory* mem,
                             u64 src2, u32 packed) noexcept {
    if (cpu == nullptr || mem == nullptr) {
        return;
    }

    static constexpr Opcode kOps[4] = {
        Opcode::ROUNDPS_XMM_XMMM128_IMM8,
        Opcode::ROUNDPD_XMM_XMMM128_IMM8,
        Opcode::ROUNDSS_XMM_XMMM32_IMM8,
        Opcode::ROUNDSD_XMM_XMMM64_IMM8,
    };

    const u32 kind = packed & 3u;
    const u8 dst_xmm = static_cast<u8>((packed >> 2) & 15u);
    const u8 imm = static_cast<u8>(packed >> 8);
    const bool is_mem = (packed & (1u << 16)) != 0;
    const bool from_vex = (packed & (1u << 17)) != 0;

    DecodedInstruction di{};
    di.opcode = kOps[kind];
    di.operands[0] = Operand::make_reg(OperandType::Reg128, dst_xmm);
    di.operands[2] = Operand::make_imm(OperandType::Imm8, imm);
    di.operand_count = 3;

    constexpr u8 kEaReg = static_cast<u8>(RegIndex::RBX);
    u64 saved_rbx = 0;
    if (is_mem) {
        saved_rbx = cpu->gpr.get64(kEaReg);
        cpu->gpr.set64(kEaReg, src2);
        MemoryOperand mop{};
        mop.base = kEaReg;
        di.operands[1] = Operand::make_mem(OperandType::Mem128, mop);
    } else {
        di.operands[1] = Operand::make_reg(OperandType::Reg128,
                                           static_cast<u8>(src2 & 15u));
    }

    ExecContext ctx{*cpu, *mem};
    ctx.vex_origin = from_vex;
    const ExecResult result = ::vitreo::cpu::ops::exec_sse4_ops(ctx, di);

    if (is_mem) {
        cpu->gpr.set64(kEaReg, saved_rbx);
    }

    if (result == ExecResult::Exception && !cpu->pending_raise.active) {
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.from_helper = true;
        pending.code = 0xC0000005u;
        pending.flags = 0;
        pending.num_args = 2;
        pending.args[0] = 0;
        pending.args[1] = is_mem ? src2 : 0;
    }
}

void JitCompiler::emit_mxcsr_or_flag(u32 flag) {
    const u32 mxcsr_off = static_cast<u32>(
        offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
    emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
    emit_mov_imm32(REG_SCRATCH3, flag);
    emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
    emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE, mxcsr_off >> 2));
}

void JitCompiler::emit_round_ie_if_snan_lane0(bool dbl) {
    // SNaN: exponent all ones, quiet bit clear, payload non-zero. Flag-free:
    // (bits & (exp|quiet)) ^ exp == 0  and  (bits & payload) != 0.
    if (dbl) {
        emit(arm64::umov_x_d(REG_SCRATCH0, 1, 0));
        emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0x7FF8000000000000ull));
        emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2));
        emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0x7FF0000000000000ull));
        emit(arm64::eor_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
    } else {
        emit(arm64::umov_w_s(REG_SCRATCH0, 1, 0));
        emit_mov_imm32(REG_SCRATCH2, 0x7FC00000u);
        emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2));
        emit_mov_imm32(REG_SCRATCH2, 0x7F800000u);
        emit(arm64::eor_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
    }
    const usize not_nan = m_code_buf.size(); emit(0);
    if (dbl) {
        emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0x0007FFFFFFFFFFFFull));
        emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2));
    } else {
        emit_mov_imm32(REG_SCRATCH2, 0x003FFFFFu);
        emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2));
    }
    const usize no_payload = m_code_buf.size(); emit(0);
    emit_mxcsr_or_flag(MXCSR_IE);
    const usize done = m_code_buf.size();
    auto rel = [](usize from, usize to) { return static_cast<i32>(to - from) * 4; };
    m_code_buf[not_nan] = dbl ? arm64::cbnz_x(REG_SCRATCH3, rel(not_nan, done))
                              : arm64::cbnz_w(REG_SCRATCH3, rel(not_nan, done));
    m_code_buf[no_payload] = dbl ? arm64::cbz_x(REG_SCRATCH3, rel(no_payload, done))
                                 : arm64::cbz_w(REG_SCRATCH3, rel(no_payload, done));
}

bool JitCompiler::emit_round_scalar(const DecodedInstruction& instr,
                                    u64 next_rip, u8 src1_xmm,
                                    bool zero_upper) {
    // Same-build A/B and bisect knob: the legacy form falls back to the
    // jit_round_op helper, the VEX form to the interpreter.
    static const bool disabled =
        std::getenv("VITREO_JIT_NO_NATIVE_ROUND_SCALAR") != nullptr;
    if (disabled) return false;
    // Intel SDM Vol. 2 ROUNDSS/ROUNDSD:
    //   DEST[31:0|63:0]   := RoundToInteger(SRC2, imm8)
    //   DEST[127:32|64]   := SRC1[127:32|64]   (legacy: SRC1 is DEST itself)
    //   DEST[MAXVL-1:128] := 0                 (VEX only)
    // imm8[1:0] = mode (00 nearest-even, 01 down, 10 up, 11 toward zero),
    // imm8[2] = use MXCSR.RC instead, imm8[3] = suppress the precision
    // exception. An inexact finite source sets MXCSR.PE (unless suppressed);
    // an SNaN source sets MXCSR.IE regardless of imm8[3] and is quietened.
    // Every value, flag and merge below is pinned by
    // tools/oracle_probe/vex_scalar_round_probe.expected (i9-12900KF).
    //
    // ARM FRINTN/M/P/Z (scalar) are exactly the four modes, independent of
    // FPCR.RMode, quieten an SNaN as x86 does, and keep the sign of zero.
    if (instr.operand_count < 3) return false;
    if (instr.prefixes.vex_l) return false;
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    if (op0.type != OperandType::Reg128) return false;
    const bool dbl = instr.opcode == Opcode::ROUNDSD_XMM_XMMM64_IMM8;
    if (!dbl && instr.opcode != Opcode::ROUNDSS_XMM_XMMM32_IMM8) return false;
    const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;
    const arm64::VSize vs = dbl ? arm64::VS_D : arm64::VS_S;
    const u8 imm = static_cast<u8>(instr.operands[2].imm);
    const u8 dst_xmm = op0.reg & 15;
    const u32 off_d = xmm_u64_off(dst_xmm, 0);
    const u32 off_1 = xmm_u64_off(src1_xmm & 15, 0);
    const u32 off_hi = ymm_high_u64_off(dst_xmm, 0);
    const bool src_reg = is_register_operand(op1.type);
    if (!src_reg && !is_memory_operand(op1.type)) return false;
    const u32 off_s = src_reg ? xmm_u64_off(op1.reg & 15, 0) : 0;
    if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1) ||
        (src_reg && !fits_ldr_q_offset(off_s)) ||
        (zero_upper && !fits_ldr_q_offset(off_hi))) {
        return false;
    }
    const u32 mxcsr_off = static_cast<u32>(
        offsetof(CpuState, sse) + offsetof(SseState, mxcsr));

    // Only the memory source's guest load can clobber NZCV; FRINT, FCMEQ,
    // INS/UMOV, the GPR mask tests and CBZ/CBNZ leave it alone.
    const bool hold = !src_reg && should_hold_nzcv();
    if (hold) emit_nzcv_spill();
    if (src_reg) {
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
    } else {
        // Source first: the guest load may BLR and V0-V7 are caller-saved.
        emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
        emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
        if (dbl) {
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH0));
        } else {
            emit_guest_load32(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::fmov_s_from_w(1, REG_SCRATCH0));
        }
    }
    auto frint = [&](u8 mode) {
        switch (mode & 3) {
        case 0: emit(arm64::frintn_s(0, 1, fs)); break;
        case 1: emit(arm64::frintm_s(0, 1, fs)); break;
        case 2: emit(arm64::frintp_s(0, 1, fs)); break;
        default: emit(arm64::frintz_s(0, 1, fs)); break;
        }
    };
    if (imm & 4) {
        // MXCSR.RC (bits 14:13) is only known at run time: TBNZ ladder,
        // same shape as the packed ROUNDPS/PD arm.
        emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, mxcsr_off >> 2));
        const usize to_hi = m_code_buf.size(); emit(0);   // RC 1x
        const usize to_m = m_code_buf.size(); emit(0);    // RC 01
        frint(0);
        const usize n_done = m_code_buf.size(); emit(0);
        const usize m_pos = m_code_buf.size();
        frint(1);
        const usize m_done = m_code_buf.size(); emit(0);
        const usize hi_pos = m_code_buf.size();
        const usize to_z = m_code_buf.size(); emit(0);    // RC 11
        frint(2);
        const usize p_done = m_code_buf.size(); emit(0);
        const usize z_pos = m_code_buf.size();
        frint(3);
        const usize done = m_code_buf.size();
        auto rel = [](usize from, usize to) {
            return static_cast<i32>(to - from) * 4;
        };
        m_code_buf[to_hi] = arm64::tbnz(REG_SCRATCH0, 14, rel(to_hi, hi_pos));
        m_code_buf[to_m] = arm64::tbnz(REG_SCRATCH0, 13, rel(to_m, m_pos));
        m_code_buf[to_z] = arm64::tbnz(REG_SCRATCH0, 13, rel(to_z, z_pos));
        m_code_buf[n_done] = arm64::b_imm(rel(n_done, done));
        m_code_buf[m_done] = arm64::b_imm(rel(m_done, done));
        m_code_buf[p_done] = arm64::b_imm(rel(p_done, done));
    } else {
        frint(imm);
    }
    if ((imm & 8) == 0) {
        // PE when a finite source changed. Decided on the BIT PATTERNS of
        // lane 0, not with a vector FCMEQ: V1 still carries the rest of the
        // source register, and an FCMEQ over an SNaN in a lane this
        // instruction never reads raises the host's invalid-operation flag,
        // which the MXCSR harvest (math/host_fenv.hpp) then reports as IE.
        // For a finite source "changed" and "bits differ" coincide (FRINT
        // keeps the sign of zero), and a non-finite source is never inexact.
        if (dbl) {
            emit(arm64::umov_x_d(REG_SCRATCH0, 1, 0));
            emit(arm64::umov_x_d(REG_SCRATCH1, 0, 0));
            emit(arm64::eor_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH1));
        } else {
            emit(arm64::umov_w_s(REG_SCRATCH0, 1, 0));
            emit(arm64::umov_w_s(REG_SCRATCH1, 0, 0));
            emit(arm64::eor_w(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH1));
        }
        const usize unchanged = m_code_buf.size(); emit(0);
        if (dbl) {
            emit_mov_imm64(REG_SCRATCH2, static_cast<i64>(0x7FF0000000000000ull));
            emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2));
            emit(arm64::eor_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        } else {
            emit_mov_imm32(REG_SCRATCH2, 0x7F800000u);
            emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2));
            emit(arm64::eor_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        }
        const usize non_finite = m_code_buf.size(); emit(0);
        emit_mxcsr_or_flag(MXCSR_PE);
        const usize pe_done = m_code_buf.size();
        auto rel = [](usize from, usize to) {
            return static_cast<i32>(to - from) * 4;
        };
        m_code_buf[unchanged] = dbl
            ? arm64::cbz_x(REG_SCRATCH3, rel(unchanged, pe_done))
            : arm64::cbz_w(REG_SCRATCH3, rel(unchanged, pe_done));
        m_code_buf[non_finite] = dbl
            ? arm64::cbz_x(REG_SCRATCH3, rel(non_finite, pe_done))
            : arm64::cbz_w(REG_SCRATCH3, rel(non_finite, pe_done));
    }
    emit_round_ie_if_snan_lane0(dbl);
    // Merge: SRC1 supplies everything above the rounded element. SRC1 is
    // read after the source and before the store, so dst == r/m and
    // src1 == r/m are both safe.
    emit(arm64::ldr_q_uoff(4, REG_CPU_STATE, off_1 >> 4));
    emit(arm64::ins_v_elem(4, 0, 0, 0, vs));
    emit(arm64::str_q_uoff(4, REG_CPU_STATE, off_d >> 4));
    if (zero_upper) {
        emit(arm64::eor_v16b(5, 5, 5));
        emit(arm64::str_q_uoff(5, REG_CPU_STATE, off_hi >> 4));
    }
    if (hold) emit_nzcv_fill();
    return true;
}

std::optional<bool> JitCompiler::compile_sse_round(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // Intel SDM Vol. 2 ROUNDPS/PD/SS/SD. The packed forms are native; the
    // scalar ones (DEST[127:32/64] merge) keep the helper → exec_sse4_ops.
    case Opcode::ROUNDPS_XMM_XMMM128_IMM8:
    case Opcode::ROUNDPD_XMM_XMMM128_IMM8:
    case Opcode::ROUNDSS_XMM_XMMM32_IMM8:
    case Opcode::ROUNDSD_XMM_XMMM64_IMM8: {
        if (instr.operand_count < 3) return false;
        if (instr.opcode == Opcode::ROUNDPS_XMM_XMMM128_IMM8 ||
            instr.opcode == Opcode::ROUNDPD_XMM_XMMM128_IMM8) {
            // ROUNDPS/PD: each element := RoundToInteger(SRC, mode), where
            // mode is imm8[1:0] (00 nearest-even, 01 down, 10 up, 11 toward
            // zero) unless imm8[2] selects MXCSR.RC; imm8[3] suppresses the
            // Precision exception. ARM FRINTN/M/P/Z are exactly the four
            // modes, independent of FPCR.RMode, and quieten an SNaN just as
            // x86 does. Unary (VEX.vvvv is 1111b), every operand via
            // vec_off() and no helper arm, so VEX.256 runs it per lane
            // through compile_vex256_lane_split with the same imm8.
            if (instr.prefixes.vex_l) return false;
            const bool dbl = instr.opcode == Opcode::ROUNDPD_XMM_XMMM128_IMM8;
            const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;
            const u8 imm = static_cast<u8>(instr.operands[2].imm);
            const u8 dst_xmm = op0.reg & 15;
            const u32 off_d = vec_off(dst_xmm);
            if (!fits_ldr_q_offset(off_d)) return false;
            const bool src_reg = is_register_operand(op1.type);
            if (!src_reg && !is_memory_operand(op1.type)) return false;
            const u32 off_s = src_reg ? vec_off(op1.reg & 15) : 0;
            if (src_reg && !fits_ldr_q_offset(off_s)) return false;
            const u32 mxcsr_off = static_cast<u32>(
                offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
            // TBZ/TBNZ, FRINT*, FCMEQ, UMAXV and the MXCSR update leave NZCV
            // alone; only the memory source's guest loads clobber it.
            const bool hold = !src_reg && should_hold_nzcv();
            if (hold) emit_nzcv_spill();
            if (src_reg) {
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
            } else {
                emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_CALL_TMP, instr, op1);
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::ins_v_gpr(1, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(1, 1, REG_SCRATCH0, arm64::VS_D));
            }
            auto frint = [&](u8 mode) {
                switch (mode & 3) {
                case 0: emit(arm64::frintn_v(0, 1, fs)); break;
                case 1: emit(arm64::frintm_v(0, 1, fs)); break;
                case 2: emit(arm64::frintp_v(0, 1, fs)); break;
                default: emit(arm64::frintz_v(0, 1, fs)); break;
                }
            };
            if (imm & 4) {
                // MXCSR.RC is bits 14:13 and is only known at run time.
                emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       mxcsr_off >> 2));
                const usize to_hi = m_code_buf.size(); emit(0);   // RC 1x
                const usize to_m = m_code_buf.size(); emit(0);    // RC 01
                frint(0);
                const usize n_done = m_code_buf.size(); emit(0);
                const usize m_pos = m_code_buf.size();
                frint(1);
                const usize m_done = m_code_buf.size(); emit(0);
                const usize hi_pos = m_code_buf.size();
                const usize to_z = m_code_buf.size(); emit(0);    // RC 11
                frint(2);
                const usize p_done = m_code_buf.size(); emit(0);
                const usize z_pos = m_code_buf.size();
                frint(3);
                const usize done = m_code_buf.size();
                auto rel = [](usize from, usize to) {
                    return static_cast<i32>(to - from) * 4;
                };
                m_code_buf[to_hi] = arm64::tbnz(REG_SCRATCH0, 14, rel(to_hi, hi_pos));
                m_code_buf[to_m] = arm64::tbnz(REG_SCRATCH0, 13, rel(to_m, m_pos));
                m_code_buf[to_z] = arm64::tbnz(REG_SCRATCH0, 13, rel(to_z, z_pos));
                m_code_buf[n_done] = arm64::b_imm(rel(n_done, done));
                m_code_buf[m_done] = arm64::b_imm(rel(m_done, done));
                m_code_buf[p_done] = arm64::b_imm(rel(p_done, done));
            } else {
                frint(imm);
            }
            if ((imm & 8) == 0) {
                // MXCSR.PE (sticky; masked by default) when any finite
                // element changed. A NaN lane is never "inexact": mask the
                // inequality with SRC == SRC (false only for NaN). ±Inf
                // rounds to itself and compares equal.
                emit(arm64::fcmeq_v(2, 0, 1, fs));
                emit(arm64::fcmeq_v(3, 1, 1, fs));
                emit(arm64::bic_v16b(2, 3, 2));
                emit(arm64::umaxv_v(2, 2, arm64::VS_B));
                emit(arm64::umov_w_b(REG_SCRATCH0, 2, 0));
                const usize skip = m_code_buf.size(); emit(0);
                emit(arm64::ldr_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
                emit_mov_imm32(REG_SCRATCH3, MXCSR_PE);
                emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH3));
                emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE,
                                       mxcsr_off >> 2));
                m_code_buf[skip] = arm64::cbz_w(
                    REG_SCRATCH0, static_cast<i32>(m_code_buf.size() - skip) * 4);
            }
            // MXCSR.IE when any source lane is an SNaN, whatever imm8[3]
            // says (measured: tools/oracle_probe/vex_scalar_round_probe.c).
            // SNaN lane = NaN (FCMEQ x,x false) with the quiet bit clear.
            emit(arm64::fcmeq_v(3, 1, 1, fs));
            emit(arm64::not_v16b(3, 3));
            if (dbl) {
                emit_mov_imm64(REG_SCRATCH0,
                               static_cast<i64>(0x0008000000000000ull));
                emit(arm64::dup_v_gpr(2, REG_SCRATCH0, arm64::VS_D));
            } else {
                emit_mov_imm32(REG_SCRATCH0, 0x00400000u);
                emit(arm64::dup_v_gpr(2, REG_SCRATCH0, arm64::VS_S));
            }
            emit(arm64::and_v16b(2, 1, 2));
            emit(arm64::cmeq_v_zero(2, 2, dbl ? arm64::VS_D : arm64::VS_S));
            emit(arm64::and_v16b(2, 2, 3));
            emit(arm64::umaxv_v(2, 2, arm64::VS_B));
            emit(arm64::umov_w_b(REG_SCRATCH0, 2, 0));
            {
                const usize skip_ie = m_code_buf.size(); emit(0);
                emit_mxcsr_or_flag(MXCSR_IE);
                m_code_buf[skip_ie] = arm64::cbz_w(
                    REG_SCRATCH0,
                    static_cast<i32>(m_code_buf.size() - skip_ie) * 4);
            }
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
            if (hold) emit_nzcv_fill();
            break;
        }
        // Scalar forms: the helper path takes register indices (always
        // xmm[]) and has no lane meaning.
        if (m_vec_lane_split) return false;
        // Native first (legacy form: DEST[127:32|64] is DEST's own). The
        // VEX form never reaches here -- the bridge at the top of this
        // function runs emit_round_scalar with SRC1 = vvvv. Used to be a
        // jit_round_op helper (BLR + pin spill + a DecodedInstruction
        // rebuilt for exec_sse4_ops) on every JavaScript Math.floor/ceil/round.
        if (emit_round_scalar(instr, next_rip, op0.reg & 15,
                              /*zero_upper=*/false)) {
            break;
        }
        u32 kind = 0;
        switch (instr.opcode) {
        case Opcode::ROUNDPD_XMM_XMMM128_IMM8: kind = 1; break;
        case Opcode::ROUNDSS_XMM_XMMM32_IMM8: kind = 2; break;
        case Opcode::ROUNDSD_XMM_XMMM64_IMM8: kind = 3; break;
        default: break;
        }
        const u8 dst_xmm = op0.reg & 15;
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        u32 packed = kind | (static_cast<u32>(dst_xmm) << 2)
                     | (static_cast<u32>(imm) << 8);

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_memory_operand(op1.type)) {
            packed |= (1u << 16);
            // Bit 17 carries the guest encoding's VEX-ness across the helper
            // boundary. Without it the helper rebuilds a LEGACY-shaped
            // DecodedInstruction from a fixed opcode table, the interpreter
            // inside computes `aligned_form = true`, and a VEX form that
            // architecturally permits a misaligned operand raises #GP. Same
            // class of defect as the VPXOR fault that terminates a launcher, one layer
            // further out: there the bridge cleared prefixes.vex, here the
            // helper never had it.
            if (instr.prefixes.vex || m_current_original_vex)
                packed |= (1u << 17);
            emit_update_rip(instr_rip);
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH4));
        } else if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit_mov_imm32(2, op1.reg & 15);
        } else {
            return false;
        }
        emit_mov_imm32(3, packed);
        EMIT_SSE_MEM_HELPER_CALL(jit_round_op);
        emit_exit_if_pending_raise();
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
