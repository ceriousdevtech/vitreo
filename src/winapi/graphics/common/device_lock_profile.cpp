// SPDX-License-Identifier: LGPL-2.1-only
/// @file device_lock_profile.cpp
/// @brief `[d3d11-lock]` (device_lock_profile.hpp).

#include "winapi/graphics/common/device_lock_profile.hpp"

#include <vitreo/common.hpp>
#include <vitreo/present_cpu.hpp>

#include <mach/mach_time.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace vitreo::winapi::device_lock_profile {

namespace {

constexpr u64 kDefaultLongHoldNs = 20'000'000;
constexpr u64 kWindowNs = 1'000'000'000;
constexpr u32 kLinesPerWindow = 20;

std::atomic<bool> g_forced{false};
std::atomic<std::FILE*> g_out{nullptr};
std::atomic<u64> g_long_hold_ns{kDefaultLongHoldNs};

/// Threads blocked in lock() now, and the most at once since the current
/// holder took the lock.
std::atomic<u32> g_waiters{0};
std::atomic<u32> g_waiters_peak{0};

std::atomic<u64> g_window_start{0};  // mach ticks; 0 until the first event
std::atomic<u64> g_wait_ns{0};
std::atomic<u64> g_waits{0};
std::atomic<u64> g_max_wait_ns{0};
std::atomic<u64> g_long_holds{0};
std::atomic<u64> g_suppressed{0};
std::atomic<u32> g_lines{0};

struct ThreadHold {
    bool active{false};
    bool suspended{false};
    u64 start{0};   // approximate ticks
    u64 held{0};    // approximate ticks before the last suspend
};
thread_local ThreadHold t_hold;

[[nodiscard]] const mach_timebase_info_data_t& timebase() noexcept {
    static const mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t tb{};
        (void)mach_timebase_info(&tb);
        if (tb.numer == 0 || tb.denom == 0) tb = {1, 1};
        return tb;
    }();
    return info;
}

[[nodiscard]] u64 ticks_to_ns(u64 ticks) noexcept {
    const auto& tb = timebase();
    return static_cast<u64>(static_cast<__uint128_t>(ticks) * tb.numer /
                            tb.denom);
}

[[nodiscard]] std::FILE* out() noexcept {
    std::FILE* file = g_out.load(std::memory_order_relaxed);
    return file != nullptr ? file : stderr;
}

void store_max(std::atomic<u64>& slot, u64 value) noexcept {
    u64 seen = slot.load(std::memory_order_relaxed);
    while (value > seen &&
           !slot.compare_exchange_weak(seen, value,
                                       std::memory_order_relaxed)) {
    }
}

void store_max(std::atomic<u32>& slot, u32 value) noexcept {
    u32 seen = slot.load(std::memory_order_relaxed);
    while (value > seen &&
           !slot.compare_exchange_weak(seen, value,
                                       std::memory_order_relaxed)) {
    }
}

/// Close the one-second window when it has run out: whichever thread gets
/// there first prints it (when there was anything to say) and opens the next.
void maybe_close_window(u64 now_ticks) noexcept {
    u64 start = g_window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        (void)g_window_start.compare_exchange_strong(
            start, now_ticks, std::memory_order_relaxed);
        return;
    }
    if (now_ticks < start) return;
    const u64 elapsed_ns = ticks_to_ns(now_ticks - start);
    if (elapsed_ns < kWindowNs) return;
    if (!g_window_start.compare_exchange_strong(start, now_ticks,
                                                std::memory_order_relaxed)) {
        return;
    }
    const u64 wait_ns = g_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 waits = g_waits.exchange(0, std::memory_order_relaxed);
    const u64 max_wait = g_max_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 long_holds = g_long_holds.exchange(0, std::memory_order_relaxed);
    const u64 suppressed = g_suppressed.exchange(0, std::memory_order_relaxed);
    g_lines.store(0, std::memory_order_relaxed);
    if (wait_ns == 0 && long_holds == 0) return;
    std::fprintf(out(),
                 "[d3d11-lock] t_ms=%.1f window_ms=%.0f wait_ms=%.1f "
                 "waits=%llu max_wait_ms=%.1f long_holds=%llu "
                 "suppressed=%llu\n",
                 diagnostics::present_cpu::host_mono_ms(),
                 static_cast<double>(elapsed_ns) / 1e6,
                 static_cast<double>(wait_ns) / 1e6,
                 static_cast<unsigned long long>(waits),
                 static_cast<double>(max_wait) / 1e6,
                 static_cast<unsigned long long>(long_holds),
                 static_cast<unsigned long long>(suppressed));
}

}  // namespace

bool enabled() noexcept {
    static const bool from_env = [] {
        const char* value = std::getenv("VITREO_D3D11_LOCK_PROFILE");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return from_env || g_forced.load(std::memory_order_relaxed);
}

void lock(std::recursive_mutex& mutex) noexcept {
    if (mutex.try_lock()) return;
    if (!enabled()) {
        mutex.lock();
        return;
    }
    const u64 t0 = mach_absolute_time();
    store_max(g_waiters_peak,
              g_waiters.fetch_add(1, std::memory_order_relaxed) + 1);
    mutex.lock();
    g_waiters.fetch_sub(1, std::memory_order_relaxed);
    const u64 t1 = mach_absolute_time();
    const u64 waited = ticks_to_ns(t1 - t0);
    g_wait_ns.fetch_add(waited, std::memory_order_relaxed);
    g_waits.fetch_add(1, std::memory_order_relaxed);
    store_max(g_max_wait_ns, waited);
    maybe_close_window(t1);
}

void begin_hold() noexcept {
    if (!enabled()) return;
    t_hold = ThreadHold{true, false, mach_approximate_time(), 0};
    g_waiters_peak.store(g_waiters.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
}

void end_hold(const char* iface, u32 slot) noexcept {
    if (!t_hold.active) return;
    const u64 now = mach_approximate_time();
    const u64 held_ticks =
        t_hold.held + (t_hold.suspended ? 0 : now - t_hold.start);
    t_hold.active = false;
    const u64 held_ns = ticks_to_ns(held_ticks);
    if (held_ns >= g_long_hold_ns.load(std::memory_order_relaxed)) {
        g_long_holds.fetch_add(1, std::memory_order_relaxed);
        if (g_lines.fetch_add(1, std::memory_order_relaxed) <
            kLinesPerWindow) {
            const unsigned tid = g_vitreo_trace_tid;
            char name[64];
            if (!diagnostics::present_cpu::thread_name(tid, name,
                                                       sizeof(name)) ||
                name[0] == '\0') {
                std::snprintf(name, sizeof(name), "tid%u", tid);
            }
            std::fprintf(out(),
                         "[d3d11-lock] t_ms=%.1f held_ms=%.1f "
                         "method=%s[%u] thread=%s waiters_max=%u\n",
                         diagnostics::present_cpu::host_mono_ms(),
                         static_cast<double>(held_ns) / 1e6,
                         iface != nullptr ? iface : "?", slot, name,
                         g_waiters_peak.load(std::memory_order_relaxed));
        } else {
            g_suppressed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    maybe_close_window(now);
}

void suspend() noexcept {
    if (!t_hold.active || t_hold.suspended) return;
    t_hold.held += mach_approximate_time() - t_hold.start;
    t_hold.suspended = true;
}

void resume() noexcept {
    if (!t_hold.active || !t_hold.suspended) return;
    t_hold.start = mach_approximate_time();
    t_hold.suspended = false;
}

void configure_for_test(std::FILE* file, u64 long_hold_ns) noexcept {
    g_out.store(file, std::memory_order_relaxed);
    g_long_hold_ns.store(long_hold_ns != 0 ? long_hold_ns
                                           : kDefaultLongHoldNs,
                         std::memory_order_relaxed);
    g_forced.store(true, std::memory_order_relaxed);
}

}  // namespace vitreo::winapi::device_lock_profile
