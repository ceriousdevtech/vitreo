// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend.hpp
/// @brief Decoded x86 -> canonical IR: the Tier 1 semantic builder.
///
/// `jit_ir.hpp` is a data model with a verifier and, until this file, no
/// producer: nothing in `src/` built an `IrBlock`. This is that producer, and
/// it is the whole of the Tier 1 frontend — it decides *x86 meaning* only.
/// It names no ARM64 register and chooses no ARM64 instruction, which is what
/// keeps `01_TARGET_ARCHITECTURE.md`'s frontend/middle-end/backend split real
/// rather than aspirational.
///
/// **All or nothing, per block.** ADR-0001 fixes the Tier 1 boundary at Seam A
/// (`JitCompiler::compile`), so a block is either wholly expressible in IR or
/// wholly declined to the Tier 0 stencil compiler. There are no per-node side
/// exits into stencil emitters: mixing IR-allocated temporaries with emitters
/// that assume fixed scratch registers is where register-corruption bugs live.
/// `build_block` therefore returns a single verdict for the block, and the
/// caller that gets `false` must emit nothing.
///
/// **Decline is the default.** Every opcode, operand shape, prefix and width
/// this file has not been taught is refused by name, with a reason. That is
/// not caution for its own sake: an unsupported form that is silently *almost*
/// handled is a wrong-result CPU bug, the most expensive class this subsystem
/// has. The reasons are enumerated (not free text) because they are the
/// coverage metric — `VITREO_JIT_IR_STATS` histograms them, and "which shape
/// blocks the most blocks" is what says what to implement next.
///
/// ## Supported forms
///
/// The 32-bit frontend includes scalar register and memory operations,
/// partial-register writes, carry-preserving flag definitions, constant-count
/// shifts, control flow, and selected SIMD forms. Coverage is per encoding and
/// operand shape: classify()/build_insn() are the authoritative admission gate.
///
/// The long-mode frontend admits I32/I64 MOV register/immediate forms,
/// ADD/SUB/AND/OR/XOR/CMP/TEST register/immediate forms, MOV loads/stores with
/// full-width base/displacement or RIP-relative addressing, and direct JMP/Jcc
/// terminators. Narrow writes, address-size/segment overrides, indexed memory,
/// indirect control flow, and other forms still decline to the baseline JIT.
/// Backend feasibility independently checks register pressure and fault state.
///
/// Declining an IR form does not mean the interpreter or baseline JIT cannot
/// execute it. Current support and validation are tracked in
/// .docs/perf/dbt-completion-milestones.md; old stage labels are historical.

#pragma once

#include "ir_passes.hpp"
#include "jit_ir.hpp"

#include <vitreo/types.hpp>

#include <cstdio>
#include <string>

namespace vitreo::cpu {
struct DecodedInstruction;
}

namespace vitreo::cpu::jit::ir {

/// Why a block was not expressible in IR.
///
/// Ordered coarse-to-fine, and stable: `VITREO_JIT_IR_STATS` prints this
/// histogram, and a reordering would silently retitle collected data.
enum class DeclineReason : u8 {
    None = 0,
    /// The block carries no decoded instructions (the caller released them,
    /// or it is empty). Nothing to build from.
    NoInstructions,
    /// Legacy histogram slot for the former blanket long-mode gate. Kept in
    /// place so previously recorded reason indices do not change.
    Bitness64,
    /// More instructions than the block builder will accept in one go.
    BlockTooLong,
    /// A prefix that changes the operation's meaning: LOCK, REP/REPNE, an
    /// operand-size or address-size override, a segment override, or VEX.
    UnsupportedPrefix,
    /// The opcode has no IR semantics in this increment.
    UnsupportedOpcode,
    /// The opcode is known but this operand shape is not — most often a
    /// memory operand where only registers are handled yet.
    UnsupportedOperand,
    /// A width the selected form does not model, such as a narrow long-mode
    /// write or a 64-bit operand in a 32-bit block.
    UnsupportedWidth,
    /// The frontend or an optimization pass violated a verified IR invariant.
    /// Count separately from unsupported-form coverage gaps.
    VerifierRejected,
    /// The IR verified, but the ARM64 backend cannot lower some op in it.
    ///
    /// A separate reason because it is a separate question. The frontend and
    /// the backend advance independently, so "the IR can say it" and "we can
    /// emit it" diverge — and a block in that gap is NOT accepted, however
    /// good the IR looks. Folding it into the accept count would report
    /// coverage the runtime does not have.
    BackendUnsupported,
    /// The backend ran out of temporaries, or a value could not survive a
    /// memory access that clobbers the scratch pool.
    ///
    /// Split from `BackendUnsupported` because the two have different owners:
    /// an unlowered op is a missing case statement, whereas this is register
    /// allocation (M6). Lumping them together hides which one to work on —
    /// the same mistake the reason-only histogram made before it learned to
    /// record opcodes.
    BackendRegisterPressure,
    /// The block was emitted but one of its deferred branches could not reach
    /// its target (JitCompiler::cold_branch_disp), so finalize_code declined
    /// it to the interpreter.
    BranchReach,
};

[[nodiscard]] const char* decline_reason_name(DeclineReason reason) noexcept;

/// Outcome of one block build attempt.
struct BuildResult {
    /// True only when a fully verified block is in `out`.
    bool ok{false};
    /// `None` iff `ok`.
    DeclineReason reason{DeclineReason::None};
    /// Index of the guest instruction that caused the decline, or
    /// `0xFFFF'FFFF` for a whole-block reason.
    u32 insn_index{0xFFFF'FFFFu};

    /// `Opcode` of the instruction that blocked the build, or 0.
    ///
    /// The reason histogram says which CATEGORY blocks most often, which is a
    /// coarse answer: "unsupported-operand" covers a memory source, a memory
    /// destination, an 8-bit form and a stack form alike. Guessing which one
    /// dominates has already been wrong once — a whole increment of
    /// ALU-with-memory-source support moved the accept rate by zero, because
    /// the blockers were elsewhere. So record the actual opcode and let the
    /// data pick the next milestone.
    u32 blocking_opcode{0};
};

inline constexpr u32 kNoInsnIndex = 0xFFFF'FFFFu;

/// Longest block this frontend will attempt. Blocks longer than this are
/// vanishingly rare and the cap keeps one pathological decode from making a
/// compile pause visible.
inline constexpr u32 kMaxBlockInsns = 256;

/// Build `count` decoded instructions starting at `entry_rip` into `out`.
///
/// `out` is cleared first and is left in an unspecified (but valid) state on
/// decline — the caller must not emit from it. `verifier_why`, when non-null,
/// receives the verifier's one-line reason on `VerifierRejected`.
///
/// `rips` supplies the guest address of each instruction. The frontend does
/// not compute them from lengths: a block's instructions are not required to
/// be contiguous in the general case, and every op's `guest_rip` is the
/// host-PC -> guest-RIP fault attribution, so guessing it is not an option.
/// `recovery_available` says whether a fault in this block would be recovered
/// by the LINEAR handler, which is the only thing that can rebuild an elided
/// flag record. On the region-cache path a bad access goes through a helper
/// that raises in software, and no metadata is consulted — so the flag-
/// recovery pass must not elide there. The frontend takes it as a parameter
/// rather than reading the environment because the memory model belongs to
/// the compiler's `GuestMemory`, not to the process.
/// `recovery_gprs` is `IrBlock::recovery_gprs`: the guest registers a plan
/// may name.
[[nodiscard]] BuildResult build_block(const DecodedInstruction* insns,
                                      const u64* rips, u32 count,
                                      u64 entry_rip, bool is_64bit,
                                      bool recovery_available,
                                      IrBlock& out, std::string* verifier_why,
                                      u16 recovery_gprs = 0x00FF);

// =============================================================================
// Coverage telemetry
// =============================================================================

namespace stats {

/// True when `VITREO_JIT_IR_STATS` is set. Read once into a process-wide
/// cache; one relaxed load on the default-off path.
[[nodiscard]] bool enabled() noexcept;

/// True when `VITREO_JIT_IR_STATS=2`: every declined block is also logged
/// with its entry RIP, the blamed instruction and the check that declined.
[[nodiscard]] bool per_block_log() noexcept;

/// Record one block build attempt. Cheap and lock-free; no-op when disabled.
void record(const BuildResult& result) noexcept;

/// Record the verifier's reason for one `VerifierRejected` block, with its
/// numbers masked so reports group by rule. A rejection is a frontend or pass
/// bug, not a coverage gap, and the reason-only histogram cannot say which
/// rule broke. No-op when disabled.
void note_verifier_reject(const char* why) noexcept;

/// Record what the flag-demand pass did to one block. Separate from `record`
/// because it happens earlier — passes run before the verifier, so a block
/// counted here may still go on to decline.
void note_pass_stats(const PassStats& stats) noexcept;

/// Why a live-out flag definition could or could not be proven dead by
/// looking one instruction into the block's successors.
enum class LiveOutVerdict : u8 {
    /// Every reachable successor opens by redefining all flags without
    /// reading them and without touching memory. The record is never
    /// observed, so publishing it is pure overhead.
    Killable = 0,
    /// A successor reaches a barrier — an instruction that reads the flags,
    /// preserves part of them, or is unrecognised — before redefining them.
    /// No lookahead depth and no recovery metadata reaches these.
    SuccessorReads,
    /// Provable except that a memory access stands between the block boundary
    /// and the redefinition. This is exactly the population M5's
    /// `JitFlagRecovery` addresses, so it is counted apart from `Killable`.
    FaultBlocked,
    /// The scan budget ran out with the flags still unsettled.
    ScanExhausted,
    /// The terminator is indirect (RET, JMP r/m, CALL r/m), so there is no
    /// compile-time successor to inspect at all.
    IndirectSuccessor,
    /// A successor's bytes could not be decoded — unmapped, or not yet
    /// written by the guest.
    Undecodable,
    /// Keep in sync with the report's array; the enum's order IS the index.
    _Count,
};

/// Record one live-out flag definition's verdict. Measurement only.
void note_liveout(LiveOutVerdict verdict) noexcept;

/// Record which opcode stopped a successor scan as a barrier. Takes the raw
/// enumerator so this header keeps its existing include set — the same reason
/// `BuildResult::blocking_opcode` is a raw value.
void note_liveout_barrier(u16 opcode) noexcept;

/// Census one block's instruction mix before any decline, so the denominator
/// is every block Tier 1 was asked about rather than every block it accepted.
/// Record which prefix caused a decline (see the report's table).
void note_prefix_decline(u32 slot) noexcept;

void note_block_census(const DecodedInstruction* insns, u32 count) noexcept;

/// Print accept rate and the decline histogram to `out`.
///
/// Split from `dump()` so the report can be reached WITHOUT the process
/// exiting. `dump()` is armed on `std::atexit`, which is fine for a console
/// guest and useless for a GUI one: an app parked in its message loop never
/// exits, SIGTERM does not run atexit handlers, and the guest re-execs as its
/// own bundle so a launcher timeout never reaches it. Coverage for exactly the
/// workloads worth measuring was therefore unobtainable.
///
/// The caller that fixes this registers `dump_to` as a SIGUSR1 hang-dump
/// section (`diagnostics::register_hang_dump_section`), which runs on the
/// servicing thread rather than in the signal handler — so ordinary buffered
/// I/O here is safe.
void dump_to(std::FILE* out) noexcept;

/// Print to stderr. Armed at exit on first `record()`; also safe to call by
/// hand.
void dump() noexcept;

/// Zero the counters. Tests only.
void reset() noexcept;

}  // namespace stats

}  // namespace vitreo::cpu::jit::ir
