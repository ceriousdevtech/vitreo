// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler.cpp
/// @brief Copy-and-patch JIT compiler implementation (CPU-040)
///
/// Compiles decoded x86 BasicBlocks into ARM64 machine code using
/// direct instruction encoding. Uses lazy flag evaluation to avoid
/// computing x86 flags after every ALU instruction.
///
/// Register convention for JIT code:
///   X19 = CpuState*        (callee-saved, pinned)
///   X20 = JitContext*       (callee-saved, pinned — helper fn ptrs + memory)
///   W0-W15 = scratch registers
///   X16-X17 = intra-procedure scratch

#include <cstdint>
///   X30 = LR (saved/restored in prologue/epilogue for BLR calls)

#include "jit_compiler.hpp"
#include "stencils.hpp"
#include "jit_coverage.hpp"
#include "jit_stats.hpp"
#include "flag_liveness.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "../../memory/mem_watch.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

/// @file instruction_compilation.cpp
/// @brief Instruction compilation
///
/// Closed cut from jit_compiler.cpp — verified with plan_extraction.py.

namespace vitreo::cpu::jit {

// ============================================================================
// Instruction compilation
// ============================================================================

namespace {

/// True when `instr` is a MOV-family instruction whose 64-bit lowering in
/// compile_64bit is architecturally flag-neutral AND leaves host NZCV intact.
///
/// Intel SDM Vol. 2 "MOV": Flags Affected = None. On the emit side there are
/// exactly two shapes that keep host NZCV:
///
///   * no memory operand at all -- reg<-reg / reg<-imm lowerings are
///     MOV/MOVZ/MOVK/LDR/STR/BFI against CpuState or a pin, none of which
///     writes NZCV, and emit_compute_ea64 is likewise ADD/SUB/LSL/LDR only;
///   * a memory DESTINATION with a non-memory source -- emit_guest_store{8,16,
///     32,64}_64 bracket emit_x64_region_store with emit_nzcv_spill/fill
///     whenever should_hold_nzcv() is true, which it is precisely when a
///     caller is relying on NZCV.
///
/// A memory SOURCE is excluded on purpose. emit_x64_region_load
/// (jit_compiler.cpp) bounds-checks with two SUBS and does NOT spill/fill, so
/// `MOV r32, [mem]` really does destroy NZCV. Claiming otherwise is the
/// recorded ADD;MOV r,[mem];ADC bug that silently dropped the carry
/// (InterpreterOracleContract.BattleNetOpenSslMulAddCarryWindow).
bool mov64_preserves_host_nzcv(const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::MOV_R64_IMM64:
    case Opcode::MOV_R64_RM64:
    case Opcode::MOV_RM64_R64:
    case Opcode::MOV_RM64_IMM32:
    case Opcode::MOV_R32_RM32:
    case Opcode::MOV_RM32_R32:
    case Opcode::MOV_R32_IMM32:
    case Opcode::MOV_RM32_IMM32:
    case Opcode::MOV_R16_RM16:
    case Opcode::MOV_RM16_R16:
    case Opcode::MOV_R16_IMM16:
    case Opcode::MOV_RM16_IMM16:
    case Opcode::MOV_R8_IMM8:
    case Opcode::MOV_R8_RM8:
    case Opcode::MOV_RM8_R8:
    case Opcode::MOV_RM8_IMM8:
        break;
    default:
        return false;
    }
    // Any source operand in memory routes through emit_x64_region_load.
    for (u8 i = 1; i < instr.operand_count; ++i) {
        if (is_memory_operand(instr.operands[i].type)) return false;
    }
    return true;
}

/// MOV / MOVZX / MOVSX / MOVSXD with a memory source: "Flags Affected: None"
/// (Intel SDM Vol. 2). Their linear64 lowering is EA arithmetic, an inline
/// NZCV-clean load whose cold stub saves live NZCV, and a GPR write, so host
/// NZCV survives unless the load reports otherwise (load_clobbered_nzcv).
bool mov64_load_is_flag_neutral(const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::MOV_R64_RM64: case Opcode::MOV_R32_RM32:
    case Opcode::MOV_R16_RM16: case Opcode::MOV_R8_RM8:
    case Opcode::MOVZX_R16_RM8: case Opcode::MOVZX_R32_RM8:
    case Opcode::MOVZX_R64_RM8: case Opcode::MOVZX_R32_RM16:
    case Opcode::MOVZX_R64_RM16:
    case Opcode::MOVSX_R16_RM8: case Opcode::MOVSX_R32_RM8:
    case Opcode::MOVSX_R64_RM8: case Opcode::MOVSX_R32_RM16:
    case Opcode::MOVSX_R64_RM16: case Opcode::MOVSXD_R64_RM32:
        break;
    default:
        return false;
    }
    return instr.operand_count == 2 &&
           is_register_operand(instr.operands[0].type) &&
           is_memory_operand(instr.operands[1].type);
}

/// True when @p instr writes no x86 flag and publishes no lazy-flag record,
/// so a pending CarrySource named by the PREVIOUS instruction is still an
/// accurate description of `flag_op`/`flag_src`/`flag_dst` after it runs.
///
/// This is a strictly weaker requirement than mov64_preserves_host_nzcv, and
/// the two must not be merged: that predicate asks whether the ARM64 PSTATE
/// survives the lowering, which a memory-source MOV's bounds check destroys.
/// The lazy RECORD survives regardless -- it lives in CpuState -- so a
/// carry consumer downstream of `add rax,rbx ; mov rcx,[rdx] ; adc rsi,0`
/// can still emit the one Add64 formula instead of the eight-way dispatch.
///
/// Every opcode here is "Flags Affected: None" in Intel SDM Vol. 2. CMOVcc
/// and SETcc READ flags and are equally transparent, but they are compiled
/// elsewhere and are left out rather than guessed at.
///
/// Only consulted on the natively-compiled path. A delegated instruction runs
/// the interpreter, which may materialize the pending record, so it must keep
/// the conservative Unknown.
bool carry_source_transparent(const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::MOV_R64_IMM64:
    case Opcode::MOV_R64_RM64:
    case Opcode::MOV_RM64_R64:
    case Opcode::MOV_RM64_IMM32:
    case Opcode::MOV_R32_RM32:
    case Opcode::MOV_RM32_R32:
    case Opcode::MOV_R32_IMM32:
    case Opcode::MOV_RM32_IMM32:
    case Opcode::MOV_R16_RM16:
    case Opcode::MOV_RM16_R16:
    case Opcode::MOV_R16_IMM16:
    case Opcode::MOV_RM16_IMM16:
    case Opcode::MOV_R8_IMM8:
    case Opcode::MOV_R8_RM8:
    case Opcode::MOV_RM8_R8:
    case Opcode::MOV_RM8_IMM8:
    case Opcode::MOVZX_R16_RM8:
    case Opcode::MOVZX_R32_RM8:
    case Opcode::MOVZX_R64_RM8:
    case Opcode::MOVZX_R32_RM16:
    case Opcode::MOVZX_R64_RM16:
    case Opcode::MOVSX_R16_RM8:
    case Opcode::MOVSX_R32_RM8:
    case Opcode::MOVSX_R64_RM8:
    case Opcode::MOVSX_R32_RM16:
    case Opcode::MOVSX_R64_RM16:
    case Opcode::MOVSXD_R64_RM32:
    case Opcode::LEA_R32_M:
    case Opcode::LEA_R64_M:
    case Opcode::BSWAP_R32:
    case Opcode::BSWAP_R64:
    case Opcode::XCHG_AX_R16:
    case Opcode::XCHG_EAX_R32:
    case Opcode::XCHG_RAX_R64:
    case Opcode::XCHG_RM8_R8:
    case Opcode::XCHG_RM16_R16:
    case Opcode::XCHG_RM32_R32:
    case Opcode::XCHG_RM64_R64:
    case Opcode::PUSH_R64:
    case Opcode::POP_R64:
    case Opcode::NOP:
    case Opcode::NOP_RM:
        return true;
    default:
        return false;
    }
}

}  // namespace

JitCompiler::FpRoundingClass
JitCompiler::fp_rounding_class(Opcode op) noexcept {
    const auto u = static_cast<u32>(op);
    // x87: the contiguous FNCLEX..FDIVR_M64 block plus the strays that sit
    // outside it. Mirrors is_fpu_opcode() in ops/fpu.cpp.
    if ((u >= static_cast<u32>(Opcode::FNCLEX) &&
         u <= static_cast<u32>(Opcode::FDIVR_M64)) ||
        op == Opcode::FFREE_STI || op == Opcode::FBLD_M80 ||
        op == Opcode::FBSTP_M80) {
        return FpRoundingClass::X87;
    }
    // SSE/AVX, but only the opcodes whose RESULT depends on MXCSR.RC. Being
    // over-inclusive here would be wrong in a way that matters: the integer
    // and data-movement SIMD lowerings are asserted to be helper-call free
    // (tests/cpu/test_jit_sse2_native.cpp scans for BLR X16), and a guard in
    // front of PXOR or MOVSS would break a real perf contract for an
    // instruction that cannot round.
    //
    // Three contiguous ranges, each closed by the enum's own section markers
    // in decode/opcodes.hpp:
    //   * SSE/SSE2/SSE3 floating-point arithmetic (ADDSS .. HSUBPS)
    //   * the conversion family (CVTSI2SS .. CVTTPD2PI) -- float<->int and
    //     float<->float both round, and the CVTT* forms that truncate
    //     unconditionally are covered by their own lowering
    //   * FMA3, which rounds once on the fused result
    const auto in = [u](Opcode lo, Opcode hi) {
        return u >= static_cast<u32>(lo) && u <= static_cast<u32>(hi);
    };
    if (in(Opcode::ADDSS_XMM_XMMM32, Opcode::HSUBPS_XMM_XMMM128) ||
        in(Opcode::CVTSI2SS_XMM_RM32, Opcode::CVTTPD2PI_MM_XMMM128) ||
        in(Opcode::VFMADD132PS_XMM_XMM_XMMM128,
           Opcode::VFMSUB132SS_XMM_XMM_XMMM32)) {
        return FpRoundingClass::Sse;
    }
    return FpRoundingClass::None;
}

bool JitCompiler::writes_fp_control_word(Opcode op) noexcept {
    switch (op) {
    case Opcode::FLDCW_M16:
    case Opcode::FLDENV_M28:
    case Opcode::FRSTOR_M108:
    case Opcode::FNINIT:
    case Opcode::LDMXCSR_M32:
    case Opcode::FXRSTOR_M512:
    // XRSTOR restores MXCSR with the SSE state component, so it changes the
    // SSE rounding and FTZ/DAZ modes exactly like FXRSTOR does.
    case Opcode::XRSTOR_M:
        return true;
    default:
        return false;
    }
}

void JitCompiler::emit_install_fp_rounding(FpRoundingClass cls) {
    // This guard sits in front of an FP instruction that does NOT write
    // EFLAGS, so it must not disturb a pending compare either: sequences like
    // `SUB ecx,1; MINPS; JNZ` deliberately keep NZCV alive across the SSE op.
    // A helper call destroys NZCV, so spill and refill around it -- the same
    // rule the x87 helper path already learned the hard way.
    emit_nzcv_spill();
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
        cls == FpRoundingClass::X87 ? &jit_fpu_apply_rounding
                                    : &jit_sse_apply_rounding)));
    emit_helper_blr();
    emit_nzcv_fill();
}

void JitCompiler::emit_block_fp_rounding_prologue(const BasicBlock& block) {
    // The per-instruction guard in compile_instruction() is emitted INSIDE the
    // block body. Any guest loop small enough to be one block therefore
    // re-executes it on every iteration: an NZCV spill, a four-word helper
    // address, a BLR and a fill in front of every ADDPS / MULSS / CVTSI2SD.
    //
    // That is the entire measured gap between packed/scalar FP SSE and integer
    // SSE of identical shape (ANDPS / PXOR / CMPPS are deliberately outside
    // FpRoundingClass and pay nothing): a flat ~3 ns per iteration that does
    // not depend on which FP op it guards, and that survives replacing the
    // lowered FADD with a plain AND.
    //
    // Hoisting keeps the contract intact -- MXCSR.RC / FCW.RC is still
    // installed before the first FP instruction executes, on the dispatcher
    // entry and on a linked hop alike, since this sits after m_link_entry_pos
    // and ahead of m_body_start_pos -- while making it once per block ENTRY
    // instead of once per loop iteration.
    //
    // Restricted to single-class blocks on purpose. The class tracking is
    // linear over the instruction list while control flow is not, so in a
    // block that mixes x87 and SSE a backward edge across a class transition
    // must keep re-installing from inside the body; those blocks are left
    // exactly as they were.
    FpRoundingClass cls = FpRoundingClass::None;
    for (u32 i = 0; i < block.instruction_count; ++i) {
        const FpRoundingClass c =
            fp_rounding_class(block.instructions[i].opcode);
        if (c == FpRoundingClass::None) continue;
        if (cls == FpRoundingClass::None) {
            cls = c;
        } else if (cls != c) {
            return;
        }
    }
    // SSE only. The x87 lowerings in ops/fpu.cpp emit their own
    // jit_fpu_apply_rounding calls around the forms that need FCW re-read
    // (measured: hoisting the X87 class loses PC=24 parity on FMUL m32 and
    // FSQRT), so the x87 class keeps the in-body guard it already has.
    if (cls != FpRoundingClass::Sse) return;
    // Intel SDM Vol. 1 10.2.3: MXCSR controls SSE independently of x87.
    // The math bridge certifies both the full MXCSR and SSE ownership, and
    // invalidates that certificate on x87/host-environment transitions.
    // A flag-neutral probe avoids publishing/reloading every GPR pin merely
    // to rediscover an unchanged environment. Null retains the helper path.
    constexpr u32 key_off = offsetof(JitContext, installed_sse_environment);
    constexpr u32 mxcsr_off = offsetof(CpuState, sse) + offsetof(SseState, mxcsr);
    static_assert(key_off % 8 == 0 && key_off / 8 < 4096);
    static_assert(mxcsr_off % 4 == 0 && mxcsr_off / 4 < 4096);
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, key_off / 8));
    const usize no_key = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(17, 16, 0));
    emit(arm64::ldr_w_uoff(16, REG_CPU_STATE, mxcsr_off / 4));
    emit(arm64::eor_x(17, 17, 16));
    const usize not_installed = m_code_buf.size();
    emit(0);
    // The install runs only when the environment changed (or there is no
    // key), so it is out of line: every block entry used to take a branch
    // over its ~22 words. A linked predecessor may have published live NZCV.
    // SSE arithmetic is EFLAGS-neutral, while this C++ call may clobber host
    // condition flags.
    std::vector<ColdArmEntry> entries;
    entries.push_back({no_key, [](i32 disp) { return arm64::cbz_x(16, disp); }});
    entries.push_back(
        {not_installed, [](i32 disp) { return arm64::cbnz_x(17, disp); }});
    defer_cold_arm(std::move(entries), [this]() {
        emit_nzcv_spill_if_kind();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            &jit_sse_apply_rounding)));
        emit_helper_blr();
        emit_nzcv_fill_if_kind();
    }, /*semantic_helper=*/false);
    m_fp_class_installed = cls;
}

// CLC and STC change CF through the overlay and leave the lazy record's KIND
// byte untouched, so a prediction about that byte survives them.
//
// classify_flag_effect() calls them Barrier, which is right for liveness -- they
// do write a flag -- but using it to drop the kind prediction as well is a
// coarser question answered with a finer instrument. The cost of the extra
// conservatism is large and specific: without a prediction, the next
// Jcc/SETcc/CMOVcc emits the generic lazy-condition walk, which tests the kind
// byte against every kind it can evaluate inline. Measured on the emission
// corpus, `ADD; CLC; JB` was 154 ARM words against 47 for `ADD; JZ` -- the same
// branch, three times the code, because one CLC sat in between.
//
// Correctness rests on two things, both verified rather than assumed:
//
//   1. The lowering (ops/arithmetic.cpp) writes rflags and cf_overlay and
//      never flag_op, so the predicted kind is still the byte that is there.
//   2. The predicted walk gates on the CF/OF overlay before its single arm,
//      exactly as the generic walk does, so a live overlay still reaches the
//      cond helper.
//
// CMC is deliberately NOT here. Its lowering calls a helper to read the
// current CF, and any helper BLR drops the prediction one branch earlier in
// this same function -- listing it would be dead code that reads as intent.
//
// The BT/BTS/BTR/BTC family qualifies for the same reason, checked the same
// way: every one of them writes CF through emit_bt_write_cf(), which stores
// cf_overlay and rflags and nothing else. BTS/BTR/BTC additionally modify the
// destination bit, which is not a flag.
//
// The rotate and shift families are NOT here, and that is not caution -- their
// lowerings call emit_set_lazy_flags(), so they do rewrite flag_op and a
// prediction cannot survive them.
//
// CMC is also not here. Its lowering calls a helper to read the current CF,
// and any helper BLR drops the prediction one branch earlier in
// compile_instruction() -- listing it would be dead code that reads as intent.
//
// A note on the memory forms of BT: their emitted code can reach a memory
// helper at run time on the cold path without this compile-time function
// having seen an emit_helper_blr(). That does not threaten correctness. The
// predicted walk keeps the runtime `CMP kind; B.NE helper` guard in front of
// its single arm, so a prediction that turns out stale costs a helper call and
// never a wrong answer. The risk of being wrong here is measured in
// instructions, not in semantics.
[[nodiscard]] bool JitCompiler::overlay_only_flag_writer(Opcode op) noexcept {
    if (op == Opcode::CLC || op == Opcode::STC) return true;
    // The 24 BT/BTS/BTR/BTC forms are contiguous in the opcode table.
    return op >= Opcode::BT_RM16_R16 && op <= Opcode::BTC_RM64_IMM8;
}

bool JitCompiler::compile_instruction(const DecodedInstruction& instr,
                                       u64 instr_rip, u64 next_rip,
                                       bool is_last) {
    m_diagnostic_instruction_rip = instr_rip;
    m_last_insn_delegated = false;
    m_flags.preserved_pred_nzcv = false;
    m_flags.lazy_kind_published = kLazyKindUnknown;
    m_flags.lazy_kind_publish_count = 0;
    m_flags.helper_blr_this_insn = false;
    m_flags.load_clobbered_nzcv = false;
    m_flags.semantic_helper_this_insn = false;
    // Forwarding is deliberately one-instruction wide.  Every lowering starts
    // invalid and must explicitly publish a write-through V0 result for the
    // immediately following instruction.
    m_xmm_v0_forward_in = m_xmm_v0_forward_out;
    m_xmm_v0_forward_out = 0xFF;
    m_xmm_v0_alias_in = m_xmm_v0_alias_out;
    m_xmm_v0_alias_out = 0xFF;
    m_x87_d0_forward_in = m_x87_d0_forward_out;
    m_x87_d0_forward_out = false;
    // compile_sse's VEX.128 bridge recursively presents a legacy-shaped
    // instruction to shared lowerings. Preserve the original encoding so the
    // PE32 legacy-only forwarding whitelist cannot consume or publish there.
    m_current_original_vex = instr.prefixes.vex;
    // The compiler only ever grows m_code_buf through emit(), one u32 per ARM
    // instruction, so this delta is the exact emitted-instruction count for
    // this x86 instruction. Reading vector::size() is cheaper than branching
    // on stats::enabled() first, and is observation-only either way.
    // ---- host rounding mode, per instruction CLASS -------------------------
    //
    // x87 arithmetic rounds per FCW.RC and SSE arithmetic per MXCSR.RC; those
    // are independent architectural fields and the host FPCR is one register,
    // so whichever was installed last silently governs the other class. That
    // is a wrong-results bug in both directions (measured: an x87 FLDCW
    // selecting chop re-rounded subsequent SSE arithmetic, and SSE ignored
    // MXCSR.RC entirely).
    //
    // The interpreter installs the right mode at exec_fpu / exec_sse_arith
    // entry. The JIT's native lowerings emit bare ARM fadd/fdiv, so they need
    // the same guarantee -- but emitting a guard before every FP instruction
    // would put a call on the SSE hot path. Instead the class is tracked at
    // COMPILE time and a guard is emitted only where it actually changes:
    // once for the first FP instruction of a block (the host mode on entry is
    // unknown, since blocks are reached from many predecessors) and again at
    // each x87<->SSE transition. A run of same-class ops emits nothing.
    //
    // The helper itself memoises on the mode already installed, so the common
    // repeat execution is a load, a compare and a not-taken branch.
    const FpRoundingClass fp_class = fp_rounding_class(instr.opcode);
    if (fp_class != FpRoundingClass::None && fp_class != m_fp_class_installed) {
        emit_install_fp_rounding(fp_class);
        m_fp_class_installed = fp_class;
    }

    const usize stats_arm_before = m_code_buf.size();
    const bool compiled =
        compile_instruction_impl(instr, instr_rip, next_rip, is_last);

    // Anything that loads a control word changes the mode the NEXT op of that
    // class needs, so force the next FP instruction to re-install rather than
    // trusting the tracked class. (Those stencils install their own mode when
    // they execute, but the tracking cannot know which value they loaded.)
    if (writes_fp_control_word(instr.opcode)) {
        m_fp_class_installed = FpRoundingClass::None;
    }
    if (compiled) {
        // Lazy-record kind prediction (see m_flags.static_lazy_kind). A single
        // central publish this instruction is the new kind. A delegate or
        // any C++ helper may have rewritten the record; a flag writer that
        // did not go through a central publisher (or went through several)
        // is unknown; a flag-neutral instruction or a Jcc leaves it alone.
        if (m_last_insn_delegated || m_flags.helper_blr_this_insn) {
            m_flags.static_lazy_kind = kLazyKindUnknown;
        } else if (m_flags.lazy_kind_publish_count == 1) {
            m_flags.static_lazy_kind = m_flags.lazy_kind_published;
        } else if (m_flags.lazy_kind_publish_count > 1) {
            m_flags.static_lazy_kind = kLazyKindUnknown;
        } else if (classify_flag_effect(instr) != FlagEffect::Neutral &&
                   !is_jcc_opcode(instr.opcode) &&
                   !overlay_only_flag_writer(instr.opcode)) {
            m_flags.static_lazy_kind = kLazyKindUnknown;
        }
        switch (m_flags.nzcv) {
        case NzcvState::Sub:
        case NzcvState::And:
        case NzcvState::Add:
        case NzcvState::IncDec:
        case NzcvState::Sub8:
        case NzcvState::Carried:
            m_flags.pred_nzcv_live = true;
            break;
        default:
            if (!m_flags.preserved_pred_nzcv) {
                m_flags.pred_nzcv_live = false;
            }
            break;
        }
        if (!m_last_insn_delegated) {
            ++m_block_native_insns;
        }
        if (coverage::enabled()) {
            // Three-way, keyed by form. `Helper` is an instruction whose
            // architectural result is computed by jit_sse_op / jit_fpu_op and
            // friends: emitted ARM64 exists, but it is a call into the
            // interpreter's own semantics, so it is not a native lowering.
            const LowerKind kind =
                m_last_insn_delegated          ? LowerKind::Delegate
                : m_flags.semantic_helper_this_insn ? LowerKind::Helper
                                                    : LowerKind::Native;
            coverage::record_compiled(instr.opcode, form_key(instr),
                                      m_block_is_64bit, kind);
        }
        // ---- VITREO_JIT_STATS / VITREO_JIT_DISASM: emitted-ARM accounting ----
        if (stats::enabled()) {
            const u32 arm_words =
                static_cast<u32>(m_code_buf.size() - stats_arm_before);
            stats::record_insn(instr.opcode, m_block_is_64bit,
                               !m_last_insn_delegated, arm_words);
            u8 mem = 0;
            for (u8 i = 0; i < instr.operand_count; ++i) {
                if (is_memory_operand(instr.operands[i].type)) {
                    mem |= 1u;
                    if (i == 0) mem |= 2u;
                }
            }
            m_stats_insns.push_back(stats::InsnSite{
                instr_rip, static_cast<u32>(instr.opcode),
                static_cast<u32>(instr.length),
                static_cast<u32>(stats_arm_before), arm_words, mem,
                static_cast<u8>(m_last_insn_delegated ? 0u : 1u)});
        }
        // ---------------------------------------------------------------------
    }
    return compiled;
}

bool JitCompiler::compile_instruction_impl(const DecodedInstruction& instr,
                                            u64 instr_rip, u64 next_rip,
                                            bool is_last) {
    // Carry the NZCV validity from the previous instruction into this one (the
    // Jcc reads m_flags.incoming_nzcv), then clear it so only a fusable compare
    // emitted below can re-arm it for the *next* instruction.
    m_flags.incoming_nzcv = m_flags.nzcv;
    m_flags.nzcv = NzcvState::None;
    m_flags.incoming_carry_source = m_flags.pending_carry_source;
    m_flags.pending_carry_source = CarrySource::Unknown;
    // Only the instruction immediately after a publisher may claim its
    // skipped flag store; anything further along may have clobbered the
    // operand registers.
    m_incoming_stub_store = m_pending_stub_store;
    m_pending_stub_store = PendingStubStore{};
    m_incoming_alu32_nz_result = m_pending_alu32_nz_result;
    m_pending_alu32_nz_result = 0xFF;

    // INT_1 (F1, ICEBP) belongs with the others: Intel SDM Vol. 2 INT n/INTO/
    // INT3/INT1 -- it raises #DB directly rather than through the IDT, which
    // exec_int_ops models and no lowering here does. Without it the JIT ran
    // the instruction as a no-op and continued, while the interpreter trapped.
    // Found by the sweep's fault column, which is the only gate that can see a
    // divergence where one engine raises and the other does not.
    if (instr.opcode == Opcode::HLT || instr.opcode == Opcode::SYSCALL ||
        instr.opcode == Opcode::SYSENTER || instr.opcode == Opcode::SYSEXIT ||
        instr.opcode == Opcode::INT_IMM8 || instr.opcode == Opcode::INTO ||
        instr.opcode == Opcode::INT_1) {
        return false;
    }

    if (instr.prefixes.lock) {
        // Native LOCK helpers (INC/DEC/XADD/CMPXCHG/OR/AND). On miss, skip
        // non-atomic native stencils and use the interpreter delegate below so
        // mid-block LOCK OR/AND (atomic flag words and work-queue sets) stay
        // atomic instead of truncating the JIT block — Intel SDM Vol. 2 LOCK.
        if (compile_locked(instr, instr_rip, next_rip, is_last)) {
            return true;
        }
    } else {
    const bool has_native = has_stencil(instr.opcode);

    static const bool disable_control = (std::getenv("VITREO_JIT_DISABLE_CONTROL") != nullptr);
    static const bool disable_memory_ops = (std::getenv("VITREO_JIT_DISABLE_MEMORY_OPS") != nullptr);
    static const bool disable_string_ops = (std::getenv("VITREO_JIT_DISABLE_STRING_OPS") != nullptr);
    static const bool disable_sse_ops = (std::getenv("VITREO_JIT_DISABLE_SSE_OPS") != nullptr);
    static const bool disable_mov_ops = (std::getenv("VITREO_JIT_DISABLE_MOV_OPS") != nullptr);
    static const bool disable_arith_ops = (std::getenv("VITREO_JIT_DISABLE_ARITH_OPS") != nullptr);
    static const bool disable_shift_ops = (std::getenv("VITREO_JIT_DISABLE_SHIFT_OPS") != nullptr);
    static const bool disable_muldiv_ops = (std::getenv("VITREO_JIT_DISABLE_MULDIV_OPS") != nullptr);
    static const bool disable_stack_ops = (std::getenv("VITREO_JIT_DISABLE_STACK_OPS") != nullptr);
    static const bool disable_fpu_ops = (std::getenv("VITREO_JIT_DISABLE_FPU_OPS") != nullptr);
    static const bool disable_mmx = (std::getenv("VITREO_JIT_NO_MMX") != nullptr);
    static const bool disable_64bit_native =
        (std::getenv("VITREO_JIT_64BIT_NO_NATIVE") != nullptr);

    // A register INC/DEC whose flags die at the next non-faulting full writer
    // needs only the value update. Use the normal width-aware register helpers
    // so AH/REX byte selection, partial writes, and x64 zero-extension retain
    // their usual contract. The liveness proof never crosses an observer or
    // fault and never marks a memory/LOCK form (Intel SDM Vol. 2A INC/DEC).
    if (m_omit_lazy_current && !disable_arith_ops &&
        (!m_block_is_64bit || !disable_64bit_native) &&
        instr.operand_count == 1 && is_register_operand(instr.operands[0].type) &&
        instr.opcode >= Opcode::INC_R16 && instr.opcode <= Opcode::DEC_RM64) {
        const auto& dst = instr.operands[0];
        const bool increment = instr.opcode <= Opcode::INC_RM64;
        const u8 width = operand_size(dst.type);
        const bool rex = instr.prefixes.has_rex();
        switch (width) {
        case 1: emit_load_gpr8(REG_SCRATCH0, dst.reg, rex); break;
        case 2: emit_load_gpr16(REG_SCRATCH0, dst.reg); break;
        case 4: emit_load_gpr32(REG_SCRATCH0, dst.reg); break;
        case 8: emit_load_gpr64(REG_SCRATCH0, dst.reg); break;
        default: return false;
        }
        if (width == 8) {
            emit(increment ? arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1)
                           : arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        } else {
            emit(increment ? arm64::add_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1)
                           : arm64::sub_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        }
        switch (width) {
        case 1: emit_store_gpr8(REG_SCRATCH0, dst.reg, rex); break;
        case 2: emit_store_gpr16(REG_SCRATCH0, dst.reg); break;
        case 4: emit_store_gpr32(REG_SCRATCH0, dst.reg); break;
        case 8: emit_store_gpr64(REG_SCRATCH0, dst.reg); break;
        }
        return true;
    }

    // In 64-bit (long) mode, every native stencil is unsafe — they all
    // assume 32-bit x86 guest register semantics (no REX, no upper-half
    // GPRs, 32-bit RIP advance). Route every instruction through the
    // universal interpreter-delegate path below. This preserves correctness
    // 100% (the interpreter is the source of truth for x64 semantics) and
    // still gains block-level dispatch amortization vs. the interpreter's
    // per-insn main loop.
    if (!m_block_is_64bit && has_native) {
        if (!disable_mov_ops
            && compile_mov(instr, instr_rip, next_rip, is_last)) {
            m_flags.pending_carry_source = m_flags.incoming_carry_source;
            // MOV is flag-neutral. Register MOVs emit only ldr/str/movz/movk.
            // Linear 32-bit MOV r, [mem] is a UXTW LDR (no SUBS). Non-linear
            // loads still clobber NZCV on the bounds check. MOV [mem], r
            // spills/fills when predecessor NZCV is live. LEA only ADDs an
            // EA, so NZCV from a prior CMP survives (`CMP; LEA; Jcc`).
            const bool lea = instr.opcode == Opcode::LEA_R32_M ||
                             instr.opcode == Opcode::LEA_R64_M;
            const bool reg_src = instr.operand_count > 1 &&
                !is_memory_operand(instr.operands[1].type);
            const bool linear_load = !m_block_is_64bit && m_cfg.linear_jit &&
                m_memory.is_linear() && instr.operand_count > 1 &&
                is_memory_operand(instr.operands[1].type);
            const bool mem_dst = instr.operand_count > 0 &&
                is_memory_operand(instr.operands[0].type);
            if (lea || reg_src || linear_load) {
                m_flags.nzcv = m_flags.incoming_nzcv;
                m_flags.preserved_pred_nzcv = true;
            } else if (mem_dst && m_flags.pred_nzcv_live) {
                // MOV [mem], r spills/fills NZCV when predecessor flags
                // are live (should_hold_nzcv). Keep the compile-time kind
                // so `CMP; MOV [mem], r; Jcc` fuses from the restored NZCV
                // (Intel SDM Vol. 2 MOV / Jcc).
                m_flags.nzcv = m_flags.incoming_nzcv;
                m_flags.preserved_pred_nzcv = true;
            }
            // Otherwise this is a NON-LINEAR MOV r, [mem]: the two-region
            // bounds check in emit_guest_load32 is a pair of SUBS, and unlike
            // the store path it does not spill/fill NZCV. So NZCV really is
            // destroyed and m_flags.nzcv must stay None.
            //
            // This arm used to be reached by loads too, claiming the flags
            // survived on the strength of the store path's spill/fill. The
            // consumer that believed it was ADC/SBB: `ADD eax,ecx;
            // MOV ecx,[ebp+0x10]; ADC esi,0` took the "directly preceding ADD
            // leaves NZCV live" fast path in compile_arithmetic and did CSET
            // CS on whatever the bounds check had left behind, silently
            // dropping the carry. Found by
            // InterpreterOracleContract.BattleNetOpenSslMulAddCarryWindow --
            // OpenSSL bignum carry propagation, which is exactly the shape
            // that notices.
            return true;
        }
        if (!disable_arith_ops
            && compile_arithmetic(instr, instr_rip, next_rip, is_last)) {
            switch (instr.opcode) {
            case Opcode::ADD_RM32_R32:
            case Opcode::ADD_R32_RM32:
            case Opcode::ADD_RM32_IMM32:
            case Opcode::ADD_RM32_IMM8:
            case Opcode::ADD_EAX_IMM32:
                m_flags.pending_carry_source = CarrySource::Add32;
                break;
            case Opcode::ADC_R32_RM32:
            case Opcode::ADC_RM32_R32:
            case Opcode::ADC_RM32_IMM32:
            case Opcode::ADC_RM32_IMM8:
                m_flags.pending_carry_source = CarrySource::Adc32;
                break;
            case Opcode::SUB_RM32_R32:
            case Opcode::SUB_R32_RM32:
            case Opcode::SUB_RM32_IMM32:
            case Opcode::SUB_RM32_IMM8:
            case Opcode::SUB_EAX_IMM32:
            case Opcode::CMP_RM32_R32:
            case Opcode::CMP_R32_RM32:
            case Opcode::CMP_RM32_IMM32:
            case Opcode::CMP_RM32_IMM8:
            case Opcode::CMP_EAX_IMM32:
                m_flags.pending_carry_source = CarrySource::Sub32;
                break;
            case Opcode::SBB_R32_RM32:
            case Opcode::SBB_RM32_R32:
            case Opcode::SBB_RM32_IMM32:
            case Opcode::SBB_RM32_IMM8:
                m_flags.pending_carry_source = CarrySource::Sbb32;
                break;
            case Opcode::AND_RM32_R32:
            case Opcode::AND_R32_RM32:
            case Opcode::AND_RM32_IMM32:
            case Opcode::AND_RM32_IMM8:
            case Opcode::AND_EAX_IMM32:
            case Opcode::OR_RM32_R32:
            case Opcode::OR_R32_RM32:
            case Opcode::OR_RM32_IMM32:
            case Opcode::OR_RM32_IMM8:
            case Opcode::OR_EAX_IMM32:
            case Opcode::XOR_RM32_R32:
            case Opcode::XOR_R32_RM32:
            case Opcode::XOR_RM32_IMM32:
            case Opcode::XOR_RM32_IMM8:
            case Opcode::XOR_EAX_IMM32:
            case Opcode::TEST_RM32_R32:
            case Opcode::TEST_RM32_IMM32:
            case Opcode::TEST_EAX_IMM32:
                m_flags.pending_carry_source = CarrySource::Logic32;
                break;
            case Opcode::NOT_RM8:
            case Opcode::NOT_RM32:
            case Opcode::BSWAP_R32:
            case Opcode::BSWAP_R64:
            case Opcode::CLD:
            case Opcode::STD:
                m_flags.pending_carry_source = m_flags.incoming_carry_source;
                break;
            default:
                break;
            }
            return true;
        }
        if (!disable_shift_ops && compile_shifts(instr, instr_rip, next_rip, is_last)) return true;
        if (!disable_muldiv_ops
            && compile_mul_div(instr, instr_rip, next_rip, is_last)) {
            if (instr.opcode == Opcode::MUL_RM32
                || instr.opcode == Opcode::MUL_RM16
                || instr.opcode == Opcode::MUL_RM8) {
                m_flags.pending_carry_source = CarrySource::Mul32;
            }
            return true;
        }
        if (!disable_control && compile_control(instr, instr_rip, next_rip, is_last)) return true;
        if (!disable_stack_ops && compile_stack(instr, instr_rip, next_rip, is_last)) {
            // Intel SDM Vol. 2 PUSH/POP: EFLAGS unchanged. Carry a live
            // CMP/TEST NZCV so `CMP; POP; Jcc` fuses from the compare
            // (hash-table end-iterator checks) instead of a helper that
            // can see stale lazy flags. PUSH stores spill/fill when
            // predecessor NZCV is live; linear POP is a flag-neutral LDR.
            if (m_flags.incoming_nzcv != NzcvState::None || m_flags.pred_nzcv_live) {
                m_flags.nzcv = m_flags.incoming_nzcv;
                m_flags.preserved_pred_nzcv = true;
            }
            return true;
        }
        if (!disable_memory_ops && compile_memory_ops(instr, instr_rip, next_rip, is_last)) {
            // Intel SDM Vol. 2 XCHG: operands swap; EFLAGS unchanged.
            switch (instr.opcode) {
            case Opcode::XCHG_EAX_R32:
            case Opcode::XCHG_RM16_R16:
            case Opcode::XCHG_RM32_R32:
            case Opcode::XCHG_RM64_R64:
                if (m_flags.incoming_nzcv != NzcvState::None || m_flags.pred_nzcv_live) {
                    m_flags.nzcv = m_flags.incoming_nzcv;
                    m_flags.preserved_pred_nzcv = true;
                }
                break;
            default:
                break;
            }
            return true;
        }
        if (!disable_string_ops && compile_string_ops(instr, instr_rip, next_rip, is_last)) return true;
        if (!disable_fpu_ops && compile_fpu(instr, instr_rip, next_rip, is_last)) {
            // Intel SDM Vol. 2 FLD/FST/FSTP: EFLAGS unchanged. Native
            // paths spill/fill around their Inf/NaN SUBS gates.
            switch (instr.opcode) {
            case Opcode::FLD_M32:
            case Opcode::FST_M32:
            case Opcode::FSTP_M32:
                m_flags.nzcv = m_flags.incoming_nzcv;
                m_flags.preserved_pred_nzcv = true;
                break;
            default:
                break;
            }
            return true;
        }
        // Native MMX packed ops before compile_sse: SSE still has helper
        // cases for PAND/POR/PXOR/PADDQ/PUNPCK* MM that would otherwise win.
        if (!disable_mmx && compile_mmx(instr, instr_rip, next_rip, is_last)) {
            return true;
        }
        if (!disable_sse_ops
            && compile_sse(instr, instr_rip, next_rip, is_last)) {
            // Intel SDM Vol. 2: packed/scalar SSE arith, moves, and shuffles
            // do not write EFLAGS. Only register-only NEON that is LDR/STR
            // plus a flag-neutral vector op may keep a prior CMP/SUB NZCV
            // (`SUB ecx,1; MINPS; JNZ`). ADDPS/MULSS/DIVPS spill/fill
            // around their Inf/NaN SUBS gates. COMISS writes EFLAGS.
            const bool writes_eflags =
                instr.opcode == Opcode::COMISS_XMM_XMMM32 ||
                instr.opcode == Opcode::UCOMISS_XMM_XMMM32 ||
                instr.opcode == Opcode::COMISD_XMM_XMMM64 ||
                instr.opcode == Opcode::UCOMISD_XMM_XMMM64 ||
                instr.opcode == Opcode::PTEST_XMM_XMMM128 ||
                instr.opcode == Opcode::PCMPISTRI_XMM_XMMM128_IMM8 ||
                instr.opcode == Opcode::PCMPISTRM_XMM_XMMM128_IMM8 ||
                instr.opcode == Opcode::PCMPESTRI_XMM_XMMM128_IMM8 ||
                instr.opcode == Opcode::PCMPESTRM_XMM_XMMM128_IMM8;
            bool any_mem = false;
            for (u8 oi = 0; oi < instr.operand_count; ++oi) {
                if (is_memory_operand(instr.operands[oi].type)) {
                    any_mem = true;
                    break;
                }
            }
            const bool host_nzcv_safe =
                instr.opcode == Opcode::MINPS_XMM_XMMM128 ||
                instr.opcode == Opcode::MAXPS_XMM_XMMM128 ||
                instr.opcode == Opcode::MINPD_XMM_XMMM128 ||
                instr.opcode == Opcode::MAXPD_XMM_XMMM128 ||
                instr.opcode == Opcode::ANDPS_XMM_XMMM128 ||
                instr.opcode == Opcode::ORPS_XMM_XMMM128 ||
                instr.opcode == Opcode::XORPS_XMM_XMMM128 ||
                instr.opcode == Opcode::ANDNPS_XMM_XMMM128 ||
                instr.opcode == Opcode::ANDPD_XMM_XMMM128 ||
                instr.opcode == Opcode::ORPD_XMM_XMMM128 ||
                instr.opcode == Opcode::XORPD_XMM_XMMM128 ||
                instr.opcode == Opcode::ANDNPD_XMM_XMMM128 ||
                instr.opcode == Opcode::MOVAPS_XMM_XMMM128 ||
                instr.opcode == Opcode::MOVAPS_XMMM128_XMM ||
                instr.opcode == Opcode::MOVSS_XMM_XMMM32 ||
                instr.opcode == Opcode::MOVSS_XMMM32_XMM ||
                instr.opcode == Opcode::MOVSD_XMM_XMMM64 ||
                instr.opcode == Opcode::MOVSD_XMMM64_XMM ||
                instr.opcode == Opcode::SHUFPS_XMM_XMMM128_IMM8 ||
                instr.opcode == Opcode::SHUFPD_XMM_XMMM128_IMM8 ||
                instr.opcode == Opcode::ADDSS_XMM_XMMM32 ||
                instr.opcode == Opcode::ADDSD_XMM_XMMM64 ||
                instr.opcode == Opcode::SUBSS_XMM_XMMM32 ||
                instr.opcode == Opcode::SUBSD_XMM_XMMM64 ||
                instr.opcode == Opcode::MULSS_XMM_XMMM32 ||
                instr.opcode == Opcode::MULSD_XMM_XMMM64 ||
                instr.opcode == Opcode::DIVSS_XMM_XMMM32 ||
                instr.opcode == Opcode::DIVSD_XMM_XMMM64 ||
                instr.opcode == Opcode::ADDPS_XMM_XMMM128 ||
                instr.opcode == Opcode::ADDPD_XMM_XMMM128 ||
                instr.opcode == Opcode::SUBPS_XMM_XMMM128 ||
                instr.opcode == Opcode::SUBPD_XMM_XMMM128 ||
                instr.opcode == Opcode::MULPS_XMM_XMMM128 ||
                instr.opcode == Opcode::MULPD_XMM_XMMM128 ||
                instr.opcode == Opcode::DIVPS_XMM_XMMM128 ||
                instr.opcode == Opcode::DIVPD_XMM_XMMM128;
            if (!writes_eflags) {
                m_flags.pending_carry_source = m_flags.incoming_carry_source;
                const bool dest_mem = instr.operand_count > 0 &&
                    is_memory_operand(instr.operands[0].type);
                const bool linear_mem = any_mem && !m_block_is_64bit &&
                    m_cfg.linear_jit && m_memory.is_linear();
                // Register-only and linear loads emit no SUBS. 32-bit
                // stores (emit_guest_store32) and 128-bit MOVAPS/MOVUPS
                // stores spill/fill when predecessor NZCV is live.
                if (host_nzcv_safe &&
                    ((!any_mem) ||
                     (linear_mem && !dest_mem) ||
                     (dest_mem && should_hold_nzcv()))) {
                    m_flags.nzcv = m_flags.incoming_nzcv;
                    m_flags.preserved_pred_nzcv = true;
                }
            }
            return true;
        }
    }

    // MMX (MOVD/MOVQ/PMULUDQ/PSRLQ/PSLLQ) has no stencil, so has_native is
    // false for it and the block above is skipped — call compile_mmx directly.
    // This is the self-extract hot path: the decompressor runs in
    // the 32-bit PE bootstrapper (hot RIPs in the 0x0040xxxx image range), so
    // these ops must be JIT'd in 32-bit mode, not just 64-bit.
    if (!m_block_is_64bit
        && !disable_mmx
        && compile_mmx(instr, instr_rip, next_rip, is_last)) {
        return true;
    }

    // 64-bit native codegen for hot REX.W opcodes (MOV/ALU/LEA/PUSH/POP/...).
    // Falls through to the delegate fallback below for any opcode not yet
    // covered, so correctness is preserved.
    if (m_block_is_64bit
        && !disable_64bit_native) {
        bool has_implicit_memory_operand = false;
        // String ops decode with ZERO operands -- their memory access is
        // implicit through RSI/RDI -- so the loop above misses them while
        // compile_string_ops still emits guest loads and stores. An access
        // violation inside one would then be raised at whatever RIP the
        // previous memory-operand instruction left behind, because
        // jit_raise_access_violation deliberately does not set RIP (it assumes
        // the caller already did). SEH/VEH would dispatch with the wrong
        // ExceptionAddress and the wrong resume point.
        //
        // Only reachable since string ops were routed into 64-bit blocks; the
        // interpreter delegate used to set RIP per instruction.
        switch (instr.opcode) {
        case Opcode::MOVSB: case Opcode::MOVSW:
        case Opcode::MOVSD_STR: case Opcode::MOVSQ:
        case Opcode::STOSB: case Opcode::STOSW:
        case Opcode::STOSD: case Opcode::STOSQ:
        case Opcode::LODSB: case Opcode::LODSW:
        case Opcode::LODSD: case Opcode::LODSQ:
        case Opcode::SCASB: case Opcode::SCASW:
        case Opcode::SCASD: case Opcode::SCASQ:
        case Opcode::CMPSB: case Opcode::CMPSW:
        case Opcode::CMPSD_STR: case Opcode::CMPSQ:
            has_implicit_memory_operand = true;
            break;
        default:
            break;
        }
        // Explicit native memory operands recover signal faults from the
        // host-PC map, while their checked-helper tails publish RIP only when
        // the cold path is taken. String operations remain helper-oriented and
        // retain their instruction-entry publication.
        if (has_implicit_memory_operand) {
            emit_update_rip(instr_rip);
        }
        if (compile_64bit(instr, instr_rip, next_rip, is_last)) {
            // Flag-neutral instructions must hand the predecessor's NZCV
            // kind on to the next one, or a Jcc one instruction away from
            // its CMP falls off fused_arm_cond and pays the ~230-instruction
            // emit_eval_lazy_condition walk instead of one B.cond.
            //
            // The 32-bit path has done this since the CMP/TEST fusion work
            // (see the MOV arm above); the 64-bit path only ever restored
            // LEA, which is why `cmp_mov_jcc` measured 11.03x against
            // `cmp_jcc`'s 7.34x on byte-identical x86-64
            // (.docs/jit-profile-2026-08-22.md). Intel SDM Vol. 2: MOV and
            // LEA both list "Flags Affected: None".
            //
            // m_flags.preserved_pred_nzcv must be set even when m_flags.incoming_nzcv is
            // None: compile_instruction clears m_flags.pred_nzcv_live on any
            // instruction that does not claim to have preserved it, and a
            // linked predecessor's runtime nzcv_kind is still usable.
            if (instr.opcode == Opcode::LEA_R64_M ||
                instr.opcode == Opcode::LEA_R32_M ||
                mov64_preserves_host_nzcv(instr) ||
                (mov64_load_is_flag_neutral(instr) &&
                 !m_flags.load_clobbered_nzcv &&
                 !m_flags.helper_blr_this_insn &&
                 m_cfg.linear64 && m_linear_base_pinned)) {
                m_flags.nzcv = m_flags.incoming_nzcv;
                m_flags.preserved_pred_nzcv = true;
            }
            // The lazy record outlives the host PSTATE, so the carry source
            // forwards across a wider set of instructions than NZCV does.
            // Without this the source dies at the first flag-neutral
            // instruction that happens to clobber NZCV, and the consumer pays
            // the eight-way FlagOp dispatch with a perfectly good answer
            // already named.
            if (carry_source_transparent(instr)) {
                m_flags.pending_carry_source = m_flags.incoming_carry_source;
            }
            return true;
        }
        // SSE/SSE2 lowerings live in compile_sse and were historically
        // 32-bit-only because every native stencil assumed 32-bit EA math.
        // emit_compute_ea now forwards to emit_compute_ea64 in long mode, so
        // the same lowerings are safe here — and they are the dominant
        // remaining x64 delegate cost (MOVD/MOVQ/PUNPCKLDQ/ADDSD/MULSD/…).
        if (!disable_mmx
            && compile_mmx(instr, instr_rip, next_rip, is_last)) {
            return true;
        }
        if (!disable_sse_ops
            && compile_sse(instr, instr_rip, next_rip, is_last)) {
            if (instr.opcode == Opcode::CRC32_R32_RM8
                || instr.opcode == Opcode::CRC32_R64_RM8
                || instr.opcode == Opcode::CRC32_R32_RM16
                || instr.opcode == Opcode::CRC32_R32_RM32
                || instr.opcode == Opcode::CRC32_R64_RM64) {
                m_flags.pending_carry_source = m_flags.incoming_carry_source;
                if (instr.operand_count < 2
                    || !is_memory_operand(instr.operands[1].type)) {
                    m_flags.nzcv = m_flags.incoming_nzcv;
                }
            }
            return true;
        }
        // 32-bit BT/BSF/SETcc/CMOV lowerings in compile_memory_ops are valid in
        // long mode too (destinations are zero-extended 32-bit writes). Their
        // EA path now forwards to emit_compute_ea64.
        if (!disable_memory_ops
            && compile_memory_ops(instr, instr_rip, next_rip, is_last)) {
            return true;
        }
        // x87 in long mode. The x87 register file is the stack, not the GPRs,
        // so none of the REX / upper-half / 32-bit-register concerns that make
        // the other 32-bit lowerings unsafe here apply. The one thing that IS
        // different is the effective address — RIP-relative operands, R8-R15
        // bases, full 64-bit displacements — and emit_compute_ea already
        // forwards to emit_compute_ea64 in long mode.
        //
        // Without this, every x87 instruction in a 64-bit block delegated and
        // X87 read 0% on JIT64: the lowerings existed, the dispatch just never
        // reached them. Validated by tests/oracle/test_oracle_x87_64.cpp,
        // which exercises loads/stores/arith through a 64-bit base, the
        // integer-operand family, the control/status word, and the compares.
        if (!disable_fpu_ops
            && compile_fpu(instr, instr_rip, next_rip, is_last)) {
            return true;
        }
    }
    }  // !instr.prefixes.lock — native (non-LOCK) stencils only

    // Intel SDM Vol. 2 UD2: raise #UD at the UD2 address and change no
    // architectural register or flag.  Guarded-terminal trace widening can
    // retain many such cold islands in one hot block; delegating each one
    // retains the full decoded IR and emits a large interpreter call body.
    // Publish the site-specific fault RIP here and share the cold exception
    // body across the block.  The constructor-captured opt-in is also part of
    // JitCompileMemo::CompiledKey, so process-wide memo reuse cannot alias the two encodings.
    if (m_block_is_64bit && instr.opcode == Opcode::UD2 &&
        m_cfg.native_ud2_enabled) {
        m_cold_ud2_exits.push_back(ColdUd2Exit{m_code_buf.size(), instr_rip});
        emit(0);  // B .cold_ud2 -- patched after the block body
        m_native_ud2_linear_unreachable = true;
        return true;
    }

    // No native stencil claimed this opcode. Emit a generic delegate that
    // calls the interpreter for exactly one instruction. The helper sets
    // cpu->rip = instr_rip, materialises any pending lazy flags (so the
    // interpreter sees canonical eflags), decodes & executes one
    // instruction via Interpreter::step(), and advances cpu->rip.
    // After return, cpu->rip == next_rip (for non-control-flow ops) and
    // lazy-flag state is clean (FlagOp::None) — so subsequent JIT code in
    // this block can safely read GPR/XMM/flags from CpuState.
    //
    // Note: control-flow ops (Jcc, JMP, RET, CALL) must not reach this
    // path — they all have native stencils. Delegated ops are pure-data
    // instructions (SIMD, string, BCD, rare arithmetic), whose rip advance
    // matches compile-time next_rip exactly.
    //
    // NOT a function-local static. Caching this latches whatever the
    // environment held at the FIRST delegate decision in the process, which
    // in a shared-process gtest binary is set by whichever test ran first.
    // check_native_parity relies on VITREO_JIT_NO_DELEGATE taking effect
    // per-test; with a cached read all 282 of those call sites silently
    // degrade to ordinary parity tests that a delegate satisfies. Compile
    // time only -- never on an executed path.
    const bool disable_delegate =
        (std::getenv("VITREO_JIT_NO_DELEGATE") != nullptr);
    if (disable_delegate) {
        return false;
    }

    // Everything below this point is the delegate path: no native lowering
    // claimed this opcode. Recorded by the compile_instruction wrapper.
    m_last_insn_delegated = true;

    // Delegate one instruction to the interpreter: pass a stable pointer to
    // *this block's* pre-decoded DecodedInstruction so the interpreter skips
    // fetch+decode (the dominant cost in the x64 extraction sample profile).
    // `&instr` aliases the immutable BasicBlock snapshot owned by the
    // resulting CompiledBlock, so synchronous GUI-pump re-entry may replace
    // the block-cache entry without dangling this baked pointer.
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
    // X2 = &instr (absolute host pointer) ; X3 = instr_rip (full u64).
    m_has_predecoded_delegate = true;
    note_unrelocatable_host_bake();
    emit_mov_imm64(2,
        static_cast<i64>(reinterpret_cast<uintptr_t>(&instr)));
    emit_mov_imm64(3, static_cast<i64>(instr_rip));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                            JIT_CTX_OFF_INTERP_DECODED >> 3));
    emit_helper_blr();  // BLR X16
    emit_exit_if_pending_raise();

    // Post-delegate rip-divergence check.  Most delegated opcodes leave
    // cpu->rip == next_rip (compile-time fall-through). But some (LOOP,
    // JCXZ/JECXZ, INTO, BOUND, plus future control-flow ops without a
    // native stencil) write a different rip.  In that case the JIT block
    // must exit immediately; otherwise it would keep running at the
    // compile-time next_rip and silently skip the branch.
    {
        u32 rip_off = rip_offset();
        if (m_block_is_64bit) {
            // 64-bit guests need a full u64 compare — RIP may be in the
            // upper canonical range (the default x64 image base is 0x140000000).
            emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, rip_off >> 3));
            u32 nlo = static_cast<u32>(next_rip);
            u32 nhi = static_cast<u32>(next_rip >> 32);
            emit_mov_imm32(REG_SCRATCH1, nlo);
            if (nhi != 0) {
                emit_mov_imm32(REG_SCRATCH2, nhi);
                // ORR X1, X1, X2, LSL #32
                emit(0xAA000000u
                     | (static_cast<u32>(REG_SCRATCH2) << 16)
                     | (32u << 10)
                     | (static_cast<u32>(REG_SCRATCH1) << 5)
                     | static_cast<u32>(REG_SCRATCH1));
            }
            emit(arm64::subs_x(31, REG_SCRATCH0, REG_SCRATCH1));
        } else {
            emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, rip_off >> 2));
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(next_rip));
            emit(arm64::subs_w(31, REG_SCRATCH0, REG_SCRATCH1));
        }
        u32 b_eq_idx = static_cast<u32>(m_code_buf.size());
        emit(0);  // placeholder B.EQ skip_exit
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_epilogue(JitExecResult::Continue);
        u32 after = static_cast<u32>(m_code_buf.size());
        i32 disp = static_cast<i32>((after - b_eq_idx) * 4);
        m_code_buf[b_eq_idx] = arm64::b_cond(arm64::EQ, disp);
    }

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

} // namespace vitreo::cpu::jit
