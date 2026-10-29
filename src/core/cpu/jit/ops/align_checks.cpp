// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — alignment #GP checks for 128-bit and VEX.256 memory operands:
// jit_require_align16/32 (the raising helpers), the legacy-SSE 16-byte rule
// (emit_m128_align_check_if_required), the cold-path check it emits
// (emit_align_check) and the end-of-block flush that places those cold
// paths (emit_flush_cold_align_checks).
//
// These are JitCompiler-wide: SSE lowerings in both bitnesses use them.
// They lived in ops/ops64.cpp until compile_64bit was split by opcode
// family (ADR-0006 step 3) and moved here verbatim.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// Aligned 128-bit move operands must be 16-byte aligned. Record the same
// Windows-visible exception used by the interpreter's #GP path, then let
// generated code exit through emit_exit_if_pending_raise().
extern "C" void jit_require_align16(CpuState* cpu, u64 address) noexcept {
    if (cpu == nullptr || cpu->pending_raise.active || (address & 15u) == 0)
        return;
    auto& pending = cpu->pending_raise;
    pending.active = true;
    pending.from_helper = true;
    pending.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION (#GP mapping)
    pending.flags = 0;
    pending.num_args = 2;
    pending.args[0] = 0;
    // A misalignment #GP does not supply CR2. Native Windows exposes an
    // unavailable address, unlike a paging fault (minmax memory oracle).
    pending.args[1] = cpu->is_64bit() ? std::numeric_limits<u64>::max()
                                     : std::numeric_limits<u32>::max();
}

// VEX.256 aligned moves (VMOVDQA/VMOVAPS/VMOVAPD ymm) require 32-byte
// alignment, not 16 (Intel SDM Vol. 2, VMOVDQA: "#GP if memory operand is not
// 32-byte aligned" for the 256-bit form). Using the 16-byte check accepts
// addresses real hardware faults on.
extern "C" void jit_require_align32(CpuState* cpu, u64 address) noexcept {
    if (cpu == nullptr || cpu->pending_raise.active || (address & 31u) == 0)
        return;
    auto& pending = cpu->pending_raise;
    pending.active = true;
    pending.from_helper = true;
    pending.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION (#GP mapping)
    pending.flags = 0;
    pending.num_args = 2;
    pending.args[0] = 0;
    pending.args[1] = cpu->is_64bit() ? std::numeric_limits<u64>::max()
                                     : std::numeric_limits<u32>::max();
}

/// The legacy-SSE 16-byte alignment rule, in one place.
///
/// Three conditions, and every one of them has already been got wrong
/// somewhere in this tree:
///
///   1. The operand must be a 128-bit MEMORY operand. Alignment is a property
///      of the operand, not the opcode -- which is why a hand-maintained list
///      of "aligned instructions" is always wrong. Measured on silicon
///      (tools/oracle_probe/sse_align_probe.c): XORPS, PXOR, PAND, POR, PADDB,
///      PMULLW, PACKSSWB, PSLLW, PCMPEQB and every other legacy packed form
///      fault on a misaligned m128, not just the MOV*A* family.
///   2. The encoding must not be VEX. `m_current_original_vex` is NOT
///      redundant with `instr.prefixes.vex`: compile_sse's VEX.128 bridge
///      re-presents a legacy-SHAPED instruction with that field already
///      cleared. Missing this raises a bogus #GP on `VPXOR xmm2, xmm1, [rdi]`
///      and terminates a launcher before it can draw a window.
///   3. The opcode must not be one of the explicitly UNALIGNED spellings.
///      These are the named exceptions to (1), and the probe confirms them:
///      movups/movupd/movdqu/lddqu do not fault where movaps/movdqa do.
void JitCompiler::emit_m128_align_check_if_required(
        u8 ea_reg, const DecodedInstruction& instr, const Operand& op) {
    // Escape hatch. The exception list below is derived from the architecture
    // for the families the silicon probe did NOT cover, and being wrong about
    // one of them FABRICATES a #GP in the middle of working guest code -- which
    // has already cost two launcher outages in one day (VPXOR, then PCMPISTRI).
    // If a guest starts dying with a GeneralProtection on a 16-byte operand,
    // set VITREO_JIT_NO_M128_ALIGN=1 to confirm this is the cause in one run,
    // then add the opcode to the exclusions and fix the interpreter to match.
    static const bool disabled =
        std::getenv("VITREO_JIT_NO_M128_ALIGN") != nullptr;
    if (disabled) return;
    if (op.type != OperandType::Mem128) return;
    if (instr.prefixes.vex || m_current_original_vex) return;
    switch (instr.opcode) {
    // The unaligned spellings. Anything NOT listed here is checked.
    case Opcode::MOVUPS_XMM_XMMM128: case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVUPD_XMM_XMMM128: case Opcode::MOVUPD_XMMM128_XMM:
    case Opcode::MOVDQU_XMM_XMMM128: case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::LDDQU_XMM_M128:
        return;
    // MOVSHDUP/MOVSLDUP were excluded here as UNMEASURED, with the note "settle
    // these on the oracle before adding them, and add the interpreter side in
    // the same change." Now measured: both raise #GP on a misaligned operand on
    // real silicon (Windows 10.0.26200, tools/oracle_probe/sse_align_probe with
    // its recorded .expected), so they are checked like every other legacy
    // 128-bit form and the interpreter gained the same rule in this change.
    // The SSE4.2 string instructions take an UNALIGNED m128 by design -- that
    // is the entire point of them. MSVC's strlen/strcmp scan arbitrary guest
    // pointers with PCMPISTRI; requiring 16-byte alignment would fault on
    // ordinary string code that runs fine on Windows.
    //
    // Measured the hard way: adding the class rule here crashed a launcher
    // deterministically at `PCMPISTRI xmm0, [eax-16], 0x0C` with eax-16 =
    // 0x7ffcfd54 (misaligned by 4), killing the process 9 times before it
    // could start its renderer helper. The ISA sweep did NOT catch it, because the
    // interpreter had been given the same wrong rule in the same change -- two
    // engines agreeing with each other is not evidence either is right.
    //
    // Since confirmed on silicon: all four report `ok` misaligned, matching
    // Intel's Type 4 wording that PCMPISTRI "does not cause #GP if the memory
    // operand is not aligned to 16 Byte boundary". The probe now covers them.
    case Opcode::PCMPISTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPISTRM_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRM_XMM_XMMM128_IMM8:
        return;
    default:
        break;
    }
    emit_align_check(ea_reg, /*align32=*/false);
}

void JitCompiler::emit_align_check(u8 ea_reg, bool align32) {
    AttrScope attr_scope(*this, EmitAttribution::Raise);
    // UBFX leaves NZCV alone; CBNZ reads none. X8 is free at every call site
    // (the EA has just been computed and nothing else is live in it).
    emit(arm64::ubfx_x(8, ea_reg, 0, align32 ? 5 : 4));
    ColdAlignCheck check;
    check.branch_pos = m_code_buf.size();
    emit(0);
    check.return_pos = m_code_buf.size();
    check.ea_reg = ea_reg;
    check.align32 = align32;
    check.xmm_cache_live = m_xmm_cache;
    m_cold_align_checks.push_back(check);
}

void JitCompiler::emit_flush_cold_align_checks() {
    AttrScope attr_scope(*this, EmitAttribution::Raise);
    for (usize i = 0; i < m_cold_align_checks.size(); ++i) {
        const ColdAlignCheck c = m_cold_align_checks[i];
        const usize dest = m_code_buf.size();
        m_code_buf[c.branch_pos] = arm64::cbnz_x(
            8, cold_branch_disp(c.branch_pos, dest, 19, "cold align check"));
        // jit_require_align16/32 raise directly and never reach the Mach fault
        // handler, so nothing else can attribute the #GP. Without publishing
        // the RIP here the exception is dispatched at whatever address the
        // block last stored, so the .pdata lookup and the unwind both start
        // from an instruction that did not fault and the guest's handler is
        // never found. Mirrors emit_flush_cold_mem_stubs().
        if (u32 insn = 0; owning_insn(c.branch_pos, insn)) {
            emit_update_rip(m_insn_rips[insn]);
        }
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::mov_x(1, c.ea_reg));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                               c.align32 ? &jit_require_align32
                                         : &jit_require_align16)));
        emit_helper_blr();
        emit_exit_if_pending_raise();
        emit_xmm_cache_reload(c.xmm_cache_live);
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(back, c.return_pos, 26,
                                           "align check return")));
    }
    m_cold_align_checks.clear();
}

}  // namespace vitreo::cpu::jit
