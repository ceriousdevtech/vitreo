// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ab_cycle.hpp
/// @brief VITREO_AB_CYCLE_S: an A/B measured inside one process.
///
/// MEASUREMENT AID, not a feature switch. A change that consults
/// ab_cycle_off() turns itself off for every other VITREO_AB_CYCLE_S-second
/// window of the process's life, and each change of arm is logged
/// ("[ab-cycle] arm=on|off"). The rate log between two markers then belongs
/// to one arm. Comparing arms inside one process cancels what makes separate
/// runs incomparable on a busy machine: Time Machine, Spotlight, a different
/// menu scene, a different shader cache state. Unset (the default), every
/// change is simply on.

#include <vitreo/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::diagnostics {

enum class AbForce : int { None = 0, On = 1, Off = 2 };

namespace ab_detail {
inline std::atomic<int>& forced() noexcept {
    static std::atomic<int> value{0};
    return value;
}
inline u64 period_seconds() noexcept {
    static const u64 seconds = [] {
        const char* v = std::getenv("VITREO_AB_CYCLE_S");
        return v != nullptr ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    return seconds;
}
/// The process's window clock: windows count from the first call that asks
/// for an arm (ab_cycle_off past its VITREO_AB_CYCLE_ONLY filter, or
/// ab_cycle_arm_name), so every caller sees the same boundaries.
inline std::chrono::steady_clock::time_point origin() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}
/// 0 = on, 1 = off, for the window @p now falls in.
inline int arm_at(std::chrono::steady_clock::time_point now,
                  u64 period) noexcept {
    const auto start = origin();
    const u64 elapsed =
        now > start
            ? static_cast<u64>(std::chrono::duration_cast<
                                   std::chrono::seconds>(now - start)
                                   .count())
            : 0ull;
    return static_cast<int>((elapsed / period) % 2);
}
}  // namespace ab_detail

/// Tests only: pin the arm (None returns to the environment).
inline void ab_cycle_force_for_test(AbForce force) noexcept {
    ab_detail::forced().store(static_cast<int>(force),
                              std::memory_order_relaxed);
}

namespace ab_detail {
/// @p feature is one of the comma-separated names in @p list.
inline bool listed(const char* list, const char* feature) noexcept {
    const usize n = std::strlen(feature);
    for (const char* p = list; *p != '\0';) {
        const char* end = std::strchr(p, ',');
        const usize len =
            end != nullptr ? static_cast<usize>(end - p) : std::strlen(p);
        if (len == n && std::strncmp(p, feature, n) == 0) return true;
        if (end == nullptr) break;
        p = end + 1;
    }
    return false;
}
}  // namespace ab_detail

/// True while the current window is the "off" arm. Windows alternate on, off,
/// on, ... from the first call. @p feature names the change asking;
/// VITREO_AB_CYCLE_ONLY=<name>[,<name>...] cycles only the changes so named
/// and leaves every other one on, to attribute a combined result. Without
/// it, EVERY cycled change in the binary toggles together.
[[nodiscard]] inline bool ab_cycle_off(const char* feature = "") noexcept {
    const int forced = ab_detail::forced().load(std::memory_order_relaxed);
    if (forced != 0) return forced == static_cast<int>(AbForce::Off);
    const u64 period = ab_detail::period_seconds();
    if (period == 0) return false;
    static const char* const only = std::getenv("VITREO_AB_CYCLE_ONLY");
    if (only != nullptr && only[0] != '\0' &&
        !ab_detail::listed(only, feature)) {
        return false;
    }
    static std::atomic<int> logged{-1};
    const int arm =
        ab_detail::arm_at(std::chrono::steady_clock::now(), period);
    if (logged.exchange(arm, std::memory_order_relaxed) != arm) {
        std::fprintf(stderr, "[ab-cycle] arm=%s\n", arm == 0 ? "on" : "off");
    }
    return arm == 1;
}

/// The arm the current window belongs to, for tagging measurement lines:
/// "on", "off", or "none" when no cycle runs (VITREO_AB_CYCLE_S unset and
/// nothing forced). Same clock as ab_cycle_off, so a line tagged "off" was
/// measured while every cycled change was off.
[[nodiscard]] inline const char* ab_cycle_arm_name(
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) noexcept {
    const int forced = ab_detail::forced().load(std::memory_order_relaxed);
    if (forced != 0) {
        return forced == static_cast<int>(AbForce::Off) ? "off" : "on";
    }
    const u64 period = ab_detail::period_seconds();
    if (period == 0) return "none";
    return ab_detail::arm_at(now, period) == 0 ? "on" : "off";
}

/// ab_cycle_off for call sites that run millions of times a second (an API
/// crossing, a per-draw path). The full check reads the clock and does an
/// atomic exchange on a line every thread shares; this one re-reads the arm
/// once per kResample calls of the calling thread and answers from its own
/// copy in between, so an arm change is seen within ~1k calls (well under a
/// millisecond on any thread this hot). One sampler per feature and thread.
struct AbCycleSampler {
    static constexpr u32 kResample = 1024;
    u32 countdown{0};
    bool off{false};
};
[[nodiscard]] inline bool ab_cycle_off_sampled(AbCycleSampler& sampler,
                                               const char* feature) noexcept {
    if (sampler.countdown != 0) {
        --sampler.countdown;
        return sampler.off;
    }
    sampler.countdown = AbCycleSampler::kResample - 1;
    sampler.off = ab_cycle_off(feature);
    return sampler.off;
}

}  // namespace vitreo::diagnostics
