// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: the condition-code producers. FXAM, FCOM/
// FCOMP/FUCOM/FUCOMP, FICOM/FICOMP, FCOMPP/FUCOMPP, FTST and FCOMI/
// FUCOMI/FCOMIP/FUCOMIP (Intel SDM Vol. 1 8.3.6).
//
// One family of compile_fpu_native's native lowerings, split out of
// ops/fpu.cpp (ADR-0006) as pure code motion: the blocks are verbatim and in
// their original order. compile_fpu_native (ops/fpu.cpp) owns the dispatch
// chain and the jit_fpu_op helper fallback; the declaration in
// jit_compiler.hpp states what the nullopt return means.

#include "../jit_helpers.hpp"
#include "fpu_internal.hpp"

#include <optional>
#include <vector>

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_fpu_compare(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const bool native_fpu_arith = native_arith_allowed(instr.opcode);

    const auto& op0 = instr.operands[0];

    // Intel SDM Vol. 2 FXAM: C1 = sign; C3/C2/C0 classify ST(0).
    if (instr.opcode == Opcode::FXAM) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 11, 3));
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_EA_TMP, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH2, 0));
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_EA_TMP, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_EA_TMP,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_EA_TMP, REG_SCRATCH4, 31));
        emit_mov_imm32(REG_SCRATCH4,
                       FPU_SW_C0 | FPU_SW_C1 | FPU_SW_C2 | FPU_SW_C3);
        emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_EA_TMP, 15));
        emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH2, 9));
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FFF));
        emit(arm64::and_w(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH4));
        const usize zero_exp = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, 0x7FFF);
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize infnan_br = m_code_buf.size();
        emit(0);
        const usize normal_pos = m_code_buf.size();
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C2);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize class_done_br = m_code_buf.size();
        emit(0);
        const usize zero_exp_pos = m_code_buf.size();
        const usize mant_nz = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C3);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize zero_done = m_code_buf.size();
        emit(0);
        const usize infnan_pos = m_code_buf.size();
        emit_mov_imm64(REG_SCRATCH4,
                       static_cast<i64>(0x8000000000000000ULL));
        emit(arm64::subs_x(31, REG_SCRATCH0, REG_SCRATCH4));
        const usize inf_br = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C0);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize nan_done = m_code_buf.size();
        emit(0);
        const usize inf_pos = m_code_buf.size();
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C0 | FPU_SW_C2);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize class_done = m_code_buf.size();
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        m_code_buf[zero_exp] = arm64::cbz_w(
            REG_SCRATCH2, static_cast<i32>(zero_exp_pos - zero_exp) * 4);
        m_code_buf[infnan_br] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(infnan_pos - infnan_br) * 4);
        m_code_buf[class_done_br] = arm64::b_imm(
            static_cast<i32>(class_done - class_done_br) * 4);
        m_code_buf[mant_nz] = arm64::cbnz_x(
            REG_SCRATCH0, static_cast<i32>(normal_pos - mant_nz) * 4);
        m_code_buf[zero_done] = arm64::b_imm(
            static_cast<i32>(class_done - zero_done) * 4);
        m_code_buf[inf_br] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(inf_pos - inf_br) * 4);
        m_code_buf[nan_done] = arm64::b_imm(
            static_cast<i32>(class_done - nan_done) * 4);
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Intel SDM Vol. 2 FCOM/FCOMP/FUCOM: C3/C2/C0 from ST(0) ? SRC.
    // Finite normals use FCMP; NaN/Inf stay on exec_fpu.
    if (native_fpu_arith &&
        (instr.opcode == Opcode::FCOM_STI ||
         instr.opcode == Opcode::FCOMP_STI ||
         instr.opcode == Opcode::FUCOM_STI ||
         instr.opcode == Opcode::FUCOMP_STI ||
         instr.opcode == Opcode::FCOM_M32 ||
         instr.opcode == Opcode::FCOMP_M32 ||
         instr.opcode == Opcode::FCOM_M64 ||
         instr.opcode == Opcode::FCOMP_M64 ||
         instr.opcode == Opcode::FICOM_M16 ||
         instr.opcode == Opcode::FICOM_M32 ||
         instr.opcode == Opcode::FICOMP_M16 ||
         instr.opcode == Opcode::FICOMP_M32 ||
         instr.opcode == Opcode::FCOMPP ||
         instr.opcode == Opcode::FUCOMPP ||
         instr.opcode == Opcode::FTST)) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 double_exp_bias_delta = 16383u - 1023u;
        constexpr u32 min_double_x87_exp = double_exp_bias_delta + 1u;
        constexpr u32 max_double_x87_exp = double_exp_bias_delta + 0x7FEu;
        std::vector<usize> slow_below_branches;
        std::vector<usize> slow_above_branches;
        // FCOM/FCOMP/FCOMPP leave integer EFLAGS unchanged. Float80
        // reconstruction uses SUBS and the comparison itself uses FCMP, so
        // preserve a pending guest condition across both native and helper
        // paths.
        const bool hold_nzcv = should_hold_nzcv();
        if (hold_nzcv) emit_nzcv_spill();

        const auto emit_st_addr = [&](u8 logical_index) {
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
            emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
            emit(arm64::movz_w(REG_SCRATCH4, logical_index));
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_w(REG_SCRATCH4, 7));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
            emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
            emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                                 REG_SCRATCH2, 0));
        };
        const auto emit_st_as_double = [&](u8 logical_index, u8 fp_reg) {
            emit_st_addr(logical_index);
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                                 offsetof(Float80, exponent_sign)));
            emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
            emit(arm64::movz_w(REG_SCRATCH4, 0x7FFF));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH1, REG_SCRATCH4));
            const usize zero_branch = m_code_buf.size();
            emit(0);
            emit_mov_imm32(REG_SCRATCH4, min_double_x87_exp);
            emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
            slow_below_branches.push_back(m_code_buf.size());
            emit(0);
            emit_mov_imm32(REG_SCRATCH4, max_double_x87_exp);
            emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
            slow_above_branches.push_back(m_code_buf.size());
            emit(0);
            emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));
            emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 12));
            emit_mov_imm32(REG_EA_TMP, double_exp_bias_delta);
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
            emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 52));
            emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 63));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH2));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            const usize ready_branch = m_code_buf.size();
            emit(0);
            const usize zero_pos = m_code_buf.size();
            emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
            emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH1, 63));
            const usize ready_pos = m_code_buf.size();
            emit(arm64::fmov_d_from_x(fp_reg, REG_SCRATCH0));
            m_code_buf[zero_branch] = arm64::cbz_w(
                REG_SCRATCH2,
                static_cast<i32>(zero_pos - zero_branch) * 4);
            m_code_buf[ready_branch] = arm64::b_imm(
                static_cast<i32>(ready_pos - ready_branch) * 4);
        };

        u8 pop_count = 0;
        // The EA has to be computed BEFORE ST(0) is converted. Converting it
        // can branch straight to the slow path (an exponent outside binary64's
        // range: subnormal, Inf, NaN, or an extended-precision magnitude), and
        // that path hands the helper REG_CALL_TMP as the operand address. With
        // the computation left in the per-opcode arms below, a slow exit
        // passed whatever X21 last held, so the helper read a wild address —
        // Intel SDM Vol. 2 FCOM: the memory operand is still the source, and
        // an out-of-range ST(0) does not change where it lives.
        if (is_memory_operand(op0.type)) {
            emit_compute_ea(REG_CALL_TMP, op0.mem, next_rip);
        }
        // ST(0) goes into D0 AFTER the source is in D1.
        //
        // The memory forms below reach emit_guest_load*, which on the
        // non-linear region path is a helper call — and AAPCS64 makes v0-v7
        // caller-saved, so an ST(0) already staged in D0 does not survive it.
        // `fcom dword [ebx]` then compared whatever the helper left against
        // the loaded value and published C0/C2/C3 from it. Found by the ISA
        // matrix sweep, which only reaches this path when the x87 stack is
        // full enough for the source-tag guard to admit the native body.
        //
        // Safe to defer: emit_st_as_double writes only its named FP register
        // and integer scratch, none of the branches below read D0, and the
        // effective address lives in REG_CALL_TMP (callee-saved) across the
        // call. Same defect family as CMPSS/INSERTPS.
        if (instr.opcode == Opcode::FTST) {
            emit(arm64::fmov_d_from_x(1, 31));
        } else if (instr.opcode == Opcode::FCOM_M32 ||
                   instr.opcode == Opcode::FCOMP_M32) {
            emit(arm64::mov_x(REG_SCRATCH2, REG_CALL_TMP));
            emit_guest_load32(REG_EA_TMP, REG_SCRATCH2);
            emit(arm64::fmov_s_from_w(1, REG_EA_TMP));
            emit(arm64::fcvt_d_from_s(1, 1));
        } else if (instr.opcode == Opcode::FCOM_M64 ||
                   instr.opcode == Opcode::FCOMP_M64) {
            emit(arm64::mov_x(REG_SCRATCH2, REG_CALL_TMP));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH0));
        } else if (instr.opcode == Opcode::FICOM_M16 ||
                   instr.opcode == Opcode::FICOMP_M16) {
            emit(arm64::mov_x(REG_SCRATCH2, REG_CALL_TMP));
            emit_guest_load16(REG_EA_TMP, REG_SCRATCH2);
            emit(arm64::sxth_w(REG_EA_TMP, REG_EA_TMP));
            emit(arm64::scvtf_d_from_w(1, REG_EA_TMP));
        } else if (instr.opcode == Opcode::FICOM_M32 ||
                   instr.opcode == Opcode::FICOMP_M32) {
            emit(arm64::mov_x(REG_SCRATCH2, REG_CALL_TMP));
            emit_guest_load32(REG_EA_TMP, REG_SCRATCH2);
            emit(arm64::scvtf_d_from_w(1, REG_EA_TMP));
        } else if (instr.opcode == Opcode::FCOMPP ||
                   instr.opcode == Opcode::FUCOMPP) {
            emit_st_as_double(1, 1);
            pop_count = 2;
        } else {
            const u8 sti = (op0.type == OperandType::Imm8)
                               ? static_cast<u8>(op0.imm) & 7
                               : 1;
            emit_st_as_double(sti, 1);
            if (instr.opcode == Opcode::FCOMP_STI ||
                instr.opcode == Opcode::FUCOMP_STI) {
                pop_count = 1;
            }
        }
        if (instr.opcode == Opcode::FCOMP_M32 ||
            instr.opcode == Opcode::FCOMP_M64 ||
            instr.opcode == Opcode::FICOMP_M16 ||
            instr.opcode == Opcode::FICOMP_M32) {
            pop_count = 1;
        }

        emit_st_as_double(0, 0);

        emit(arm64::fcmp_d(0, 1));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        // Intel SDM Vol. 2 FCOM/FCOMP/FCOMPP: a valid comparison clears C1
        // in addition to replacing C0/C2/C3. C1 is not sticky across FCOM.
        emit_mov_imm32(REG_SCRATCH4,
                       FPU_SW_C0 | FPU_SW_C1 | FPU_SW_C2 | FPU_SW_C3);
        emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize unord_br = m_code_buf.size();
        emit(0);
        const usize eq_br = m_code_buf.size();
        emit(0);
        const usize lt_br = m_code_buf.size();
        emit(0);
        const usize flags_done_br = m_code_buf.size();
        emit(0);
        const usize unord_pos = m_code_buf.size();
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C0 | FPU_SW_C2 | FPU_SW_C3);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize unord_done = m_code_buf.size();
        emit(0);
        const usize eq_pos = m_code_buf.size();
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C3);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize eq_done = m_code_buf.size();
        emit(0);
        const usize lt_pos = m_code_buf.size();
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_C0);
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize flags_done = m_code_buf.size();
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        for (u8 i = 0; i < pop_count; ++i) {
            emit_fpu_pop();
        }
        const usize native_done = m_code_buf.size();
        emit(0);

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        u32 kind = kFpuKindNone;
        if (is_memory_operand(op0.type)) {
            kind = kFpuKindMem;
            emit(arm64::mov_x(3, REG_CALL_TMP));  // full 64-bit EA
        } else if (op0.type == OperandType::Imm8) {
            kind = kFpuKindSti;
            emit_mov_imm32(3, static_cast<u8>(op0.imm) & 7);
        } else {
            emit(arm64::movz_w(3, 0));
        }
        const u32 packed = (kind << 28) |
            (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);
        emit_mov_imm32(2, packed);
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
        emit_semantic_helper_blr();

        const usize done_pos = m_code_buf.size();
        if (hold_nzcv) emit_nzcv_fill();
        m_code_buf[unord_br] = arm64::b_cond(
            arm64::VS, static_cast<i32>(unord_pos - unord_br) * 4);
        m_code_buf[eq_br] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(eq_pos - eq_br) * 4);
        m_code_buf[lt_br] = arm64::b_cond(
            arm64::MI, static_cast<i32>(lt_pos - lt_br) * 4);
        m_code_buf[flags_done_br] = arm64::b_imm(
            static_cast<i32>(flags_done - flags_done_br) * 4);
        m_code_buf[unord_done] = arm64::b_imm(
            static_cast<i32>(flags_done - unord_done) * 4);
        m_code_buf[eq_done] = arm64::b_imm(
            static_cast<i32>(flags_done - eq_done) * 4);
        for (const usize branch : slow_below_branches) {
            m_code_buf[branch] = arm64::b_cond(
                arm64::CC, static_cast<i32>(slow_pos - branch) * 4);
        }
        for (const usize branch : slow_above_branches) {
            m_code_buf[branch] = arm64::b_cond(
                arm64::HI, static_cast<i32>(slow_pos - branch) * 4);
        }
        m_code_buf[native_done] = arm64::b_imm(
            static_cast<i32>(done_pos - native_done) * 4);

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Intel SDM Vol. 2 FCOMI/FUCOMI: ZF/PF/CF from ST(0) ? ST(i);
    // OF, SF, and AF are cleared. Finite normals use FCMP.
    if (native_fpu_arith &&
        (instr.opcode == Opcode::FCOMI_STI ||
         instr.opcode == Opcode::FUCOMI_STI ||
         instr.opcode == Opcode::FCOMIP_STI ||
         instr.opcode == Opcode::FUCOMIP_STI) &&
        op0.type == OperandType::Imm8) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 double_exp_bias_delta = 16383u - 1023u;
        constexpr u32 min_double_x87_exp = double_exp_bias_delta + 1u;
        constexpr u32 max_double_x87_exp = double_exp_bias_delta + 0x7FEu;
        const u8 sti = static_cast<u8>(op0.imm) & 7;
        std::vector<usize> slow_below_branches;
        std::vector<usize> slow_above_branches;
        const auto emit_st_addr = [&](u8 logical_index) {
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
            emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
            emit(arm64::movz_w(REG_SCRATCH4, logical_index));
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_w(REG_SCRATCH4, 7));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
            emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
            emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                                 REG_SCRATCH2, 0));
        };
        const auto emit_st_as_double = [&](u8 logical_index, u8 fp_reg) {
            emit_st_addr(logical_index);
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                                 offsetof(Float80, exponent_sign)));
            emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
            emit(arm64::movz_w(REG_SCRATCH4, 0x7FFF));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH1, REG_SCRATCH4));
            const usize zero_branch = m_code_buf.size();
            emit(0);
            emit_mov_imm32(REG_SCRATCH4, min_double_x87_exp);
            emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
            slow_below_branches.push_back(m_code_buf.size());
            emit(0);
            emit_mov_imm32(REG_SCRATCH4, max_double_x87_exp);
            emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
            slow_above_branches.push_back(m_code_buf.size());
            emit(0);
            emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));
            emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 12));
            emit_mov_imm32(REG_EA_TMP, double_exp_bias_delta);
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
            emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 52));
            emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 63));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH2));
            emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            const usize ready_branch = m_code_buf.size();
            emit(0);
            const usize zero_pos = m_code_buf.size();
            emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
            emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH1, 63));
            const usize ready_pos = m_code_buf.size();
            emit(arm64::fmov_d_from_x(fp_reg, REG_SCRATCH0));
            m_code_buf[zero_branch] = arm64::cbz_w(
                REG_SCRATCH2,
                static_cast<i32>(zero_pos - zero_branch) * 4);
            m_code_buf[ready_branch] = arm64::b_imm(
                static_cast<i32>(ready_pos - ready_branch) * 4);
        };
        emit_st_as_double(0, 0);
        emit_st_as_double(sti, 1);
        emit(arm64::fcmp_d(0, 1));
        emit(arm64::mrs_nzcv(REG_CALL_TMP));
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                              JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();
        emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::None));
        emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
        emit_clear_cf_overlay();
        const u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, rf_off >> 3));
        emit_mov_imm64(REG_SCRATCH4,
                       static_cast<i64>(~(FLAG_CF | FLAG_ZF | FLAG_PF |
                                          FLAG_OF | FLAG_SF | FLAG_AF)));
        emit(arm64::and_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_CALL_TMP, 28));
        emit(arm64::ands_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize unord_br = m_code_buf.size();
        emit(0);
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_CALL_TMP, 30));
        emit(arm64::ands_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize eq_br = m_code_buf.size();
        emit(0);
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_CALL_TMP, 31));
        emit(arm64::ands_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize lt_br = m_code_buf.size();
        emit(0);
        const usize flags_done_br = m_code_buf.size();
        emit(0);
        const usize unord_pos = m_code_buf.size();
        emit_mov_imm64(REG_SCRATCH4,
                       static_cast<i64>(FLAG_CF | FLAG_ZF | FLAG_PF));
        emit(arm64::orr_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize unord_done = m_code_buf.size();
        emit(0);
        const usize eq_pos = m_code_buf.size();
        emit_mov_imm64(REG_SCRATCH4, static_cast<i64>(FLAG_ZF));
        emit(arm64::orr_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize eq_done = m_code_buf.size();
        emit(0);
        const usize lt_pos = m_code_buf.size();
        emit_mov_imm64(REG_SCRATCH4, static_cast<i64>(FLAG_CF));
        emit(arm64::orr_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        const usize flags_done = m_code_buf.size();
        emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, rf_off >> 3));
        if (instr.opcode == Opcode::FCOMIP_STI ||
            instr.opcode == Opcode::FUCOMIP_STI) {
            emit_fpu_pop();
        }
        const usize native_done = m_code_buf.size();
        emit(0);
        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        const u32 packed = (kFpuKindSti << 28) |
            (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);
        emit_mov_imm32(2, packed);
        emit_mov_imm32(3, sti);
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
        emit_semantic_helper_blr();
        const usize done_pos = m_code_buf.size();
        m_code_buf[unord_br] = arm64::b_cond(
            arm64::NE, static_cast<i32>(unord_pos - unord_br) * 4);
        m_code_buf[eq_br] = arm64::b_cond(
            arm64::NE, static_cast<i32>(eq_pos - eq_br) * 4);
        m_code_buf[lt_br] = arm64::b_cond(
            arm64::NE, static_cast<i32>(lt_pos - lt_br) * 4);
        m_code_buf[flags_done_br] = arm64::b_imm(
            static_cast<i32>(flags_done - flags_done_br) * 4);
        m_code_buf[unord_done] = arm64::b_imm(
            static_cast<i32>(flags_done - unord_done) * 4);
        m_code_buf[eq_done] = arm64::b_imm(
            static_cast<i32>(flags_done - eq_done) * 4);
        for (const usize branch : slow_below_branches) {
            m_code_buf[branch] = arm64::b_cond(
                arm64::CC, static_cast<i32>(slow_pos - branch) * 4);
        }
        for (const usize branch : slow_above_branches) {
            m_code_buf[branch] = arm64::b_cond(
                arm64::HI, static_cast<i32>(slow_pos - branch) * 4);
        }
        m_code_buf[native_done] = arm64::b_imm(
            static_cast<i32>(done_pos - native_done) * 4);
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    return std::nullopt;
}

} // namespace vitreo::cpu::jit
