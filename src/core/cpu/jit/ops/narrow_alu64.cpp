// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: byte and word ALU ops with full
// 64-bit addresses. SHR r/m8,1, CMP/XOR/SUB/AND/OR/TEST r/m8, and
// TEST/CMP/ADD/SUB r/m16.
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
const bool no_alu16 = std::getenv("VITREO_JIT_64BIT_NO_ALU16") != nullptr;
}  // namespace

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_narrow_alu64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // SHR r/m8,1 — CF receives original bit 0, OF receives the original
    // byte's MSB, and ZF/SF/PF describe the byte result. Preserve all other
    // register bits and use a checked full-width address for memory RMW
    // (Intel SDM Vol. 2, SAL/SAR/SHL/SHR).
    case Opcode::SHR_RM8_1: {
        const bool has_rex = instr.prefixes.has_rex();
        const bool dst_is_mem = is_mem(op0);
        if (dst_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, has_rex);
        }
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));
        emit_mov_imm32(REG_SCRATCH1, 1);
        emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        emit_set_shift_lazy_flags(CpuState::FlagOp::Shr8,
                                  REG_SCRATCH4, REG_SCRATCH1,
                                  REG_SCRATCH0);
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

    // ========================================================================
    // 8-bit memory ops with full 64-bit addresses.
    //
    // The standard emit_guest_load8/store8 helpers truncate the address to
    // 32 bits, which is wrong for 64-bit blocks running code that touches
    // memory above 4 GB (the default x64 image base is 0x140000000). The
    // lowering inlines the 64-bit slow-path call to jit_mem_read8 /
    // jit_mem_write8 so the address stays a full 64 bits.
    //
    // This handles the dominant fallback at 0x140095845
    //   42 80 3c 18 0d   CMP byte ptr [rax+r11], 0x0D
    // a memchr('\r') loop, plus other 8-bit reg/imm CMP/MOV/TEST forms.
    // ========================================================================

    // CMP r/m8, imm8 / CMP r/m8, r8 / CMP r8, r/m8 / CMP AL, imm8
    case Opcode::CMP_RM8_IMM8:
    case Opcode::CMP_RM8_R8:
    case Opcode::CMP_R8_RM8:
    case Opcode::CMP_AL_IMM8: {
        // Read an 8-bit memory operand into `dst` through the inline load fast
        // path, which falls back to jit_mem_read8 on a region miss.
        auto read8_to = [&](u8 dst, const Operand& m) {
            emit_compute_ea64(REG_SCRATCH4, m.mem, next_rip);
            emit_guest_load8(dst, REG_SCRATCH4);
        };
        // Load LHS into W0.
        if (is_mem(op0)) {
            read8_to(REG_SCRATCH0, op0);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        // Load RHS into W1, stashing W0 across any BLR.
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1,
                           static_cast<u32>(op1.imm) & 0xFFu);
        } else if (is_mem(op1)) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));   // X21 = W0
            read8_to(REG_SCRATCH1, op1);
            emit(arm64::mov_w(REG_SCRATCH0, REG_CALL_TMP));   // restore
        } else {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        }
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Sub8,
                            REG_SCRATCH1, REG_SCRATCH0);
        break;
    }

    // XOR r8, r/m8 — Intel SDM Vol. 2, "XOR". The selected destination byte
    // is replaced with the bitwise result, all other GPR bits are preserved,
    // and logic flags are derived from the 8-bit result. A memory source uses
    // the checked full-width long-mode address path.
    case Opcode::XOR_R8_RM8: {
        const bool has_rex = instr.prefixes.has_rex();
        // Must go through emit_store_gpr8: a 64-bit block keeps the low eight
        // GPRs in pinned ARM registers, so a byte written straight to CpuState
        // is overwritten by the pin spill at the next helper call or epilogue.
        auto store_reg8 = [&](u8 src, u8 reg) {
            emit_store_gpr8(src, reg, has_rex);
        };
        if (is_mem(op1)) {
            emit_compute_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH4);
        } else {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, has_rex);
        }
        emit_load_gpr8(REG_SCRATCH0, op0.reg, has_rex);
        emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Xor8,
                            REG_SCRATCH1, REG_SCRATCH0);
        store_reg8(REG_SCRATCH0, op0.reg);
        break;
    }

    // SUB/AND/OR r/m8 forms — Intel SDM Vol. 2, "SUB", "AND", and "OR". Compute at
    // byte width, preserve all non-destination GPR bits, and keep memory
    // faults/addressing on the checked full-width long-mode path.
    case Opcode::SUB_AL_IMM8:
    case Opcode::SUB_RM8_IMM8:
    case Opcode::SUB_RM8_R8:
    case Opcode::SUB_R8_RM8:
    case Opcode::AND_AL_IMM8:
    case Opcode::AND_RM8_IMM8:
    case Opcode::AND_RM8_R8:
    case Opcode::AND_R8_RM8:
    case Opcode::OR_AL_IMM8:
    case Opcode::OR_RM8_IMM8:
    case Opcode::OR_RM8_R8:
    case Opcode::OR_R8_RM8: {
        const bool is_sub = instr.opcode == Opcode::SUB_AL_IMM8 ||
                            instr.opcode == Opcode::SUB_RM8_IMM8 ||
                            instr.opcode == Opcode::SUB_RM8_R8 ||
                            instr.opcode == Opcode::SUB_R8_RM8;
        const bool is_or = instr.opcode == Opcode::OR_AL_IMM8 ||
                           instr.opcode == Opcode::OR_RM8_IMM8 ||
                           instr.opcode == Opcode::OR_RM8_R8 ||
                           instr.opcode == Opcode::OR_R8_RM8;
        const bool has_rex = instr.prefixes.has_rex();
        const bool dst_is_mem = is_mem(op0);
        const bool src_is_mem = is_mem(op1);
        // Must go through emit_store_gpr8: a 64-bit block keeps the low eight
        // GPRs in pinned ARM registers, so a byte written straight to CpuState
        // is overwritten by the pin spill at the next helper call or epilogue.
        auto store_reg8 = [&](u8 src, u8 reg) {
            emit_store_gpr8(src, reg, has_rex);
        };
        auto read8_to = [&](u8 dst, const Operand& operand) {
            emit_compute_ea64(REG_SCRATCH4, operand.mem, next_rip);
            emit_guest_load8(dst, REG_SCRATCH4);
        };

        if (src_is_mem) {
            read8_to(REG_SCRATCH1, op1);
            emit_load_gpr8(REG_SCRATCH0, op0.reg, has_rex);
        } else {
            if (dst_is_mem) read8_to(REG_SCRATCH0, op0);
            else emit_load_gpr8(REG_SCRATCH0, op0.reg, has_rex);
            if (is_imm(op1)) {
                emit_mov_imm32(REG_SCRATCH1,
                               static_cast<u32>(op1.imm) & 0xFFu);
            } else {
                emit_load_gpr8(REG_SCRATCH1, op1.reg, has_rex);
            }
        }

        if (is_sub) {
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        } else if (is_or) {
            emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        } else {
            emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        }
        emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(is_sub ? CpuState::FlagOp::Sub8 :
                            is_or ? CpuState::FlagOp::Or8
                                  : CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        if (dst_is_mem) {
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store8(17, REG_SCRATCH4);
        } else {
            store_reg8(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // TEST r/m8, r8 / TEST AL, imm8 / TEST r/m8, imm8
    case Opcode::TEST_RM8_R8:
    case Opcode::TEST_AL_IMM8:
    case Opcode::TEST_RM8_IMM8: {
        auto read8_to = [&](u8 dst, const Operand& m) {
            emit_compute_ea64(REG_SCRATCH4, m.mem, next_rip);
            emit_guest_load8(dst, REG_SCRATCH4);
        };
        if (is_mem(op0)) {
            read8_to(REG_SCRATCH0, op0);
        } else {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        }
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1,
                           static_cast<u32>(op1.imm) & 0xFFu);
        } else {
            emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        }
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        // Host NZCV as TEST r/m32 leaves it: the byte result in bits 31:24
        // gives this width's SF (N) and ZF (Z), and TST clears C and V,
        // TEST's CF and OF (Intel SDM Vol. 2 TEST). A following Jcc then
        // branches on NZCV instead of walking the lazy record's kind.
        emit(arm64::lsl_w_imm(REG_SCRATCH4, REG_SCRATCH0, 24));
        emit(arm64::ands_w(31, REG_SCRATCH4, REG_SCRATCH4));
        emit_set_lazy_flags(CpuState::FlagOp::And8,
                            REG_SCRATCH1, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    // TEST r/m16, r16 / TEST AX, imm16 / TEST r/m16, imm16. The result is
    // used only for flags; neither operand is modified. Memory operands still
    // use a full-width long-mode effective address and the checked read helper.
    case Opcode::TEST_RM16_R16:
    case Opcode::TEST_AX_IMM16:
    case Opcode::TEST_RM16_IMM16: {
        auto read16_to = [&](u8 dst, const Operand& m) {
            emit_compute_ea64(REG_SCRATCH4, m.mem, next_rip);
            emit_guest_load16(dst, REG_SCRATCH4);
        };
        if (is_mem(op0)) {
            read16_to(REG_SCRATCH0, op0);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1,
                           static_cast<u32>(op1.imm) & 0xFFFFu);
        } else {
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        // Host NZCV for this width, as in the TEST r/m8 arm above.
        emit(arm64::lsl_w_imm(REG_SCRATCH4, REG_SCRATCH0, 16));
        emit(arm64::ands_w(31, REG_SCRATCH4, REG_SCRATCH4));
        emit_set_lazy_flags(CpuState::FlagOp::And16,
                            REG_SCRATCH1, REG_SCRATCH0);
        m_flags.nzcv = NzcvState::And;
        break;
    }

    // CMP r/m16, r16 / CMP r16, r/m16 / CMP r/m16, imm16 / CMP AX, imm16 /
    // CMP r/m16, imm8 (sign-extended). Memory operands use the 64-bit
    // address slow-path call to jit_mem_read16.
    case Opcode::CMP_RM16_R16:
    case Opcode::CMP_R16_RM16:
    case Opcode::CMP_RM16_IMM16:
    case Opcode::CMP_RM16_IMM8:
    case Opcode::CMP_AX_IMM16: {
        auto read16_to = [&](u8 dst, const Operand& m) {
            emit_compute_ea64(REG_SCRATCH4, m.mem, next_rip);
            emit_guest_load16(dst, REG_SCRATCH4);
        };
        if (is_mem(op0)) {
            read16_to(REG_SCRATCH0, op0);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1,
                           static_cast<u32>(op1.imm) & 0xFFFFu);
        } else if (is_mem(op1)) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            read16_to(REG_SCRATCH1, op1);
            emit(arm64::mov_w(REG_SCRATCH0, REG_CALL_TMP));
        } else {
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
        emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                            REG_SCRATCH1, REG_SCRATCH0);
        break;
    }

    // ADD/SUB r/m16 forms in a 64-bit block. These are ordinary 16-bit
    // operations (not REX.W), but their effective addresses are still
    // 64-bit. Keep memory access in the narrow GuestMemory helpers so page
    // protection, faults, and executable-write invalidation remain exact.
    case Opcode::ADD_RM16_R16:
    case Opcode::ADD_R16_RM16:
    case Opcode::ADD_RM16_IMM16:
    case Opcode::ADD_RM16_IMM8:
    case Opcode::ADD_AX_IMM16:
    case Opcode::SUB_RM16_R16:
    case Opcode::SUB_R16_RM16:
    case Opcode::SUB_RM16_IMM16:
    case Opcode::SUB_RM16_IMM8:
    case Opcode::SUB_AX_IMM16: {
        if (no_alu16) return false;
        const bool is_add =
            instr.opcode == Opcode::ADD_RM16_R16 ||
            instr.opcode == Opcode::ADD_R16_RM16 ||
            instr.opcode == Opcode::ADD_RM16_IMM16 ||
            instr.opcode == Opcode::ADD_RM16_IMM8 ||
            instr.opcode == Opcode::ADD_AX_IMM16;
        const bool dst_is_mem = is_mem(op0);
        const bool src_is_mem = is_mem(op1);

        auto read16_to = [&](u8 dst, const Operand& operand) {
            emit_compute_ea64(REG_SCRATCH4, operand.mem, next_rip);
            emit_guest_load16_64(dst, REG_SCRATCH4);
        };

        if (dst_is_mem) {
            // X21 is callee-saved, so the destination EA survives both the
            // read helper and the later write helper.
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH4));
            emit_guest_load16_64(REG_SCRATCH0, REG_CALL_TMP);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }

        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1,
                           static_cast<u32>(op1.imm) & 0xFFFFu);
        } else if (src_is_mem) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            read16_to(REG_SCRATCH1, op1);
            emit(arm64::mov_w(REG_SCRATCH0, REG_CALL_TMP));
        } else {
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
        }

        if (is_add) {
            emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
            emit_set_lazy_flags(CpuState::FlagOp::Add16,
                                REG_SCRATCH1, REG_SCRATCH0);
        } else {
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
            emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                                REG_SCRATCH1, REG_SCRATCH0);
        }

        if (dst_is_mem) {
            emit_guest_store16_64(REG_SCRATCH0, REG_CALL_TMP);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
