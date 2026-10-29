// SPDX-License-Identifier: LGPL-2.1-only

/// @file message_loop.cpp
/// @brief GetMessageW/PeekMessageW/DispatchMessageW/TranslateMessage (WIN32-053)

#include "message_loop.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_handle.hpp"
#include <cstdlib>
#include <chrono>
#include <cstdio>    // fprintf
#include <pthread.h>
#include "core/sync/ntsync.hpp"
#include <thread>    // this_thread::sleep_for
#include <unistd.h>
#include <vector>

/// @file msgwaitformultipleobjects.cpp
/// @brief MsgWaitForMultipleObjects
///
/// Closed cut from message_loop.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// MsgWaitForMultipleObjects
// -----------------------------------------------------------------------------

u32 MessageLoopManager::msg_wait_for_multiple_objects(const u32* handles,
                                                      u32 n_count,
                                                      bool wait_all,
                                                      u32 timeout_ms,
                                                      u32 wake_mask,
                                                      bool alertable,
                                                      bool input_available) {
    if (n_count != 0 && (!handles || !m_sync_mgr)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    // MWMO_ALERTABLE (Learn): pending QueueUserAPC ends the wait with
    // WAIT_IO_COMPLETION after the APC functions run.
    if (alertable && m_sync_mgr && m_sync_mgr->try_drain_alertable_apcs()) {
        return WAIT_IO_COMPLETION;
    }

    const u32 current_tid = ThreadManager::get_current_thread_id_static();

    // Learn: MsgWaitForMultipleObjectsEx wakes on input matching dwWakeMask
    // for the calling thread's queue, on any thread that owns windows or
    // receives thread messages — not only the primary thread. A QS_ALLINPUT
    // wait must observe posted thread messages. Object waits remain below
    // via ntsync, and AppKit pumping stays on the primary thread only.
    if (current_tid == m_queue.primary_thread_id()) {
        m_bridge.pump_events(m_queue);
    }

    const auto message_ready = [&]() -> bool {
        // wake_mask 0 still allows MsgWait to return for system/queue events
        // on Windows. Vitreo does not yet synthesize those internal events;
        // queued input must match the caller's requested QS_* classes.
        if (wake_mask == 0) {
            return false;
        }
        // Learn: without MWMO_INPUTAVAILABLE, already-seen unread input does
        // not satisfy the wait (user32_windows_probe mwmo_seen_input_*).
        const bool queued = input_available
            ? m_queue.has_input_for_thread(current_tid, m_hwnd_mgr, wake_mask)
            : m_queue.has_new_input_for_thread(current_tid, m_hwnd_mgr,
                                              wake_mask);
        if (queued) {
            return true;
        }
        // WM_PAINT and WM_TIMER are synthesized on retrieval, not stored in
        // MessageQueue. Their readiness must still satisfy QS_PAINT/QS_TIMER
        // while a caller waits; otherwise an infinite message wait sleeps
        // past a timer deadline until unrelated input arrives. Observe only:
        // waiting must neither validate the update region nor consume a timer.
        if ((wake_mask & windowing::QS::PAINT) && m_paint_mgr &&
            m_paint_mgr->find_window_needing_paint(current_tid, 0) != 0) {
            return true;
        }
        if ((wake_mask & windowing::QS::TIMER) && m_user32_ext_mgr) {
            windowing::HWND hwnd = 0;
            u64 timer_id = 0;
            GuestAddr callback = 0;
            return m_user32_ext_mgr->find_expired_timer(
                hwnd, timer_id, callback, current_tid, false);
        }
        return false;
    };

    // VITREO_MOUSE_DIAG: while a thread is parked here, record once a second
    // what it is waiting on and whether the queue says a message is ready.
    // A thread that stays parked while input is being posted to it is the
    // shape of the tooltip freeze, and VITREO_TRACE is too slow to catch it.
    static std::FILE* const wait_diag = [] () -> std::FILE* {
        const char* path = std::getenv("VITREO_MOUSE_DIAG");
        if (path == nullptr || *path == '\0') return nullptr;
        return std::fopen((std::string(path) + ".wait").c_str(), "w");
    }();
    auto last_diag = std::chrono::steady_clock::now();

    const auto check_ready = [&]() -> std::optional<u32> {
        if (alertable && m_sync_mgr && m_sync_mgr->try_drain_alertable_apcs()) {
            return WAIT_IO_COMPLETION;
        }
        const bool msg_ready = message_ready();
        if (wait_diag != nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_diag >= std::chrono::seconds(1)) {
                last_diag = now;
                u32 head_hwnd = 0, head_msg = 0, head_tid = 0;
                int head_owner = -1;
                const usize depth = m_queue.diag_head(
                    m_hwnd_mgr, head_hwnd, head_msg, head_tid, head_owner);
                std::fprintf(wait_diag,
                             "tid=%u mask=%x n=%u msg_ready=%d has_input=%d "
                             "depth=%zu head{hwnd=%x msg=%x tgt=%u owner=%d}\n",
                             current_tid, wake_mask, n_count,
                             msg_ready ? 1 : 0,
                             m_queue.has_input_for_thread(
                                 current_tid, m_hwnd_mgr, 0xFFFFFFFFu) ? 1 : 0,
                             depth, head_hwnd, head_msg, head_tid, head_owner);
                std::fflush(wait_diag);
            }
        }

        if (n_count == 0) {
            if (msg_ready) return WAIT_OBJECT_0;
            return std::nullopt;
        }

        if (wait_all) {
            // MWMO_WAITALL: every handle signaled AND a queued message.
            // Only touch handles once a message is present so auto-reset
            // events are not consumed and then discarded.
            if (!msg_ready) return std::nullopt;
            const u32 result = m_sync_mgr->wait_for_multiple_objects(
                n_count, handles, true, 0);
            if (result == WAIT_FAILED) return result;
            // Learn: WAIT_ABANDONED_0..+n-1 still means every object was
            // acquired (ownership granted); must not discard and re-wait.
            if (result == WAIT_OBJECT_0 ||
                (result >= WAIT_ABANDONED &&
                 result < WAIT_ABANDONED + n_count)) {
                return result;
            }
            return std::nullopt;
        }

        const u32 result = m_sync_mgr->wait_for_multiple_objects(
            n_count, handles, false, 0);
        if (result != WAIT_TIMEOUT) return result;
        if (msg_ready) return WAIT_OBJECT_0 + n_count;
        return std::nullopt;
    };

    if (auto result = check_ready()) return *result;
    if (timeout_ms == 0) return WAIT_TIMEOUT;

    const bool infinite = (timeout_ms == 0xFFFFFFFFu);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(infinite ? 100000 : timeout_ms);
    constexpr auto kPumpInterval = std::chrono::milliseconds(16);
    auto last_pump = std::chrono::steady_clock::now();

    // Every wake source this wait has is event-driven: a handle signal wakes
    // the ntsync park (and, on the AppKit thread, the run loop); a queue post
    // wakes the run loop and the per-thread message event registered below;
    // native input wakes nextEvent. The only thing that has to be timed is the
    // next due WM_TIMER, so the park lasts until then (capped to bound a wake
    // the loop could still miss, e.g. a paint invalidated from another
    // thread), not a fixed 16 ms slice.
    constexpr u32 kParkCapMs = 100;
    static const bool s_cap_off = sync_mechanism_off("parkcap");
    const auto park_budget_ms = [&](u32 remaining_ms) -> u32 {
        const u32 cap = s_cap_off ? 16u : kParkCapMs;
        u32 ms = remaining_ms < cap ? remaining_ms : cap;
        if ((wake_mask & windowing::QS::TIMER) && m_user32_ext_mgr) {
            const u32 due = m_user32_ext_mgr->next_timer_due_ms(current_tid);
            if (due < ms) ms = due;
        }
        return ms;
    };

    // Alertable waits park on the thread's APC ntsync event (same object
    // note_apc_queued signals). Message posts poke that same event so one
    // extra wait slot covers both APC and queue wakes.
    const u64 apc_ev = (alertable && m_sync_mgr)
                           ? m_sync_mgr->apc_wait_object_for_current_thread()
                           : 0;

    // A worker MsgWait parks on its handles in ntsync, which a PostMessage
    // cannot see. Give the thread an auto-reset message event, waited on
    // beside the handles, that every queue post signals. A stale signal from
    // an earlier wait only costs one extra readiness check.
    static const bool s_msgwake_off = sync_mechanism_off("msgwake");
    const bool worker_handle_wait =
        (n_count > 0 && !wait_all && pthread_main_np() == 0 && !s_msgwake_off);
    static thread_local ntsync::ObjectId t_msg_event = 0;
    struct WakeRegistration {
        windowing::MessageQueue* queue = nullptr;
        u32 tid = 0;
        ~WakeRegistration() { if (queue) queue->unregister_thread_wake(tid); }
    } wake_registration;
    if (worker_handle_wait || apc_ev != 0) {
        ntsync::ObjectId ev = apc_ev;
        if (ev == 0) {
            if (t_msg_event == 0) {
                t_msg_event = ntsync::global().create_event(
                    /*manual_reset=*/false, /*initial_state=*/false);
            }
            ev = t_msg_event;
        }
        m_queue.register_thread_wake(current_tid, [ev]() {
            (void)ntsync::global().set_event(ev);
        });
        wake_registration.queue = &m_queue;
        wake_registration.tid = current_tid;
        // A post that landed between the readiness check above and the
        // registration would otherwise be missed until the next wake.
        if (auto result = check_ready()) return *result;
    }

    const bool on_main = pthread_main_np() != 0;
    struct ParkArm {
        SyncManager* sync; bool on;
        ~ParkArm() { if (on && sync) sync->disarm_gui_wake(); }
    } park_arm{m_sync_mgr, on_main};

    for (;;) {
        if (m_queue.shutdown_requested()) return WAIT_FAILED;
        // Arm the wake for exactly these handles before this iteration's
        // readiness check, so a signal or post landing after the check is
        // delivered as an event and nothing else wakes the park.
        if (on_main && m_sync_mgr) m_sync_mgr->arm_gui_wake(n_count, handles);
        if (auto result = check_ready()) return *result;

        auto now = std::chrono::steady_clock::now();
        if (!infinite && now >= deadline) return WAIT_TIMEOUT;

        u32 remaining_ms = 0xFFFFFFFFu;
        if (!infinite) {
            const auto rem =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now)
                    .count();
            if (rem <= 0) return WAIT_TIMEOUT;
            remaining_ms = static_cast<u32>(rem);
        }
        u32 slice_ms = park_budget_ms(remaining_ms);

        if (n_count > 0 && !wait_all) {
            // The host main thread cannot sit in pthread_cond_wait: AppKit
            // only delivers mouse/keyboard while CFRunLoop is running.
            // wait_events parks in nextEvent; ntsync set_host_wake and the
            // queue's host wake poke that runloop.
            if (pthread_main_np() != 0) {
                m_bridge.wait_events(m_queue, slice_ms);
                m_bridge.pump_events(m_queue);
            } else {
                const u64 extra = apc_ev != 0 ? apc_ev
                                  : (worker_handle_wait ? t_msg_event : 0);
                const u32 result = m_sync_mgr->wait_for_multiple_objects(
                    n_count, handles, false, slice_ms, extra);
                if (result == WAIT_OBJECT_0 + n_count) {
                    // Extra slot: APC and/or message poke — check_ready
                    // distinguishes WAIT_IO_COMPLETION from QS_* input.
                } else if (result != WAIT_TIMEOUT) {
                    return result;
                }
            }
        } else if (apc_ev != 0 && m_sync_mgr && pthread_main_np() == 0) {
            // nCount==0 / wait_all + alertable: park on the APC event so a
            // QueueUserAPC ends the wait without a busy poll. Message posts
            // poke the same event via register_thread_wake above.
            const ntsync::ObjectId id = apc_ev;
            (void)ntsync::global().wait(
                &id, 1, /*wait_all=*/false, slice_ms, current_tid, nullptr);
        } else {
            // wait_all: a handle becoming signaled does not wake this park;
            // keep the short re-check for that rarely used form.
            if (wait_all && slice_ms > 16) slice_ms = 16;
            // nCount==0 or wait_all: wait for a posted message (and for
            // wait_all, then 0-timeout handle check via check_ready).
            if (pthread_main_np() != 0) {
                m_bridge.wait_events(m_queue, slice_ms);
                m_bridge.pump_events(m_queue);
            } else {
                m_queue.wait_for_input_for_thread(
                    std::chrono::milliseconds(slice_ms), current_tid,
                    m_hwnd_mgr, wake_mask);
            }
        }

        if (auto result = check_ready()) return *result;

        now = std::chrono::steady_clock::now();
        if (!infinite && now >= deadline) return WAIT_TIMEOUT;

        if (current_tid == m_queue.primary_thread_id() && now - last_pump >= kPumpInterval) {
            m_bridge.pump_events(m_queue);
            last_pump = now;
            if (auto result = check_ready()) return *result;
        }
    }
}

u32 MessageLoopManager::wait_for_input_idle(u64 process_handle, u32 timeout_ms) {
    // user32_windows_probe wfii_current_process: GetCurrentProcess() →
    // WAIT_FAILED and leaves last-error untouched.
    if (process_handle == 0xFFFFFFFFFFFFFFFFull ||
        process_handle == 0xFFFFFFFFull) {
        return WAIT_FAILED;
    }
    if (!m_sync_mgr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    const Handle h = static_cast<Handle>(process_handle);
    u32 pid = 0;
    if (!m_sync_mgr->process_pid(h, pid)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    ipc::ProcessObject* po = ipc::find_tracked_process(static_cast<pid_t>(pid));
    if (po == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    // Learn: subsequent WaitForInputIdle calls return immediately.
    if (po->input_idle_latched.load(std::memory_order_acquire)) {
        return 0;
    }
    const auto process_idle = [&]() -> bool {
        if (po->exited.load(std::memory_order_acquire)) return false;
        for (const auto& entry : ipc::hwnd_registry_enumerate()) {
            if (entry.owner_host_pid == pid && entry.parent_hwnd == 0) {
                return true;
            }
        }
        return false;
    };
    if (process_idle()) {
        po->input_idle_latched.store(true, std::memory_order_release);
        return 0;
    }
    if (timeout_ms == 0) return WAIT_TIMEOUT;

    const bool infinite = timeout_ms == 0xFFFFFFFFu;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(infinite ? 0 : timeout_ms);
    for (;;) {
        if (po->exited.load(std::memory_order_acquire)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        if (process_idle()) {
            po->input_idle_latched.store(true, std::memory_order_release);
            return 0;
        }
        if (!infinite && std::chrono::steady_clock::now() >= deadline) {
            return WAIT_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static bool read_wait_handles(Marshaler& m, u32 count, GuestAddr handles_ptr,
                              std::vector<u32>& handles) {
    constexpr u32 MAXIMUM_WAIT_OBJECTS = 64;
    if (count >= MAXIMUM_WAIT_OBJECTS || (count != 0 && handles_ptr == 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    handles.reserve(count);
    const usize stride = m.cpu_is_64bit() ? sizeof(u64) : sizeof(u32);
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr address = handles_ptr + static_cast<GuestAddr>(i) * stride;
        if (m.cpu_is_64bit()) {
            auto value = m.read_u64(address);
            if (value.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            handles.push_back(static_cast<u32>(value.value()));
        } else {
            auto value = m.read_u32(address);
            if (value.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            handles.push_back(value.value());
        }
    }
    return true;
}

void impl_MsgWaitForMultipleObjects(Marshaler& m, MessageLoopManager& mlm) {
    auto n_count    = static_cast<u32>(m.arg(0));
    const GuestAddr handles_ptr = m.arg_ptr(1);
    const bool wait_all = m.arg(2) != 0;
    auto timeout_ms = static_cast<u32>(m.arg(3));
    auto wake_mask  = static_cast<u32>(m.arg(4));

    std::vector<u32> handles;
    if (!read_wait_handles(m, n_count, handles_ptr, handles)) {
        m.set_return(0xFFFFFFFFu);
        return;
    }

    VITREO_TRACE( "[msgloop] MsgWaitForMultipleObjects: nCount=%u timeout=%u wakeMask=0x%x\n",
                 n_count, timeout_ms, wake_mask);

    u32 result = mlm.msg_wait_for_multiple_objects(
        handles.data(), n_count, wait_all, timeout_ms, wake_mask);

    VITREO_TRACE( "[msgloop] MsgWaitForMultipleObjects -> 0x%x\n", result);
    m.set_return(result);
}

void impl_MsgWaitForMultipleObjectsEx(Marshaler& m, MessageLoopManager& mlm) {
    const u32 n_count = static_cast<u32>(m.arg(0));
    const GuestAddr handles_ptr = m.arg_ptr(1);
    const u32 timeout_ms = static_cast<u32>(m.arg(2));
    const u32 wake_mask = static_cast<u32>(m.arg(3));
    // DWORD dwFlags sits in the x64 stack slot: low dword only.
    const u32 flags = m.arg32(4);
    constexpr u32 MWMO_WAITALL = 0x0001;
    constexpr u32 MWMO_ALERTABLE = 0x0002;
    constexpr u32 MWMO_INPUTAVAILABLE = 0x0004;
    constexpr u32 VALID_FLAGS = 0x0007;

    if ((flags & ~VALID_FLAGS) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }

    std::vector<u32> handles;
    if (!read_wait_handles(m, n_count, handles_ptr, handles)) {
        m.set_return(0xFFFFFFFFu);
        return;
    }

    const u32 result = mlm.msg_wait_for_multiple_objects(
        handles.data(), n_count, (flags & MWMO_WAITALL) != 0,
        timeout_ms, wake_mask, (flags & MWMO_ALERTABLE) != 0,
        (flags & MWMO_INPUTAVAILABLE) != 0);
    if (windowing::diag::msg_pump_diag()) {
        std::fprintf(stderr,
                     "[msgpump-diag] MsgWaitEx tid=%u n=%u timeout=%u "
                     "wake=0x%x flags=0x%x -> 0x%x\n",
                     ThreadManager::get_current_thread_id_static(),
                     n_count, timeout_ms, wake_mask, flags, result);
        std::fflush(stderr);
    }
    m.set_return(result);
}

} // namespace vitreo::winapi
