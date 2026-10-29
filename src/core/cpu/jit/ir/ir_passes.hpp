// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_passes.hpp
/// @brief Optimization passes over the canonical IR, and the switches that
///        turn each of them off.
///
/// Every pass in this file obeys four rules the milestone plan makes
/// non-negotiable (`03_MILESTONES.md` M8): it has a pure IR unit suite, an
/// individual kill switch, a stats counter, and it must not change observable
/// semantics when disabled. The kill switch is not a debugging nicety — it is
/// how a correctness regression gets bisected to one pass in an afternoon
/// instead of a week.
///
/// ## Passes run BEFORE the verifier, deliberately
///
/// `build_block` verifies structural and encoded fault-state obligations
/// after the pass pipeline. Rejection safely declines the block to Tier 0.
/// This verifier does not prove arbitrary pass semantics: differential tests
/// are still required, including for register-alias transformations.
///
/// Fault-aware flag demand and recovery are verified independently of the
/// emitter. The stencil compiler also guards may-fault memory operations;
/// these passes encode the same architectural obligation as explicit IR.

#pragma once

#include "jit_ir.hpp"

#include <vitreo/types.hpp>

namespace vitreo::cpu::jit::ir {

/// One bit per pass, for `VITREO_JIT_IR_PASSES`. Default is every pass on;
/// clear a bit to bisect.
enum class Pass : u32 {
    None        = 0,
    /// Flag-demand elimination: a flag definition whose value no consumer can
    /// observe before it is overwritten does not need to be computed.
    FlagDemand  = 1u << 0,
    /// NZCV residency: mark a flag definition whose consumer is a branch in
    /// the same block as ALSO living in host PSTATE, so the branch can read it
    /// with one instruction instead of reconstructing it from the record.
    NzcvResidency = 1u << 1,
    /// Flag-recovery elision: kill a flag definition that is dead except that
    /// a may-fault op stands between it and its killer, by publishing a
    /// recipe the fault handler uses to rebuild the record from the
    /// interrupted register file. Clearing this bit restores the conservative
    /// behaviour exactly — the definition is published as before.
    FlagRecovery = 1u << 2,
    /// Reuse unchanged width-matched guest register reads. PE32 values use
    /// architectural pins; pure long-mode blocks use allocated snapshots.
    RegisterReadReuse = 1u << 3,
    /// Share width-matched I32/I64 ALU constants within one guest instruction
    /// of a scalar or snapshot-allocatable block.
    ScalarConstants = 1u << 4,
    /// Dead CF/OF overlay elimination: drop a long-mode `SetCf`/`SetOf` that a
    /// later full flag definition overwrites before anything can observe it,
    /// then the pure ops that only fed it.
    OverlayDemand = 1u << 5,
    /// Long mode: a flag definition read only by carry conditions of later
    /// instructions in the block, with nothing between that can fault or
    /// disturb host NZCV, lives in PSTATE alone instead of publishing its
    /// lazy record (ADD ; ADC ; SBB keeps CF in ARM C).
    PstateCarry = 1u << 6,
    /// Long mode: a flag definition whose one reader is the mid-block
    /// conditional exit right after it, and which is otherwise dead on the
    /// fall-through (killed by a later definition, every fault before that
    /// covered by a recovery plan), publishes its record on that exit's
    /// taken edge instead of inline (`FlagDef::exit_deferred`).
    ExitRecord = 1u << 7,
};

/// True when `pass` is enabled. Reads `VITREO_JIT_IR_PASSES` once into a
/// process-wide cache; an unrecognised value leaves every pass ON, because
/// failing open here loses optimization, never correctness — the opposite of
/// the Tier 1 gate itself, which fails closed.
[[nodiscard]] bool pass_enabled(Pass pass) noexcept;

struct PassStats;
u32 reuse_register_reads(IrBlock& block, PassStats& stats);
u32 defer_records_to_exits(IrBlock& block, PassStats& stats,
                           bool recovery_available);
/// No memory, narrow/vector operations or CF-merge helpers. A terminal branch
/// may destroy scratch state only after all integer values have died.
bool scalar_block_allocatable(const IrBlock& block) noexcept;
bool snapshot_block_allocatable(const IrBlock& block) noexcept;
bool requires_snapshot_backing(const IrBlock& block) noexcept;
/// Shared bounded spill-slot plan. False means pressure exceeds backing capacity.
bool plan_spill_slots(const IrBlock& block, std::vector<u8>* assignment);
u32 common_scalar_constants(IrBlock& block, PassStats& stats);


/// Statistics from one pipeline run over one block.
struct PassStats {
    u32 register_reads_reused{0};
    /// `SetCf`/`SetOf` overlays removed by `eliminate_dead_overlays`, and the
    /// pure value ops that became unused with them.
    u32 overlays_killed{0};
    u32 values_dce{0};
    u32 scalar_constants_reused{0};
    /// Flag definitions proven dead and downgraded to `FlagRepr::Dead`.
    u32 flags_killed{0};
    /// Flag definitions that were dead but had to keep publishing anyway
    /// because something other than the record depends on the store.
    u32 flags_kept{0};
    /// Of those, the ones kept ONLY because a may-fault op sits between the
    /// definition and its killer. This is the population that M5's recovery
    /// metadata would unlock, so it is counted before any of that is built:
    /// if it is small on real guests, the recovery work is not worth its
    /// complexity and the milestone should spend itself elsewhere.
    u32 flags_kept_by_fault{0};
    /// Kept because something genuinely reads the value. No recovery metadata
    /// can help these.
    u32 flags_kept_by_use{0};
    /// Kept because no later definition overwrites them — live out of the
    /// block. Cross-block flag liveness, not recovery, is what reaches these.
    u32 flags_live_out{0};
    /// Flag definitions additionally marked as resident in host PSTATE.
    u32 nzcv_resident{0};
    /// Of those, the ones demoted to PSTATE alone (`keep_carry_in_pstate`).
    u32 flags_pstate_only{0};
    /// Definitions whose record moved onto a mid-block exit's taken edge.
    u32 flags_exit_deferred{0};
    /// Flag definitions killed because a recovery plan covers the may-fault
    /// ops that would otherwise have kept them alive. A subset of
    /// `flags_killed`.
    u32 flags_killed_by_recovery{0};
    /// Definitions where recovery was attempted and refused. Counted so the
    /// producer's reach is visible without reading the code.
    u32 recovery_declined{0};
    /// Refusals by reason, indexed by `RecoveryDecline`.
    u32 recovery_decline_reason[9]{};
    /// Integer values used under a DIFFERENT guest instruction than the one
    /// that defined them. The emitter's scratch pool resets at every guest
    /// instruction, so this is the population cross-instruction residency
    /// (M6) would serve — and if it is zero, residency has nothing to hold.
    u32 cross_instruction_values{0};
    /// `Lea` nodes structurally identical to an earlier `Lea` in the same
    /// block. These are what an EA-commoning pass would fold, and folding them
    /// is what would CREATE cross-instruction values in the first place.
    u32 total_leas{0};
    u32 redundant_leas{0};
    /// Of those, the ones whose duplicate is under a different guest
    /// instruction — the subset that needs residency rather than just CSE.
    u32 redundant_leas_cross_insn{0};
};

/// Measure, without changing anything, how much cross-instruction value
/// traffic this block would have. It answers whether any values are live
/// across a guest instruction boundary today, and whether an EA-commoning
/// pass would create some.
/// Telemetry only: counts cross-instruction values and equivalent `Lea`
/// pairs. It transforms nothing, and the Lea search is quadratic, so callers
/// run it only when `stats::enabled()`. `run_passes` deliberately does not.
void measure_cross_instruction_opportunity(const IrBlock& block,
                                           PassStats& stats) noexcept;

/// Run the enabled passes over `block`, in the fixed order below. Returns what
/// each pass did, for `VITREO_JIT_IR_STATS`.
///
/// The pipeline is deliberately a plain function with an explicit order rather
/// than a registry: with one pass, a registry would be architecture for its own
/// sake. Further passes, when they exist, run in this order: canonicalize,
/// copy-prop, fold, DCE, flag demand.
PassStats run_passes(IrBlock& block, bool recovery_available);

/// Flag-demand elimination, exposed for its unit tests.
///
/// A flag definition is dead when a later `DefFlags` overwrites it and nothing
/// in between can observe it. "Observe" includes faulting: the signal handler
/// reconstructs architectural state from the lazy record, so a fault between
/// definition and killer would read whatever the elided store left behind.
/// That condition is the one `compute_omit_lazy_flags` misses, and here it is
/// a single guard in a forward scan.
///
/// Returns the number of definitions killed; updates `stats`.
u32 eliminate_dead_flags(IrBlock& block, PassStats& stats,
                         bool recovery_available);

/// True when materializing `record` sets BOTH CF and OF from the record alone,
/// whatever RFLAGS held before. Only such a record makes an earlier CF/OF
/// overlay unobservable: a multi-bit 64-bit shift, a rotate or a narrow NEG
/// leaves CF or OF as the stored RFLAGS bit, which is exactly what `SetCf`
/// writes. Pinned against `FlagState::materialize` by a unit test.
[[nodiscard]] bool record_owns_cf_and_of(FlagState::FlagOp record) noexcept;

/// Dead overlay elimination, exposed for its unit tests. Long mode only.
///
/// A `SetCf`/`SetOf` writes the RFLAGS bit and the overlay byte. Both are dead
/// when a later live `DefFlags` whose record owns CF and OF publishes before
/// any flag reader, fault, branch or POPF: publication clears the overlays,
/// and materialization then takes CF and OF from the record. Removed overlays
/// are followed by dead-code elimination of the pure ops that fed only them.
///
/// Returns the number of overlays removed; updates `stats`.
u32 eliminate_dead_overlays(IrBlock& block, PassStats& stats);

/// Mark flag definitions whose result can also stay in host PSTATE for a
/// branch in the same block, exposed for its unit tests.
///
/// The representation lattice already allows two homes at once: the record is
/// the architectural home, and an `NzcvKind` alongside it is an additional
/// fast-path fact. This pass is what establishes that fact — it does not
/// remove the record, because a fault, a successor block or a later reader
/// still needs it.
///
/// Returns the number of definitions marked.
u32 assign_nzcv_residency(IrBlock& block, PassStats& stats);

/// Long mode only. Demote a record-backed definition with an `Add` or `Sub`
/// NZCV claim to `InPstate` when every reader before its killer is an
/// `EvalCc` of CF (B or NB), and nothing between the definition and its
/// killer can fault, read the flags any other way, or lower to code that
/// writes host NZCV. The record is then never observable: no fault, exit,
/// helper or later block can see it before the killer redefines the flags.
/// The definition keeps one operand, its producer's result, and the pure ops
/// that only fed the record are swept -- as are those feeding a dead ADC/SBB
/// record, which is retargeted at its carry op's result, the second source
/// of any other dead record (it names one source twice), and pure values
/// nothing reads. Runs after
/// `assign_nzcv_residency`.
///
/// Returns the number of definitions demoted; updates `stats`.
u32 keep_carry_in_pstate(IrBlock& block, PassStats& stats);

/// Why a recovery plan could not be built. Recorded per attempt, because
/// "the producer declined" is not actionable and the reason is: extending it
/// to 8/16-bit records is a different job from extending it to CF-preserving
/// merges, and only a census says which is worth doing.
enum class RecoveryDecline : u8 {
    None = 0,
    /// The record is CF-preserving (INC/DEC) — it needs the OLD carry, which
    /// no register holds by fault time.
    CarryMerge,
    /// A CF/OF overlay is live over the definition; the handler's
    /// reconstruction clears overlays, which is only right for a full writer.
    OverlayLive,
    /// The record is not one of the five the handler can rebuild — most often
    /// an 8- or 16-bit width.
    UnsupportedRecord,
    /// A may-fault op sits inside the definition's or the killer's OWN guest
    /// instruction, where the record's publication point relative to the
    /// access is a property of that lowering, not of this analysis.
    FaultInsideEndpoint,
    /// An operand is neither a guest register nor an immediate — a load
    /// result, a scratch temporary, an extension.
    OperandNotRecoverable,
    /// A named guest register is overwritten before the last covered
    /// instruction, so its pin no longer holds the value.
    OperandClobbered,
    /// A covered may-fault op is a STORE, which can raise in software from
    /// its cold-stub helper instead of as a hardware fault — and only the
    /// hardware fault handler rebuilds an elided record.
    StoreRaisesInHelper,
    /// Structural: not a 32-bit block, no covered instruction, etc.
    Structural,
    _Count,
};

/// Name for the report.
[[nodiscard]] const char* recovery_decline_name(RecoveryDecline r) noexcept;

/// Try to build a recovery plan for the flag definition at `def_index`, whose
/// killer is at `killer_index`, so it can be killed despite may-fault ops
/// standing between them. Exposed for its unit tests.
///
/// Returns false — and leaves `plan` untouched — whenever anything about the
/// shape is not provable. Every early return is a case where the definition
/// stays published, which is always semantically safe.
[[nodiscard]] bool try_plan_flag_recovery(const IrBlock& block, u32 def_index,
                                          u32 killer_index,
                                          FlagRecoveryPlan& plan,
                                          RecoveryDecline* why = nullptr) noexcept;

}  // namespace vitreo::cpu::jit::ir
