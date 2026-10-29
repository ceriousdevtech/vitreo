// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_locked's LOCK r/m64 forms: INC/DEC/NOT/NEG, XADD,
// CMPXCHG, XCHG and OR/AND/XOR/ADD/SUB/ADC/SBB r/m64.
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
std::optional<bool> JitCompiler::compile_locked_rmw64(
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

    // ---- LOCK r/m64 (Intel SDM Vol. 2 LOCK) — immediate helper BLR ----
    case Opcode::INC_RM64: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Inc, 8, op0.mem,
                                   next_rip, nullptr);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_inc64)));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::DEC_RM64: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Dec, 8, op0.mem,
                                   next_rip, nullptr);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_dec64)));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::NOT_RM64: {
        if (is_register_operand(op0.type)) return false;
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_not64)));
        emit_helper_blr();
        break;
    }

    case Opcode::NEG_RM64: {
        if (is_register_operand(op0.type)) return false;
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_neg64)));
        emit_helper_blr();
        break;
    }

    case Opcode::XADD_RM64_R64: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Xadd, 8, op0.mem,
                                   next_rip, &op1);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::movz_w(3, static_cast<u16>(op1.reg)));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_xadd64)));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::CMPXCHG_RM64_R64: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Cmpxchg, 8, op0.mem,
                                   next_rip, &op1);
        emit_load_gpr64(REG_SCRATCH1, op1.reg);
        emit(arm64::mov_x(10, REG_SCRATCH1));
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_x(3, 10));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_cmpxchg64)));
        emit_helper_blr();
        // The helper materialises RFLAGS; both arms now leave CMP's NZCV.
        emit_nzcv_sub_from_rflags();
        join_native_lock(nl);
        m_flags.nzcv = NzcvState::Sub;
        break;
    }

    case Opcode::XCHG_RM64_R64: {
        // Intel SDM Vol. 2 XCHG: memory form is locked with or without F0.
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        (void)emit_native_lock_rmw(nl, NativeLockKind::Xchg, 8, op0.mem,
                                   next_rip, &op1);
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::movz_w(3, static_cast<u16>(op1.reg)));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_atomic_xchg64)));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    case Opcode::OR_RM64_R64:
    case Opcode::OR_RM64_IMM8:
    case Opcode::OR_RM64_IMM32:
    case Opcode::AND_RM64_R64:
    case Opcode::AND_RM64_IMM8:
    case Opcode::AND_RM64_IMM32:
    case Opcode::XOR_RM64_R64:
    case Opcode::XOR_RM64_IMM8:
    case Opcode::XOR_RM64_IMM32:
    case Opcode::ADD_RM64_R64:
    case Opcode::ADD_RM64_IMM8:
    case Opcode::ADD_RM64_IMM32:
    case Opcode::SUB_RM64_R64:
    case Opcode::SUB_RM64_IMM8:
    case Opcode::SUB_RM64_IMM32:
    case Opcode::ADC_RM64_R64:
    case Opcode::ADC_RM64_IMM8:
    case Opcode::ADC_RM64_IMM32:
    case Opcode::SBB_RM64_R64:
    case Opcode::SBB_RM64_IMM8:
    case Opcode::SBB_RM64_IMM32: {
        if (is_register_operand(op0.type)) return false;
        NativeLockSite nl;
        {
            NativeLockKind kind = NativeLockKind::Or;
            bool native = true;
            switch (instr.opcode) {
            case Opcode::AND_RM64_R64: case Opcode::AND_RM64_IMM8:
            case Opcode::AND_RM64_IMM32: kind = NativeLockKind::And; break;
            case Opcode::XOR_RM64_R64: case Opcode::XOR_RM64_IMM8:
            case Opcode::XOR_RM64_IMM32: kind = NativeLockKind::Xor; break;
            case Opcode::ADD_RM64_R64: case Opcode::ADD_RM64_IMM8:
            case Opcode::ADD_RM64_IMM32: kind = NativeLockKind::Add; break;
            case Opcode::SUB_RM64_R64: case Opcode::SUB_RM64_IMM8:
            case Opcode::SUB_RM64_IMM32: kind = NativeLockKind::Sub; break;
            case Opcode::OR_RM64_R64: case Opcode::OR_RM64_IMM8:
            case Opcode::OR_RM64_IMM32: break;
            default: native = false; break;  // ADC/SBB: no LSE form
            }
            if (native) {
                (void)emit_native_lock_rmw(nl, kind, 8, op0.mem, next_rip,
                                           &op1);
            }
        }
        const bool is_reg_src =
            instr.opcode == Opcode::OR_RM64_R64 ||
            instr.opcode == Opcode::AND_RM64_R64 ||
            instr.opcode == Opcode::XOR_RM64_R64 ||
            instr.opcode == Opcode::ADD_RM64_R64 ||
            instr.opcode == Opcode::SUB_RM64_R64 ||
            instr.opcode == Opcode::ADC_RM64_R64 ||
            instr.opcode == Opcode::SBB_RM64_R64;
        if (is_reg_src) {
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
            emit(arm64::mov_x(10, REG_SCRATCH1));
        } else {
            // Decoder already sign-extends IMM8/IMM32 into op1.imm.
            emit_mov_imm64(10, static_cast<i64>(op1.imm));
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_x(3, 10));
        void* helper = reinterpret_cast<void*>(&jit_lock_or64);
        switch (instr.opcode) {
        case Opcode::AND_RM64_R64:
        case Opcode::AND_RM64_IMM8:
        case Opcode::AND_RM64_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_and64);
            break;
        case Opcode::XOR_RM64_R64:
        case Opcode::XOR_RM64_IMM8:
        case Opcode::XOR_RM64_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_xor64);
            break;
        case Opcode::ADD_RM64_R64:
        case Opcode::ADD_RM64_IMM8:
        case Opcode::ADD_RM64_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_add64);
            break;
        case Opcode::SUB_RM64_R64:
        case Opcode::SUB_RM64_IMM8:
        case Opcode::SUB_RM64_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_sub64);
            break;
        case Opcode::ADC_RM64_R64:
        case Opcode::ADC_RM64_IMM8:
        case Opcode::ADC_RM64_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_adc64);
            break;
        case Opcode::SBB_RM64_R64:
        case Opcode::SBB_RM64_IMM8:
        case Opcode::SBB_RM64_IMM32:
            helper = reinterpret_cast<void*>(&jit_lock_sbb64);
            break;
        default:
            break;
        }
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(helper)));
        emit_helper_blr();
        join_native_lock(nl);
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
