// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Stack opcodes (PUSH, POP)

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

bool JitCompiler::compile_stack(const DecodedInstruction& instr,
                                 u64 instr_rip, u64 next_rip,
                                 bool is_last) {
    const auto& op0 = instr.operands[0];

    // A 66H operand-size override makes PUSH/POP a 2-byte stack transfer in
    // every mode (Intel SDM Vol. 2). Every lowering below hardcodes the
    // default slot width, so leave the 16-bit forms to the interpreter rather
    // than moving the stack pointer by the wrong amount.
    if (instr.prefixes.operand_size) return false;

    // ---- Stack-write fault atomicity ----
    //
    // Intel SDM Vol. 2, PUSH: "If a page fault or alignment check occurs, the
    // ESP register is restored to its original value" — the instruction is
    // aborted, not partially retired. So the architectural ESP commit must
    // come AFTER the fault-capable store, never before.
    //
    // This is not spec pedantry. A faulting stack write is NORMAL on Windows:
    // touching the guard page is how the stack grows. The handler commits a
    // page and the instruction RETRIES — and with ESP already decremented the
    // retry decrements it again, so the guest's stack pointer drifts by 4 on
    // every guard-page hit.
    //
    // The new ESP therefore waits in REG_CALL_TMP, which is callee-saved and
    // which the cold memory stubs are documented never to touch precisely so
    // callers can park a value across an access. POP below already relies on
    // that. The x64 CALL lowering states the same rule (ops64.cpp: "Keep
    // architectural RSP unchanged until the fault-capable stack write
    // retires").

    switch (instr.opcode) {

    // ---- PUSH r32 ----
    case Opcode::PUSH_R32: {
        // The source register must be read *before* RSP is decremented:
        // `PUSH ESP` (0x54) stores the value ESP held on entry, not the
        // already-decremented one. Re-loading op0.reg after the RSP store
        // pushes ESP-4 and silently shifts every stack-relative value the
        // callee derives from it.
        emit_load_gpr32(REG_SCRATCH1, op0.reg);
        emit_load_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
        emit_guest_store32(REG_SCRATCH1, REG_CALL_TMP);
        emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        break;
    }

    // ---- PUSH imm32 / imm8 ----
    case Opcode::PUSH_IMM32:
    case Opcode::PUSH_IMM8: {
        emit_load_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op0.imm));
        emit_guest_store32(REG_SCRATCH1, REG_CALL_TMP);
        emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        break;
    }

    // ---- PUSH r/m32 ----
    case Opcode::PUSH_RM32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        // The W9 stash the old sequence needed is gone with the ESP reload it
        // protected: REG_CALL_TMP is callee-saved, so the source value in
        // REG_SCRATCH1 is the only thing that has to reach the store.
        emit_load_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
        emit_guest_store32(REG_SCRATCH1, REG_CALL_TMP);
        emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        break;
    }

    // ---- POP r32 ----
    case Opcode::POP_R32: {
        // Write the destination register last, so `POP ESP` ends with ESP
        // holding the popped value rather than value+4. Mirrors POP_R64.
        // REG_CALL_TMP (callee-saved) holds the old RSP across the BLR that
        // emit_guest_load32 may emit.
        emit_load_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit_guest_load32(REG_SCRATCH1, REG_CALL_TMP);
        emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
        emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit_store_gpr32(REG_SCRATCH1, op0.reg);
        break;
    }

    // ---- POP r/m32 — 8F /0 ----
    case Opcode::POP_RM32: {
        // value = [ESP]; ESP += 4; r/m32 = value.
        // X21 (callee-saved) holds the old ESP across the helper calls.
        emit_load_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit_guest_load32(REG_SCRATCH0, REG_CALL_TMP);
        if (is_register_operand(op0.type)) {
            emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
            emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
            // The destination LAST: `pop esp` keeps the popped value.
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            // Intel SDM Vol. 2 POP: the EA uses the incremented ESP, and a
            // faulting destination write commits nothing -- ESP included.
            // ESP is committed after the store; the EA is the old ESP plus
            // the increment (ESP cannot be an index), wrapping at 32 bits.
            // The EA helper writes only W3/W5, so the value stays in W0.
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            if (op0.mem.has_base() &&
                op0.mem.base == static_cast<u8>(RegIndex::RSP)) {
                emit(arm64::add_w_imm(REG_SCRATCH3, REG_SCRATCH3, 4));
            }
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH3);
            emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
            emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        }
        break;
    }

    // ---- PUSHA / PUSHAD — 60 ----
    // Intel SDM Vol. 2: push EAX,ECX,EDX,EBX,original-ESP,EBP,ESI,EDI.
    // Temp ESP is captured before the first push.
    case Opcode::PUSHA: {
        const u8 esp = static_cast<u8>(RegIndex::RSP);
        emit_load_gpr32(REG_CALL_TMP, esp); // original ESP
        constexpr u8 order[] = {0, 1, 2, 3, 4, 5, 6, 7};
        // Intel SDM Vol. 2 PUSHA: a faulting slot write commits no register,
        // so ESP moves once, after the eighth store retires; a fault on any
        // slot leaves it at the original value (the interpreter's order).
        // Each slot address is recomputed from X21, callee-saved across the
        // store helper; the store clobbers the scratch pool.
        for (u8 i = 0; i < 8; ++i) {
            const u8 r = order[i];
            if (r == 4) {
                emit(arm64::mov_w(REG_SCRATCH1, REG_CALL_TMP));
            } else {
                emit_load_gpr32(REG_SCRATCH1, r);
            }
            const u32 delta = 4u * (static_cast<u32>(i) + 1u);
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_CALL_TMP, delta));
            emit_guest_store32(REG_SCRATCH1, REG_SCRATCH0);
        }
        emit(arm64::sub_w_imm(REG_SCRATCH0, REG_CALL_TMP, 32));
        emit_store_gpr32(REG_SCRATCH0, esp);
        break;
    }

    // ---- POPA / POPAD — 61 ----
    // Intel SDM Vol. 2: pop EDI,ESI,EBP,skip-ESP,EBX,EDX,ECX,EAX.
    case Opcode::POPA: {
        // Windows on Intel (tools/oracle_probe/stack_access_fault_probe
        // .expected, `popad partial`): a fault on slot i leaves ESP at its
        // starting value while the registers popped from slots 0..i-1 hold
        // their popped values -- the interpreter's order. So each register
        // is written as its slot loads and ESP moves once, at the end. X21
        // (callee-saved) keeps the starting ESP across the load helpers; the
        // ESP slot is read and discarded, as the interpreter reads it.
        const u8 esp = static_cast<u8>(RegIndex::RSP);
        emit_load_gpr32(REG_CALL_TMP, esp);
        constexpr u8 order[] = {7, 6, 5, 4, 3, 2, 1, 0};
        for (u8 i = 0; i < 8; ++i) {
            const u8 r = order[i];
            if (i == 0) {
                emit_guest_load32(REG_SCRATCH1, REG_CALL_TMP);
            } else {
                emit(arm64::add_w_imm(REG_SCRATCH0, REG_CALL_TMP, 4u * i));
                emit_guest_load32(REG_SCRATCH1, REG_SCRATCH0);
            }
            if (r != 4) emit_store_gpr32(REG_SCRATCH1, r);
        }
        emit(arm64::add_w_imm(REG_SCRATCH0, REG_CALL_TMP, 32));
        emit_store_gpr32(REG_SCRATCH0, esp);
        break;
    }

    // ---- ENTER imm16, imm8 — C8 ----
    //
    // Intel SDM Vol. 2 ENTER. NestingLevel is an immediate, so the
    // display-copy loop is unrolled at compile time:
    //
    //   Push(rBP); FrameTemp := rSP
    //   if nesting > 0 {
    //       for i in 1 .. nesting-1 { rBP -= slot; Push([rBP]) }
    //       Push(FrameTemp)
    //   }
    //   rBP := FrameTemp; rSP -= AllocSize
    //
    // In long mode the operand size is forced to 64 and StackAddrSize is 64,
    // so the slot is 8 bytes and every rSP/rBP touch must be full width.
    // Matches interpreter exec_enter (interpreter/ops/indirect_ctrl.cpp),
    // which selects `stack_size = is_64bit() ? 8 : 4` — that is the contract.
    //
    // 66H (16-bit frame) never reaches here: the operand-size gate at the top
    // of compile_stack sends it to the delegate in both modes.
    case Opcode::ENTER: {
        const u16 frame_size = static_cast<u16>(op0.imm);
        const u8 nesting =
            static_cast<u8>(instr.operands[1].imm) & 0x1Fu;
        const u8 bp = static_cast<u8>(RegIndex::RBP);
        const u8 sp = static_cast<u8>(RegIndex::RSP);
        const u32 slot = m_block_is_64bit ? 8u : 4u;

        // ENTER leaves EFLAGS unaffected, but the bounds check inside every
        // guest load/store is a SUBS. The store helpers hold NZCV for
        // themselves; the load helpers do not, so hold it across the whole
        // sequence. emit_nzcv_spill is depth-counted, so the inner holds
        // inside emit_guest_store32/64 collapse into this one.
        const bool hold_flags = should_hold_nzcv();
        if (hold_flags) emit_nzcv_spill();

        // Registers: in a 64-bit block RSP/RBP are pinned (X12/X13) and every
        // guest access clobbers X0-X4/X8-X10/X17 even on its inline fast path
        // (emit_x64_region_load/store). So rSP and rBP are re-read from the
        // pin at each step and never held in a scratch across an access, and
        // FrameTemp lives in REG_CALL_TMP (X21, callee-saved, restored around
        // emit_helper_blr by the pin store/reload).
        // rSP is committed only after the store retires (interpreter
        // `push_value`: write_memory, then set_sp). The post-store re-read
        // of the pin is not redundant — the store clobbers the scratch pool,
        // and the pin still holds the pre-push rSP precisely because nothing
        // has committed yet.
        const auto emit_push_slot = [&](u8 value_reg) {
            emit_load_gpr_ptr(REG_SCRATCH0, sp);
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, slot));
            if (m_block_is_64bit) {
                emit_guest_store64(value_reg, REG_SCRATCH0);
            } else {
                emit_guest_store32(value_reg, REG_SCRATCH0);
            }
            emit_load_gpr_ptr(REG_SCRATCH0, sp);
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, slot));
            emit_store_gpr_ptr(REG_SCRATCH0, sp);
        };

        // Push(rBP)
        emit_load_gpr_ptr(REG_SCRATCH1, bp);
        emit_push_slot(REG_SCRATCH1);

        // FrameTemp := rSP, after the push.
        emit_load_gpr_ptr(REG_CALL_TMP, sp);

        if (nesting > 0) {
            for (u8 i = 1; i < nesting; ++i) {
                // rBP := rBP - slot; Push([rBP])
                emit_load_gpr_ptr(REG_SCRATCH0, bp);
                emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, slot));
                emit_store_gpr_ptr(REG_SCRATCH0, bp);
                if (m_block_is_64bit) {
                    emit_guest_load64(REG_SCRATCH1, REG_SCRATCH0);
                } else {
                    emit_guest_load32(REG_SCRATCH1, REG_SCRATCH0);
                }
                // Only the load's destination survives the access; the push
                // below re-reads rSP from the pin, so X1 stays intact until
                // the store consumes it (the store stashes src into X17
                // before touching anything else).
                emit_push_slot(REG_SCRATCH1);
            }
            // Push(FrameTemp) — nesting level 1..31
            emit_push_slot(REG_CALL_TMP);
        }

        // rBP := FrameTemp; rSP := rSP - AllocSize
        emit_store_gpr_ptr(REG_CALL_TMP, bp);
        emit_load_gpr_ptr(REG_SCRATCH0, sp);
        if (frame_size == 0) {
            // no-op
        } else if (frame_size <= 0xFFFu) {
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, frame_size));
        } else {
            emit_mov_imm32(REG_SCRATCH1, frame_size);
            emit(arm64::sub_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        }
        emit_store_gpr_ptr(REG_SCRATCH0, sp);

        if (hold_flags) emit_nzcv_fill();
        break;
    }

    default: return false;
    }

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

} // namespace vitreo::cpu::jit
