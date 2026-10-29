// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_unavailable.cpp
/// @brief x86_64 stand-in for the ARM64 JIT backend.
///
/// Built only when VITREO_ARCH_X86_64 is selected. It provides the JitCompiler
/// surface that the execution loop links against, and declines to compile.
///
/// Declining is a legitimate JitCompiler outcome, not a fake success: compile()
/// already returns nullptr for blocks the ARM64 backend cannot handle, and the
/// run loop responds by stepping the interpreter, which is the CPU semantic
/// reference. So an Intel build runs guests correctly, just without native
/// codegen -- until the native x86_64 backend replaces this file.
///
/// The genuinely ARM64-specific sources are excluded from this build:
///
///   jit_compiler.cpp   -- the execute() trampoline clobbers x0-x7
///   linear_fault.cpp   -- fault fixup reads the pinned CpuState from __x[19]
///   stencils, ops/*, instruction_compilation, jit_helpers*, stage_b1,
///   constructor_destructor, 64_bit_emit_primitives_used
///                      -- ARM64 instruction emitters
///
/// The arch-neutral bookkeeping (block_cache, jit_coverage, flag_liveness,
/// idiom_detector, hot_block_dump) is still built and still real.
///

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/state/flag_state.hpp"

#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Crash-handler telemetry published by the ARM64 backend.
//
// main.cpp reports these on a host fault. Zero is the honest value here: no
// JIT region exists, so the crash report shows none rather than stale data.
// ---------------------------------------------------------------------------
#include "jit_crash_breadcrumbs.hpp"
constinit thread_local volatile vitreo::cpu::jit::JitCrashBreadcrumbs
    vitreo::cpu::jit::jit_crash_breadcrumbs{};

namespace vitreo::cpu::jit {

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------
void*                     JitCodeArena::s_arena_reservation       = nullptr;
usize                     JitCodeArena::s_arena_total             = 0;
void*                     JitCodeArena::s_arena_region            = nullptr;
usize                     JitCodeArena::s_arena_budget            = 0;
usize                     JitCodeArena::s_arena_chunk_size        = 0;
usize                     JitCodeArena::s_arena_island_size       = 0;
u32                       JitCodeArena::s_arena_chunk_shift       = 0;
usize                     JitCodeArena::s_arena_chunk_count       = 0;
usize                     JitCodeArena::s_arena_soft_budget       = 0;
std::unique_ptr<JitCodeArena::ArenaChunk[]> JitCodeArena::s_arena_chunks;
std::mutex                JitCodeArena::s_arena_chunk_mutex;
std::atomic<usize>        JitCodeArena::s_arena_available         {0};
std::atomic<usize>        JitCodeArena::s_arena_pending_count     {0};
std::atomic<usize>        JitCodeArena::s_arena_used_total        {0};
std::atomic<u64>          JitCodeArena::s_arena_epoch             {1};
std::atomic<u64>          JitCodeArena::s_arena_retire_seq        {1};
std::atomic<u64>          JitCodeArena::s_arena_chunk_frees       {0};
std::atomic<u64>          JitCodeArena::s_arena_retire_generation {0};
std::atomic<u64>          JitCodeArena::s_arena_reclaims          {0};
std::atomic<u64>          JitCodeArena::s_arena_generation        {0};
std::atomic<u64>          JitCodeArena::s_arena_min_generation    {1};
std::atomic<bool>         JitCodeArena::s_arena_reclaim_requested {false};
std::atomic<bool>         JitCodeArena::s_arena_growth_failed     {false};
std::atomic<u64>          JitCodeArena::s_arena_gate_closed_ns    {0};
std::atomic<u64>          JitCodeArena::s_arena_gate_refusals     {0};
std::atomic<u64>          JitCodeArena::s_arena_reclaim_retry_ns  {0};
std::atomic<u32>          JitCodeArena::s_arena_drain_failures    {0};
std::atomic<u64>          JitCodeArena::s_arena_gate_abandons     {0};
std::mutex                JitCodeArena::s_arena_reclaim_mutex;
std::atomic<bool>         JitCompileMemo::s_compiled_memo_enabled   {false};
std::atomic<bool>         JitCompiler::s_jit_link_enabled        {false};
std::array<JitCompileMemo::Shard, JitCompileMemo::kMemoShards>
                          JitCompileMemo::s_shards;
std::atomic<u64>          JitCompileMemo::s_max_span_pages{0};

// Shared compiled-block table: nothing is ever published on this build, so
// every query answers "no record" and every mutation is a no-op.
void JitCompileMemo::clear_compile_memo() noexcept {}
usize JitCompileMemo::compile_memo_size() noexcept { return 0; }
void JitCompileMemo::forget_guest_memory(GuestMemory*) noexcept {}
JitCompileMemo::CompileMemoStats JitCompileMemo::compile_memo_stats() noexcept {
    return {};
}
void JitCompileMemo::reset_compile_memo_stats() noexcept {}
void JitCompileMemo::dump_records(const char*) noexcept {}
bool JitCompiler::try_adopt_shared_block(GuestAddr, bool, BasicBlock&) {
    return false;
}
bool JitCompiler::shared_adoption_enabled() noexcept { return false; }
void JitCompiler::link_adopted_block(BasicBlock&) {}
usize JitCompileMemo::purge_shared_records_overlapping(GuestAddr,
                                                     usize, u64) noexcept {
    return 0;
}
bool JitCompileMemo::shared_smc_demoted(GuestAddr) noexcept { return false; }
bool JitCompileMemo::accept_smc_compilation(GuestAddr, u64, bool& tracked) noexcept {
    tracked = false;
    return true;
}
u64 shared_table_smc_purges() noexcept { return 0; }
void reset_shared_table_smc_purges() noexcept {}

/// No JIT means no JIT access-violation dump is ever armed, so there is
/// nothing pending to flush.
void jit_flush_pending_av_dump() {}

/// Nothing compiles here, so the compile memo is permanently empty and there
/// are no statistics to write.
void memo_stats_dump_to(std::FILE*) noexcept {}

// ---------------------------------------------------------------------------
// JitCodeArena::ArenaReadLease
//
// The lease guards a MAP_JIT arena against reclaim while a thread executes
// from it. With no arena and no compiled code, there is nothing to guard, so
// acquiring always succeeds and does nothing.
// ---------------------------------------------------------------------------
JitCodeArena::ArenaReadLease::ArenaReadLease() noexcept { m_acquired = true; }
JitCodeArena::ArenaReadLease::ArenaReadLease(StableExecution) noexcept {
    m_acquired = true;
}
JitCodeArena::ArenaReadLease::ArenaReadLease(DeferAcquire) noexcept {}
JitCodeArena::ArenaReadLease::~ArenaReadLease() = default;

bool JitCodeArena::ArenaReadLease::acquire() noexcept {
    m_acquired = true;
    return true;
}
void JitCodeArena::ArenaReadLease::release() noexcept { m_acquired = false; }
bool JitCodeArena::ArenaReadLease::thread_holds() noexcept { return false; }
void JitCodeArena::ArenaReadLease::refresh_epoch() noexcept {}

// ---------------------------------------------------------------------------
// JitCompiler
// ---------------------------------------------------------------------------
JitCompiler::JitCompiler(GuestMemory& memory) noexcept : m_memory(memory) {}
JitCompiler::~JitCompiler() = default;

/// Always decline. The caller falls back to the interpreter.
CompiledBlock* JitCompiler::compile(const BasicBlock&) { return nullptr; }

/// Unreachable: compile() never produces a block, so nothing can be executed.
/// Reaching here means a caller fabricated a CompiledBlock or the run loop
/// took a JIT path without a successful compile -- a Vitreo bug that must not
/// be papered over by returning a plausible-looking result.
JitExecResult JitCompiler::execute(CompiledBlock&, CpuState&) {
    std::fprintf(stderr,
        "[jit] FATAL: JitCompiler::execute() reached on the x86_64 build.\n"
        "      No native code backend exists here and compile() always\n"
        "      declines, so no CompiledBlock can be valid. This is a\n"
        "      dispatcher bug, not a missing feature.\n");
    std::abort();
}

/// Unreachable for the same reason as execute(): publishing requires a
/// CompiledBlock, and compile() never produces one. Every caller guards on a
/// non-null compile result, so reaching here means a fabricated block.
void JitCompiler::publish_compiled_block(BasicBlock&, CompiledBlock&) {
    std::fprintf(stderr,
        "[jit] FATAL: JitCompiler::publish_compiled_block() reached on the\n"
        "      x86_64 build. compile() always declines, so no CompiledBlock\n"
        "      can exist to publish. This is a dispatcher bug.\n");
    std::abort();
}

// ---------------------------------------------------------------------------
// Retained-IR budget
//
// The budget meters pre-decoded instruction arrays shared through the
// process-wide compile memo. Nothing compiles here, so nothing is ever
// admitted and the accounted total is permanently zero -- resetting it is a
// genuine no-op rather than a skipped step.
// ---------------------------------------------------------------------------
void memo_ir_reset() noexcept {}

// L3 in-chain API thunk calls exist only with a native backend: nothing is
// compiled here, so no API-call tail exists to carry the stub.
bool JitCompiler::inchain_thunks_codegen() noexcept { return false; }
void JitCompiler::set_inchain_thunks_codegen_for_test(int) noexcept {}
void JitCompiler::set_inchain_thunks_runtime_gate(bool) noexcept {}
void JitCompiler::set_inchain_counting_for_test(bool) noexcept {}
u64 JitCompiler::inchain_resumed_this_thread() noexcept { return 0; }
u64 JitCompiler::inchain_left_this_thread() noexcept { return 0; }
u64 JitCompiler::inchain_broke_outer_this_thread() noexcept { return 0; }
JitCompiler::InChainNext JitCompiler::inchain_from_stub(
    CpuState*, JitContext* ctx) noexcept {
    return InChainNext{nullptr, ctx != nullptr ? ctx->native_sp_base : 0};
}

void JitCompiler::free_block(CompiledBlock&) {}
void JitCompiler::set_block_cache(BlockCache* cache) { m_block_cache = cache; }

bool JitCompiler::synchronize_executable_writes() { return false; }
bool JitCompiler::synchronize_arena_generation()  { return false; }
bool JitCodeArena::reclaim_arena_if_requested()    { return false; }
void JitCodeArena::abandon_gate_if_stalled() noexcept {}
// No arena exists here: nothing can be released, retired or swept.
void JitCodeArena::release_chunk(Cursor&, bool) noexcept {}
void* JitCodeArena::alloc_far_branch(const void*, u64, usize) { return nullptr; }
usize JitCodeArena::retire_arena_chunks(usize, bool) { return 0; }
usize JitCodeArena::sweep_retired_chunks(bool) { return 0; }

void JitCompiler::maybe_pump_gui() {}
void JitCompiler::maybe_pump_gui_after_execute() {}

// ---------------------------------------------------------------------------
// Linear fault handler
//
// On ARM64 this traps guest accesses that fall outside the linear reservation
// and fixes them up from the pinned CpuState register. That fixup is part of
// JIT-generated code; with no JIT there is nothing to install, and a genuine
// host fault must reach the crash handler in main.cpp rather than be caught
// here and silently resumed.
// ---------------------------------------------------------------------------
bool install_linear_fault_handler() { return false; }
bool register_linear_reservation(u8*, usize) { return false; }
void unregister_linear_reservation(u8*) {}
bool linear_reservation_registered(u8*, usize) { return false; }
bool publish_jit_rip_block(uintptr_t, u32, const JitRipSite*, u32,
                           const JitPinMap&, u32, u32) {
    return false;
}
bool jit_fault_metadata_for_pc(uintptr_t, JitFaultMetadata&) noexcept {
    return false;
}
bool jit_guest_rip_for_pc(uintptr_t, GuestAddr&) noexcept { return false; }
void retire_jit_rip_range(uintptr_t, uintptr_t) noexcept {}
// Diagnostic dump of the ARM64 stack-proof counters. There are none here.
void dump_stack_proof_stats() noexcept {}

LinearReservationRegistration::~LinearReservationRegistration() = default;
bool LinearReservationRegistration::acquire(u8*, usize) { return false; }
void LinearReservationRegistration::reset() noexcept {}

} // namespace vitreo::cpu::jit
