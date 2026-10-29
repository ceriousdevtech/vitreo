// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 8-bit ALU without a carry-in: TEST, ADD, SUB, OR, AND, XOR
//              and CMP, r/m8 and AL forms. ADC/SBB 8 are in
//              ops/adc_sbb.cpp.
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

std::optional<bool> JitCompiler::compile_alu8(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- TEST 8-bit ----
    case Opcode::TEST_AL_IMM8:
    case Opcode::TEST_RM8_IMM8: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit(arm64::sxtb_w(REG_SCRATCH2, REG_SCRATCH0));
        emit(arm64::ands_w(31, REG_SCRATCH2, REG_SCRATCH2));
        m_flags.nzcv = NzcvState::And;
        break;
    }

    case Opcode::TEST_RM8_R8: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        }
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit(arm64::sxtb_w(REG_SCRATCH2, REG_SCRATCH0));
        emit(arm64::ands_w(31, REG_SCRATCH2, REG_SCRATCH2));
        m_flags.nzcv = NzcvState::And;
        break;
    }

    // ---- 8-bit ALU ----
    case Opcode::ADD_AL_IMM8: {
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Add8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::ADD_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Add8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    // Intel SDM Vol. 2A ADD: opcode 00 /r — ADD r/m8, r8. DEST := DEST + SRC.
    case Opcode::ADD_RM8_R8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Add8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::OR_AL_IMM8: {
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Or8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::OR_RM8_IMM8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Or8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::OR_RM8_R8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Or8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::OR_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Or8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    case Opcode::AND_AL_IMM8: {
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::SUB_AL_IMM8: {
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF)); // AND W0, W0, #0xFF
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::XOR_AL_IMM8: {
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Xor8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::SUB_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF)); // AND W0, W0, #0xFF
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    // Intel SDM Vol. 2B SUB: opcode 28 /r — SUB r/m8, r8. DEST := DEST - SRC.
    case Opcode::SUB_RM8_R8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::XOR_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Xor8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    // Intel SDM Vol. 2C XOR: opcode 30 /r — XOR r/m8, r8. DEST := DEST XOR SRC.
    case Opcode::XOR_RM8_R8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Xor8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // Intel SDM Vol. 2C XOR: opcode 80 /6 ib — XOR r/m8, imm8.
    case Opcode::XOR_RM8_IMM8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Xor8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // Intel SDM Vol. 2A AND: opcode 20 /r — AND r/m8, r8. DEST := DEST AND SRC.
    case Opcode::AND_RM8_R8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // Intel SDM Vol. 2A AND: opcode 22 /r — AND r8, r/m8.
    case Opcode::AND_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    case Opcode::AND_RM8_IMM8: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::ADD_RM8_IMM8: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF)); // AND W0, W0, #0xFF
        emit_set_lazy_flags(CpuState::FlagOp::Add8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::SUB_RM8_IMM8: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF));
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // ---- CMP 8-bit ----
    case Opcode::CMP_RM8_IMM8:
    case Opcode::CMP_AL_IMM8: {
        // Zero-extended byte SUBS: Z and C match x86 ZF/CF, so unsigned Jcc
        // can fuse. N/V are 32-bit and do not match SF/OF — Sub8 rejects those.
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm) & 0xFF);
        emit(arm64::subs_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        // AND (no S) preserves NZCV; mask result for lazy Sub8 reconstruct.
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF));
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        // Zero-ext SUBS is only safe for unsigned Jcc. Clear any predecessor
        // nzcv_kind so signed Jcc cannot fuse stale Sub NZCV (Intel SDM
        // Vol. 2 CMP / Jcc; ContoursCreate cmp byte / jge).
        emit_store_nzcv_kind(NzcvState::None);
        m_flags.nzcv = NzcvState::Sub8;
        break;
    }

    case Opcode::CMP_RM8_R8: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH3);
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        }
        emit(arm64::subs_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF));
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_nzcv_kind(NzcvState::None);
        m_flags.nzcv = NzcvState::Sub8;
        break;
    }

    case Opcode::CMP_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH3, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH3);
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit(arm64::subs_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF));
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        emit_store_nzcv_kind(NzcvState::None);
        m_flags.nzcv = NzcvState::Sub8;
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
