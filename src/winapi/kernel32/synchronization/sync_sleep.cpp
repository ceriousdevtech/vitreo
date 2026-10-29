// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_sleep.cpp
/// @brief Process-handle waits and Sleep.
///
/// Closed cut from sync.cpp — verified with plan_extraction.py.

#include "sync.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"
#include "winapi/common/host_yield.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/sync/ntsync.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/named_sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Process handles (cross-process IPC, Stage 1)
// =============================================================================

Handle SyncManager::create_process_handle(int pid) {
    ipc::ProcessObject* obj = ipc::track_process(static_cast<pid_t>(pid));
    if (obj == nullptr) return NULL_HANDLE;
    return m_handles.allocate(HandleType::Process, obj, 0);
}

Handle SyncManager::create_completed_process_handle(u32 process_exit_code_value) {
    ipc::ProcessObject* obj =
        ipc::create_completed_process(process_exit_code_value);
    if (obj == nullptr) return NULL_HANDLE;
    return m_handles.allocate(HandleType::Process, obj, 0);
}

bool SyncManager::process_exit_code(Handle h, u32& code) const {
    if (m_handles.get_type(h) != HandleType::Process) return false;
    auto* po = static_cast<ipc::ProcessObject*>(
        m_handles.get(h, HandleType::Process));
    if (po == nullptr) return false;
    constexpr u32 STILL_ACTIVE = 259;
    code = po->exited.load(std::memory_order_acquire)
               ? po->exit_code.load(std::memory_order_acquire)
               : STILL_ACTIVE;
    return true;
}

bool SyncManager::process_pid(Handle h, u32& out_pid) const {
    if (m_handles.get_type(h) != HandleType::Process) return false;
    auto* po = static_cast<ipc::ProcessObject*>(
        m_handles.get(h, HandleType::Process));
    if (po == nullptr || po->pid <= 0) return false;
    out_pid = static_cast<u32>(po->pid);
    return true;
}

// =============================================================================
// Sleep
// =============================================================================

void SyncManager::sleep(u32 milliseconds) {
    // Sleep(0) yields the remainder of the timeslice without leaving the
    // runnable state or losing priority, which is what Windows documents and
    // what spin-then-yield loops depend on (host_yield.hpp).
    if (milliseconds == 0) {
        host_yield_timeslice();
        return;
    }

    // Learn: INFINITE means the suspension does not time out. A chrono
    // cast of 0xFFFFFFFF ms is ~49.7 days and returns; park until process
    // shutdown instead (thread_lifecycle / Sleep contract). Wake via the
    // waitable-timer CV that begin_shutdown already notifies.
    if (milliseconds == INFINITE_WAIT) {
        std::unique_lock lock(m_waitable_timer_mutex);
        m_waitable_timer_cv.wait(lock, [this] {
            return m_shutdown.load(std::memory_order_acquire) ||
                   m_waitable_timer_stop;
        });
        return;
    }

    // Compute the deadline once from a monotonic clock and sleep toward it.
    // Relative sleeping re-bases on each wakeup, so a caller that is preempted
    // between computing its delay and issuing the sleep silently sleeps long;
    // an absolute deadline cannot drift that way.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);

    // Learn Sleep: the thread stays unrunnable until the interval elapses.
    // Host nanosleep / pthread waits return on EINTR (SIGCHLD from
    // CreateProcess children). Windows has no such interrupt, so resume until
    // the deadline. A single sleep_until that returns early turns a 120s
    // 50ms-poll loop into an instant timeout.
    auto sleep_until_deadline = [&](std::chrono::steady_clock::time_point until) {
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_until(until);
        }
    };

    if (!timer_resolution_is_high()) {
        sleep_until_deadline(deadline);
        return;
    }

    // A caller holding timeBeginPeriod has asked for tighter timing than the
    // host scheduler grants by default. Sleep to just short of the deadline,
    // then close the last sub-millisecond by yielding rather than spinning hot:
    // this bounds the overshoot without burning a core, which matters because
    // the guest thread doing the sleeping is usually the one that also has to
    // render the next frame.
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
        host_yield_timeslice();
    }
}

} // namespace vitreo::winapi
