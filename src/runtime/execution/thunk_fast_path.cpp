// SPDX-License-Identifier: LGPL-2.1-only

/// @file thunk_fast_path.cpp
/// @brief Inline handling for the hottest kernel32 thunks.
///        See thunk_fast_path.hpp.
///
/// Split out of cli/commands/execution_loop.cpp. Everything here is declared
/// in the header, so nothing may sit in an anonymous namespace — that would
/// give it internal linkage regardless of `static`, and the dispatch loop
/// calls all of it.

#include "winapi/kernel32/synchronization/srw_word.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include <winapi/core/api_profile.hpp>

#include <vitreo/time_diag.hpp>

#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_block.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/thread/tls.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "winapi/common/timer_resolution.hpp"
#include "winapi/common/host_yield.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/memory_region.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/execution/exception_dispatch.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "runtime/execution/exception_dispatch32.hpp"
#include "runtime/diagnostics/fault_report.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#include <vitreo/virtual_tsc.hpp>
#include <csignal>

namespace vitreo::cli {

namespace {
// Process-wide CS manager for Enter/Leave/TryEnter fast thunks. Bound from
// run.cpp alongside set_module_manager; workers share the same process mgr.
winapi::CriticalSectionManager* g_fast_thunk_cs_mgr = nullptr;
winapi::SyncManager* g_fast_thunk_sync_mgr = nullptr;
}  // namespace

void set_fast_thunk_cs_manager(winapi::CriticalSectionManager* mgr) {
    g_fast_thunk_cs_mgr = mgr;
}

void set_fast_thunk_sync_manager(winapi::SyncManager* mgr) {
    g_fast_thunk_sync_mgr = mgr;
}

winapi::CriticalSectionManager* fast_thunk_cs_manager() noexcept {
    return g_fast_thunk_cs_mgr;
}

winapi::SyncManager* fast_thunk_sync_manager() noexcept {
    return g_fast_thunk_sync_mgr;
}

// =============================================================================
// Hot-thunk fast path (Stage B2)
// -----------------------------------------------------------------------------
// GetLastError / SetLastError / GetCurrentThreadId are called extremely
// frequently from CRT/COM/winhttp code paths. The generic dispatch path
// pays for: resolve_handler hash lookup, std::function invocation through
// the Marshaler wrapper, callee-saved guest register save/restore, and a
// pre+post TEB[0x34] sync (read_u32 + write_u32 on every call).
//
// The fast path bypasses all of that by inlining the contract directly,
// then falling through to the standard thunk RET emulation.
// =============================================================================
// ---------------------------------------------------------------------------
// O(1) membership probe for the fast-thunk address set.
//
// maybe_fast_thunk_call_inner is a 30-way chain of `rip == f.<field>` compares
// and is_lean_hot_thunk is a 19-way chain, and BOTH are walked on every thunk
// dispatch. A dispatch that is not a fast thunk -- the common case, since the
// registered-handler path is much larger than this set -- pays all 49 compares
// before falling through. Measured on a live browser renderer,
// `is_lean_hot_thunk` was the single largest non-idle symbol in the process and
// import-thunk dispatch was 19.5% of non-parked time against 4.2% spent
// actually running translated guest code.
//
// So reject in O(1) first. An open-addressed set keyed on the dispatch address
// makes a miss one multiply, one mask and one load. Layout-independent on
// purpose: it assumes nothing about thunk arena spacing, which is the kind of
// assumption that silently rots.
namespace {

constexpr u8 kFastThunkHasBody = 1u << 0;
constexpr u8 kFastThunkLeanHot = 1u << 1;

struct ThunkProbe {
    static constexpr usize kSlots = 256;  // >= 4x the ~34 live entries
    GuestAddr addr[kSlots]{};
    u8        flags[kSlots]{};

    static usize hash(GuestAddr a) noexcept {
        // Thunk addresses are at least 8-byte aligned, so drop the dead bits
        // before mixing or every entry lands in the same few slots.
        return static_cast<usize>(
            ((a >> 3) * 0x9E3779B97F4A7C15ull) >> 56) & (kSlots - 1);
    }
    void insert(GuestAddr a, u8 bits) noexcept {
        if (a == 0) return;
        usize i = hash(a);
        for (usize n = 0; n < kSlots; ++n) {
            if (addr[i] == 0) { addr[i] = a; flags[i] = bits; return; }
            if (addr[i] == a) { flags[i] = static_cast<u8>(flags[i] | bits); return; }
            i = (i + 1) & (kSlots - 1);
        }
    }
    [[nodiscard]] u8 lookup(GuestAddr a) const noexcept {
        if (a == 0) return 0;
        usize i = hash(a);
        for (usize n = 0; n < kSlots; ++n) {
            const GuestAddr slot = addr[i];
            if (slot == a) return flags[i];
            if (slot == 0) return 0;
            i = (i + 1) & (kSlots - 1);
        }
        return 0;
    }
};

// Each resolved set owns its immutable membership probe. Concurrent execution
// contexts cannot replace one another's probe, and cached copies retain it.
[[nodiscard]] u8 fast_thunk_flags(GuestAddr rip, const FastThunkAddrs& f) noexcept {
    const auto* probe = static_cast<const ThunkProbe*>(f.membership_probe.get());
    return probe != nullptr ? probe->lookup(rip) : 0xFFu;
}

void publish_thunk_probe(FastThunkAddrs& f) {
    auto probe = std::make_shared<ThunkProbe>();
    // Every address the comparison chain in maybe_fast_thunk_call_inner tests.
    for (GuestAddr a : {f.get_last_error, f.set_last_error,
                        f.get_current_thread_id, f.get_current_process_id,
                        f.get_tick_count, f.get_tick_count64, f.time_get_time,
                        f.query_performance_counter,
                        f.query_performance_frequency,
                        f.sleep, f.sleep_ex, f.tls_get_value, f.tls_set_value,
                        f.interlocked_increment, f.interlocked_decrement,
                        f.interlocked_compare_exchange, f.is_debugger_present,
                        f.heap_alloc, f.heap_free, f.wait_for_single_object,
                        f.release_semaphore, f.get_overlapped_result,
                        f.sleep_condition_variable_cs,
                        f.acquire_srw_exclusive, f.release_srw_exclusive,
                        f.try_acquire_srw_exclusive, f.acquire_srw_shared,
                        f.release_srw_shared, f.try_acquire_srw_shared,
                        f.enter_critical_section, f.leave_critical_section,
                        f.try_enter_critical_section,
                        f.fgetc, f.ungetc}) {
        probe->insert(a, kFastThunkHasBody);
    }
    // The subset is_lean_hot_thunk answers true for.
    for (GuestAddr a : {f.heap_alloc, f.heap_free, f.wait_for_single_object,
                        f.release_semaphore, f.get_overlapped_result,
                        f.sleep_condition_variable_cs, f.sleep, f.sleep_ex,
                        f.enter_critical_section, f.leave_critical_section,
                        f.try_enter_critical_section,
                        f.acquire_srw_exclusive, f.release_srw_exclusive,
                        f.try_acquire_srw_exclusive, f.acquire_srw_shared,
                        f.release_srw_shared, f.try_acquire_srw_shared,
                        f.fgetc, f.ungetc}) {
        probe->insert(a, kFastThunkLeanHot);
    }
    f.membership_probe = std::move(probe);
}

}  // namespace

void resolve_fast_thunks(FastThunkAddrs& f,
                                const winapi::ThunkRegistry& reg) {
    if (f.resolved) return;
    // Nested run_guest_callback (bsearch comparators, WndProc) constructs a
    // fresh FastThunkAddrs every time. get_thunk_address is a string-hash
    // lookup that allocates; doing it 12 times per compare showed up as
    // resolve_fast_thunks in the load sample. Cache per thread + registry.
    thread_local FastThunkAddrs t_cached{};
    thread_local u64 t_cached_instance = 0;
    thread_local usize t_cached_count = 0;
    if (t_cached.resolved && t_cached_instance == reg.instance_id() &&
        t_cached_count == reg.function_count()) {
        f = t_cached;
        return;
    }
    // get_thunk_address returns the guest-visible entry stub (NOP sled). After
    // maybe_skip_thunk_nop_sled, cpu.rip is the dispatch gateway
    // (stub + STUB_SIZE). Store that gateway so maybe_fast_thunk_call matches.
    auto store = [&](GuestAddr& slot, auto result) {
        if (result.is_ok()) {
            slot = reg.dispatch_address_for_export(result.value());
        }
    };
    store(f.get_last_error,
          reg.get_thunk_address("kernel32.dll", "GetLastError"));
    store(f.set_last_error,
          reg.get_thunk_address("kernel32.dll", "SetLastError"));
    store(f.get_current_thread_id,
          reg.get_thunk_address("kernel32.dll", "GetCurrentThreadId"));
    // Clock reads. Guests poll these in tight wait loops. An exclusive-mode
    // title spins on GetTickCount ~309,000 times a second, and on Windows they
    // are a read of
    // KUSER_SHARED_DATA rather than a call. Routing them through the full
    // host-call path (handler resolve, std::function dispatch, hostcall-ring
    // register snapshot, TEB sync) makes a nearly-free Windows API one of the
    // most expensive things the guest can do.
    store(f.get_tick_count,
          reg.get_thunk_address("kernel32.dll", "GetTickCount"));
    store(f.get_tick_count64,
          reg.get_thunk_address("kernel32.dll", "GetTickCount64"));
    store(f.time_get_time,
          reg.get_thunk_address("winmm.dll", "timeGetTime"));
    store(f.query_performance_counter,
          reg.get_thunk_address("kernel32.dll", "QueryPerformanceCounter"));
    store(f.query_performance_frequency,
          reg.get_thunk_address("kernel32.dll", "QueryPerformanceFrequency"));
    store(f.get_current_process_id,
          reg.get_thunk_address("kernel32.dll", "GetCurrentProcessId"));
    store(f.sleep, reg.get_thunk_address("kernel32.dll", "Sleep"));
    store(f.sleep_ex, reg.get_thunk_address("kernel32.dll", "SleepEx"));
    store(f.tls_get_value,
          reg.get_thunk_address("kernel32.dll", "TlsGetValue"));
    store(f.tls_set_value,
          reg.get_thunk_address("kernel32.dll", "TlsSetValue"));
    store(f.interlocked_increment,
          reg.get_thunk_address("kernel32.dll", "InterlockedIncrement"));
    store(f.interlocked_decrement,
          reg.get_thunk_address("kernel32.dll", "InterlockedDecrement"));
    store(f.interlocked_compare_exchange,
          reg.get_thunk_address("kernel32.dll", "InterlockedCompareExchange"));
    store(f.is_debugger_present,
          reg.get_thunk_address("kernel32.dll", "IsDebuggerPresent"));
    // ntdll!RtlGetCurrentPeb is not registered — skip silently.
    store(f.heap_alloc, reg.get_thunk_address("kernel32.dll", "HeapAlloc"));
    store(f.heap_free, reg.get_thunk_address("kernel32.dll", "HeapFree"));
    store(f.wait_for_single_object,
          reg.get_thunk_address("kernel32.dll", "WaitForSingleObject"));
    store(f.release_semaphore,
          reg.get_thunk_address("kernel32.dll", "ReleaseSemaphore"));
    store(f.get_overlapped_result,
          reg.get_thunk_address("kernel32.dll", "GetOverlappedResult"));
    store(f.sleep_condition_variable_cs,
          reg.get_thunk_address("kernel32.dll", "SleepConditionVariableCS"));
    // Critical-section enter/leave dominate 32-bit loads. Init/Delete stay
    // on the slow path: they allocate guest debug-info and rewrite the full
    // CRITICAL_SECTION layout.
    // SRW locks. The hot lock is an SRWLOCK, so a renderer process
    // takes and releases them constantly: measured on a renderer sitting idle
    // on a static login screen, AcquireSRWLockExclusive +
    // ReleaseSRWLockExclusive + TryAcquireSRWLockExclusive were 1.9M of 2.0M
    // slow thunk dispatches (95%). On Windows each is a handful of
    // instructions on one pointer-sized word; paying a full marshaling round
    // trip for that is what leaves the UI with no headroom to answer input.
    // Init/Delete stay on the slow path (they rewrite the guest lock word and
    // register the lock).
    store(f.acquire_srw_exclusive,
          reg.get_thunk_address("kernel32.dll", "AcquireSRWLockExclusive"));
    store(f.release_srw_exclusive,
          reg.get_thunk_address("kernel32.dll", "ReleaseSRWLockExclusive"));
    store(f.try_acquire_srw_exclusive,
          reg.get_thunk_address("kernel32.dll", "TryAcquireSRWLockExclusive"));
    store(f.acquire_srw_shared,
          reg.get_thunk_address("kernel32.dll", "AcquireSRWLockShared"));
    store(f.release_srw_shared,
          reg.get_thunk_address("kernel32.dll", "ReleaseSRWLockShared"));
    store(f.try_acquire_srw_shared,
          reg.get_thunk_address("kernel32.dll", "TryAcquireSRWLockShared"));
    store(f.enter_critical_section,
          reg.get_thunk_address("kernel32.dll", "EnterCriticalSection"));
    store(f.leave_critical_section,
          reg.get_thunk_address("kernel32.dll", "LeaveCriticalSection"));
    store(f.try_enter_critical_section,
          reg.get_thunk_address("kernel32.dll", "TryEnterCriticalSection"));
    // Byte-at-a-time parsers can make millions of CRT calls. Keep the real
    // CRT handlers (including stream locking and error behavior), but allow
    // their dispatch to skip diagnostic stack-clobber bookkeeping just like
    // the other lean hot handlers below.
    store(f.fgetc, reg.get_thunk_address("msvcrt.dll", "fgetc"));
    store(f.ungetc, reg.get_thunk_address("msvcrt.dll", "ungetc"));
    f.resolved = true;
    if (auto handler = reg.resolve_handler(f.fgetc); handler.is_ok())
        f.fgetc_handler = handler.value();
    if (auto handler = reg.resolve_handler(f.ungetc); handler.is_ok())
        f.ungetc_handler = handler.value();
    publish_thunk_probe(f);
    t_cached = f;
    t_cached_instance = reg.instance_id();
    t_cached_count = reg.function_count();
}

[[nodiscard]] bool is_lean_hot_thunk(
    GuestAddr rip,
    const FastThunkAddrs& f) {
    // O(1) when the probe is published (always, after the first
    // resolve_fast_thunks). 0xFF means "not built yet" — fall back to the
    // literal chain rather than guess, because answering `true` wrongly here
    // would skip bookkeeping the slow path depends on.
    if (const u8 flags = fast_thunk_flags(rip, f); flags != 0xFFu) {
        return (flags & kFastThunkLeanHot) != 0;
    }
    return rip != 0 &&
        (rip == f.heap_alloc || rip == f.heap_free ||
         rip == f.wait_for_single_object || rip == f.release_semaphore ||
         rip == f.get_overlapped_result ||
         rip == f.sleep_condition_variable_cs ||
         rip == f.sleep || rip == f.sleep_ex ||
         rip == f.enter_critical_section || rip == f.leave_critical_section ||
         rip == f.try_enter_critical_section ||
         rip == f.acquire_srw_exclusive || rip == f.release_srw_exclusive ||
         rip == f.try_acquire_srw_exclusive ||
         rip == f.acquire_srw_shared || rip == f.release_srw_shared ||
         rip == f.try_acquire_srw_shared ||
         rip == f.fgetc || rip == f.ungetc);
}

// The hostcall ring is a diagnostic facility, not part of a Windows-visible
// API contract.  Capturing names, caller modules, registers and stack args on
// every uncontended lock/allocator call can cost far more than the operation
// itself. Keep detailed capture available whenever a related diagnostic was
// explicitly requested.
bool fast_thunk_has_body(GuestAddr rip, const FastThunkAddrs& f) noexcept {
    const u8 flags = fast_thunk_flags(rip, f);
    return flags != 0xFFu && (flags & kFastThunkHasBody) != 0;
}

bool hot_hostcall_diagnostics_enabled() {
    static const bool enabled =
        std::getenv("VITREO_HOSTCALL_HOT") != nullptr ||
        std::getenv("VITREO_HOSTCALL_DETAIL") != nullptr ||
        std::getenv("VITREO_TRACE_BAD_CALLER") != nullptr ||
        std::getenv("VITREO_TRACE_MODULE") != nullptr ||
        std::getenv("VITREO_TRACE_DURING_DLLMAIN") != nullptr;
    return enabled;
}

// Hostcall history is post-mortem diagnostics, not guest-visible state.  The
// old default captured and formatted every generic thunk call even when no
// hostcall diagnostic had been requested.  That made APIs such as waits,
// semaphores and heap operations spend more time in snprintf/module lookup
// than in their Windows contract.  Keep the facility opt-in and make every
// diagnostic which consumes HostcallEntry explicitly enable it.

namespace {

// Match SyncManager::sleep (sync_sleep.cpp): Sleep(0) yields; otherwise sleep
// to an absolute deadline, with a high-resolution tight close when
// timeBeginPeriod is active.
// Threads currently inside a fast-path Sleep. The Intel/native32 probe in
// sync.cpp already reports this as `concurrent`/`peak`; ARM takes the fast
// thunk instead, so without the same counters here the two platforms cannot be
// compared on the one number that distinguishes "the guest is waiting" from
// "the guest is waiting SINGLE-FILE" -- i.e. hurry-up-and-wait.
std::atomic<int> g_fast_in_sleep{0};
std::atomic<int> g_fast_max_in_sleep{0};

void fast_sleep_ms(u32 milliseconds) {
    // A blocking fast thunk must be visible as a host call, or a concurrent
    // SuspendThread waits forever for this thread to reach a checkpoint it
    // cannot reach while asleep. See ThreadSuspendRegistry::HostCallScope.
    winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
    if (milliseconds == 0) {
        winapi::host_yield_timeslice();
        return;
    }
    // Learn: INFINITE does not time out. Do not chrono-cast 0xFFFFFFFF ms.
    constexpr u32 kInfinite = 0xFFFFFFFFu;
    if (milliseconds == kInfinite) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    }
    const int depth = g_fast_in_sleep.fetch_add(1) + 1;
    int prev = g_fast_max_in_sleep.load();
    while (depth > prev &&
           !g_fast_max_in_sleep.compare_exchange_weak(prev, depth)) {}
    struct DepthGuard {
        ~DepthGuard() { g_fast_in_sleep.fetch_sub(1); }
    } depth_guard;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    auto sleep_until_deadline = [&](std::chrono::steady_clock::time_point until) {
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_until(until);
        }
    };
    if (!winapi::timer_resolution_is_high()) {
        sleep_until_deadline(deadline);
        return;
    }
    constexpr auto YIELD_MARGIN = std::chrono::microseconds(500);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return;
        }
        if (deadline - now > YIELD_MARGIN) {
            sleep_until_deadline(deadline - YIELD_MARGIN);
            continue;
        }
        winapi::host_yield_timeslice();
    }
}

void record_fast_sleep(u32 milliseconds, u64 actual_ns, u32 ret_addr) {
    // ARM routes Sleep through this fast path, so the sleep accounting in
    // impl_Sleep never sees it -- which is why VITREO_PROFILE_WAITS reported
    // "Sleep: count=0" on ARM while the guest was plainly sleeping. Aggregate
    // it here: total sleep wall and the requested durations, which is what
    // says whether a ~14ms pipeline stage is the guest asking to wait.
    // Which guest loop is doing the sleeping. ARM's sleeps land here, not in
    // impl_Sleep, so the VITREO_SLEEP_CALLERS histogram has to be duplicated on
    // this side or it reports nothing at all on ARM.
    static const bool sleep_callers = std::getenv("VITREO_SLEEP_CALLERS") != nullptr;
    if (sleep_callers && ret_addr != 0) {
        static std::mutex cmu;
        static std::unordered_map<u32, std::pair<u64, u64>> by_ret;
        static u64 cn = 0;
        std::lock_guard<std::mutex> lk(cmu);
        auto& e = by_ret[ret_addr];
        ++e.first;
        e.second += milliseconds;
        if ((++cn % 2000) == 0) {
            std::vector<std::pair<u32, std::pair<u64, u64>>> rows(by_ret.begin(),
                                                                  by_ret.end());
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) {
                          return a.second.second > b.second.second;
                      });
            std::fprintf(stderr, "[sleep-callers] after %llu sleeps:\n",
                         (unsigned long long)cn);
            for (usize i = 0; i < rows.size() && i < 8; ++i) {
                std::fprintf(stderr,
                    "[sleep-callers]   ret=0x%08x n=%-7llu total=%7.2fs avg=%.1fms\n",
                    rows[i].first, (unsigned long long)rows[i].second.first,
                    rows[i].second.second / 1000.0,
                    static_cast<double>(rows[i].second.second) /
                        static_cast<double>(rows[i].second.first));
            }
        }
    }
    static const bool sleep_stats = std::getenv("VITREO_SLEEP_STATS") != nullptr;
    if (sleep_stats) {
        static std::mutex mu;
        static u64 n = 0, total_ns = 0;
        static std::unordered_map<u32, u64> requested;
        static std::chrono::steady_clock::time_point first{};
        std::lock_guard<std::mutex> lk(mu);
        if (n == 0) first = std::chrono::steady_clock::now();
        ++n;
        total_ns += actual_ns;
        ++requested[milliseconds];
        if ((n % 1000) == 0) {
            const double wall =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - first).count() / 1000.0;
            std::vector<std::pair<u32, u64>> rows(requested.begin(),
                                                  requested.end());
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            // `concurrent` is total sleep divided by wall: 1.0 means exactly
            // one thread was ever asleep at a time, i.e. the guest's tick loop
            // is running single-file.
            std::fprintf(stderr,
                         "[sleep-stats] n=%llu total=%.2fs wall=%.2fs "
                         "concurrent=%.2f peak=%d avg=%.3fms  top:",
                         (unsigned long long)n,
                         static_cast<double>(total_ns) / 1e9, wall,
                         wall > 0 ? static_cast<double>(total_ns) / 1e9 / wall
                                  : 0.0,
                         g_fast_max_in_sleep.load(),
                         static_cast<double>(total_ns) / 1e6 /
                             static_cast<double>(n));
            for (usize i = 0; i < rows.size() && i < 5; ++i) {
                std::fprintf(stderr, " %ums x%llu", rows[i].first,
                             (unsigned long long)rows[i].second);
            }
            std::fprintf(stderr, "\n");
        }
    }
    diagnostics::phase_add_sleep_wall(actual_ns);
    if (diagnostics::perf_window_active()) {
        diagnostics::perf_add_wait(diagnostics::PerfWaitKind::Sleep, actual_ns);
    }
    if (perf::enabled()) {
        perf::count(perf::Counter::sleep_calls);
        if (milliseconds == 0) perf::count(perf::Counter::sleep_zero);
        const u64 requested_ns = static_cast<u64>(milliseconds) * 1'000'000ULL;
        perf::record(perf::Hist::sleep_overshoot,
                     actual_ns > requested_ns ? actual_ns - requested_ns : 0);
    }
}

void write_last_error_teb(cpu::CpuState& cpu, GuestMemory& memory, u32 err) {
    winapi::set_last_error(err);
    const bool is64 = cpu.is_64bit();
    const GuestAddr teb = is64 ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
    const GuestAddr off = is64 ? GuestAddr{0x68} : GuestAddr{0x34};
    (void)memory.write_u32(teb + off, err);
}

[[nodiscard]] GuestAddr arg_ptr(cpu::CpuState& cpu, GuestMemory& memory,
                                u32 index) {
    if (cpu.is_64bit()) {
        switch (index) {
        case 0: return static_cast<GuestAddr>(cpu.gpr.rcx);
        case 1: return static_cast<GuestAddr>(cpu.gpr.rdx);
        case 2: return static_cast<GuestAddr>(cpu.gpr.r8);
        default: return 0;
        }
    }
    return static_cast<GuestAddr>(
        memory.read_u32(cpu.sp() + 4 + static_cast<u64>(index) * 4)
            .value_or(0));
}

[[nodiscard]] u32 arg_u32(cpu::CpuState& cpu, GuestMemory& memory, u32 index) {
    if (cpu.is_64bit()) {
        switch (index) {
        case 0: return static_cast<u32>(cpu.gpr.rcx);
        case 1: return static_cast<u32>(cpu.gpr.rdx);
        case 2: return static_cast<u32>(cpu.gpr.r8);
        default: return 0;
        }
    }
    return memory.read_u32(cpu.sp() + 4 + static_cast<u64>(index) * 4)
        .value_or(0);
}

[[nodiscard]] u64 arg_u64(cpu::CpuState& cpu, GuestMemory& memory, u32 index) {
    if (cpu.is_64bit()) {
        switch (index) {
        case 0: return cpu.gpr.rcx;
        case 1: return cpu.gpr.rdx;
        case 2: return cpu.gpr.r8;
        default: return 0;
        }
    }
    return static_cast<u64>(
        memory.read_u32(cpu.sp() + 4 + static_cast<u64>(index) * 4)
            .value_or(0));
}

// TEB TLS layout — same offsets as tls.cpp (WIN32 TEB contract).
constexpr u32 kTlsTebSlots = 64;
constexpr u32 kTebTlsSlots32 = 0x0E10;
constexpr u32 kTebTlsSlots64 = 0x1480;
std::mutex g_fast_interlocked_mutex;

[[nodiscard]] GuestAddr current_teb_base(const cpu::CpuState& cpu) {
    return cpu.is_64bit()
        ? static_cast<GuestAddr>(cpu.seg.gs_desc.base)
        : static_cast<GuestAddr>(cpu.seg.fs_desc.base);
}

[[nodiscard]] u64 teb_ptr_read(GuestMemory& memory, GuestAddr slot, bool is64) {
    if (is64) return memory.read_u64(slot).value_or(0);
    return static_cast<u64>(memory.read_u32(slot).value_or(0));
}

void teb_ptr_write(GuestMemory& memory, GuestAddr slot, u64 value, bool is64) {
    if (is64) (void)memory.write_u64(slot, value);
    else (void)memory.write_u32(slot, static_cast<u32>(value));
}

// The one implementation the TlsGetValue/TlsSetValue thunks also use
// (winapi/kernel32/thread/tls.hpp); two copies of the lazy allocation raced.
[[nodiscard]] GuestAddr read_tls_expansion(GuestMemory& memory, GuestAddr teb,
                                           bool is64) {
    return winapi::tls_expansion_slots(memory, teb, is64);
}

[[nodiscard]] GuestAddr ensure_tls_expansion(GuestMemory& memory, GuestAddr teb,
                                             bool is64) {
    return winapi::ensure_tls_expansion_slots(memory, teb, is64);
}

[[nodiscard]] GuestAddr tls_slot_addr(GuestMemory& memory, GuestAddr teb,
                                      bool is64, u32 index, bool allocate) {
    if (teb == 0) return 0;
    const u32 pointer_size = is64 ? 8u : 4u;
    if (index < kTlsTebSlots) {
        return teb + (is64 ? kTebTlsSlots64 : kTebTlsSlots32) +
               static_cast<GuestAddr>(index) * pointer_size;
    }
    GuestAddr expansion = read_tls_expansion(memory, teb, is64);
    if (expansion == 0) {
        if (!allocate) return 0;
        expansion = ensure_tls_expansion(memory, teb, is64);
        if (expansion == 0) return 0;
    }
    return expansion +
           static_cast<GuestAddr>(index - kTlsTebSlots) * pointer_size;
}

void set_eax(cpu::CpuState& cpu, u32 value) {
    // In 64-bit mode, writes to a 32-bit GPR zero the upper half. Fast
    // thunks model an API's EAX return, so preserving the thunk gateway's
    // upper bits can turn a legitimate zero result into a non-null pointer.
    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RAX), value);
}

[[nodiscard]] u32 interlocked_inc(GuestMemory& memory, GuestAddr target) {
    if (auto* hp = memory.host_ptr_mut(target, 4)) {
        auto* p = reinterpret_cast<u32*>(hp);
        return __atomic_add_fetch(p, 1u, __ATOMIC_SEQ_CST);
    }
    std::lock_guard<std::mutex> lock(g_fast_interlocked_mutex);
    const u32 new_value = memory.read_u32(target).value_or(0) + 1;
    (void)memory.write_u32(target, new_value);
    return new_value;
}

[[nodiscard]] u32 interlocked_dec(GuestMemory& memory, GuestAddr target) {
    if (auto* hp = memory.host_ptr_mut(target, 4)) {
        auto* p = reinterpret_cast<u32*>(hp);
        return __atomic_sub_fetch(p, 1u, __ATOMIC_SEQ_CST);
    }
    std::lock_guard<std::mutex> lock(g_fast_interlocked_mutex);
    const u32 new_value = memory.read_u32(target).value_or(0) - 1;
    (void)memory.write_u32(target, new_value);
    return new_value;
}

[[nodiscard]] u32 interlocked_cmpxchg(GuestMemory& memory, GuestAddr dest,
                                      u32 exchange, u32 comperand) {
    if (auto* hp = memory.host_ptr_mut(dest, 4)) {
        auto* p = reinterpret_cast<u32*>(hp);
        u32 expected = comperand;
        (void)__atomic_compare_exchange_n(
            p, &expected, exchange,
            /*weak=*/false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return expected;
    }
    std::lock_guard<std::mutex> lock(g_fast_interlocked_mutex);
    const u32 old_value = memory.read_u32(dest).value_or(0);
    if (old_value == comperand) {
        (void)memory.write_u32(dest, exchange);
    }
    return old_value;
}

}  // namespace

// Return true if the current cpu.rip matched one of the hot thunks and
// the call was handled in-line. Caller should still perform the standard
// thunk RET emulation (pop return address + stdcall_bytes on x86, pop
// 8 bytes on x64) on true.
//
// Handles both 32-bit and 64-bit guests. On x64, TEB is at gs_base and
// LastErrorValue lives at TEB+0x68; SetLastError takes its arg in RCX.
// A host thunk replaces a real x86 callee, but otherwise would never write a
// guest callee frame below ESP. That difference is observable when later code
// reuses the dead stack area: native Windows callees leave ordinary local-stack
// writes there, while a host-only implementation leaves older guest data
// intact indefinitely. Default-wipe 0x200 bytes to approximate those callee
// writes. VITREO_THUNK_STACK_CLOBBER=0 disables; any other size overrides.
void maybe_clobber_x86_thunk_callee_stack(
    cpu::CpuState& cpu,
    GuestMemory& memory) {
    if (cpu.is_64bit()) {
        return;
    }
    static const u32 clobber_bytes = []() -> u32 {
        const char* s = std::getenv("VITREO_THUNK_STACK_CLOBBER");
        if (s == nullptr || s[0] == '\0') {
            return 0x200;
        }
        const unsigned long v = std::strtoul(s, nullptr, 0);
        if (v == 0) {
            return 0;
        }
        if (v > 0x10000ul) {
            return 0x200;
        }
        return static_cast<u32>(v & ~3ul);
    }();
    if (clobber_bytes == 0) {
        return;
    }
    const u64 sp = cpu.sp();
    if (sp < clobber_bytes) {
        return;
    }
    const GuestAddr base = static_cast<GuestAddr>(sp - clobber_bytes);
    if (Byte* host = memory.host_ptr_mut(base, clobber_bytes)) {
        std::memset(host, 0, clobber_bytes);
        return;
    }
    for (u32 off = 0; off < clobber_bytes; off += 4) {
        (void)memory.write_u32(base + off, 0);
    }
}

[[nodiscard]] bool maybe_fast_thunk_call_inner(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    const FastThunkAddrs& f) {
    const GuestAddr rip = cpu.rip;
    const bool is64 = cpu.is_64bit();
    // Reject non-fast-thunks in O(1). Without this every dispatch bound for the
    // registered-handler path walked all 30 compares below first.
    if (const u8 flags = fast_thunk_flags(rip, f);
        flags != 0xFFu && (flags & kFastThunkHasBody) == 0) {
        return false;
    }
    // Clock reads first: they are by far the hottest of the fast thunks, and
    // the comparison chain is walked on every thunk dispatch.
    if (rip != 0 && rip == f.get_tick_count) {
        // DWORD GetTickCount(void) - low 32 bits of ms since boot.
        perf::count(perf::Counter::k32_get_tick_count);
        const u32 ticks = static_cast<u32>(winapi::tick_count_ms() & 0xFFFFFFFFULL);
        set_eax(cpu, ticks);
        return true;
    }
    if (rip != 0 && rip == f.time_get_time) {
        // DWORD timeGetTime(void). Shares the boot clock with GetTickCount;
        // winmm's own implementation reads the same source.
        perf::count(perf::Counter::mm_time_get_time);
        const u32 ticks = winapi::time_get_time();
        set_eax(cpu, ticks);
        return true;
    }
    if (rip != 0 && rip == f.get_tick_count64) {
        // ULONGLONG GetTickCount64(void) - full 64-bit result in RAX.
        const u64 ticks = winapi::tick_count_ms();
        if (is64) {
            cpu.gpr.rax = ticks;
        } else {
            // x86 returns a 64-bit value in EDX:EAX.
            cpu.gpr.rax = (cpu.gpr.rax & 0xFFFFFFFF00000000ULL) | (ticks & 0xFFFFFFFFULL);
            cpu.gpr.rdx = (cpu.gpr.rdx & 0xFFFFFFFF00000000ULL) | (ticks >> 32);
        }
        return true;
    }
    if (rip != 0 && rip == f.query_performance_counter) {
        // BOOL QueryPerformanceCounter(LARGE_INTEGER* count). Same 10 MHz
        // QPC domain as the registered handler.
        const GuestAddr counter = arg_ptr(cpu, memory, 0);
        perf::count(perf::Counter::k32_query_perf_counter);
        const u64 qpc_now = virtual_qpc_ticks();
        (void)memory.write_u64(counter, qpc_now);
        time_diag_observe(TimeApi::Qpc, qpc_now);
        set_eax(cpu, 1u);
        return true;
    }
    if (rip != 0 && rip == f.query_performance_frequency) {
        // BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq).
        const GuestAddr freq_ptr = arg_ptr(cpu, memory, 0);
        (void)memory.write_u64(freq_ptr, kQpcHz);
        set_eax(cpu, 1u);
        return true;
    }
    if (rip == f.get_last_error) {
        // DWORD GetLastError(void)
        // TEB.LastErrorValue is the source of truth for both bitnesses
        // (x86 fs:[0x34], x64 gs:[0x68]); the slow thunk dispatch syncs the
        // host copy into it after every handler, and guest code may write it
        // directly.
        u32 le = 0;
        {
            const GuestAddr teb =
                is64 ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
            auto r = memory.read_u32(teb + (is64 ? 0x68 : 0x34));
            if (r.is_ok()) le = r.value();
            winapi::set_last_error(le);
        }
        set_eax(cpu, le);
        return true;
    }
    if (rip == f.set_last_error) {
        // VOID SetLastError(DWORD dwErrCode)
        // 32-bit stdcall: arg at [esp+4]. 64-bit MS ABI: arg in ECX.
        const u32 err = arg_u32(cpu, memory, 0);
        write_last_error_teb(cpu, memory, err);
        return true;
    }
    if (rip == f.get_current_thread_id) {
        // DWORD GetCurrentThreadId(void)
        const u32 tid = winapi::ThreadManager::get_current_thread_id_static();
        set_eax(cpu, tid);
        return true;
    }
    if (rip != 0 && rip == f.get_current_process_id) {
        // DWORD GetCurrentProcessId(void) — host PID (ProcessManager contract).
        set_eax(cpu, static_cast<u32>(::getpid()));
        return true;
    }
    if (rip != 0 && rip == f.is_debugger_present) {
        // BOOL IsDebuggerPresent(void) — always false.
        set_eax(cpu, 0);
        return true;
    }
    if (rip != 0 && (rip == f.fgetc || rip == f.ungetc)) {
        const auto* handler = rip == f.fgetc ? f.fgetc_handler : f.ungetc_handler;
        if (handler == nullptr) return false;
        // Preserve the DLL's arena, buffer layout and failure policy while
        // avoiding the execution loop's generic bookkeeping transition.
        winapi::Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        (*handler)(marshaler);
        return true;
    }
    if (rip != 0 && rip == f.sleep) {
        // VOID Sleep(DWORD dwMilliseconds)
        const u32 ms = arg_u32(cpu, memory, 0);
        const auto t0 = std::chrono::steady_clock::now();
        fast_sleep_ms(ms);
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        record_fast_sleep(ms, ns,
                          cpu.is_64bit()
                              ? 0u
                              : memory.read_u32(static_cast<GuestAddr>(
                                    cpu.gpr.rsp)).value_or(0u));
        return true;
    }
    if (rip != 0 && rip == f.sleep_ex) {
        // DWORD SleepEx(DWORD dwMilliseconds, BOOL bAlertable)
        // Alertable waits need APC drain — leave those on the slow path.
        const u32 ms = arg_u32(cpu, memory, 0);
        const u32 alertable = arg_u32(cpu, memory, 1);
        if (alertable != 0) {
            return false;
        }
        const auto t0 = std::chrono::steady_clock::now();
        fast_sleep_ms(ms);
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        record_fast_sleep(ms, ns,
                          cpu.is_64bit()
                              ? 0u
                              : memory.read_u32(static_cast<GuestAddr>(
                                    cpu.gpr.rsp)).value_or(0u));
        set_eax(cpu, 0);
        return true;
    }
    // SRW acquire/release/try, exclusive and shared. Mirrors the registered
    // handlers exactly, including the guest lock-word write real Windows makes
    // observable (guest code reads the SRWLOCK value inline) and the BOOLEAN
    // result of the Try forms. The shared forms map onto the exclusive
    // implementation the same way the registered handlers do -- exclusive is
    if (rip != 0 &&
        (rip == f.acquire_srw_exclusive || rip == f.release_srw_exclusive ||
         rip == f.try_acquire_srw_exclusive ||
         rip == f.acquire_srw_shared || rip == f.release_srw_shared ||
         rip == f.try_acquire_srw_shared)) {
        auto* sync_mgr = g_fast_thunk_sync_mgr;
        if (sync_mgr == nullptr) return false;
        const GuestAddr lock = arg_ptr(cpu, memory, 0);
        if (lock == 0) return false;
        // The guest-word protocol first: every SRW entry point (registered
        // handler, this inline path, SleepConditionVariableSRW) must agree
        // on where the lock state lives, or a sleep sees a lock it does not
        // own and the guest spins on ERROR_INVALID_PARAMETER.
        if (winapi::srw_word::Word w;
            winapi::srw_word::resolve(memory, lock, is64, w)) {
            const u32 tid = winapi::ThreadManager::get_current_thread_id_static();
            if (rip == f.acquire_srw_exclusive) {
                winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
                winapi::srw_word::acquire_exclusive(w, tid, sync_mgr->shutdown_flag());
                return true;
            }
            if (rip == f.acquire_srw_shared) {
                winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
                winapi::srw_word::acquire_shared(w, tid, sync_mgr->shutdown_flag());
                return true;
            }
            if (rip == f.release_srw_exclusive) {
                winapi::srw_word::release_exclusive(w, tid);
                return true;
            }
            if (rip == f.release_srw_shared) {
                (void)winapi::srw_word::release_shared(w, tid);
                return true;
            }
            const bool ok = (rip == f.try_acquire_srw_shared)
                                ? winapi::srw_word::try_acquire_shared(w, tid)
                                : winapi::srw_word::try_acquire_exclusive(w, tid);
            set_eax(cpu, ok ? 1u : 0u);
            return true;
        }
        // Shared really is shared here. Routing the shared forms to the
        // exclusive ones is sound but serialises all readers -- see the
        // SrwLock comment in sync.hpp for what that cost on a live UI.
        if (rip == f.acquire_srw_exclusive) {
            winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
            sync_mgr->acquire_srw_lock_exclusive(lock);
            (void)memory.write_u32(lock, 1);
            return true;
        }
        if (rip == f.acquire_srw_shared) {
            winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
            sync_mgr->acquire_srw_lock_shared(lock);
            (void)memory.write_u32(lock, 1);
            return true;
        }
        if (rip == f.release_srw_exclusive) {
            sync_mgr->release_srw_lock_exclusive(lock);
            (void)memory.write_u32(lock, 0);
            return true;
        }
        if (rip == f.release_srw_shared) {
            // Clear the guest word only when the last reader leaves.
            if (sync_mgr->release_srw_lock_shared(lock)) {
                (void)memory.write_u32(lock, 0);
            }
            return true;
        }
        const bool ok = (rip == f.try_acquire_srw_shared)
                            ? sync_mgr->try_acquire_srw_lock_shared(lock)
                            : sync_mgr->try_acquire_srw_lock_exclusive(lock);
        if (ok) (void)memory.write_u32(lock, 1);
        set_eax(cpu, ok ? 1u : 0u);
        return true;
    }

    if (rip != 0 && rip == f.sleep_condition_variable_cs) {
        auto* cs_mgr = g_fast_thunk_cs_mgr;
        auto* sync_mgr = g_fast_thunk_sync_mgr;
        if (cs_mgr == nullptr || sync_mgr == nullptr) {
            return false;
        }
        cs_mgr->note_bitness(is64);
        const GuestAddr cv_addr = arg_ptr(cpu, memory, 0);
        const GuestAddr cs_addr = arg_ptr(cpu, memory, 1);
        const u32 milliseconds = arg_u32(cpu, memory, 2);
        const bool ok = [&] {
            winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
            // Same guest-word CV as the registered handler and the wakers.
            if (winapi::srw_word::Word cvw;
                cs_addr != 0 &&
                winapi::srw_word::resolve(memory, cv_addr, is64, cvw)) {
                const u32 tid = winapi::ThreadManager::get_current_thread_id_static();
                if (cs_mgr->owning_thread(cs_addr) != tid ||
                    cs_mgr->recursion_count(cs_addr) != 1) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_PARAMETER);
                    return false;
                }
                const bool woken = winapi::srw_word::cv_sleep_with(
                    cvw, milliseconds, sync_mgr->shutdown_flag(),
                    [&] { cs_mgr->leave(cs_addr); }, [&] { cs_mgr->enter(cs_addr); });
                if (!woken) winapi::set_last_error(winapi::error_codes::ERROR_TIMEOUT);
                return woken;
            }
            return sync_mgr->sleep_condition_variable_cs(
                *cs_mgr, cv_addr, cs_addr, milliseconds);
        }();
        if (!ok) {
            write_last_error_teb(cpu, memory, winapi::get_last_error());
        }
        set_eax(cpu, ok ? 1u : 0u);
        return true;
    }
    if (rip != 0 && rip == f.tls_get_value) {
        // LPVOID TlsGetValue(DWORD dwTlsIndex) — TEB slots (tls.cpp contract).
        const u32 index = arg_u32(cpu, memory, 0);
        if (index >= winapi::TLS_TOTAL_SLOTS) {
            write_last_error_teb(cpu, memory,
                                 winapi::error_codes::ERROR_INVALID_PARAMETER);
            set_eax(cpu, 0);
            return true;
        }
        write_last_error_teb(cpu, memory, winapi::error_codes::ERROR_SUCCESS);
        const GuestAddr teb = current_teb_base(cpu);
        if (GuestAddr slot =
                tls_slot_addr(memory, teb, is64, index, /*allocate=*/false)) {
            const u64 value = teb_ptr_read(memory, slot, is64);
            if (is64) {
                cpu.gpr.rax = value;
            } else {
                set_eax(cpu, static_cast<u32>(value));
            }
        } else {
            set_eax(cpu, 0);
        }
        return true;
    }
    if (rip != 0 && rip == f.tls_set_value) {
        // BOOL TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue)
        const u32 index = arg_u32(cpu, memory, 0);
        const u64 value = arg_u64(cpu, memory, 1);
        if (index >= winapi::TLS_TOTAL_SLOTS) {
            write_last_error_teb(cpu, memory,
                                 winapi::error_codes::ERROR_INVALID_PARAMETER);
            set_eax(cpu, 0);
            return true;
        }
        const GuestAddr teb = current_teb_base(cpu);
        GuestAddr slot =
            tls_slot_addr(memory, teb, is64, index, /*allocate=*/true);
        if (slot == 0) {
            write_last_error_teb(cpu, memory,
                                 winapi::error_codes::ERROR_NOT_ENOUGH_MEMORY);
            set_eax(cpu, 0);
            return true;
        }
        teb_ptr_write(memory, slot, value, is64);
        set_eax(cpu, 1u);
        return true;
    }
    if (rip != 0 && rip == f.interlocked_increment) {
        // LONG InterlockedIncrement(LONG volatile *Addend)
        const GuestAddr target = arg_ptr(cpu, memory, 0);
        set_eax(cpu, interlocked_inc(memory, target));
        return true;
    }
    if (rip != 0 && rip == f.interlocked_decrement) {
        // LONG InterlockedDecrement(LONG volatile *Addend)
        const GuestAddr target = arg_ptr(cpu, memory, 0);
        set_eax(cpu, interlocked_dec(memory, target));
        return true;
    }
    if (rip != 0 && rip == f.interlocked_compare_exchange) {
        // LONG InterlockedCompareExchange(LONG volatile *Destination,
        //                                 LONG Exchange, LONG Comperand)
        const GuestAddr dest = arg_ptr(cpu, memory, 0);
        const u32 exchange = arg_u32(cpu, memory, 1);
        const u32 comperand = arg_u32(cpu, memory, 2);
        set_eax(cpu, interlocked_cmpxchg(memory, dest, exchange, comperand));
        return true;
    }
    // CRITICAL_SECTION enter/leave/try — same CriticalSectionManager contract
    // as create_critical_section_thunks (no last-error; null CS is a no-op /
    // FALSE). Requires set_fast_thunk_cs_manager; otherwise fall through.
    if (rip != 0 &&
        (rip == f.enter_critical_section || rip == f.leave_critical_section ||
         rip == f.try_enter_critical_section)) {
        auto* cs_mgr = g_fast_thunk_cs_mgr;
        if (cs_mgr == nullptr) {
            return false;
        }
        cs_mgr->note_bitness(is64);
        const GuestAddr cs_addr = arg_ptr(cpu, memory, 0);
        if (rip == f.enter_critical_section) {
            if (cs_addr) {
                // Contended enter blocks; stay visible to SuspendThread.
                winapi::ThreadSuspendRegistry::HostCallScope host_call_scope;
                cs_mgr->enter(cs_addr);
            }
            return true;
        }
        if (rip == f.leave_critical_section) {
            if (cs_addr) {
                cs_mgr->leave(cs_addr);
            }
            return true;
        }
        // TryEnterCriticalSection
        const bool ok = cs_addr ? cs_mgr->try_enter(cs_addr) : false;
        set_eax(cpu, ok ? 1u : 0u);
        return true;
    }
    return false;
}

// The fast path never reaches a registered handler, so it needs its own
// sample. Keyed by the dispatch address, which is what the handler wrapper
// uses too -- one row per export regardless of which route serviced the call.
[[nodiscard]] bool maybe_fast_thunk_call(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    const FastThunkAddrs& f) {
    // Recording the whole-program call stream requires every call to be
    // observable, and a fast thunk services the guest without ever reaching a
    // handler -- so it would silently vanish from the stream. An instrument
    // with blind spots is worse than none: the missing calls show up as
    // "divergences" that are really just holes. Correctness mode does not need
    // the speed, so the fast paths step aside while it is on.
    if (diagnostics::callstream_enabled() &&
        !diagnostics::callstream_keep_fastpath()) return false;
    if (!winapi::api_profile::enabled()) {
        return maybe_fast_thunk_call_inner(cpu, memory, f);
    }
    const GuestAddr key = static_cast<GuestAddr>(cpu.rip);
    const auto t0 = std::chrono::steady_clock::now();
    const bool hit = maybe_fast_thunk_call_inner(cpu, memory, f);
    if (hit) {
        winapi::api_profile::record(key, static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count()));
    }
    return hit;
}

[[nodiscard]] bool finish_thunk_ret(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    const winapi::ThunkRegistry& thunk_registry,
    GuestAddr thunk_addr) {
    // Same contract as the outer dispatch loop: x64 pops 8 bytes and the
    // caller cleans args; x86 stdcall pops 4 + resolve_stdcall_bytes.
    if (cpu.is_64bit()) {
        auto ret_addr_result = memory.read_u64(cpu.sp());
        if (ret_addr_result.is_err()) {
            return false;
        }
        cpu.rip = ret_addr_result.value();
        cpu.set_sp(cpu.sp() + 8);
        return true;
    }
    auto ret_addr_result = memory.read_u32(cpu.sp());
    if (ret_addr_result.is_err()) {
        return false;
    }
    cpu.rip = static_cast<u64>(ret_addr_result.value());
    const u16 cleanup = thunk_registry.resolve_stdcall_bytes(thunk_addr);
    cpu.set_sp(cpu.sp() + 4 + cleanup);
    return true;
}

}  // namespace vitreo::cli
