#pragma once

/// @file thunk_inchain.hpp
/// @brief L3: API crossings served from translated code (the runtime side).
///
/// An API-call tail whose target is a thunk reaches serve_in_chain_thunk
/// from translated code (cpu::jit::JitContext::inchain_hook) instead of
/// leaving execute() for the chain loop's try_chain_thunk_inline. It serves
/// exactly the crossings the direct path serves (thunk_direct.hpp), with the
/// same function, run_direct_thunk -- so the export's own locks, the arena
/// safepoint, enter_host_call / leave_host_call (SuspendThread), the x86
/// callee-stack clobber, the Win64 nonvolatile and MXCSR-control restore,
/// the stdcall pop and the TEB last-error contract are the direct path's,
/// unchanged -- but only for exports whose contract class allows resuming
/// in-chain (winapi::InChainClass: A or AGuestComRef). Everything else, and
/// everything when the lever or a diagnostic says so, is NotServed and takes
/// today's path untouched.
///
/// After the call it reports how translated code may continue:
///   Returned    RET emulated; nothing forbids resuming in-chain (the JIT
///               side then checks what a dispatcher re-entry re-establishes);
///   Resume      a SuspendThread is pending: pass the dispatcher, which parks;
///   BreakOuter  what try_chain_thunk_inline answers with BreakOuter: a raise
///               (t_chain_slow_thunk_pending_raise set, as the direct path
///               does), an unwind, a control transfer, a lost or retired
///               lease (chain_may_resume false), or guest code that ran
///               inside the export (a guest COM Release); and, as the
///               dispatcher tests has_exited() after every thunk, a process
///               that has exited by the time the call returns (ExitProcess
///               on any thread: a contended EnterCriticalSection then
///               returns without ownership, critical_section.cpp) -- the
///               chain loop then stops the thread, never running the
///               instruction after the call;
///   Fatal       the return address was unreadable.
///
/// Switches:
///   VITREO_JIT_INCHAIN_THUNKS=0  no stub is emitted at all (the JIT's codegen
///                                switch; byte-identical API-call tails).
///   VITREO_AB_CYCLE_ONLY=l3      with VITREO_AB_CYCLE_S: the off arm
///                                declines every direct (L3) call at run
///                                time (the stub still runs its slab test
///                                and BLR); `l3k` does the same for the
///                                L3-K exports.
///   VITREO_D3D11_COM_DIRECT=0    no direct crossing, so no D3D11/DXGI call is
///                                in-chain: the direct slots stay unmarked
///                                (no call-out). With L3-K on (VITREO_L3K)
///                                the stub is still emitted and the L3-K
///                                kernel32/CRT exports are still served
///                                in-chain ("WP-R1 off, L3 unmarked, L3-K
///                                on"); with both off no stub is emitted.
///   VITREO_L3K=0                 no L3-K entry is built (host_in_chain.hpp).
/// Diagnostics that need the full path force NotServed (logged once):
/// everything direct_crossing_allowed() refuses, the per-call slow-hostcall
/// threshold, VITREO_ABI_ALIGN_TRAP, and (at codegen) the perf window, the
/// exit stats, JIT coverage/stats and the hot-block dump.

#include <vitreo/ab_cycle.hpp>
#include <vitreo/common.hpp>

#include "core/cpu/jit/jit_code_arena.hpp"
#include "core/cpu/jit/jit_context.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"

namespace vitreo::winapi {
class ProcessManager;
}  // namespace vitreo::winapi

namespace vitreo::cli {

struct FastThunkAddrs;

/// What a chain loop lends serve_in_chain_thunk while it runs translated
/// code (published through JitCompiler::InChainScope). One per dispatch
/// loop invocation; `chain_lease` is set for each chain.
struct InChainThunkEnv {
    const winapi::ThunkRegistry* registry{nullptr};
    GuestMemory* memory{nullptr};
    winapi::Marshaler* marshaler{nullptr};
    winapi::ThreadSuspendRegistry::PublishGuard* suspend_guard{nullptr};
    /// The running chain's arena lease, parked for the host call exactly as
    /// try_chain_thunk_inline parks it.
    cpu::jit::JitCodeArena::ArenaReadLease* chain_lease{nullptr};
    /// The loop's per-call slow-hostcall diagnostic (VITREO_SLOW_HOSTCALL_MS):
    /// it times the full path, so it forces the full path.
    bool slow_hostcall_diag{false};
    /// The "l3" ab-cycle arm, re-read once per kResample calls.
    diagnostics::AbCycleSampler ab_sampler{};
    /// L3-K: the loop's resolved fast-thunk set (thunk_fast_path.hpp), whose
    /// inline bodies serve the L3-K exports that have one. Null: no L3-K
    /// export is served in-chain.
    const FastThunkAddrs* fast_thunks{nullptr};
    /// The "l3k" ab-cycle arm.
    diagnostics::AbCycleSampler l3k_sampler{};
    /// The loop's process: after every served call the crossing tests
    /// has_exited(), as the dispatcher does after every thunk, and leaves
    /// (BreakOuter) for the chain loop's stop. Null (tests of the serve
    /// alone): no process to observe.
    const winapi::ProcessManager* process{nullptr};
};

/// The in-chain serve hook (cpu::jit::InChainThunkHook); @p env is an
/// InChainThunkEnv.
[[nodiscard]] cpu::jit::InChainThunkResult serve_in_chain_thunk(
    cpu::CpuState& cpu, void* env);

/// Whether this process serves in-chain crossings at all: false when a
/// diagnostic needs the full dispatch path (logged once, at the first
/// call). The per-call switches (ab-cycle, slow-hostcall) are separate.
[[nodiscard]] bool in_chain_thunks_allowed() noexcept;

/// The process-wide diagnostic that keeps the in-chain crossing (L3) or the
/// in-chain kernel32/CRT exports (L3-K) off, or null when neither gate has
/// one: what in_chain_thunks_allowed() and L3-K's own gate decide from. For
/// the tests of those paths, which a machine-wide marker
/// (/tmp/vitreo-hostcall-ring) or an inherited trace switch would otherwise
/// fail wholesale.
[[nodiscard]] const char* in_chain_full_path_diagnostic() noexcept;

/// Whether anything could ever be served in-chain in this process right
/// now: the direct crossing (VITREO_D3D11_COM_DIRECT) or L3-K (VITREO_L3K) is
/// statically on (or forced by a test), no diagnostic needs the full path,
/// and in_chain_thunks_allowed(). A run loop hands it to
/// JitCompiler::set_inchain_thunks_runtime_gate before it compiles, so an
/// arm without the lever emits no in-chain stub at all.
[[nodiscard]] bool in_chain_thunks_possible() noexcept;

/// Tests: crossings the calling thread served in-chain (counted only while
/// counting is on, so a crossing pays nothing for it otherwise).
[[nodiscard]] u64 in_chain_crossings_this_thread() noexcept;
/// Tests: of those, the L3-K ones (DirectThunk::in_chain_only).
[[nodiscard]] u64 in_chain_host_crossings_this_thread() noexcept;
/// Tests: every call the calling thread's translated code OFFERED the
/// crossing -- each stub call-out, served or declined (counted only while
/// counting is on). Zero proves a slot never called out.
[[nodiscard]] u64 in_chain_offers_this_thread() noexcept;
void set_in_chain_crossing_counting(bool on) noexcept;

/// guest_run_loop.cpp: the in-chain crossing raised (pending_raise), so the
/// outer loop must run SEH without re-running the handler -- what
/// try_chain_thunk_inline's direct path sets on a Raising crossing.
void note_chain_thunk_pending_raise() noexcept;

/// guest_run_loop.cpp: the in-chain crossing left an unwind in flight
/// (pending_unwind), so the outer loop acts on it without re-running the
/// handler for the gateway rip still names -- the direct path's Unwinding
/// exit from try_chain_thunk_inline.
void note_chain_thunk_unwinding() noexcept;

}  // namespace vitreo::cli
