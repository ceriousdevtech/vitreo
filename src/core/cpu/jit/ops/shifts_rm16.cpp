// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — SHL / SHR / SAR r/m16 (imm8 / 1 / CL).
//
// One family of compile_shifts' opcode space, split out of ops/shifts.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_shifts (ops/shifts.cpp) owns the dispatch chain and the shared
// is_last tail; see the declaration in jit_compiler_lowering.hpp for the
// nullopt / true contract.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_shifts_rm16(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- SHR r/m16, imm8 / 1 / CL ----
    // Intel SDM Vol. 2: COUNT masked with 1Fh for 16-bit operand size.
    // FlagOp::Shr16 records original+count so CF/OF materialize correctly.
    case Opcode::SHR_RM16_IMM8:
    case Opcode::SHR_RM16_1: {
        u8 shift = (instr.opcode == Opcode::SHR_RM16_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(shift));
        emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));
        emit_set_shift_lazy_flags(CpuState::FlagOp::Shr16,
                                  REG_SCRATCH4, REG_SCRATCH1,
                                  REG_SCRATCH0);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }
    case Opcode::SHR_RM16_CL: {
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #31
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_shift_lazy_flags(CpuState::FlagOp::Shr16,
                                  REG_SCRATCH4, REG_SCRATCH1,
                                  REG_SCRATCH0);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }
        break;
    }

    // ---- SHL r/m16, imm8 / 1 / CL ----
    // FlagOp::Shl16 only materializes ZSP (CF forced false). Match interpreter
    // CF/OF via overlays (Intel SDM Vol. 2; COUNT & 1Fh).
    case Opcode::SHL_RM16_IMM8:
    case Opcode::SHL_RM16_1: {
        u8 shift = (instr.opcode == Opcode::SHL_RM16_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit_mov_imm32(REG_SCRATCH1, shift);
        emit(arm64::lsl_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        // One convention for the whole SHL family (see the r/m8 form).
        emit_set_shift_lazy_flags(CpuState::FlagOp::Shl16,
                                  REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH0);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }
    case Opcode::SHL_RM16_CL: {
        bool is_mem = !is_register_operand(op0.type);
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #31
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0); // CBZ W8, skip
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        // Re-derive the masked count: in a 64-bit block the guest load above
        // goes through emit_x64_region_load / emit_guest_load16_64, which use
        // W8 as scratch on the INLINE path. The 32-bit linear path never
        // touched it, so this only breaks for a memory destination in an x64
        // block -- and it breaks silently, computing CF/OF from garbage while
        // the result value stays correct.
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #31
        emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_shift_lazy_flags(CpuState::FlagOp::Shl16,
                                  REG_SCRATCH4, REG_SCRATCH1,
                                  REG_SCRATCH0);
        // CF: count>16 → 0; else (orig >> (16-count)) & 1
        emit(arm64::cmp_w_imm(8, 16));
        const usize cf_hi = m_code_buf.size();
        emit(0u); // B.HI cf_zero
        emit(arm64::movz_w(REG_SCRATCH2, 16));
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, 8)); // 16 - count
        emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH2));
        emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1));
        const usize cf_join = m_code_buf.size();
        emit(0u); // B done
        const usize cf_zero = m_code_buf.size();
        m_code_buf[cf_hi] = arm64::b_cond(
            arm64::HI, static_cast<i32>(cf_zero - cf_hi) * 4);
        emit(arm64::movz_w(REG_SCRATCH2, 0));
        const usize cf_done = m_code_buf.size();
        m_code_buf[cf_join] = arm64::b_imm(
            static_cast<i32>(cf_done - cf_join) * 4);
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(0x8A210063u);
            emit(0xAA020063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH2, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));
            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);
            emit(0x530F3C03u); // UBFX W3, W0, #15, #1
            emit(0x4A020063u); // EOR W3, W3, W2
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH3, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH4, 0x800));
            emit(0x8A240021u);
            emit(0x53155063u);
            emit(0xAA030021u);
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, rf_off / 8));
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
        }
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }
        break;
    }

    // ---- SAR r/m16, imm8 / 1 / CL ----
    case Opcode::SAR_RM16_IMM8:
    case Opcode::SAR_RM16_1: {
        u8 shift = (instr.opcode == Opcode::SAR_RM16_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;
        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit_mov_imm32(REG_SCRATCH1, shift);
        emit(arm64::sxth_x(REG_SCRATCH0, REG_SCRATCH0));
        if (shift < 32) {
            emit(arm64::asr_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));
        }
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        // One convention for the whole SAR family (see the SHL forms): the
        // record carries the original and the count, so CF -- the sign bit
        // once the count reaches the operand width -- no longer needs an
        // overlay computed here.
        emit_set_shift_lazy_flags(CpuState::FlagOp::Sar16,
                                  REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH0);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }
    case Opcode::SAR_RM16_CL: {
        bool is_mem = !is_register_operand(op0.type);
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(RegIndex::RCX));
        // Re-derive the masked count: in a 64-bit block the guest load above
        // goes through emit_x64_region_load / emit_guest_load16_64, which use
        // W8 as scratch on the INLINE path. The 32-bit linear path never
        // touched it, so this only breaks for a memory destination in an x64
        // block -- and it breaks silently, computing CF/OF from garbage while
        // the result value stays correct.
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #31
        emit(arm64::sxth_x(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::asrv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_shift_lazy_flags(CpuState::FlagOp::Sar16,
                                  REG_SCRATCH4, REG_SCRATCH1,
                                  REG_SCRATCH0);
        emit(arm64::cmp_w_imm(8, 16));
        const usize cf_hs = m_code_buf.size();
        emit(0u); // B.HS use_sign
        emit(arm64::movz_w(REG_SCRATCH2, 1));
        emit(arm64::sub_w(REG_SCRATCH2, 8, REG_SCRATCH2)); // count - 1
        emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH2));
        emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1));
        const usize cf_join = m_code_buf.size();
        emit(0u);
        const usize cf_sign = m_code_buf.size();
        m_code_buf[cf_hs] = arm64::b_cond(
            arm64::CS, static_cast<i32>(cf_sign - cf_hs) * 4);
        emit(0x530F3C82u); // UBFX W2, W4, #15, #1 (orig in W4)
        const usize cf_done = m_code_buf.size();
        m_code_buf[cf_join] = arm64::b_imm(
            static_cast<i32>(cf_done - cf_join) * 4);
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(0x8A210063u);
            emit(0xAA020063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH2, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));
            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);
            emit(arm64::movz_w(REG_SCRATCH1, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(REG_SCRATCH1, 0x800));
            emit(0x8A210063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
        }
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
