// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_locked's 32-bit and width-implicit LOCK forms:
// INC/DEC/NOT/NEG, XADD, XCHG, OR/AND/ADD/SUB/ADC/SBB/XOR r/m32,
// CMPXCHG r/m32 and r/m8, CMPXCHG8B and CMPXCHG16B.
//
// One family of compile_locked's opcode space, split out of
// ops/memory_ops.cpp as pure code motion: the case arms are verbatim and in
// their original order, and emit_locked_ea is compile_locked's lambda,
// copied verbatim. compile_locked (ops/memory_ops.cpp) owns the LOCK_DIAG
// preamble, the dispatch chain, the unsupported-opcode diagnostic and the
// block tail.

#include "../jit_helpers.hpp"
#include "core/cpu/lock_prefix.hpp"

#include <atomic>
#include <optional>

namespace vitreo::cpu::jit {

// nullopt = not in this family; false = declined; true = the arm reached
// `break`. See jit_compiler_lowering.hpp.
std::optional<bool> JitCompiler::compile_locked_rmw32(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    const auto emit_locked_ea = [&](u8 dst_arm_reg, const MemoryOperand& mem) {
        if (m_block_is_64bit) {
            emit_compute_ea64(dst_arm_reg, mem, next_rip);
        } else {
            emit_compute_ea(dst_arm_reg, mem, next_rip);
        }
    };

    switch (instr.opcode) {

    case Opcode::INC_RM32: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Inc, 4, op0.mem,
                                   next_rip, nullptr);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_LOCK_INC32 >> 3));
        emit_helper_blr();  // BLR X16
        join_native_lock(nl);
        break;
    }

    case Opcode::DEC_RM32: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Dec, 4, op0.mem,
                                   next_rip, nullptr);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_LOCK_DEC32 >> 3));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::XADD_RM32_R32: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Xadd, 4, op0.mem,
                                   next_rip, &op1);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::movz_w(3, static_cast<u16>(op1.reg)));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_LOCK_XADD32 >> 3));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::CMPXCHG_RM32_R32: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Cmpxchg, 4, op0.mem,
                                   next_rip, &op1);
        emit_load_gpr32(REG_SCRATCH1, op1.reg);
        emit(arm64::mov_w(10, REG_SCRATCH1));
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_LOCK_CMPXCHG32 >> 3));
        emit_helper_blr();
        // The helper materialises RFLAGS; both arms now leave CMP's NZCV.
        emit_nzcv_sub_from_rflags();
        join_native_lock(nl);
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    case Opcode::CMPXCHG_RM8_R8: {
        // LOCK CMPXCHG [byte], 1 on a UI-thread work flag
        // (Intel SDM Vol. 2: CMPXCHG r/m8 + LOCK).
        if (is_register_operand(op0.type)) return false;
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit(arm64::mov_w(10, REG_SCRATCH1));
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_LOCK_CMPXCHG8 >> 3));
        emit_helper_blr();
        // The helper materialises RFLAGS: claim CMP's NZCV from them.
        emit_nzcv_sub_from_rflags();
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    case Opcode::CMPXCHG8B_M64: {
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_LOCK_CMPXCHG8B >> 3));
        emit_helper_blr();
        break;
    }

    case Opcode::CMPXCHG16B_M128: {
        // Intel SDM Vol. 2 CMPXCHG16B + LOCK: atomic 128-bit CAS of RCX:RBX
        // into 16-byte-aligned m128. Immediate helper address — no extra
        // JitContext slot.
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_cmpxchg16b)));
        emit_helper_blr();
        emit_exit_if_pending_raise();
        break;
    }

    // ---- LOCK XCHG r/m32, r32 ----
    //
    // The same lowering `compile_memory_ops` has always used for the
    // unprefixed form: `jit_atomic_xchg32` is a single seq_cst host exchange
    // when the operand is aligned and mapped, and takes the exclusive path
    // itself otherwise. Intel SDM Vol. 2 XCHG makes the memory form atomic
    // with or without F0, so the LOCK form needs no semantics the unprefixed
    // lowering lacks — `compile_locked` simply had no arm for it, and a miss
    // here falls through to the per-instruction interpreter delegate.
    //
    // That delegate is why this matters far beyond one missing lowering. Its
    // dispatcher-level `LockPrefixGuard` is constructed with no operand
    // information, so it takes the process-wide exclusive mutex and spins over
    // every thread's reader slot — for an operand that needed none of it.
    // Measured 2026-09-12, reproduced n=2: ONE such guest
    // instruction executed 4.2M+ times in 70 s and produced 7.48M of the 7.50M
    // exclusive LOCK acquisitions in the busiest process, against 21 that were
    // genuinely unaligned.
    //
    // 8- and 16-bit XCHG still delegate: they have no atomic helper to call,
    // and measured three orders of magnitude below this form.
    case Opcode::XCHG_RM32_R32: {
        // LOCK with a register destination is #UD (Intel SDM Vol. 2 LOCK), and
        // an unprefixed reg,reg XCHG needs no atomicity. Decline both and let
        // the existing path decide, rather than changing that behaviour here.
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Xchg, 4, op0.mem,
                                   next_rip, &op1);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::movz_w(3, static_cast<u16>(op1.reg)));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_ATOMIC_XCHG32 >> 3));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::OR_RM32_R32:
    case Opcode::OR_RM32_IMM8:
    case Opcode::OR_RM32_IMM32:
    case Opcode::AND_RM32_R32:
    case Opcode::AND_RM32_IMM8:
    case Opcode::AND_RM32_IMM32:
    case Opcode::ADD_RM32_R32:
    case Opcode::ADD_RM32_IMM8:
    case Opcode::ADD_RM32_IMM32:
    case Opcode::SUB_RM32_R32:
    case Opcode::SUB_RM32_IMM8:
    case Opcode::SUB_RM32_IMM32: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        {
            NativeLockKind kind = NativeLockKind::Or;
            switch (instr.opcode) {
            case Opcode::AND_RM32_R32: case Opcode::AND_RM32_IMM8:
            case Opcode::AND_RM32_IMM32: kind = NativeLockKind::And; break;
            case Opcode::ADD_RM32_R32: case Opcode::ADD_RM32_IMM8:
            case Opcode::ADD_RM32_IMM32: kind = NativeLockKind::Add; break;
            case Opcode::SUB_RM32_R32: case Opcode::SUB_RM32_IMM8:
            case Opcode::SUB_RM32_IMM32: kind = NativeLockKind::Sub; break;
            default: break;
            }
            (void)emit_native_lock_rmw(nl, kind, 4, op0.mem, next_rip, &op1);
        }
        const bool is_reg_src =
            instr.opcode == Opcode::OR_RM32_R32 ||
            instr.opcode == Opcode::AND_RM32_R32 ||
            instr.opcode == Opcode::ADD_RM32_R32 ||
            instr.opcode == Opcode::SUB_RM32_R32;
        if (is_reg_src) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::mov_w(10, REG_SCRATCH1));
        } else {
            emit_mov_imm32(10, static_cast<u32>(op1.imm));
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        u32 helper_off = JIT_CTX_OFF_LOCK_OR32;
        switch (instr.opcode) {
        case Opcode::AND_RM32_R32:
        case Opcode::AND_RM32_IMM8:
        case Opcode::AND_RM32_IMM32:
            helper_off = JIT_CTX_OFF_LOCK_AND32;
            break;
        case Opcode::ADD_RM32_R32:
        case Opcode::ADD_RM32_IMM8:
        case Opcode::ADD_RM32_IMM32:
            helper_off = JIT_CTX_OFF_LOCK_ADD32;
            break;
        case Opcode::SUB_RM32_R32:
        case Opcode::SUB_RM32_IMM8:
        case Opcode::SUB_RM32_IMM32:
            helper_off = JIT_CTX_OFF_LOCK_SUB32;
            break;
        default:
            break;
        }
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::ADC_RM32_R32:
    case Opcode::ADC_RM32_IMM8:
    case Opcode::ADC_RM32_IMM32:
    case Opcode::SBB_RM32_R32:
    case Opcode::SBB_RM32_IMM8:
    case Opcode::SBB_RM32_IMM32: {
        // PE32 LOCK ADC/SBB — immediate helper BLR (no new JitContext slots).
        if (is_register_operand(op0.type)) return false;
        const bool is_reg_src =
            instr.opcode == Opcode::ADC_RM32_R32 ||
            instr.opcode == Opcode::SBB_RM32_R32;
        if (is_reg_src) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::mov_w(10, REG_SCRATCH1));
        } else {
            emit_mov_imm32(10, static_cast<u32>(op1.imm));
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        void* helper = reinterpret_cast<void*>(&jit_lock_adc32);
        switch (instr.opcode) {
        case Opcode::SBB_RM32_R32:
        case Opcode::SBB_RM32_IMM8:
        case Opcode::SBB_RM32_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_sbb32);
            break;
        default:
            break;
        }
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(helper)));
        emit_helper_blr();
        break;
    }

    // ---- LOCK XOR r/m32 ----
    //
    // OR, AND, ADD and SUB r/m32 are handled above and XOR r/m64 below, but the
    // 32-bit XOR was never wired, so it fell to the interpreter delegate and its
    // operand-less process-wide LockPrefixGuard. Same wiring-gap class as
    // XCHG_RM32_R32.
    case Opcode::XOR_RM32_R32:
    case Opcode::XOR_RM32_IMM8:
    case Opcode::XOR_RM32_IMM32: {
        if (is_register_operand(op0.type)) return false;
        if (instr.opcode == Opcode::XOR_RM32_R32) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::mov_w(10, REG_SCRATCH1));
        } else {
            // Decoder already sign-extends IMM8 into op1.imm; truncating to 32
            // bits is what makes `lock xor dword ptr [x], -1` flip all 32 bits.
            emit_mov_imm32(10, static_cast<u32>(op1.imm));
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_xor32)));
        emit_helper_blr();
        break;
    }

    // ---- LOCK NOT / NEG r/m32 ----
    //
    // The r/m64 twins are above. Both are single-operand, so there is nothing
    // to load before the EA.
    case Opcode::NOT_RM32: {
        if (is_register_operand(op0.type)) return false;
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_not32)));
        emit_helper_blr();
        break;
    }

    case Opcode::NEG_RM32: {
        if (is_register_operand(op0.type)) return false;
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_neg32)));
        emit_helper_blr();
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
