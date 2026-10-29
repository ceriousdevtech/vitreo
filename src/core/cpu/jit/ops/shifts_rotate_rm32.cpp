// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — ROL / ROR r/m32 (imm8 / 1 / CL).
//
// One family of compile_shifts' opcode space, split out of ops/shifts.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_shifts (ops/shifts.cpp) owns the dispatch chain and the shared
// is_last tail; see the declaration in jit_compiler_lowering.hpp for the
// nullopt / true contract.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_shifts_rotate_rm32(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- ROR r/m32, imm8 / 1 ----
    case Opcode::ROR_RM32_IMM8:
    case Opcode::ROR_RM32_1: {
        u8 shift = (instr.opcode == Opcode::ROR_RM32_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;

        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC X3, X3, X4
            emit(0x531F7C04u);   // UBFX W4, W0, #31, #1
            emit(0xAA040063u);   // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                emit(arm64::movz_w(4, 0x800));
                emit(0x8A240063u);   // BIC X3, X3, X4
                emit(0x531F7C02u);   // UBFX W2, W0, #31, #1
                emit(0x531E7804u);   // UBFX W4, W0, #30, #1
                emit(0x4A020084u);   // EOR W4, W4, W2
                emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
                emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                      of_overlay_offset()));
                emit(0x53155084u);   // LSL W4, W4, #11
                emit(0xAA040063u);   // ORR X3, X3, X4
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- ROR r/m32, CL ----
    // Intel: count = CL & 0x1F. If count == 0, flags and destination are
    // unchanged. Else rotate, then CF = MSB(result); OF (count==1 only) =
    // MSB(result) XOR bit30(result). Lazy flags must be materialized first
    // because we write rflags directly.
    case Opcode::ROR_RM32_CL:
    case Opcode::ROL_RM32_CL: {
        const bool is_rol = (instr.opcode == Opcode::ROL_RM32_CL);
        bool is_mem = !is_register_operand(op0.type);

        // Check masked count up front; if zero, skip everything.
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(cpu::RegIndex::RCX));
        // AND W8, W1, #0x1F
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0);  // CBZ W8, end (patched later)

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(cpu::RegIndex::RCX));
        // Recompute masked count in W8 (W8 itself is preserved across loads).
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));

        // Perform the rotation. ARM RORV uses low 5 bits of Wm for 32-bit
        // rotates, which matches x86's count mask. For ROL, negate the count.
        if (is_rol) {
            emit(0x4B0103E1u); // SUB W1, WZR, W1 (NEG W1, W1)
        }
        emit(arm64::rorv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        // Update CF and (conditionally) OF in rflags.
        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            // Clear CF (bit 0).
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);                         // BIC X3, X3, X4
            if (is_rol) {
                // CF = bit 0 of result.  AND W4, W0, #1
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                // CF = bit 31 of result.  UBFX W4, W0, #31, #1
                emit(0x531F7C04u);
            }
            emit(0xAA040063u);                         // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            // OF is defined only when masked count == 1. Leave a pending
            // FlagOp's OF intact for every other count (Intel: undefined).
            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);  // B.NE skip OF
            emit(arm64::movz_w(4, 0x800));
            emit(0x8A240063u);                         // BIC X3, X3, X4
            emit(0x531F7C02u);                         // UBFX W2, W0, #31, #1
            if (is_rol) {
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                emit(0x531E7804u);
            }
            emit(0x4A020084u);   // EOR W4, W4, W2  (OF bit value 0/1)
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(0x53155084u);   // LSL W4, W4, #11
            emit(0xAA040063u);                         // ORR X3, X3, X4
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        // Store rotated result.
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }

        // Patch CBZ to branch here (past all flag+store code).
        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            i32 off_bytes =
                static_cast<i32>(skip_target - cbz_idx) * 4;
            m_code_buf[cbz_idx] = arm64::cbz_w(8, off_bytes);
        }
        break;
    }

    // ---- ROL r/m32, imm8 ----
    case Opcode::ROL_RM32_IMM8:
    case Opcode::ROL_RM32_1: {
        // ROL_RM32_1 (D1 /0) rotates by an implicit 1 — the count is not an
        // immediate operand. Its OF is defined (the shift==1 branch below),
        // which is exactly why decompression checksum loops use it hot.
        u8 shift = (instr.opcode == Opcode::ROL_RM32_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;

        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        u8 ror_amount = (32 - shift) & 31;
        emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, ror_amount));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC X3, X3, X4
            emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH0, 0x1)); // AND W4, W0, #1
            emit(0xAA040063u);   // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                emit(arm64::movz_w(4, 0x800));
                emit(0x8A240063u);   // BIC X3, X3, X4
                emit(0x531F7C02u);   // UBFX W2, W0, #31, #1
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1)); // AND W4, W0, #1
                emit(0x4A020084u);   // EOR W4, W4, W2
                emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
                emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                      of_overlay_offset()));
                emit(0x53155084u);   // LSL W4, W4, #11
                emit(0xAA040063u);   // ORR X3, X3, X4
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
