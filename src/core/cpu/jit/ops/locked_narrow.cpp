// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_locked's LOCK 8- and 16-bit forms: the r/m8 and
// r/m16 ALU and unary ops, XADD/XCHG r/m8 and r/m16, and CMPXCHG r/m16.
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
std::optional<bool> JitCompiler::compile_locked_narrow(
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

    case Opcode::OR_RM16_R16:
    case Opcode::OR_RM16_IMM8:
    case Opcode::OR_RM16_IMM16:
    case Opcode::AND_RM16_R16:
    case Opcode::AND_RM16_IMM8:
    case Opcode::AND_RM16_IMM16: {
        // 16-bit atomic flag words: `lock and/or word ptr [obj+disp], imm8`.
        if (is_register_operand(op0.type)) return false;
        const bool is_reg_src =
            instr.opcode == Opcode::OR_RM16_R16 ||
            instr.opcode == Opcode::AND_RM16_R16;
        if (is_reg_src) {
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit(arm64::mov_w(10, REG_SCRATCH1));
        } else {
            emit_mov_imm32(10, static_cast<u32>(op1.imm) & 0xffffu);
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        u32 helper_off = JIT_CTX_OFF_LOCK_OR16;
        switch (instr.opcode) {
        case Opcode::AND_RM16_R16:
        case Opcode::AND_RM16_IMM8:
        case Opcode::AND_RM16_IMM16:
            helper_off = JIT_CTX_OFF_LOCK_AND16;
            break;
        default:
            break;
        }
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
        emit_helper_blr();
        break;
    }

    // ---- LOCK r/m8 and the remaining r/m16 forms --------------------------
    //
    // These all fell to the interpreter delegate, whose dispatcher-level
    // LockPrefixGuard has no operand and takes the PROCESS-WIDE exclusive
    // mutex (see XCHG_RM32_R32 in ops/locked_rmw32.cpp). In a 64-bit title,
    // LOCK XADD r/m8 alone sent 4.9% of all LOCK operations exclusive.
    // The helpers (jit_helpers_lock.cpp) do the RMW with a host atomic of the
    // operand's width and write flags/registers only after it commits.
    case Opcode::ADD_RM8_R8:   case Opcode::ADD_RM8_IMM8:
    case Opcode::OR_RM8_R8:    case Opcode::OR_RM8_IMM8:
    case Opcode::ADC_RM8_R8:   case Opcode::ADC_RM8_IMM8:
    case Opcode::SBB_RM8_R8:   case Opcode::SBB_RM8_IMM8:
    case Opcode::AND_RM8_R8:   case Opcode::AND_RM8_IMM8:
    case Opcode::SUB_RM8_R8:   case Opcode::SUB_RM8_IMM8:
    case Opcode::XOR_RM8_R8:   case Opcode::XOR_RM8_IMM8:
    case Opcode::INC_RM8:      case Opcode::DEC_RM8:
    case Opcode::NOT_RM8:      case Opcode::NEG_RM8:
    case Opcode::ADD_RM16_R16: case Opcode::ADD_RM16_IMM8:
    case Opcode::ADD_RM16_IMM16:
    case Opcode::ADC_RM16_R16: case Opcode::ADC_RM16_IMM8:
    case Opcode::ADC_RM16_IMM16:
    case Opcode::SBB_RM16_R16: case Opcode::SBB_RM16_IMM8:
    case Opcode::SBB_RM16_IMM16:
    case Opcode::SUB_RM16_R16: case Opcode::SUB_RM16_IMM8:
    case Opcode::SUB_RM16_IMM16:
    case Opcode::XOR_RM16_R16: case Opcode::XOR_RM16_IMM8:
    case Opcode::XOR_RM16_IMM16:
    case Opcode::INC_RM16:     case Opcode::DEC_RM16:
    case Opcode::NOT_RM16:     case Opcode::NEG_RM16: {
        // LOCK with a register destination is #UD (Intel SDM Vol. 2 LOCK);
        // the delegate raises it.
        if (is_register_operand(op0.type)) return false;
        // Selector values: jit_helpers_lock.cpp kLockNarrow*.
        u32 sel = 0;
        bool byte = true;
        switch (instr.opcode) {
        case Opcode::ADD_RM8_R8:  case Opcode::ADD_RM8_IMM8:  sel = 0; break;
        case Opcode::OR_RM8_R8:   case Opcode::OR_RM8_IMM8:   sel = 1; break;
        case Opcode::ADC_RM8_R8:  case Opcode::ADC_RM8_IMM8:  sel = 2; break;
        case Opcode::SBB_RM8_R8:  case Opcode::SBB_RM8_IMM8:  sel = 3; break;
        case Opcode::AND_RM8_R8:  case Opcode::AND_RM8_IMM8:  sel = 4; break;
        case Opcode::SUB_RM8_R8:  case Opcode::SUB_RM8_IMM8:  sel = 5; break;
        case Opcode::XOR_RM8_R8:  case Opcode::XOR_RM8_IMM8:  sel = 6; break;
        case Opcode::INC_RM8:  sel = 8; break;
        case Opcode::DEC_RM8:  sel = 9; break;
        case Opcode::NOT_RM8:  sel = 10; break;
        case Opcode::NEG_RM8:  sel = 11; break;
        case Opcode::ADD_RM16_R16: case Opcode::ADD_RM16_IMM8:
        case Opcode::ADD_RM16_IMM16: sel = 0; byte = false; break;
        case Opcode::ADC_RM16_R16: case Opcode::ADC_RM16_IMM8:
        case Opcode::ADC_RM16_IMM16: sel = 2; byte = false; break;
        case Opcode::SBB_RM16_R16: case Opcode::SBB_RM16_IMM8:
        case Opcode::SBB_RM16_IMM16: sel = 3; byte = false; break;
        case Opcode::SUB_RM16_R16: case Opcode::SUB_RM16_IMM8:
        case Opcode::SUB_RM16_IMM16: sel = 5; byte = false; break;
        case Opcode::XOR_RM16_R16: case Opcode::XOR_RM16_IMM8:
        case Opcode::XOR_RM16_IMM16: sel = 6; byte = false; break;
        case Opcode::INC_RM16: sel = 8; byte = false; break;
        case Opcode::DEC_RM16: sel = 9; byte = false; break;
        case Opcode::NOT_RM16: sel = 10; byte = false; break;
        case Opcode::NEG_RM16: sel = 11; byte = false; break;
        default: return false;
        }
        const bool unary = sel >= 8;
        if (!unary) {
            if (instr.operand_count < 2) return false;
            if (is_register_operand(op1.type)) {
                if (byte) {
                    emit_load_gpr8(REG_SCRATCH1, op1.reg,
                                   instr.prefixes.has_rex());
                } else {
                    emit_load_gpr16(REG_SCRATCH1, op1.reg);
                }
                emit(arm64::mov_w(10, REG_SCRATCH1));
            } else if (is_immediate_operand(op1.type)) {
                // The decoder sign-extends imm8 into op1.imm; truncate to the
                // operand width (the helper truncates again).
                emit_mov_imm32(10, static_cast<u32>(op1.imm) &
                                       (byte ? 0xFFu : 0xFFFFu));
            } else {
                return false;
            }
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        if (unary) emit(arm64::movz_w(3, 0));
        else emit(arm64::mov_w(3, 10));
        emit(arm64::movz_w(4, static_cast<u16>(sel)));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            byte ? reinterpret_cast<void*>(&jit_lock_alu8)
                 : reinterpret_cast<void*>(&jit_lock_alu16))));
        emit_helper_blr();
        break;
    }

    case Opcode::XADD_RM8_R8:
    case Opcode::XCHG_RM8_R8: {
        if (is_register_operand(op0.type)) return false;
        // Register code: bit 8 marks AH/CH/DH/BH (4-7 without REX).
        const u8 r = static_cast<u8>(op1.reg & 15);
        const bool legacy_high = r >= 4 && r <= 7 && !instr.prefixes.has_rex();
        const u16 code = static_cast<u16>(r | (legacy_high ? 0x100u : 0u));
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::movz_w(3, code));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            instr.opcode == Opcode::XADD_RM8_R8
                ? reinterpret_cast<void*>(&jit_lock_xadd8)
                : reinterpret_cast<void*>(&jit_atomic_xchg8))));
        emit_helper_blr();
        break;
    }

    case Opcode::XADD_RM16_R16:
    case Opcode::XCHG_RM16_R16: {
        if (is_register_operand(op0.type)) return false;
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::movz_w(3, static_cast<u16>(op1.reg & 15)));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            instr.opcode == Opcode::XADD_RM16_R16
                ? reinterpret_cast<void*>(&jit_lock_xadd16)
                : reinterpret_cast<void*>(&jit_atomic_xchg16))));
        emit_helper_blr();
        break;
    }

    case Opcode::CMPXCHG_RM16_R16: {
        if (is_register_operand(op0.type)) return false;
        emit_load_gpr16(REG_SCRATCH1, op1.reg);
        emit(arm64::mov_w(10, REG_SCRATCH1));
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_w(3, 10));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_lock_cmpxchg16)));
        emit_helper_blr();
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
