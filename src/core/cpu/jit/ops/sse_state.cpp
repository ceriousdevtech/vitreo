// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — MXCSR, the FXSAVE image and cache-control hints: FXSAVE/
// FXRSTOR, CLFLUSH, MOVNTI, LDMXCSR and STMXCSR.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

// Intel SDM Vol. 2 FXSAVE/FXRSTOR: 16-byte aligned 512-byte image. Layout,
// abridged FTW, and FXSAVE64 vs FXSAVE (REX.W in long mode) live in the
// interpreter; this helper reconstructs a dummy memory operand at `ea`.
extern "C" void jit_publish_mxcsr(CpuState* cpu);
extern "C" void jit_discard_mxcsr_exceptions(CpuState* cpu);

extern "C" void jit_fxsave_fxrstor(CpuState* cpu, GuestMemory* mem, u64 ea,
                                   u32 flags) noexcept {
    if (cpu == nullptr || mem == nullptr) {
        return;
    }

    constexpr u8 kEaReg = static_cast<u8>(RegIndex::RAX);
    const u64 saved = cpu->gpr.get64(kEaReg);
    cpu->gpr.set64(kEaReg, ea);

    DecodedInstruction di{};
    di.opcode = (flags & 1u) ? Opcode::FXRSTOR_M512 : Opcode::FXSAVE_M512;
    if ((flags & 2u) != 0) {
        di.prefixes.rex = 0x48;  // REX.W → FXSAVE64 / FXRSTOR64
    }
    MemoryOperand mop{};
    mop.base = kEaReg;
    di.operands[0] = Operand::make_mem(OperandType::MemUnknown, mop);
    di.operand_count = 1;

    ExecContext ctx{*cpu, *mem};
    const ExecResult result = ::vitreo::cpu::ops::exec_sse_data(ctx, di);
    cpu->gpr.set64(kEaReg, saved);

    if (result == ExecResult::Exception && !cpu->pending_raise.active) {
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.from_helper = true;
        pending.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION (#GP mapping)
        pending.flags = 0;
        pending.num_args = 2;
        pending.args[0] = 0;
        pending.args[1] = ea;
    }
}

extern "C" void jit_require_align16(CpuState* cpu, u64 address) noexcept;

std::optional<bool> JitCompiler::compile_sse_state(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // Intel SDM Vol. 2 FXSAVE/FXRSTOR: #GP if m512 is not 16-byte aligned.
    // Semantic image is owned by exec_sse_data (abridged FTW, FXSAVE64).
    case Opcode::FXSAVE_M512:
    case Opcode::FXRSTOR_M512: {
        emit_update_rip(instr_rip);
        emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::mov_x(1, REG_SCRATCH4));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_require_align16)));
        emit_helper_blr();
        emit_exit_if_pending_raise();

        emit_compute_ea(REG_SCRATCH4, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
        u32 flags = 0;
        if (instr.opcode == Opcode::FXRSTOR_M512) {
            flags |= 1u;
        }
        if (instr.prefixes.rex_w()) {
            flags |= 2u;
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH4));
        emit_mov_imm32(3, flags);
        EMIT_SSE_MEM_HELPER_CALL(jit_fxsave_fxrstor);
        emit_exit_if_pending_raise();
        break;
    }

    // Intel SDM Vol. 2 CLFLUSH: cache-line flush is a hint; the address
    // must still be valid (same probe as a byte load).
    case Opcode::CLFLUSH_M8: {
        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
        emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
        break;
    }

    // Intel SDM Vol. 2 MOVNTI: GPR store; non-temporal hint is ignored.
    case Opcode::MOVNTI_M32_R32: {
        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        emit_load_gpr32(REG_SCRATCH1, op1.reg);
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
        emit_guest_store32(REG_SCRATCH1, REG_SCRATCH2);
        break;
    }
    case Opcode::MOVNTI_M64_R64: {
        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        emit_load_gpr64(REG_SCRATCH1, op1.reg);
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
        emit_guest_store64(REG_SCRATCH1, REG_SCRATCH2);
        break;
    }

    // Intel SDM Vol. 2 LDMXCSR: MXCSR := m32. Interpreter applies
    // mxcsr_mask (supported bits) rather than #GP on reserved bits.
    case Opcode::LDMXCSR_M32: {
        const u32 mxcsr_off = static_cast<u32>(
            offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
        const u32 mask_off = static_cast<u32>(
            offsetof(CpuState, sse) + offsetof(SseState, mxcsr_mask));
        if (!fits_ldr_w_offset(mxcsr_off) || !fits_ldr_w_offset(mask_off)) {
            return false;
        }
        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
        emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        emit(arm64::ldr_w_uoff(REG_SCRATCH1, REG_CPU_STATE, mask_off >> 2));
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, mxcsr_off >> 2));
        // LDMXCSR writes the sticky bits outright; anything pending is
        // superseded and must not resurface on the next STMXCSR.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(
            reinterpret_cast<uintptr_t>(&jit_discard_mxcsr_exceptions)));
        emit_helper_blr();
        break;
    }

    // Intel SDM Vol. 2 STMXCSR: m32 := MXCSR (reserved bits stored as 0).
    case Opcode::STMXCSR_M32: {
        const u32 mxcsr_off = static_cast<u32>(
            offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
        if (!fits_ldr_w_offset(mxcsr_off)) {
            return false;
        }
        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        // MXCSR's sticky exception bits are accumulated lazily by the host FPU
        // and folded into architectural state only where the guest can see
        // them — the same design the x87 status word uses. Publish them here,
        // matching the interpreter's STMXCSR. Without this the JIT stored
        // MXCSR's reset value however much arithmetic had rounded.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(
            reinterpret_cast<uintptr_t>(&jit_publish_mxcsr)));
        emit_helper_blr();
        emit(arm64::ldr_w_uoff(REG_SCRATCH1, REG_CPU_STATE, mxcsr_off >> 2));
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
        emit_guest_store32(REG_SCRATCH1, REG_SCRATCH2);
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit
