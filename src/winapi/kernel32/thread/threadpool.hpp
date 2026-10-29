// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file threadpool.hpp
/// @brief kernel32 thread-pool objects (Vista thread pool API): work, timer
///        and wait objects, and the callback-instance functions.
///
/// A stubbed thread pool is a bad failure: `SubmitThreadpoolWork` and
/// `SetThreadpoolTimer` return void, so a guest cannot tell that its callback
/// was dropped. The work simply never runs and whatever it would have
/// completed never completes. This implementation actually runs every
/// callback, on a guest thread, with the arguments Windows passes, and keeps
/// the object alive until its callbacks return. Behaviour Learn leaves open
/// is measured by tools/oracle_probe/s4_threadpool_probe.c.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class ThreadManager;
class SyncManager;

/// One `PTP_WORK`. Windows keeps the object alive until its outstanding
/// callbacks finish, even if the caller has already closed it.
struct ThreadpoolWork {
    GuestAddr callback{0};
    GuestAddr context{0};
    /// Submitted callbacks that have not returned yet.
    u32 outstanding{0};
    /// Submissions that have not been dispatched to a thread yet.
    u32 queued{0};
    /// The caller has released its reference; free once nothing is running.
    bool closed{false};
};

/// One `PTP_TIMER`.
struct ThreadpoolTimer {
    GuestAddr callback{0};
    GuestAddr context{0};
    u32 outstanding{0};
    bool closed{false};
    /// IsThreadpoolTimerSet: true from SetThreadpoolTimer(non-NULL) until the
    /// timer is cancelled — it stays TRUE after a one-shot expires
    /// (s4_threadpool_probe tpt_oneshot isset_after=1).
    bool set{false};
    /// A future expiry is scheduled.
    bool armed{false};
    std::chrono::steady_clock::time_point due{};
    u32 period_ms{0};
};

/// One `PTP_WAIT`.
struct ThreadpoolWait {
    GuestAddr callback{0};
    GuestAddr context{0};
    u32 outstanding{0};
    bool closed{false};
    /// Bumped by every SetThreadpoolWait; a waiter whose generation is stale
    /// does not call back.
    u64 generation{0};
    /// Cancels the host waiter of the current generation.
    std::shared_ptr<std::atomic<bool>> cancel;
};

/// Implements CreateThreadpoolWork / SubmitThreadpoolWork /
/// CloseThreadpoolWork / WaitForThreadpoolWorkCallbacks /
/// TrySubmitThreadpoolCallback, the TP_TIMER and TP_WAIT families,
/// QueueUserWorkItem, and the *WhenCallbackReturns instance functions.
class ThreadpoolManager {
public:
    ThreadpoolManager() = default;
    ~ThreadpoolManager();
    ThreadpoolManager(const ThreadpoolManager&) = delete;
    ThreadpoolManager& operator=(const ThreadpoolManager&) = delete;

    void set_thread_manager(ThreadManager* threads) noexcept {
        m_threads = threads;
    }
    /// Needed by wait objects and by SetEvent/ReleaseSemaphore/ReleaseMutex
    /// WhenCallbackReturns.
    void set_sync_manager(SyncManager* sync) noexcept { m_sync = sync; }
    /// LeaveCriticalSectionWhenCallbackReturns: leaves a guest
    /// CRITICAL_SECTION as the current guest thread.
    using LeaveCriticalSection = std::function<void(GuestAddr)>;
    void set_leave_critical_section(LeaveCriticalSection fn) {
        m_leave_cs = std::move(fn);
    }

    /// Process teardown: stop the timer thread and cancel every wait.
    void begin_shutdown();

    void impl_CreateThreadpoolWork(Marshaler& m);
    void impl_SubmitThreadpoolWork(Marshaler& m);
    void impl_CloseThreadpoolWork(Marshaler& m);
    void impl_WaitForThreadpoolWorkCallbacks(Marshaler& m);
    void impl_TrySubmitThreadpoolCallback(Marshaler& m);

    void impl_CreateThreadpoolTimer(Marshaler& m);
    void impl_SetThreadpoolTimer(Marshaler& m);
    void impl_SetThreadpoolTimerEx(Marshaler& m);
    void impl_IsThreadpoolTimerSet(Marshaler& m);
    void impl_WaitForThreadpoolTimerCallbacks(Marshaler& m);
    void impl_CloseThreadpoolTimer(Marshaler& m);

    void impl_CreateThreadpoolWait(Marshaler& m);
    void impl_SetThreadpoolWait(Marshaler& m);
    void impl_SetThreadpoolWaitEx(Marshaler& m);
    void impl_WaitForThreadpoolWaitCallbacks(Marshaler& m);
    void impl_CloseThreadpoolWait(Marshaler& m);

    /// QueueUserWorkItem — the legacy pool submission. Its callback has the
    /// same shape as a thread start routine, so it takes ONE argument.
    void impl_QueueUserWorkItem(Marshaler& m);

    void impl_CallbackMayRunLong(Marshaler& m);
    void impl_SetEventWhenCallbackReturns(Marshaler& m);
    void impl_ReleaseSemaphoreWhenCallbackReturns(Marshaler& m);
    void impl_ReleaseMutexWhenCallbackReturns(Marshaler& m);
    void impl_LeaveCriticalSectionWhenCallbackReturns(Marshaler& m);
    void impl_FreeLibraryWhenCallbackReturns(Marshaler& m);
    void impl_DisassociateCurrentThreadFromCallback(Marshaler& m);

    /// Called when a guest thread finishes. Wired to
    /// ThreadManager::set_thread_exit_callback; a thread that was not running
    /// a pool callback is ignored. Runs on the finishing thread, so the
    /// *WhenCallbackReturns actions act as that thread.
    void on_thread_exit(u32 tid);

    /// Called when a callback of @p object returns (or disassociates), so
    /// WaitForThreadpool*Callbacks can observe completion and a closed
    /// object can be freed.
    void on_work_callback_finished(u32 object);

    /// Number of live work objects, for tests.
    [[nodiscard]] usize live_work_count() const;

private:
    /// Per-callback PTP_CALLBACK_INSTANCE state.
    struct CallbackInstance {
        u32 object{0};
        bool disassociated{false};
        std::vector<std::function<void()>> on_return;
    };
    struct RunningCallback {
        u32 object{0};
        u64 instance{0};
    };

    /// Launch a callback on a guest thread with a fresh callback instance:
    /// `(instance, context, third[, fourth])`. Caller holds no lock and has
    /// already counted the callback as outstanding on @p object.
    [[nodiscard]] bool dispatch(u32 object, GuestAddr callback, GuestAddr context,
                                u64 third, u64 fourth, usize arg_count);
    [[nodiscard]] bool read_environment_ok(Marshaler& m, GuestAddr environment);
    [[nodiscard]] u32 new_object_id();
    [[nodiscard]] bool add_instance_action(u64 instance, std::function<void()> fn);
    void ensure_timer_thread();
    void timer_thread_main();
    void arm_wait(u32 wait, u64 generation, Handle handle, u32 timeout_ms,
                  std::shared_ptr<std::atomic<bool>> cancel);
    [[nodiscard]] bool outstanding_zero(u32 object) const;
    void release_object_if_idle(u32 object);

    ThreadManager* m_threads{nullptr};
    SyncManager* m_sync{nullptr};
    LeaveCriticalSection m_leave_cs;

    mutable std::mutex m_mutex;
    std::condition_variable m_idle;
    std::condition_variable m_timer_cv;
    std::unordered_map<u32, ThreadpoolWork> m_work;
    std::unordered_map<u32, ThreadpoolTimer> m_timers;
    std::unordered_map<u32, ThreadpoolWait> m_waits;
    std::unordered_map<u64, CallbackInstance> m_instances;
    /// Guest thread currently running a pool callback.
    std::unordered_map<u32, RunningCallback> m_running;
    /// PTP_* values the guest only ever passes back to this module.
    u32 m_next_work{0x7B000004};
    u64 m_next_instance{0x7A000004};
    bool m_timer_thread_started{false};
    bool m_shutdown{false};
    std::thread m_timer_thread;
    /// Host threads parked in a TP_WAIT; teardown waits for them to leave.
    u32 m_live_waiters{0};
};

/// Build the thread-pool export entries and wire them to @p mgr.
void add_threadpool_exports(DllExports& exports, ThreadpoolManager& mgr);

} // namespace vitreo::winapi
