// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 16-bit ALU: TEST, CMP, ADD, SUB, OR, AND and XOR,
//              r/m16 and AX forms. ADC/SBB 16 are in ops/adc_sbb.cpp.
//
// One family of compile_arithmetic's lowerings, split out of
// ops/arithmetic.cpp (ADR-0006 step 3) as pure code motion: the arms are
// verbatim and in their original order. compile_arithmetic
// (ops/arithmetic.cpp) owns the dispatch chain and the end-of-block tail;
// the declaration in jit_compiler_lowering.hpp states what the nullopt
// return means.

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <optional>

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_alu16(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- TEST 16-bit ----
    // TEST r/m16, imm16 and TEST AX, imm16 were 64-bit-only; 32-bit guests
    // (TS3 load) hit jit_interpret_decoded → exec_test → read_u16.
    case Opcode::TEST_RM16_R16:
    case Opcode::TEST_AX_IMM16:
    case Opcode::TEST_RM16_IMM16: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        if (is_immediate_operand(op1.type))
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFFFF);
        else
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And16,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit(arm64::sxth_w(REG_SCRATCH2, REG_SCRATCH0));
        emit(arm64::ands_w(31, REG_SCRATCH2, REG_SCRATCH2));
        m_flags.nzcv = NzcvState::And;
        break;
    }

    // ---- CMP 16-bit ----
    case Opcode::CMP_RM16_R16:
    case Opcode::CMP_R16_RM16:
    case Opcode::CMP_RM16_IMM16:
    case Opcode::CMP_RM16_IMM8:
    case Opcode::CMP_AX_IMM16: {
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
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                            REG_SCRATCH1, REG_SCRATCH0);
        break;
    }

    // ---- ADD 16-bit ----
    // Intel SDM Vol. 2A ADD: DEST := DEST + SRC; OF/SF/ZF/AF/PF/CF from result.
    case Opcode::ADD_RM16_R16:
    case Opcode::ADD_R16_RM16:
    case Opcode::ADD_RM16_IMM16:
    case Opcode::ADD_RM16_IMM8:
    case Opcode::ADD_AX_IMM16: {
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
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Add16,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- SUB 16-bit ----
    // Intel SDM Vol. 2A SUB: DEST := DEST - SRC; OF/SF/ZF/AF/PF/CF from result.
    case Opcode::SUB_RM16_R16:
    case Opcode::SUB_R16_RM16:
    case Opcode::SUB_RM16_IMM16:
    case Opcode::SUB_RM16_IMM8:
    case Opcode::SUB_AX_IMM16: {
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
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- OR 16-bit (non-LOCK; LOCK forms stay in compile_locked) ----
    // Intel SDM Vol. 2A OR: DEST := DEST OR SRC; OF=CF=0; SF/ZF/PF from result.
    case Opcode::OR_RM16_R16:
    case Opcode::OR_R16_RM16:
    case Opcode::OR_RM16_IMM16:
    case Opcode::OR_RM16_IMM8:
    case Opcode::OR_AX_IMM16: {
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
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Or16,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- AND 16-bit ----
    case Opcode::AND_RM16_R16:
    case Opcode::AND_R16_RM16:
    case Opcode::AND_RM16_IMM16:
    case Opcode::AND_RM16_IMM8:
    case Opcode::AND_AX_IMM16: {
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
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And16,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- XOR 16-bit ----
    case Opcode::XOR_RM16_R16:
    case Opcode::XOR_R16_RM16:
    case Opcode::XOR_RM16_IMM16:
    case Opcode::XOR_RM16_IMM8:
    case Opcode::XOR_AX_IMM16: {
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
        emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Xor16,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
