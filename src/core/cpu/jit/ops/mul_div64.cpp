// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: multiply and divide. The IMUL r32
// forms inside a 64-bit block, and IMUL/MUL/DIV/IDIV 64-bit.
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
std::optional<bool> JitCompiler::compile_mul_div64(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip,
    bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // IMUL r32, r/m32 (and 3-operand imm forms) inside a 64-bit block.
    // Register source only; memory source falls back. Mirrors compile_mul_div
    // (FlagOp::Imul32: flag_src = full signed 64-bit product for CF/OF).
    // ========================================================================
    case Opcode::IMUL_R32_RM32: {
        // The r/m operand may be memory (Intel SDM Vol. 2 IMUL, 0F AF /r).
        // Declining it here sent every `imul r32, [mem]` to the interpreter.
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH4);
        }
        emit_load_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        // Within-block liveness may prove this record dead (flag_liveness.cpp,
        // the IMUL rule): a register-only full writer replaces it first.
        if (!m_omit_lazy_current) {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        }
        break;
    }

    case Opcode::IMUL_R32_RM32_IMM32:
    case Opcode::IMUL_R32_RM32_IMM8: {
        // Same as the 64-bit three-operand form below, which has always
        // handled a memory r/m. MEASURED: `imul r32, [mem], imm` was the
        // single most-delegated instruction in a browser renderer --
        // 37,458,519 interpreter round trips, 12.2% of ALL delegated
        // instructions -- purely because this arm declined memory.
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit_mov_imm32(REG_SCRATCH1,
                       static_cast<u32>(instr.operands[2].imm));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        // Within-block liveness may prove this record dead (flag_liveness.cpp,
        // the IMUL rule): a register-only full writer replaces it first.
        if (!m_omit_lazy_current) {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, src_off >> 3));
        }
        break;
    }

    // ========================================================================
    // IMUL 64-bit — two-operand and three-operand forms.
    //
    // Lazy flag convention (FlagOp::Imul64):
    //   flag_src = SMULH result (high 64 bits of signed product)
    //   flag_dst = MUL result   (low 64 bits)
    // Materialization: CF=OF=1 if hi != sign_extension(lo).
    // ========================================================================

    // IMUL R64, R/M64  — two-operand: dst = dst * src
    case Opcode::IMUL_R64_RM64: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::smulh(REG_CALL_TMP, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::mul_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags64(CpuState::FlagOp::Imul64, REG_CALL_TMP, REG_SCRATCH0);
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // IMUL R64, R/M64, IMM8 / IMM32  — three-operand: dst = src * imm
    case Opcode::IMUL_R64_RM64_IMM8:
    case Opcode::IMUL_R64_RM64_IMM32: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_mov_imm64(REG_SCRATCH1, instr.operands[2].imm);
        emit(arm64::smulh(REG_CALL_TMP, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::mul_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags64(CpuState::FlagOp::Imul64, REG_CALL_TMP, REG_SCRATCH0);
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // MUL r/m64 — unsigned RDX:RAX := RAX * SRC. Intel SDM Vol. 2, MUL:
    // CF and OF are set iff the high half is nonzero; the other status flags
    // are undefined. Preserve Vitreo's prior values for those undefined flags.
    //
    // Load SRC before RAX. A checked memory load may call a host helper and
    // clobber caller-saved ARM registers; the former native lowering loaded
    // RAX first and therefore corrupted memory-source products on that path.
    // IMUL r/m64 (0xF7 /5) — signed RAX * SRC -> RDX:RAX.
    //
    // This had NO JIT lowering at all, so every one delegated to the
    // interpreter. It is not rare: it is the one-operand form the compiler
    // emits for signed division by a constant (magic-number multiply, take the
    // high half, shift), so .NET's clrjit runs it constantly -- the last
    // instruction the interpreter saw before coreclr's failure was exactly
    // `48 f7 e9` = imul rcx.
    //
    // Signed, so SMULH/MUL rather than MUL_RM64's UMULH/MUL. CF and OF are set
    // when the full 128-bit product does not sign-extend from the low 64 bits,
    // i.e. high != (low >> 63) -- NOT "high != 0", which is the unsigned test
    // and would report overflow for every negative result.
    case Opcode::IMUL_RM64: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
        }

        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // High half FIRST: MUL overwrites the multiplicand register.
        emit(arm64::smulh(REG_CALL_TMP, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::mul_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RDX));

        // Lazy record: FlagOp::Imul64 reads flag_src = HIGH, flag_dst = LOW and
        // computes cf = high != (low >> 63). ZF/SF/PF/AF are architecturally
        // undefined for IMUL, and the lazy op owns them.
        {
            const u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul64));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            emit(arm64::str_x_uoff(REG_CALL_TMP, REG_CPU_STATE,
                                   flag_src_offset() >> 3));
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   flag_dst_offset() >> 3));
        }
        m_flags.nzcv = NzcvState::None;
        break;
    }

    case Opcode::MUL_RM64: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
        }

        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::umulh(REG_CALL_TMP, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::mul_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RDX));

        // Publish CF/OF eagerly while retaining the pending lazy operation
        // that owns the undefined ZF/SF/PF/AF values. Overlay encoding is
        // 1=false, 2=true, so CSET(NE)+1 directly supplies both overlays.
        const u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::movz_w(REG_SCRATCH2, 0x0801));
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::subs_x(31, REG_CALL_TMP, 31));
        emit(arm64::cset_w(REG_SCRATCH4, arm64::NE));
        emit(arm64::orr_x_lsl(REG_SCRATCH2, REG_SCRATCH4,
                              REG_SCRATCH4, 11));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::add_w_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit(arm64::strb_uoff(REG_SCRATCH4, REG_CPU_STATE,
                              cf_overlay_offset()));
        emit(arm64::strb_uoff(REG_SCRATCH4, REG_CPU_STATE,
                              of_overlay_offset()));
        m_flags.nzcv = NzcvState::None;
        break;
    }

    // DIV r/m64 — unsigned RDX:RAX / SRC → RAX rem RDX. Intel SDM Vol. 2A:
    // zero divisor or quotient that does not fit in 64 bits raises #DE without
    // committing. Host __int128 math via jit_div_rm64 (same as interpreter);
    // #DE uses jit_raise_divide_error like PE32 DIV_RM32.
    case Opcode::DIV_RM64: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_div_rm64)));
        emit_helper_blr();  // BLR X16 — W0 = 0 ok, nonzero #DE
        const usize overflow_branch = m_code_buf.size();
        emit(0); // CBNZ W0, divide_error
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[overflow_branch] = arm64::cbnz_w(
            REG_SCRATCH0,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    // IDIV r/m64 — signed RDX:RAX / SRC → RAX rem RDX. Same #DE contract.
    case Opcode::IDIV_RM64: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_idiv_rm64)));
        emit_helper_blr();
        const usize overflow_branch = m_code_buf.size();
        emit(0); // CBNZ W0, divide_error
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[overflow_branch] = arm64::cbnz_w(
            REG_SCRATCH0,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
