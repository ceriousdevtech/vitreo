// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: unary read-modify-write ops.
// INC/DEC r/m8, INC/DEC/NEG/NOT r/m64, NOT r/m32 and INC/DEC/NEG r/m32.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
// Launch-time diagnostic switches, not guest environment variables. Re-reading
// environ while compiling every ALU instruction serializes compiler threads
// on libc's environment lock. Preserve the switches without that hot-path work.
const bool helper_capcf = std::getenv("VITREO_JIT_HELPER_CAPCF") != nullptr;
const bool no_unary32 = std::getenv("VITREO_JIT_64BIT_NO_UNARY32") != nullptr;
const bool no_inc32 = std::getenv("VITREO_JIT_64BIT_NO_INC32") != nullptr;
const bool no_dec32 = std::getenv("VITREO_JIT_64BIT_NO_DEC32") != nullptr;
const bool no_neg32 = std::getenv("VITREO_JIT_64BIT_NO_NEG32") != nullptr;
}  // namespace

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_unary64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // Unary RMW: INC / DEC / NEG / NOT
    // ========================================================================

    case Opcode::INC_RM8:
    case Opcode::DEC_RM8: {
        const bool increment = instr.opcode == Opcode::INC_RM8;
        const bool has_rex = instr.prefixes.has_rex();
        const bool dst_is_mem = is_mem(op0);

        // INC/DEC preserve CF. Capture the incoming lazy carry before the
        // byte RMW replaces the previous flag producer; X21 survives checked
        // guest-memory helper calls.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_CAPTURE_CF >> 3));
        emit_helper_blr();  // BLR X16
        emit(arm64::mov_x(REG_CALL_TMP, 0));

        if (dst_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, has_rex);
        }
        emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
        if (increment) {
            emit(arm64::add_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        } else {
            emit_mov_imm32(REG_SCRATCH3, 1);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
        }
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags_preserve_cf(
            increment ? CpuState::FlagOp::Inc8 : CpuState::FlagOp::Dec8,
            REG_SCRATCH1, REG_SCRATCH0, REG_CALL_TMP);

        if (dst_is_mem) {
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store8(17, REG_SCRATCH4);
        } else {
            // Pin-aware: see the store_reg8 note above.
            emit_store_gpr8(REG_SCRATCH0, op0.reg, has_rex);
        }
        break;
    }

    case Opcode::INC_RM64: {
        // INC preserves the incoming CF. Capture it before replacing the
        // prior lazy-flag producer; X21 survives any checked memory helper.
        //
        // Inline, not the capture_cf helper: emit_lazy_cf_to_call_tmp is the
        // same computation (overlay first, then live NZCV, then the known
        // 32-bit FlagOp formulas, helper only for the uncommon states), and
        // the helper cost -- BLR plus an 8-register pin spill and refill on
        // EVERY execution -- was most of what made a loop-tail DEC cost 33
        // emitted instructions.
        // A record within-block liveness proved dead (flag_liveness.cpp,
        // the INC/DEC rule) is not published, so the carry it would have
        // preserved is not captured either: the capture READS the prior
        // producer's record, which the same analysis may have elided.
        const bool publish = !m_omit_lazy_current;
        if (publish && helper_capcf) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_CAPTURE_CF >> 3));
            emit_helper_blr();
            emit(arm64::mov_x(REG_CALL_TMP, 0));
        } else if (publish) {
            emit_lazy_cf_to_call_tmp();
        }
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        if (publish) {
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_CALL_TMP, 32));
            emit_set_lazy_flags64(CpuState::FlagOp::Inc64,
                                  REG_SCRATCH1, REG_SCRATCH0);
        }
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::DEC_RM64: {
        // DEC has the same partial-flag contract as INC: preserve CF while
        // replacing OF/SF/ZF/AF/PF with the result-derived lazy state.
        // Inline capture; see INC_RM64 above.
        // A record within-block liveness proved dead (flag_liveness.cpp,
        // the INC/DEC rule) is not published, so the carry it would have
        // preserved is not captured either: the capture READS the prior
        // producer's record, which the same analysis may have elided.
        const bool publish = !m_omit_lazy_current;
        if (publish && helper_capcf) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_CAPTURE_CF >> 3));
            emit_helper_blr();
            emit(arm64::mov_x(REG_CALL_TMP, 0));
        } else if (publish) {
            emit_lazy_cf_to_call_tmp();
        }
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        if (publish) {
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_CALL_TMP, 32));
            emit_set_lazy_flags64(CpuState::FlagOp::Dec64,
                                  REG_SCRATCH1, REG_SCRATCH0);
        }
        if (m_cfg.forward_join_state_enabled) {
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::NEG_RM64: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));  // save original
        emit(arm64::negs_x(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags64(CpuState::FlagOp::Neg64,
                              REG_SCRATCH1, REG_SCRATCH0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::NOT_RM64: {
        // NOT does not affect flags.
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mvn_x(REG_SCRATCH0, REG_SCRATCH0));
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // NOT r/m32 — architectural 32-bit write zero-extends into the full GPR.
    // No EFLAGS effect. Intel SDM Vol. 2 NOT.
    case Opcode::NOT_RM32: {
        const bool operand_is_mem = is_mem(op0);
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mvn_w(REG_SCRATCH0, REG_SCRATCH0));
        if (operand_is_mem) {
            // Load helpers clobber the address register — recompute EA.
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // INC/DEC/NEG r/m32 inside long-mode blocks. The operand result is a
    // 32-bit architectural write and therefore clears the destination GPR's
    // upper half. Memory addresses remain full 64-bit. INC/DEC preserve the
    // incoming CF, including when it is still represented by lazy flags.
    case Opcode::INC_RM32:
    case Opcode::DEC_RM32:
    case Opcode::NEG_RM32: {
        if (no_unary32) return false;
        const bool is_inc = instr.opcode == Opcode::INC_RM32;
        const bool is_dec = instr.opcode == Opcode::DEC_RM32;
        if (is_inc && no_inc32)
            return false;
        if (is_dec && no_dec32)
            return false;
        if (!is_inc && !is_dec &&
            no_neg32)
            return false;
        const bool operand_is_mem = is_mem(op0);

        // An INC/DEC record proved dead is neither published nor fed: see
        // INC_RM64 above.
        const bool publish = !m_omit_lazy_current;
        if ((is_inc || is_dec) && publish) {
            if (helper_capcf) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        JIT_CTX_OFF_CAPTURE_CF >> 3));
                emit_helper_blr();
                emit(arm64::mov_w(REG_CALL_TMP, 0));
            } else {
                emit_lazy_cf_to_call_tmp();
            }
        }

        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }

        emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
        if (is_inc) {
            emit(arm64::add_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
            if (publish) {
                emit_set_lazy_flags_preserve_cf(
                    CpuState::FlagOp::Inc32, REG_SCRATCH1, REG_SCRATCH0,
                    REG_CALL_TMP);
            }
        } else if (is_dec) {
            emit_mov_imm32(REG_SCRATCH3, 1);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3));
            if (publish) {
                emit_set_lazy_flags_preserve_cf(
                    CpuState::FlagOp::Dec32, REG_SCRATCH1, REG_SCRATCH0,
                    REG_CALL_TMP);
            }
        } else {
            emit(arm64::sub_w(REG_SCRATCH0, 31, REG_SCRATCH0));
            emit_set_lazy_flags(CpuState::FlagOp::Neg32,
                                REG_SCRATCH1, REG_SCRATCH0);
        }

        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
