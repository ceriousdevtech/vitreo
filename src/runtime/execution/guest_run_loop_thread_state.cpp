// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: per-thread guest run state (JIT block cache and
// compiler, dispatch-block preparation, module manager, active CPU).
// Split out of guest_run_loop.cpp as verbatim code motion.

#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/guest_run_loop_internal.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/translation_store_stats.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/mem_watch.hpp"
#include <vitreo/diagnostics.hpp>
#include "core/base/perf_window.hpp"
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/execution/block_decoder.hpp"
#include <cstdlib>
#include <memory>

namespace vitreo::cli {

// ============================================================================
// Per-thread JIT state — shared between the main loop and callbacks
// ============================================================================

constinit thread_local cpu::jit::BlockCache*  t_block_cache   = nullptr;
constinit thread_local cpu::jit::JitCompiler* t_jit_compiler  = nullptr;
constinit thread_local bool                   t_jit_enabled   = false;
constinit thread_local winapi::ModuleManager* t_module_manager = nullptr;

// Block caches are per guest thread while compiled translations are retained
// in the process-wide memo. An evicted block that runs again is not free: it is
// re-decoded and re-adopted from the memo (and re-linked), and on a thread
// whose working set exceeds the cap that churn sits on the critical path.
//
// The cost of an entry: an LRU list node holding a BasicBlock (128 bytes), an
// addr->iterator hash node, a page refcount node and a page->blocks set node.
// The decoded-instruction array (144 bytes per instruction) is NOT a per-entry
// cost any more: since 2026-09 a compiled block releases it
// (release_block_decoded) and rebuilds it only when something walks it.
//
// History: 262,144 until 2026-08-24, cut to 65,536 because cached entries then
// retained their decoded arrays (measured at 65,536: ~2.0M live mallocs,
// 435 MB, in a 1.9 GB launcher process); 16,384 cost a 22% startup
// regression (~12 s of ~43 s splash-to-window) and was reverted to 65,536.
// 2026-10-23, after decoded-array release, 262,144 vs 65,536 on the same
// launcher binary, interleaved, per navigation (.docs/perf/results/,
// 2026-10-23 block-cache capacity): a view switch -29%, a content page -41%,
// the first visit of that view -30..-41%, renderer CPU -34..-47%; whole-tree
// RSS unchanged within the run-to-run spread (8.3 GB both). Override with
// VITREO_JIT_BLOCK_CACHE to re-measure.
static constexpr usize kDefaultBlockCacheCapacity = 262144;

/// Per-thread decoded-block cache capacity, overridable for measurement.
/// `VITREO_JIT_BLOCK_CACHE` is a block count; 0 or unset keeps the default.
usize per_thread_block_cache_capacity() {
    static const usize cap = [] {
        const char* env = std::getenv("VITREO_JIT_BLOCK_CACHE");
        if (env == nullptr) return kDefaultBlockCacheCapacity;
        const long long parsed = std::atoll(env);
        if (parsed <= 0) return kDefaultBlockCacheCapacity;
        return static_cast<usize>(parsed);
    }();
    return cap;
}

/// Rebuild a released block's decoded array before the interpreter walks it.
/// @return true when @p block is safe to interpret; false when the entry is
///         stale and the caller must fall back to single-stepping.
bool ensure_block_decoded(GuestMemory& memory,
                                 cpu::jit::BlockCache& cache,
                                 cpu::jit::BasicBlock& block) {
    if (block.instructions.size() == block.instruction_count) return true;
    if (rematerialize_block_instructions(memory, block)) return true;
    // The guest bytes behind this entry changed. Drop it so the next dispatch
    // decodes and compiles the code that is actually there.
    (void)cache.invalidate(block.start_addr);
    return false;
}

/// Publish a selected compilation, retaining decoded operands when required.
DispatchCompileResult compile_and_publish_dispatch_block(
    cpu::jit::BasicBlock& block, cpu::jit::JitCompiler& compiler) {
    if (auto* compiled = compiler.compile(block)) {
        // Publication may retire the compiler's transient CompiledBlock record.
        // Capture every property needed below before transferring it to cache.
        const u32 code_size = compiled->code_size;
        const bool baked = compiled->decoded_owner != nullptr ||
                           compiled->delegate_insns != nullptr;
        compiler.publish_compiled_block(block, *compiled);
        if (block.compiled_code != nullptr) {
            (void)cpu::jit::release_block_decoded(block, baked);
        }
        return {DispatchCompileStatus::Published, code_size};
    }

    // Arena pressure is transient: the next dispatch must be allowed to retry.
    if (compiler.compile_deferred()) {
        return {DispatchCompileStatus::Deferred, 0};
    }
    block.compile_failed = true;
    return {DispatchCompileStatus::Failed, 0};
}

/// Count this execution once and apply the same compilation policy in both
/// dispatch loops. A missing compiler means interpreted execution.
DispatchCompileResult compile_dispatch_block(
    cpu::jit::BasicBlock& block, cpu::jit::JitCompiler* compiler) {
    const bool compile_now = compiler && !block.compiled_code &&
        !block.compile_failed &&
        cpu::jit::JitCompiler::execution_reaches_hot_threshold(block);
    ++block.execution_count;
    if (!compile_now) return {};
    return compile_and_publish_dispatch_block(block, *compiler);
}

/// Resolve one dispatch entry for both the main loop and nested callbacks.
/// A null compiler keeps the callback's decoded-block interpreter mode. Native
/// code is synchronized before lookup and adopted code is linked only after
/// runtime probe/thunk vetoes. Scheduling, compilation and execution remain
/// with the calling loop.
cpu::jit::BasicBlock* prepare_dispatch_block(
    cpu::CpuState& cpu, GuestMemory& memory, cpu::jit::BlockCache& block_cache,
    cpu::jit::JitCompiler* jit_compiler,
    winapi::ThunkRegistry& thunk_registry) {
    if (jit_compiler) {
        jit_compiler->set_thunk_region(thunk_registry.thunk_base(),
                                       thunk_registry.thunk_end());
        // L3: which of those thunks an API-call tail may serve in-chain.
        jit_compiler->set_inchain_slot_map(thunk_registry.in_chain_slot_map());
        // Reclaim only between native entries, then discard stale local state.
        (void)cpu::jit::JitCodeArena::reclaim_arena_if_requested();
        (void)jit_compiler->synchronize_arena_generation();
        // Executable writes and FlushInstructionCache affect all guest threads.
        (void)jit_compiler->synchronize_executable_writes();
        jit_compiler->service_cold_residency();
    }
    auto* block = block_cache.lookup(cpu.rip);
    bool adopted_shared = false;
    if (!block) {
        cpu::jit::BasicBlock shared_block;
        if (jit_compiler && jit_compiler->try_adopt_shared_block(
                cpu.rip, cpu.is_64bit(), shared_block)) {
            block = block_cache.insert(std::move(shared_block));
            adopted_shared = true;
        } else {
            const bool ts_timing = cpu::jit::tstats::measuring();
            const u64 ts_start = ts_timing ? cpu::jit::tstats::now_ticks() : 0;
            auto new_block = decode_basic_block(memory, cpu.rip, cpu.is_64bit());
            if (ts_timing) {
                cpu::jit::tstats::note_decode(
                    cpu::jit::tstats::ticks_to_ns(cpu::jit::tstats::now_ticks() - ts_start));
            }
            const bool empty = new_block.instruction_count == 0;
            diagnostics::perf_note_block_miss(cpu.rip, empty);
            if (!empty) block = block_cache.insert(std::move(new_block));
        }
    }
    // Probe ranges require interpreter-visible writes. Thunk-region entries
    // must never become native chain targets.
    if (block &&
        (mem_watch_force_interpreter().load(std::memory_order_relaxed) ||
         pctrace_overlaps(block->start_addr, block->byte_length) ||
         pc_watch_overlaps(block->start_addr, block->byte_length) ||
         thunk_registry.is_in_thunk_region(block->start_addr))) {
        block->compile_failed = true;
        block->compiled_code = nullptr;
        block->compiled_link_code = nullptr;
        block->compiled_call_code = nullptr;
        block->compiled_pin_layout = 0;
        block->compiled_requires_protection_lease = false;
        // L1 copies compiled_link_code, so withdraw its cached pointer too.
        cpu::jit::l1_note_published(*block);
        return nullptr;
    }

    // Native entries may have released their decoded array. Only a block that
    // will be interpreted or compiled needs it restored at this point.
    if (block && !block->compiled_code && block->instruction_count > 0 &&
        block->instructions.size() != block->instruction_count &&
        !ensure_block_decoded(memory, block_cache, *block)) {
        return nullptr;
    }
    if (adopted_shared && block && block->compiled_code) {
        jit_compiler->link_adopted_block(*block);
    }
    return block;
}

winapi::ModuleManager*              g_module_manager = nullptr;
constinit thread_local cpu::CpuState*         t_active_guest_cpu = nullptr;

void set_module_manager(winapi::ModuleManager* mgr) {
    t_module_manager = mgr;
    pctrace_set_module_manager(mgr);
    // Primary guest execution often hops off the thread that called this
    // (AppKit main vs CPU thread). Worker threads never see the thread-local.
    // Keep a process-wide fallback so x64 .pdata dispatch can find modules.
    if (mgr != nullptr) {
        g_module_manager = mgr;
    }
}

cpu::CpuState* active_guest_cpu() noexcept {
    return t_active_guest_cpu;
}

cpu::CpuState& active_guest_cpu_or(cpu::CpuState& fallback) noexcept {
    return t_active_guest_cpu != nullptr ? *t_active_guest_cpu : fallback;
}
// Owned JIT state for worker threads (main thread uses stack locals)
static thread_local std::unique_ptr<cpu::jit::BlockCache>  t_owned_cache;
static thread_local std::unique_ptr<cpu::jit::JitCompiler> t_owned_compiler;

void init_thread_jit_state(GuestMemory& memory) {
    if (t_block_cache) return;  // already initialized
    bool jit_ok = (std::getenv("VITREO_NO_JIT") == nullptr);
    if (!jit_ok) return;
    t_owned_cache =
        std::make_unique<cpu::jit::BlockCache>(per_thread_block_cache_capacity());
    t_owned_compiler = std::make_unique<cpu::jit::JitCompiler>(memory);
    t_block_cache  = t_owned_cache.get();
    t_jit_compiler = t_owned_compiler.get();
    t_jit_compiler->set_block_cache(t_block_cache);
    t_jit_enabled  = true;
}

} // namespace vitreo::cli
