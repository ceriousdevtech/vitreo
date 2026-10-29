// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file message_time.hpp
/// @brief Guest-visible Win32 message/timer timestamps.
///
/// MSG.time, GetMessageTime, and TIMERPROC dwTime are GetTickCount-domain
/// values. Host scheduling (TimerInfo::last_fire) stays on steady_clock.

#include "winapi/kernel32/system/time.hpp"
#include "windowing/messages/message_types.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

constexpr u32 USER_TIMER_MINIMUM = 10;
constexpr u32 USER_TIMER_MAXIMUM = 0x7FFFFFFF;

/// Unsigned 32-bit elapsed ticks. Correct across the 49.7-day DWORD wrap.
[[nodiscard]] inline u32 tick_delta32(u32 later, u32 earlier) noexcept {
    return later - earlier;
}

[[nodiscard]] inline bool tick_in_closed_range32(u32 value, u32 first,
                                                 u32 last) noexcept {
    return tick_delta32(value, first) <= tick_delta32(last, first);
}

[[nodiscard]] inline bool timer_diag_enabled() noexcept {
    static const bool enabled = std::getenv("VITREO_TIMER_DIAG") != nullptr;
    return enabled;
}

inline void timer_diag(const char* fmt, ...) noexcept {
    if (!timer_diag_enabled()) return;
    static std::atomic<u64> last_ms{0};
    static std::atomic<u32> suppressed{0};
    const u64 now = tick_count_ms();
    const u64 prev = last_ms.load(std::memory_order_relaxed);
    if (prev != 0 && now - prev < 250u) {
        suppressed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    last_ms.store(now, std::memory_order_relaxed);
    const u32 skipped = suppressed.exchange(0, std::memory_order_relaxed);
    if (skipped != 0) {
        std::fprintf(stderr, "[timer-diag] suppressed %u lines\n", skipped);
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

/// Assign GetTickCount-domain time when a producer left MSG.time unset.
inline void stamp_message_time(windowing::MSG& msg) noexcept {
    if (msg.time == 0) {
        msg.time = tick_count32();
    }
}

[[nodiscard]] inline windowing::MSG make_timer_message(
    windowing::HWND hwnd, u64 timer_id, GuestAddr callback) noexcept {
    windowing::MSG msg{};
    msg.hwnd = hwnd;
    msg.message = windowing::WM::TIMER;
    msg.wParam = timer_id;
    msg.lParam = static_cast<u64>(callback);
    msg.time = tick_count32();
    timer_diag("[timer-diag] WM_TIMER hwnd=0x%x id=%llu msg.time=%u gettick=%u "
               "delta=%u\n",
               hwnd, static_cast<unsigned long long>(timer_id), msg.time,
               tick_count32(), tick_delta32(tick_count32(), msg.time));
    return msg;
}

} // namespace vitreo::winapi
