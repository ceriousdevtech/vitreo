// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — ROL / ROR r/m8 and r/m16 (imm8 / 1 / CL).
//
// One family of compile_shifts' opcode space, split out of ops/shifts.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_shifts (ops/shifts.cpp) owns the dispatch chain and the shared
// is_last tail; see the declaration in jit_compiler_lowering.hpp for the
// nullopt / true contract.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_shifts_rotate_narrow(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- ROL / ROR r/m16, imm8 / 1 ----
    // Duplicate low 16 into both halves then use 32-bit ROR (period 16).
    // Partial-flag: CF (and OF for count==1) only — no FlagOp.
    case Opcode::ROR_RM16_IMM8:
    case Opcode::ROR_RM16_1:
    case Opcode::ROL_RM16_IMM8:
    case Opcode::ROL_RM16_1: {
        const bool is_rol =
            (instr.opcode == Opcode::ROL_RM16_IMM8 ||
             instr.opcode == Opcode::ROL_RM16_1);
        u8 shift = (instr.opcode == Opcode::ROR_RM16_1 ||
                    instr.opcode == Opcode::ROL_RM16_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;

        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH0, 16));
        if (is_rol) {
            u8 ror_amount = static_cast<u8>((32 - shift) & 31);
            emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, ror_amount));
        } else {
            emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));
        }
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC X3, X3, X4
            if (is_rol) {
                // CF = bit 0 of result.
                emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH0, 0x1));
            } else {
                // CF = bit 15 of result.
                emit(0x530F3C04u); // UBFX W4, W0, #15, #1
            }
            emit(0xAA040063u);   // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                emit(arm64::movz_w(4, 0x800));
                emit(0x8A240063u);   // BIC X3, X3, X4
                emit(0x530F3C02u);   // UBFX W2, W0, #15, #1
                if (is_rol) {
                    emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
                } else {
                    emit(0x530E3804u); // UBFX W4, W0, #14, #1
                }
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
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- ROL / ROR r/m16, CL ----
    case Opcode::ROR_RM16_CL:
    case Opcode::ROL_RM16_CL: {
        const bool is_rol = (instr.opcode == Opcode::ROL_RM16_CL);
        bool is_mem = !is_register_operand(op0.type);

        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(cpu::RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #0x1F
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0);  // CBZ W8, end

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(cpu::RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));

        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH0, 16));
        if (is_rol) {
            emit(0x4B0103E1u); // SUB W1, WZR, W1 (NEG W1, W1)
        }
        emit(arm64::rorv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);
            if (is_rol) {
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                emit(0x530F3C04u); // UBFX W4, W0, #15, #1
            }
            emit(0xAA040063u);
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);
            emit(arm64::movz_w(4, 0x800));
            emit(0x8A240063u);
            emit(0x530F3C02u); // UBFX W2, W0, #15, #1
            if (is_rol) {
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                emit(0x530E3804u); // UBFX W4, W0, #14, #1
            }
            emit(0x4A020084u); // EOR W4, W4, W2
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(0x53155084u);
            emit(0xAA040063u);
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }

        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            i32 off_bytes =
                static_cast<i32>(skip_target - cbz_idx) * 4;
            m_code_buf[cbz_idx] = arm64::cbz_w(8, off_bytes);
        }
        break;
    }

    // ---- ROL / ROR r/m8, imm8 / 1 ----
    // Replicate the byte across all four lanes then use 32-bit ROR (period 8).
    // Partial-flag: CF (and OF for count==1) only — no FlagOp.
    case Opcode::ROR_RM8_IMM8:
    case Opcode::ROR_RM8_1:
    case Opcode::ROL_RM8_IMM8:
    case Opcode::ROL_RM8_1: {
        const bool is_rol =
            (instr.opcode == Opcode::ROL_RM8_IMM8 ||
             instr.opcode == Opcode::ROL_RM8_1);
        u8 shift = (instr.opcode == Opcode::ROR_RM8_1 ||
                    instr.opcode == Opcode::ROL_RM8_1)
                       ? 1 : static_cast<u8>(op1.imm & 31);
        if (shift == 0) break;

        bool is_mem = !is_register_operand(op0.type);
        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH0, 8));
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH0, 16));
        if (is_rol) {
            u8 ror_amount = static_cast<u8>((32 - shift) & 31);
            emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, ror_amount));
        } else {
            emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));
        }
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC X3, X3, X4
            if (is_rol) {
                // CF = bit 0 of result.
                emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH0, 0x1));
            } else {
                // CF = bit 7 of result.
                emit(0x53071C04u); // UBFX W4, W0, #7, #1
            }
            emit(0xAA040063u);   // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                emit(arm64::movz_w(4, 0x800));
                emit(0x8A240063u);   // BIC X3, X3, X4
                emit(0x53071C02u);   // UBFX W2, W0, #7, #1
                if (is_rol) {
                    emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
                } else {
                    emit(0x53061804u); // UBFX W4, W0, #6, #1
                }
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
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        break;
    }

    // ---- ROL / ROR r/m8, CL ----
    case Opcode::ROR_RM8_CL:
    case Opcode::ROL_RM8_CL: {
        const bool is_rol = (instr.opcode == Opcode::ROL_RM8_CL);
        bool is_mem = !is_register_operand(op0.type);

        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(cpu::RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F)); // AND W8, W1, #0x1F
        u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0);  // CBZ W8, end

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        emit_load_gpr32(REG_SCRATCH1,
                         static_cast<u8>(cpu::RegIndex::RCX));
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));

        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH0, 8));
        emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH0, 16));
        if (is_rol) {
            emit(0x4B0103E1u); // SUB W1, WZR, W1 (NEG W1, W1)
        }
        emit(arm64::rorv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);
            if (is_rol) {
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                emit(0x53071C04u); // UBFX W4, W0, #7, #1
            }
            emit(0xAA040063u);
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);
            emit(arm64::movz_w(4, 0x800));
            emit(0x8A240063u);
            emit(0x53071C02u); // UBFX W2, W0, #7, #1
            if (is_rol) {
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                emit(0x53061804u); // UBFX W4, W0, #6, #1
            }
            emit(0x4A020084u); // EOR W4, W4, W2
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(0x53155084u);
            emit(0xAA040063u);
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (is_mem) {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }

        {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            i32 off_bytes =
                static_cast<i32>(skip_target - cbz_idx) * 4;
            m_code_buf[cbz_idx] = arm64::cbz_w(8, off_bytes);
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
