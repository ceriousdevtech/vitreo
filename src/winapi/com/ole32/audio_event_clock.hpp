#pragma once
/// @file audio_event_clock.hpp
/// @brief The WASAPI shared-mode audio engine's processing pass.
///
/// Learn, IAudioClient::Initialize / SetEventHandle: a stream initialised with
/// AUDCLNT_STREAMFLAGS_EVENTCALLBACK is serviced by a client thread that waits
/// on the event handle it registered; "while the stream is running, the system
/// periodically signals the event to indicate to the client that audio data is
/// available for processing". The engine runs on its own period whether or not
/// the client has queued data (an underrun plays silence), so the signal must
/// be driven by a clock, not by buffer completions: a client that starts the
/// stream and then waits for the first event before writing would otherwise
/// never be woken.
///
/// One host thread runs the engine for every running render stream: each
/// period it first reads the stream's period of frames out of its endpoint
/// buffer (IAudioPushVoice::engine_pass -- so padding has dropped by then),
/// then signals the stream's event, if it has one. Polling streams get the
/// same passes with no event. The thread is host machinery, not a guest
/// thread, so it runs under a real-time (time-constraint) policy, and its
/// deadlines stay on a fixed grid: a late wake makes up the missed periods
/// in the engine read and signals once, rather than drifting.
///
/// remove() is synchronous: once it returns, the handle it named is never
/// signalled and the voice is never touched again, so the caller may close
/// or destroy them.

#include "audio/contracts/audio_renderer.hpp"

#include <vitreo/common.hpp>
#include <vitreo/present_cpu.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace vitreo::winapi {

class AudioEventClock {
public:
    /// Signals one event handle. Runs on the clock thread.
    using Signal = std::function<void(u64 handle)>;

    AudioEventClock(std::chrono::nanoseconds period, Signal signal)
        : m_period(period), m_signal(std::move(signal)) {}

    ~AudioEventClock() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    AudioEventClock(const AudioEventClock&) = delete;
    AudioEventClock& operator=(const AudioEventClock&) = delete;

    /// Run the engine for stream @p key every period: an engine pass on
    /// @p voice (when not null), then a signal of @p handle (when not 0).
    /// Replaces any registration already held for the key.
    void add(u64 key, u64 handle, audio::IAudioPushVoice* voice = nullptr) {
        {
            std::lock_guard lock(m_mutex);
            m_streams[key] = Stream{handle, voice};
            if (!m_thread.joinable()) m_thread = std::thread([this] { run(); });
        }
        m_cv.notify_all();
    }

    /// Stop servicing stream @p key. Waits for an in-flight tick, so neither
    /// its handle nor its voice is used after this returns.
    void remove(u64 key) {
        std::unique_lock lock(m_mutex);
        m_streams.erase(key);
        if (std::this_thread::get_id() == m_thread.get_id()) return;
        m_idle_cv.wait(lock, [this] { return !m_ticking; });
    }

    /// Wait out an in-flight tick: any handle replaced or removed before this
    /// call is not being signalled once it returns.
    void quiesce() {
        std::unique_lock lock(m_mutex);
        if (std::this_thread::get_id() == m_thread.get_id()) return;
        m_idle_cv.wait(lock, [this] { return !m_ticking; });
    }

    [[nodiscard]] bool contains(u64 key) const {
        std::lock_guard lock(m_mutex);
        return m_streams.contains(key);
    }

    [[nodiscard]] std::chrono::nanoseconds period() const noexcept {
        return m_period;
    }

private:
    struct Stream {
        u64 handle{0};
        audio::IAudioPushVoice* voice{nullptr};
    };

    /// Real-time scheduling for the engine thread: a periodic constraint of
    /// one engine period, a small computation budget, and a deadline of half
    /// a period. Falls back to the highest QoS class if refused.
    void elevate_thread() const {
#if defined(__APPLE__)
        mach_timebase_info_data_t timebase{};
        (void)mach_timebase_info(&timebase);
        const auto to_abs = [&timebase](std::chrono::nanoseconds ns) {
            return static_cast<uint32_t>(static_cast<u64>(ns.count()) * timebase.denom /
                                         timebase.numer);
        };
        thread_time_constraint_policy_data_t policy{};
        policy.period = to_abs(m_period);
        policy.computation = to_abs(m_period / 10);
        policy.constraint = to_abs(m_period / 2);
        policy.preemptible = TRUE;
        if (thread_policy_set(pthread_mach_thread_np(pthread_self()),
                              THREAD_TIME_CONSTRAINT_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS) {
            (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        }
#endif
    }

    void run() {
        elevate_thread();
        std::unique_lock lock(m_mutex);
        auto next = std::chrono::steady_clock::now() + m_period;
        std::vector<Stream> streams;
        Stats stats;
        while (!m_stop) {
            if (m_streams.empty()) {
                m_cv.wait(lock, [this] { return m_stop || !m_streams.empty(); });
                next = std::chrono::steady_clock::now() + m_period;
                continue;
            }
            if (m_cv.wait_until(lock, next, [this] { return m_stop; })) break;
            const auto now = std::chrono::steady_clock::now();
            const auto late = now - next;
            // Every period boundary passed since the last pass is engine time
            // owed to the streams: read it all now, signal once, and keep the
            // next deadline on the same grid.
            u64 periods = 1;
            if (now >= next + m_period) periods += static_cast<u64>((now - next) / m_period);
            next += m_period * static_cast<i64>(periods);
            streams.clear();
            for (const auto& [key, stream] : m_streams) {
                (void)key;
                streams.push_back(stream);
            }
            m_ticking = true;
            lock.unlock();
            const auto elapsed = static_cast<u64>(m_period.count()) * periods;
            for (const Stream& stream : streams) {
                if (stream.voice != nullptr) stream.voice->engine_pass(elapsed);
                if (stream.handle != 0) m_signal(stream.handle);
            }
            stats.note_tick(late, periods - 1u, streams.size(), m_period);
            lock.lock();
            m_ticking = false;
            m_idle_cv.notify_all();
        }
    }

    /// Per-second `[wasapi-clock]` diagnostics, owned by the clock thread.
    struct Stats {
        double window_start_ms{0.0};
        u64 ticks{0};
        u64 dropped{0};
        double late_max_us{0.0};
        u32 windows{0};

        void note_tick(std::chrono::steady_clock::duration late, u64 dropped_periods,
                       usize streams, std::chrono::nanoseconds period) {
            const double now_ms = diagnostics::present_cpu::host_mono_ms();
            const double late_us =
                std::chrono::duration<double, std::micro>(late).count();
            ++ticks;
            dropped += dropped_periods;
            late_max_us = std::max(late_max_us, late_us);
            if (window_start_ms == 0.0) {
                window_start_ms = now_ms;
                std::fprintf(stderr,
                    "[wasapi-clock] format period_us=%.0f (dropped=periods merged into "
                    "a late pass; late_max_us=worst wake lateness)\n",
                    std::chrono::duration<double, std::micro>(period).count());
                return;
            }
            if (now_ms - window_start_ms < 1000.0) return;
            const double period_us =
                std::chrono::duration<double, std::micro>(period).count();
            const bool anomaly = dropped != 0 || late_max_us > period_us / 2.0;
            if (anomaly || windows % 10 == 0) {
                std::fprintf(stderr,
                    "[wasapi-clock] t_ms=%.1f dt_ms=%.0f anom=%d ticks=%llu "
                    "dropped=%llu late_max_us=%.0f streams=%zu\n",
                    now_ms, now_ms - window_start_ms, anomaly ? 1 : 0,
                    static_cast<unsigned long long>(ticks),
                    static_cast<unsigned long long>(dropped), late_max_us,
                    static_cast<size_t>(streams));
            }
            ++windows;
            window_start_ms = now_ms;
            ticks = 0;
            dropped = 0;
            late_max_us = 0.0;
        }
    };

    const std::chrono::nanoseconds m_period;
    const Signal m_signal;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idle_cv;
    std::unordered_map<u64, Stream> m_streams;
    std::thread m_thread;
    bool m_ticking{false};
    bool m_stop{false};
};

} // namespace vitreo::winapi
