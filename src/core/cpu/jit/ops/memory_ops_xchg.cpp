// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_memory_ops' exchange and compare-exchange family:
// XCHG EAX/AX and r/m16/r/m32, XADD r/m8/r/m16/r/m32, CMPXCHG r/m16/r/m32,
// CMPXCHG8B and CMPXCHG16B, with the jit_cmpxchg16b runtime helper that
// the CMPXCHG16B arm calls.
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

// Intel SDM Vol. 2 CMPXCHG16B: compare RDX:RAX with 16-byte-aligned m128.
// Equal → ZF=1 and store RCX:RBX; unequal → ZF=0 and load m128 into RDX:RAX.
// Unaligned m128 is #GP (STATUS_ACCESS_VIOLATION). LOCK BLR's
// jit_lock_cmpxchg16b via immediate address — no extra JitContext slot.
extern "C" void jit_cmpxchg16b(CpuState* cpu, GuestMemory* mem, u64 addr) {
    if (cpu == nullptr || mem == nullptr || cpu->pending_raise.active) {
        return;
    }

    auto raise_gp = [&]() {
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.from_helper = true;
        pending.code = 0xC0000005u;
        pending.flags = 0;
        pending.num_args = 2;
        pending.args[0] = 0;
        pending.args[1] = addr;
    };

    if ((addr & 0xFu) != 0) {
        raise_gp();
        return;
    }

    auto lo = mem->read_u64(addr);
    auto hi = mem->read_u64(addr + 8);
    if (!lo.is_ok() || !hi.is_ok()) {
        raise_gp();
        return;
    }

    cpu->materialize_lazy_flags();

    const u64 mem_lo = lo.value();
    const u64 mem_hi = hi.value();
    const u64 rax = cpu->gpr.get64(static_cast<u8>(RegIndex::RAX));
    const u64 rdx = cpu->gpr.get64(static_cast<u8>(RegIndex::RDX));

    if (mem_lo == rax && mem_hi == rdx) {
        cpu->flags.set_zf(true);
        const u64 rbx = cpu->gpr.get64(static_cast<u8>(RegIndex::RBX));
        const u64 rcx = cpu->gpr.get64(static_cast<u8>(RegIndex::RCX));
        auto r1 = mem->write_u64(addr, rbx);
        auto r2 = mem->write_u64(addr + 8, rcx);
        if (!r1.is_ok() || !r2.is_ok()) {
            raise_gp();
        }
    } else {
        cpu->flags.set_zf(false);
        cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), mem_lo);
        cpu->gpr.set64(static_cast<u8>(RegIndex::RDX), mem_hi);
    }
}

// nullopt = not in this family; false = declined; true = the arm reached
// `break`. See jit_compiler_lowering.hpp.
std::optional<bool> JitCompiler::compile_memory_ops_xchg(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- XCHG EAX, r32 ----
    case Opcode::XCHG_EAX_R32: {
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr32(REG_SCRATCH1, op1.reg);
        emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr32(REG_SCRATCH0, op1.reg);
        break;
    }

    // ---- XCHG AX, r16 ----
    // `66 90+r`. Sixteen bits, so both upper halves survive -- the whole
    // reason this is a distinct form from `XCHG EAX, r32`.
    case Opcode::XCHG_AX_R16: {
        emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr16(REG_SCRATCH1, op1.reg);
        emit_store_gpr16(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
        emit_store_gpr16(REG_SCRATCH0, op1.reg);
        break;
    }

    // ---- XCHG r/m16, r16 ----
    // Intel SDM Vol. 2B XCHG: exchange operands; no flags. The memory form is
    // implicitly locked, so it is one host atomic exchange (jit_atomic_xchg16,
    // shared with LOCK XCHG r/m16). It used to be a plain load then store,
    // which another thread's RMW could interleave with.
    case Opcode::XCHG_RM16_R16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit_store_gpr16(REG_SCRATCH1, op0.reg);
            emit_store_gpr16(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH2));
            emit(arm64::movz_w(3, static_cast<u16>(op1.reg & 15)));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                                  &jit_atomic_xchg16)));
            emit_helper_blr();
            emit_exit_if_pending_raise();
        }
        break;
    }

    // ---- XCHG r/m32, r32 ----
    case Opcode::XCHG_RM32_R32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit_store_gpr32(REG_SCRATCH1, op0.reg);
            emit_store_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            // Implicitly locked (Intel SDM Vol. 2 XCHG): the native LOCK path
            // applies unchanged, and leaves EFLAGS and NZCV alone as XCHG must.
            NativeLockSite nl;
            (void)emit_native_lock_rmw(nl, NativeLockKind::Xchg, 4, op0.mem,
                                       next_rip, &op1);
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(
                1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH2));
            emit(arm64::movz_w(3, static_cast<u16>(op1.reg)));
            emit(arm64::ldr_x_uoff(
                16, REG_JIT_CTX, JIT_CTX_OFF_ATOMIC_XCHG32 >> 3));
            emit_helper_blr();
            join_native_lock(nl);
        }
        break;
    }

    // ---- XADD r/m32, r32 ----
    case Opcode::XADD_RM32_R32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags(CpuState::FlagOp::Add32,
                                REG_SCRATCH1, REG_SCRATCH2);
            emit_store_gpr32(REG_SCRATCH2, op0.reg);
            emit_store_gpr32(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            // Save old memory value in REG_CALL_TMP (callee-saved).
            emit(arm64::mov_w(REG_CALL_TMP, REG_SCRATCH0));
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags(CpuState::FlagOp::Add32,
                                REG_SCRATCH1, REG_SCRATCH0);
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
            emit_store_gpr32(REG_CALL_TMP, op1.reg);
        }
        break;
    }

    // Intel SDM Vol. 2C XADD: 0F C1 /r (66h) — XADD r/m16, r16.
    // TEMP := SRC + DEST; SRC := DEST; DEST := TEMP. Flags from Add16.
    // emit_store_gpr16 preserves upper 16 bits of each GPR.
    case Opcode::XADD_RM16_R16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxth_w(REG_SCRATCH2, REG_SCRATCH2));
            emit_set_lazy_flags(CpuState::FlagOp::Add16,
                                REG_SCRATCH1, REG_SCRATCH2);
            emit_store_gpr16(REG_SCRATCH2, op0.reg);
            emit_store_gpr16(REG_SCRATCH0, op1.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_w(REG_CALL_TMP, REG_SCRATCH0));
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxth_w(REG_SCRATCH0, REG_SCRATCH0));
            emit_set_lazy_flags(CpuState::FlagOp::Add16,
                                REG_SCRATCH1, REG_SCRATCH0);
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
            emit_store_gpr16(REG_CALL_TMP, op1.reg);
        }
        break;
    }

    // Intel SDM Vol. 2C XADD: 0F C0 /r — XADD r/m8, r8.
    // TEMP := SRC + DEST; SRC := DEST; DEST := TEMP. Flags from the addition.
    case Opcode::XADD_RM8_R8: {
        const bool has_rex = instr.prefixes.has_rex();
        if (is_register_operand(op0.type)) {
            emit_load_gpr8(REG_SCRATCH0, op0.reg, has_rex);
            emit_load_gpr8(REG_SCRATCH1, op1.reg, has_rex);
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxtb_w(REG_SCRATCH2, REG_SCRATCH2));
            emit_set_lazy_flags(CpuState::FlagOp::Add8,
                                REG_SCRATCH1, REG_SCRATCH2);
            emit_store_gpr8(REG_SCRATCH2, op0.reg, has_rex);
            emit_store_gpr8(REG_SCRATCH0, op1.reg, has_rex);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_w(REG_CALL_TMP, REG_SCRATCH0));
            emit_load_gpr8(REG_SCRATCH1, op1.reg, has_rex);
            emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
            emit_set_lazy_flags(CpuState::FlagOp::Add8,
                                REG_SCRATCH1, REG_SCRATCH0);
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
            emit_store_gpr8(REG_CALL_TMP, op1.reg, has_rex);
        }
        break;
    }

    // ---- CMPXCHG r/m32, r32 ----
    case Opcode::CMPXCHG_RM32_R32: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
            emit_load_gpr32(REG_SCRATCH1, op0.reg);
            // Flags are `accumulator - r/m`. This arm, its RM16 sibling, the
            // interpreter and every lock helper all had it the other way
            // round, each citing an oracle; the matrix's "hardware column"
            // that settled it last time never ran hardware.
            // tools/oracle_probe/cmpxchg_flag_probe.c does, and its recorded
            // output shows a hand-encoded `lock cmpxchg [ebx], cl` leaving
            // exactly `cmp al, bl`'s flags. ZF is the same either way, which
            // is why every lock loop kept working while CF/SF/OF/AF/PF came
            // out inverted on every unequal compare.
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit_set_lazy_flags(CpuState::FlagOp::Sub32, REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::subs_w(31, REG_SCRATCH0, REG_SCRATCH1));
            usize bne_pos = m_code_buf.size();
            emit(0);
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
            usize b_done = m_code_buf.size();
            emit(0);
            usize ne_pos = m_code_buf.size();
            emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
            // `DEST := TEMP` also runs on the unequal path (Intel SDM Vol. 2
            // CMPXCHG). Writing the destination back with its own value is not
            // a no-op in long mode: emit_store_gpr32 applies x86-64's
            // zero-extension and clears bits 63:32, which hardware does and
            // this arm did not. Matches the interpreter's register path.
            emit_store_gpr32(REG_SCRATCH1, op0.reg);
            usize done_pos = m_code_buf.size();
            m_code_buf[bne_pos] = arm64::b_cond(arm64::NE,
                static_cast<i32>(ne_pos - bne_pos) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done_pos - b_done) * 4);
        } else {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::mov_w(10, REG_SCRATCH1));
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            // Full 64-bit EA. This was MOV W2, W2 -- a self-move that
            // zero-extends and silently dropped bits 63:32 of the address, so
            // `lock cmpxchg [r], r32` (every InterlockedCompareExchange and
            // spinlock in x64 code) hit the wrong memory whenever the heap sat
            // above 4 GB. The sibling jit_atomic_xchg32 already took a u64.
            emit(arm64::mov_x(2, REG_SCRATCH2));
            emit(arm64::mov_w(3, 10));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_CMPXCHG32 >> 3));
            emit_helper_blr();
        }
        break;
    }

    // Intel SDM Vol. 2 CMPXCHG: 0F B1 /r (66h) — compare AX with r/m16.
    // Equal → ZF=1, r16 → r/m16; unequal → ZF=0, r/m16 → AX.
    // No CMPXCHG16 helper — PE32 uses load/store (single-threaded parity).
    case Opcode::CMPXCHG_RM16_R16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
            emit_load_gpr16(REG_SCRATCH1, op0.reg);
            // Flags: AX − rm; see the RM32 arm above for the measurement.
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxth_w(REG_SCRATCH2, REG_SCRATCH2));
            emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                                REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::subs_w(31, REG_SCRATCH0, REG_SCRATCH1));
            usize bne_pos = m_code_buf.size();
            emit(0);
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
            usize b_done = m_code_buf.size();
            emit(0);
            usize ne_pos = m_code_buf.size();
            emit_store_gpr16(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
            usize done_pos = m_code_buf.size();
            m_code_buf[bne_pos] = arm64::b_cond(arm64::NE,
                static_cast<i32>(ne_pos - bne_pos) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done_pos - b_done) * 4);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
            // AX − rm; see the RM32 arm above for the measurement.
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::uxth_w(REG_SCRATCH2, REG_SCRATCH2));
            emit_set_lazy_flags(CpuState::FlagOp::Sub16,
                                REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::mov_w(REG_CALL_TMP, REG_SCRATCH1));
            emit(arm64::subs_w(31, REG_SCRATCH0, REG_SCRATCH1));
            usize bne_pos = m_code_buf.size();
            emit(0);
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
            usize b_done = m_code_buf.size();
            emit(0);
            usize ne_pos = m_code_buf.size();
            emit_store_gpr16(REG_CALL_TMP, static_cast<u8>(RegIndex::RAX));
            usize done_pos = m_code_buf.size();
            m_code_buf[bne_pos] = arm64::b_cond(arm64::NE,
                static_cast<i32>(ne_pos - bne_pos) * 4);
            m_code_buf[b_done] = arm64::b_imm(
                static_cast<i32>(done_pos - b_done) * 4);
        }
        break;
    }

    // ---- CMPXCHG8B m64 ----
    case Opcode::CMPXCHG8B_M64: {
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_CMPXCHG8B >> 3));
        emit_helper_blr();
        break;
    }

    // ---- CMPXCHG16B m128 (REX.W 0F C7 /1) ----
    case Opcode::CMPXCHG16B_M128: {
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_cmpxchg16b)));
        emit_helper_blr();
        emit_exit_if_pending_raise();
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
