#pragma once

/// @file time_diag.hpp
/// @brief VITREO_TIME_DIAG — guest-clock continuity and cross-domain drift.
///
/// A guest that drives a simulation from elapsed time needs every clock Vitreo
/// exposes to advance together. Vitreo answers the Win32 timing APIs from two
/// host sources: the millisecond APIs (GetTickCount, GetTickCount64,
/// timeGetTime) read `mach_continuous_time`, while QueryPerformanceCounter and
/// RDTSC read the 1 GHz virtual TSC in `virtual_tsc.hpp`. Those are separate
/// host clocks with separate epochs, so nothing but measurement proves they
/// keep the same *rate*.
///
/// This records two independent classes of defect:
///
///   1. Per-call-site and per-thread discontinuity — a clock running backwards,
///      wrapping, or jumping forward by an implausible interval. A guest that
///      computes `now - previous` and scales a decay rate by the result turns
///      one such jump into an enormous simulation step.
///   2. Cross-domain drift — the millisecond domain and the 1 GHz domain
///      disagreeing about how much time has passed. This is the failure the
///      virtual TSC exists to prevent (host mach ticks are 24 MHz on Apple
///      Silicon, so mixing raw domains inflates elapsed time ~40x), and the one
///      a per-clock monotonicity check cannot see: each domain can be perfectly
///      monotonic on its own while disagreeing with the other.
///
/// Entirely opt-in. With `VITREO_TIME_DIAG` unset every entry point costs one
/// predictable branch on a cached flag, so instrumenting a call site does not
/// change the cost of the loop that contains it.

#include "vitreo/types.hpp"

namespace vitreo {

/// The guest-visible clock a sample came from. The millisecond APIs are kept
/// distinct rather than merged because they are separate guest contracts with
/// separate widths, and a defect can appear in one without the others.
enum class TimeApi : u8 {
    GetTickCount,    ///< DWORD ms, wraps every ~49.7 days.
    GetTickCount64,  ///< ULONGLONG ms.
    TimeGetTime,     ///< DWORD ms, wraps every ~49.7 days.
    Qpc,             ///< QueryPerformanceCounter, 1 GHz virtual TSC.
    Rdtsc,           ///< RDTSC/RDTSCP, same 1 GHz virtual TSC.
};

/// Whether `VITREO_TIME_DIAG` was set in the environment. Cached after the
/// first call.
[[nodiscard]] bool time_diag_enabled() noexcept;

/// Record one clock reading. @p raw_value is exactly what the guest will see,
/// in that API's own units, before any truncation to 32 bits. @p caller is the
/// guest return address when the caller can supply one cheaply, or 0 to group
/// the sample by API alone.
///
/// Only call after `time_diag_enabled()` returns true; `time_diag_observe`
/// applies that gate for you.
void time_diag_record(TimeApi api, u64 raw_value, u64 caller) noexcept;

/// Gated form of `time_diag_record`. This is the one call sites should use.
inline void time_diag_observe(TimeApi api, u64 raw_value,
                              u64 caller = 0) noexcept {
    if (!time_diag_enabled()) return;
    time_diag_record(api, raw_value, caller);
}

/// Write the per-call-site summary to stderr. Called at guest exit so a run
/// that produced no anomaly still reports what it observed — an empty log
/// otherwise cannot be distinguished from instrumentation that never ran.
void time_diag_report_summary() noexcept;

}  // namespace vitreo
