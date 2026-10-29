// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Multiply/divide opcodes (MUL, IMUL, IDIV, DIV)

#include "../jit_helpers.hpp"
#include "../../seh/seh32.hpp"

namespace vitreo::cpu::jit {

// Intel SDM Vol. 2A, DIV: a zero divisor or a quotient too large for the
// destination raises #DE without committing EAX/EDX. The dispatcher consumes
// this pending Windows exception exactly like an interpreter-raised #DE.
extern "C" void jit_raise_divide_error(CpuState* cpu, u64 instr_rip) {
    if (!cpu || cpu->pending_raise.active) return;
    cpu->rip = instr_rip;
    auto& pending = cpu->pending_raise;
    pending.active = true;
    pending.from_helper = true;
    pending.code = STATUS_INTEGER_DIVIDE_BY_ZERO;
    pending.flags = 0;
    pending.num_args = 0;
}

bool JitCompiler::compile_mul_div(const DecodedInstruction& instr,
                                   u64 instr_rip, u64 next_rip,
                                   bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- IMUL r16, r/m16 ----
    // Intel SDM Vol. 2A, IMUL: multiply signed 16-bit operands, store the low
    // 16 bits in the destination, and define CF/OF from whether that truncated
    // result sign-extends back to the full product. Keep the full product in
    // lazy flag state so flag consumers observe the same contract without an
    // interpreter-delegate call in scalar fixed-point loops.
    case Opcode::IMUL_R16_RM16: {
        // Load the SOURCE before the destination, matching IMUL_R32_RM32 and
        // IMUL_R64_RM64 below. This arm used to stage the destination in W0
        // ahead of the branch, and emit_guest_load16 reaches a helper call on
        // the non-linear region path, which clobbers W0 — so `imul ax, [ebx]`
        // multiplied by zero. Masked in production because 32-bit guests
        // default to the linear path, where the load is an inline LDRH; found
        // by the ISA matrix sweep. Same defect as the ADC/SBB r8 memory form.
        if (is_register_operand(op1.type)) {
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::sxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::sxth_w(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH0, op0.reg);
        {
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul16));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                  flag_op_offset()));
            emit_clear_cf_overlay();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   flag_src_offset() >> 3));
        }
        break;
    }

    // ---- IMUL r16, r/m16, imm8 / imm16 ----
    case Opcode::IMUL_R16_RM16_IMM8:
    case Opcode::IMUL_R16_RM16_IMM16: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit(arm64::sxth_w(REG_SCRATCH0, REG_SCRATCH0));
        const i32 immediate = instr.opcode == Opcode::IMUL_R16_RM16_IMM8
            ? static_cast<i8>(static_cast<u8>(instr.operands[2].imm))
            : static_cast<i16>(static_cast<u16>(instr.operands[2].imm));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(immediate));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH0, op0.reg);
        {
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul16));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                  flag_op_offset()));
            emit_clear_cf_overlay();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   flag_src_offset() >> 3));
        }
        break;
    }

    // ---- MUL r/m8 — unsigned AL * r/m8 → AX ----
    // Intel SDM Vol. 2A, MUL r/m8: AX := AL * r/m8; CF=OF=(AH != 0).
    case Opcode::MUL_RM8: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr8(REG_SCRATCH1, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::uxtb_w(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::umull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // High byte of AX → flag_dst for FlagOp::Mul32 (CF/OF := high != 0).
        emit(0xD348FC00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                         | static_cast<u32>(REG_SCRATCH1)); // LSR X1, X0, #8
        emit(arm64::uxtb_w(REG_SCRATCH1, REG_SCRATCH1));
        emit_set_lazy_flags(CpuState::FlagOp::Mul32,
                            REG_SCRATCH0, REG_SCRATCH1);
        break;
    }

    // ---- MUL r/m16 — unsigned AX * r/m16 → DX:AX ----
    // Intel SDM Vol. 2A, MUL r/m16: DX:AX := AX * r/m16; CF=OF=(DX != 0).
    case Opcode::MUL_RM16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::umull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // High half of product → DX and flag_dst (CF/OF := high != 0).
        emit(0xD350FC00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                         | static_cast<u32>(REG_SCRATCH1)); // LSR X1, X0, #16
        emit(arm64::uxth_w(REG_SCRATCH1, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        emit_set_lazy_flags(CpuState::FlagOp::Mul32,
                            REG_SCRATCH0, REG_SCRATCH1);
        break;
    }

    // ---- MUL r/m32 — unsigned EAX * r/m32 → EDX:EAX ----
    case Opcode::MUL_RM32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr32(REG_SCRATCH0,
                         static_cast<u8>(RegIndex::RAX));
        emit(arm64::umull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH0,
                          static_cast<u8>(RegIndex::RAX));
        emit(0xD360FC00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                         | static_cast<u32>(REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH1,
                          static_cast<u8>(RegIndex::RDX));
        emit_set_lazy_flags(CpuState::FlagOp::Mul32,
                            REG_SCRATCH0, REG_SCRATCH1);
        break;
    }

    // ---- IMUL r32, r/m32 ----
    case Opcode::IMUL_R32_RM32: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                    src_off >> 3));
        }
        break;
    }

    // ---- IMUL r32, r/m32, imm32 / imm8 ----
    case Opcode::IMUL_R32_RM32_IMM32:
    case Opcode::IMUL_R32_RM32_IMM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_mov_imm32(REG_SCRATCH1,
                        static_cast<u32>(instr.operands[2].imm));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                    src_off >> 3));
        }
        break;
    }

    // ---- IMUL r/m8 — signed AL × r/m8 → AX (single-operand) ----
    // Intel SDM Vol. 2A, IMUL r/m8: CF/OF when AX does not equal sign-extended AL.
    case Opcode::IMUL_RM8: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr8(REG_SCRATCH1, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::sxtb_x(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::sxtb_x(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        {
            emit(arm64::sxth_x(REG_SCRATCH0, REG_SCRATCH0));
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul8));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                    src_off >> 3));
        }
        break;
    }

    // ---- IMUL r/m16 — signed AX × r/m16 → DX:AX (single-operand) ----
    // Intel SDM Vol. 2A, IMUL r/m16: CF/OF when DX:AX does not equal
    // sign-extended AX. Keep the full 32-bit product in FlagOp::Imul16.
    case Opcode::IMUL_RM16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::sxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::sxth_w(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(0xD350FC00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                         | static_cast<u32>(REG_SCRATCH1)); // LSR X1, X0, #16
        emit_store_gpr16(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        {
            // Sign-extend the 32-bit product so FlagOp::Imul16 sees the full
            // signed value (matches two-operand IMUL r16 flag contract).
            emit(arm64::sxtw_x(REG_SCRATCH0, REG_SCRATCH0));
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul16));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                    src_off >> 3));
        }
        break;
    }

    // ---- IMUL r/m32 — signed EAX × r/m32 → EDX:EAX (single-operand) ----
    case Opcode::IMUL_RM32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH1, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::smull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(0xD360FC00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                         | static_cast<u32>(REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::Imul32));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
            u32 src_off = flag_src_offset();
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                    src_off >> 3));
            u32 dst_off = flag_dst_offset();
            emit(0x2A0003E0u
                 | (static_cast<u32>(REG_SCRATCH1) << 16)
                 | static_cast<u32>(REG_SCRATCH3));
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    dst_off >> 3));
        }
        break;
    }

    // ---- DIV r/m8 — unsigned AX / r/m8 → AL remainder AH ----
    // Intel SDM Vol. 2A, DIV r/m8. Quotient must fit in AL; else #DE.
    case Opcode::DIV_RM8: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr8(REG_SCRATCH1, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH3);
        }
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::uxtb_w(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));

        const usize zero_branch = m_code_buf.size();
        emit(0); // CBZ Wdivisor, divide_error
        emit(arm64::udiv_x(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::subs_w(31, REG_SCRATCH4, REG_SCRATCH2));
        const usize overflow_branch = m_code_buf.size();
        emit(0); // B.NE divide_error (quotient > 0xFF)

        emit(arm64::msub_x(
            REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH0));
        // AX = (AH=remainder << 8) | (AL=quotient)
        emit(arm64::orr_x_lsl(
            REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH2, 8));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr(); // BLR X16
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH1,
            static_cast<i32>(divide_error - zero_branch) * 4);
        m_code_buf[overflow_branch] = arm64::b_cond(
            arm64::NE,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    // ---- DIV r/m16 — unsigned DX:AX / r/m16 → AX remainder DX ----
    // Intel SDM Vol. 2A, DIV. Quotient fits in 16 bits iff DX < divisor.
    // emit_store_gpr16 clobbers REG_SCRATCH4, so move the quotient aside
    // before committing AX/DX.
    case Opcode::DIV_RM16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH1, op0.reg); // divisor
        } else {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH3);
        }
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr16(REG_SCRATCH2, static_cast<u8>(RegIndex::RDX));

        const usize zero_branch = m_code_buf.size();
        emit(0); // CBZ Wdivisor, divide_error
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH1));
        const usize overflow_branch = m_code_buf.size();
        emit(0); // B.HS divide_error (DX >= divisor)

        // X3 = zero_extend(AX) | (zero_extend(DX) << 16)
        emit(arm64::orr_x_lsl(
            REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2, 16));
        emit(arm64::udiv_x(
            REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH1));
        emit(arm64::msub_x(
            REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH3));
        emit(arm64::mov_x(REG_SCRATCH2, REG_SCRATCH4)); // quot off SCRATCH4
        emit_store_gpr16(REG_SCRATCH2, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RDX));
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr(); // BLR X16
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH1,
            static_cast<i32>(divide_error - zero_branch) * 4);
        m_code_buf[overflow_branch] = arm64::b_cond(
            arm64::CS,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    // ---- DIV r/m32 — unsigned EDX:EAX / r/m32 -> EAX remainder EDX ----
    // Intel SDM Vol. 2A, DIV. For an unsigned double-width dividend, the
    // quotient fits in 32 bits iff EDX < divisor. Check that and zero before
    // issuing ARM UDIV, because AArch64 division itself does not raise #DE.
    case Opcode::DIV_RM32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH1, op0.reg); // divisor
        } else {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH3);
        }
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr32(REG_SCRATCH2, static_cast<u8>(RegIndex::RDX));

        const usize zero_branch = m_code_buf.size();
        emit(0); // CBZ Wdivisor, divide_error
        emit(arm64::subs_w(31, REG_SCRATCH2, REG_SCRATCH1));
        const usize overflow_branch = m_code_buf.size();
        emit(0); // B.HS divide_error (EDX >= divisor)

        // X3 = zero_extend(EAX) | (zero_extend(EDX) << 32)
        emit(arm64::orr_x_lsl(
            REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2, 32));
        emit(arm64::udiv_x(
            REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH1));
        emit(arm64::msub_x(
            REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH3));
        emit_store_gpr32(REG_SCRATCH4, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RDX));
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr(); // BLR X16
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH1,
            static_cast<i32>(divide_error - zero_branch) * 4);
        m_code_buf[overflow_branch] = arm64::b_cond(
            arm64::CS,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    // ---- IDIV r/m8 — signed AX / r/m8 → AL remainder AH ----
    // Intel SDM Vol. 2A, IDIV r/m8. Quotient must fit in signed AL; else #DE.
    case Opcode::IDIV_RM8: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr8(REG_SCRATCH1, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH3);
        }
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));

        const usize zero_branch = m_code_buf.size();
        emit(0); // CBZ Wdivisor, divide_error
        emit(arm64::sxth_x(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::sxtb_x(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::sdiv_x(
            REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sxtb_x(REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::subs_x(31, REG_SCRATCH4, REG_SCRATCH2));
        const usize overflow_branch = m_code_buf.size();
        emit(0); // B.NE divide_error

        emit(arm64::msub_x(
            REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH0));
        // Mask to AL/AH: a sign-extended quotient still has 0xFF in bits
        // 15:8, which would corrupt AH if OR'd in raw.
        emit(arm64::uxtb_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::uxtb_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::orr_x_lsl(
            REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH2, 8));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr(); // BLR X16
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH1,
            static_cast<i32>(divide_error - zero_branch) * 4);
        m_code_buf[overflow_branch] = arm64::b_cond(
            arm64::NE,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    // ---- IDIV r/m16 — signed DX:AX / r/m16 → AX remainder DX ----
    // Intel SDM Vol. 2A, IDIV. Check the 32-bit quotient against its
    // sign-extended low 16 bits before committing; inequality is #DE.
    // emit_store_gpr16 clobbers REG_SCRATCH4.
    case Opcode::IDIV_RM16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH1, op0.reg); // divisor bits
        } else {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH3);
        }
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr16(REG_SCRATCH2, static_cast<u8>(RegIndex::RDX));

        const usize zero_branch = m_code_buf.size();
        emit(0); // CBZ Wdivisor, divide_error
        emit(arm64::orr_x_lsl(
            REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2, 16));
        emit(arm64::sxtw_x(REG_SCRATCH3, REG_SCRATCH3));
        emit(arm64::sxth_x(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::sdiv_x(
            REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH1));
        emit(arm64::sxth_x(REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::subs_x(31, REG_SCRATCH4, REG_SCRATCH0));
        const usize overflow_branch = m_code_buf.size();
        emit(0); // B.NE divide_error

        emit(arm64::msub_x(
            REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH3));
        emit(arm64::mov_x(REG_SCRATCH2, REG_SCRATCH4)); // quot off SCRATCH4
        emit_store_gpr16(REG_SCRATCH2, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RDX));
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr(); // BLR X16
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH1,
            static_cast<i32>(divide_error - zero_branch) * 4);
        m_code_buf[overflow_branch] = arm64::b_cond(
            arm64::NE,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        break;
    }

    // ---- IDIV r/m32 — signed EDX:EAX / r/m32 -> EAX remainder EDX ----
    // Intel SDM Vol. 2A, IDIV. ARM SDIV truncates toward zero like x86. Check
    // the full 64-bit quotient against its sign-extended low 32 bits before
    // committing architectural registers; inequality is x86 quotient
    // overflow and therefore #DE.
    case Opcode::IDIV_RM32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH1, op0.reg); // divisor bits
        } else {
            emit_compute_ea(REG_SCRATCH3, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH3);
        }
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr32(REG_SCRATCH2, static_cast<u8>(RegIndex::RDX));

        const usize zero_branch = m_code_buf.size();
        emit(0); // CBZ Wdivisor, divide_error
        emit(arm64::orr_x_lsl(
            REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH2, 32));
        emit(arm64::sxtw_x(REG_SCRATCH1, REG_SCRATCH1));
        emit(arm64::sdiv_x(
            REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH1));
        emit(arm64::sxtw_x(REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::subs_x(31, REG_SCRATCH4, REG_SCRATCH0));
        const usize overflow_branch = m_code_buf.size();
        emit(0); // B.NE divide_error

        emit(arm64::msub_x(
            REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH3));
        emit_store_gpr32(REG_SCRATCH4, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RDX));
        const usize done_branch = m_code_buf.size();
        emit(0); // B done

        const usize divide_error = m_code_buf.size();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_mov_imm64(1, static_cast<i64>(instr_rip));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_raise_divide_error)));
        emit_helper_blr(); // BLR X16
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_epilogue(JitExecResult::Continue);

        const usize done = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_w(
            REG_SCRATCH1,
            static_cast<i32>(divide_error - zero_branch) * 4);
        m_code_buf[overflow_branch] = arm64::b_cond(
            arm64::NE,
            static_cast<i32>(divide_error - overflow_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
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
