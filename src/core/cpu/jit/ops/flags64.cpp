// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: ops that read or write RFLAGS
// directly. CMOVcc r64, SETcc, LAHF/SAHF, CLC/STC/CMC and CLD/STD.
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
std::optional<bool> JitCompiler::compile_flags64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // CMOVcc r64, r/m64  — REX.W 0F 4x /r
    //
    // If cc is true, dest = src; otherwise dest is unchanged.
    // Implemented as: cond = cond_fn(cpu); new = src; dest = cond ? new : dest;
    // No flag effect.
    // ========================================================================
    case Opcode::CMOVO_R64_RM64:
    case Opcode::CMOVNO_R64_RM64:
    case Opcode::CMOVB_R64_RM64:
    case Opcode::CMOVNB_R64_RM64:
    case Opcode::CMOVZ_R64_RM64:
    case Opcode::CMOVNZ_R64_RM64:
    case Opcode::CMOVBE_R64_RM64:
    case Opcode::CMOVNBE_R64_RM64:
    case Opcode::CMOVS_R64_RM64:
    case Opcode::CMOVNS_R64_RM64:
    case Opcode::CMOVP_R64_RM64:
    case Opcode::CMOVNP_R64_RM64:
    case Opcode::CMOVL_R64_RM64:
    case Opcode::CMOVNL_R64_RM64:
    case Opcode::CMOVLE_R64_RM64:
    case Opcode::CMOVNLE_R64_RM64: {
        // Map CMOV opcode → cc index (0..15) using the same encoding as Jcc.
        // CMOVcc opcodes are 0F 40 + cc, so the low 4 bits of (op - CMOVO) are cc.
        u8 cc;
        switch (instr.opcode) {
        case Opcode::CMOVO_R64_RM64:   cc = 0x0; break;
        case Opcode::CMOVNO_R64_RM64:  cc = 0x1; break;
        case Opcode::CMOVB_R64_RM64:   cc = 0x2; break;
        case Opcode::CMOVNB_R64_RM64:  cc = 0x3; break;
        case Opcode::CMOVZ_R64_RM64:   cc = 0x4; break;
        case Opcode::CMOVNZ_R64_RM64:  cc = 0x5; break;
        case Opcode::CMOVBE_R64_RM64:  cc = 0x6; break;
        case Opcode::CMOVNBE_R64_RM64: cc = 0x7; break;
        case Opcode::CMOVS_R64_RM64:   cc = 0x8; break;
        case Opcode::CMOVNS_R64_RM64:  cc = 0x9; break;
        case Opcode::CMOVP_R64_RM64:   cc = 0xA; break;
        case Opcode::CMOVNP_R64_RM64:  cc = 0xB; break;
        case Opcode::CMOVL_R64_RM64:   cc = 0xC; break;
        case Opcode::CMOVNL_R64_RM64:  cc = 0xD; break;
        case Opcode::CMOVLE_R64_RM64:  cc = 0xE; break;
        case Opcode::CMOVNLE_R64_RM64: cc = 0xF; break;
        default: return false;
        }

        const int cmov_fac = fused_arm_cond(m_flags.incoming_nzcv, cc);
        const bool cmov_src_is_reg = is_register_operand(op1.type);

        // Load source value (r/m64) into X21 (callee-saved, survives BLR).
        //
        // The r/m64 memory form may NOT capture the fused condition into a
        // GPR before the load: emit_x64_region_load bounds-checks with SUBS
        // (clobbering NZCV) and, on a region-cache miss, calls the read
        // helper with X0 = JitContext* and returns its value in X0 — so a
        // CSET taken into W0 beforehand is destroyed exactly on the slow
        // path. Hold NZCV across the load instead and select afterwards with
        // the fused condition (Intel SDM Vol. 2 CMOVcc: Flags Affected =
        // None, and the condition is the one live at the instruction).
        if (cmov_src_is_reg) {
            emit_load_gpr64(REG_CALL_TMP, op1.reg);
        } else {
            if (cmov_fac >= 0) emit_nzcv_spill();
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op1);
            emit_guest_load64(REG_CALL_TMP, REG_SCRATCH3);
            if (cmov_fac >= 0) emit_nzcv_fill();
        }

        if (cmov_fac >= 0) {
            // Load current dest value into X2 (the "false" leg of the select).
            emit_load_gpr64(REG_SCRATCH2, op0.reg);
            emit(arm64::csel_x(REG_SCRATCH0, REG_CALL_TMP, REG_SCRATCH2,
                               static_cast<u8>(cmov_fac)));
            // CSEL, the GPR moves and the store below leave NZCV alone, and
            // the register form has no access to hold it across: a following
            // reader of the same publisher still fuses (Intel SDM Vol. 2
            // CMOVcc: Flags Affected = None).
            if (cmov_src_is_reg) m_flags.nzcv = m_flags.incoming_nzcv;
        } else {
            emit_eval_lazy_condition(cc);   // 0/1 in W0; may BLR
            emit_load_gpr64(REG_SCRATCH2, op0.reg);
            // CMP W0, #0 (subs WZR, W0, #0 — sets Z if cond was false)
            emit(arm64::cmp_w_imm(0, 0));
            // CSEL X0, X21, X2, NE → X0 = (cond != 0) ? src : old_dest
            emit(arm64::csel_x(REG_SCRATCH0, REG_CALL_TMP, REG_SCRATCH2,
                               arm64::NE));
        }

        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // CMOVcc r32, r/m32 is lowered once, in compile_memory_ops, which the
    // 64-bit chain reaches immediately after this compiler. That copy carries
    // the register-pin fast path this one never had (a pinned dest and source
    // collapse to a single CSEL), and its not-taken path now zero-extends
    // DEST[63:32] per Intel SDM Vol. 2 CMOVcc, which was the only thing the
    // x64 copy did better.

    // ========================================================================
    // SETcc RM8 — write 0 or 1 to an 8-bit register destination.
    //
    // Only register destinations with index 0-3 (AL/CL/DL/BL) are handled
    // natively. Indices 4-7 have AH/SPL ambiguity without knowing the REX
    // state at emit time; indices 8-15 require a different low-byte store
    // path. All others fall through to the interpreter delegate.
    // ========================================================================
    case Opcode::SETO_RM8:   case Opcode::SETNO_RM8:
    case Opcode::SETB_RM8:   case Opcode::SETNB_RM8:
    case Opcode::SETZ_RM8:   case Opcode::SETNZ_RM8:
    case Opcode::SETBE_RM8:  case Opcode::SETNBE_RM8:
    case Opcode::SETS_RM8:   case Opcode::SETNS_RM8:
    case Opcode::SETP_RM8:   case Opcode::SETNP_RM8:
    case Opcode::SETL_RM8:   case Opcode::SETNL_RM8:
    case Opcode::SETLE_RM8:  case Opcode::SETNLE_RM8: {
        if (!is_register_operand(op0.type) || op0.reg >= 4) return false;

        u8 cc;
        switch (instr.opcode) {
        case Opcode::SETO_RM8:   cc = 0x0; break;
        case Opcode::SETNO_RM8:  cc = 0x1; break;
        case Opcode::SETB_RM8:   cc = 0x2; break;
        case Opcode::SETNB_RM8:  cc = 0x3; break;
        case Opcode::SETZ_RM8:   cc = 0x4; break;
        case Opcode::SETNZ_RM8:  cc = 0x5; break;
        case Opcode::SETBE_RM8:  cc = 0x6; break;
        case Opcode::SETNBE_RM8: cc = 0x7; break;
        case Opcode::SETS_RM8:   cc = 0x8; break;
        case Opcode::SETNS_RM8:  cc = 0x9; break;
        case Opcode::SETP_RM8:   cc = 0xA; break;
        case Opcode::SETNP_RM8:  cc = 0xB; break;
        case Opcode::SETL_RM8:   cc = 0xC; break;
        case Opcode::SETNL_RM8:  cc = 0xD; break;
        case Opcode::SETLE_RM8:  cc = 0xE; break;
        case Opcode::SETNLE_RM8: cc = 0xF; break;
        default: return false;
        }

        const int set_fac = fused_arm_cond(m_flags.incoming_nzcv, cc);
        emit_eval_jcc(cc, set_fac);
        // W0 is already 0 or 1. Store into low byte of destination GPR.
        emit_store_gpr8(0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    // LAHF — AH = (rflags[7:0]) | 0x02 (bit 1 always 1). Valid in long mode
    // because CPUID.80000001H:ECX.LAHF-SAHF[0] is advertised. Intel SDM Vol. 2.
    case Opcode::LAHF: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit_mov_imm32(REG_SCRATCH4, 0xFFu);
        emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 2));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_SCRATCH3, 8));
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

    // SAHF — SF,ZF,AF,PF,CF from AH bits 7,6,4,2,0. Intel SDM Vol. 2.
    case Opcode::SAHF: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();
        u32 rax_off = gpr_off(static_cast<u8>(RegIndex::RAX));
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rax_off >> 3));
        emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 8));
        emit_mov_imm32(REG_SCRATCH4, 0xD5u);  // SF|ZF|AF|PF|CF
        emit(arm64::and_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::bic_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
        emit(arm64::movz_w(REG_SCRATCH4, 2));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rf_off >> 3));
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
    // materialize of a pending ADD/SUB/… — that was a top call-site on
    // GoogleUpdater after M5v3 made the materialize body itself cheap.
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

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
