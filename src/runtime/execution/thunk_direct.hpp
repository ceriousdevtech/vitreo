#pragma once

/// @file thunk_direct.hpp
/// @brief The direct API-thunk crossing (WP-R1, Track R C3 + C6).
///
/// An export with a direct entry (winapi::ThunkRegistry::direct_thunk -- the
/// D3D11/DXGI COM method tables) is served as one plain call. Compared with
/// dispatch_slow_api_thunk it skips only dispatch overhead: resolve_handler,
/// both std::function layers, and the per-crossing diagnostic statics (every
/// diagnostic that needs the full path turns the direct path off), and it
/// locates the return address and the stack arguments with one guest-memory
/// lookup (a checked one whenever there are stack arguments; see
/// run_direct_thunk). It keeps, exactly as the full path does:
///   * the export's own locks -- the direct entry is the export's body, so
///     the D3D11/DXGI device lock is taken inside it;
///   * the x86 callee-stack clobber (maybe_clobber_x86_thunk_callee_stack);
///   * enter_host_call / leave_host_call (SuspendThread parks the thread);
///   * the arena safepoint: the chain's lease is parked for the call;
///   * Win64 nonvolatile GPRs, XMM6-15 and the MXCSR control bits, restored
///     unless the handler transferred control (thunk_sets_fp_control kept);
///   * pending_unwind / thunk_control_transfer / pending_raise, left for the
///     caller exactly as the full path leaves them;
///   * the RET: x64 pops 8, x86 pops 4 + the export's stdcall bytes.
///
/// The L3-K in-chain-only entries (DirectThunk::in_chain_only: a kernel32 or
/// CRT export's registered handler, served only by the in-chain crossing)
/// run through this same function with the full path's TEB.LastErrorValue
/// sync (DirectThunk::sync_last_error), and neither note nor count as a
/// direct COM crossing.
///
/// For the direct entries, TEB.LastErrorValue is NOT synced: measured on Windows,
/// D3D11 and DXGI COM methods leave the calling thread's last error as
/// it was -- device creation calls that fail, context setters, draws, Map,
/// queries, Present, ResizeBuffers, AddRef/Release. Not touching the TEB is
/// therefore the contract, not a shortcut. (The three measured exceptions --
/// a Flush that submits GPU work, the process's first CreateSwapChain and a
/// swap chain's final Release -- are recorded in .docs/contracts/d3d11.md and
/// dxgi.md; neither path models them.)
///
/// The table this reads is the target the in-chain call lever (L3) can
/// branch to from translated code: a published DirectThunk never moves.

#include <vitreo/common.hpp>

#include "core/cpu/jit/jit_code_arena.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"

#include <atomic>

namespace vitreo::cli {

/// What a direct crossing left for its caller (each one maps onto the
/// SlowThunkDispatch the full path would have returned).
enum class DirectThunkResult : u8 {
    /// The handler ran and the RET was emulated: rip is the return address.
    Returned,
    /// The handler moved rip itself (thunk_control_transfer).
    ControlTransferred,
    /// RtlUnwind is in flight (pending_unwind): rip and the nonvolatiles are
    /// the unwind's; leave through the outer dispatcher.
    Unwinding,
    /// RaiseException left rip at the gateway (pending_raise): the outer
    /// SEH path dispatches; no RET was emulated.
    Raising,
    /// The return address could not be read.
    Fatal,
};

/// Whether this process may serve crossings from direct entries: false when
/// any diagnostic that reads the full path's bookkeeping is on (the hostcall
/// ring and its hot/detail/caller/module traces, the call stream,
/// VITREO_API_TRACE, VITREO_ABI_CHECK). VITREO_API_PROFILE builds no direct
/// table. The per-call slow-hostcall threshold is the caller's to check.
/// Decided once, at the first crossing (not at static initialisation: the
/// call-stream gate opens its output file when first asked).
///
/// direct_crossing_blocker() names the diagnostic that keeps it false, or is
/// null: the hostcall ring is also switched on by the machine-wide
/// /tmp/vitreo-hostcall-ring marker, so a test of the direct path checks it
/// rather than read a stray marker as a regression.
[[nodiscard]] const char* direct_crossing_blocker() noexcept;
namespace detail {
extern std::atomic<int> g_direct_crossing_allowed;  // -1 not yet decided
[[nodiscard]] bool decide_direct_crossing_allowed() noexcept;
}  // namespace detail
[[nodiscard]] inline bool direct_crossing_allowed() noexcept {
    const int decided =
        detail::g_direct_crossing_allowed.load(std::memory_order_relaxed);
    return decided >= 0 ? decided != 0
                        : detail::decide_direct_crossing_allowed();
}

/// The direct entry to serve the crossing at @p rip with, or null for the
/// full path: no entry, the lever off (winapi::com_direct::active), or a
/// diagnostic that needs the full path.
[[nodiscard]] inline const winapi::DirectThunk* direct_thunk_for(
    const winapi::ThunkRegistry& registry, GuestAddr rip,
    bool slow_hostcall_diag) noexcept;

/// Serve one crossing from @p direct (see the file comment). @p safepoint,
/// when non-null, is the chain's arena safepoint: parked for the call and
/// taken back before returning (its outcome is left in it).
[[nodiscard]] DirectThunkResult run_direct_thunk(
    const winapi::DirectThunk& direct,
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard,
    cpu::jit::JitCodeArena::HostCallSafepoint* safepoint);

/// Direct crossings the calling thread has made while counting was on.
[[nodiscard]] u64 direct_thunk_crossings_this_thread() noexcept;
/// Tests: count crossings in direct_thunk_crossings_this_thread (off by
/// default, so a crossing pays nothing for the counter).
void set_direct_thunk_counting(bool on) noexcept;

inline const winapi::DirectThunk* direct_thunk_for(
    const winapi::ThunkRegistry& registry, GuestAddr rip,
    bool slow_hostcall_diag) noexcept {
    const winapi::DirectThunk* direct = registry.direct_thunk(rip);
    if (direct == nullptr || slow_hostcall_diag ||
        !winapi::com_direct::active() || !direct_crossing_allowed()) {
        return nullptr;
    }
    return direct;
}

}  // namespace vitreo::cli
