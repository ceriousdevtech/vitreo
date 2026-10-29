// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window_report_runtime.cpp
/// @brief Perf report sections for the OS boundary: synchronization, the
///        thunk boundary, filesystem, registry, graphics, guest threads and
///        per-thread instructions. Split out of perf_window.cpp as verbatim
///        code motion.

#include <vitreo/diagnostics.hpp>

#include "perf_window.hpp"
#include "perf_window_report_internal.hpp"

#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <cstdarg>
#include <string>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace vitreo::diagnostics::perf_window_detail {

void report_runtime(ReportBuf& out, State& st, u64 window_ns, u64 wait_ns,
                    u64 wait_calls, u64 sleep_ns, u64 fs_ns, u64 fs_calls,
                    u64 fs_bytes) noexcept {
    static const char* kWaitName[] = {
        "Sleep", "WaitForSingleObject", "WaitForMultipleObjects",
        "SignalObjectAndWait"};
    out("\nSynchronization:\n");
    out("  Total wait:            %.3fs  (%llu calls)\n",
                 static_cast<double>(wait_ns) / 1e9,
                 static_cast<unsigned long long>(wait_calls));
    for (u32 i = 0; i < kWaitKindCount; ++i) {
        const u64 c = st.wait[i].calls.load(std::memory_order_relaxed);
        const u64 ns = st.wait[i].ns.load(std::memory_order_relaxed);
        const u64 mx = st.wait[i].max_ns.load(std::memory_order_relaxed);
        out(
                     "  %-22s %llu  total=%.3fs  avg=%.1fus  max=%.1fus\n",
                     kWaitName[i], static_cast<unsigned long long>(c),
                     static_cast<double>(ns) / 1e9,
                     c ? (static_cast<double>(ns) / static_cast<double>(c) / 1e3)
                       : 0.0,
                     static_cast<double>(mx) / 1e3);
    }
    (void)sleep_ns;

    // Inclusive time per guest-to-host crossing. A multi-process browser
    // guest at idle exits translated code into an API thunk millions of
    // times, so this column decides whether the boundary or the emitted code
    // is the responsiveness problem. Crossing counts alone do not.
    static const char* kThunkName[] = {
        "GuardIcall (RIP:=RAX)", "Fast (inline)", "Slow (marshaled)",
        "Not served here", "Direct (COM entry)"};
    static_assert(sizeof(kThunkName) / sizeof(kThunkName[0]) ==
                  kThunkKindCount);
    u64 thunk_ns_total = 0;
    u64 thunk_calls_total = 0;
    for (const auto& t : st.thunk) {
        thunk_ns_total += t.ns.load(std::memory_order_relaxed);
        thunk_calls_total += t.calls.load(std::memory_order_relaxed);
    }
    out("\nThunk boundary (inclusive, per crossing):\n");
    out("  Total:                 %.3fs  (%llu crossings, %.1f%% of window)\n",
                 static_cast<double>(thunk_ns_total) / 1e9,
                 static_cast<unsigned long long>(thunk_calls_total),
                 window_ns ? 100.0 * static_cast<double>(thunk_ns_total) /
                                 static_cast<double>(window_ns)
                           : 0.0);
    for (u32 i = 0; i < kThunkKindCount; ++i) {
        const u64 c = st.thunk[i].calls.load(std::memory_order_relaxed);
        const u64 ns = st.thunk[i].ns.load(std::memory_order_relaxed);
        const u64 mx = st.thunk[i].max_ns.load(std::memory_order_relaxed);
        out("  %-22s %llu  total=%.3fs  avg=%.0fns  max=%.1fus\n",
                     kThunkName[i], static_cast<unsigned long long>(c),
                     static_cast<double>(ns) / 1e9,
                     c ? (static_cast<double>(ns) / static_cast<double>(c))
                       : 0.0,
                     static_cast<double>(mx) / 1e3);
    }
    out("  Note: wall time, so a thunk that blocks (a wait, real I/O) charges"
        " its block time here too; compare against the wait and filesystem"
        " sections before reading a large average as boundary overhead.\n");

    static const char* kFsName[] = {"CreateFile", "ReadFile", "WriteFile"};
    out("\nFilesystem:\n");
    out("  Total:                 %.3fs  (%llu calls, %llu bytes)\n",
                 static_cast<double>(fs_ns) / 1e9,
                 static_cast<unsigned long long>(fs_calls),
                 static_cast<unsigned long long>(fs_bytes));
    for (u32 i = 0; i < kFsKindCount; ++i) {
        const u64 c = st.fs[i].calls.load(std::memory_order_relaxed);
        const u64 ns = st.fs[i].ns.load(std::memory_order_relaxed);
        const u64 b = st.fs[i].bytes.load(std::memory_order_relaxed);
        out("  %-22s %llu  %.3fs  %llu bytes\n", kFsName[i],
                     static_cast<unsigned long long>(c),
                     static_cast<double>(ns) / 1e9,
                     static_cast<unsigned long long>(b));
    }

    out("\nRegistry:\n");
    out("  Calls:                 %llu\n",
                 static_cast<unsigned long long>(
                     st.registry_calls.load(std::memory_order_relaxed)));
    out("  Time:                  %.3fs\n",
                 static_cast<double>(
                     st.registry_ns.load(std::memory_order_relaxed)) /
                     1e9);

    out("\nGraphics:\n");
    out("  Presents:              %llu\n",
                 static_cast<unsigned long long>(
                     st.presents.load(std::memory_order_relaxed)));
    out("  Draws:                 %llu\n",
                 static_cast<unsigned long long>(
                     st.draws.load(std::memory_order_relaxed)));
    out("  Present wall time:     %.3fs\n",
                 static_cast<double>(
                     st.graphics_ns.load(std::memory_order_relaxed)) /
                     1e9);

    out("\nGuest threads (last snapshot / max):\n");
    out("  Active:                %u  (max %u)\n",
                 st.guest_threads.load(std::memory_order_relaxed),
                 st.guest_threads_max.load(std::memory_order_relaxed));
    out("  Runnable:              %u\n",
                 st.guest_runnable.load(std::memory_order_relaxed));
    out("  Suspended:             %u\n",
                 st.guest_suspended.load(std::memory_order_relaxed));
    out("  Host workers:          %u  (max %u)\n",
                 st.host_workers.load(std::memory_order_relaxed),
                 st.host_workers_max.load(std::memory_order_relaxed));

    {
        std::lock_guard<std::mutex> lock(st.thread_mu);
        if (!st.threads.empty()) {
            out("\nPer-thread instructions:\n");
            for (ThreadAcc* t : st.threads) {
                const u64 tj = t->jit.load(std::memory_order_relaxed);
                const u64 ti = t->interp.load(std::memory_order_relaxed);
                const u64 tf = t->fallback.load(std::memory_order_relaxed);
                const u64 tot = tj + ti + tf;
                if (tot == 0) continue;
                out(
                             "  T%u  total=%llu  jit=%.1f%%  interp=%.1f%%  "
                             "fallback=%.1f%%\n",
                             t->serial,
                             static_cast<unsigned long long>(tot),
                             100.0 * static_cast<double>(tj) /
                                 static_cast<double>(tot),
                             100.0 * static_cast<double>(ti) /
                                 static_cast<double>(tot),
                             100.0 * static_cast<double>(tf) /
                                 static_cast<double>(tot));
            }
        }
    }
}

}  // namespace vitreo::diagnostics::perf_window_detail
