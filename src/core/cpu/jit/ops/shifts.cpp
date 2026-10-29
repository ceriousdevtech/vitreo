// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Shift and rotate opcodes (SHL, SHR, SAR, ROR, ROL, RCL, RCR,
// SHLD, SHRD)
//
// compile_shifts owns the dispatch chain and the shared is_last tail. The
// 32-bit SHL/SHR/SAR forms (with the EMIT_SHIFT_IMM / EMIT_SHIFT_CL macros
// only they use) are lowered here; every other shift/rotate family lives in
// ops/shifts_<family>.cpp (ADR-0006 step 3) and is reached from `default:`.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

// ============================================================================
// EMIT_SHIFT_IMM — immediate shift (SHL/SHR/SAR)
// ============================================================================
#define EMIT_SHIFT_IMM(shift_emit, shift_count_expr, flag_op)                 \
    do {                                                                       \
        bool is_mem = !is_register_operand(op0.type);                          \
        const bool pe32_pin_dst = !is_mem                                      \
            && m_pins_live && op0.reg < 16 && m_pin_arm[op0.reg] != 0xFF;     \
        const u8 shift_dst = pe32_pin_dst ? m_pin_arm[op0.reg]                \
                                           : REG_SCRATCH0;                    \
        if (is_mem) {                                                          \
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);                  \
            emit_guest_load32(shift_dst, REG_SCRATCH2);                        \
        } else {                                                               \
            emit_load_gpr32(shift_dst, op0.reg);                               \
        }                                                                      \
        emit(arm64::mov_w(REG_SCRATCH4, shift_dst));                           \
        emit_mov_imm32(REG_SCRATCH1, shift_count_expr);                        \
        shift_emit;                                                            \
        emit_set_shift_lazy_flags(flag_op, REG_SCRATCH4,                       \
                                  REG_SCRATCH1, shift_dst);                    \
        if (is_mem) {                                                          \
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);                  \
            emit_guest_store32(shift_dst, REG_SCRATCH2);                       \
        } else {                                                               \
            emit_store_gpr32(shift_dst, op0.reg);                              \
        }                                                                      \
    } while (0)

// ============================================================================
// EMIT_SHIFT_CL — shift by CL register (with CL==0 skip)
// ============================================================================
#define EMIT_SHIFT_CL(shiftv_emit, flag_op)                                   \
    do {                                                                       \
        bool is_mem = !is_register_operand(op0.type);                          \
        const bool pe32_pin_dst = !is_mem                                      \
            && m_pins_live && op0.reg < 16 && m_pin_arm[op0.reg] != 0xFF;     \
        const u8 shift_dst = pe32_pin_dst ? m_pin_arm[op0.reg]                \
                                           : REG_SCRATCH0;                    \
        if (is_mem) {                                                          \
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);                  \
            emit_guest_load32(shift_dst, REG_SCRATCH2);                        \
        } else {                                                               \
            emit_load_gpr32(shift_dst, op0.reg);                               \
        }                                                                      \
        emit_load_gpr32(REG_SCRATCH1,                                          \
                         static_cast<u8>(RegIndex::RCX));                      \
        /* AND W8, W1, #31 */                                                  \
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));                         \
        /* CBZ W8, skip_label (placeholder) */                                 \
        u32 cl_cbz_idx = static_cast<u32>(m_code_buf.size());                  \
        emit(0);                                                               \
        emit(arm64::mov_w(REG_SCRATCH4, shift_dst));                           \
        shiftv_emit;                                                           \
        emit_set_shift_lazy_flags(flag_op, REG_SCRATCH4,                       \
                                  REG_SCRATCH1, shift_dst);                    \
        if (is_mem) {                                                          \
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);                  \
            emit_guest_store32(shift_dst, REG_SCRATCH2);                       \
        } else {                                                               \
            emit_store_gpr32(shift_dst, op0.reg);                              \
        }                                                                      \
        {                                                                      \
            u32 skip_target = static_cast<u32>(m_code_buf.size());             \
            i32 off_bytes = static_cast<i32>(skip_target - cl_cbz_idx) * 4;    \
            m_code_buf[cl_cbz_idx] = arm64::cbz_w(8, off_bytes);              \
        }                                                                      \
    } while (0)

bool JitCompiler::compile_shifts(const DecodedInstruction& instr,
                                  u64 instr_rip, u64 next_rip,
                                  bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- SHL ----
    case Opcode::SHL_RM32_IMM8:
    case Opcode::SHL_RM32_1: {
        u8 shift = (instr.opcode == Opcode::SHL_RM32_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;
        EMIT_SHIFT_IMM(
            emit(arm64::lsl_w_imm(shift_dst, shift_dst, shift)),
            static_cast<u32>(shift),
            CpuState::FlagOp::Shl32);
        break;
    }
    case Opcode::SHL_RM32_CL: {
        EMIT_SHIFT_CL(
            emit(arm64::lslv_w(shift_dst, shift_dst, REG_SCRATCH1)),
            CpuState::FlagOp::Shl32);
        break;
    }

    // ---- SHR ----
    case Opcode::SHR_RM32_IMM8:
    case Opcode::SHR_RM32_1: {
        u8 shift = (instr.opcode == Opcode::SHR_RM32_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;
        EMIT_SHIFT_IMM(
            emit(arm64::lsr_w_imm(shift_dst, shift_dst, shift)),
            static_cast<u32>(shift),
            CpuState::FlagOp::Shr32);
        break;
    }
    case Opcode::SHR_RM32_CL: {
        EMIT_SHIFT_CL(
            emit(arm64::lsrv_w(shift_dst, shift_dst, REG_SCRATCH1)),
            CpuState::FlagOp::Shr32);
        break;
    }

    // ---- SAR ----
    case Opcode::SAR_RM32_IMM8:
    case Opcode::SAR_RM32_1: {
        u8 shift = (instr.opcode == Opcode::SAR_RM32_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;
        EMIT_SHIFT_IMM(
            emit(arm64::asr_w_imm(shift_dst, shift_dst, shift)),
            static_cast<u32>(shift),
            CpuState::FlagOp::Sar32);
        break;
    }
    case Opcode::SAR_RM32_CL: {
        EMIT_SHIFT_CL(
            emit(arm64::asrv_w(shift_dst, shift_dst, REG_SCRATCH1)),
            CpuState::FlagOp::Sar32);
        break;
    }

    default: {
        // The shift/rotate families, one per ops/shifts_<family>.cpp
        // (ADR-0006 step 3). A family returns nullopt for an opcode outside
        // it and true when its arm reached break; the shared tail below then
        // runs exactly as after the old single switch. Case labels are
        // disjoint across families, so chain order is not observable.
        std::optional<bool> lowered = compile_shifts_shld_shrd(instr, next_rip);
        if (!lowered) lowered = compile_shifts_rotate_rm32(instr, next_rip);
        if (!lowered) lowered = compile_shifts_rm8(instr, next_rip);
        if (!lowered) lowered = compile_shifts_rm16(instr, next_rip);
        if (!lowered) lowered = compile_shifts_rotate_narrow(instr, next_rip);
        if (!lowered) lowered = compile_shifts_rcl_rcr_rm32(instr, next_rip);
        if (!lowered) lowered = compile_shifts_rcl_rcr_narrow(instr, next_rip);
        if (!lowered.has_value() || !*lowered) return false;
        break;
    }
    }

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

#undef EMIT_SHIFT_IMM
#undef EMIT_SHIFT_CL

} // namespace vitreo::cpu::jit
