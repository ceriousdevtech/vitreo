// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — INC, DEC, NEG and NOT at 8, 16 and 32 bits; the
//              32-bit-path twin of ops/unary64.cpp.
//
// One family of compile_arithmetic's lowerings, split out of
// ops/arithmetic.cpp (ADR-0006 step 3) as pure code motion: the arms are
// verbatim and in their original order. compile_arithmetic
// (ops/arithmetic.cpp) owns the dispatch chain and the end-of-block tail;
// the declaration in jit_compiler_lowering.hpp states what the nullopt
// return means.
//
// EMIT_UNARY_RMW_PRESERVE_CF moved with INC_RM32/DEC_RM32, its only
// users; EMIT_UNARY_RMW, unused today, moved with its sibling.

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <optional>

namespace vitreo::cpu::jit {

// ============================================================================
// Unary RMW macro — shared by INC/DEC/NEG/NOT
// ============================================================================
#define EMIT_UNARY_RMW(op_emit, flag_op, result_reg)                          \
    do {                                                                       \
        bool is_mem = !is_register_operand(op0.type);                          \
        u8 mem_addr = 0xFF;                                                    \
        if (is_mem) {                                                          \
            if (!try_esp_disp_load32(REG_SCRATCH0, op0.mem)) {                  \
                mem_addr = emit_addr32(REG_CALL_TMP, op0.mem, next_rip);       \
                emit_guest_load32(REG_SCRATCH0, mem_addr);                     \
            }                                                                  \
        } else {                                                               \
            emit_load_gpr32(REG_SCRATCH0, op0.reg);                            \
        }                                                                      \
        emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));                        \
        op_emit;                                                               \
        emit_set_lazy_flags(flag_op, REG_SCRATCH1, result_reg);                \
        if (is_mem) {                                                          \
            m_flags.hold_nzcv = true;                                                \
            if (!try_esp_disp_store32(result_reg, op0.mem)) {                   \
                if (mem_addr == 0xFF)                                          \
                    mem_addr = emit_addr32(REG_CALL_TMP, op0.mem, next_rip);   \
                emit_guest_store32(result_reg, mem_addr);                      \
            }                                                                  \
            m_flags.hold_nzcv = false;                                               \
        } else {                                                               \
            emit_store_gpr32(result_reg, op0.reg);                             \
        }                                                                      \
    } while (0)

#define EMIT_UNARY_RMW_PRESERVE_CF(op_emit, flag_op, result_reg)              \
    do {                                                                       \
        bool is_mem = !is_register_operand(op0.type);                          \
        u8 mem_addr = 0xFF;                                                    \
        if (is_mem) {                                                          \
            if (!try_esp_disp_load32(REG_SCRATCH0, op0.mem)) {                  \
                mem_addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);       \
                emit_guest_load32(REG_SCRATCH0, mem_addr);                     \
                /* The load's cold path calls a helper with arguments in     */\
                /* x0-x2, so an address left in REG_SCRATCH2 does not        */\
                /* survive it and the store below would write through a dead */\
                /* register. The two macros above are safe only because they */\
                /* can spend REG_CALL_TMP (x21, callee-saved) on the address; */\
                /* this one cannot -- x21 already holds the carry. Drop the  */\
                /* cache and recompute, which is what the hand-written NEG   */\
                /* arms have always done. emit_addr32 returns a PIN instead  */\
                /* of the scratch for a plain [reg], and a pin does survive, */\
                /* which is why this only ever bit the forms the pin path    */\
                /* declines: a segment override, an index, a displacement.   */\
                if (mem_addr == REG_SCRATCH2) mem_addr = 0xFF;                 \
            }                                                                  \
        } else {                                                               \
            emit_load_gpr32(REG_SCRATCH0, op0.reg);                            \
        }                                                                      \
        emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));                        \
        op_emit;                                                               \
        emit_set_lazy_flags_preserve_cf(                                       \
            flag_op, REG_SCRATCH1, result_reg, REG_CALL_TMP);                  \
        if (is_mem) {                                                          \
            m_flags.hold_nzcv = true;                                                \
            if (!try_esp_disp_store32(result_reg, op0.mem)) {                   \
                if (mem_addr == 0xFF)                                          \
                    mem_addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);   \
                emit_guest_store32(result_reg, mem_addr);                      \
            }                                                                  \
            m_flags.hold_nzcv = false;                                               \
        } else {                                                               \
            emit_store_gpr32(result_reg, op0.reg);                             \
        }                                                                      \
    } while (0)

std::optional<bool> JitCompiler::compile_unary(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];

    switch (instr.opcode) {

    // ---- INC / DEC / NEG / NOT ----
    // Intel SDM Vol. 2A, INC/DEC: update OF/SF/ZF/AF/PF for the selected
    // operand width while preserving CF. The preserved carry is captured in
    // the lazy-flag record before the read/modify/write sequence.
    case Opcode::INC_RM8:
    case Opcode::DEC_RM8: {
        const bool increment = instr.opcode == Opcode::INC_RM8;
        const bool is_mem = !is_register_operand(op0.type);
        emit_lazy_cf_to_call_tmp();
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg,
                           instr.prefixes.has_rex());
        }
        emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
        if (increment) {
            emit(arm64::add_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        } else {
            emit_mov_imm32(REG_SCRATCH3, 1);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
        }
        emit_set_lazy_flags_preserve_cf(
            increment ? CpuState::FlagOp::Inc8 : CpuState::FlagOp::Dec8,
            REG_SCRATCH1, REG_SCRATCH0, REG_CALL_TMP);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg,
                            instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::INC_RM16:
    case Opcode::DEC_RM16: {
        const bool increment = instr.opcode == Opcode::INC_RM16;
        const bool is_mem = !is_register_operand(op0.type);
        emit_lazy_cf_to_call_tmp();
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
        if (increment) {
            emit(arm64::add_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        } else {
            emit_mov_imm32(REG_SCRATCH3, 1);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
        }
        emit_set_lazy_flags_preserve_cf(
            increment ? CpuState::FlagOp::Inc16 : CpuState::FlagOp::Dec16,
            REG_SCRATCH1, REG_SCRATCH0, REG_CALL_TMP);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::INC_RM32: {
        emit_lazy_cf_to_call_tmp();
        // Immediate ADDS: emit_guest_load32 clobbers W3, so a register +1
        // would increment by the region base instead of 1. Intel SDM Vol. 2
        // INC r/m32. CF stays on the lazy overlay; fused_arm_cond rejects
        // unsigned conditions.
        EMIT_UNARY_RMW_PRESERVE_CF(
            emit(arm64::adds_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1)),
            CpuState::FlagOp::Inc32, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::IncDec;
        break;
    }

    case Opcode::DEC_RM32: {
        emit_lazy_cf_to_call_tmp();
        EMIT_UNARY_RMW_PRESERVE_CF(
            emit(arm64::subs_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1)),
            CpuState::FlagOp::Dec32, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::IncDec;
        break;
    }

    case Opcode::NEG_RM32: {
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::sub_w(REG_SCRATCH1, 31, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Neg32,
                            REG_SCRATCH0, REG_SCRATCH1);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH1, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH1, op0.reg);
        }
        break;
    }

    case Opcode::NEG_RM8: {
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        // W1 = 0 - W0; low 8 bits hold the 8-bit negation.
        emit(arm64::sub_w(REG_SCRATCH1, 31, REG_SCRATCH0));
        // Lazy Sub8 with a=0 implied (a = dst + src mod 256).
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH0, REG_SCRATCH1);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH1, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH1, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::NEG_RM16: {
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        // W1 = 0 - W0; low 16 bits hold the 16-bit negation.
        emit(arm64::sub_w(REG_SCRATCH1, 31, REG_SCRATCH0));
        // Lazy Sub16 with a=0 implied (a = dst + src mod 65536).
        emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                            REG_SCRATCH0, REG_SCRATCH1);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH1, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH1, op0.reg);
        }
        break;
    }

    case Opcode::NOT_RM8: {
        // Intel SDM Vol. 2A, NOT: invert every bit in the destination and
        // leave all status flags unchanged. The byte-register helpers retain
        // the untouched bytes of the containing GPR, including AH/CH/DH/BH.
        const bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit(arm64::mvn_w(REG_SCRATCH0, REG_SCRATCH0));
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    case Opcode::NOT_RM32: {
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mvn_w(REG_SCRATCH0, REG_SCRATCH0));
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

#undef EMIT_UNARY_RMW
#undef EMIT_UNARY_RMW_PRESERVE_CF

} // namespace vitreo::cpu::jit
