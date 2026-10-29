// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_memory_ops' condition-code consumers: SETcc r/m8
// for all 16 conditions, CMOVcc r32, r/m32 and CMOVcc r16, r/m16.
//
// One family of compile_memory_ops's opcode space, split out of
// ops/memory_ops.cpp as pure code motion: the case arms are verbatim and in
// their original order. compile_memory_ops (ops/memory_ops.cpp) owns the
// dispatch chain and the block tail.

#include "../jit_helpers.hpp"
#include "core/cpu/lock_prefix.hpp"

#include <atomic>
#include <optional>

namespace vitreo::cpu::jit {

// nullopt = not in this family; false = declined; true = the arm reached
// `break`. See jit_compiler_lowering.hpp.
std::optional<bool> JitCompiler::compile_memory_ops_cond(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- SETcc — all 16 condition codes ----
    case Opcode::SETO_RM8:   case Opcode::SETNO_RM8:
    case Opcode::SETB_RM8:   case Opcode::SETNB_RM8:
    case Opcode::SETZ_RM8:   case Opcode::SETNZ_RM8:
    case Opcode::SETBE_RM8:  case Opcode::SETNBE_RM8:
    case Opcode::SETS_RM8:   case Opcode::SETNS_RM8:
    case Opcode::SETP_RM8:   case Opcode::SETNP_RM8:
    case Opcode::SETL_RM8:   case Opcode::SETNL_RM8:
    case Opcode::SETLE_RM8:  case Opcode::SETNLE_RM8: {
        u8 cond;
        switch (instr.opcode) {
        case Opcode::SETO_RM8:   cond = 0x0; break;
        case Opcode::SETNO_RM8:  cond = 0x1; break;
        case Opcode::SETB_RM8:   cond = 0x2; break;
        case Opcode::SETNB_RM8:  cond = 0x3; break;
        case Opcode::SETZ_RM8:   cond = 0x4; break;
        case Opcode::SETNZ_RM8:  cond = 0x5; break;
        case Opcode::SETBE_RM8:  cond = 0x6; break;
        case Opcode::SETNBE_RM8: cond = 0x7; break;
        case Opcode::SETS_RM8:   cond = 0x8; break;
        case Opcode::SETNS_RM8:  cond = 0x9; break;
        case Opcode::SETP_RM8:   cond = 0xA; break;
        case Opcode::SETNP_RM8:  cond = 0xB; break;
        case Opcode::SETL_RM8:   cond = 0xC; break;
        case Opcode::SETNL_RM8:  cond = 0xD; break;
        case Opcode::SETLE_RM8:  cond = 0xE; break;
        case Opcode::SETNLE_RM8: cond = 0xF; break;
        default: return false;
        }
        const int set_fac = fused_arm_cond(m_flags.incoming_nzcv, cond);
        emit_eval_jcc(cond, set_fac);
        bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // ---- CMOVcc r32, r/m32 ----
    case Opcode::CMOVO_R32_RM32:
    case Opcode::CMOVNO_R32_RM32:
    case Opcode::CMOVB_R32_RM32:
    case Opcode::CMOVNB_R32_RM32:
    case Opcode::CMOVZ_R32_RM32:
    case Opcode::CMOVNZ_R32_RM32:
    case Opcode::CMOVBE_R32_RM32:
    case Opcode::CMOVNBE_R32_RM32:
    case Opcode::CMOVS_R32_RM32:
    case Opcode::CMOVNS_R32_RM32:
    case Opcode::CMOVP_R32_RM32:
    case Opcode::CMOVNP_R32_RM32:
    case Opcode::CMOVL_R32_RM32:
    case Opcode::CMOVNL_R32_RM32:
    case Opcode::CMOVLE_R32_RM32:
    case Opcode::CMOVNLE_R32_RM32: {
        u8 cond;
        switch (instr.opcode) {
        case Opcode::CMOVO_R32_RM32:   cond = 0x0; break;
        case Opcode::CMOVNO_R32_RM32:  cond = 0x1; break;
        case Opcode::CMOVB_R32_RM32:   cond = 0x2; break;
        case Opcode::CMOVNB_R32_RM32:  cond = 0x3; break;
        case Opcode::CMOVZ_R32_RM32:   cond = 0x4; break;
        case Opcode::CMOVNZ_R32_RM32:  cond = 0x5; break;
        case Opcode::CMOVBE_R32_RM32:  cond = 0x6; break;
        case Opcode::CMOVNBE_R32_RM32: cond = 0x7; break;
        case Opcode::CMOVS_R32_RM32:   cond = 0x8; break;
        case Opcode::CMOVNS_R32_RM32:  cond = 0x9; break;
        case Opcode::CMOVP_R32_RM32:   cond = 0xA; break;
        case Opcode::CMOVNP_R32_RM32:  cond = 0xB; break;
        case Opcode::CMOVL_R32_RM32:   cond = 0xC; break;
        case Opcode::CMOVNL_R32_RM32:  cond = 0xD; break;
        case Opcode::CMOVLE_R32_RM32:  cond = 0xE; break;
        case Opcode::CMOVNLE_R32_RM32: cond = 0xF; break;
        default: return false;
        }
        // Intel SDM Vol. 2 CMOVcc: dest := src if cc, else dest. EFLAGS
        // unchanged. ARM CSEL does not write NZCV, so a fused CMP's host
        // flags stay live — do not SUBS/ANDS after that CMP.
        const int fac32 = fused_arm_cond(m_flags.incoming_nzcv, cond);
        const bool src_is_reg = is_register_operand(op1.type);
        if (fac32 >= 0 && src_is_reg) {
            const u8 dpin = (m_pins_live && op0.reg < 16) ? m_pin_arm[op0.reg]
                                                        : 0xFF;
            const u8 spin = (m_pins_live && op1.reg < 16) ? m_pin_arm[op1.reg]
                                                        : 0xFF;
            const u8 d = (dpin != 0xFF) ? dpin : REG_SCRATCH0;
            const u8 s = (spin != 0xFF) ? spin : REG_SCRATCH1;
            if (dpin == 0xFF) emit_load_gpr32(d, op0.reg);
            if (spin == 0xFF) emit_load_gpr32(s, op1.reg);
            emit(arm64::csel_w(d, s, d, static_cast<u8>(fac32)));
            if (dpin == 0xFF) emit_store_gpr32(d, op0.reg);
            // CSEL does not write NZCV. Keep the predecessor kind so a
            // later Jcc in this block can still fuse (Intel SDM Vol. 2
            // CMOVcc: Flags Affected = None).
            if (m_flags.incoming_nzcv != NzcvState::None) {
                m_flags.nzcv = m_flags.incoming_nzcv;
            }
            break;
        }
        if (fac32 >= 0) {
            // EA / load can clobber NZCV. CSET first, then CSEL-from-Z.
            emit(arm64::cset_w(REG_CALL_TMP, static_cast<u8>(fac32)));
        }
        if (src_is_reg) {
            emit_load_gpr32(10, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_w(10, REG_SCRATCH0));
        }
        if (fac32 >= 0) {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
            emit(arm64::cmp_w_imm(REG_CALL_TMP, 0));
            emit(arm64::csel_w(REG_SCRATCH0, 10, REG_SCRATCH0, arm64::NE));
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
            break;
        }
        // Not-taken must still write the destination: Intel SDM Vol. 2
        // CMOVcc Operation zeroes DEST[63:32] when the operand size is 32 and
        // IA-32e mode is active, condition false included. A branch that skips
        // the store leaves the old upper half, which is what the interpreter's
        // `else if (ctx.cpu.is_64bit())` arm exists to prevent. Select instead
        // of branching -- one word more than the CBZ form, branchless, and
        // correct in both bitnesses (a 32-bit block re-storing the unchanged
        // value is a no-op).
        emit(arm64::mov_w(REG_CALL_TMP, 10));
        emit_eval_lazy_condition(cond);
        emit_load_gpr32(REG_SCRATCH1, op0.reg);
        emit(arm64::cmp_w_imm(REG_SCRATCH0, 0));
        emit(arm64::csel_w(REG_SCRATCH1, REG_CALL_TMP, REG_SCRATCH1,
                           arm64::NE));
        emit_store_gpr32(REG_SCRATCH1, op0.reg);
        break;
    }

    // ---- CMOVcc r16, r/m16 ----
    // Intel SDM Vol. 2 CMOVcc: 0F 40-4F /r with 66h. Moves only the low 16
    // bits; emit_store_gpr16 preserves dest GPR[31:16].
    case Opcode::CMOVO_R16_RM16:
    case Opcode::CMOVNO_R16_RM16:
    case Opcode::CMOVB_R16_RM16:
    case Opcode::CMOVNB_R16_RM16:
    case Opcode::CMOVZ_R16_RM16:
    case Opcode::CMOVNZ_R16_RM16:
    case Opcode::CMOVBE_R16_RM16:
    case Opcode::CMOVNBE_R16_RM16:
    case Opcode::CMOVS_R16_RM16:
    case Opcode::CMOVNS_R16_RM16:
    case Opcode::CMOVP_R16_RM16:
    case Opcode::CMOVNP_R16_RM16:
    case Opcode::CMOVL_R16_RM16:
    case Opcode::CMOVNL_R16_RM16:
    case Opcode::CMOVLE_R16_RM16:
    case Opcode::CMOVNLE_R16_RM16: {
        u8 cond;
        switch (instr.opcode) {
        case Opcode::CMOVO_R16_RM16:   cond = 0x0; break;
        case Opcode::CMOVNO_R16_RM16:  cond = 0x1; break;
        case Opcode::CMOVB_R16_RM16:   cond = 0x2; break;
        case Opcode::CMOVNB_R16_RM16:  cond = 0x3; break;
        case Opcode::CMOVZ_R16_RM16:   cond = 0x4; break;
        case Opcode::CMOVNZ_R16_RM16:  cond = 0x5; break;
        case Opcode::CMOVBE_R16_RM16:  cond = 0x6; break;
        case Opcode::CMOVNBE_R16_RM16: cond = 0x7; break;
        case Opcode::CMOVS_R16_RM16:   cond = 0x8; break;
        case Opcode::CMOVNS_R16_RM16:  cond = 0x9; break;
        case Opcode::CMOVP_R16_RM16:   cond = 0xA; break;
        case Opcode::CMOVNP_R16_RM16:  cond = 0xB; break;
        case Opcode::CMOVL_R16_RM16:   cond = 0xC; break;
        case Opcode::CMOVNL_R16_RM16:  cond = 0xD; break;
        case Opcode::CMOVLE_R16_RM16:  cond = 0xE; break;
        case Opcode::CMOVNLE_R16_RM16: cond = 0xF; break;
        default: return false;
        }
        // Same CSEL fusion as r32: live NZCV for reg src; CSET-then-CSEL-from-Z
        // for mem src (EA / load can clobber NZCV). CMOVcc does not write
        // EFLAGS; CSEL does not write NZCV.
        const int fac16 = fused_arm_cond(m_flags.incoming_nzcv, cond);
        const bool src16_is_reg = is_register_operand(op1.type);
        if (fac16 >= 0 && src16_is_reg) {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit(arm64::csel_w(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH0,
                               static_cast<u8>(fac16)));
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
            if (m_flags.incoming_nzcv != NzcvState::None) {
                m_flags.nzcv = m_flags.incoming_nzcv;
            }
            break;
        }
        if (fac16 >= 0) {
            emit(arm64::cset_w(REG_CALL_TMP, static_cast<u8>(fac16)));
        }
        if (src16_is_reg) {
            emit_load_gpr16(10, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_w(10, REG_SCRATCH0));
        }
        if (fac16 >= 0) {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            emit(arm64::cmp_w_imm(REG_CALL_TMP, 0));
            emit(arm64::csel_w(REG_SCRATCH0, 10, REG_SCRATCH0, arm64::NE));
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
            break;
        }
        emit(arm64::mov_w(REG_CALL_TMP, 10));
        emit_eval_lazy_condition(cond);
        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit_store_gpr16(REG_CALL_TMP, op0.reg);
        usize skip_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH0,
            static_cast<i32>(skip_pos - cbz_pos) * 4);
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
