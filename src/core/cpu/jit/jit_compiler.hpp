// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_compiler.hpp
/// @brief Copy-and-patch JIT compiler for x86 -> ARM64 (CPU-040)
///
/// The compiler class only. The block vocabulary is in jit_block.hpp and the
/// pinned-register context is in jit_context.hpp; both are included here so
/// every existing dependent keeps compiling, but a file that needs only one of
/// them should say so and stop depending on the emitter.
///
/// The class body is split by responsibility across jit_compiler_*.hpp,
/// textually included in order inside the class below. Member order, and so
/// object layout and initialization order, is the order of those includes:
/// never reorder them or move lines between them.

#include <core/cpu/jit/jit_block.hpp>
#include <core/cpu/jit/jit_context.hpp>
#include <core/cpu/jit/lazy_flag_state.hpp>
#include <core/cpu/jit/jit_code_arena.hpp>
#include <core/cpu/jit/jit_config.hpp>
#include <core/cpu/jit/jit_compile_memo.hpp>
#include <core/cpu/jit/ir/ir_frontend.hpp>
#include <core/cpu/jit/linear_fault.hpp>
#include <core/cpu/jit/flag_liveness.hpp>
#include <core/cpu/jit/translation_store.hpp>

#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

/// L3: the host trampoline an in-chain API-call exit BLRs to (defined in
/// jit_inchain_thunk.cpp, native backends only).
extern "C" void vitreo_jit_inchain_trampoline();

namespace vitreo::cpu::jit {
struct JitCrashBreadcrumbs;
/// JIT compiler that translates decoded x86 BasicBlocks into ARM64
/// Dump the process-wide compile-memo counters (VITREO_JIT_MEMO_STATS' report)
/// to `out`. Exposed so the SIGUSR1 hang dump can ask for it: the atexit report
/// never runs in a guest process that is killed, which is every GUI guest.
void memo_stats_dump_to(std::FILE* out) noexcept;
void smc_note_interpret_hit() noexcept;

/// Revalidate the calling thread's memory translation caches from inside a
/// scalar memory helper (a no-op unless @p ctx is that thread's running JIT).
void jit_revalidate_memory_caches(JitContext* ctx) noexcept;

/// Emitted bytes by category, summed over every successful compile while
/// enabled (set_emit_attribution). Diagnostic: the code-size reports read
/// it. A word belongs to the innermost category scope that emitted it,
/// except inside a region category (Prologue, Exit*, Raise, ColdMem,
/// ColdArms), which keeps everything emitted under it but another Exit*. Hot is the part
/// before the block's hot/cold split point, cold the part after.
struct EmitAttribution {
    enum Cat : u8 {
        Prologue,    ///< dispatcher entry: LR save, pin and base loads
        Body,        ///< instruction lowering not claimed below
        Address,     ///< effective-address formation
        MemFast,     ///< inline memory access: window/check-byte test, access
        Flags,       ///< lazy-flag records, NZCV kind/spill, condition eval
        GprState,    ///< CpuState register loads/stores, pin spills
        Rip,         ///< RIP publication
        Helper,      ///< helper calls
        Guard,       ///< write-watch / SMC marking
        ExitLink,    ///< patchable direct-link slots (B/BL to a successor)
        ExitProbe,   ///< inline L1 probes: linked-tail stubs, indirect/RET tails
        ExitDispatch,///< return to the dispatcher: RIP/count/NZCV/pin stores
        ExitOther,   ///< API-call exits, mid-block Jcc exit stubs
        Raise,       ///< raise checks and exits, alignment and UD2 stubs
        ColdMem,     ///< cold memory slow paths and linear store stubs
        ColdArms,    ///< deferred cold instruction arms
        Other,       ///< outside every scope (post-body, cold remainder)
        Count,
    };
    u64 hot[Count]{};
    u64 cold[Count]{};
    u64 blocks = 0;
    u64 split_blocks = 0;  ///< blocks whose cold part is non-empty
};
void set_emit_attribution(bool on) noexcept;
[[nodiscard]] EmitAttribution emit_attribution() noexcept;
void reset_emit_attribution() noexcept;

class JitCompiler {
public:
    explicit JitCompiler(GuestMemory& memory) noexcept;
    ~JitCompiler();

    // No copy
    JitCompiler(const JitCompiler&) = delete;
    JitCompiler& operator=(const JitCompiler&) = delete;

    /// Compile a basic block into native ARM64 code.
    /// Returns the compiled block, or nullptr on failure.
    ///
    /// One retry is internal: an attempt whose emitted code would read a lazy
    /// flag record that within-block liveness elided (see m_elided_span) is
    /// discarded before publication and the block is compiled again with no
    /// record omitted.
    CompiledBlock* compile(const BasicBlock& block);

    /// Publish a just-compiled record into its owning cache block and release
    /// transient compiler metadata when no pre-decoded delegate owns it.
    /// Generated code lives in the process arena; the cache receives every
    /// field needed for dispatch before the temporary record is removed.
    void publish_compiled_block(BasicBlock& block, CompiledBlock& compiled);

private:
    /// Direct-link bookkeeping shared by publish_compiled_block() and
    /// link_adopted_block(): remember this block's own patch slots and patch
    /// any of its recorded edges whose target this thread already has.
    void link_published_block(const CompiledBlock& compiled);

#define VITREO_JIT_COMPILER_CLASS_BODY 1
#include <core/cpu/jit/jit_compiler_runtime_api.hpp>
#include <core/cpu/jit/jit_compiler_compile_state.hpp>
#include <core/cpu/jit/jit_compiler_gpr_pins.hpp>
#include <core/cpu/jit/jit_compiler_cold_stubs.hpp>
#include <core/cpu/jit/jit_compiler_block_layout.hpp>
#include <core/cpu/jit/jit_compiler_vector_state.hpp>
#include <core/cpu/jit/jit_compiler_linking.hpp>
#include <core/cpu/jit/jit_compiler_emit_core.hpp>
#include <core/cpu/jit/jit_compiler_shared_stubs.hpp>
#include <core/cpu/jit/jit_compiler_emit_access.hpp>
#include <core/cpu/jit/jit_compiler_emit_tails.hpp>
#include <core/cpu/jit/jit_compiler_lowering.hpp>
#include <core/cpu/jit/jit_compiler_direct_links.hpp>
#include <core/cpu/jit/jit_compiler_translation_store.hpp>
#include <core/cpu/jit/jit_compiler_cold_residency.hpp>
#undef VITREO_JIT_COMPILER_CLASS_BODY
};

/// Flush the opt-in stack-proof report. The CLI
/// guest-exit path calls this explicitly because re-executed guest processes
/// can bypass normal host atexit handlers. Idempotent and a no-op when each
/// diagnostic is disabled.
void dump_stack_proof_stats() noexcept;

/// Emitted-word footprint of the cold block tail, summed over every compile
/// in the process. Diagnostic: the split-cold report and the cold-stub size
/// gates read it; nothing on an execution path does.
struct ColdTailFootprint {
    enum Piece : u32 {
        MemSites,        ///< count of cold memory stubs flushed (not words)
        MemLean,         ///< JIT-view lean variants
        MemRange,        ///< range-by-fault variants
        MemCrossing,     ///< crossing-by-fault variants
        MemSubpage,      ///< mixed-page sub-page decisions
        MemEntry,        ///< RIP/NZCV publish and operand moves
        MemCall,         ///< helper call or shared slow-path call
        MemTail,         ///< raise check, result move, unspill, return
        MemVector,       ///< 128/256-bit helper bodies
        MemCore,         ///< per-site try_region + helper bodies
        MemSharedBody,   ///< in-block shared helper bodies
        MemLazy,         ///< lazy slots, records and block lazy entries
        DeferredArms,
        LinearStores,
        Ud2,
        AlignChecks,
        RaiseExit,
        MidJccExits,
        Blocks,          ///< count of blocks that flushed a cold tail
        Count,
    };
    u64 words[Count]{};
    /// The `[jit] split-cold` totals: placed blocks and their two parts.
    u64 split_placed = 0;
    u64 split_hot_bytes = 0;
    u64 split_cold_bytes = 0;
};
[[nodiscard]] ColdTailFootprint cold_tail_footprint() noexcept;
void reset_cold_tail_footprint() noexcept;
/// Add @p words to @p piece. Emitters only.
void note_cold_tail_words(ColdTailFootprint::Piece piece, u64 words) noexcept;

/// Lazy cold memory stubs (JitCompiler::cold_lazy_enabled), process totals.
struct ColdLazyStats {
    u64 sites = 0;               ///< lazy slots emitted
    u64 materialized = 0;        ///< slots patched to a materialized stub
    u64 materialized_bytes = 0;  ///< island bytes those stubs took
    u64 races = 0;               ///< materializations that lost the slot CAS
    u64 side_exits = 0;          ///< takes that could not materialize
    u64 eager_withdrawn = 0;     ///< blocks withdrawn to recompile eagerly
    u64 mixed_eager = 0;         ///< first takes on a mixed page sent eager
};
[[nodiscard]] ColdLazyStats cold_lazy_stats() noexcept;
void reset_cold_lazy_stats() noexcept;
/// Host entry every JitContext::cold_lazy_entry holds.
[[nodiscard]] void* cold_lazy_trampoline_address() noexcept;
/// TEST-ONLY: every lazy take behaves as if its island were full.
void set_cold_lazy_force_side_exit_for_test(bool on) noexcept;
/// Tests: every materialization finds its island full (unlike the forced
/// side exit, this takes the eager-recompile fallback).
void set_cold_lazy_force_island_full_for_test(bool on) noexcept;
/// A block [start, start + length) holds an instruction whose lazy stub
/// could not be materialized, so it compiles with eager slow paths.
[[nodiscard]] bool cold_eager_requested_for(GuestAddr start, u32 length) noexcept;
/// Tests: whether a first take on a mixed host page recompiles its block
/// eagerly (default) or materializes the stub like any other take.
void set_cold_lazy_mixed_eager_for_test(bool on) noexcept;
/// Tests: forget every instruction marked for eager slow paths.
void reset_cold_eager_for_test() noexcept;

} // namespace vitreo::cpu::jit
