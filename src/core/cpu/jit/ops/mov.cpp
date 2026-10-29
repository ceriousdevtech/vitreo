// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — MOV and data-movement opcodes

#include "../jit_helpers.hpp"

namespace vitreo::cpu::ops {
u64 virtual_tsc_ticks() noexcept;
}

namespace vitreo::cpu::jit {

extern "C" u64 jit_virtual_tsc_ticks() noexcept {
    return vitreo::cpu::ops::virtual_tsc_ticks();
}

bool JitCompiler::compile_mov(const DecodedInstruction& instr,
                               u64 /*instr_rip*/, u64 next_rip,
                               bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    case Opcode::NOP:
    case Opcode::PAUSE:
        // Intel SDM Vol. 2 PAUSE: spin-wait hint; no architectural state change.
        break;

    case Opcode::RDTSC:
    case Opcode::RDTSCP: {
        // Intel SDM Vol. 2 RDTSC: EDX:EAX := TSC. On Intel 64, RAX/RDX
        // high halves are cleared (emit_store_gpr32 zero-extends).
        // RDTSCP also writes ECX := IA32_TSC_AUX[31:0]; not installed, so 0.
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_virtual_tsc_ticks)));
        emit_helper_blr();  // X0 = virtual TSC ns
        emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 32));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        if (instr.opcode == Opcode::RDTSCP) {
            emit_mov_imm32(REG_SCRATCH0, 0);
            emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RCX));
        }
        break;
    }

    case Opcode::NOP_RM:
        break;

    case Opcode::LFENCE:
    case Opcode::MFENCE:
    case Opcode::SFENCE:
        // DMB ISH. Explicit x86 fences must order guest memory even when the
        // optional strict-TSO diagnostic mode is disabled. This is stronger
        // than LFENCE/SFENCE individually but preserves their contract.
        emit(0xD5033BBFu);
        break;

    case Opcode::MOV_R32_IMM32: {
        u32 imm = static_cast<u32>(op1.imm);
        const bool pe32_pinned_dst = !m_block_is_64bit && m_pins_live
            && op0.reg < 8 && m_pin_arm[op0.reg] != 0xFF;
        const u8 arm_dst = pe32_pinned_dst ? m_pin_arm[op0.reg]
                                           : REG_SCRATCH0;
        emit_mov_imm32(arm_dst, imm);
        emit_store_gpr32(arm_dst, op0.reg);
        break;
    }

    case Opcode::MOV_R32_RM32: {
        if (is_register_operand(op1.type)) {
            const bool pe32_pinned_src = !m_block_is_64bit && m_pins_live
                && op1.reg < 8 && m_pin_arm[op1.reg] != 0xFF;
            const u8 arm_src = pe32_pinned_src ? m_pin_arm[op1.reg]
                                               : REG_SCRATCH0;
            emit_load_gpr32(arm_src, op1.reg);
            emit_store_gpr32(arm_src, op0.reg);
            break;
        } else if (!try_esp_disp_load32(REG_SCRATCH0, op1.mem)) {
            const u8 addr = emit_addr32(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, addr);
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOV_RM32_R32: {
        if (is_register_operand(op0.type)) {
            const bool pe32_pinned_src = !m_block_is_64bit && m_pins_live
                && op1.reg < 8 && m_pin_arm[op1.reg] != 0xFF;
            const u8 arm_src = pe32_pinned_src ? m_pin_arm[op1.reg]
                                               : REG_SCRATCH0;
            emit_load_gpr32(arm_src, op1.reg);
            emit_store_gpr32(arm_src, op0.reg);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
            if (try_esp_disp_store32(REG_SCRATCH0, op0.mem)) break;
            const u8 addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, addr);
        }
        break;
    }

    case Opcode::MOV_RM32_IMM32: {
        u32 imm = static_cast<u32>(op1.imm);
        if (is_register_operand(op0.type)) {
            const bool pe32_pinned_dst = !m_block_is_64bit && m_pins_live
                && op0.reg < 8 && m_pin_arm[op0.reg] != 0xFF;
            const u8 arm_dst = pe32_pinned_dst ? m_pin_arm[op0.reg]
                                               : REG_SCRATCH0;
            emit_mov_imm32(arm_dst, imm);
            emit_store_gpr32(arm_dst, op0.reg);
        } else {
            emit_mov_imm32(REG_SCRATCH0, imm);
            if (try_esp_disp_store32(REG_SCRATCH0, op0.mem)) break;
            const u8 addr = emit_addr32(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, addr);
        }
        break;
    }

    case Opcode::MOV_R8_IMM8: {
        emit_mov_imm32(REG_SCRATCH0, static_cast<u32>(op1.imm) & 0xFF);
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    case Opcode::MOV_RM8_R8: {
        emit_load_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        if (is_register_operand(op0.type)) {
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    case Opcode::MOV_R8_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        break;
    }

    case Opcode::MOV_RM8_IMM8: {
        if (is_register_operand(op0.type)) {
            emit_mov_imm32(REG_SCRATCH0, static_cast<u32>(op1.imm) & 0xFF);
            emit_store_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        } else {
            emit_mov_imm32(REG_SCRATCH0, static_cast<u32>(op1.imm) & 0xFF);
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    case Opcode::XCHG_RM8_R8: {
        // A memory-form XCHG is implicitly locked (Intel SDM Vol. 2 XCHG:
        // "the processor's locking protocol is automatically implemented ...
        // regardless of the presence or absence of the LOCK prefix"). It goes
        // to the same host-atomic helper as LOCK XCHG r/m8 (compile_locked),
        // rather than to the interpreter delegate, whose operand-less
        // LockPrefixGuard would take the process-wide exclusive mutex.
        if (!is_register_operand(op0.type)) {
            const u8 r = static_cast<u8>(op1.reg & 15);
            const bool legacy_high =
                r >= 4 && r <= 7 && !instr.prefixes.has_rex();
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH2));
            emit(arm64::movz_w(3, static_cast<u16>(
                                      r | (legacy_high ? 0x100u : 0u))));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                                  &jit_atomic_xchg8)));
            emit_helper_blr();
            emit_exit_if_pending_raise();
            break;
        }
        emit_load_gpr8(REG_SCRATCH0, op0.reg, instr.prefixes.has_rex());
        emit_load_gpr8(REG_SCRATCH1, op1.reg, instr.prefixes.has_rex());
        emit_store_gpr8(REG_SCRATCH1, op0.reg, instr.prefixes.has_rex());
        emit_store_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        break;
    }

    case Opcode::MOV_RM16_R16: {
        emit_load_gpr16(REG_SCRATCH0, op1.reg);
        if (is_register_operand(op0.type)) {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    case Opcode::MOV_R16_RM16: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_store_gpr16(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOV_RM16_IMM16:
    case Opcode::MOV_R16_IMM16: {
        u16 imm = static_cast<u16>(op1.imm);
        emit_mov_imm32(REG_SCRATCH0, imm);
        if (is_register_operand(op0.type)) {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    case Opcode::MOVZX_R16_RM8: {
        // Intel SDM Vol. 2B MOVZX: zero-extend r/m8 into r16 (bits 16..31 preserved).
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_store_gpr16(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOVZX_R32_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOVZX_R32_RM16: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
            // UXTH: UBFM Wd, Wn, #0, #15
            emit(0x53003C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                              | static_cast<u32>(REG_SCRATCH0));
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOVSX_R16_RM8: {
        // Intel SDM Vol. 2B MOVSX: sign-extend r/m8 into r16 (bits 16..31 preserved).
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        }
        // SXTB: SBFM Wd, Wn, #0, #7
        emit(0x13001C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH0));
        emit_store_gpr16(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOVSX_R32_RM8: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr8(REG_SCRATCH0, op1.reg, instr.prefixes.has_rex());
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        }
        // SXTB: SBFM Wd, Wn, #0, #7
        emit(0x13001C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH0));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOVSX_R32_RM16: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        }
        // SXTH: SBFM Wd, Wn, #0, #15
        emit(0x13003C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH0));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::CDQ: {
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // ASR W1, W0, #31
        emit(0x131F7C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH1));
        emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        break;
    }

    case Opcode::CWD: {
        // Intel SDM Vol. 2A CWD: DX := SignExtend(AX); AX unchanged.
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // SXTH W0, W0
        emit(0x13003C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH0));
        // ASR W1, W0, #31 → 0x0000 or 0xFFFF in low 16 for DX
        emit(0x131F7C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH1));
        emit_store_gpr16(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        break;
    }

    case Opcode::CBW: {
        // Intel SDM Vol. 2A CBW: AX := sign-extend(AL); bits 16..31 of EAX preserved.
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX), false);
        // SXTB W0, W0
        emit(0x13001C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH0));
        emit_store_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::CWDE: {
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // SXTH W0, W0
        emit(0x13003C00u | (static_cast<u32>(REG_SCRATCH0) << 5)
                          | static_cast<u32>(REG_SCRATCH0));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::LEA_R32_M: {
        emit_compute_ea(REG_SCRATCH0, op1.mem, next_rip, false);
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::MOV_AL_MOFFS8: {
        emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
        emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
        emit_store_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::MOV_MOFFS8_AL: {
        emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
        break;
    }

    case Opcode::MOV_EAX_MOFFS32: {
        emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
        emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    case Opcode::MOV_MOFFS32_EAX: {
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        break;
    }

    default: return false;
    }

    if (is_last) {
        emit_update_rip(next_rip);
        emit_tail_link_or_exit(next_rip, m_flags.nzcv);
    }
    return true;
}

} // namespace vitreo::cpu::jit
