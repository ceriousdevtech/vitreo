// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — lane insert/extract: PINSRW, PEXTRW, EXTRACTPS, PEXTRB/D/Q,
// PINSRB/D/Q and INSERTPS.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_sse_insert_extract(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- PINSRW xmm, r32/m16, imm8 ----
    case Opcode::PINSRW_XMM_R32M16_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 7;
        u16 packed_dst = static_cast<u16>(dst_xmm | (sel << 8));
        // Native: INS Vd.H[sel], Wn is the whole instruction — insert the low
        // 16 bits of the source and leave every other lane alone (Intel SDM
        // Vol. 2 PINSRW). Both forms were calling jit_sse_op, which runs the
        // interpreter's own semantics for what is a single ARM instruction.
        {
            const u32 off_d = vec_off(dst_xmm);
            if (fits_ldr_q_offset(off_d)) {
                if (is_register_operand(op1.type)) {
                    emit_load_gpr32(REG_SCRATCH0, op1.reg);
                } else {
                    // Source first: emit_guest_load16 can BLR and v0-v7 are
                    // caller-saved.
                    emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                    emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                    emit_guest_load16(REG_SCRATCH0, REG_SCRATCH4);
                }
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                emit(arm64::ins_v_gpr(0, sel, REG_SCRATCH0, arm64::VS_H));
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
                break;
            }
        }
        if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 190));
            emit(arm64::movz_w(3, packed_dst));
            emit(arm64::movz_w(4, op1.reg & 15));
            EMIT_SSE_OP_CALL();
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::movz_w(2, 191));
            emit(arm64::movz_w(3, packed_dst));
            emit(arm64::mov_x(4, REG_SCRATCH4));
            EMIT_SSE_OP_CALL();
        }
        break;
    }

    // ---- PEXTRW r32/m16, xmm, imm8 ----
    //
    // Two encodings share this opcode. The legacy `66 0F C5 /r ib` form has a
    // register destination; the SSE4.1 `66 0F 3A 15 /r ib` form allows a
    // MEMORY destination and stores 2 bytes (Intel SDM Vol. 2, PEXTRW).
    //
    // This arm used to read `op0.reg` unconditionally. On the memory form that
    // reads a register index out of a MemoryOperand union and then writes the
    // extracted word into whatever GPR that produced — clobbering an unrelated
    // register while never storing to memory at all. Found by the ISA matrix
    // sweep: the JIT wrote EAX where the interpreter correctly left it alone.
    case Opcode::PEXTRW_R32_XMM_IMM8: {
        const u8 sel = static_cast<u8>(instr.operands[2].imm) & 7;
        if (is_memory_operand(op0.type)) {
            // The selected word lives at a fixed CpuState offset, so this is a
            // halfword load and a 2-byte guest store — no helper needed.
            const u32 off = xmm_u64_off(op1.reg & 15, 0) + sel * 2u;
            if (m_vec_hi_lane || (off & 1u) != 0u || (off >> 1) > 0xFFFu) {
                return false;
            }
            emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit(arm64::ldrh_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 1));
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH4);
            break;
        }
        {
            // Register destination: the word, zero-extended into the GPR
            // (Intel SDM Vol. 2 PEXTRW: "The upper bits of r32 or r64 are
            // zeroed"). A 32-bit GPR write is exactly that in both modes.
            const u32 off = xmm_u64_off(op1.reg & 15, 0) + sel * 2u;
            if (!m_vec_hi_lane && (off & 1u) == 0u && (off >> 1) <= 0xFFFu) {
                emit(arm64::ldrh_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 1));
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
                break;
            }
        }
        const u16 packed_src = static_cast<u16>((op1.reg & 15) | (sel << 8));
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::movz_w(2, 192));
        emit(arm64::movz_w(3, op0.reg & 15));
        emit(arm64::movz_w(4, packed_src));
        EMIT_SSE_OP_CALL();
        break;
    }

    // ---- EXTRACTPS r/m32, xmm, imm8 / PEXTRD r/m32, xmm, imm8 ----
    // Intel SDM Vol. 2 EXTRACTPS/PEXTRD: DEST := SRC[imm8[1:0]*32 +: 32];
    // a register destination is zero-extended into r64, an m32 destination
    // has no alignment requirement. VEX.128 forms arrive bridged (no NDS, no
    // XMM destination). The memory forms used to decline in long mode.
    case Opcode::EXTRACTPS_RM32_XMM_IMM8:
    case Opcode::PEXTRD_RM32_XMM_IMM8: {
        u8 src_xmm = op1.reg & 15;
        u32 off_s = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(off_s)) return false;
        u8 lane = static_cast<u8>(instr.operands[2].imm) & 3;
        if (is_register_operand(op0.type)) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
            emit(arm64::umov_w_s(REG_SCRATCH0, 0, lane));
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
            break;
        }
        if (!is_memory_operand(op0.type)) return false;
        // The address first: the lane is read into a scratch the long-mode
        // EA computation is free to use.
        emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
        emit(arm64::umov_w_s(REG_SCRATCH0, 0, lane));
        emit_guest_store32(REG_SCRATCH0, REG_SCRATCH4);
        break;
    }

    // ---- PEXTRQ r64, xmm, imm8 — extract D lane (reg dest only) ----
    case Opcode::PEXTRQ_RM64_XMM_IMM8: {
        if (!is_register_operand(op0.type)) return false;
        u8 src_xmm = op1.reg & 15;
        u32 off_s = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(off_s)) return false;
        u8 lane = static_cast<u8>(instr.operands[2].imm) & 1;
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
        emit(arm64::umov_x_d(REG_SCRATCH0, 0, lane));
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- PEXTRB r32/m8, xmm, imm8 — extract B lane (Intel SDM Vol. 2) ----
    case Opcode::PEXTRB_RM8_XMM_IMM8: {
        u8 src_xmm = op1.reg & 15;
        u32 off_s = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(off_s)) return false;
        u8 lane = static_cast<u8>(instr.operands[2].imm) & 0xF;
        if (is_register_operand(op0.type)) {
            // PEXTRB writes the byte zero-extended into the 32-bit GPR.
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
            emit(arm64::umov_w_b(REG_SCRATCH0, 0, lane));
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
            break;
        }
        if (!is_memory_operand(op0.type)) return false;
        emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_s >> 4));
        emit(arm64::umov_w_b(REG_SCRATCH0, 0, lane));
        emit_guest_store8(REG_SCRATCH0, REG_SCRATCH4);
        break;
    }

    // ---- PINSRD xmm, r32, imm8 — insert dword from GPR (reg src only) ----
    case Opcode::PINSRD_XMM_RM32_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        u8 lane = static_cast<u8>(instr.operands[2].imm) & 3;
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        emit(arm64::ins_v_gpr(0, lane, REG_SCRATCH0, arm64::VS_S));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ---- PINSRQ xmm, r/m64, imm8 — insert qword from GPR or m64 ----
    //
    // Intel SDM Vol. 2 PINSRQ: DEST[imm8[0]*64 +: 64] := SRC; the memory form
    // is an m64 with no alignment requirement (the legacy 16-byte rule covers
    // 128-bit operands only). The qword is loaded into a GPR before V0 is
    // formed from CpuState, so nothing vector-typed is live across the guest
    // load's possible helper BLR. VEX.128 VPINSRQ arrives through the bridge
    // (vvvv merge + upper zero already emitted); VEX.L=1 is #UD and stays
    // with the delegate, which raises it.
    case Opcode::PINSRQ_XMM_RM64_IMM8: {
        const bool mem_src = !is_register_operand(op1.type);
        if (mem_src && !is_memory_operand(op1.type)) return false;
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        u8 lane = static_cast<u8>(instr.operands[2].imm) & 1;
        if (mem_src) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        emit(arm64::ins_v_gpr(0, lane, REG_SCRATCH0, arm64::VS_D));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ---- PINSRB xmm, r32, imm8 — insert byte from GPR (reg src only) ----
    case Opcode::PINSRB_XMM_RM8_IMM8: {
        u8 dst_xmm = op0.reg & 15;
        u32 off_d = xmm_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d)) return false;
        u8 lane = static_cast<u8>(instr.operands[2].imm) & 0xF;
        // PINSRB reads the low byte of r/m8 (use 32-bit load; INS picks low 8).
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH4);
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        emit(arm64::ins_v_gpr(0, lane, REG_SCRATCH0, arm64::VS_B));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        break;
    }

    // ---- INSERTPS xmm, xmm/m32, imm8 / VINSERTPS xmm, xmm, xmm/m32, imm8 ----
    // Intel SDM Vol. 2 INSERTPS: COUNT_S = imm8[7:6] for a register source
    // and 0 for m32, COUNT_D = imm8[5:4], ZMASK = imm8[3:0]; every lane the
    // imm8 does not name comes from SRC1 -- the destination for the legacy
    // form, VEX.vvvv for VINSERTPS, which also zeroes DEST[MAXVL-1:128]. The
    // m32 operand has no alignment requirement and raises no SIMD
    // floating-point exception. The memory form used to decline in long
    // mode, which made it 72% of all delegated instructions in a 64-bit title
    // session.
    case Opcode::INSERTPS_XMM_XMMM32_IMM8: {
        const bool vex = instr.prefixes.vex;
        const u8 dst_xmm = op0.reg & 15;
        const u8 src1_xmm = vex ? (instr.prefixes.vex_vvvv & 15) : dst_xmm;
        const u32 off_d = xmm_u64_off(dst_xmm, 0);
        const u32 off_1 = xmm_u64_off(src1_xmm, 0);
        const u32 off_dhi = ymm_high_u64_off(dst_xmm, 0);
        if (!fits_ldr_q_offset(off_d) || !fits_ldr_q_offset(off_1) ||
            (vex && !fits_ldr_q_offset(off_dhi))) {
            return false;
        }
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 src_lane = (imm >> 6) & 3;
        const u8 dst_lane = (imm >> 4) & 3;
        const u8 zmask = imm & 0xF;
        const bool mem_src = !is_register_operand(op1.type);
        if (mem_src && !is_memory_operand(op1.type)) return false;
        // Source before destination: emit_guest_load* may BLR, and v0-v7 are
        // caller-saved, so a destination staged in V0 first does not survive.
        // INSERTPS keeps every lane the imm8 does not name, and those were
        // coming back as helper leftovers. Same defect as CMPSS/CMPSD.
        if (!mem_src) {
            const u32 off_s = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_q_offset(off_s)) return false;
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, off_s >> 4));
        } else {
            src_lane = 0;
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::ins_v_gpr(1, 0, REG_SCRATCH0, arm64::VS_S));
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_1 >> 4));
        emit(arm64::ins_v_elem(0, dst_lane, 1, src_lane, arm64::VS_S));
        for (u8 i = 0; i < 4; ++i) {
            if (zmask & (1u << i)) {
                emit(arm64::ins_v_gpr(0, i, 31, arm64::VS_S));   // WZR → zero lane
            }
        }
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_d >> 4));
        if (vex) {
            emit(arm64::movi_v16b(2, 0));
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, off_dhi >> 4));
        }
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
