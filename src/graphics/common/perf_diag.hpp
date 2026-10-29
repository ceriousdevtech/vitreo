#pragma once

/// @file perf_diag.hpp
/// @brief Aggregated performance telemetry, disabled unless VITREO_PERF_DIAG is set.
///
/// This exists to answer "where does a frame actually go" without the
/// per-event logging that would distort the very thing being measured.
/// Everything is a relaxed atomic behind a single process-wide flag, and
/// nothing is printed until a report interval elapses (~1 Hz) or the process
/// exits.
///
/// Usage from a hot path:
/// @code
///     perf::count(perf::Counter::dd_unlock);
///     const auto t0 = perf::now();
///     ...
///     perf::record(perf::Hist::present_submit, perf::since(t0));
/// @endcode
///
/// `perf::enabled()` is a single relaxed load of a cached bool, so guarded
/// call sites cost nothing measurable when telemetry is off.

#include <vitreo/common.hpp>

#include <chrono>

namespace vitreo::perf {

// -----------------------------------------------------------------------------
// Counters
// -----------------------------------------------------------------------------
// Kept as an X-macro so adding a metric touches exactly one line and the
// reporter picks up the label automatically.

#define VITREO_PERF_COUNTERS(X)                                               \
    /* DirectDraw frontend */                                                 \
    X(dd_lock,                  "ddraw.lock")                                 \
    X(dd_unlock,                "ddraw.unlock")                               \
    X(dd_unlock_primary,        "ddraw.unlock.primary")                       \
    X(dd_present_request,       "ddraw.present_request")                      \
    X(dd_present_unavailable,   "ddraw.present_unavailable")                  \
    /* Presenter */                                                           \
    X(present_upload_full,      "present.upload.full")                        \
    X(present_upload_palette,   "present.upload.palette")                     \
    X(present_nochange_skip,    "present.skip.no_change")                     \
    X(present_submit,           "present.submit")                             \
    X(present_coalesced,        "present.coalesced")                          \
    X(present_drawable_fail,    "present.drawable_fail")                      \
    X(present_resize_drop,      "present.resize_drop")                        \
    /* Event pump */                                                          \
    X(pump_calls,               "pump.calls")                                 \
    X(pump_cross_thread,        "pump.cross_thread")                          \
    /* Timing */                                                              \
    X(sleep_calls,              "sleep.calls")                                \
    X(sleep_zero,               "sleep.zero")                                 \
    X(mm_timer_set,             "winmm.timeSetEvent")                         \
    X(mm_timer_kill,            "winmm.timeKillEvent")                        \
    X(mm_timer_fire,            "winmm.timer_fire")                           \
    X(mm_begin_period,          "winmm.timeBeginPeriod")                      \
    X(mm_end_period,            "winmm.timeEndPeriod")                        \
    X(mm_time_get_time,         "winmm.timeGetTime")                          \
    X(k32_get_tick_count,       "kernel32.GetTickCount")                      \
    X(k32_query_perf_counter,   "kernel32.QueryPerformanceCounter")

enum class Counter : u32 {
#define VITREO_PERF_ENUM(name, label) name,
    VITREO_PERF_COUNTERS(VITREO_PERF_ENUM)
#undef VITREO_PERF_ENUM
    COUNT
};

// -----------------------------------------------------------------------------
// Histograms
// -----------------------------------------------------------------------------
// One shared bucket scale spans both the frame-scale spans the brief asks for
// (<1ms .. >40ms) and the finer event-pump scale (<0.25ms .. >10ms), so there
// is only one set of edges to reason about.

#define VITREO_PERF_HISTS(X)                                                  \
    X(present_update_to_submit, "present.update->submit")                     \
    X(present_submit_to_done,   "present.submit->complete")                   \
    X(present_drawable_wait,    "present.drawable_wait")                      \
    X(frame_interval,           "present.frame_interval")                     \
    X(pump_duration,            "pump.duration")                              \
    X(pump_cross_thread_wait,   "pump.cross_thread_wait")                     \
    X(sleep_overshoot,          "sleep.overshoot")                            \
    X(mm_timer_lateness,        "winmm.timer_lateness")

enum class Hist : u32 {
#define VITREO_PERF_ENUM(name, label) name,
    VITREO_PERF_HISTS(VITREO_PERF_ENUM)
#undef VITREO_PERF_ENUM
    COUNT
};

constexpr u32 HIST_BUCKETS = 8;

/// Upper edges in nanoseconds; the final bucket is unbounded.
constexpr u64 HIST_EDGES_NS[HIST_BUCKETS - 1] = {
    250'000ULL,     // 0.25 ms
    1'000'000ULL,   // 1 ms
    4'000'000ULL,   // 4 ms
    8'000'000ULL,   // 8 ms
    16'000'000ULL,  // 16 ms
    25'000'000ULL,  // 25 ms
    40'000'000ULL,  // 40 ms
};

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// Cached once at first use from VITREO_PERF_DIAG. Never changes afterwards,
/// so branch prediction makes guarded call sites free in the disabled case.
[[nodiscard]] bool enabled() noexcept;

inline TimePoint now() noexcept { return Clock::now(); }

[[nodiscard]] inline u64 since(TimePoint start) noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now() - start)
            .count());
}

void count(Counter counter, u64 amount = 1) noexcept;
void record(Hist hist, u64 nanoseconds) noexcept;

/// Set an instantaneous value (last-writer-wins) rather than accumulating.
/// Used for gauges such as command buffers currently in flight, where the
/// report wants the peak observed over the interval.
void gauge_max(Counter counter, u64 value) noexcept;

/// Emit the aggregated report if the interval has elapsed. Cheap enough to
/// call from a per-frame path: one clock read and one comparison when enabled,
/// one predictable branch when not.
void maybe_report() noexcept;

/// Force a report immediately (process exit, or an explicit flush point).
void report_now() noexcept;

/// RAII span that records into a histogram on scope exit.
class Span {
public:
    explicit Span(Hist hist) noexcept
        : m_hist(hist), m_active(enabled()), m_start(m_active ? now() : TimePoint{}) {}

    ~Span() {
        if (m_active) record(m_hist, since(m_start));
    }

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

private:
    Hist m_hist;
    bool m_active;
    TimePoint m_start;
};

} // namespace vitreo::perf
