// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — ADC and SBB at 8, 16 and 32 bits (Intel SDM Vol. 2A ADC,
//              Vol. 2B SBB); the 32-bit-path twin of ops/adc_sbb64.cpp.
//
// One family of compile_arithmetic's lowerings, split out of
// ops/arithmetic.cpp (ADR-0006 step 3) as pure code motion: the arms are
// verbatim and in their original order. compile_arithmetic
// (ops/arithmetic.cpp) owns the dispatch chain and the end-of-block tail;
// the declaration in jit_compiler_lowering.hpp states what the nullopt
// return means.
//
// emit_store_adc_sbb32_operands and emit_narrow_sbb_flags were lambdas at
// the top of compile_arithmetic used only by these arms; they moved with
// them, still lambdas.

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <optional>

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_adc_sbb(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    auto emit_store_adc_sbb32_operands = [&] {
        const u32 src_off = flag_src_offset();
        const u32 dst_off = flag_dst_offset();
        if (m_block_is_64bit) {
            emit(arm64::str_x_uoff(
                REG_SCRATCH4, REG_CPU_STATE, src_off >> 3));
            emit(arm64::mov_w(REG_SCRATCH3, REG_SCRATCH0));
            emit(arm64::str_x_uoff(
                REG_SCRATCH3, REG_CPU_STATE, dst_off >> 3));
            return;
        }
        static_assert(offsetof(CpuState, flag_dst)
                      == offsetof(CpuState, flag_src) + sizeof(u64));
        static_assert((offsetof(CpuState, flag_src) & 7u) == 0);
        static_assert(offsetof(CpuState, flag_src) <= 4095u);
        // X4 is (carry-in << 32) | u32(source). W0 arithmetic guarantees
        // X0 is the zero-extended result, so the adjacent lazy operands can
        // be published with one pair store.
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, src_off));
        emit(arm64::stp_x_off(
            REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH3, 0));
    };

    auto emit_narrow_sbb_flags = [&](CpuState::FlagOp flag_op) {
        // Pack carry-in above the zero-extended source, matching Sbb32's
        // internal lazy-state contract.
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_CALL_TMP, 32));
        emit(arm64::orr_x(
            REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit(arm64::str_x_uoff(
            REG_SCRATCH4, REG_CPU_STATE, flag_src_offset() >> 3));
        emit(arm64::str_x_uoff(
            REG_SCRATCH0, REG_CPU_STATE, flag_dst_offset() >> 3));
        emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(flag_op));
        emit(arm64::strb_uoff(
            REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
        emit_clear_cf_overlay();
    };

    switch (instr.opcode) {

    // ---- ADC 16-bit (Intel SDM Vol. 2A ADC: DEST := DEST + SRC + CF) ----
    // ADC/SBB16 family for PE32 native JIT.
    case Opcode::ADC_RM16_R16:
    case Opcode::ADC_R16_RM16:
    case Opcode::ADC_RM16_IMM16:
    case Opcode::ADC_RM16_IMM8:
    case Opcode::ADC_AX_IMM16: {
        emit_lazy_cf_to_call_tmp();
        bool dst_is_mem = !is_register_operand(op0.type);
        bool src_is_mem = !is_register_operand(op1.type)
                       && !is_immediate_operand(op1.type);
        if (src_is_mem) {
            emit_compute_ea(REG_SCRATCH3, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH3);
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        } else if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            if (is_immediate_operand(op1.type))
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFFFF);
            else
                emit_load_gpr16(REG_SCRATCH1, op1.reg);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            if (is_immediate_operand(op1.type))
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFFFF);
            else
                emit_load_gpr16(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_narrow_sbb_flags(CpuState::FlagOp::Adc16);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- SBB 16-bit (Intel SDM Vol. 2B SBB: DEST := DEST - SRC - CF) ----
    case Opcode::SBB_RM16_R16:
    case Opcode::SBB_R16_RM16:
    case Opcode::SBB_RM16_IMM16:
    case Opcode::SBB_RM16_IMM8:
    case Opcode::SBB_AX_IMM16: {
        emit_lazy_cf_to_call_tmp();
        bool dst_is_mem = !is_register_operand(op0.type);
        bool src_is_mem = !is_register_operand(op1.type)
                       && !is_immediate_operand(op1.type);
        if (src_is_mem) {
            emit_compute_ea(REG_SCRATCH3, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH3);
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        } else if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            if (is_immediate_operand(op1.type))
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFFFF);
            else
                emit_load_gpr16(REG_SCRATCH1, op1.reg);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            if (is_immediate_operand(op1.type))
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFFFF);
            else
                emit_load_gpr16(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_narrow_sbb_flags(CpuState::FlagOp::Sbb16);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::SBB_AL_IMM8:
    case Opcode::SBB_RM8_IMM8: {
        emit_lazy_cf_to_call_tmp();
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_narrow_sbb_flags(CpuState::FlagOp::Sbb8);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::SBB_R8_RM8:
    case Opcode::SBB_RM8_R8: {
        emit_lazy_cf_to_call_tmp();
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            // Memory source: load it FIRST, then the destination register.
            //
            // REG_SCRATCH0 is W0, and emit_guest_load8 reaches a helper call
            // on the non-linear region path — which clobbers W0. Staging the
            // destination byte there before the load silently destroyed it,
            // so `ADC AL, [mem]` computed 0 + src + CF. Masked in production
            // because 32-bit guests default to the linear path, where the load
            // is an inline LDRB and clobbers nothing; found by the ISA matrix
            // sweep, which runs the region path, and settled against silicon.
            // Same ordering as XOR_R8_RM8 and CMP_R8_RM8 below.
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_narrow_sbb_flags(CpuState::FlagOp::Sbb8);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // Intel SDM Vol. 2A ADC: opcode 10 /r — ADC r/m8, r8.
    // DEST := DEST + SRC + CF. Lazy flags use Adc8 (same packing as Sbb8).
    case Opcode::ADC_AL_IMM8:
    case Opcode::ADC_RM8_IMM8: {
        emit_lazy_cf_to_call_tmp();
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_narrow_sbb_flags(CpuState::FlagOp::Adc8);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::ADC_RM8_R8:
    case Opcode::ADC_R8_RM8: {
        emit_lazy_cf_to_call_tmp();
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            // Memory source: load it FIRST, then the destination register.
            //
            // REG_SCRATCH0 is W0, and emit_guest_load8 reaches a helper call
            // on the non-linear region path — which clobbers W0. Staging the
            // destination byte there before the load silently destroyed it,
            // so `ADC AL, [mem]` computed 0 + src + CF. Masked in production
            // because 32-bit guests default to the linear path, where the load
            // is an inline LDRB and clobbers nothing; found by the ISA matrix
            // sweep, which runs the region path, and settled against silicon.
            // Same ordering as XOR_R8_RM8 and CMP_R8_RM8 below.
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_narrow_sbb_flags(CpuState::FlagOp::Adc8);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // ---- ADC (add with carry) ----
    case Opcode::ADC_R32_RM32: {
        emit_lazy_cf_to_call_tmp();
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        {
            emit(0xD3607C00u | (static_cast<u32>(REG_CALL_TMP) << 5) | 4u);
            emit(0xAA010084u);
            emit_store_adc_sbb32_operands();
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::Adc32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::ADC_RM32_R32: {
        emit_lazy_cf_to_call_tmp();
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, op1.reg);
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        {
            emit(0xD3607C00u | (static_cast<u32>(REG_CALL_TMP) << 5) | 4u);
            emit(0xAA010084u);
            emit_store_adc_sbb32_operands();
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::Adc32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::ADC_RM32_IMM32:
    case Opcode::ADC_RM32_IMM8: {
        emit_lazy_cf_to_call_tmp();
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        const bool pe32_zero_imm = !m_block_is_64bit
            && static_cast<u32>(op1.imm) == 0;
        if (!pe32_zero_imm) {
            i32 imm = static_cast<i32>(op1.imm);
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(imm));
        }
        // CMP Wcf,#1 puts the x86 carry-in into ARM's C flag (cf==1 ->
        // no borrow -> C=1), then ADCS computes dst+src+cf and leaves the
        // true carry-out in NZCV. Doing it as two plain ADDs cannot produce a
        // correct single carry-out, which is why the old code had to recompute
        // CF from flag_src/flag_dst on every step of a carry chain.
        emit(arm64::cmp_w_imm(REG_CALL_TMP, 1));
        const u8 src_reg = pe32_zero_imm ? 31 : REG_SCRATCH1;
        emit(arm64::adcs_w(REG_SCRATCH0, REG_SCRATCH0, src_reg));
        {
            emit(0xD3607C00u | (static_cast<u32>(REG_CALL_TMP) << 5) | 4u);
            if (!pe32_zero_imm) emit(0xAA010084u);
            emit_store_adc_sbb32_operands();
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::Adc32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        // The carry-out now sits in ARM NZCV, so a following ADC
        // takes the cheap m_flags.incoming_nzcv fast path in emit_lazy_cf_to_call_tmp
        // instead of recomputing CF from the lazy FlagOp. Only claim it for a
        // register destination -- emit_guest_store32's bounds-check SUBS would
        // clobber NZCV before the next instruction sees it.
        if (!dst_is_mem) m_flags.nzcv = NzcvState::Add;
        break;
    }

    // ---- SBB (subtract with borrow) ----
    case Opcode::SBB_R32_RM32: {
        emit_lazy_cf_to_call_tmp();
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        {
            emit(0xD3607C00u | (static_cast<u32>(REG_CALL_TMP) << 5) | 4u);
            emit(0xAA010084u);
            emit_store_adc_sbb32_operands();
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::Sbb32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::SBB_RM32_R32: {
        emit_lazy_cf_to_call_tmp();
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, op1.reg);
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        {
            emit(0xD3607C00u | (static_cast<u32>(REG_CALL_TMP) << 5) | 4u);
            emit(0xAA010084u);
            emit_store_adc_sbb32_operands();
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::Sbb32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::SBB_RM32_IMM32:
    case Opcode::SBB_RM32_IMM8: {
        emit_lazy_cf_to_call_tmp();
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        const bool pe32_zero_imm = !m_block_is_64bit
            && static_cast<u32>(op1.imm) == 0;
        if (!pe32_zero_imm) {
            i32 imm = static_cast<i32>(op1.imm);
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(imm));
        }
        // ARM SBCS computes Rn - Rm - (1 - C) and x86 SBB computes
        // dst - src - CF, so the carry-in is inverted: SUBS WZR,WZR,Wcf sets
        // C = !cf. The carry-out is inverted coming back out too (x86 CF =
        // !C), which is exactly what the NzcvState::Sub fast path assumes.
        emit(arm64::subs_w(31, 31, REG_CALL_TMP));
        const u8 src_reg = pe32_zero_imm ? 31 : REG_SCRATCH1;
        emit(arm64::sbcs_w(REG_SCRATCH0, REG_SCRATCH0, src_reg));
        {
            emit(0xD3607C00u | (static_cast<u32>(REG_CALL_TMP) << 5) | 4u);
            if (!pe32_zero_imm) emit(0xAA010084u);
            emit_store_adc_sbb32_operands();
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::Sbb32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        // The carry-out now sits in ARM NZCV, so a following SBB
        // takes the cheap m_flags.incoming_nzcv fast path in emit_lazy_cf_to_call_tmp
        // instead of recomputing CF from the lazy FlagOp. Only claim it for a
        // register destination -- emit_guest_store32's bounds-check SUBS would
        // clobber NZCV before the next instruction sees it.
        if (!dst_is_mem) m_flags.nzcv = NzcvState::Sub;
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
