// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: bit scans, byte swaps and bit
// tests. BSR/BSF r64, BSWAP r64/r32, BT/BTS/BTR/BTC r/m64 and
// LZCNT/TZCNT r64.
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
std::optional<bool> JitCompiler::compile_bit_ops64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // BSR r64,r/m64 — for a nonzero source, write the index of its most
    // significant set bit and clear ZF. For zero, set ZF and leave the
    // destination unchanged. CF/OF/SF/PF/AF are architecturally undefined;
    // match the interpreter's deterministic choices for JIT parity (Intel
    // SDM Vol. 2, BSR).
    case Opcode::BSR_R64_RM64: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();  // BLR X16
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        }

        const usize zero_branch = m_code_buf.size();
        emit(0);
        emit(arm64::clz_x(REG_SCRATCH1, REG_SCRATCH0));
        emit_mov_imm64(REG_SCRATCH2, 63);
        emit(arm64::sub_x(REG_SCRATCH1, REG_SCRATCH2, REG_SCRATCH1));
        emit_store_gpr64(REG_SCRATCH1, op0.reg);
        {
            const u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
            emit_mov_imm32(REG_SCRATCH4, 0x8D5);
            emit(0x8A240063u);  // clear CF/PF/AF/ZF/SF/OF
            emit_mov_imm32(REG_SCRATCH4, 1);
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));

            // SF = source bit 63.
            emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 7));
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH1));

            // PF = even parity of source[7:0]. Fold to a nibble, then use
            // the 0x9669 even-parity lookup bitmap.
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH1, 4));
            emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
            emit_mov_imm32(REG_SCRATCH2, 0xFu);
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
            emit_mov_imm32(REG_SCRATCH2, 0x9669u);
            emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            emit_mov_imm32(REG_SCRATCH1, 1);
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 2));
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        }
        const usize done_branch = m_code_buf.size();
        emit(0);

        const usize zero_target = m_code_buf.size();
        {
            const u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
            emit_mov_imm32(REG_SCRATCH4, 1);
            emit(0x8A240063u);  // clear CF
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0xAA040063u);  // ORR X3,X3,X4 — set ZF
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        }
        const usize done_target = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_x(
            REG_SCRATCH0, static_cast<i32>(zero_target - zero_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done_target - done_branch) * 4);
        emit_mov_imm32(REG_SCRATCH3,
                       static_cast<u32>(CpuState::FlagOp::None));
        emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                              flag_op_offset()));
        emit_clear_cf_overlay();
        break;
    }

    // BSF r64,r/m64 — for a nonzero source, write the index of its least
    // significant set bit and clear ZF. For zero, set ZF and leave the
    // destination unchanged. CF/OF/SF/PF/AF are architecturally undefined;
    // match the interpreter's deterministic choices for JIT parity (Intel
    // SDM Vol. 2, BSF).
    case Opcode::BSF_R64_RM64: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();  // BLR X16
        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        }

        const usize zero_branch = m_code_buf.size();
        emit(0);
        emit(arm64::rbit_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::clz_x(REG_SCRATCH1, REG_SCRATCH1));
        emit_store_gpr64(REG_SCRATCH1, op0.reg);
        {
            const u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
            emit_mov_imm32(REG_SCRATCH4, 0x8D5);
            emit(0x8A240063u);  // clear CF/PF/AF/ZF/SF/OF
            emit_mov_imm32(REG_SCRATCH4, 1);
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));

            // SF = source bit 63.
            emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
            emit(arm64::lsl_x_imm(REG_SCRATCH1, REG_SCRATCH1, 7));
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH1));

            // PF = even parity of source[7:0].
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_SCRATCH2, REG_SCRATCH1, 4));
            emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
            emit_mov_imm32(REG_SCRATCH2, 0xFu);
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
            emit_mov_imm32(REG_SCRATCH2, 0x9669u);
            emit(arm64::lsrv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            emit_mov_imm32(REG_SCRATCH1, 1);
            emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 2));
            emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        }
        const usize done_branch = m_code_buf.size();
        emit(0);

        const usize zero_target = m_code_buf.size();
        {
            const u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
            emit_mov_imm32(REG_SCRATCH4, 1);
            emit(0x8A240063u);  // clear CF
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0xAA040063u);  // ORR X3,X3,X4 — set ZF
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        }
        const usize done_target = m_code_buf.size();
        m_code_buf[zero_branch] = arm64::cbz_x(
            REG_SCRATCH0, static_cast<i32>(zero_target - zero_branch) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done_target - done_branch) * 4);
        emit_mov_imm32(REG_SCRATCH3,
                       static_cast<u32>(CpuState::FlagOp::None));
        emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                              flag_op_offset()));
        emit_clear_cf_overlay();
        break;
    }

    // ========================================================================
    // BSWAP r64  — REX.W 0F C8+rd
    // Byte-swaps a 64-bit register. Does not affect flags.
    // ========================================================================
    case Opcode::BSWAP_R64: {
        emit_load_gpr64(REG_SCRATCH0, op0.reg);
        emit(arm64::rev_x(REG_SCRATCH0, REG_SCRATCH0));
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // BSWAP r32 — byte-swap; architectural 32-bit write zeroes the upper half.
    case Opcode::BSWAP_R32: {
        emit_load_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::rev_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // BT / BTS / BTR / BTC r/m64, r64 — CF-only (Intel SDM Vol. 2). Register
    // bases mask the index to 6 bits; memory bases use signed (index/64)*8
    // byte displacement into a bit string. Prefer CF overlay so a pending
    // ALU FlagOp can still answer ZF.
    case Opcode::BT_RM64_R64:
    case Opcode::BTS_RM64_R64:
    case Opcode::BTR_RM64_R64:
    case Opcode::BTC_RM64_R64: {
        if (!is_register_operand(op1.type)) return false;
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea64(REG_CALL_TMP, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op0);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
            emit(arm64::asr_x_imm(REG_SCRATCH3, REG_SCRATCH1, 6));
            emit(arm64::lsl_x_imm(REG_SCRATCH3, REG_SCRATCH3, 3));
            // Full 64-bit signed displacement — do not sxtw (that truncates).
            emit(arm64::add_x(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH3));
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);  // load may clobber W1
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
        }
        emit_mov_imm32(REG_SCRATCH2, 63);
        emit(arm64::and_x(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));

        emit(arm64::lsrv_x(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH2, 1);
        emit(arm64::and_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));
        emit_bt_write_cf(REG_SCRATCH4);

        if (instr.opcode != Opcode::BT_RM64_R64) {
            emit_mov_imm32(REG_SCRATCH2, 1);
            emit(arm64::lslv_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            switch (instr.opcode) {
            case Opcode::BTS_RM64_R64:
                emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTR_RM64_R64:
                emit(arm64::bic_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTC_RM64_R64:
                emit(arm64::eor_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            default:
                break;
            }
            if (dst_is_mem) {
                emit_guest_store64(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_store_gpr64(REG_SCRATCH0, op0.reg);
            }
        }
        break;
    }

    // BT / BTS / BTR / BTC r/m64, imm8 — same CF overlay contract as the
    // register-index forms. Memory with imm >= 64 addresses outside this
    // qword; leave those to the interpreter (matches RM32_IMM8 policy).
    case Opcode::BT_RM64_IMM8:
    case Opcode::BTS_RM64_IMM8:
    case Opcode::BTR_RM64_IMM8:
    case Opcode::BTC_RM64_IMM8: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem && (op1.imm & ~static_cast<i64>(63)) != 0) return false;
        if (dst_is_mem) {
            emit_compute_ea64(REG_CALL_TMP, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_CALL_TMP, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        const u8 bit_pos = static_cast<u8>(op1.imm & 63);
        emit(arm64::ubfx_x(REG_SCRATCH4, REG_SCRATCH0, bit_pos, 1));
        emit_bt_write_cf(REG_SCRATCH4);
        if (instr.opcode != Opcode::BT_RM64_IMM8) {
            emit_mov_imm64(REG_SCRATCH2, 1ull << bit_pos);
            switch (instr.opcode) {
            case Opcode::BTS_RM64_IMM8:
                emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTR_RM64_IMM8:
                emit(arm64::bic_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTC_RM64_IMM8:
                emit(arm64::eor_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            default:
                break;
            }
            if (dst_is_mem) {
                emit_guest_store64(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_store_gpr64(REG_SCRATCH0, op0.reg);
            }
        }
        break;
    }

    // LZCNT / TZCNT r64, r/m64 (BMI1). CF=(src==0); ZF=(dst==0). Other flags
    // undefined; leave them intact like the r32 lowering.
    case Opcode::LZCNT_R64_RM64:
    case Opcode::TZCNT_R64_RM64: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_register_operand(op1.type)) {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op1);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        }

        if (instr.opcode == Opcode::TZCNT_R64_RM64) {
            emit(arm64::rbit_x(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::clz_x(REG_SCRATCH1, REG_SCRATCH1));
        } else {
            emit(arm64::clz_x(REG_SCRATCH1, REG_SCRATCH0));
        }
        emit_store_gpr64(REG_SCRATCH1, op0.reg);

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit_mov_imm32(REG_SCRATCH4, 0x41u);  // CF|ZF
        emit(0x8A240063u);                    // BIC X3, X3, X4

        usize src_nz_pos = m_code_buf.size();
        emit(0);  // CBNZ src
        emit_mov_imm32(REG_SCRATCH4, 0x1u);
        emit(0xAA040063u);  // ORR X3, X3, X4
        usize after_cf = m_code_buf.size();
        m_code_buf[src_nz_pos] = arm64::cbnz_x(
            REG_SCRATCH0, static_cast<i32>(after_cf - src_nz_pos) * 4);

        usize dst_nz_pos = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, 0x40u);
        emit(0xAA040063u);
        usize after_zf = m_code_buf.size();
        m_code_buf[dst_nz_pos] = arm64::cbnz_x(
            REG_SCRATCH1, static_cast<i32>(after_zf - dst_nz_pos) * 4);

        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::None));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
