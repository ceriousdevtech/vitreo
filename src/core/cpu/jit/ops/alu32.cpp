// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 32-bit binary ALU: ADD, SUB, AND, OR, XOR, CMP and TEST,
//              r/m32 and EAX forms.
//
// One family of compile_arithmetic's lowerings, split out of
// ops/arithmetic.cpp (ADR-0006 step 3) as pure code motion: the arms are
// verbatim and in their original order. compile_arithmetic
// (ops/arithmetic.cpp) owns the dispatch chain and the end-of-block tail;
// the declaration in jit_compiler_lowering.hpp states what the nullopt
// return means.
//
// EMIT_BINARY_ALU moved with the ADD..XOR arms, its only users.

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <optional>

namespace vitreo::cpu::jit {

// ============================================================================
// Binary ALU macro — shared by ADD/SUB/AND/OR/XOR (32-bit)
// ============================================================================
#define EMIT_BINARY_ALU(alu_emit, flag_op)                                    \
    do {                                                                       \
        bool dst_is_mem = !is_register_operand(op0.type);                      \
        bool src_is_mem = !is_register_operand(op1.type)                       \
                       && !is_immediate_operand(op1.type);                     \
        const bool src_is_reg = is_register_operand(op1.type);                 \
        /* Intel SDM Vol. 1 §3.4.1.1: a 32-bit write zero-extends the full     \
           64-bit GPR. ARM W-form ALU does the same to an X pin, so long-mode  \
           register destinations stay in the pin. add eax,eax still stages:    \
           emit_set_lazy_flags names the source register, which would already  \
           hold the result. */                                                 \
        const bool pin_dst = !dst_is_mem                                       \
            && m_pins_live && op0.reg < 16 && m_pin_arm[op0.reg] != 0xFF     \
            && !(src_is_reg && op1.reg == op0.reg);                            \
        const bool pin_src = pin_dst && src_is_reg                             \
            && op1.reg < 16 && m_pin_arm[op1.reg] != 0xFF;                    \
        const u8 alu_dst = pin_dst ? m_pin_arm[op0.reg]                       \
                                         : REG_SCRATCH0;                      \
        const u8 alu_src = pin_src ? m_pin_arm[op1.reg]                       \
                                         : REG_SCRATCH1;                      \
        u8 mem_addr = 0xFF;                                                    \
        if (src_is_mem) {                                                      \
            if (!try_esp_disp_load32(alu_src, op1.mem)) {                      \
                mem_addr = emit_addr32(REG_CALL_TMP, op1.mem, next_rip);       \
                emit_guest_load32(alu_src, mem_addr);                          \
            }                                                                  \
            emit_load_gpr32(alu_dst, op0.reg);                                 \
        } else if (dst_is_mem) {                                               \
            if (!try_esp_disp_load32(alu_dst, op0.mem)) {                      \
                mem_addr = emit_addr32(REG_CALL_TMP, op0.mem, next_rip);       \
                emit_guest_load32(alu_dst, mem_addr);                          \
            }                                                                  \
            if (is_immediate_operand(op1.type))                                \
                emit_mov_imm32(alu_src, static_cast<u32>(op1.imm));            \
            else                                                               \
                emit_load_gpr32(alu_src, op1.reg);                             \
        } else {                                                               \
            emit_load_gpr32(alu_dst, op0.reg);                                 \
            if (is_immediate_operand(op1.type))                                \
                emit_mov_imm32(alu_src, static_cast<u32>(op1.imm));            \
            else                                                               \
                emit_load_gpr32(alu_src, op1.reg);                             \
        }                                                                      \
        alu_emit;                                                              \
        emit_set_lazy_flags(flag_op, alu_src, alu_dst);                        \
        if (dst_is_mem) {                                                      \
            m_flags.hold_nzcv = true;                                                \
            if (!try_esp_disp_store32(alu_dst, op0.mem)) {                     \
                if (mem_addr == 0xFF)                                          \
                    mem_addr = emit_addr32(REG_CALL_TMP, op0.mem, next_rip);   \
                emit_guest_store32(alu_dst, mem_addr);                         \
            }                                                                  \
            m_flags.hold_nzcv = false;                                               \
        } else {                                                               \
            emit_store_gpr32(alu_dst, op0.reg);                                \
        }                                                                      \
    } while (0)

std::optional<bool> JitCompiler::compile_alu32(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- 32-bit binary ALU ----
    case Opcode::ADD_RM32_R32:
    case Opcode::ADD_R32_RM32:
    case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_RM32_IMM8:
    case Opcode::ADD_EAX_IMM32: {
        // ADDS leaves NZCV valid; ARM64 C == x86 CF for addition (no inversion,
        // unlike SUB). A following ADC can then consume the carry natively
        // instead of reconstructing it — the dominant idiom in bignum Comba
        // multiply-accumulate. Memory destinations spill NZCV across the store.
        EMIT_BINARY_ALU(
            emit(arm64::adds_w(alu_dst, alu_dst, alu_src)),
            CpuState::FlagOp::Add32);
        m_flags.nzcv = NzcvState::Add;
        break;
    }

    case Opcode::SUB_RM32_R32:
    case Opcode::SUB_R32_RM32:
    case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_RM32_IMM8:
    case Opcode::SUB_EAX_IMM32: {
        // SUBS leaves NZCV valid for CMP/SUB→Jcc fusion. Memory destinations
        // spill NZCV across the store.
        EMIT_BINARY_ALU(
            emit(arm64::subs_w(alu_dst, alu_dst, alu_src)),
            CpuState::FlagOp::Sub32);
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    case Opcode::AND_RM32_R32:
    case Opcode::AND_R32_RM32:
    case Opcode::AND_RM32_IMM32:
    case Opcode::AND_RM32_IMM8:
    case Opcode::AND_EAX_IMM32: {
        EMIT_BINARY_ALU(
            emit(arm64::ands_w(alu_dst, alu_dst, alu_src)),
            CpuState::FlagOp::And32);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    case Opcode::OR_RM32_R32:
    case Opcode::OR_R32_RM32:
    case Opcode::OR_RM32_IMM32:
    case Opcode::OR_RM32_IMM8:
    case Opcode::OR_EAX_IMM32: {
        EMIT_BINARY_ALU(
            emit(arm64::orr_w(alu_dst, alu_dst, alu_src)),
            CpuState::FlagOp::Or32);
        break;
    }

    case Opcode::XOR_RM32_R32:
    case Opcode::XOR_R32_RM32:
    case Opcode::XOR_RM32_IMM32:
    case Opcode::XOR_RM32_IMM8:
    case Opcode::XOR_EAX_IMM32: {
        EMIT_BINARY_ALU(
            emit(arm64::eor_w(alu_dst, alu_dst, alu_src)),
            CpuState::FlagOp::Xor32);
        break;
    }

    // ---- CMP 32-bit (SUB, no store) ----
    case Opcode::CMP_RM32_R32:
    case Opcode::CMP_R32_RM32:
    case Opcode::CMP_RM32_IMM32:
    case Opcode::CMP_RM32_IMM8:
    case Opcode::CMP_EAX_IMM32: {
        bool dst_is_mem = !is_register_operand(op0.type);
        bool src_is_mem = !is_register_operand(op1.type)
                       && !is_immediate_operand(op1.type);
        const bool pe32_zero_imm = !m_block_is_64bit
            && is_immediate_operand(op1.type)
            && static_cast<u32>(op1.imm) == 0;
        if (src_is_mem) {
            if (!try_esp_disp_load32(REG_SCRATCH1, op1.mem)) {
                const u8 addr = emit_addr32(REG_SCRATCH3, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH1, addr);
            }
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else if (dst_is_mem) {
            if (!try_esp_disp_load32(REG_SCRATCH0, op0.mem)) {
                const u8 addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, addr);
            }
            if (is_immediate_operand(op1.type)) {
                if (!pe32_zero_imm) {
                    emit_mov_imm32(REG_SCRATCH1,
                                   static_cast<u32>(op1.imm));
                }
            } else {
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
            }
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
            if (is_immediate_operand(op1.type)) {
                if (!pe32_zero_imm) {
                    emit_mov_imm32(REG_SCRATCH1,
                                   static_cast<u32>(op1.imm));
                }
            } else {
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
            }
        }
        // SUBS (not SUB) so NZCV is left valid for a following Jcc to branch
        // on natively (CMP→Jcc fusion). Harmless for the lazy path, which
        // ignores NZCV. m_flags.nzcv arms the fusion for the next instruction.
        const u8 cmp_src = pe32_zero_imm ? 31 : REG_SCRATCH1;
        emit(arm64::subs_w(REG_SCRATCH0, REG_SCRATCH0, cmp_src));
        emit_set_lazy_flags(CpuState::FlagOp::Sub32,
                            cmp_src, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    // ---- TEST 32-bit (AND, no store) ----
    case Opcode::TEST_RM32_R32:
    case Opcode::TEST_EAX_IMM32: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            if (!try_esp_disp_load32(REG_SCRATCH0, op0.mem)) {
                const u8 addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, addr);
            }
            if (is_immediate_operand(op1.type))
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
            else
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
            if (is_immediate_operand(op1.type))
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
            else
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
        }
        // ANDS so NZCV is valid for a following Jcc (TEST→Jcc fusion). Only the
        // signed / zero / sign conditions fuse (ANDS clears C/V); see
        // fused_arm_cond.
        emit(arm64::ands_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And32,
                            REG_SCRATCH1, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    case Opcode::TEST_RM32_IMM32: {
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            if (!try_esp_disp_load32(REG_SCRATCH0, op0.mem)) {
                const u8 addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, addr);
            }
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
        emit(arm64::ands_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::And32,
                            REG_SCRATCH1, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

#undef EMIT_BINARY_ALU

} // namespace vitreo::cpu::jit
