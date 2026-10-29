#include "graphics/common/perf_diag.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace vitreo::perf {
namespace {

constexpr auto REPORT_INTERVAL = std::chrono::seconds(1);

const char* const COUNTER_LABELS[] = {
#define VITREO_PERF_LABEL(name, label) label,
    VITREO_PERF_COUNTERS(VITREO_PERF_LABEL)
#undef VITREO_PERF_LABEL
};

const char* const HIST_LABELS[] = {
#define VITREO_PERF_LABEL(name, label) label,
    VITREO_PERF_HISTS(VITREO_PERF_LABEL)
#undef VITREO_PERF_LABEL
};

const char* const BUCKET_LABELS[HIST_BUCKETS] = {
    "<0.25ms", "0.25-1ms", "1-4ms", "4-8ms",
    "8-16ms",  "16-25ms",  "25-40ms", ">40ms",
};

struct State {
    std::array<std::atomic<u64>, static_cast<usize>(Counter::COUNT)> counters{};
    std::array<std::array<std::atomic<u64>, HIST_BUCKETS>,
               static_cast<usize>(Hist::COUNT)> hists{};
    /// Running max and sum per histogram, so the report can show the worst
    /// observed span - the tail is usually the interesting part, and a bucket
    /// alone cannot distinguish 41 ms from 400 ms.
    std::array<std::atomic<u64>, static_cast<usize>(Hist::COUNT)> hist_max{};
    std::array<std::atomic<u64>, static_cast<usize>(Hist::COUNT)> hist_sum{};

    std::mutex report_mutex;
    TimePoint last_report{};
    TimePoint started{};
    bool started_valid{false};
};

State& state() {
    static State s;
    return s;
}

/// Report-path only: read a counter and reset it, so each line covers just the
/// interval that elapsed rather than the whole run.
u64 take(std::atomic<u64>& value) noexcept {
    return value.exchange(0, std::memory_order_relaxed);
}

u32 bucket_for(u64 nanoseconds) noexcept {
    for (u32 index = 0; index < HIST_BUCKETS - 1; ++index) {
        if (nanoseconds < HIST_EDGES_NS[index]) return index;
    }
    return HIST_BUCKETS - 1;
}

void emit_report_locked(double elapsed_seconds) {
    State& s = state();
    if (elapsed_seconds <= 0.0) return;

    // The final exit flush often covers only the few milliseconds since the
    // last periodic report. Extrapolating a per-second rate from that produces
    // numbers like "4810583/s" that look like findings and are not, so the rate
    // column is suppressed below a threshold where it stops being meaningful.
    constexpr double MIN_RATE_INTERVAL_SECONDS = 0.05;
    const bool show_rate = elapsed_seconds >= MIN_RATE_INTERVAL_SECONDS;

    std::fprintf(stderr, "\n=== vitreo perf (%.3fs%s) ===\n", elapsed_seconds,
                 show_rate ? "" : ", rates omitted: interval too short");

    bool any_counter = false;
    for (usize index = 0; index < static_cast<usize>(Counter::COUNT); ++index) {
        const u64 total = take(s.counters[index]);
        if (total == 0) continue;
        any_counter = true;
        if (show_rate) {
            std::fprintf(stderr, "  %-28s %10llu  (%8.1f/s)\n",
                         COUNTER_LABELS[index],
                         static_cast<unsigned long long>(total),
                         static_cast<double>(total) / elapsed_seconds);
        } else {
            std::fprintf(stderr, "  %-28s %10llu\n", COUNTER_LABELS[index],
                         static_cast<unsigned long long>(total));
        }
    }
    if (!any_counter) std::fprintf(stderr, "  (no counter activity)\n");

    for (usize index = 0; index < static_cast<usize>(Hist::COUNT); ++index) {
        u64 samples = 0;
        std::array<u64, HIST_BUCKETS> buckets{};
        for (u32 bucket = 0; bucket < HIST_BUCKETS; ++bucket) {
            buckets[bucket] = take(s.hists[index][bucket]);
            samples += buckets[bucket];
        }
        if (samples == 0) {
            (void)take(s.hist_max[index]);
            (void)take(s.hist_sum[index]);
            continue;
        }
        const u64 worst = take(s.hist_max[index]);
        const u64 sum = take(s.hist_sum[index]);
        std::fprintf(stderr, "  %s  n=%llu avg=%.3fms max=%.3fms\n",
                     HIST_LABELS[index],
                     static_cast<unsigned long long>(samples),
                     static_cast<double>(sum) / static_cast<double>(samples) / 1e6,
                     static_cast<double>(worst) / 1e6);
        for (u32 bucket = 0; bucket < HIST_BUCKETS; ++bucket) {
            if (buckets[bucket] == 0) continue;
            std::fprintf(stderr, "      %-9s %8llu  %5.1f%%\n",
                         BUCKET_LABELS[bucket],
                         static_cast<unsigned long long>(buckets[bucket]),
                         100.0 * static_cast<double>(buckets[bucket]) /
                             static_cast<double>(samples));
        }
    }
    std::fflush(stderr);
}

} // namespace

bool enabled() noexcept {
    static const bool on = [] {
        const bool value = std::getenv("VITREO_PERF_DIAG") != nullptr;
        if (value) {
            State& s = state();
            s.started = now();
            s.last_report = s.started;
            s.started_valid = true;
            // A run that ends via ExitProcess or a guest crash still gets its
            // final interval flushed rather than silently losing it.
            std::atexit([] { report_now(); });
        }
        return value;
    }();
    return on;
}

void count(Counter counter, u64 amount) noexcept {
    if (!enabled()) return;
    state().counters[static_cast<usize>(counter)].fetch_add(
        amount, std::memory_order_relaxed);
}

void gauge_max(Counter counter, u64 value) noexcept {
    if (!enabled()) return;
    auto& slot = state().counters[static_cast<usize>(counter)];
    u64 previous = slot.load(std::memory_order_relaxed);
    while (value > previous &&
           !slot.compare_exchange_weak(previous, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
    }
}

void record(Hist hist, u64 nanoseconds) noexcept {
    if (!enabled()) return;
    State& s = state();
    const usize index = static_cast<usize>(hist);
    s.hists[index][bucket_for(nanoseconds)].fetch_add(1,
                                                      std::memory_order_relaxed);
    s.hist_sum[index].fetch_add(nanoseconds, std::memory_order_relaxed);

    auto& worst = s.hist_max[index];
    u64 previous = worst.load(std::memory_order_relaxed);
    while (nanoseconds > previous &&
           !worst.compare_exchange_weak(previous, nanoseconds,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    }
}

void maybe_report() noexcept {
    if (!enabled()) return;
    State& s = state();
    const TimePoint current = now();
    {
        // Cheap pre-check without the lock: the common case is "not yet".
        std::lock_guard<std::mutex> guard(s.report_mutex);
        if (current - s.last_report < REPORT_INTERVAL) return;
        const double elapsed =
            std::chrono::duration<double>(current - s.last_report).count();
        s.last_report = current;
        emit_report_locked(elapsed);
    }
}

void report_now() noexcept {
    if (!enabled()) return;
    State& s = state();
    const TimePoint current = now();
    std::lock_guard<std::mutex> guard(s.report_mutex);
    const double elapsed =
        std::chrono::duration<double>(current - s.last_report).count();
    s.last_report = current;
    emit_report_locked(elapsed);
}

} // namespace vitreo::perf
