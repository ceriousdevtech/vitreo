// SPDX-License-Identifier: LGPL-2.1-only

/// @file register_wait.cpp
/// @brief SList pop and RegisterWaitForSingleObject / UnregisterWait*.
///
/// Additional Synchronization banner: InterlockedPopEntrySList and
/// the RegisterWait family.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
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

#include "winapi/kernel32/synchronization/slist.hpp"

namespace vitreo::winapi {

// =============================================================================
// Additional Synchronization, Thread, and Proc-Thread-Attribute-List APIs
// =============================================================================

void Kernel32ExtManager::impl_InterlockedPopEntrySList(Marshaler& m) {
    // PSLIST_ENTRY InterlockedPopEntrySList(PSLIST_HEADER ListHead)
    const GuestAddr head = m.arg_ptr(0);
    m.set_return(head == 0 ? 0 : slist::pop(m_memory, head, m.cpu_is_64bit()));
}


void Kernel32ExtManager::impl_RegisterWaitForSingleObject(Marshaler& m) {
    GuestAddr phNew = m.arg_ptr(0);
    Handle wait_object = static_cast<Handle>(m.arg(1));
    GuestAddr callback = m.arg_ptr(2);
    GuestAddr context = m.arg_ptr(3);
    u32 milliseconds = static_cast<u32>(m.arg(4));
    u32 flags = static_cast<u32>(m.arg(5));

    if (!m_sync_manager || !m_thread_manager || callback == 0 || wait_object == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    u32 wait_handle = 0xFF000000u + (++m_event_counter);
    auto state = std::make_shared<RegisteredWaitState>();
    {
        std::lock_guard<std::mutex> lock(m_registered_wait_mutex);
        m_registered_waits[wait_handle] = state;
    }

    if (phNew != 0) {
        const auto write_result = m.cpu_is_64bit()
            ? m_memory.write_u64(phNew, wait_handle)
            : m_memory.write_u32(phNew, wait_handle);
        if (write_result.is_err()) {
            std::lock_guard lock(m_registered_wait_mutex);
            m_registered_waits.erase(wait_handle);
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
    }

    VITREO_TRACE("[kernel32_ext] RegisterWaitForSingleObject(obj=0x%x, cb=0x%llx, ctx=0x%llx, ms=%u, flags=0x%x) -> 0x%x\n",
                 wait_object,
                 static_cast<unsigned long long>(callback),
                 static_cast<unsigned long long>(context),
                 milliseconds,
                 flags,
                 wait_handle);
    ipc::ipc_log(
        "RegisterWaitForSingleObject obj=0x%x cb=0x%llx ctx=0x%llx ms=%u "
        "flags=0x%x -> wait=0x%x",
        wait_object,
        static_cast<unsigned long long>(callback),
        static_cast<unsigned long long>(context),
        milliseconds, flags, wait_handle);

    SyncManager* sync_manager = m_sync_manager;
    ThreadManager* thread_manager = m_thread_manager;
    std::thread worker(
        [this, sync_manager, thread_manager, wait_object, callback, context,
         milliseconds, flags, wait_handle, state]() {
        constexpr u32 WT_EXECUTEINWAITTHREAD = 0x00000004u;
        constexpr u32 WT_EXECUTEONLYONCE = 0x00000008u;
        // Host wait threads default TLS guest tid to 1 (main/UI). That makes
        // SyncManager treat RegisterWait as the GUI thread and invoke the
        // AppKit wait-pump from a background pthread — and it also collides
        // ntsync waiter identity with the real main thread. Use a stable
        // high tid that is never a real guest thread id.
        const u32 waiter_tid = 0xF0000000u | (wait_handle & 0x0FFFFFFFu);
        ThreadManager::set_current_thread_id(waiter_tid);
        // It runs the guest's WAITORTIMERCALLBACK: a thread of this process.
        ThreadManager::ServiceThreadScope service_thread(thread_manager, waiter_tid);

        while (!state->cancelled.load(std::memory_order_acquire) &&
               !m_shutdown.load(std::memory_order_acquire)) {
            const u32 ret = sync_manager->wait_for_single_object_cancellable(
                wait_object, milliseconds, state->cancelled);

            if (state->cancelled.load(std::memory_order_acquire) ||
                m_shutdown.load(std::memory_order_acquire)) {
                break;
            }
            if (ret != WAIT_OBJECT_0 && ret != WAIT_TIMEOUT) {
                // DuplicateHandle may promote a local event to
                // SharedSyncObject and destroy the ntsync id under a parked
                // waiter (WAIT_FAILED). If the HANDLE is still waitable,
                // re-enter the wait on the new backing object instead of
                // permanently dropping the ObjectWatcher.
                if (sync_manager->is_waitable_sync_handle(wait_object)) {
                    continue;
                }
                ipc::ipc_log(
                    "RegisterWait wait failed wait=0x%x obj=0x%x ret=%u "
                    "waiter_tid=0x%x",
                    wait_handle, wait_object, ret, waiter_tid);
                break;
            }

            // Publish the callback under the state lock, re-checking the
            // cancellation, so UnregisterWait sees exactly the callbacks that
            // will run (no conservative ERROR_IO_PENDING needed).
            const u64 timer_or_wait_fired = (ret == WAIT_TIMEOUT) ? 1u : 0u;
            const bool in_wait_thread = (flags & WT_EXECUTEINWAITTHREAD) != 0;
            u32 tid = 0;
            Handle thread_handle = INVALID_HANDLE;
            {
                std::lock_guard lock(state->mutex);
                if (state->cancelled.load(std::memory_order_acquire)) break;
                (void)reap_callback_threads(state->callback_threads);
                thread_handle = thread_manager->create_thread(
                    0, callback, context, 0, &tid, timer_or_wait_fired);
                if (thread_handle == INVALID_HANDLE || thread_handle == 0) {
                    ipc::ipc_log(
                        "RegisterWait create_thread failed wait=0x%x obj=0x%x",
                        wait_handle, wait_object);
                    break;
                }
                if (in_wait_thread) {
                    state->inline_running = true;
                } else {
                    state->callback_threads.push_back(thread_handle);
                }
            }
            if (auto* gt = thread_manager->get_thread(tid)) gt->launched = true;
            thread_manager->run_async(tid, callback, context, timer_or_wait_fired, 0, 2);
            if (in_wait_thread) {
                // WT_EXECUTEINWAITTHREAD: the wait thread runs the callback,
                // so the next wait starts only after it returns
                // (rw_inwaitthread_serialised during=1 after=2).
                (void)sync_manager->wait_for_single_object(thread_handle, INFINITE_WAIT);
                thread_manager->close_thread_handle(thread_handle);
                std::lock_guard lock(state->mutex);
                state->inline_running = false;
                state->cv.notify_all();
            }
            if ((flags & WT_EXECUTEONLYONCE) != 0) break;
        }

        // Windows keeps the registration until UnregisterWait(Ex), even
        // after a WT_EXECUTEONLYONCE callback.
        std::vector<Handle> callback_threads;
        Handle completion_event = NULL_HANDLE;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] {
                return state->cancelled.load(std::memory_order_acquire) ||
                       m_shutdown.load(std::memory_order_acquire);
            });
            callback_threads = std::move(state->callback_threads);
            completion_event = state->completion_event;
        }
        for (const Handle callback_thread : callback_threads) {
            if (!m_shutdown.load(std::memory_order_acquire)) {
                (void)sync_manager->wait_for_single_object(
                    callback_thread, INFINITE_WAIT);
            }
            thread_manager->close_thread_handle(callback_thread);
        }

        if (!m_shutdown.load(std::memory_order_acquire) &&
            completion_event != NULL_HANDLE &&
            completion_event != INVALID_HANDLE) {
            (void)sync_manager->set_event(
                completion_event,
                SyncTraceContext{"UnregisterWaitExCompletion", 0});
        }

        {
            std::lock_guard lock(state->mutex);
            state->finalized = true;
            state->cv.notify_all();
        }
        thread_manager->note_host_thread_done();
    });
    thread_manager->track_host_thread(std::move(worker));

    m.set_return(1);
}

bool Kernel32ExtManager::unregister_wait_state(
    const std::shared_ptr<RegisteredWaitState>& state, Handle completion) {
    bool running = false;
    {
        std::lock_guard lock(state->mutex);
        if (completion != INVALID_HANDLE) state->completion_event = completion;
        state->cancelled.store(true, std::memory_order_release);
        running = state->inline_running ||
                  reap_callback_threads(state->callback_threads);
    }
    state->cv.notify_all();
    m_sync_manager->notify_waiters();
    if (completion == INVALID_HANDLE) {
        // Blocking form: wait for running callbacks (rw_unregisterex_blocking).
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] {
            return state->finalized || m_shutdown.load(std::memory_order_acquire);
        });
        return true;
    }
    return !running;
}

void Kernel32ExtManager::impl_UnregisterWait(Marshaler& m) {
    // BOOL UnregisterWait(HANDLE WaitHandle): non-blocking. FALSE with
    // ERROR_IO_PENDING only while a callback runs; an idle registration,
    // or one whose WT_EXECUTEONLYONCE callback already returned, is TRUE
    // (rw_unregister_idle / _after_fired_once / _while_running).
    const u32 h = static_cast<u32>(m.arg(0));
    std::shared_ptr<RegisteredWaitState> state;
    {
        std::lock_guard<std::mutex> lock(m_registered_wait_mutex);
        auto it = m_registered_waits.find(h);
        if (it != m_registered_waits.end()) {
            state = it->second;
            m_registered_waits.erase(it);
        }
    }
    if (!state) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (!unregister_wait_state(state, NULL_HANDLE)) {
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_UnregisterWaitEx(Marshaler& m) {
    // BOOL UnregisterWaitEx(HANDLE WaitHandle, HANDLE CompletionEvent)
    // INVALID_HANDLE_VALUE blocks until callbacks finish. NULL or an event
    // behave like UnregisterWait (FALSE + ERROR_IO_PENDING while a callback
    // runs); the event is set once the callbacks have returned
    // (rw_unregisterex_null_while_running, _event_while_running, _event_idle).
    const u32 h = static_cast<u32>(m.arg(0));
    const Handle completion_event = static_cast<Handle>(m.arg(1));
    if (completion_event != NULL_HANDLE &&
        completion_event != INVALID_HANDLE &&
        (!m_handle_table ||
         m_handle_table->get_type(completion_event) != HandleType::Event)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    std::shared_ptr<RegisteredWaitState> state;
    {
        std::lock_guard<std::mutex> lock(m_registered_wait_mutex);
        auto it = m_registered_waits.find(h);
        if (it != m_registered_waits.end()) {
            state = it->second;
            m_registered_waits.erase(it);
        }
    }
    if (!state) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (!unregister_wait_state(state, completion_event)) {
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
