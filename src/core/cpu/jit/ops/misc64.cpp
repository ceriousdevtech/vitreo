// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: NOP/NOP_RM/PAUSE, RDTSC/RDTSCP
// and the unlocked XCHG/XADD/CMPXCHG r/m64 forms.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

extern "C" u64 jit_virtual_tsc_ticks() noexcept;

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_misc64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    case Opcode::NOP:
    case Opcode::NOP_RM:
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

    // ---- Unlocked XCHG / XADD / CMPXCHG r/m64 (Intel SDM Vol. 2) ----
    // XCHG mem is implicitly locked (same as interpreter exec_xchg_rm).
    // Explicit LOCK F0 forms stay on compile_locked.
    case Opcode::XCHG_RM64_R64: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
            emit_store_gpr64(REG_SCRATCH1, op0.reg);
            emit_store_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            // Implicitly locked (Intel SDM Vol. 2 XCHG): the native LOCK path
            // applies unchanged, and leaves EFLAGS and NZCV alone as XCHG must.
            NativeLockSite nl;
            (void)emit_native_lock_rmw(nl, NativeLockKind::Xchg, 8, op0.mem,
                                       next_rip, &op1);
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH2));
            emit(arm64::movz_w(3, static_cast<u16>(op1.reg)));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                                  &jit_atomic_xchg64)));
            emit_helper_blr();
            emit_exit_if_pending_raise();
            join_native_lock(nl);
        }
        break;
    }

    case Opcode::XADD_RM64_R64: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
            emit(arm64::add_x(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags64(CpuState::FlagOp::Add64,
                                  REG_SCRATCH1, REG_SCRATCH2);
            emit_store_gpr64(REG_SCRATCH2, op0.reg);
            emit_store_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
            emit(arm64::add_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags64(CpuState::FlagOp::Add64,
                                  REG_SCRATCH1, REG_SCRATCH0);
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
            emit_store_gpr64(REG_CALL_TMP, op1.reg);
        }
        break;
    }

    // CMPXCHG r/m64, r64: compare RAX with r/m64. Equal → store r64, ZF=1;
    // unequal → r/m64 → RAX, ZF=0. Flags from DEST − accumulator (Sub64).
    case Opcode::CMPXCHG_RM64_R64: {
        constexpr u8 rax = static_cast<u8>(RegIndex::RAX);
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH0, rax);
            emit_load_gpr64(REG_SCRATCH1, op0.reg);
            // RAX - r/m, measured on silicon; see ops/memory_ops_xchg.cpp's
            // RM32 arm and tools/oracle_probe/cmpxchg_flag_probe.c.
            emit(arm64::sub_x(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags64(CpuState::FlagOp::Sub64,
                                  REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::subs_x(31, REG_SCRATCH0, REG_SCRATCH1));
            const usize bne_pos = m_code_buf.size();
            emit(0);
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            const usize b_done = m_code_buf.size();
            emit(0);
            const usize ne_pos = m_code_buf.size();
            emit_store_gpr64(REG_SCRATCH1, rax);
            const usize done_pos = m_code_buf.size();
            m_code_buf[bne_pos] = arm64::b_cond(
                arm64::NE, static_cast<i32>(ne_pos - bne_pos) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done_pos - b_done) * 4);
        } else {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr64(REG_SCRATCH0, rax);
            // Result must not live in REG_SCRATCH3 — emit_set_lazy_flags64
            // clobbers X3 while writing FlagOp before saving flag_dst.
            emit(arm64::sub_x(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags64(CpuState::FlagOp::Sub64,
                                  REG_SCRATCH1, REG_SCRATCH4);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH1));
            emit(arm64::subs_x(31, REG_SCRATCH0, REG_SCRATCH1));
            const usize bne_pos = m_code_buf.size();
            emit(0);
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH2, instr, op0);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
            const usize b_done = m_code_buf.size();
            emit(0);
            const usize ne_pos = m_code_buf.size();
            emit_store_gpr64(REG_CALL_TMP, rax);
            const usize done_pos = m_code_buf.size();
            m_code_buf[bne_pos] = arm64::b_cond(
                arm64::NE, static_cast<i32>(ne_pos - bne_pos) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done_pos - b_done) * 4);
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
