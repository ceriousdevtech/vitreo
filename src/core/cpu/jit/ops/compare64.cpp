// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: compares. CMP/TEST r/m64 and
// CMP/TEST r/m32 (flags only, nothing stored).
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// ============================================================================
// CMP / TEST template — 64-bit (compute but don't store)
// ============================================================================
// `alu_emit` computes X0 = lhs_reg OP rhs_reg. A compare writes no guest
// register, so a pinned register operand is read in place instead of being
// copied into X0/X1 first; the lazy record then names the pin as its source.
// That stays exact on the deferred path too: only the instruction right after
// may replay the record (a Jcc, which writes no GPR).
#define EMIT_COMPARE64(alu_emit, flag_op)                                    \
    do {                                                                      \
        bool dst_is_mem = is_mem(op0);                                        \
        bool src_is_mem = is_mem(op1);                                        \
        bool src_is_imm = is_imm(op1);                                        \
        u8 lhs_reg = REG_SCRATCH0;                                            \
        u8 rhs_reg = REG_SCRATCH1;                                            \
        if (src_is_mem) {                                                     \
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);               \
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH3);                    \
            /* Read after the access: its slow path preserves pins. */        \
            const u8 lhs_pin = pinned_gpr64(op0.reg);                         \
            if (lhs_pin != 0xFF) lhs_reg = lhs_pin;                           \
            else emit_load_gpr64(REG_SCRATCH0, op0.reg);                      \
        } else if (dst_is_mem) {                                              \
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);               \
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);                    \
            if (src_is_imm) {                                                 \
                emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));                  \
            } else {                                                          \
                const u8 rhs_pin = pinned_gpr64(op1.reg);                     \
                if (rhs_pin != 0xFF) rhs_reg = rhs_pin;                       \
                else emit_load_gpr64(REG_SCRATCH1, op1.reg);                  \
            }                                                                 \
        } else {                                                              \
            const u8 lhs_pin = pinned_gpr64(op0.reg);                         \
            if (lhs_pin != 0xFF) lhs_reg = lhs_pin;                           \
            else emit_load_gpr64(REG_SCRATCH0, op0.reg);                      \
            if (src_is_imm) {                                                 \
                emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));                  \
            } else {                                                          \
                const u8 rhs_pin = pinned_gpr64(op1.reg);                     \
                if (rhs_pin != 0xFF) rhs_reg = rhs_pin;                       \
                else emit_load_gpr64(REG_SCRATCH1, op1.reg);                  \
            }                                                                 \
        }                                                                     \
        alu_emit;                                                             \
        emit_set_lazy_flags64(flag_op, rhs_reg, REG_SCRATCH0);                \
    } while (0)

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_compare64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // CMP/TEST never store, so a memory operand is only ever read — and that
    // read happens before the ALU op, leaving NZCV valid for a following Jcc.
    case Opcode::CMP_RM64_R64:
    case Opcode::CMP_R64_RM64:
    case Opcode::CMP_RM64_IMM32:
    case Opcode::CMP_RM64_IMM8:
    case Opcode::CMP_RAX_IMM32: {
        EMIT_COMPARE64(
            emit(arm64::subs_x(REG_SCRATCH0, lhs_reg, rhs_reg)),
            CpuState::FlagOp::Sub64);
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    case Opcode::TEST_RM64_R64:
    case Opcode::TEST_RM64_IMM32:
    case Opcode::TEST_RAX_IMM32: {
        EMIT_COMPARE64(
            emit(arm64::ands_x(REG_SCRATCH0, lhs_reg, rhs_reg)),
            CpuState::FlagOp::And64);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    // CMP 32-bit — same as SUB but no store.
    case Opcode::CMP_RM32_R32:
    case Opcode::CMP_R32_RM32:
    case Opcode::CMP_RM32_IMM32:
    case Opcode::CMP_RM32_IMM8:
    case Opcode::CMP_EAX_IMM32: {
        // CMP r32,0 needs neither a scratch copy nor an immediate
        // materialisation when the guest register is pinned.  SUBS to WZR
        // sets exactly the same NZCV, while the unchanged pin is also the
        // subtraction result that the lazy x86 flag record requires.
        if (is_register_operand(op0.type) && is_imm(op1) &&
            static_cast<u32>(op1.imm) == 0) {
            const u8 pin = pinned_gpr32(op0.reg);
            if (pin != 0xFF) {
                emit(arm64::subs_w(31, pin, 31));
                emit_set_lazy_flags(CpuState::FlagOp::Sub32, 31, pin);
                m_flags.nzcv = NzcvState::Sub;
                break;
            }
        }
        auto read32_to = [&](u8 dst, const Operand& o) {
            emit_compute_ea64(REG_SCRATCH4, o.mem, next_rip);
            emit_guest_load32(dst, REG_SCRATCH4);
        };
        // Register operands that are pinned are read in place (W form: the
        // SUBS reads bits 31:0 only), as in EMIT_COMPARE64; the 32-bit lazy
        // record zero-extends its source itself.
        u8 lhs_reg = REG_SCRATCH0;
        u8 rhs_reg = REG_SCRATCH1;
        // The memory operand (at most one) is read first; register operands
        // are read afterwards, from their pins when pinned -- the access's
        // slow path preserves pins -- so nothing is held across it.
        if (is_mem(op0)) read32_to(REG_SCRATCH0, op0);
        if (is_mem(op1)) read32_to(REG_SCRATCH1, op1);
        if (is_register_operand(op0.type)) {
            const u8 pin = pinned_gpr32(op0.reg);
            if (pin != 0xFF) lhs_reg = pin;
            else emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
        } else if (is_register_operand(op1.type)) {
            const u8 pin = pinned_gpr32(op1.reg);
            if (pin != 0xFF) rhs_reg = pin;
            else emit_load_gpr32(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::subs_w(REG_SCRATCH0, lhs_reg, rhs_reg));
        emit_set_lazy_flags(CpuState::FlagOp::Sub32, rhs_reg, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    // TEST 32-bit.
    case Opcode::TEST_RM32_R32:
    case Opcode::TEST_EAX_IMM32:
    case Opcode::TEST_RM32_IMM32: {
        // A pinned register self-test needs neither operand copy. Keep W0 as
        // the result so lazy SF/ZF/PF materialization remains identical to
        // the generic path while the following Jcc can consume this ANDS.
        if (instr.opcode == Opcode::TEST_RM32_R32 &&
            is_register_operand(op0.type) &&
            is_register_operand(op1.type) && op0.reg == op1.reg) {
            const u8 pin = pinned_gpr32(op0.reg);
            if (pin != 0xFF) {
                // Intel SDM Vol. 2 ADD/SUB/TEST: TEST's SF/ZF are the N/Z of its
                // AND result, while CF/OF are cleared and PF is derived from
                // that result. A preceding proven ALU write to this exact pin left
                // the same result's N/Z in PSTATE. Its omitted lazy record is
                // the proof of both producer identity and adjacency. Reuse
                // only N/Z, publish TEST's own lazy record for every other
                // architectural flag, and avoid a dependent ANDS.
                const bool replayable_add =
                    m_flags.incoming_nzcv == NzcvState::Add &&
                    m_incoming_stub_store.valid &&
                    m_incoming_stub_store.op == CpuState::FlagOp::Add32 &&
                    m_incoming_stub_store.dst == pin;
                const bool replayable_sub =
                    m_flags.incoming_nzcv == NzcvState::Sub &&
                    m_incoming_stub_store.valid &&
                    m_incoming_stub_store.op == CpuState::FlagOp::Sub32 &&
                    m_incoming_stub_store.dst == pin;
                const bool exact_alu_nz =
                    m_flags.incoming_nzcv == NzcvState::NzOnly &&
                    m_incoming_alu32_nz_result == pin;
                if (replayable_add || replayable_sub || exact_alu_nz) {
                    emit_set_lazy_flags(CpuState::FlagOp::And32, pin, pin);
                    m_flags.nzcv = NzcvState::NzOnly;
                    break;
                }
                emit(arm64::ands_w(REG_SCRATCH0, pin, pin));
                emit_set_lazy_flags(CpuState::FlagOp::And32, pin,
                                    REG_SCRATCH0);
                m_flags.nzcv = NzcvState::And;
                break;
            }
        }
        auto read32_to = [&](u8 dst, const Operand& o) {
            emit_compute_ea64(REG_SCRATCH4, o.mem, next_rip);
            emit_guest_load32(dst, REG_SCRATCH4);
        };
        // As for CMP: the memory operand first, then pinned registers in
        // place. The AND result still lands in W0 for the lazy record.
        u8 lhs_reg = REG_SCRATCH0;
        u8 rhs_reg = REG_SCRATCH1;
        if (is_mem(op0)) read32_to(REG_SCRATCH0, op0);
        if (is_mem(op1)) read32_to(REG_SCRATCH1, op1);
        if (is_register_operand(op0.type)) {
            const u8 pin = pinned_gpr32(op0.reg);
            if (pin != 0xFF) lhs_reg = pin;
            else emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
        } else if (is_register_operand(op1.type)) {
            const u8 pin = pinned_gpr32(op1.reg);
            if (pin != 0xFF) rhs_reg = pin;
            else emit_load_gpr32(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::ands_w(REG_SCRATCH0, lhs_reg, rhs_reg));
        emit_set_lazy_flags(CpuState::FlagOp::And32, rhs_reg, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
