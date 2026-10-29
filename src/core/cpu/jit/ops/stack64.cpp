// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: the stack. PUSH/POP r64,
// PUSH/POP r/m64 and PUSH imm, plus the stack-limit helper that
// generated code calls through jit_ctx.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

extern "C" void jit_require_stack_limit(
    CpuState* cpu, GuestMemory* memory, u64 next_rsp) noexcept {
    if (cpu == nullptr || memory == nullptr || cpu->pending_raise.active ||
        cpu->stack_overflow_dispatch_depth != 0) {
        return;
    }
    const GuestAddr teb = static_cast<GuestAddr>(cpu->seg.gs_desc.base);
    const auto stack_limit = memory->read_u64(teb + 0x10);
    if (!stack_limit.is_ok()) return;
    // SetThreadStackGuarantee reserves recovery space for handling an
    // overflow; it does not move NT_TIB.StackLimit or reduce the stack
    // available to ordinary calls and pushes.
    if (next_rsp >= stack_limit.value()) {
        cpu->stack_overflow_active = false;
        return;
    }
    if (cpu->stack_overflow_active) return;
    cpu->stack_overflow_active = true;
    auto& pending = cpu->pending_raise;
    pending.active = true;
    pending.from_helper = true;
    pending.code = 0xC00000FDu;  // STATUS_STACK_OVERFLOW
    pending.flags = 0;
    pending.num_args = 0;
}

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_stack64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // PUSH / POP 64-bit  (8-byte stack)
    // ========================================================================

    case Opcode::PUSH_R64: {
        if (instr.prefixes.operand_size) return false;  // 66H: 2-byte slot
        // RSP -= 8; [RSP] = R64.  Long-mode blocks already keep the low
        // eight GPRs in full-width host pins, so operate on the pins instead
        // of bouncing both operands through X0/X2.  PUSH RSP is the one
        // exception for the value: Intel defines it to store the value from
        // before the decrement, so snapshot that pin first.
        const u8 rsp_index = static_cast<u8>(RegIndex::RSP);
        const bool pinned_rsp = m_pins_live &&
            m_pin_arm[rsp_index] != 0xFF;

        u8 value_reg = REG_SCRATCH0;
        const bool pinned_value = m_pins_live && op0.reg < 16 &&
            m_pin_arm[op0.reg] != 0xFF;
        if (pinned_value && op0.reg != rsp_index) {
            value_reg = m_pin_arm[op0.reg];
        } else {
            emit_load_gpr64(value_reg, op0.reg);
        }

        const u8 rsp_reg = pinned_rsp ? m_pin_arm[rsp_index]
                                      : REG_SCRATCH2;
        if (!pinned_rsp) emit_load_gpr64(rsp_reg, rsp_index);
        // Intel SDM Vol. 2 PUSH: a faulting stack write commits nothing. On
        // Windows the one deliverable PUSH fault is the stack guard's, whose
        // CONTEXT names the un-decremented RSP (call_fault_context_probe,
        // stack_access_fault_probe).
        if (pinned_rsp && m_current_site != ~0u) {
            // The hot form stays `SUB pin, pin, #8; STR`: the decrement is
            // recorded on this instruction's site and both fault paths (the
            // signal handler and execute() after a software raise) add the
            // 8 back, so precision costs nothing here.
            emit(arm64::sub_x_imm(rsp_reg, rsp_reg, 8));
            m_esp_frame_live = false;
            m_insn_rsp_undo[m_current_site] = 8;
            emit_guest_store64(value_reg, rsp_reg);
        } else {
            // RSP moves only after the store retires (the CALL lowering's
            // order); the slot lives in X21, callee-saved, across the
            // store's helper call.
            emit(arm64::sub_x_imm(REG_CALL_TMP, rsp_reg, 8));
            emit_guest_store64(value_reg, REG_CALL_TMP);
            if (pinned_rsp) {
                emit(arm64::mov_x(rsp_reg, REG_CALL_TMP));
                m_esp_frame_live = false;
            } else {
                emit_store_gpr64(REG_CALL_TMP, rsp_index);
            }
        }
        break;
    }

    case Opcode::POP_R64: {
        if (instr.prefixes.operand_size) return false;  // 66H: 2-byte slot
        const u8 rsp_index = static_cast<u8>(RegIndex::RSP);
        const bool pinned_rsp = m_pins_live &&
            m_pin_arm[rsp_index] != 0xFF;

        // For every destination except RSP itself, the pinned stack pointer
        // can remain the checked load address and then be incremented in
        // place.  A low destination pin can receive the loaded value directly
        // as well.  POP RSP retains the ordered scratch sequence below: its
        // final architectural value is the popped qword, not old RSP + 8.
        if (pinned_rsp && op0.reg != rsp_index) {
            const bool pinned_dst = op0.reg < 16 &&
                m_pin_arm[op0.reg] != 0xFF;
            const u8 dst_reg = pinned_dst ? m_pin_arm[op0.reg]
                                          : REG_SCRATCH0;
            emit_guest_load64(dst_reg, m_pin_arm[rsp_index]);
            emit(arm64::add_x_imm(m_pin_arm[rsp_index],
                                  m_pin_arm[rsp_index], 8));
            m_esp_frame_live = false;
            if (!pinned_dst) emit_store_gpr64(dst_reg, op0.reg);
            break;
        }

        // R64 = [RSP]; RSP += 8.
        // Use REG_CALL_TMP (X21, callee-saved) to hold the old RSP across the
        // BLR inside emit_guest_load64 — caller-saved X2 is not preserved.
        emit_load_gpr64(REG_CALL_TMP, rsp_index);
        emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 8));
        emit_store_gpr64(REG_CALL_TMP, rsp_index);
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // ========================================================================
    // PUSH / POP r/m64 (memory or extended-register forms)
    // ========================================================================

    // PUSH r/m64  — FF /6 (REX.W implicit in 64-bit mode)
    case Opcode::PUSH_RM64: {
        if (instr.prefixes.operand_size) return false;  // 66H: 2-byte slot
        // value = R/M64; RSP -= 8; [RSP] = value
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH3);
        }
        // RSP moves only after the stack write retires (see PUSH r64).
        emit_load_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_CALL_TMP, REG_CALL_TMP, 8));
        emit_guest_store64(REG_SCRATCH0, REG_CALL_TMP);
        emit_store_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        break;
    }

    // POP r/m64  — 8F /0
    case Opcode::POP_RM64: {
        if (instr.prefixes.operand_size) return false;  // 66H: 2-byte slot
        // value = [RSP]; RSP += 8; R/M64 = value.
        // REG_CALL_TMP (X21, callee-saved) holds the old RSP across the
        // helper calls of the slot load and the destination store.
        emit_load_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
        if (is_register_operand(op0.type)) {
            emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 8));
            emit_store_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
            // The destination LAST: `pop rsp` keeps the popped value.
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        } else {
            // Intel SDM Vol. 2 POP: the EA uses the incremented RSP, and a
            // faulting destination write commits nothing -- RSP included. So
            // RSP is committed only after the store; the EA is formed from
            // the old RSP plus the increment (RSP cannot be an index). The
            // EA helper writes only X3/X5, so the popped value stays in X0.
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            if (op0.mem.has_base() &&
                op0.mem.base == static_cast<u8>(RegIndex::RSP)) {
                emit(arm64::add_x_imm(REG_SCRATCH3, REG_SCRATCH3, 8));
            }
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH3);
            emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 8));
            emit_store_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        }
        break;
    }

    // ========================================================================
    // PUSH IMM — 64-bit mode: sign-extend immediate to 64 bits, push 8 bytes.
    // PUSH_IMM16 is intentionally omitted (rare; delegate handles it).
    // ========================================================================
    case Opcode::PUSH_IMM8:
    case Opcode::PUSH_IMM32: {
        emit_mov_imm64(REG_SCRATCH0, sx_imm64(op0));
        // RSP moves only after the stack write retires (see PUSH r64).
        emit_load_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_CALL_TMP, REG_CALL_TMP, 8));
        emit_guest_store64(REG_SCRATCH0, REG_CALL_TMP);
        emit_store_gpr64(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
