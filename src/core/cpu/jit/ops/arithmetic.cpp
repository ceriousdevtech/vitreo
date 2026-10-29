// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Arithmetic opcodes (ADD, SUB, AND, OR, XOR, CMP, TEST,
//              INC, DEC, NEG, NOT, ADC, SBB, 8/16/32-bit variants)
//
// compile_arithmetic was one 1,627-line switch. Its ALU families now live
// one per ops/<family>.cpp (ADR-0006 step 3), moved verbatim: alu32.cpp,
// alu16.cpp, alu8.cpp, adc_sbb.cpp and unary.cpp. The shared CF helpers
// emit_lazy_cf_to_call_tmp and emit_carry_flag_control are in
// carry_flag.cpp. This file keeps the entry point, the flag and
// byte-order arms (BSWAP, CLC/STC/CMC, CLD/STD, LAHF, SAHF), the family
// dispatch and the end-of-block tail every family shares.

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <optional>

namespace vitreo::cpu::jit {

bool JitCompiler::compile_arithmetic(const DecodedInstruction& instr,
                                      u64 instr_rip, u64 next_rip,
                                      bool is_last) {
    const auto& op0 = instr.operands[0];

    switch (instr.opcode) {

    // ---- BSWAP r32 — byte-reverse GPR low 32 bits, zero-extend to 64.
    // No flag effects. Exactly ARM64 REV (32-bit form).
    case Opcode::BSWAP_R32: {
        emit_load_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::rev_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- BSWAP r64 — byte-reverse whole 64-bit GPR. No flag effects.
    case Opcode::BSWAP_R64: {
        // Pin-aware: a 64-bit block holds the low eight GPRs in ARM registers,
        // so a direct CpuState round trip would read a stale slot and be
        // overwritten by the pin spill.
        emit_load_gpr64(REG_SCRATCH0, op0.reg);
        emit(arm64::rev_x(REG_SCRATCH0, REG_SCRATCH0));
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- CLC/STC/CMC — CF only. Leave a pending ALU FlagOp intact and
    // record the forced carry in cf_overlay so later Jcc/ADC can answer
    // ZF from FlagOp and CF from the overlay without a full materialize.
    case Opcode::CLC:
    case Opcode::STC:
    case Opcode::CMC:
        emit_carry_flag_control(instr.opcode);
        break;

    // ---- CLD / STD — clear/set DF (bit 10) on rflags. ----
    // DF is a control flag, not a STATUS_FLAGS_MASK bit: lazy FlagOp
    // materialization never writes it. Touching DF must not force a
    // materialize of a pending ADD/SUB/… — it was a measured top materialize
    // call site once M5v3 made the materialize body itself cheap.
    case Opcode::CLD:
    case Opcode::STD: {
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::movz_w(REG_SCRATCH4, 0x400));   // bit 10
        if (instr.opcode == Opcode::CLD) {
            emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        } else {
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        }
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        break;
    }

    // ---- LAHF — AH = (rflags[7:0]) | 0x02 (bit 1 always 1).
    case Opcode::LAHF: {
        // LAHF only READS flags, so it cannot discard the lazy record the way
        // a full-flag writer can — rflags must be authoritative first. But the
        // helper is a no-op whenever nothing is pending, and an unconditional
        // BLR (with its pin spill/reload) dominated this instruction. Test the
        // lazy state inline instead: flag_op == None with both overlays clear
        // is exactly the state in which materialize_lazy_flags does nothing
        // (nzcv_kind is deliberately not tested — the helper does not consume
        // it either, so it cannot distinguish the two paths).
        static_assert(offsetof(CpuState, of_overlay) ==
                      offsetof(CpuState, cf_overlay) + 1);
        static_assert((offsetof(CpuState, cf_overlay) & 1u) == 0);
        emit(arm64::ldrb_uoff(REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
        emit(arm64::ldrh_uoff(REG_SCRATCH4, REG_CPU_STATE,
                             cf_overlay_offset() >> 1));
        emit(arm64::orr_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        const usize lahf_skip_br = m_code_buf.size();
        emit(0);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();
        m_code_buf[lahf_skip_br] = arm64::cbz_w(
            REG_SCRATCH3,
            static_cast<i32>(m_code_buf.size() - lahf_skip_br) * 4);
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        // ah = (rf & 0xFF) | 0x02  →  then shifted to bits 15:8
        emit_mov_imm32(REG_SCRATCH4, 0xFFu);
        emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 2));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_SCRATCH3, 8));
        // Load RAX, clear bits 15:8, OR in the new AH byte, store.
        emit_pin_spill_one(static_cast<u8>(RegIndex::RAX));
        u32 rax_off = gpr_off(static_cast<u8>(RegIndex::RAX));
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rax_off >> 3));
        emit_mov_imm32(REG_SCRATCH4, 0xFF00u);
        emit(arm64::bic_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rax_off >> 3));
        emit_pin_reload_one(static_cast<u8>(RegIndex::RAX));
        break;
    }

    // ---- SAHF — SF,ZF,AF,PF,CF from AH bits 7,6,4,2,0.
    case Opcode::SAHF: {
        // SAHF is a PARTIAL flag writer: it sets SF/ZF/AF/PF/CF and leaves OF
        // alone (Intel SDM Vol. 2). A pending lazy FlagOp therefore cannot be
        // discarded the way COMISS discards it — the op still owns OF, and a
        // later materialize would re-derive the five bits SAHF just wrote.
        // The pending record has to be frozen into rflags first; only the
        // *unconditional* helper call is avoidable — see LAHF above for the
        // inline lazy-state test.
        //
        // The skipped BLR is also what used to flush the register pins, and
        // AH is read back out of CpuState below. Spill the RAX pin explicitly
        // so the fast path does not read a stale copy (a preceding MOV AH,imm
        // lands in the pinned host register, not in CpuState).
        emit_pin_spill_one(static_cast<u8>(RegIndex::RAX));
        emit(arm64::ldrb_uoff(REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
        emit(arm64::ldrh_uoff(REG_SCRATCH4, REG_CPU_STATE,
                             cf_overlay_offset() >> 1));
        emit(arm64::orr_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        const usize sahf_skip_br = m_code_buf.size();
        emit(0);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();
        m_code_buf[sahf_skip_br] = arm64::cbz_w(
            REG_SCRATCH3,
            static_cast<i32>(m_code_buf.size() - sahf_skip_br) * 4);
        // Fetch AH into bits 7:0 of W3.
        u32 rax_off = gpr_off(static_cast<u8>(RegIndex::RAX));
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rax_off >> 3));
        emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 8));
        emit_mov_imm32(REG_SCRATCH4, 0xD5u);  // SF|ZF|AF|PF|CF
        emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        // Load rflags, clear the D5 bits, OR in AH's D5 bits, force bit 1.
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::bic_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
        emit(arm64::movz_w(REG_SCRATCH4, 2));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rf_off >> 3));
        break;
    }

    // The ALU families split out of this switch (ADR-0006 step 3), one per
    // ops/<family>.cpp. Their opcode sets are disjoint from each other and
    // from the arms above, so the chain order is not observable; it follows
    // compile frequency. A family's true means its arm reached `break`, not
    // that the end-of-block tail ran: the tail below still runs here.
    default: {
        std::optional<bool> lowered = compile_alu32(instr, next_rip);
        if (!lowered) lowered = compile_alu8(instr, next_rip);
        if (!lowered) lowered = compile_unary(instr, next_rip);
        if (!lowered) lowered = compile_adc_sbb(instr, next_rip);
        if (!lowered) lowered = compile_alu16(instr, next_rip);
        if (!lowered.has_value() || !*lowered) return false;
        break;
    }
    }

    if (is_last) {
        emit_update_rip(next_rip);
        emit_tail_link_or_exit(next_rip, m_flags.nzcv);
    }
    return true;
}

} // namespace vitreo::cpu::jit
