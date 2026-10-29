// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_memory_ops' bit scan and count family: BSF and BSR
// r16/r32, POPCNT r16/r32/r64 and LZCNT/TZCNT r32.
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
std::optional<bool> JitCompiler::compile_memory_ops_bitscan(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- BSF r16, r/m16 ----
    // Mirror BSF r32. ZF=1 ⇒ dest unchanged (preserve GPR[31:16] and low 16).
    // ZF=0 ⇒ write bit index via emit_store_gpr16 (keeps upper 16).
    case Opcode::BSF_R16_RM16: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_register_operand(op1.type)) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        }

        usize cbz_pos = m_code_buf.size();
        emit(0);

        emit(arm64::rbit_w(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::clz_w(REG_SCRATCH1, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH1, op0.reg);
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0x8A240063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }
        usize done_br_pos = m_code_buf.size();
        emit(0);

        usize zero_pos = m_code_buf.size();
        {
            // ZF=1, dest untouched (SDM undefined; interpreter leaves dest).
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0xAA040063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        usize done_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH0,
            static_cast<i32>(zero_pos - cbz_pos) * 4);
        m_code_buf[done_br_pos] = arm64::b_imm(
            static_cast<i32>(done_pos - done_br_pos) * 4);

        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::None));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        break;
    }

    // ---- BSF r32, r/m32 ----
    case Opcode::BSF_R32_RM32: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }

        usize cbz_pos = m_code_buf.size();
        emit(0);

        emit(arm64::rbit_w(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::clz_w(REG_SCRATCH1, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH1, op0.reg);
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0x8A240063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }
        usize done_br_pos = m_code_buf.size();
        emit(0);

        usize zero_pos = m_code_buf.size();
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0xAA040063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        usize done_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH0,
            static_cast<i32>(zero_pos - cbz_pos) * 4);
        m_code_buf[done_br_pos] = arm64::b_imm(
            static_cast<i32>(done_pos - done_br_pos) * 4);

        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::None));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        break;
    }

    // ---- BSR r16, r/m16 ----
    // Zero-extended load + (31 - clz) equals interpreter's 15-(clz-16).
    case Opcode::BSR_R16_RM16: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_register_operand(op1.type)) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        }

        usize cbz_pos = m_code_buf.size();
        emit(0);

        emit(arm64::clz_w(REG_SCRATCH1, REG_SCRATCH0));
        emit_mov_imm32(REG_SCRATCH2, 31);
        emit(arm64::sub_w(REG_SCRATCH1, REG_SCRATCH2, REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH1, op0.reg);
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0x8A240063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }
        usize done_br_pos = m_code_buf.size();
        emit(0);

        usize zero_pos = m_code_buf.size();
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0xAA040063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        usize done_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH0,
            static_cast<i32>(zero_pos - cbz_pos) * 4);
        m_code_buf[done_br_pos] = arm64::b_imm(
            static_cast<i32>(done_pos - done_br_pos) * 4);

        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::None));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        break;
    }

    // ---- BSR r32, r/m32 ----
    case Opcode::BSR_R32_RM32: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }

        usize cbz_pos = m_code_buf.size();
        emit(0);

        emit(arm64::clz_w(REG_SCRATCH1, REG_SCRATCH0));
        emit_mov_imm32(REG_SCRATCH2, 31);
        emit(arm64::sub_w(REG_SCRATCH1, REG_SCRATCH2, REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH1, op0.reg);
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0x8A240063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }
        usize done_br_pos = m_code_buf.size();
        emit(0);

        usize zero_pos = m_code_buf.size();
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit_mov_imm32(REG_SCRATCH4, 0x40);
            emit(0xAA040063u);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        usize done_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH0,
            static_cast<i32>(zero_pos - cbz_pos) * 4);
        m_code_buf[done_br_pos] = arm64::b_imm(
            static_cast<i32>(done_pos - done_br_pos) * 4);

        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(CpuState::FlagOp::None));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        break;
    }

    // ---- POPCNT r16/r32/r64, r/m ----
    // Intel SDM Vol. 2 POPCNT: dest = popcount(src);
    // ZF=(result==0); OF=SF=AF=CF=PF=0.
    case Opcode::POPCNT_R16_RM16:
    case Opcode::POPCNT_R32_RM32:
    case Opcode::POPCNT_R64_RM64: {
        // Materialize lazy flags before overwriting
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();  // BLR X16

        const bool is64 = instr.opcode == Opcode::POPCNT_R64_RM64;
        const bool is16 = instr.opcode == Opcode::POPCNT_R16_RM16;

        if (is_register_operand(op1.type)) {
            if (is64) {
                emit_load_gpr64(REG_SCRATCH0, op1.reg);
            } else if (is16) {
                emit_load_gpr16(REG_SCRATCH0, op1.reg);  // UXTH → 16-bit mask
            } else {
                emit_load_gpr32(REG_SCRATCH0, op1.reg);
            }
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            if (is64) {
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else if (is16) {
                emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }

        // ARM64 NEON popcount. r64 uses the full D0; r16/r32 use S0
        // (high vector lanes already zero from FMOV).
        if (is64) {
            emit(0x9E670000u |
                 (static_cast<u32>(REG_SCRATCH0) << 5));  // FMOV D0, X0
        } else {
            emit(0x1E270000u |
                 (static_cast<u32>(REG_SCRATCH0) << 5));  // FMOV S0, W0
        }
        emit(0x0E205800u);                            // CNT V0.8B, V0.8B
        emit(0x0E31B800u);                            // ADDV B0, V0.8B
        emit(0x0E013C00u |
             static_cast<u32>(REG_SCRATCH0));          // UMOV W0, V0.B[0]

        if (is64) {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        } else if (is16) {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }

        // Flags: clear CF,PF,AF,ZF,SF,OF; set ZF if result==0
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        // Clear bits 0(CF),2(PF),4(AF),6(ZF),7(SF),11(OF) = 0x8D5
        emit_mov_imm32(REG_SCRATCH4, 0x8D5u);
        emit(0x8A240063u);  // BIC X3, X3, X4

        // If result != 0, skip setting ZF
        usize cbnz_pos = m_code_buf.size();
        emit(0);  // placeholder CBNZ
        emit_mov_imm32(REG_SCRATCH4, 0x40u);
        emit(0xAA040063u);  // ORR X3, X3, X4 (set ZF)
        usize done_pos = m_code_buf.size();
        m_code_buf[cbnz_pos] = arm64::cbnz_w(REG_SCRATCH0,
            static_cast<i32>(done_pos - cbnz_pos) * 4);

        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));

        {
            u32 op_off = flag_op_offset();
            emit_mov_imm32(REG_SCRATCH3,
                           static_cast<u32>(CpuState::FlagOp::None));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
            emit_clear_cf_overlay();
        }
        break;
    }

    // ---- LZCNT / TZCNT r32, r/m32 (BMI1). Semantics:
    //   LZCNT: dst = CLZ(src);           CF=(src==0); ZF=(dst==0)
    //   TZCNT: dst = CLZ(RBIT(src));     CF=(src==0); ZF=(dst==0)
    // Other flags undefined; fuzz mask narrows comparison to CF+ZF.
    case Opcode::LZCNT_R32_RM32:
    case Opcode::TZCNT_R32_RM32: {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }

        if (instr.opcode == Opcode::TZCNT_R32_RM32) {
            emit(arm64::rbit_w(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::clz_w(REG_SCRATCH1, REG_SCRATCH1));
        } else {
            emit(arm64::clz_w(REG_SCRATCH1, REG_SCRATCH0));
        }
        emit_store_gpr32(REG_SCRATCH1, op0.reg);

        // Flags: clear CF(bit0) + ZF(bit6); set CF if src==0; set ZF if dst==0.
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit_mov_imm32(REG_SCRATCH4, 0x41u);   // CF|ZF bits
        emit(0x8A240063u);                     // BIC X3, X3, X4

        // Set CF (bit 0) if src==0.
        usize src_nz_pos = m_code_buf.size();
        emit(0);  // CBNZ src==0 ? skip : set CF
        emit_mov_imm32(REG_SCRATCH4, 0x1u);
        emit(0xAA040063u);                     // ORR X3, X3, X4
        usize after_cf = m_code_buf.size();
        m_code_buf[src_nz_pos] = arm64::cbnz_w(REG_SCRATCH0,
            static_cast<i32>(after_cf - src_nz_pos) * 4);

        // Set ZF (bit 6) if dst==0.
        usize dst_nz_pos = m_code_buf.size();
        emit(0);
        emit_mov_imm32(REG_SCRATCH4, 0x40u);
        emit(0xAA040063u);                     // ORR X3, X3, X4
        usize after_zf = m_code_buf.size();
        m_code_buf[dst_nz_pos] = arm64::cbnz_w(REG_SCRATCH1,
            static_cast<i32>(after_zf - dst_nz_pos) * 4);

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
    return true;
}

} // namespace vitreo::cpu::jit
