// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: register arithmetic. FADD/FSUB/FSUBR/FMUL/
// FDIV/FDIVR in their ST(0),ST(i), ST(i),ST(0) and pop forms (Intel SDM
// Vol. 1 8.3.5).
//
// One family of compile_fpu_native's native lowerings, split out of
// ops/fpu.cpp (ADR-0006) as pure code motion: the blocks are verbatim and in
// their original order. compile_fpu_native (ops/fpu.cpp) owns the dispatch
// chain and the jit_fpu_op helper fallback; the declaration in
// jit_compiler.hpp states what the nullopt return means.

#include "../jit_helpers.hpp"
#include "fpu_internal.hpp"

#include <cstdlib>
#include <optional>
#include <vector>

namespace vitreo::cpu::jit {

namespace {

// Its only reader is the register-arithmetic lowering below.
const bool kDisableFpuRegArith =
    std::getenv("VITREO_JIT_DISABLE_FPU_REG_ARITH") != nullptr;

}  // namespace

std::optional<bool> JitCompiler::compile_fpu_arith_reg(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const bool native_fpu_arith = native_arith_allowed(instr.opcode);
    const bool disable_fpu_reg_arith = kDisableFpuRegArith;

    const auto& op0 = instr.operands[0];

    // Native STI arith lowers through binary64 for values that fit. Specials
    // take the helper so Float80 edge cases stay on exec_fpu.
    if (native_fpu_arith && !disable_fpu_reg_arith &&
        (instr.opcode == Opcode::FADD_STI ||
         instr.opcode == Opcode::FSUB_STI ||
         instr.opcode == Opcode::FSUBR_STI ||
         instr.opcode == Opcode::FMUL_STI ||
         instr.opcode == Opcode::FDIV_STI ||
         instr.opcode == Opcode::FDIVR_STI ||
         instr.opcode == Opcode::FADDP_STI ||
         instr.opcode == Opcode::FSUBP_STI ||
         instr.opcode == Opcode::FSUBRP_STI ||
         instr.opcode == Opcode::FMULP_STI ||
         instr.opcode == Opcode::FDIVP_STI ||
         instr.opcode == Opcode::FDIVRP_STI ||
         instr.opcode == Opcode::FADD_TO_STI ||
         instr.opcode == Opcode::FSUB_TO_STI ||
         instr.opcode == Opcode::FSUBR_TO_STI ||
         instr.opcode == Opcode::FMUL_TO_STI ||
         instr.opcode == Opcode::FDIV_TO_STI ||
         instr.opcode == Opcode::FDIVR_TO_STI) &&
        op0.type == OperandType::Imm8) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 double_exp_bias_delta = 16383u - 1023u;
        constexpr u32 min_double_x87_exp = double_exp_bias_delta + 1u;
        constexpr u32 max_double_x87_exp = double_exp_bias_delta + 0x7FEu;
        const u8 sti = static_cast<u8>(op0.imm) & 7;
        const bool consume_self_d0 = m_x87_d0_forward_in &&
                                     instr.opcode == Opcode::FADD_STI &&
                                     sti == 0;
        const bool publish_self_d0 = m_x87_d0_forward_out &&
                                     instr.opcode == Opcode::FADD_STI &&
                                     sti == 0;
        std::vector<usize> slow_below_branches;
        std::vector<usize> slow_above_branches;

        const auto emit_st_addr = [&](u8 logical_index) {
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE,
                                 status_offset));
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

        const bool dest_sti =
            instr.opcode == Opcode::FADDP_STI ||
            instr.opcode == Opcode::FSUBP_STI ||
            instr.opcode == Opcode::FSUBRP_STI ||
            instr.opcode == Opcode::FMULP_STI ||
            instr.opcode == Opcode::FDIVP_STI ||
            instr.opcode == Opcode::FDIVRP_STI ||
            instr.opcode == Opcode::FADD_TO_STI ||
            instr.opcode == Opcode::FSUB_TO_STI ||
            instr.opcode == Opcode::FSUBR_TO_STI ||
            instr.opcode == Opcode::FMUL_TO_STI ||
            instr.opcode == Opcode::FDIV_TO_STI ||
            instr.opcode == Opcode::FDIVR_TO_STI;
        const bool do_pop =
            instr.opcode == Opcode::FADDP_STI ||
            instr.opcode == Opcode::FSUBP_STI ||
            instr.opcode == Opcode::FSUBRP_STI ||
            instr.opcode == Opcode::FMULP_STI ||
            instr.opcode == Opcode::FDIVP_STI ||
            instr.opcode == Opcode::FDIVRP_STI;
        const u8 dest_logical = dest_sti ? sti : 0;

        // Precision control governs these results (Intel SDM Vol. 1 8.1.5.2),
        // but this path computes in ARM64 binary64 and cannot express a 24-bit
        // significand. PC is runtime state — a guest can change it with FLDCW
        // at any point, and D3D9's CreateDevice does exactly that — so the
        // check has to be emitted rather than decided at compile time. When PC
        // selects single precision, fall through to the same helper the
        // Inf/NaN/subnormal cases already use, which rounds correctly.
        // PC == 0 is Single, so a zero field takes the slow path.
        constexpr u32 control_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, control_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 0x300));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        const usize pc_single_branch = m_code_buf.size();
        emit(0);

        usize forwarded_sources_branch = 0;
        if (consume_self_d0) {
            forwarded_sources_branch = m_code_buf.size();
            emit(0);  // CBNZ W6 -> D0 already mirrors both ST(0) operands
        }
        emit_st_as_double(0, 0);
        const usize second_source_pos = m_code_buf.size();
        if (sti == 0) {
            // The D8 C0 self-source form reads the same architectural ST(0)
            // twice.  Reuse its checked conversion instead of reloading and
            // reconverting the same Float80 slot.
            emit(arm64::mov_v16b(1, 0));
        } else {
            emit_st_as_double(sti, 1);
        }
        if (consume_self_d0) {
            m_code_buf[forwarded_sources_branch] = arm64::cbnz_w(
                kX87D0Valid,
                static_cast<i32>(second_source_pos -
                                 forwarded_sources_branch) * 4);
        }
        switch (instr.opcode) {
        case Opcode::FADD_STI:
        case Opcode::FADDP_STI:
        case Opcode::FADD_TO_STI:
            emit(arm64::fadd_s_scalar(0, 0, 1, arm64::FS_D));
            break;
        case Opcode::FSUB_STI:
        case Opcode::FSUBRP_STI:
        case Opcode::FSUBR_TO_STI:
            emit(arm64::fsub_s_scalar(0, 0, 1, arm64::FS_D));
            break;
        case Opcode::FSUBR_STI:
        case Opcode::FSUBP_STI:
        case Opcode::FSUB_TO_STI:
            emit(arm64::fsub_s_scalar(0, 1, 0, arm64::FS_D));
            break;
        case Opcode::FMUL_STI:
        case Opcode::FMULP_STI:
        case Opcode::FMUL_TO_STI:
            emit(arm64::fmul_s_scalar(0, 0, 1, arm64::FS_D));
            break;
        case Opcode::FDIV_STI:
        case Opcode::FDIVRP_STI:
        case Opcode::FDIVR_TO_STI:
            emit(arm64::fdiv_s_scalar(0, 0, 1, arm64::FS_D));
            break;
        case Opcode::FDIVR_STI:
        case Opcode::FDIVP_STI:
        case Opcode::FDIV_TO_STI:
            emit(arm64::fdiv_s_scalar(0, 1, 0, arm64::FS_D));
            break;
        default:
            break;
        }

        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH0, 52));
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        // A biased exponent of 0 means +-0 OR A SUBNORMAL. Treating the whole
        // class as zero flushed every subnormal RESULT -- measured against
        // Windows, log(1e-320) returned -inf instead of -736.827 because the
        // preceding FADD wrote its subnormal sum as 0. The FLD_M64 gate already
        // distinguishes the two; do the same here and let a subnormal fall
        // through to the helper, which handles it exactly.
        const usize result_nonzero_exp_branch = m_code_buf.size();
        emit(0);   // CBNZ SCRATCH2 -> ordinary exponent check
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1));  // drop sign
        const usize result_zero_branch = m_code_buf.size();
        emit(0);   // CBZ SCRATCH4 -> a true +-0
        const usize result_subnormal_branch = m_code_buf.size();
        emit(0);   // B -> slow path (subnormal)
        const usize result_exp_check_pos = m_code_buf.size();
        emit(arm64::movz_w(REG_SCRATCH4, 0x7FF));  // reload: clobbered above
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH4));
        const usize result_special_branch = m_code_buf.size();
        emit(0);

        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH0, 12));
        emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit_mov_imm64(REG_SCRATCH1,
                       static_cast<i64>(0x8000000000000000ULL));
        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH1, double_exp_bias_delta);
        emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
        emit(arm64::mov_x(17, REG_SCRATCH4));
        emit(arm64::mov_w(REG_EA_TMP, REG_SCRATCH2));
        emit_st_addr(dest_logical);
        emit(arm64::str_x_uoff(17, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_EA_TMP, REG_SCRATCH4, 31));
        if (publish_self_d0) {
            emit(arm64::movz_w(kX87D0Valid, 1));
        }
        if (do_pop) {
            emit_fpu_pop();
        }
        const usize normal_done_branch = m_code_buf.size();
        emit(0);

        const usize result_zero_pos = m_code_buf.size();
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 15));
        emit_st_addr(dest_logical);
        emit(arm64::str_x_uoff(31, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        if (publish_self_d0) {
            emit(arm64::movz_w(kX87D0Valid, 1));
        }
        if (do_pop) {
            emit_fpu_pop();
        }
        const usize zero_done_branch = m_code_buf.size();
        emit(0);

        const usize slow_pos = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        const u32 packed = (kFpuKindSti << 28) |
            (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);
        emit_mov_imm32(2, packed);
        emit_mov_imm32(3, sti);
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
        emit_semantic_helper_blr(); // blr x16
        if (publish_self_d0) {
            emit(arm64::movz_w(kX87D0Valid, 0));
        }

        const usize done_pos = m_code_buf.size();
        m_code_buf[pc_single_branch] = arm64::cbz_w(
            REG_SCRATCH2,
            static_cast<i32>(slow_pos - pc_single_branch) * 4);
        for (const usize branch : slow_below_branches) {
            m_code_buf[branch] = arm64::b_cond(
                arm64::CC, static_cast<i32>(slow_pos - branch) * 4);
        }
        for (const usize branch : slow_above_branches) {
            m_code_buf[branch] = arm64::b_cond(
                arm64::HI, static_cast<i32>(slow_pos - branch) * 4);
        }
        m_code_buf[result_nonzero_exp_branch] = arm64::cbnz_w(
            REG_SCRATCH2,
            static_cast<i32>(result_exp_check_pos -
                             result_nonzero_exp_branch) * 4);
        m_code_buf[result_zero_branch] = arm64::cbz_x(
            REG_SCRATCH4,
            static_cast<i32>(result_zero_pos - result_zero_branch) * 4);
        m_code_buf[result_subnormal_branch] = arm64::b_imm(
            static_cast<i32>(slow_pos - result_subnormal_branch) * 4);
        m_code_buf[result_special_branch] = arm64::b_cond(
            arm64::EQ,
            static_cast<i32>(slow_pos - result_special_branch) * 4);
        m_code_buf[normal_done_branch] = arm64::b_imm(
            static_cast<i32>(done_pos - normal_done_branch) * 4);
        m_code_buf[zero_done_branch] = arm64::b_imm(
            static_cast<i32>(done_pos - zero_done_branch) * 4);

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
