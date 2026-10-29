#include "winapi/common/object_access_constants.hpp"
// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_wait.cpp
/// @brief Wait Functions — delegates to ntsync::Manager.
///
/// Closed cut from sync.cpp — verified with plan_extraction.py.

#include <string>
#include <cctype>
#include "sync.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"
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
// Wait Functions  (delegates to ntsync::Manager for atomic resolution)
// =============================================================================

u32 SyncManager::wait_for_single_object(
    Handle h, u32 milliseconds, SyncTraceContext trace, u64* blocked_ns) {
    return wait_for_single_object_impl(
        h, milliseconds, trace, &m_shutdown, blocked_ns);
}

u32 SyncManager::signal_object_and_wait(
    Handle signal, Handle wait, u32 milliseconds, bool alertable) {
    if (signal == NULL_HANDLE || wait == NULL_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    // The signal is unconditional: an APC that ends the wait does not undo it
    // (Learn; handles_waits_probe soaw_apc_pending signals, then 0xC0).
    const HandleType signal_type = m_handles.get_type(signal);
    bool signaled = false;
    switch (signal_type) {
    case HandleType::Event:
        signaled = set_event(signal);
        break;
    case HandleType::Mutex:
        signaled = release_mutex(signal);
        break;
    case HandleType::Semaphore:
        signaled = release_semaphore(signal, 1, nullptr);
        break;
    default:
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if (!signaled) {
        // release_mutex / release_semaphore / set_event set GetLastError
        // (ERROR_NOT_OWNER / ERROR_TOO_MANY_POSTS / ERROR_INVALID_HANDLE).
        // Do not overwrite with a blanket INVALID_HANDLE.
        if (get_last_error() == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        }
        return WAIT_FAILED;
    }

    if (!alertable) {
        return wait_for_single_object(wait, milliseconds);
    }

    // Park on the object and this thread's APC event together: an APC
    // queued while parked ends the wait immediately, per the alertable
    // contract, with no re-check slice.
    const bool infinite = milliseconds == INFINITE_WAIT;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(infinite ? 0 : milliseconds);
    const u64 apc_ev = apc_wait_object_for_current_thread();
    for (bool first_probe = true;; first_probe = false) {
        if (try_drain_alertable_apcs()) {
            return WAIT_IO_COMPLETION;
        }
        u32 remaining = INFINITE_WAIT;
        if (!infinite) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                // A zero/elapsed timeout still tests the object once.
                if (!first_probe) return WAIT_TIMEOUT;
                remaining = 0;
            } else {
                remaining = static_cast<u32>(std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now).count());
            }
        }
        const u32 result = wait_for_multiple_objects(
            1, &wait, /*wait_all=*/false, remaining, apc_ev);
        if (result == WAIT_OBJECT_0 + 1) continue;   // APC event: drain above
        return result;
    }
}

bool sync_mechanism_off(const char* name) noexcept {
    std::string env = "VITREO_SYNC_OFF_";
    std::string file = "/tmp/vitreo-sync-off-";
    for (const char* c = name; *c != '\0'; ++c) {
        env += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        file += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    }
    if (std::getenv(env.c_str()) != nullptr) return true;
    return ::access(file.c_str(), F_OK) == 0;
}

void SyncManager::arm_gui_wake(u32 count, const Handle* handles) {
    std::vector<ntsync::ObjectId> ids;
    ids.reserve(count);
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        for (u32 i = 0; i < count; ++i) {
            const Handle h = handles[i];
            if (resolve_shared(h)) continue;
            u64 id = 0;
            if (m_handles.get_type(h) == HandleType::Thread) {
                auto* gt = m_handles.get_as<GuestThread>(h, HandleType::Thread);
                if (gt) id = gt->nt_id;
            } else if (m_handles.get_type(h) == HandleType::Process) {
                auto* po = static_cast<ipc::ProcessObject*>(
                    m_handles.get(h, HandleType::Process));
                if (po) id = po->exit_event;
            } else {
                id = resolve_nt_id(h);
            }
            if (id != 0) ids.push_back(id);
        }
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    ntsync::global().arm_host_wake(tid, ids.data(), static_cast<u32>(ids.size()));
    if (m_wait_park_arm) m_wait_park_arm(true);
}

void SyncManager::disarm_gui_wake() {
    ntsync::global().disarm_host_wake(
        ThreadManager::get_current_thread_id_static());
    if (m_wait_park_arm) m_wait_park_arm(false);
}

u64 SyncManager::apc_wait_object_for_current_thread() {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard<std::mutex> lk(m_apc_events_mutex);
    auto it = m_apc_events.find(tid);
    if (it != m_apc_events.end()) return it->second;
    const u64 id = ntsync::global().create_event(/*manual_reset=*/false,
                                                 /*initial_state=*/false);
    m_apc_events.emplace(tid, id);
    return id;
}

u64 SyncManager::alert_wait_object_for_thread(u32 thread_id) {
    std::lock_guard<std::mutex> lk(m_apc_events_mutex);
    auto it = m_alert_events.find(thread_id);
    if (it != m_alert_events.end()) return it->second;
    const u64 id = ntsync::global().create_event(/*manual_reset=*/false,
                                                 /*initial_state=*/false);
    m_alert_events.emplace(thread_id, id);
    return id;
}

void SyncManager::discard_alert_wait_object(u32 thread_id) {
    u64 id = 0;
    {
        std::lock_guard<std::mutex> lk(m_apc_events_mutex);
        auto it = m_alert_events.find(thread_id);
        if (it == m_alert_events.end()) return;
        id = it->second;
        m_alert_events.erase(it);
    }
    ntsync::global().destroy(id);
}

void SyncManager::signal_apc_wait_object(u32 thread_id) {
    u64 id = 0;
    {
        std::lock_guard<std::mutex> lk(m_apc_events_mutex);
        auto it = m_apc_events.find(thread_id);
        if (it == m_apc_events.end()) return;
        id = it->second;
    }
    (void)ntsync::global().set_event(id);
}

u32 SyncManager::wait_for_single_object_cancellable(
    Handle h, u32 milliseconds,
    const std::atomic<bool>& cancel_requested,
    SyncTraceContext trace) {
    return wait_for_single_object_impl(
        h, milliseconds, trace, &cancel_requested, nullptr);
}

void SyncManager::log_wait_kind(Handle h, u32 milliseconds) {
    static const bool s_log_kinds =
        std::getenv("VITREO_LOG_WAIT_KINDS") != nullptr;
    if (!s_log_kinds) return;
    static std::mutex kind_mu;
    static std::set<Handle> kind_seen;
    {
        std::lock_guard<std::mutex> lk(kind_mu);
        if (!kind_seen.insert(h).second) return;
    }
    std::string kname;
    const char* kkind = "unknown";
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        if (auto it = m_events.find(h); it != m_events.end()) {
            kname = it->second->name;
            kkind = "event";
        } else if (auto mi = m_mutexes.find(h); mi != m_mutexes.end()) {
            kname = mi->second->name; kkind = "mutex";
        } else if (auto si = m_semaphores.find(h); si != m_semaphores.end()) {
            kname = si->second->name; kkind = "semaphore";
        } else if (m_handles.get_type(h) == HandleType::Thread) {
            kkind = "thread";
        } else if (m_handles.get_type(h) == HandleType::Process) {
            kkind = "process";
        }
    }
    std::fprintf(stderr,
                 "[wait-kind] h=0x%04x kind=%s type=%u name='%s' timeout=%s\n",
                 static_cast<unsigned>(h), kkind,
                 static_cast<unsigned>(m_handles.get_type(h)), kname.c_str(),
                 milliseconds == INFINITE_WAIT ? "INFINITE" : "timed");
}

u32 SyncManager::wait_for_single_object_impl(
    Handle h, u32 milliseconds, SyncTraceContext trace,
    const std::atomic<bool>* cancel_requested, u64* blocked_ns) {
    ThreadManager::refresh_current_thread_scheduling();
    if (m_shutdown.load(std::memory_order_acquire) ||
        (cancel_requested != nullptr &&
         cancel_requested->load(std::memory_order_acquire))) {
        return WAIT_FAILED;
    }
    if (h == NULL_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if ((m_handles.get_type(h) == HandleType::Event || m_handles.get_type(h) == HandleType::Mutex) &&
        !(m_handles.get_access(h) & sync_access::SYNCHRONIZE)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return WAIT_FAILED;
    }
    trace_event_wait(h, milliseconds, trace);

    // -------------------------------------------------------------------------
    // Pseudohandles: GetCurrentThread() = -2 (0xFFFFFFFE).  A thread can never
    // wait on itself.
    // -------------------------------------------------------------------------
    if (h == 0xFFFFFFFE || h == 0xFFFFFFFF) {
        if (milliseconds == 0) return WAIT_TIMEOUT;
        if (milliseconds != INFINITE_WAIT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            return WAIT_TIMEOUT;
        }
        // Learn: INFINITE waits until the object is signaled. Current-process /
        // current-thread pseudo handles are not signaled while this guest is
        // alive, so park until shutdown instead of inventing WAIT_TIMEOUT.
        while (!m_shutdown.load(std::memory_order_acquire) &&
               (cancel_requested == nullptr ||
                !cancel_requested->load(std::memory_order_acquire))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return WAIT_FAILED;
    }

    // VITREO_LOG_WAITS=1 announces unbounded waits before they park. A thread
    // blocked forever never reaches a completion log, so entry logging is the
    // only way to see which object a parked guest thread is waiting on.
    //
    // Only the FIRST unbounded wait per (thread, handle) is reported. Most
    // INFINITE waits are not parks at all — they are poll loops whose object is
    // already signalled, and logging each one buried the interesting entries
    // under the full instruction trace.
    // What KIND of object is behind a handle, reported once per handle for any
    // wait -- timed or not. The INFINITE-only path below cannot answer this for
    // a handle that is waited on with a timeout, such as an object that paces a
    // loader through thousands of short timed waits, each satisfied early.
    log_wait_kind(h, milliseconds);

    static const bool s_log_waits = std::getenv("VITREO_LOG_WAITS") != nullptr;
    if (s_log_waits && milliseconds == INFINITE_WAIT) {
        static std::mutex s_seen_mu;
        static std::set<std::pair<u32, Handle>> s_seen;
        const u32 wait_tid = ThreadManager::get_current_thread_id_static();
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(s_seen_mu);
            first = s_seen.emplace(wait_tid, h).second;
        }
        if (first) {
            std::string wname;
            const char* wkind = "local";
            {
                std::shared_lock<HostRwLock> lock(m_mutex);
                if (auto it = m_events.find(h); it != m_events.end()) {
                    wname = it->second->name; wkind = "event";
                } else if (auto mi = m_mutexes.find(h); mi != m_mutexes.end()) {
                    wname = mi->second->name; wkind = "mutex";
                } else if (auto si = m_semaphores.find(h);
                           si != m_semaphores.end()) {
                    wname = si->second->name; wkind = "semaphore";
                } else if (m_handles.get_type(h) == HandleType::Thread) {
                    wkind = "thread";
                    if (auto* gt = m_handles.get_as<GuestThread>(
                            h, HandleType::Thread)) {
                        ipc::ipc_log(
                            "WaitEnter INFINITE h=0x%x kind=thread target_tid=%u waiter_tid=%u",
                            static_cast<unsigned>(h), gt->thread_id,
                            wait_tid);
                        first = false; // already logged
                    }
                } else if (m_handles.get_type(h) == HandleType::Process) {
                    wkind = "process";
                }
            }
            if (first) {
            ipc::ipc_log("WaitEnter INFINITE h=0x%x kind=%s name='%s' tid=%u",
                         static_cast<unsigned>(h), wkind, wname.c_str(),
                         wait_tid);
            }
        }
    }

    // Cross-process named sync object (event/mutex/semaphore): wait via the
    // shared-memory object so a peer in another host process can release us.
    const bool ipc_active = ipc::ipc_enabled();
    u64 resolved_local_nt_id = 0;
    if (ipc_active) {
        std::shared_ptr<ipc::SharedSyncObject> shared;
        std::string name;
        // All three object kinds carry a name; consulting only m_events made
        // every shared mutex/semaphore wait log as '', which reads like an
        // anonymous object being waited on. The kind matters too: a blocked
        // guest thread is diagnosable only if the log says which named object
        // it is parked on.
        const char* kind = "?";
        {
            std::shared_lock<HostRwLock> lock(m_mutex);
            if (auto it = m_events.find(h); it != m_events.end()) {
                shared = it->second->shared;
                resolved_local_nt_id = it->second->nt_id;
                if (shared) {
                    name = it->second->name;
                    kind = "event";
                }
            } else if (auto mi = m_mutexes.find(h); mi != m_mutexes.end()) {
                shared = mi->second->shared;
                resolved_local_nt_id = mi->second->nt_id;
                if (shared) {
                    name = mi->second->name;
                    kind = "mutex";
                }
            } else if (auto si = m_semaphores.find(h); si != m_semaphores.end()) {
                shared = si->second->shared;
                resolved_local_nt_id = si->second->nt_id;
                if (shared) {
                    name = si->second->name;
                    kind = "semaphore";
                }
            }
        }
        if (shared) {
            ipc::SharedSyncObject* arr[1] = {shared.get()};
            u32 tid = ThreadManager::get_current_thread_id_static();
            // On the GUI thread, slice the wait and pump AppKit between
            // slices so worker-thread CreateWindowEx / CFRunLoopPerformBlock
            // work can complete (otherwise UIManagerStarted deadlocks).
            constexpr u32 kPumpSliceMs = 16;
            const bool pump =
                (tid == (m_thread_manager ? m_thread_manager->main_thread_id() : 1u) && m_wait_pump && milliseconds != 0);
            // Peer signals cannot wake this process's run loop yet, so the
            // shared-object path still polls; the park only drains AppKit.
            auto gui_pump = [&]() {
                if (m_wait_park) m_wait_park(0); else m_wait_pump();
            };
            u32 r = WAIT_TIMEOUT;
            if (!pump) {
                r = ipc::wait_set(
                    arr, 1, /*wait_all=*/false, milliseconds, tid,
                    cancel_requested, blocked_ns, &m_shutdown);
            } else if (milliseconds == INFINITE_WAIT) {
                for (;;) {
                    if (m_shutdown.load(std::memory_order_acquire) ||
                        (cancel_requested != nullptr &&
                         cancel_requested->load(std::memory_order_acquire))) {
                        return WAIT_FAILED;
                    }
                    r = ipc::wait_set(
                        arr, 1, /*wait_all=*/false, kPumpSliceMs, tid,
                        cancel_requested, blocked_ns);
                    if (r != WAIT_TIMEOUT) break;
                    gui_pump();
                }
            } else {
                u32 remaining = milliseconds;
                for (;;) {
                    if (m_shutdown.load(std::memory_order_acquire) ||
                        (cancel_requested != nullptr &&
                         cancel_requested->load(std::memory_order_acquire))) {
                        return WAIT_FAILED;
                    }
                    const u32 slice =
                        remaining < kPumpSliceMs ? remaining : kPumpSliceMs;
                    r = ipc::wait_set(
                        arr, 1, /*wait_all=*/false, slice, tid,
                        cancel_requested, blocked_ns);
                    if (r != WAIT_TIMEOUT) break;
                    remaining -= slice;
                    if (remaining == 0) break;
                    gui_pump();
                }
            }
            ipc::ipc_log("Wait(shared %s) '%s' timeout=%u -> %s",
                         kind, name.c_str(), milliseconds,
                         r == 0 ? "SIGNALED" : (r == WAIT_TIMEOUT ? "timeout" : "other"));
            return r;
        }
    }

    // -------------------------------------------------------------------------
    // Resolve handle to an ntsync ObjectId.  Thread handles carry their own
    // manual-reset completion event in `GuestThread::nt_id`; all other
    // synchronization handles resolve via SyncManager's maps.
    // -------------------------------------------------------------------------
    u64 nt_id = resolved_local_nt_id;
    if (m_handles.get_type(h) == HandleType::Thread) {
        auto* gt = m_handles.get_as<GuestThread>(h, HandleType::Thread);
        if (!gt) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        nt_id = gt->nt_id;
        if (milliseconds == INFINITE_WAIT &&
            environment_flag_enabled("VITREO_THREAD_DIAG")) {
            static std::mutex s_thread_wait_diag_mu;
            static std::vector<std::pair<u32, Handle>> s_thread_wait_diag_seen;
            const u32 self = ThreadManager::get_current_thread_id_static();
            const std::pair<u32, Handle> key{self, h};
            std::lock_guard lock(s_thread_wait_diag_mu);
            if (s_thread_wait_diag_seen.size() < 128 &&
                std::find(s_thread_wait_diag_seen.begin(),
                          s_thread_wait_diag_seen.end(),
                          key) == s_thread_wait_diag_seen.end()) {
                s_thread_wait_diag_seen.push_back(key);
                std::fprintf(
                    stderr,
                    "[thread-diag] pid=%d wait-target self=%u handle=0x%x "
                    "target=%u start=0x%llx nt=0x%llx\n",
                    static_cast<int>(::getpid()), self, h, gt->thread_id,
                    static_cast<unsigned long long>(gt->start_address),
                    static_cast<unsigned long long>(gt->nt_id));
            }
        }
    } else if (m_handles.get_type(h) == HandleType::Process) {
        // A process handle's exit event fires when the tracked host process
        // exits. Process identity/liveness is a base CreateProcess contract,
        // independent of the optional shared-object IPC feature gate.
        auto* po = static_cast<ipc::ProcessObject*>(
            m_handles.get(h, HandleType::Process));
        if (po) nt_id = po->exit_event;
    }
    if (nt_id == 0) {
        // Local events/mutexes/semaphores and extra waitables (ConsoleInput,
        // memory-resource notification). Must run even when IPC is enabled:
        // those handles are not SharedSyncObject entries.
        std::shared_lock<HostRwLock> lock(m_mutex);
        nt_id = resolve_nt_id(h);
    }
    if (nt_id == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    ntsync::ObjectId id = nt_id;
    u32 tid = ThreadManager::get_current_thread_id_static();
    // Same GUI-thread AppKit pump as the shared-object path above. Local
    // events (CreateEvent followed by WaitForSingleObject) otherwise
    // deadlock when a worker's CreateDialogIndirectParam →
    // vtr_run_on_main_sync needs the GUI thread's CFRunLoop. MS Learn warns
    // WFSO on a window-creating thread can deadlock; Vitreo serializes Cocoa
    // UI onto guest tid 1, so slicing + pump is required host plumbing.
    constexpr u32 kPumpSliceMs = 16;
    // With a run-loop park installed the GUI thread no longer sleeps a fixed
    // slice: it probes the object, then parks in AppKit until an ntsync
    // signal, a queue post, or a worker's main-thread block wakes the run
    // loop. kParkCapMs only bounds a wake the run loop could miss.
    constexpr u32 kParkCapMs = 100;
    const bool pump = (tid == (m_thread_manager ? m_thread_manager->main_thread_id() : 1u) && m_wait_pump && milliseconds != 0);
    static const bool s_park_off = sync_mechanism_off("guipark");
    const bool use_park = m_wait_park && !s_park_off;
    const u32 probe_ms = use_park ? 0u : kPumpSliceMs;
    auto gui_park = [&](u32 cap_ms) {
        if (use_park) m_wait_park(cap_ms); else m_wait_pump();
        if (use_park) {
            ntsync::global().disarm_host_wake(tid);
            if (m_wait_park_arm) m_wait_park_arm(false);
        }
    };
    // Arm THIS object's wake before every probe so a signal between probe
    // and park is delivered, and only that object's signal wakes the park.
    auto gui_arm = [&]() {
        if (!pump || !use_park) return;
        const ntsync::ObjectId armed = id;
        ntsync::global().arm_host_wake(tid, &armed, 1);
        if (m_wait_park_arm) m_wait_park_arm(true);
    };
    // WaitForSingleObject never pumps on Windows; the park only keeps AppKit
    // events moving while the GUI thread is blocked for long. A park costs a
    // hop through the host main thread at both ends, and CoreAnimation/GCD
    // traffic ends it spuriously, so a wait satisfied within one pump slice
    // (the latency native events had under the slicing design, and the
    // cadence of the host main thread's own run-loop service in run.cpp)
    // waits on the object directly and parks only after that. Main work
    // queued by workers never needs this thread: the host main thread drains
    // it itself, and WaitForMultipleObjects already waits without parking.
    const auto wait_start = std::chrono::steady_clock::now();
    u32 r = WAIT_TIMEOUT;
    if (pump && use_park) {
        r = ntsync::global().wait(
            &id, 1, /*wait_all=*/false,
            milliseconds < kPumpSliceMs ? milliseconds : kPumpSliceMs, tid,
            cancel_requested, blocked_ns);
    }
    if (r != WAIT_TIMEOUT) {
        // Satisfied (or failed) without parking.
    } else if (!pump) {
        r = ntsync::global().wait(
            &id, 1, /*wait_all=*/false, milliseconds, tid, cancel_requested,
            blocked_ns);
    } else if (milliseconds == INFINITE_WAIT) {
        for (;;) {
            if (m_shutdown.load(std::memory_order_acquire) ||
                (cancel_requested != nullptr &&
                 cancel_requested->load(std::memory_order_acquire))) {
                return WAIT_FAILED;
            }
            gui_arm();
            r = ntsync::global().wait(
                &id, 1, /*wait_all=*/false, probe_ms, tid,
                cancel_requested, blocked_ns);
            if (r != WAIT_TIMEOUT) break;
            gui_park(kParkCapMs);
        }
    } else {
        const auto deadline =
            wait_start + std::chrono::milliseconds(milliseconds);
        for (;;) {
            if (m_shutdown.load(std::memory_order_acquire) ||
                (cancel_requested != nullptr &&
                 cancel_requested->load(std::memory_order_acquire))) {
                return WAIT_FAILED;
            }
            const auto now = std::chrono::steady_clock::now();
            const u32 remaining = now >= deadline ? 0u
                : static_cast<u32>(std::chrono::duration_cast<
                      std::chrono::milliseconds>(deadline - now).count());
            const u32 slice =
                remaining < kPumpSliceMs ? remaining : kPumpSliceMs;
            gui_arm();
            r = ntsync::global().wait(
                &id, 1, /*wait_all=*/false, use_park ? 0u : slice, tid,
                cancel_requested, blocked_ns);
            if (r != WAIT_TIMEOUT) break;
            if (remaining == 0) break;
            gui_park(remaining < kParkCapMs ? remaining : kParkCapMs);
        }
    }
    if (pump && use_park) {
        ntsync::global().disarm_host_wake(tid);
        if (m_wait_park_arm) m_wait_park_arm(false);
    }
    // Translate ntsync codes to Win32.  Same values, but normalize WAIT_ABANDONED_0.
    if (r == ntsync::WAIT_ABANDONED_0) return WAIT_ABANDONED;
    return r;
}

void SyncManager::notify_waiters() {
    ntsync::global().notify_waiters();
}

bool SyncManager::is_waitable_sync_handle(Handle h) const {
    if (h == NULL_HANDLE || h == INVALID_HANDLE) return false;
    std::shared_lock<HostRwLock> lock(m_mutex);
    if (resolve_shared(h)) return true;
    if (resolve_nt_id(h) != 0) return true;
    if (m_handles.get_type(h) == HandleType::Thread) {
        auto* gt = m_handles.get_as<GuestThread>(h, HandleType::Thread);
        return gt != nullptr && gt->nt_id != 0;
    }
    if (m_handles.get_type(h) == HandleType::Process) {
        auto* po = static_cast<ipc::ProcessObject*>(
            m_handles.get(h, HandleType::Process));
        return po != nullptr && po->exit_event != 0;
    }
    return false;
}

u32 SyncManager::wait_for_multiple_objects(u32 count, const Handle* handles,
                                            bool wait_all, u32 milliseconds,
                                            u64 extra_local_id) {
    ThreadManager::refresh_current_thread_scheduling();
    if (m_shutdown.load(std::memory_order_acquire)) {
        return WAIT_FAILED;
    }
    if (count == 0 || handles == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (u32 i = 0; i < count; ++i) {
        log_wait_kind(handles[i], milliseconds);
        trace_event_wait(handles[i], milliseconds, {});
    }

    struct WaitEntry {
        Handle handle{NULL_HANDLE};
        u64 nt_id{0};
        std::shared_ptr<ipc::SharedSyncObject> shared;
    };
    std::vector<WaitEntry> entries(count);
    bool any_shared = false;
    bool any_local = false;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        for (u32 i = 0; i < count; ++i) {
            Handle h = handles[i];
            if ((m_handles.get_type(h) == HandleType::Event ||
                 m_handles.get_type(h) == HandleType::Mutex) &&
                !(m_handles.get_access(h) & sync_access::SYNCHRONIZE)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                return WAIT_FAILED;
            }
            entries[i].handle = h;
            entries[i].shared = resolve_shared(h);
            if (entries[i].shared) {
                any_shared = true;
                continue;
            }
            if (m_handles.get_type(h) == HandleType::Thread) {
                auto* gt = m_handles.get_as<GuestThread>(h, HandleType::Thread);
                if (gt) entries[i].nt_id = gt->nt_id;
            } else if (m_handles.get_type(h) == HandleType::Process) {
                auto* po = static_cast<ipc::ProcessObject*>(
                    m_handles.get(h, HandleType::Process));
                if (po) entries[i].nt_id = po->exit_event;
            } else {
                entries[i].nt_id = resolve_nt_id(h);
            }
            if (entries[i].nt_id == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                return WAIT_FAILED;
            }
            any_local = true;
        }
    }
    if (extra_local_id != 0) {
        WaitEntry extra;
        extra.nt_id = extra_local_id;
        entries.push_back(extra);
        count = static_cast<u32>(entries.size());
        any_local = true;
    }

    u32 tid = ThreadManager::get_current_thread_id_static();

    // Homogeneous shared set: keep the dedicated ipc::wait_set path.
    if (any_shared && !any_local) {
        std::vector<ipc::SharedSyncObject*> raw;
        raw.reserve(count);
        for (auto& e : entries) raw.push_back(e.shared.get());
        return ipc::wait_set(
            raw.data(), count, wait_all, milliseconds, tid, &m_shutdown);
    }

    // Homogeneous local set: atomic ntsync wait.
    if (!any_shared) {
        std::vector<ntsync::ObjectId> ids;
        ids.reserve(count);
        for (const auto& e : entries) ids.push_back(e.nt_id);
        return ntsync::global().wait(
            ids.data(), static_cast<u32>(ids.size()), wait_all, milliseconds,
            tid, &m_shutdown);
    }

    // Mixed local + SharedSyncObject (common after DuplicateHandle promotes a
    // cross-process event). The event may be signaled while MsgWait/WFMO
    // still lists local thread and process handles. Probe both worlds; park
    // briefly on local ntsync so shared SetEvent's notify_waiters can
    // interrupt the slice.
    const bool infinite = (milliseconds == INFINITE_WAIT);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(infinite ? 0 : milliseconds);
    constexpr u32 kSliceMs = 16;

    auto probe = [&]() -> std::optional<u32> {
        if (!wait_all) {
            for (u32 i = 0; i < count; ++i) {
                if (entries[i].shared) {
                    const auto result =
                        entries[i].shared->try_consume(tid);
                    if (result != ipc::SyncConsumeResult::NotReady) {
                        return (result == ipc::SyncConsumeResult::Abandoned
                                    ? WAIT_ABANDONED
                                    : WAIT_OBJECT_0) + i;
                    }
                } else {
                    ntsync::ObjectId id = entries[i].nt_id;
                    const u32 r = ntsync::global().wait(
                        &id, 1, false, 0, tid, &m_shutdown);
                    if (r == ntsync::WAIT_ABANDONED_0) {
                        return WAIT_ABANDONED + i;
                    }
                    if (r == WAIT_OBJECT_0) return WAIT_OBJECT_0 + i;
                    if (r == WAIT_FAILED) return WAIT_FAILED;
                }
            }
            return std::nullopt;
        }

        // wait_all: try shared first (undo on failure), then local 0-timeout.
        std::vector<std::pair<u32, bool>> shared_acquired;
        for (u32 i = 0; i < count; ++i) {
            if (!entries[i].shared) continue;
            const auto result = entries[i].shared->try_consume(tid);
            if (result == ipc::SyncConsumeResult::NotReady) {
                for (const auto& [idx, was_abandoned] : shared_acquired) {
                    entries[idx].shared->undo_consume(tid, was_abandoned);
                }
                return std::nullopt;
            }
            shared_acquired.emplace_back(
                i, result == ipc::SyncConsumeResult::Abandoned);
        }
        std::vector<ntsync::ObjectId> local_ids;
        std::vector<u32> local_original_indices;
        local_ids.reserve(count);
        local_original_indices.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            if (!entries[i].shared) {
                local_ids.push_back(entries[i].nt_id);
                local_original_indices.push_back(i);
            }
        }
        std::optional<u32> abandoned_index;
        for (const auto& [idx, was_abandoned] : shared_acquired) {
            if (was_abandoned &&
                (!abandoned_index || idx < *abandoned_index)) {
                abandoned_index = idx;
            }
        }
        if (!local_ids.empty()) {
            const u32 r = ntsync::global().wait(
                local_ids.data(), static_cast<u32>(local_ids.size()), true, 0,
                tid, &m_shutdown);
            if (r == WAIT_FAILED) {
                for (const auto& [idx, was_abandoned] : shared_acquired) {
                    entries[idx].shared->undo_consume(tid, was_abandoned);
                }
                return WAIT_FAILED;
            }
            if (r == WAIT_TIMEOUT) {
                for (const auto& [idx, was_abandoned] : shared_acquired) {
                    entries[idx].shared->undo_consume(tid, was_abandoned);
                }
                return std::nullopt;
            }
            if (r >= ntsync::WAIT_ABANDONED_0 &&
                r < ntsync::WAIT_ABANDONED_0 + local_ids.size()) {
                const u32 idx = local_original_indices[
                    r - ntsync::WAIT_ABANDONED_0];
                if (!abandoned_index || idx < *abandoned_index) {
                    abandoned_index = idx;
                }
            }
        }
        return abandoned_index ? WAIT_ABANDONED + *abandoned_index
                               : WAIT_OBJECT_0;
    };

    for (;;) {
        if (m_shutdown.load(std::memory_order_acquire)) return WAIT_FAILED;
        if (auto r = probe()) return *r;
        if (milliseconds == 0) return WAIT_TIMEOUT;

        u32 slice = kSliceMs;
        if (!infinite) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return WAIT_TIMEOUT;
            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now)
                    .count();
            slice = static_cast<u32>(
                remaining_ms < static_cast<i64>(kSliceMs) ? remaining_ms
                                                          : kSliceMs);
        }

        std::vector<ntsync::ObjectId> local_ids;
        for (const auto& e : entries) {
            if (!e.shared) local_ids.push_back(e.nt_id);
        }
        if (!local_ids.empty()) {
            // Learn: only the object(s) satisfying the complete guest wait
            // may be consumed. A local-only parking wait used to swallow an
            // auto-reset event/semaphore release, or acquire a mutex, and
            // discard its result before rechecking the mixed set.
            const u32 parked = ntsync::global().wait_for_notification(
                local_ids.data(), static_cast<u32>(local_ids.size()),
                slice, tid, &m_shutdown, /*return_if_ready=*/!wait_all);
            if (parked == WAIT_FAILED) return WAIT_FAILED;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        }
    }
}

void SyncManager::begin_shutdown() {
    m_shutdown.store(true, std::memory_order_release);
    ntsync::global().notify_waiters();
    // Release waitable-timer workers parked on a deadline as well.
    m_waitable_timer_cv.notify_all();
    // Windows process termination ends threads blocked in condition-variable
    // waits too. This is an internal cancellation wake: do not advance the
    // guest-visible condition generation or consume/signal a guest object.
    std::shared_lock<HostRwLock> lock(m_mutex);
    for (const auto& [addr, condition] : m_condition_variables) {
        (void)addr;
        // Take each waiter's mutex before notifying, for the same reason the
        // wake_* paths bump the generation under it: m_shutdown is part of the
        // wait predicate, and a waiter that has already evaluated it as false
        // but has not yet parked would otherwise miss this notify and block
        // forever at teardown. Acquiring the mutex cannot deadlock here — a
        // waiter only holds it across the predicate check and the park.
        {
            std::lock_guard<std::mutex> publish(condition->wait_mutex);
        }
        condition->cv.notify_all();
    }
}

} // namespace vitreo::winapi
