// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_memory_ops' BMI1/BMI2 GPR ops: PDEP/PEXT, the BMI1
// flag writers ANDN/BLSI/BLSMSK/BLSR/BZHI/BEXTR, and the BMI2 flag-neutral
// SHLX/SHRX/SARX/RORX/MULX. ADX and MOVBE, which sat between PDEP/PEXT and
// BMI1 in the original switch, stay in ops/memory_ops.cpp.
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
std::optional<bool> JitCompiler::compile_memory_ops_bmi(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- PDEP / PEXT ------------------------------------------------------
    //
    // No ARM64 equivalent, so these call a helper (jit_pdep64 / jit_pext64).
    // Neither writes EFLAGS, so nothing pending may be retired here.
    // DEST = ModRM.reg, SRC = VEX.vvvv, MASK = r/m.
    case Opcode::PDEP_R32_R32_RM32:
    case Opcode::PDEP_R64_R64_RM64:
    case Opcode::PEXT_R32_R32_RM32:
    case Opcode::PEXT_R64_R64_RM64: {
        const bool w64 = instr.opcode == Opcode::PDEP_R64_R64_RM64 ||
                         instr.opcode == Opcode::PEXT_R64_R64_RM64;
        const bool is_pdep = instr.opcode == Opcode::PDEP_R32_R32_RM32 ||
                             instr.opcode == Opcode::PDEP_R64_R64_RM64;
        const u8 src_reg = instr.prefixes.vex_vvvv & 15;

        // Mask (r/m) into X1, source (vvvv) into X0 — the helper's arguments.
        // The 32-bit forms hand over zero-extended halves and keep the low 32
        // bits of the result, so one 64-bit helper serves both widths.
        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH1, op1.reg);
            else emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else if (m_block_is_64bit) {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            if (w64) emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
            else emit_guest_load32_64(REG_SCRATCH1, REG_SCRATCH2);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        if (w64) emit_load_gpr64(REG_SCRATCH0, src_reg);
        else emit_load_gpr32(REG_SCRATCH0, src_reg);
        if (!w64) {
            emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH1));
        }

        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                               (is_pdep ? JIT_CTX_OFF_PDEP64
                                        : JIT_CTX_OFF_PEXT64) >> 3));
        emit_helper_blr();  // X0 = result

        if (w64) emit_store_gpr64(0, op0.reg);
        else emit_store_gpr32(0, op0.reg);
        break;
    }

    // ---- BMI1 flag writers + BZHI ---------------------------------------
    //
    // All of these set SF/ZF from the result, define CF per instruction, and
    // clear OF. AF and PF are undefined (SF too, for BEXTR), so
    // emit_bmi_result_flags leaves them alone.
    case Opcode::ANDN_R32_R32_RM32:
    case Opcode::ANDN_R64_R64_RM64: {
        const bool w64 = instr.opcode == Opcode::ANDN_R64_R64_RM64;
        const u8 src1 = instr.prefixes.vex_vvvv & 15;
        // DEST := NOT(SRC1) AND SRC2 — ARM64 BIC is exactly that.
        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op1.reg);
            else emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }
        if (w64) emit_load_gpr64(REG_SCRATCH1, src1);
        else emit_load_gpr32(REG_SCRATCH1, src1);
        emit(w64 ? arm64::bic_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)
                 : arm64::bic_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        if (w64) emit_store_gpr64(REG_SCRATCH0, op0.reg);
        else emit_store_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::movz_w(REG_SCRATCH1, 0));  // CF := 0
        emit_bmi_result_flags(REG_SCRATCH0, REG_SCRATCH1, w64);
        break;
    }

    // BLSI  DEST := (-SRC) AND SRC     CF := (SRC != 0)
    // BLSMSK DEST := (SRC-1) XOR SRC   CF := (SRC == 0)
    // BLSR  DEST := (SRC-1) AND SRC    CF := (SRC == 0)
    // Destination is VEX.vvvv; the decoder gives only the r/m source operand.
    case Opcode::BLSI_R32_RM32:
    case Opcode::BLSI_R64_RM64:
    case Opcode::BLSMSK_R32_RM32:
    case Opcode::BLSMSK_R64_RM64:
    case Opcode::BLSR_R32_RM32:
    case Opcode::BLSR_R64_RM64: {
        const bool w64 = instr.opcode == Opcode::BLSI_R64_RM64 ||
                         instr.opcode == Opcode::BLSMSK_R64_RM64 ||
                         instr.opcode == Opcode::BLSR_R64_RM64;
        const bool is_blsi = instr.opcode == Opcode::BLSI_R32_RM32 ||
                             instr.opcode == Opcode::BLSI_R64_RM64;
        const bool is_blsmsk = instr.opcode == Opcode::BLSMSK_R32_RM32 ||
                               instr.opcode == Opcode::BLSMSK_R64_RM64;
        const u8 dst = instr.prefixes.vex_vvvv & 15;

        if (is_register_operand(op0.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op0.reg);
            else emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }

        // CF is defined against the *source*, so capture it before the source
        // register is reused for the result.
        emit(w64 ? arm64::cmp_x_imm(REG_SCRATCH0, 0)
                 : arm64::cmp_w_imm(REG_SCRATCH0, 0));
        emit(arm64::cset_w(REG_SCRATCH1, is_blsi ? arm64::NE : arm64::EQ));

        if (is_blsi) {
            // -SRC via SUB from the zero register.
            emit(w64 ? arm64::sub_x(REG_SCRATCH4, 31, REG_SCRATCH0)
                     : arm64::sub_w(REG_SCRATCH4, 31, REG_SCRATCH0));
            emit(w64 ? arm64::and_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH0)
                     : arm64::and_w(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH0));
        } else {
            emit(w64 ? arm64::sub_x_imm(REG_SCRATCH4, REG_SCRATCH0, 1)
                     : arm64::sub_w_imm(REG_SCRATCH4, REG_SCRATCH0, 1));
            if (is_blsmsk) {
                emit(w64 ? arm64::eor_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH0)
                         : arm64::eor_w(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH0));
            } else {
                emit(w64 ? arm64::and_x(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH0)
                         : arm64::and_w(REG_SCRATCH0, REG_SCRATCH4, REG_SCRATCH0));
            }
        }

        if (w64) emit_store_gpr64(REG_SCRATCH0, dst);
        else emit_store_gpr32(REG_SCRATCH0, dst);
        emit_bmi_result_flags(REG_SCRATCH0, REG_SCRATCH1, w64);
        break;
    }

    // BZHI DEST := SRC with all bits at index N and above cleared, where
    // N = index[7:0]. N >= operand size leaves SRC untouched, and CF is
    // exactly that "N was out of range" condition (Intel SDM Vol. 2 BZHI).
    case Opcode::BZHI_R32_RM32_R32:
    case Opcode::BZHI_R64_RM64_R64: {
        const bool w64 = instr.opcode == Opcode::BZHI_R64_RM64_R64;
        const u32 opsize = w64 ? 64u : 32u;
        const u8 idx_reg = instr.prefixes.vex_vvvv & 15;

        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op1.reg);
            else emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }
        // N = index[7:0]; the upper bits of the index register never
        // participate, so mask before comparing against the operand size.
        emit_load_gpr32(REG_SCRATCH2, idx_reg);
        emit_mov_imm32(REG_SCRATCH4, 0xFFu);
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));

        // mask = (1 << N) - 1. ARM's LSLV masks N to 5/6 bits, so the value
        // is only meaningful for N < opsize -- the CSEL below discards it
        // otherwise.
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(w64 ? arm64::lslv_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2)
                 : arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));
        emit(w64 ? arm64::sub_x_imm(REG_SCRATCH4, REG_SCRATCH4, 1)
                 : arm64::sub_w_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit(w64 ? arm64::and_x(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH4)
                 : arm64::and_w(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH4));

        // N >= opsize: DEST := SRC, CF := 1.
        emit(arm64::cmp_w_imm(REG_SCRATCH2, opsize));
        emit(arm64::cset_w(REG_SCRATCH1, arm64::CS));
        emit(w64 ? arm64::csel_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4, arm64::CS)
                 : arm64::csel_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4, arm64::CS));

        if (w64) emit_store_gpr64(REG_SCRATCH0, op0.reg);
        else emit_store_gpr32(REG_SCRATCH0, op0.reg);
        emit_bmi_result_flags(REG_SCRATCH0, REG_SCRATCH1, w64);
        break;
    }

    // BEXTR DEST := (SRC >> START) AND ((1 << LEN) - 1), with START and LEN
    // taken from control[7:0] and control[15:8]. A START at or past the
    // operand size yields zero; a LEN at or past it applies no mask at all.
    // CF and OF are cleared, ZF is from the result, SF is undefined.
    case Opcode::BEXTR_R32_RM32_R32:
    case Opcode::BEXTR_R64_RM64_R64: {
        const bool w64 = instr.opcode == Opcode::BEXTR_R64_RM64_R64;
        const u32 opsize = w64 ? 64u : 32u;
        const u8 ctl_reg = instr.prefixes.vex_vvvv & 15;

        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op1.reg);
            else emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }
        emit_load_gpr32(REG_SCRATCH1, ctl_reg);
        emit_mov_imm32(REG_SCRATCH4, 0xFFu);
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH1, REG_SCRATCH4));   // START
        emit(arm64::lsr_w_imm(REG_SCRATCH1, REG_SCRATCH1, 8));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));   // LEN

        // Shift right by START, then force zero when START >= opsize (ARM
        // would otherwise have masked the count and shifted by START % opsize).
        emit(w64 ? arm64::lsrv_x(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH2)
                 : arm64::lsrv_w(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH2));
        emit(arm64::cmp_w_imm(REG_SCRATCH2, opsize));
        emit(w64 ? arm64::csel_x(REG_SCRATCH0, 31, REG_SCRATCH4, arm64::CS)
                 : arm64::csel_w(REG_SCRATCH0, 31, REG_SCRATCH4, arm64::CS));

        // mask = (1 << LEN) - 1, or all ones when LEN >= opsize.
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(w64 ? arm64::lslv_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1)
                 : arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit(w64 ? arm64::sub_x_imm(REG_SCRATCH4, REG_SCRATCH4, 1)
                 : arm64::sub_w_imm(REG_SCRATCH4, REG_SCRATCH4, 1));
        emit(w64 ? arm64::and_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH4)
                 : arm64::and_w(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH4));
        emit(arm64::cmp_w_imm(REG_SCRATCH1, opsize));
        emit(w64 ? arm64::csel_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3, arm64::CS)
                 : arm64::csel_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH3, arm64::CS));

        if (w64) emit_store_gpr64(REG_SCRATCH0, op0.reg);
        else emit_store_gpr32(REG_SCRATCH0, op0.reg);
        emit(arm64::movz_w(REG_SCRATCH1, 0));  // CF := 0
        emit_bmi_result_flags(REG_SCRATCH0, REG_SCRATCH1, w64);
        break;
    }

    // ---- BMI2 flag-neutral data ops -------------------------------------
    //
    // SHLX/SHRX/SARX r32a, r/m32, r32b: shift r/m by the count in VEX.vvvv,
    // leaving EFLAGS completely untouched -- including for a zero count, and
    // including CF, which is what separates them from SHL/SHR/SAR. ARM64's
    // LSLV/LSRV/ASRV read the count from the low 5 bits of Rm (6 for the X
    // form), matching x86's masking exactly, so no explicit AND is needed.
    //
    // Because none of these touch flags, they must NOT clear a pending lazy
    // FlagOp or the CF/OF overlays: a carry chain like `ADD; SHRX; ADC` has to
    // see the ADD's carry.
    case Opcode::SHLX_R32_RM32_R32:
    case Opcode::SHRX_R32_RM32_R32:
    case Opcode::SARX_R32_RM32_R32:
    case Opcode::SHLX_R64_RM64_R64:
    case Opcode::SHRX_R64_RM64_R64:
    case Opcode::SARX_R64_RM64_R64: {
        const bool w64 = instr.opcode == Opcode::SHLX_R64_RM64_R64 ||
                         instr.opcode == Opcode::SHRX_R64_RM64_R64 ||
                         instr.opcode == Opcode::SARX_R64_RM64_R64;
        const u8 count_reg = instr.prefixes.vex_vvvv & 15;

        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op1.reg);
            else emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }
        if (w64) emit_load_gpr64(REG_SCRATCH1, count_reg);
        else emit_load_gpr32(REG_SCRATCH1, count_reg);

        switch (instr.opcode) {
        case Opcode::SHLX_R32_RM32_R32:
            emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)); break;
        case Opcode::SHRX_R32_RM32_R32:
            emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)); break;
        case Opcode::SARX_R32_RM32_R32:
            emit(arm64::asrv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)); break;
        case Opcode::SHLX_R64_RM64_R64:
            emit(arm64::lslv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)); break;
        case Opcode::SHRX_R64_RM64_R64:
            emit(arm64::lsrv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)); break;
        default:
            emit(arm64::asrv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1)); break;
        }

        if (w64) emit_store_gpr64(REG_SCRATCH0, op0.reg);
        else emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // RORX r32, r/m32, imm8 — rotate right by imm8 masked to 5 (or 6) bits.
    // No flags, so again nothing pending may be retired.
    case Opcode::RORX_R32_RM32_IMM8:
    case Opcode::RORX_R64_RM64_IMM8: {
        const bool w64 = instr.opcode == Opcode::RORX_R64_RM64_IMM8;
        if (instr.operand_count < 3) return false;
        const u8 amount =
            static_cast<u8>(instr.operands[2].imm) & (w64 ? 63u : 31u);

        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op1.reg);
            else emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }
        if (amount != 0) {
            emit(w64 ? arm64::ror_x_imm(REG_SCRATCH0, REG_SCRATCH0, amount)
                     : arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, amount));
        }
        // A zero rotate is still a write: the 32-bit form zero-extends into
        // bits 63:32 (Intel SDM Vol. 1 3.4.1.1).
        if (w64) emit_store_gpr64(REG_SCRATCH0, op0.reg);
        else emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // MULX r32a, r32b, r/m32 — unsigned (E)DX * r/m. High half to ModRM.reg,
    // low half to VEX.vvvv, no flags. When both destinations name the same
    // register the SDM says it receives the HIGH half, which falls out of
    // storing low first and high second.
    case Opcode::MULX_R32_R32_RM32:
    case Opcode::MULX_R64_R64_RM64: {
        const bool w64 = instr.opcode == Opcode::MULX_R64_R64_RM64;
        const u8 lo_reg = instr.prefixes.vex_vvvv & 15;
        const u8 rdx = static_cast<u8>(RegIndex::RDX);

        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH0, op1.reg);
            else emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            if (w64) {
                emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
        }

        if (w64) {
            emit_load_gpr64(REG_SCRATCH1, rdx);
            emit(arm64::umulh(REG_SCRATCH3, REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::mul_x(REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH0));
            emit_store_gpr64(REG_SCRATCH4, lo_reg);
            emit_store_gpr64(REG_SCRATCH3, op0.reg);
        } else {
            // MOV Wd, Wn zero-extends into the X register, so a plain 64-bit
            // multiply of the two zero-extended halves gives the full product.
            emit_load_gpr32(REG_SCRATCH1, rdx);
            emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH1));
            emit(arm64::mul_x(REG_SCRATCH3, REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH3, 32));
            emit_store_gpr32(REG_SCRATCH3, lo_reg);
            emit_store_gpr32(REG_SCRATCH4, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
