// SPDX-License-Identifier: LGPL-2.1-only

/// @file present_rate.cpp
/// @brief Implementation of the present-rate probe. See present_rate.hpp.

#include "graphics/common/present_rate.hpp"

#include <atomic>
#include <ctime>
#include <unistd.h>

namespace vitreo::graphics {

namespace {

u64 now_ns() noexcept {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull +
           static_cast<u64>(ts.tv_nsec);
}

/// Gap between consecutive presents, in the units a reader actually reasons
/// in. 8/17/34 are 120/60/30 fps; past that the question stops being "which
/// refresh rate" and becomes "how long did it stall".
constexpr u64 kBucketCeilingMs[] = {8, 17, 34, 50, 100, 250};
constexpr usize kBucketCount = (sizeof(kBucketCeilingMs) /
                                sizeof(kBucketCeilingMs[0])) + 1;

struct State {
    std::atomic<u64> presents{0};
    std::atomic<u64> drawable_skips{0};
    std::atomic<u64> vsync_presents{0};
    std::atomic<u64> first_ns{0};
    std::atomic<u64> last_ns{0};
    std::atomic<u64> max_gap_ns{0};
    std::atomic<u64> buckets[kBucketCount]{};
};

// Present times of the most recent frames, for recent_presents: a second
// of frames at any refresh rate fits, and older slots are overwritten. A
// writer claims its slot with one increment, so concurrent presenters never
// share one.
constexpr u64 kRecentSlots = 1024;
constexpr u64 kRecentWindowNs = 1000000000ull;
std::atomic<u64> g_recent_ns[kRecentSlots]{};
std::atomic<u64> g_recent_count{0};
std::atomic<u64> g_recent_size{0};  // width << 32 | height

State& state() {
    static State s;
    return s;
}

} // namespace

void note_present(bool vsync) noexcept {
    State& s = state();
    const u64 t = now_ns();
    const u64 slot = g_recent_count.fetch_add(1, std::memory_order_relaxed);
    g_recent_ns[slot % kRecentSlots].store(t, std::memory_order_release);
    const u64 previous = s.last_ns.exchange(t, std::memory_order_relaxed);
    s.presents.fetch_add(1, std::memory_order_relaxed);
    if (vsync) s.vsync_presents.fetch_add(1, std::memory_order_relaxed);

    if (previous == 0) {
        // First present in this process: no interval to attribute yet.
        u64 expected = 0;
        s.first_ns.compare_exchange_strong(expected, t,
                                           std::memory_order_relaxed);
        return;
    }
    if (t <= previous) return;   // another thread raced ahead; skip the sample

    const u64 gap = t - previous;
    const u64 gap_ms = gap / 1000000ull;
    usize bucket = kBucketCount - 1;
    for (usize i = 0; i < kBucketCount - 1; ++i) {
        if (gap_ms < kBucketCeilingMs[i]) { bucket = i; break; }
    }
    s.buckets[bucket].fetch_add(1, std::memory_order_relaxed);

    u64 worst = s.max_gap_ns.load(std::memory_order_relaxed);
    while (gap > worst &&
           !s.max_gap_ns.compare_exchange_weak(worst, gap,
                                               std::memory_order_relaxed)) {
    }
}

void note_present_size(u32 width, u32 height) noexcept {
    g_recent_size.store((static_cast<u64>(width) << 32) | height,
                        std::memory_order_relaxed);
}

u64 present_clock_ns() noexcept { return now_ns(); }

RecentPresents recent_presents(u64 now) noexcept {
    RecentPresents recent;
    const u64 size = g_recent_size.load(std::memory_order_relaxed);
    recent.width = static_cast<u32>(size >> 32);
    recent.height = static_cast<u32>(size);
    const u64 count = g_recent_count.load(std::memory_order_acquire);
    const u64 window_start = now > kRecentWindowNs ? now - kRecentWindowNs : 0;
    const u64 oldest = count > kRecentSlots ? count - kRecentSlots : 0;
    u64 newer = 0;  // present time of the frame after the one examined
    u64 interval_total = 0;
    u32 intervals = 0;
    for (u64 i = count; i > oldest; --i) {
        const u64 at = g_recent_ns[(i - 1) % kRecentSlots].load(std::memory_order_acquire);
        if (at == 0 || at > now) continue;  // a slot still being written
        const u64 interval = newer == 0 ? now - at : (newer > at ? newer - at : 0);
        if (newer != 0) {
            interval_total += interval;
            ++intervals;
        }
        // The open interval counts too: a stall shows while it lasts.
        if (static_cast<double>(interval) / 1e6 > recent.worst_ms) {
            recent.worst_ms = static_cast<double>(interval) / 1e6;
        }
        if (at < window_start) break;
        ++recent.frames;
        newer = at;
    }
    if (intervals > 0) {
        recent.average_ms = static_cast<double>(interval_total) / intervals / 1e6;
    }
    return recent;
}

usize recent_frame_intervals_ms(u64 now, u64 window_ns, float* out,
                                usize max) noexcept {
    if (out == nullptr || max == 0) return 0;
    const u64 count = g_recent_count.load(std::memory_order_acquire);
    const u64 window_start = now > window_ns ? now - window_ns : 0;
    const u64 oldest = count > kRecentSlots ? count - kRecentSlots : 0;
    // Walk newest to oldest, filling @p out from its end, then slide the
    // written tail to the front so the result reads oldest first.
    usize written = 0;
    u64 newer = 0;
    for (u64 i = count; i > oldest && written < max; --i) {
        const u64 at = g_recent_ns[(i - 1) % kRecentSlots].load(std::memory_order_acquire);
        if (at == 0 || at > now) continue;  // a slot still being written
        if (at < window_start) break;
        if (newer != 0 && newer > at) {
            out[max - 1 - written] = static_cast<float>(newer - at) / 1e6f;
            ++written;
        }
        newer = at;
    }
    for (usize i = 0; i < written; ++i) out[i] = out[max - written + i];
    return written;
}

void reset_present_rate_for_testing() noexcept {
    for (auto& slot : g_recent_ns) slot.store(0, std::memory_order_relaxed);
    g_recent_count.store(0, std::memory_order_relaxed);
    g_recent_size.store(0, std::memory_order_relaxed);
}

void note_drawable_skip() noexcept {
    state().drawable_skips.fetch_add(1, std::memory_order_relaxed);
}

void present_rate_report(std::FILE* out) noexcept {
    if (out == nullptr) return;
    State& s = state();
    const u64 n = s.presents.load(std::memory_order_relaxed);
    const u64 skips = s.drawable_skips.load(std::memory_order_relaxed);
    if (n == 0 && skips == 0) {
        std::fprintf(out, "[present-rate pid=%d] no frames presented by this "
                          "process\n", static_cast<int>(::getpid()));
        return;
    }
    const u64 first = s.first_ns.load(std::memory_order_relaxed);
    const u64 last = s.last_ns.load(std::memory_order_relaxed);
    const double seconds =
        last > first ? static_cast<double>(last - first) / 1e9 : 0.0;

    std::fprintf(out,
        "\n=== present rate (pid %d) ===\n"
        "frames=%llu  vsync=%llu  window=%.2fs  average=%.1f fps  worst gap=%.1f ms\n"
        "dropped acquisitions=%llu  (%.1f%% of attempts -- load independent)\n"
        "gaps between consecutive presents:\n",
        static_cast<int>(::getpid()),
        static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(
            s.vsync_presents.load(std::memory_order_relaxed)),
        seconds,
        seconds > 0.0 ? static_cast<double>(n - 1) / seconds : 0.0,
        static_cast<double>(s.max_gap_ns.load(std::memory_order_relaxed)) / 1e6,
        static_cast<unsigned long long>(skips),
        (n + skips) > 0
            ? 100.0 * static_cast<double>(skips) / static_cast<double>(n + skips)
            : 0.0);

    for (usize i = 0; i < kBucketCount; ++i) {
        const u64 count = s.buckets[i].load(std::memory_order_relaxed);
        if (count == 0) continue;
        if (i == kBucketCount - 1) {
            std::fprintf(out, "  >= %4llu ms  %10llu   (a visible stall)\n",
                         static_cast<unsigned long long>(
                             kBucketCeilingMs[kBucketCount - 2]),
                         static_cast<unsigned long long>(count));
        } else {
            std::fprintf(out, "  <  %4llu ms  %10llu   (>= %.0f fps)\n",
                         static_cast<unsigned long long>(kBucketCeilingMs[i]),
                         static_cast<unsigned long long>(count),
                         1000.0 / static_cast<double>(kBucketCeilingMs[i]));
        }
    }
    std::fflush(out);
}

} // namespace vitreo::graphics
