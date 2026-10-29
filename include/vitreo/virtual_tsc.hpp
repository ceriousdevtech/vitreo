#pragma once

/// @file virtual_tsc.hpp
/// @brief One monotonic host clock behind both RDTSC and
///        QueryPerformanceCounter.
///
/// Windows 7-era guests treat QPC as the TSC: they call
/// QueryPerformanceFrequency once and then read either QPC or RDTSC. Those two
/// surfaces must therefore stay in a fixed, stable relationship. Host
/// mach_absolute_time ticks at 24 MHz on Apple Silicon, so exposing raw host
/// ticks in one surface and nanoseconds in the other makes elapsed time look
/// ~40x too large. Everything here is derived from `virtual_monotonic_ns()` so
/// that cannot happen.
///
/// They must share a timebase; they must NOT share a *rate*. Real Windows
/// reports QPF = 10 MHz while the TSC ticks in the GHz range, and matching
/// those separately is a compatibility requirement rather than cosmetics:
///
///   - A QPC delta at the old 1 GHz rate crossed INT32_MAX after ~2.1 seconds.
///     A 32-bit guest that stores a QPC delta, or an intermediate product such
///     as `delta * 1000`, in a 32-bit integer overflowed under Vitreo where it
///     never would on Windows. The wrapped value becomes a huge or negative
///     elapsed time. At 10 MHz that same delta takes ~214 seconds, matching
///     real hardware.
///   - A 1 GHz TSC also contradicted the GHz-class CPU Vitreo advertises.
///
/// The TSC rate is the host's real core clock: the highest clock of its
/// fastest core class (the P-core maximum on Apple silicon, 4512 MHz on an M4
/// Pro). Apple cores have no fixed base clock, so this is the nominal rate a
/// TSC would tick at. CPUID leaves 0x15/0x16 and the Windows processor clock
/// surfaces report this same rate.

#include "vitreo/types.hpp"

#include <mach/mach_time.h>

namespace vitreo {

/// QueryPerformanceFrequency, matching what modern Windows reports.
inline constexpr u64 kQpcHz = 10'000'000ULL;

/// Host core clock in Hz from the host hardware profile (platform layer), or
/// 0 when the host does not expose one.
[[nodiscard]] u64 host_max_core_hz_for_tsc() noexcept;

/// Guest TSC rate when the host exposes no core clock.
inline constexpr u64 kFallbackGuestTscHz = 3'000'000'000ULL;

/// Guest TSC rate: the host's core clock, fixed for the life of the process
/// so RDTSC-against-QPC calibration converges.
[[nodiscard]] inline u64 guest_tsc_hz() noexcept {
    static const u64 hz = [] {
        const u64 host = host_max_core_hz_for_tsc();
        return host != 0 ? host : kFallbackGuestTscHz;
    }();
    return hz;
}

inline constexpr u64 kNsPerSecond = 1'000'000'000ULL;

/// The single monotonic source. Every guest-visible high-resolution clock is
/// a scaling of this, so they cannot drift apart. Use mach_continuous_time so
/// QPC advances across host sleep the same way GetTickCount does (Learn).
[[nodiscard]] inline u64 virtual_monotonic_ns() noexcept {
    static mach_timebase_info_data_t info = []() {
        mach_timebase_info_data_t i{};
        mach_timebase_info(&i);
        return i;
    }();
    const u64 ticks = mach_continuous_time();
    return ticks * static_cast<u64>(info.numer) / static_cast<u64>(info.denom);
}

/// QueryPerformanceCounter, in kQpcHz units.
[[nodiscard]] inline u64 virtual_qpc_ticks() noexcept {
    static_assert(kNsPerSecond % kQpcHz == 0,
                  "QPC period must divide a nanosecond count exactly");
    return virtual_monotonic_ns() / (kNsPerSecond / kQpcHz);
}

/// RDTSC/RDTSCP, in guest_tsc_hz() units. The 128-bit intermediate keeps the
/// scaling exact for any uptime the host can reach.
[[nodiscard]] inline u64 virtual_tsc_ticks() noexcept {
    const auto scaled = static_cast<unsigned __int128>(virtual_monotonic_ns()) *
                        guest_tsc_hz();
    return static_cast<u64>(scaled / kNsPerSecond);
}

}  // namespace vitreo
