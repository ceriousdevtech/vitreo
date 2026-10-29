// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: the register stack. FLD/FST/FSTP ST(i), FXCH,
// the constant pushes FLD1/FLDZ/FLDPI/FLDL2T/FLDL2E/FLDLG2/FLDLN2 and
// FCMOVcc (Intel SDM Vol. 1 8.3.3-8.3.4), plus emit_fpu_pop, the TOP/tag
// bookkeeping the arithmetic, compare and store families end with.
//
// One family of compile_fpu_native's native lowerings, split out of
// ops/fpu.cpp (ADR-0006) as pure code motion: the blocks are verbatim and in
// their original order. compile_fpu_native (ops/fpu.cpp) owns the dispatch
// chain and the jit_fpu_op helper fallback; the declaration in
// jit_compiler.hpp states what the nullopt return means.

#include "../jit_helpers.hpp"
#include "fpu_internal.hpp"

#include <optional>

namespace vitreo::cpu::jit {

void JitCompiler::emit_fpu_pop() {
    // Matches FpuState::pop(): set_tag(top(), Empty); set_top((top()+1)&7).
    constexpr u32 status_offset = static_cast<u32>(
        offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
    constexpr u32 tag_offset = static_cast<u32>(
        offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));

    // x87 TOP bookkeeping via ARM64 bitfield instructions instead of
    // open-coded shift/mask/merge. UBFX replaces LSR+MOVZ+AND on the read;
    // BFI replaces MOVZ+MVN+AND+LSL+ORR on the write, and because BFI takes
    // only the low 3 bits of its source the (top + 1) & 7 wrap is implicit,
    // so the second mask disappears as well. 22 instructions -> 14, on the pop
    // that most x87 lowerings end with.
    //
    // This only makes the stack bookkeeping cheaper. It does not change where
    // the stack lives. Keeping the values themselves in ARM registers across
    // a block is a separate change, and it trades against Float80 storage and
    // bit-exact PC rounding.
    emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
    emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
    emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 11, 3));

    emit(arm64::add_x_imm(REG_EA_TMP, REG_CPU_STATE, tag_offset));
    emit(arm64::ldrh_reg(REG_SCRATCH0, REG_EA_TMP, 31));
    emit(arm64::lsl_w_imm(REG_CALL_TMP, REG_SCRATCH2, 1));
    emit(arm64::movz_w(REG_SCRATCH4, static_cast<u16>(FpuTag::Empty)));
    emit(arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_CALL_TMP));
    emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
    emit(arm64::strh_reg(REG_SCRATCH0, REG_EA_TMP, 31));

    emit(arm64::add_w_imm(REG_SCRATCH2, REG_SCRATCH2, 1));
    emit(arm64::bfi_w(REG_SCRATCH1, REG_SCRATCH2, 11, 3));
    emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
}

std::optional<bool> JitCompiler::compile_fpu_stack(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];

    if ((instr.opcode == Opcode::FLD_STI ||
         instr.opcode == Opcode::FSTP_STI ||
         instr.opcode == Opcode::FST_STI) &&
        op0.type == OperandType::Imm8) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        const u8 sti = static_cast<u8>(op0.imm) & 7;

        // W5 = old physical TOP. X3 = source value address.
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_EA_TMP, REG_SCRATCH1, 11, 3));

        if (instr.opcode == Opcode::FLD_STI) {
            emit(arm64::movz_w(REG_SCRATCH2, sti));
            emit(arm64::add_w(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH2));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        } else {
            // FSTP reads old ST(0); its destination is old ST(i).
            emit(arm64::mov_w(REG_SCRATCH2, REG_EA_TMP));
        }
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                             REG_SCRATCH2, 0));
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));

        // Compute destination physical index and commit the packed value.
        if (instr.opcode == Opcode::FLD_STI) {
            emit(arm64::movz_w(REG_SCRATCH4, 1));
            emit(arm64::sub_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
        } else {
            emit(arm64::movz_w(REG_SCRATCH4, sti));
            emit(arm64::add_w(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH4));
            emit(arm64::mov_w(REG_EA_TMP, REG_SCRATCH2));
        }
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3,
                             REG_SCRATCH2, 0));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 1));

        if (instr.opcode == Opcode::FLD_STI) {
            // push() derives Zero versus Valid from the copied value.
            emit(arm64::movz_w(REG_SCRATCH4, 3));
            emit(arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_EA_TMP));
            emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_w(REG_SCRATCH4,
                               static_cast<u16>(FpuTag::Valid)));
            const usize mantissa_zero_branch = m_code_buf.size();
            emit(0);
            const usize tag_ready_branch = m_code_buf.size();
            emit(0);
            const usize check_exponent_pos = m_code_buf.size();
            emit(arm64::movz_w(REG_SCRATCH0, 0x7FFF));
            emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH0));
            const usize exponent_nonzero_branch = m_code_buf.size();
            emit(0);
            emit(arm64::movz_w(REG_SCRATCH4,
                               static_cast<u16>(FpuTag::Zero)));
            const usize tag_ready_pos = m_code_buf.size();
            emit(arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_EA_TMP));
            emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::strh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));

            m_code_buf[mantissa_zero_branch] = arm64::cbz_x(
                REG_SCRATCH0,
                static_cast<i32>(check_exponent_pos -
                                 mantissa_zero_branch) * 4);
            m_code_buf[tag_ready_branch] = arm64::b_imm(
                static_cast<i32>(tag_ready_pos - tag_ready_branch) * 4);
            m_code_buf[exponent_nonzero_branch] = arm64::cbnz_w(
                REG_SCRATCH0,
                static_cast<i32>(tag_ready_pos -
                                 exponent_nonzero_branch) * 4);

            // Update TOP after the source snapshot and destination/tag writes.
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE,
                                 status_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
            emit_mov_imm32(REG_SCRATCH4, FPU_SW_TOP_MASK);
            emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
            emit(arm64::lsr_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
            emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 11));
            emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_EA_TMP));
            emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        } else if (instr.opcode == Opcode::FSTP_STI) {
            // pop() marks old ST(0) empty, independently of FSTP's target.
            emit(arm64::movz_w(REG_SCRATCH4, 3));
            emit(arm64::lsr_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
            emit(arm64::movz_w(REG_SCRATCH0, sti));
            emit(arm64::sub_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH0));
            emit(arm64::movz_w(REG_SCRATCH0, 7));
            emit(arm64::and_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH0));
            emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
            emit(arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_EA_TMP));
            emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::strh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));

            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE,
                                 status_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
            emit_mov_imm32(REG_SCRATCH4, FPU_SW_TOP_MASK);
            emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
            emit(arm64::lsr_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
            emit(arm64::movz_w(REG_SCRATCH4, 1));
            emit(arm64::add_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
            emit(arm64::movz_w(REG_SCRATCH4, 7));
            emit(arm64::and_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
            emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 11));
            emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_EA_TMP));
            emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        }

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Intel SDM Vol. 2 FXCH ST(i): swap ST(0) and ST(i). Tags stay on the
    // physical slots (interpreter set_st does not retag).
    if (instr.opcode == Opcode::FXCH_STI && op0.type == OperandType::Imm8) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        const u8 sti = static_cast<u8>(op0.imm) & 7;

        const auto emit_phys_addr = [&](u8 logical, u8 dst_reg) {
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
            emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
            emit(arm64::movz_w(REG_SCRATCH4, logical));
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_w(REG_SCRATCH4, 7));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
            emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::add_x_imm(dst_reg, REG_CPU_STATE, st_offset));
            emit(arm64::add_x_lsl(dst_reg, dst_reg, REG_SCRATCH2, 0));
        };

        emit_phys_addr(0, REG_SCRATCH3);
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_EA_TMP, REG_SCRATCH4, 31));
        emit(arm64::mov_x(17, REG_SCRATCH3));

        emit_phys_addr(sti, REG_SCRATCH3);
        emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH4, 31));

        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::strh_reg(REG_EA_TMP, REG_SCRATCH4, 31));
        emit(arm64::add_x_imm(REG_SCRATCH4, 17,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::str_x_uoff(REG_SCRATCH1, 17, 0));
        emit(arm64::strh_reg(REG_SCRATCH2, REG_SCRATCH4, 31));

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Intel SDM Vol. 2 FLD1/FLDZ: push the 8087 80-bit constant (RC=nearest).
    if (instr.opcode == Opcode::FLD1 || instr.opcode == Opcode::FLDZ ||
        instr.opcode == Opcode::FLDPI || instr.opcode == Opcode::FLDL2T ||
        instr.opcode == Opcode::FLDL2E || instr.opcode == Opcode::FLDLG2 ||
        instr.opcode == Opcode::FLDLN2) {
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        // Bake the ROUND-TOWARD-ZERO significand and add a runtime delta chosen
        // by the control word's RC field. A baked round-to-nearest constant is
        // wrong for three of the four RC settings -- see x87_const in
        // fpu_state.hpp. Values come from that one definition so this does not
        // become a third copy.
        //
        // delta is always 0 or 1 (every constant is irrational, so round-up is
        // exactly trunc+1), which makes the whole RC decision a 4-bit lookup:
        //   delta = (rc_mask >> rc) & 1,  rc = FCW[11:10]
        //   rc: 0=nearest 1=down 2=up 3=chop; all constants are positive, so
        //   down and chop both truncate.
        u64 mantissa = 0;
        u32 exp_sign = 0;
        u16 tag = static_cast<u16>(FpuTag::Zero);
        u32 rc_mask = 0;   // 0 = exact constant, RC cannot change it
        const auto take = [&](const x87_const::RoundedConst& c) {
            mantissa = c.trunc;
            exp_sign = c.exponent;
            tag = static_cast<u16>(FpuTag::Valid);
            // bit0: nearest, bit2: round-up. nearest is trunc or trunc+1.
            rc_mask = (c.nearest != c.trunc ? 0x1u : 0u) | 0x4u;
        };
        switch (instr.opcode) {
        case Opcode::FLD1:
            mantissa = 0x8000000000000000ULL;
            exp_sign = 0x3FFF;
            tag = static_cast<u16>(FpuTag::Valid);
            break;
        case Opcode::FLDPI:  take(x87_const::kPi);  break;
        case Opcode::FLDL2T: take(x87_const::kL2T); break;
        case Opcode::FLDL2E: take(x87_const::kL2E); break;
        case Opcode::FLDLG2: take(x87_const::kLg2); break;
        case Opcode::FLDLN2: take(x87_const::kLn2); break;
        default:
            break;
        }

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 11, 3));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::mov_w(REG_EA_TMP, REG_SCRATCH2));

        emit_mov_imm32(REG_SCRATCH4, FPU_SW_TOP_MASK | FPU_SW_C1);
        emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));

        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH2, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 0));
        emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(mantissa));
        if (rc_mask != 0) {
            constexpr u32 control_offset = static_cast<u32>(
                offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CPU_STATE, control_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
            emit(arm64::ubfx_w(REG_SCRATCH1, REG_SCRATCH1, 10, 2));  // rc
            emit(arm64::movz_w(REG_SCRATCH2, static_cast<u16>(rc_mask)));
            emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::movz_w(REG_SCRATCH4, 1));
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::add_x_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2, 0));
        }
        emit_mov_imm32(REG_SCRATCH1, exp_sign);
        emit(arm64::movz_w(REG_SCRATCH2, tag));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));

        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 1));
        emit(arm64::movz_w(REG_SCRATCH0, 3));
        emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH0, REG_EA_TMP));
        emit(arm64::mvn_w(REG_SCRATCH0, REG_SCRATCH0));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::lslv_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));

        // FLD1 is exactly representable in binary64.  Keep the complete
        // Float80 push authoritative above, and publish a one-instruction D0
        // mirror only for the adjacent register FADD chain.  Intel SDM Vol. 2
        // defines D8 C0 as ST(0) <- ST(0) + ST(0); no other ST(i) form may
        // consume this self-source mirror.
        if (instr.opcode == Opcode::FLD1 &&
            m_next_opcode == Opcode::FADD_STI &&
            !kDisableX87D0Forward &&
            native_arith_allowed(Opcode::FADD_STI)) {
            emit_mov_imm64(REG_SCRATCH0,
                           static_cast<i64>(0x3FF0000000000000ULL));
            emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
            emit(arm64::movz_w(kX87D0Valid, 1));
            m_x87_d0_forward_out = true;
        }

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if ((instr.opcode == Opcode::FCMOVB_STI ||
         instr.opcode == Opcode::FCMOVE_STI ||
         instr.opcode == Opcode::FCMOVBE_STI ||
         instr.opcode == Opcode::FCMOVU_STI ||
         instr.opcode == Opcode::FCMOVNB_STI ||
         instr.opcode == Opcode::FCMOVNE_STI ||
         instr.opcode == Opcode::FCMOVNBE_STI ||
         instr.opcode == Opcode::FCMOVNU_STI) &&
        op0.type == OperandType::Imm8) {
        u8 cond = 0x2;
        switch (instr.opcode) {
        case Opcode::FCMOVB_STI:   cond = 0x2; break;
        case Opcode::FCMOVE_STI:   cond = 0x4; break;
        case Opcode::FCMOVBE_STI:  cond = 0x6; break;
        case Opcode::FCMOVU_STI:   cond = 0xA; break;
        case Opcode::FCMOVNB_STI:  cond = 0x3; break;
        case Opcode::FCMOVNE_STI:  cond = 0x5; break;
        case Opcode::FCMOVNBE_STI: cond = 0x7; break;
        case Opcode::FCMOVNU_STI:  cond = 0xB; break;
        default: break;
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                              (JIT_CTX_OFF_COND_FNS + cond * 8u) >> 3));
        emit_helper_blr();
        const usize skip_br = m_code_buf.size();
        emit(0);
        constexpr u32 st_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, st));
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        const u8 sti = static_cast<u8>(op0.imm) & 7;
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
        emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
        emit(arm64::movz_w(REG_SCRATCH4, sti));
        emit(arm64::add_w(REG_SCRATCH1, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::movz_x(REG_SCRATCH4, sizeof(Float80), 0));
        emit(arm64::mul_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::mul_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, st_offset));
        emit(arm64::add_x_lsl(REG_EA_TMP, REG_SCRATCH3, REG_SCRATCH1, 0));
        emit(arm64::add_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2, 0));
        emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_EA_TMP, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_EA_TMP,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_SCRATCH3,
                             offsetof(Float80, exponent_sign)));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH4, 31));
        const usize done_pos = m_code_buf.size();
        m_code_buf[skip_br] = arm64::cbz_w(
            REG_SCRATCH0, static_cast<i32>(done_pos - skip_br) * 4);
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    return std::nullopt;
}

} // namespace vitreo::cpu::jit
