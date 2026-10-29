// SPDX-License-Identifier: LGPL-2.1-only

/// @file gpu_busy.cpp
/// @brief Implementation of the GPU busy probe. See gpu_busy.hpp.

#include "graphics/common/gpu_busy.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <utility>

namespace vitreo::graphics {

namespace {

constexpr double kWindowS = 1.0;
// A second of command buffers at a few hundred per frame and 120 Hz fits;
// older slots are overwritten.
constexpr usize kSlots = 8192;

std::atomic<bool> g_wanted{false};
std::atomic<u64> g_allocated{0};

std::mutex g_mutex;
std::pair<double, double> g_intervals[kSlots];
usize g_next = 0;
usize g_count = 0;

} // namespace

bool gpu_busy_wanted() noexcept {
    return g_wanted.load(std::memory_order_relaxed);
}

void set_gpu_busy_wanted(bool wanted) noexcept {
    g_wanted.store(wanted, std::memory_order_relaxed);
}

void note_gpu_interval(double start_s, double end_s,
                       u64 device_allocated_bytes) noexcept {
    if (device_allocated_bytes != 0) {
        g_allocated.store(device_allocated_bytes, std::memory_order_relaxed);
    }
    // An empty or failed command buffer reports 0 or an inverted interval.
    if (!(start_s > 0.0) || !(end_s > start_s)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_intervals[g_next] = {start_s, end_s};
    g_next = (g_next + 1) % kSlots;
    if (g_count < kSlots) ++g_count;
}

RecentGpuBusy recent_gpu_busy(double now_s) noexcept {
    RecentGpuBusy recent;
    recent.window_ms = kWindowS * 1000.0;
    recent.allocated_bytes = g_allocated.load(std::memory_order_relaxed);
    const double window_start = now_s - kWindowS;
    static std::pair<double, double> scratch[kSlots];
    usize n = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (usize i = 0; i < g_count; ++i) {
            const auto& interval = g_intervals[i];
            if (interval.second <= window_start || interval.first >= now_s) continue;
            scratch[n++] = {std::max(interval.first, window_start),
                            std::min(interval.second, now_s)};
        }
    }
    recent.command_buffers = static_cast<u32>(n);
    std::sort(scratch, scratch + n);
    double busy = 0.0;
    double covered_to = window_start;
    for (usize i = 0; i < n; ++i) {
        const double start = std::max(scratch[i].first, covered_to);
        if (scratch[i].second > start) {
            busy += scratch[i].second - start;
            covered_to = scratch[i].second;
        }
    }
    recent.busy_ms = busy * 1000.0;
    return recent;
}

double gpu_busy_clock_s() noexcept {
    return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9;
}

void reset_gpu_busy_for_testing() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_next = 0;
    g_count = 0;
    g_allocated.store(0, std::memory_order_relaxed);
}

} // namespace vitreo::graphics
