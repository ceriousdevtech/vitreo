#include "winapi/media/winmm/timer_scheduler.hpp"

#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"

#include <algorithm>
#include <vector>

namespace vitreo::winapi {

MultimediaTimerScheduler::~MultimediaTimerScheduler() { shutdown(); }

void MultimediaTimerScheduler::set_proc_invoker(ProcInvoker invoker) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_proc_invoker = std::move(invoker);
}

void MultimediaTimerScheduler::set_event_signaler(EventSignaler signaler) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_event_signaler = std::move(signaler);
}

u32 MultimediaTimerScheduler::create(u32 delay, u32 resolution,
                                     GuestAddr callback, u64 user_data,
                                     u32 flags) {
    // lpTimeProc is required for every callback type: it is a function pointer
    // for TIME_CALLBACK_FUNCTION and an event HANDLE for the EVENT variants,
    // and neither is meaningful as null.
    if (callback == 0) return 0;
    (void)resolution;  // advisory on Windows; the scheduler runs at 1 ms granularity
    // Learn: uDelay outside the advertised min/max is an error (return NULL).
    // Clamping 0→1 turns Qt PreciseTimer interval 0 into a 1 ms TIME_PERIODIC
    // TIMEPROC storm instead of failing so the caller can SetTimer.
    if (delay < TIMER_PERIOD_MIN || delay > TIMER_PERIOD_MAX) return 0;

    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_stopping) return 0;

    const u32 id = m_next_id++;
    if (m_next_id == 0) m_next_id = 1;  // never hand out 0

    Timer timer;
    timer.id = id;
    timer.period_ms = delay;
    timer.callback = callback;
    timer.user_data = user_data;
    timer.flags = flags;
    timer.deadline = Clock::now() + std::chrono::milliseconds(delay);
    m_timers.emplace(id, timer);

    perf::count(perf::Counter::mm_timer_set);
    ensure_thread_locked();
    m_wake.notify_all();
    return id;
}

bool MultimediaTimerScheduler::destroy(u32 timer_id) {
    std::unique_lock<std::mutex> lock(m_mutex);
    u32 flags = 0;
    if (auto it = m_timers.find(timer_id); it != m_timers.end()) {
        flags = it->second.flags;
    } else if (m_firing_id == timer_id) {
        flags = m_firing_flags;
    }
    const bool erased = m_timers.erase(timer_id) != 0;

    // Learn TimeProc: the callback may call timeKillEvent (and timeSetEvent).
    // Waiting for this id's callback to finish would wait for ourselves.
    const bool killing_self =
        m_firing_id == timer_id && m_thread.joinable() &&
        std::this_thread::get_id() == m_thread.get_id();
    // Only TIME_KILL_SYNCHRONOUS waits out an in-flight callback. Waiting for
    // any other timer deadlocks a caller that holds a lock its TIMEPROC also
    // takes (DestroyWithoutKillSynchronousDoesNotWait).
    const bool sync_kill = (flags & TIME_KILL_SYNCHRONOUS) != 0;
    if (!killing_self && sync_kill && m_firing_id == timer_id) {
        m_callback_done.wait(lock, [&] { return m_firing_id != timer_id; });
    }

    if (erased) perf::count(perf::Counter::mm_timer_kill);
    m_wake.notify_all();
    return erased;
}

void MultimediaTimerScheduler::shutdown() {
    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_running && !m_thread.joinable()) {
            m_timers.clear();
            return;
        }
        m_stopping = true;
        m_timers.clear();
        thread_to_join = std::move(m_thread);
    }
    m_wake.notify_all();
    if (thread_to_join.joinable()) thread_to_join.join();

    std::lock_guard<std::mutex> guard(m_mutex);
    m_running = false;
    m_stopping = false;
}

usize MultimediaTimerScheduler::active_count() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_timers.size();
}

void MultimediaTimerScheduler::ensure_thread_locked() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread([this] { run(); });
}

void MultimediaTimerScheduler::run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        if (m_stopping) return;

        if (m_timers.empty()) {
            m_wake.wait(lock);
            continue;
        }

        // Earliest deadline across all timers.
        auto next = std::min_element(
            m_timers.begin(), m_timers.end(),
            [](const auto& a, const auto& b) {
                return a.second.deadline < b.second.deadline;
            });
        const auto deadline = next->second.deadline;

        if (Clock::now() < deadline) {
            m_wake.wait_until(lock, deadline);
            continue;  // re-evaluate: the set may have changed while waiting
        }

        // Collect everything due now, so a batch of coincident timers fires in
        // one pass rather than re-scanning per timer.
        std::vector<Timer> due;
        const auto now = Clock::now();
        for (auto& [id, timer] : m_timers) {
            if (timer.deadline <= now) due.push_back(timer);
        }

        for (const Timer& timer : due) {
            if (m_stopping) return;
            // Re-check: an earlier callback in this batch may have killed it.
            auto it = m_timers.find(timer.id);
            if (it == m_timers.end()) continue;

            // Learn timeSetEvent TIME_KILL_SYNCHRONOUS / timeKillEvent: a
            // caller may free dwUser as soon as timeKillEvent returns. Publish
            // the in-flight id before dropping the one-shot from the map, or
            // destroy() sees "already gone" and returns while dispatch() is
            // still about to run.
            m_firing_id = timer.id;
            m_firing_flags = timer.flags;

            perf::record(perf::Hist::mm_timer_lateness,
                         static_cast<u64>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 now - timer.deadline).count()));

            if (it->second.periodic()) {
                // Advance from the previous deadline, not from now, so a late
                // callback does not push every subsequent tick later.
                auto advanced =
                    it->second.deadline + std::chrono::milliseconds(it->second.period_ms);
                if (advanced <= now) {
                    // Far enough behind that catching up would fire a burst of
                    // back-to-back callbacks. Re-anchor to the next whole
                    // period instead: a stalled decoder should resume on
                    // cadence, not receive a storm of missed ticks at once.
                    advanced = now + std::chrono::milliseconds(it->second.period_ms);
                }
                it->second.deadline = advanced;
            } else {
                m_timers.erase(it);
            }

            lock.unlock();
            dispatch(timer);
            lock.lock();
            m_firing_id = 0;
            m_callback_done.notify_all();
        }
    }
}

void MultimediaTimerScheduler::dispatch(const Timer& timer) {
    perf::count(perf::Counter::mm_timer_fire);

    static const bool mm_timer_diag =
        std::getenv("VITREO_MM_TIMER_DIAG") != nullptr;
    if (mm_timer_diag) {
        static std::atomic<u64> fire_count{0};
        const u64 count = fire_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 32 || (count % 1000) == 0) {
            std::fprintf(stderr,
                         "[mm-timer-diag] fire count=%llu id=%u proc=0x%08llx "
                         "user=0x%llx flags=0x%x\n",
                         static_cast<unsigned long long>(count), timer.id,
                         static_cast<unsigned long long>(timer.callback),
                         static_cast<unsigned long long>(timer.user_data), timer.flags);
            std::fflush(stderr);
        }
    }

    const u32 callback_type = timer.flags & TIME_CALLBACK_TYPE_MASK;
    if (callback_type == TIME_CALLBACK_EVENT_SET ||
        callback_type == TIME_CALLBACK_EVENT_PULSE) {
        EventSignaler signaler;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            signaler = m_event_signaler;
        }
        if (signaler) {
            signaler(static_cast<u32>(timer.callback),
                     callback_type == TIME_CALLBACK_EVENT_PULSE);
        } else {
            VITREO_TRACE("[winmm] timer id=%u event mode with no signaler wired\n",
                         timer.id);
        }
        return;
    }

    ProcInvoker invoker;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        invoker = m_proc_invoker;
    }
    if (invoker) {
        invoker(timer.callback, timer.id, timer.user_data);
    } else {
        VITREO_TRACE("[winmm] timer id=%u proc=0x%llx with no invoker wired\n",
                     timer.id,
                     static_cast<unsigned long long>(timer.callback));
    }
}

} // namespace vitreo::winapi
