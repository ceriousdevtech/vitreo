// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_inchain_thunk.cpp
/// @brief L3: an API crossing served from translated code (the JIT side).
///
/// An API-call tail's in-chain exit (emit_api_call_exit_body) stores every
/// guest register into CpuState, persists NZCV and BLRs to
/// vitreo_jit_inchain_trampoline below. The trampoline calls
/// JitCompiler::inchain_from_stub, which hands the crossing to the
/// runtime's serve hook (cli::serve_in_chain_thunk: the direct crossing,
/// unchanged) and then decides where execution goes:
///
///   * back into the stub with the successor's link entry: the stub reloads
///     the pins and NZCV from CpuState and branches, exactly like a linked
///     hop -- no dispatcher round trip;
///   * or out of execute() altogether, from the trampoline itself: SP back
///     to native_sp_base, pop the chain's LR frame, return Continue. That is
///     byte-for-byte the shared continue exit's epilogue (pins and NZCV are
///     already in CpuState), so every outcome that does not resume -- a
///     declined call, a raise, a lost lease, a SuspendThread -- reaches the
///     dispatcher in the state the dispatcher path would have produced, and
///     no arena byte runs after a call that may have let the arena move.
///
/// Resuming is allowed only when everything a dispatcher re-entry would
/// re-establish is unchanged by the call (see inchain_from_stub). The
/// trampoline lives in vitreo's own text, never in the arena, which is what
/// makes leaving from it safe whatever happened to the arena meanwhile.

#include <core/cpu/jit/block_cache.hpp>
#include <core/cpu/jit/hot_block_dump.hpp>
#include <core/cpu/jit/jit_code_arena.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include <core/cpu/jit/jit_coverage.hpp>
#include <core/cpu/jit/jit_stats.hpp>
#include <core/memory/mem_watch.hpp>

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

/// The trampoline's C++ call returns this pair in X0/X1 (AAPCS64: a 16-byte
/// non-HFA composite is returned in two registers).
extern "C" {
struct VitreoInChainNext {
    void* link_entry;
    vitreo::u64 exit_sp;
};
}
static_assert(sizeof(VitreoInChainNext) == 16);

// Referenced only by the module asm below: `used` keeps LTO from
// internalizing or dropping it.
extern "C" __attribute__((used)) VitreoInChainNext vitreo_jit_inchain_from_stub(
    vitreo::cpu::CpuState* cpu, vitreo::cpu::jit::JitContext* ctx) noexcept {
    const auto next = vitreo::cpu::jit::JitCompiler::inchain_from_stub(cpu, ctx);
    return VitreoInChainNext{next.link_entry, next.exit_sp};
}

static_assert(static_cast<int>(vitreo::cpu::jit::JitExecResult::Continue) == 0,
              "the trampoline's exit returns MOV W0, #0 as Continue");

// Entered by BLR X16 from an in-chain exit, with X19 = CpuState*, X20 =
// JitContext*, X30 = the stub's continuation, SP 16-byte aligned (every
// translated frame is a multiple of 16). Saves X29/X30, calls the C++ half,
// and either returns into the stub (X0 = successor link entry) or leaves
// execute() exactly as the shared continue exit does: SP := native_sp_base
// (discarding any native guest-CALL frames, as the process-wide continue
// exit always does), LDR X30 from the chain's LR frame, W0 = Continue, RET
// into execute()'s asm after its BLR.
#if defined(__APPLE__)
#define VITREO_L3_SYM(name) "_" #name
#else
#define VITREO_L3_SYM(name) #name
#endif
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl " VITREO_L3_SYM(vitreo_jit_inchain_trampoline) "\n"
    VITREO_L3_SYM(vitreo_jit_inchain_trampoline) ":\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    "    mov x0, x19\n"
    "    mov x1, x20\n"
    "    bl " VITREO_L3_SYM(vitreo_jit_inchain_from_stub) "\n"
    "    ldp x29, x30, [sp], #16\n"
    "    cbz x0, 1f\n"
    "    ret\n"
    "1:\n"
    "    mov sp, x1\n"
    "    ldr x30, [sp], #16\n"
    "    mov w0, #0\n"
    "    ret\n");
#undef VITREO_L3_SYM

namespace vitreo::cpu::jit {

namespace {

/// -1: follow the environment; 0/1: forced by a test.
std::atomic<int> g_inchain_codegen_force{-1};
/// The runtime's gate (set_inchain_thunks_runtime_gate). Open by default, so
/// a compiler used without a runtime (unit tests) follows the environment.
std::atomic<bool> g_inchain_runtime_gate{true};

/// Tests: per-thread counts, kept only while counting is on.
std::atomic<bool> g_inchain_counting{false};
thread_local u64 t_inchain_resumed = 0;
thread_local u64 t_inchain_left = 0;
thread_local u64 t_inchain_broke_outer = 0;

void note_left(JitContext& ctx) noexcept {
    ++ctx.inchain_left;
    if (g_inchain_counting.load(std::memory_order_relaxed)) ++t_inchain_left;
}

/// Leave, and have the chain loop break to the outer dispatcher.
void note_break_outer(JitContext& ctx) noexcept {
    note_left(ctx);
    ctx.inchain_break = static_cast<u32>(InChainBreak::Outer);
    if (g_inchain_counting.load(std::memory_order_relaxed)) {
        ++t_inchain_broke_outer;
    }
}

void note_resumed(JitContext& ctx) noexcept {
    ++ctx.inchain_resumed;
    if (g_inchain_counting.load(std::memory_order_relaxed)) {
        ++t_inchain_resumed;
    }
}

/// The JIT diagnostics that count dispatcher entries or block executions:
/// with any of them on, API-call tails are emitted exactly as before, so
/// what they report stays comparable (an in-chain call is a dispatcher
/// entry that did not happen).
[[nodiscard]] const char* jit_diagnostic_needing_dispatcher() noexcept {
    if (const char* v = std::getenv("VITREO_JIT_EXIT_STATS");
        v != nullptr && v[0] != '0') {
        return "VITREO_JIT_EXIT_STATS";
    }
    if (const char* w = std::getenv("VITREO_PERF_WINDOW");
        w != nullptr && w[0] != '\0') {
        return "VITREO_PERF_WINDOW";
    }
    if (coverage::enabled()) return "VITREO_JIT_COVERAGE";
    if (stats::enabled()) return "the JIT stats";
    if (hot_dump::enabled()) return "the hot-block dump";
    return nullptr;
}

bool decide_inchain_codegen() noexcept {
    // Kill switch, for bisection and the cross-run gate.
    if (const char* v = std::getenv("VITREO_JIT_INCHAIN_THUNKS");
        v != nullptr && v[0] == '0' && v[1] == '\0') {
        std::fprintf(stderr,
                     "[l3] VITREO_JIT_INCHAIN_THUNKS=0: in-chain API thunk "
                     "calls OFF\n");
        return false;
    }
    if (const char* why = jit_diagnostic_needing_dispatcher()) {
        std::fprintf(stderr,
                     "[l3] in-chain API thunk calls off: %s counts dispatcher "
                     "entries\n",
                     why);
        return false;
    }
    return true;
}

/// The successor's link entry for @p rip from this thread's L1 (both
/// ways), the same probe the emitted tails make.
[[nodiscard]] void* l1_link_entry(const JitContext& ctx, GuestAddr rip) noexcept {
    const auto* l1 = reinterpret_cast<const BlockCache::L1Entry*>(
        ctx.block_cache_l1);
    if (l1 == nullptr || rip == 0) return nullptr;
    const BlockCache::L1Entry* set = l1 + BlockCache::l1_index(rip);
    for (u32 way = 0; way < BlockCache::kL1Ways; ++way) {
        // Same ABI as this chain (the layout is fixed today; checked, not
        // assumed, since a mismatch would enter with the wrong pins).
        if (set[way].tag == rip &&
            set[way].pin_layout == ctx.active_pin_layout) {
            return set[way].link_code;
        }
    }
    return nullptr;
}

}  // namespace

bool JitCompiler::inchain_thunks_codegen() noexcept {
    if (!g_inchain_runtime_gate.load(std::memory_order_relaxed)) return false;
    const int forced = g_inchain_codegen_force.load(std::memory_order_relaxed);
    if (forced >= 0) return forced != 0;
    static const bool on = decide_inchain_codegen();
    return on;
}

void JitCompiler::set_inchain_thunks_runtime_gate(bool open) noexcept {
    g_inchain_runtime_gate.store(open, std::memory_order_relaxed);
}

void JitCompiler::set_inchain_thunks_codegen_for_test(int value) noexcept {
    g_inchain_codegen_force.store(value < 0 ? -1 : (value != 0 ? 1 : 0),
                                  std::memory_order_relaxed);
}

void JitCompiler::set_inchain_counting_for_test(bool on) noexcept {
    g_inchain_counting.store(on, std::memory_order_relaxed);
}

u64 JitCompiler::inchain_resumed_this_thread() noexcept {
    return t_inchain_resumed;
}

u64 JitCompiler::inchain_left_this_thread() noexcept { return t_inchain_left; }

u64 JitCompiler::inchain_broke_outer_this_thread() noexcept {
    return t_inchain_broke_outer;
}

JitCompiler::InChainNext JitCompiler::inchain_from_stub(
    CpuState* cpu, JitContext* ctx) noexcept {
    const InChainNext leave{nullptr, ctx->native_sp_base};
    const InChainThunkHook hook = ctx->inchain_hook;
    JitCompiler* const jit = ctx->inchain_owner;
    if (hook == nullptr || jit == nullptr || cpu != ctx->cpu) return leave;
    // Watched writes are attributed per dispatched block (execute()).
    if (mem_watch_enabled()) return leave;

    // The code this crossing would resume in must be as live as a linked
    // hop's target: no retirement round since this compiler last
    // synchronized -- execute()'s own entry test. Taken BEFORE the call: the
    // crossing parks the chain's lease, and the re-published epoch must then
    // not be what lets a chunk retired earlier in this chain be recycled
    // under the code we return to.
    const u64 retire_seq = JitCodeArena::arena_retire_seq();
    if (retire_seq != jit->m_arena_retire_seq) return leave;
    GuestMemory& memory = jit->m_memory;
    const u32 executes = jit->m_exec_sample;

    const InChainThunkResult served = hook(*cpu, ctx->inchain_env);
    switch (served) {
    case InChainThunkResult::NotServed:
        // Nothing ran: the chain loop serves the call as it always did.
        return leave;
    case InChainThunkResult::Returned:
    case InChainThunkResult::Resume:
        break;
    case InChainThunkResult::BreakOuter:
        note_break_outer(*ctx);
        return leave;
    case InChainThunkResult::Fatal:
        note_left(*ctx);
        ctx->inchain_break = static_cast<u32>(InChainBreak::Fatal);
        return leave;
    }

    // The export ran. Resume in translated code only when the state a
    // dispatcher re-entry would re-establish is current -- compared against
    // what this compiler last SYNCHRONIZED, not against a snapshot taken
    // before the call, so a foreign change from earlier in the chain is not
    // carried past the API boundary any more than the dispatcher path
    // carried it:
    //  * no execute() on this compiler: no guest code ran inside the export
    //    (the runtime reports that as BreakOuter itself; this is the second
    //    line);
    //  * no retirement round (the code under us and every cached pointer);
    //  * no unsynchronized executable write (synchronize_executable_writes);
    //  * where execute() revalidates its memory caches (everything but the
    //    32-bit linear JIT), no unsynchronized map/unmap/commit or protect
    //    change (revalidate_memory_caches);
    //  * no link-generation bump (execute() republishes it and drops the
    //    return predictors).
    // When any of it moved, the chain loop must not continue either: a
    // nested dispatch, or an executable write the export made (its observer
    // invalidates synchronously), may have freed the very BasicBlock the
    // chain loop holds, and the chain loop's self-RIP path would run it
    // again. Leave for the OUTER dispatcher, which synchronizes and looks
    // the block up afresh -- as the event pump does after a safepoint that
    // did not resume Continue.
    const bool executing_linear = memory.is_linear() && !cpu->is_64bit() &&
                                  jit->m_cfg.linear_jit;
    const bool revalidates = !executing_linear && !jit->m_cfg.no_jitcache;
    if (jit->m_exec_sample != executes ||
        JitCodeArena::arena_retire_seq() != retire_seq ||
        memory.executable_generation() != jit->m_executable_generation ||
        (revalidates && (memory.generation() != jit->m_region_gen ||
                         memory.protect_sequence() != jit->m_protect_seq)) ||
        s_link_generation.load(std::memory_order_relaxed) !=
            ctx->link_generation) {
        note_break_outer(*ctx);
        return leave;
    }
    // A SuspendThread is pending: nothing moved, so the chain loop may take
    // it (its suspend_pending() check parks before more guest code).
    if (served == InChainThunkResult::Resume) {
        note_left(*ctx);
        return leave;
    }

    void* next = l1_link_entry(*ctx, static_cast<GuestAddr>(cpu->rip));
    if (next == nullptr && jit->m_block_cache != nullptr &&
        ctx->block_cache_l1 != nullptr) {
        // The chain loop's own post-thunk lookup (it refreshes L1).
        if (const BasicBlock* block =
                jit->m_block_cache->lookup_fast(static_cast<GuestAddr>(cpu->rip))) {
            if (block->compiled_pin_layout == ctx->active_pin_layout) {
                next = block->compiled_link_code;
            }
        }
    }
    if (next == nullptr) {
        note_left(*ctx);
        return leave;
    }
    // execute() makes the arena executable for this thread before every
    // entry; a host call is allowed to have left it writable.
    if (bool* writable = jit->m_tls_jit_writable;
        writable != nullptr && *writable) {
        pthread_jit_write_protect_np(1);
        *writable = false;
    }
    note_resumed(*ctx);
    return InChainNext{next, 0};
}

}  // namespace vitreo::cpu::jit
