// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file lazy_flag_state.hpp
/// @brief The JIT's lazy-flag emission state, as one type.
///
/// x86 defines six flags after almost every arithmetic instruction and reads
/// them almost never, so the JIT records the operands and materializes the
/// flags only when something asks. Doing that well is the single largest lever
/// on emulated throughput -- flag materialization measures ~60% of real CPU on
/// hash and decompress loops -- and it is also where this subsystem's bugs
/// come from.
///
/// Before this type, these thirteen fields were thirteen members of
/// `JitCompiler`, indistinguishable among 135 others from the code arena, the
/// pin layout and the environment configuration. Nothing said they were one
/// machine, that they all reset together at a block boundary and at an
/// in-block join, or which of them may legally be read when.
///
/// The bug history is what that costs. Each of these shipped, and every one is
/// a violated invariant of the state below:
///
///   - a Jcc consulting the lazy `FlagOp` fast paths BEFORE the CF overlay,
///     so `ADD; CLC; JB` branched on the ADD's carry. Fixed, regressed, and
///     fixed again -- because "a live full-flag FlagOp implies no overlay" is
///     false by design: CLC/STC/CMC/BT install one over a live FlagOp.
///   - `Carried` NZCV fused off with a bare `B.cond`, reading NZCV a helper
///     had already destroyed.
///   - a fused forward Jcc taking an in-block edge while a deferred lazy-flag
///     store was still owed, dropping it on the taken path.
///   - flag liveness treating a Jcc as fall-through only, losing EFLAGS on
///     the taken edge.
///   - the x87 helper path never spilling NZCV.
///   - `MOV r,[mem]` documented as NZCV-preserving while its bounds-check
///     SUBS destroys it.
///   - a partial-flag op (ROR/ROL/BT) installing a lazy `FlagOp` and
///     overwriting pending flags.
///
/// Grouping the state is the first half of fixing that; the accessors that
/// make the invariants unrepresentable are the second, and are deliberately a
/// separate change so this one can be proved to emit identical code.
///
/// Lifetime: **one basic block**. Everything here is reset by
/// `reset_for_block()`, and the subset that a control-flow join invalidates is
/// reset by `reset_at_join()`.

#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>

namespace vitreo::cpu::jit {

// ---- CMP/TEST → Jcc flag fusion -------------------------------------
// The x86 CMP/TEST immediately before a Jcc leaves ARM64 NZCV valid (the
// compare is emitted as SUBS/ANDS). The following Jcc can then branch
// natively (CSET off NZCV) instead of calling a cond helper that
// re-materializes lazy flags. `nzcv` records what the just-compiled
// instruction left in NZCV; `incoming_nzcv` is that value carried into the
// next instruction's compilation (the Jcc reads it). Both reset to None for
// any instruction that isn't a fusable compare, so a stale NZCV can never
// be consumed. Lazy flags are ALSO set by the compare, so later flag reads
// stay correct — fusion is a pure fast-path for the branch.
// NzOnly: a nonzero-count shift republished its result with TST. Only N
// and Z are the result's SF/ZF; x86 CF (the shifted-out bit) and OF
// (count==1 only) live solely in the lazy record, and TST cleared ARM
// C/V. fused_arm_cond therefore accepts only Z/S conditions, and
// nzcv_kind_byte deliberately maps it to 0 so no runtime kind byte can
// let a cross-block carried Jcc or emit_cset_for_lazy_kind consume it.
// FpCmp: COMISS/UCOMISS/COMISD/UCOMISD left their FCMP's NZCV live. Intel
// SDM Vol. 2A defines their EFLAGS as ZF,PF,CF = 111 unordered, 000 greater,
// 001 less, 100 equal, with OF/SF/AF cleared; ARM FCMP gives N,Z,C,V = 0011,
// 0010, 1000, 0110 for the same outcomes. fused_arm_cond maps only the
// conditions one ARM condition can express from that (B/NB/BE/A/P/NP), and
// nzcv_kind_byte maps it to 0 like NzOnly, because no runtime kind-byte
// consumer knows the FCMP encoding.
enum class NzcvState : u8 {
    None, Sub, And, Add, IncDec, Sub8, Carried, NzOnly, FpCmp
};

// Track the lazy-CF producer while compiling one basic block. Unlike
// NZCV, this metadata survives flag-neutral MOV-family instructions
// because the lazy operands remain in CpuState. Carry consumers can then
// emit the one matching formula instead of an eight-way runtime dispatch.
// Unknown remains the conservative state at block entry and after any
// instruction whose flag effect is not classified here.
enum class CarrySource : u8 {
    Unknown,
    Add32,
    Adc32,
    Sub32,
    Sbb32,
    Mul32,
    Logic32,
    // 64-bit blocks. A 64-bit block records its lazy operands X-width
    // (emit_set_lazy_flags64), so the consumer's formula must load them
    // X-width too — hence separate values rather than a width flag.
    //
    // There is deliberately no Adc64/Sbb64: CpuState::FlagOp has none
    // either, because those records pack carry-in into bits 63:32 of
    // flag_src, which 64-bit operands already occupy. 64-bit ADC/SBB
    // never take the lazy path; they lower to ADCS/SBCS and keep the
    // carry in PSTATE.
    Add64,
    Sub64,
    Logic64,
};

/// "no prediction available" for the lazy-flag record's `kind` byte. At
/// namespace scope rather than inside LazyFlagState because it is a sentinel
/// value for that byte, not state, and every lowering that decides whether to
/// predict names it.
constexpr int kLazyKindUnknown = -1;

/// The JIT's per-block lazy-flag emission state.
///
/// Deliberately a plain aggregate for now: this type was introduced by moving
/// thirteen members verbatim, and the move was gated on the emitted ARM64
/// being byte-identical. Accessors that enforce the invariants above come
/// next, as their own change, so that a behavioural difference cannot hide
/// inside a mechanical one.
struct LazyFlagState {
    /// What the just-compiled instruction left in host NZCV.
    NzcvState nzcv{NzcvState::None};

    /// `nzcv` carried into the next instruction's compilation; the Jcc reads
    /// this one. Both reset to None for any instruction that is not a fusable
    /// compare, so a stale NZCV can never be consumed.
    NzcvState incoming_nzcv{NzcvState::None};

    /// Compile-time view of the lazy-flag record's `kind` byte
    /// (CpuState::flag_op), or kLazyKindUnknown.
    ///
    /// Advisory only: it chooses which ONE arm of the lazy-condition walk to
    /// emit, and that arm stays behind the runtime `CMP kind; B.NE helper`
    /// guard, so a stale prediction costs a helper call, never a wrong
    /// result. The generic walk is the semantic reference; the prediction is
    /// exactly the arm that walk would have selected for the same byte.
    ///
    /// Lifecycle: set at the end of compile_instruction() when the lowering
    /// published the record exactly once through a central writer
    /// (note_lazy_kind_publish); kept across flag-neutral instructions and
    /// Jcc; dropped by any helper BLR, any interpreter delegate, any other
    /// flag writer, and at every in-block join (the same reset point as
    /// `nzcv`).
    int static_lazy_kind{kLazyKindUnknown};
    int lazy_kind_published{kLazyKindUnknown};
    u8 lazy_kind_publish_count{0};
    bool helper_blr_this_insn{false};
    /// Set by a guest-memory load lowering that can destroy host NZCV (the
    /// region-cache bounds SUBS, the shared hot-guard BLR). The linear64
    /// inline load is NZCV-clean and its cold stub saves NZCV when it is
    /// live, so a flag-neutral load that leaves this clear keeps the
    /// predecessor's NZCV claim. Reset per instruction.
    bool load_clobbered_nzcv{false};
    /// Set when this instruction's ARCHITECTURAL RESULT is produced by a C++
    /// helper (jit_sse_op / jit_fpu_op / jit_round_op / jit_sse41_op /
    /// jit_pcmpxstrx / jit_fxsave_fxrstor) rather than by emitted ARM64.
    ///
    /// Distinct from `helper_blr_this_insn`, which is true for EVERY helper
    /// call including guest-memory access thunks, fault raises and flag
    /// materialisation. Those are part of a native lowering; these are the
    /// interpreter doing the work behind a native-looking `case` arm.
    /// Without this distinction the coverage scoreboard counts a helper call
    /// as a native lowering.
    bool semantic_helper_this_insn{false};
    /// Set around a memory RMW store that just wrote NZCV (ADDS/SUBS/ANDS).
    /// emit_guest_store* otherwise sees both `nzcv` and `incoming_nzcv` as None.
    bool hold_nzcv{false};
    /// Set around a linear64 access whose FAST path preserves NZCV: the
    /// emitter stamps ColdMemStub::spill_nzcv so the flag save/restore is paid
    /// only when the rarely-taken helper stub actually runs.
    bool stub_spill_nzcv{false};
    /// Nested emit_nzcv_spill/fill depth so FSTP (exp-check SUBS +
    /// emit_guest_store32) does not overwrite an outer CMP spill.
    u32 nzcv_hold_depth{0};
    /// Host NZCV from a linked predecessor may still be live. Dispatcher
    /// entry clears nzcv_kind, so the kind-byte Jcc path helpers there.
    /// Survives flag-neutral ops that do not clobber NZCV (reg MOV, LEA,
    /// linear 32-bit loads). A bounds-check SUBS or helper BLR kills it.
    bool pred_nzcv_live{false};
    /// This instruction kept predecessor NZCV (or spilled/filled it).
    bool preserved_pred_nzcv{false};

    /// The lazy-CF producer, tracked across the block. Unlike NZCV this
    /// survives flag-neutral MOV-family instructions, because the lazy
    /// operands remain in CpuState.
    CarrySource pending_carry_source{CarrySource::Unknown};
    CarrySource incoming_carry_source{CarrySource::Unknown};

    // No reset_for_block()/reset_at_join() here, deliberately.
    //
    // Writing them from the field list looked obvious and was wrong twice:
    // block entry sets `pred_nzcv_live = TRUE` (a linked predecessor may still
    // have NZCV live, which is the whole point of the carried state), not the
    // default false; and an in-block join DOES clear both carry sources, which
    // a reading of "the carry source is backed by CpuState, so a join cannot
    // invalidate it" says it should not.
    //
    // Both were caught by reading the two reset sites in jit_compiler.cpp
    // rather than by reasoning about the fields. The resets stay exactly where
    // they are until this type has accessors, so that this move can be proved
    // to emit identical code; folding them in is the next change, and it needs
    // its own evidence.
};

}  // namespace vitreo::cpu::jit
