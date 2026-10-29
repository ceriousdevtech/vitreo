#pragma once

/// @file timer_resolution.hpp
/// @brief Process-wide timer resolution, as requested via timeBeginPeriod.
///
/// Windows models timer resolution as a process-global property with reference
/// counting: every timeBeginPeriod(n) must be matched by a timeEndPeriod(n),
/// and the effective resolution is the smallest period any caller currently
/// holds. Restoring the previous value when the smallest holder releases is
/// part of the contract - a library that raises resolution for a video decode
/// and then drops it must not leave the whole process at 1 ms forever.
///
/// This lives outside winmm because the *consumers* are elsewhere: Sleep and
/// the wait paths in kernel32 need to know how precise the guest expects
/// timing to be, and should not have to link against winmm to find out.

#include <vitreo/common.hpp>

namespace vitreo::winapi {

/// Windows reports these through timeGetDevCaps; keep the two in agreement.
constexpr u32 TIMER_PERIOD_MIN = 1;
constexpr u32 TIMER_PERIOD_MAX = 1000000;

/// Resolution reported when no caller holds a request. Windows' default timer
/// resolution is ~15.6 ms (the legacy 64 Hz tick).
constexpr u32 TIMER_PERIOD_DEFAULT = 16;

/// Reference-counted timeBeginPeriod. False when `period` is out of the range
/// timeGetDevCaps advertises, which Windows reports as TIMERR_NOCANDO.
[[nodiscard]] bool timer_resolution_acquire(u32 period);

/// Release one reference taken by timer_resolution_acquire. False when the
/// caller never held a matching request - Windows likewise rejects an
/// unbalanced timeEndPeriod rather than silently ignoring it.
[[nodiscard]] bool timer_resolution_release(u32 period);

/// Smallest period currently held, or TIMER_PERIOD_DEFAULT when none is.
[[nodiscard]] u32 timer_resolution_effective() noexcept;

/// True while any caller holds a request finer than the default. Wait paths
/// use this to decide whether tight timing is worth spending CPU on.
[[nodiscard]] bool timer_resolution_is_high() noexcept;

/// Drop every outstanding request. For test isolation only.
void timer_resolution_reset_for_testing() noexcept;

} // namespace vitreo::winapi
