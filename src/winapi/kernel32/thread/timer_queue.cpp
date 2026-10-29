// SPDX-License-Identifier: LGPL-2.1-only

/// @file timer_queue.cpp
/// @brief Timer-queue timers, Get/SetThreadContext, and proc-thread attributes.
///
/// CreateTimerQueueTimer callbacks, thread context and cycle-time queries,
/// and Initialize/Update/DeleteProcThreadAttributeList.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/system/processor_topology.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/thread_context.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/common/thread_times_oracle.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Timer-queue timers. CreateTimerQueueTimer arms a one-shot or periodic
// callback. Left as a missing-export stub the call returns 0xFFFFFFFF and
// no timer ever fires.
//
// Learn: https://learn.microsoft.com/en-us/windows/win32/api/threadpoollegacyapiset/
// Behaviour Learn leaves open is measured by s4_threadpool_probe (x86 == x64):
//  - ChangeTimerQueueTimer re-arms from now with the new DueTime and Period
//    (ctqt_change_far_to_near, ctqt_change_periodic); on an expired one-shot
//    it returns TRUE and changes nothing (ctqt_change_expired_oneshot).
//  - DeleteTimerQueueTimer / DeleteTimerQueueEx with INVALID_HANDLE_VALUE
//    wait for running callbacks and return TRUE; with NULL or an event while
//    a callback runs they return FALSE / ERROR_IO_PENDING, and the event is
//    set only after the callback returns (dtqt_*_while_running,
//    dtqex_null_while_running).
// ---------------------------------------------------------------------------

namespace {
constexpr u32 WT_EXECUTEINTIMERTHREAD = 0x00000020u;
constexpr u32 WT_EXECUTEONLYONCE = 0x00000008u;
}  // namespace

// HANDLE CreateTimerQueue(void)
void Kernel32ExtManager::impl_CreateTimerQueue(Marshaler& m) {
    // A queue is the set of timers created against it; it needs no backing
    // object beyond its handle value, which DeleteTimerQueueEx matches.
    const u32 handle = 0xFD000000u + m_timer_queue_counter.fetch_add(1) + 1;
    m.set_return(handle);
}

bool Kernel32ExtManager::reap_callback_threads(std::vector<Handle>& threads) {
    bool running = false;
    if (m_thread_manager == nullptr) return false;
    for (auto it = threads.begin(); it != threads.end();) {
        const auto code = m_thread_manager->get_exit_code(*it);
        if (code.is_ok() && code.value() == STILL_ACTIVE) {
            running = true;
            ++it;
        } else {
            m_thread_manager->close_thread_handle(*it);
            it = threads.erase(it);
        }
    }
    return running;
}

// BOOL CreateTimerQueueTimer(PHANDLE, HANDLE Queue, WAITORTIMERCALLBACK,
//                            PVOID Param, DWORD DueTime, DWORD Period, ULONG Flags)
void Kernel32ExtManager::impl_CreateTimerQueueTimer(Marshaler& m) {
    const GuestAddr phNewTimer = m.arg_ptr(0);
    const u32 queue = static_cast<u32>(m.arg(1));
    const GuestAddr callback = m.arg_ptr(2);
    const GuestAddr parameter = m.arg_ptr(3);
    const u32 due_time = m.arg32(4);
    const u32 period = m.arg32(5);
    const u32 flags = m.arg32(6);

    if (callback == 0 || phNewTimer == 0 || !m_thread_manager || !m_sync_manager) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const u32 handle = 0xFC000000u + m_timer_queue_counter.fetch_add(1) + 1;
    auto state = std::make_shared<TimerQueueTimerState>();
    state->queue = queue;
    state->once = (flags & WT_EXECUTEONLYONCE) != 0;
    state->in_timer_thread = (flags & WT_EXECUTEINTIMERTHREAD) != 0;
    state->period_ms = state->once ? 0u : period;
    state->due = std::chrono::steady_clock::now() + std::chrono::milliseconds(due_time);
    const auto write_result = m.cpu_is_64bit()
        ? m_memory.write_u64(phNewTimer, handle)
        : m_memory.write_u32(phNewTimer, handle);
    if (write_result.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_timer_queue_mutex);
        m_timer_queue_timers[handle] = state;
    }

    ThreadManager* threads = m_thread_manager;
    SyncManager* sync = m_sync_manager;
    std::thread worker([this, threads, sync, callback, parameter, state]() {
        std::unique_lock<std::mutex> lock(state->mutex);
        while (!state->deleted && !m_shutdown.load(std::memory_order_acquire)) {
            if (state->expired_oneshot) {
                state->cv.wait(lock);
                continue;
            }
            const u64 generation = state->generation;
            const auto due = state->due;
            if (std::chrono::steady_clock::now() < due) {
                state->cv.wait_until(lock, due);
                continue;
            }
            if (generation != state->generation) continue;
            if (state->period_ms != 0) {
                state->due = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(state->period_ms);
            } else {
                state->expired_oneshot = true;
            }
            (void)reap_callback_threads(state->callback_threads);
            // WAITORTIMERCALLBACK(Parameter, TimerOrWaitFired = TRUE).
            u32 tid = 0;
            const Handle th = threads->create_thread(0, callback, parameter, 0,
                                                     &tid, /*fired=*/1u);
            if (th == INVALID_HANDLE || th == 0) continue;
            state->callback_threads.push_back(th);
            lock.unlock();
            threads->run_async(tid, callback, parameter, 1u, 0, 2);
            if (state->in_timer_thread) {
                // WT_EXECUTEINTIMERTHREAD: the timer's own thread runs the
                // callback, so the next expiry waits for it to return.
                (void)sync->wait_for_single_object(th, INFINITE_WAIT);
            }
            lock.lock();
        }
        // Deleted: drain the callbacks, then report completion.
        std::vector<Handle> pending = std::move(state->callback_threads);
        const Handle completion = state->completion_event;
        lock.unlock();
        for (const Handle th : pending) {
            if (!m_shutdown.load(std::memory_order_acquire)) {
                (void)sync->wait_for_single_object(th, INFINITE_WAIT);
            }
            threads->close_thread_handle(th);
        }
        if (!m_shutdown.load(std::memory_order_acquire) &&
            completion != NULL_HANDLE && completion != INVALID_HANDLE) {
            (void)sync->set_event(completion);
        }
        lock.lock();
        state->finished = true;
        state->cv.notify_all();
        lock.unlock();
        threads->note_host_thread_done();
    });
    threads->track_host_thread(std::move(worker));
    m.set_return(1);
}

// BOOL ChangeTimerQueueTimer(HANDLE Queue, HANDLE Timer, ULONG DueTime, ULONG Period)
void Kernel32ExtManager::impl_ChangeTimerQueueTimer(Marshaler& m) {
    const u32 timer = static_cast<u32>(m.arg(1));
    const u32 due_time = m.arg32(2);
    const u32 period = m.arg32(3);
    std::shared_ptr<TimerQueueTimerState> state;
    {
        std::lock_guard<std::mutex> lock(m_timer_queue_mutex);
        auto it = m_timer_queue_timers.find(timer);
        if (it != m_timer_queue_timers.end()) state = it->second;
    }
    if (!state) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        // Learn: an expired one-shot timer is not updated (and the call
        // still succeeds: ctqt_change_expired_oneshot ok=1).
        if (!state->expired_oneshot) {
            state->due = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(due_time);
            state->period_ms = state->once ? 0u : period;
            ++state->generation;
        }
    }
    state->cv.notify_all();
    m.set_return(1);
}

bool Kernel32ExtManager::delete_timer_state(
    const std::shared_ptr<TimerQueueTimerState>& state, Handle completion,
    bool& io_pending) {
    std::unique_lock<std::mutex> lock(state->mutex);
    const bool running = reap_callback_threads(state->callback_threads);
    state->deleted = true;
    if (completion != INVALID_HANDLE) state->completion_event = completion;
    state->cv.notify_all();
    if (completion == INVALID_HANDLE) {
        // Blocking delete: wait for the timer thread to drain its callbacks.
        state->cv.wait(lock, [&] {
            return state->finished || m_shutdown.load(std::memory_order_acquire);
        });
        return true;
    }
    io_pending = io_pending || running;
    return true;
}

// BOOL DeleteTimerQueueTimer(HANDLE Queue, HANDLE Timer, HANDLE CompletionEvent)
void Kernel32ExtManager::impl_DeleteTimerQueueTimer(Marshaler& m) {
    const u32 timer = static_cast<u32>(m.arg(1));
    const auto completion = static_cast<Handle>(m.arg(2));
    std::shared_ptr<TimerQueueTimerState> state;
    {
        std::lock_guard<std::mutex> lock(m_timer_queue_mutex);
        auto it = m_timer_queue_timers.find(timer);
        if (it != m_timer_queue_timers.end()) {
            state = it->second;
            m_timer_queue_timers.erase(it);
        }
    }
    if (!state) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    bool io_pending = false;
    (void)delete_timer_state(state, completion, io_pending);
    if (io_pending) {
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

// BOOL DeleteTimerQueueEx(HANDLE TimerQueue, HANDLE CompletionEvent)
void Kernel32ExtManager::impl_DeleteTimerQueueEx(Marshaler& m) {
    const u32 queue = static_cast<u32>(m.arg(0));
    const auto completion = static_cast<Handle>(m.arg(1));
    std::vector<std::shared_ptr<TimerQueueTimerState>> timers;
    {
        std::lock_guard<std::mutex> lock(m_timer_queue_mutex);
        for (auto it = m_timer_queue_timers.begin(); it != m_timer_queue_timers.end();) {
            if (it->second->queue == queue) {
                timers.push_back(it->second);
                it = m_timer_queue_timers.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Every timer of the queue is deleted with the same completion rule; an
    // event given here is set once the last of them has drained.
    bool io_pending = false;
    const Handle per_timer = completion == INVALID_HANDLE ? INVALID_HANDLE : NULL_HANDLE;
    for (const auto& state : timers) {
        (void)delete_timer_state(state, per_timer, io_pending);
    }
    if (completion != NULL_HANDLE && completion != INVALID_HANDLE && m_sync_manager) {
        if (!io_pending) {
            (void)m_sync_manager->set_event(completion);
        } else {
            SyncManager* sync = m_sync_manager;
            ThreadManager* threads = m_thread_manager;
            std::thread waiter([this, sync, threads, timers, completion] {
                for (const auto& state : timers) {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->cv.wait(lock, [&] {
                        return state->finished || m_shutdown.load(std::memory_order_acquire);
                    });
                }
                if (!m_shutdown.load(std::memory_order_acquire)) {
                    (void)sync->set_event(completion);
                }
                threads->note_host_thread_done();
            });
            threads->track_host_thread(std::move(waiter));
        }
    }
    if (io_pending) {
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

// BOOL DeleteTimerQueue(HANDLE TimerQueue): DeleteTimerQueueEx(TimerQueue,
// NULL) per Learn.
void Kernel32ExtManager::impl_DeleteTimerQueue(Marshaler& m) {
    const u32 queue = static_cast<u32>(m.arg(0));
    std::vector<std::shared_ptr<TimerQueueTimerState>> timers;
    {
        std::lock_guard<std::mutex> lock(m_timer_queue_mutex);
        for (auto it = m_timer_queue_timers.begin(); it != m_timer_queue_timers.end();) {
            if (it->second->queue == queue) {
                timers.push_back(it->second);
                it = m_timer_queue_timers.erase(it);
            } else {
                ++it;
            }
        }
    }
    bool io_pending = false;
    for (const auto& state : timers) {
        (void)delete_timer_state(state, NULL_HANDLE, io_pending);
    }
    if (io_pending) {
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetThreadId(Marshaler& m) {
    const u64 raw = m.arg(0);
    const bool pseudo = m.cpu_is_64bit() ? raw == ~u64{1}
                                       : static_cast<u32>(raw) == static_cast<u32>(-2);
    // A HANDLE is pointer-sized, even though our real handle namespace is
    // 32-bit. Native x64 rejects upper bits and a zero-extended pseudo handle;
    // truncating first would turn either into a different valid handle.
    if (m.cpu_is_64bit() && !pseudo &&
        (raw > UINT32_MAX || raw == static_cast<u32>(-2))) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const Handle h = static_cast<Handle>(raw);
    u32 tid = 0;
    u32 error = error_codes::ERROR_INVALID_HANDLE;
    if (!pseudo) {
        std::lock_guard lock(m_external_thread_mutex);
        const auto handle_it = m_external_primary_handles.find(h);
        if (handle_it != m_external_primary_handles.end() && m_handle_table &&
            m_handle_table->get_type(h) == HandleType::Thread) {
            const u32 query = ThreadManager::THREAD_QUERY_INFORMATION |
                              ThreadManager::THREAD_QUERY_LIMITED_INFORMATION;
            if (m_handle_table->get_access(h) & query) {
                tid = handle_it->second;
            } else {
                error = error_codes::ERROR_ACCESS_DENIED;
            }
            if (tid == 0) set_last_error(error);
            m.set_return(tid);
            return;
        }
    }
    // Resolve and read the ID while the owning manager holds its lifecycle
    // mutex, so closing the final handle cannot free the thread mid-query.
    if (m_thread_manager) {
        tid = m_thread_manager->thread_id_for_handle(
            h, ThreadManager::THREAD_QUERY_LIMITED_INFORMATION, error);
    }
    // Learn GetThreadId; native thread_id_access_probe in both ABIs. Success
    // preserves last-error, including for a terminated but retained thread.
    if (tid == 0) set_last_error(error);
    m.set_return(tid);
}

// Learn GetProcessIdOfThread; process_id_of_thread_probe records the rights,
// handle-width and post-exit behavior for both guest ABIs. External primary
// threads belong to their child process, including after its handle closes.
void Kernel32ExtManager::impl_GetProcessIdOfThread(Marshaler& m) {
    const u64 raw = m.arg(0);
    const bool pseudo = m.cpu_is_64bit() ? raw == ~u64{1}
                                       : static_cast<u32>(raw) == static_cast<u32>(-2);
    if (m.cpu_is_64bit() && !pseudo &&
        (raw > UINT32_MAX || raw == static_cast<u32>(-2))) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const Handle h = static_cast<Handle>(raw);
    if (!pseudo) {
        std::lock_guard lock(m_external_thread_mutex);
        const auto handle_it = m_external_primary_handles.find(h);
        if (handle_it != m_external_primary_handles.end() && m_handle_table &&
            m_handle_table->get_type(h) == HandleType::Thread) {
            const u32 query = ThreadManager::THREAD_QUERY_INFORMATION |
                              ThreadManager::THREAD_QUERY_LIMITED_INFORMATION;
            if (!(m_handle_table->get_access(h) & query)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                m.set_return(0);
                return;
            }
            const auto found = m_external_primary_by_tid.find(handle_it->second);
            if (found != m_external_primary_by_tid.end() &&
                found->second.process != nullptr) {
                // ProcessObject is stable through host-process teardown;
                // this query does not borrow an object from a closed HANDLE.
                m.set_return(static_cast<u32>(found->second.process->pid));
                return;
            }
        }
    }
    u32 error = error_codes::ERROR_INVALID_HANDLE;
    if (m_thread_manager && m_process_manager &&
        m_thread_manager->thread_id_for_handle(
            h, ThreadManager::THREAD_QUERY_LIMITED_INFORMATION, error) != 0) {
        m.set_return(m_process_manager->get_current_process_id());
        return;
    }
    set_last_error(error);
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetThreadTimes(Marshaler& m) {
    const u64 raw = m.arg(0);
    const bool x64 = m.cpu_is_64bit();
    const bool pseudo = x64 ? raw == ~u64{1}
                            : static_cast<u32>(raw) == static_cast<u32>(-2);
    if (x64 && !pseudo && (raw > UINT32_MAX || raw == static_cast<u32>(-2))) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const Handle handle = static_cast<Handle>(raw);
    if (!pseudo) {
        std::lock_guard lock(m_external_thread_mutex);
        if (m_external_primary_handles.contains(handle) && m_handle_table &&
            m_handle_table->get_type(handle) == HandleType::Thread) {
            const u32 query = ThreadManager::THREAD_QUERY_INFORMATION |
                              ThreadManager::THREAD_QUERY_LIMITED_INFORMATION;
            if (!(m_handle_table->get_access(handle) & query)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
            } else {
                static std::atomic<bool> reported{false};
                if (!reported.exchange(true))
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!GetThreadTimes: cross-process thread CPU accounting\n");
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            }
            m.set_return(0);
            return;
        }
    }
    std::array<u64, 4> times{};
    u32 error = error_codes::ERROR_INVALID_HANDLE;
    if (!m_thread_manager || !m_thread_manager->get_thread_times(handle, times, error)) {
        if (error == error_codes::ERROR_CALL_NOT_IMPLEMENTED) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!GetThreadTimes: host thread CPU counters unavailable\n");
        }
        set_last_error(error);
        m.set_return(0);
        return;
    }
    // Native oracle: writes are sequential; bad outputs raise rather than
    // return ERROR_NOACCESS. x86 writes low/high separately; x64 writes 8B.
    auto fault = [&](GuestAddr address, usize size) {
        auto* cpu = m.cpu();
        if (!cpu) return;
        for (usize offset = 0; offset < size; ++offset) {
            if (offset > std::numeric_limits<GuestAddr>::max() - address ||
                !m_memory.is_range_writable(address + offset, 1, x64)) {
                if (offset <= std::numeric_limits<GuestAddr>::max() - address)
                    address += offset;
                break;
            }
        }
        auto& pending = cpu->pending_raise;
        pending.clear();
        pending.active = true;
        pending.code = nt_status::STATUS_ACCESS_VIOLATION;
        pending.num_args = 2;
        pending.args[0] = 1;
        pending.args[1] = address;
    };
    static_assert(sizeof(u64) == thread_times_oracle::FILETIME_SIZE);
    for (usize index = 0; index < times.size(); ++index) {
        const GuestAddr pointer = m.arg_ptr(index + 1);
        if (x64) {
            if (!m_memory.write_u64(pointer, times[index]).is_ok()) {
                fault(pointer, sizeof(u64));
                return;
            }
        } else {
            if (!m_memory.write_u32(pointer, static_cast<u32>(times[index])).is_ok()) {
                fault(pointer, sizeof(u32));
                return;
            }
            const GuestAddr high = pointer + thread_times_oracle::FILETIME_HIGH;
            if (!m_memory.write_u32(high, static_cast<u32>(times[index] >> 32)).is_ok()) {
                fault(high, sizeof(u32));
                return;
            }
        }
    }
    m.set_return(1);
}

// GetThreadContext / SetThreadContext are NtGet/NtSetContextThread with the
// status reported through the last error; success leaves it untouched
// (thread_context_probe).
void Kernel32ExtManager::impl_GetThreadContext(Marshaler& m) {
    const u32 status = nt_get_context_thread(m, m_thread_manager, m.arg(0), m.arg_ptr(1));
    if (status != 0) set_last_error(thread_context_status_to_win32(status));
    m.set_return(status == 0 ? 1 : 0);
}

void Kernel32ExtManager::impl_SetThreadContext(Marshaler& m) {
    const u32 status = nt_set_context_thread(m, m_thread_manager, m.arg(0), m.arg_ptr(1));
    if (status != 0) set_last_error(thread_context_status_to_win32(status));
    m.set_return(status == 0 ? 1 : 0);
}

void Kernel32ExtManager::impl_GetThreadGroupAffinity(Marshaler& m) {
    // BOOL GetThreadGroupAffinity(HANDLE hThread, PGROUP_AFFINITY GroupAffinity)
    // The thread's affinity in the single group (processor_topology_probe "gtga").
    const Handle thread = static_cast<Handle>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0 || m_thread_manager == nullptr) {
        set_last_error(out == 0 ? error_codes::ERROR_INVALID_PARAMETER : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    u64 stored = 0;
    if (const u32 error = m_thread_manager->get_thread_affinity(thread, stored); error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    if (!processor_topology::write_group_affinity(m, out, effective_thread_affinity(m, stored), 0)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_SetThreadGroupAffinity(Marshaler& m) {
    // BOOL SetThreadGroupAffinity(HANDLE hThread, const GROUP_AFFINITY* GroupAffinity,
    //                             PGROUP_AFFINITY PreviousGroupAffinity)
    // Measured (processor_topology_probe "stga", x86 = x64):
    // - a group other than 0, a non-zero Reserved word, or a mask with any
    //   processor outside the group fails with ERROR_INVALID_PARAMETER and
    //   leaves PreviousGroupAffinity alone (unlike SetThreadAffinityMask, a
    //   partly-outside mask is not trimmed);
    // - a zero mask succeeds without changing the affinity;
    // - PreviousGroupAffinity may be NULL; when given it receives the prior
    //   affinity.
    const Handle thread = static_cast<Handle>(m.arg(0));
    const GuestAddr wanted_at = m.arg_ptr(1);
    const GuestAddr previous_at = m.arg_ptr(2);
    processor_topology::GroupAffinity wanted;
    if (wanted_at == 0 || !processor_topology::read_group_affinity(m, wanted_at, wanted) ||
        wanted.group != 0 || !wanted.reserved_zero ||
        (wanted.mask & ~processor_topology::guest_affinity(m, processor_topology::processor_mask())) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    u64 previous = 0;
    const u32 error = wanted.mask == 0
        ? m_thread_manager->get_thread_affinity(thread, previous)
        : m_thread_manager->set_thread_affinity(thread, wanted.mask & process_affinity_mask(m), previous);
    if (error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    if (previous_at != 0 &&
        !processor_topology::write_group_affinity(m, previous_at, effective_thread_affinity(m, previous), 0)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

// BOOL SetThreadInformation(HANDLE hThread, THREAD_INFORMATION_CLASS,
//                           LPVOID ThreadInformation, DWORD ThreadInformationSize)
// Learn: only ThreadMemoryPriority and ThreadPowerThrottling are supported.
// Validation order and errors measured by power_throttling_probe.c (x86 = x64):
// the structure is checked before the handle; THREAD_SET_INFORMATION is
// required (THREAD_SET_LIMITED_INFORMATION -> ERROR_ACCESS_DENIED).
void Kernel32ExtManager::impl_SetThreadInformation(Marshaler& m) {
    // processthreadsapi.h THREAD_INFORMATION_CLASS (SDK 10.0.26100).
    enum : u32 {
        ThreadMemoryPriority = 0,
        ThreadAbsoluteCpuPriority = 1,
        ThreadDynamicCodePolicy = 2,
        ThreadPowerThrottling = 3,
    };
    const Handle thread = static_cast<Handle>(m.arg(0));
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 size = static_cast<u32>(m.arg(3));
    auto fail = [&](u32 error) {
        set_last_error(error);
        m.set_return(0);
    };
    GuestAddr faulted = 0;
    auto read = [&](u32 offset, u32& out) {
        auto value = m_memory.read_u32(info + offset);
        if (value.is_err()) {
            faulted = info + offset;
            return false;
        }
        out = value.value();
        return true;
    };
    // Windows reads the structure in user mode: an unreadable one faults.
    auto fault = [&] {
        if (!m.raise_access_violation(false, faulted)) {
            fail(error_codes::ERROR_NOACCESS);
        }
    };

    switch (info_class) {
    case ThreadMemoryPriority: {
        // MEMORY_PRIORITY_INFORMATION { ULONG MemoryPriority; }; winnt.h
        // MEMORY_PRIORITY_VERY_LOW 1 .. NORMAL 5 (LOWEST 0 is refused).
        if (size != 4) return fail(error_codes::ERROR_BAD_LENGTH);
        u32 priority = 0;
        if (!read(0, priority)) return fault();
        if (priority < 1 || priority > 5) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        // Memory priority orders working-set trimming; Vitreo never trims a
        // guest working set, so the recorded priority has nothing to reorder.
        const u32 error = m_thread_manager != nullptr
            ? m_thread_manager->set_thread_memory_priority(thread, priority)
            : error_codes::ERROR_INVALID_HANDLE;
        if (error != 0) return fail(error);
        m.set_return(1);
        return;
    }
    case ThreadPowerThrottling: {
        u32 version = 0, control = 0, state = 0;
        if (size >= 12 &&
            (!read(0, version) || !read(4, control) || !read(8, state))) {
            return fault();
        }
        u32 error = validate_thread_power_throttling(size, version, control, state);
        if (error == 0) {
            error = m_thread_manager != nullptr
                ? m_thread_manager->set_thread_power_throttling(thread, control, state)
                : error_codes::ERROR_INVALID_HANDLE;
        }
        if (error != 0) return fail(error);
        m.set_return(1);
        return;
    }
    case ThreadAbsoluteCpuPriority:
    case ThreadDynamicCodePolicy: {
        if (size != 4) return fail(error_codes::ERROR_BAD_LENGTH);
        static std::atomic<bool> reported[2]{};
        if (!reported[info_class - 1].exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED kernel32!SetThreadInformation: %s\n",
                         info_class == ThreadAbsoluteCpuPriority
                             ? "ThreadAbsoluteCpuPriority"
                             : "ThreadDynamicCodePolicy");
        }
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    default:
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
}

void Kernel32ExtManager::impl_QueryThreadCycleTime(Marshaler& m) {
    GuestAddr p = m.arg_ptr(1);
    if (p) (void)m_memory.write_u64(p, 0);
    m.set_return(1);
}

void Kernel32ExtManager::impl_QueryProcessCycleTime(Marshaler& m) {
    GuestAddr p = m.arg_ptr(1);
    if (p) (void)m_memory.write_u64(p, 0);
    m.set_return(1);
}

void Kernel32ExtManager::impl_InitializeProcThreadAttributeList(Marshaler& m) {
    GuestAddr list = m.arg_ptr(0);
    const u32 attribute_capacity = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    GuestAddr size_ptr = m.arg_ptr(3);
    if (size_ptr == 0 || flags != 0 || attribute_capacity == 0 ||
        attribute_capacity > 64) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // The structure is opaque to callers. Reserve a fixed header plus one
    // pointer-sized key/size/value record per requested attribute.
    const u64 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const u64 required_size =
        pointer_size * 4u + static_cast<u64>(attribute_capacity) *
                                pointer_size * 3u;
    auto write_required_size = [&] {
        if (m.cpu_is_64bit()) {
            return m_memory.write_u64(size_ptr, required_size).is_ok();
        }
        return m_memory
            .write_u32(size_ptr, static_cast<u32>(required_size))
            .is_ok();
    };

    if (list == 0) {
        if (!write_required_size()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }

    u64 supplied_size = 0;
    if (m.cpu_is_64bit()) {
        auto value = m_memory.read_u64(size_ptr);
        if (value.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        supplied_size = value.value();
    } else {
        auto value = m_memory.read_u32(size_ptr);
        if (value.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        supplied_size = value.value();
    }
    if (!write_required_size()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (supplied_size < required_size) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }

    for (u64 index = 0; index < required_size; ++index) {
        if (m_memory.write_u8(list + index, 0).is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_proc_thread_attribute_mutex);
        ProcThreadAttributeListState state;
        state.capacity = attribute_capacity;
        m_proc_thread_attribute_lists[list] = std::move(state);
    }
    ipc::ipc_log(
        "InitializeProcThreadAttributeList list=0x%llx capacity=%u size=%llu -> TRUE",
        static_cast<unsigned long long>(list), attribute_capacity,
        static_cast<unsigned long long>(required_size));
    VITREO_TRACE(
        "[kernel32_ext] InitializeProcThreadAttributeList(list=0x%llx, "
        "capacity=%u, size=%llu) -> TRUE\n",
        static_cast<unsigned long long>(list), attribute_capacity,
        static_cast<unsigned long long>(required_size));
    m.set_return(1);
}

void Kernel32ExtManager::impl_UpdateProcThreadAttribute(Marshaler& m) {
    constexpr u64 PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020002;
    constexpr u64 PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY = 0x00020007;
    constexpr u64 PROC_THREAD_ATTRIBUTE_JOB_LIST = 0x0002000D;
    constexpr u64 PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = 0x0002000E;
    constexpr u64 PROC_THREAD_ATTRIBUTE_COMPONENT_FILTER = 0x0002001A;
    constexpr u64 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016;
    constexpr u32 PROCESS_CREATION_CHILD_PROCESS_RESTRICTED = 0x1;
    constexpr u32 PROCESS_CREATION_CHILD_PROCESS_OVERRIDE = 0x2;
    const GuestAddr list = m.arg_ptr(0);
    const u64 flags = m.arg(1);
    const u64 attribute = m.arg(2);
    const GuestAddr value_ptr = m.arg_ptr(3);
    const u64 value_size = m.arg(4);
    const GuestAddr previous_value = m.arg_ptr(5);
    const GuestAddr return_size = m.arg_ptr(6);
    const u64 handle_size = m.cpu_is_64bit() ? 8u : 4u;

    ipc::ipc_log(
        "UpdateProcThreadAttribute list=0x%llx attribute=0x%llx size=%llu",
        static_cast<unsigned long long>(list),
        static_cast<unsigned long long>(attribute),
        static_cast<unsigned long long>(value_size));

    if (list == 0 || flags != 0 || value_ptr == 0 || value_size == 0 ||
        previous_value != 0 || return_size != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // PROC_THREAD_ATTRIBUTE_COMPONENT_FILTER is not implemented. Fail with
    // ERROR_NOT_SUPPORTED: an unavailable component filter is an older-Windows
    // capability boundary. ERROR_INVALID_PARAMETER turns that optional
    // attribute into a process-launch failure.
    if (attribute == PROC_THREAD_ATTRIBUTE_COMPONENT_FILTER) {
        VITREO_TRACE(
            "[kernel32_ext] UpdateProcThreadAttribute(attribute=0x%llx) "
            "-> FALSE (unsupported attribute)\n",
            static_cast<unsigned long long>(attribute));
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }

    ProcThreadAttributeListState updated;
    {
        std::lock_guard<std::mutex> lock(m_proc_thread_attribute_mutex);
        const auto found = m_proc_thread_attribute_lists.find(list);
        if (found == m_proc_thread_attribute_lists.end()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (found->second.attributes.contains(attribute) ||
            found->second.attribute_count >= found->second.capacity) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        updated = found->second;
    }

    if (attribute == PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE) {
        // The value is one HPCON by value. Rejecting this attribute is what
        // makes a ConPTY front-end (node-pty, Windows Terminal) fail to launch
        // its child at all, so it has to be accepted and carried to
        // CreateProcess rather than treated as unsupported.
        if (value_size != handle_size) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        // Unlike pointer-valued attributes, lpValue is the HPCON itself.
        // Microsoft Learn: Creating a Pseudoconsole session, preparing the
        // child process (UpdateProcThreadAttribute(..., hpc, sizeof(hpc), ...)).
        const u64 raw = value_ptr;
        updated.pseudo_console = raw;
        updated.attributes.insert(attribute);
        updated.attribute_count += 1;
        {
            std::lock_guard<std::mutex> lock(m_proc_thread_attribute_mutex);
            const auto found = m_proc_thread_attribute_lists.find(list);
            if (found == m_proc_thread_attribute_lists.end()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            found->second = updated;
        }
        VITREO_TRACE("[conpty] UpdateProcThreadAttribute(PSEUDOCONSOLE "
                     "HPCON=0x%llx)\n", static_cast<unsigned long long>(raw));
        m.set_return(1);
        return;
    }

    if (attribute == PROC_THREAD_ATTRIBUTE_HANDLE_LIST ||
        attribute == PROC_THREAD_ATTRIBUTE_JOB_LIST) {
        if (value_size % handle_size != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        std::vector<Handle> handles;
        handles.reserve(static_cast<usize>(value_size / handle_size));
        for (u64 offset = 0; offset < value_size; offset += handle_size) {
            u64 raw_handle = 0;
            if (m.cpu_is_64bit()) {
                auto value = m_memory.read_u64(value_ptr + offset);
                if (value.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                raw_handle = value.value();
            } else {
                auto value = m_memory.read_u32(value_ptr + offset);
                if (value.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                raw_handle = value.value();
            }
            const Handle handle = static_cast<Handle>(raw_handle);
            if (raw_handle != handle || handle == CURRENT_PROCESS_PSEUDO ||
                handle == CURRENT_THREAD_PSEUDO || !m_handle_table ||
                !m_handle_table->is_valid(handle)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            if (attribute == PROC_THREAD_ATTRIBUTE_HANDLE_LIST) {
                if ((m_handle_table->get_flags(handle) &
                     HandleFlags::Inherit) == 0) {
                    VITREO_TRACE(
                        "[kernel32_ext] UpdateProcThreadAttribute("
                        "handle=0x%llx) -> FALSE (non-inheritable handle)\n",
                        static_cast<unsigned long long>(raw_handle));
                    set_last_error(error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
            } else {
                std::lock_guard lock(m_job_mutex);
                if (m_handle_table->get_type(handle) != HandleType::Job ||
                    !m_jobs.contains(handle)) {
                    set_last_error(error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
            }
            handles.push_back(handle);
        }
        if (attribute == PROC_THREAD_ATTRIBUTE_HANDLE_LIST) {
            for (const Handle h : handles) {
                ipc::ipc_log(
                    "HANDLE_LIST entry h=0x%x type=%s flags=0x%x",
                    static_cast<unsigned>(h),
                    m_handle_table
                        ? handle_type_to_string(m_handle_table->get_type(h))
                        : "?",
                    m_handle_table ? m_handle_table->get_flags(h) : 0u);
            }
            updated.inherited_handles = std::move(handles);
        } else {
            updated.job_handles = std::move(handles);
        }
    } else if (attribute == PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY) {
        if (value_size != sizeof(u32) && value_size != sizeof(u64) &&
            value_size != sizeof(u64) * 2) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        std::array<u8, sizeof(u64) * 2> raw{};
        if (m_memory.read_bytes(value_ptr, raw.data(),
                                static_cast<usize>(value_size)).is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        std::memcpy(updated.mitigation_policy.data(), raw.data(), raw.size());
        updated.mitigation_policy_size = static_cast<u32>(value_size);
    } else if (attribute == PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY) {
        if (value_size != sizeof(u32)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        auto policy = m_memory.read_u32(value_ptr);
        if (policy.is_err() || policy.value() == 0 ||
            (policy.value() &
             ~(PROCESS_CREATION_CHILD_PROCESS_RESTRICTED |
               PROCESS_CREATION_CHILD_PROCESS_OVERRIDE)) != 0 ||
            policy.value() ==
                (PROCESS_CREATION_CHILD_PROCESS_RESTRICTED |
                 PROCESS_CREATION_CHILD_PROCESS_OVERRIDE)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        updated.child_process_policy = policy.value();
    } else {
        VITREO_TRACE(
            "[kernel32_ext] UpdateProcThreadAttribute(attribute=0x%llx) "
            "-> FALSE (unsupported attribute)\n",
            static_cast<unsigned long long>(attribute));
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }

    updated.attributes.insert(attribute);
    ++updated.attribute_count;
    {
        std::lock_guard<std::mutex> lock(m_proc_thread_attribute_mutex);
        const auto found = m_proc_thread_attribute_lists.find(list);
        if (found == m_proc_thread_attribute_lists.end() ||
            found->second.attributes.contains(attribute) ||
            found->second.attribute_count >= found->second.capacity) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        found->second = std::move(updated);
    }
    ipc::ipc_log(
        "UpdateProcThreadAttribute attribute=0x%llx -> TRUE",
        static_cast<unsigned long long>(attribute));
    m.set_return(1);
}

void Kernel32ExtManager::impl_DeleteProcThreadAttributeList(Marshaler& m) {
    const GuestAddr list = m.arg_ptr(0);
    std::lock_guard<std::mutex> lock(m_proc_thread_attribute_mutex);
    m_proc_thread_attribute_lists.erase(list);
    m.set_return(0);
}

} // namespace vitreo::winapi
