#pragma once

/// @file timer_scheduler.hpp
/// @brief Multimedia timer scheduling for timeSetEvent / timeKillEvent.
///
/// One scheduler thread services every timer from a monotonic deadline queue.
/// Windows uses a shared timer service rather than a thread per timer, and so
/// does this: applications that create a timer per decoded stream would
/// otherwise cost a host thread each.
///
/// Callbacks are dispatched through an injected invoker rather than called
/// directly, because a TIMEPROC is guest code and needs a guest thread context
/// (CpuState, stack, TEB) that only the run loop knows how to build. The
/// scheduler owns *when* a timer fires; the host owns *how* guest code runs.

#include <vitreo/common.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace vitreo::winapi {

/// fuEvent bits accepted by timeSetEvent.
enum : u32 {
    TIME_ONESHOT             = 0x0000,
    TIME_PERIODIC            = 0x0001,
    TIME_CALLBACK_FUNCTION   = 0x0000,
    TIME_CALLBACK_EVENT_SET  = 0x0010,
    TIME_CALLBACK_EVENT_PULSE = 0x0020,
    TIME_CALLBACK_TYPE_MASK  = 0x0030,
    TIME_KILL_SYNCHRONOUS    = 0x0100,
};

class MultimediaTimerScheduler {
public:
    /// Runs a guest TIMEPROC: void CALLBACK(UINT id, UINT, DWORD_PTR user,
    /// DWORD_PTR, DWORD_PTR). Called on the scheduler thread.
    using ProcInvoker =
        std::function<void(GuestAddr proc, u32 timer_id, u64 user_data)>;

    /// Signals an event handle for TIME_CALLBACK_EVENT_SET / _PULSE, where
    /// lpTimeProc carries a HANDLE rather than a function pointer.
    using EventSignaler = std::function<void(u32 handle, bool pulse)>;

    MultimediaTimerScheduler() = default;
    ~MultimediaTimerScheduler();

    MultimediaTimerScheduler(const MultimediaTimerScheduler&) = delete;
    MultimediaTimerScheduler& operator=(const MultimediaTimerScheduler&) = delete;

    void set_proc_invoker(ProcInvoker invoker);
    void set_event_signaler(EventSignaler signaler);

    /// Returns a non-zero timer ID, or 0 when the request cannot be honoured.
    /// Learn timeSetEvent: uDelay outside timeGetDevCaps min/max is an error
    /// (NULL), not a clamped period.
    [[nodiscard]] u32 create(u32 delay, u32 resolution, GuestAddr callback,
                             u64 user_data, u32 flags);

    /// Cancels a timer. False when the ID was never issued or already killed.
    /// On return no further callback for this ID will be dispatched. When
    /// called from another thread, any callback already running for it has
    /// finished ONLY for a TIME_KILL_SYNCHRONOUS timer (Learn timeSetEvent);
    /// other timers return without waiting. Learn TimeProc may call timeKillEvent
    /// from the callback itself; that path must not wait for the current
    /// invocation.
    [[nodiscard]] bool destroy(u32 timer_id);

    /// Stops the scheduler thread and drops all timers. Idempotent.
    void shutdown();

    /// Test observability: timers currently scheduled.
    [[nodiscard]] usize active_count() const;

private:
    using Clock = std::chrono::steady_clock;

    struct Timer {
        u32 id{0};
        u32 period_ms{0};
        GuestAddr callback{0};
        u64 user_data{0};  ///< DWORD_PTR dwUser: full pointer width on x64
        u32 flags{0};
        Clock::time_point deadline{};
        [[nodiscard]] bool periodic() const noexcept {
            return (flags & TIME_PERIODIC) != 0;
        }
    };

    void ensure_thread_locked();
    void run();
    void dispatch(const Timer& timer);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    /// Signalled when a callback finishes, so destroy() can wait it out.
    std::condition_variable m_callback_done;

    std::map<u32, Timer> m_timers;
    u32 m_next_id{1};
    u32 m_firing_id{0};       // timer whose callback is running, 0 if none
    u32 m_firing_flags{0};    // fuEvent of that timer
    bool m_running{false};
    bool m_stopping{false};
    std::thread m_thread;

    ProcInvoker m_proc_invoker;
    EventSignaler m_event_signaler;
};

} // namespace vitreo::winapi
