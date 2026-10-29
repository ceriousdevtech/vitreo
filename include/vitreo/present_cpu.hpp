// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file present_cpu.hpp
/// @brief `[present-cpu]`: CPU time per presented frame, per thread.
///
/// MEASUREMENT INSTRUMENT, on only under VITREO_PRESENT_RATE. The frame rate
/// alone cannot say WHICH thread a change took time off: a pipeline whose
/// threads wait on one another turns time removed from one of them into
/// waiting. This meter reads each thread's own CPU clock (Mach
/// thread_info(THREAD_BASIC_INFO), user + system) and reports, once per
/// second, how many milliseconds of CPU each thread spent per presented
/// frame:
///
///   - the thread that presents (read at every present; UE presents from its
///     RHI thread), with the mean and the largest single-frame delta;
///   - every guest thread that has a name (SetThreadDescription or the
///     MS_VC_EXCEPTION convention), read once per window.
///
/// Every line carries the `[ab-cycle]` arm its window ran in
/// (ab_cycle_arm_name), and a window never spans an arm change: it closes at
/// the first present of the new arm. With VITREO_AB_CYCLE_S the two arms of
/// one process can therefore be compared thread by thread.
///
/// Cost: one thread_info() per present on the presenting thread plus one per
/// named thread per second; no allocation after a thread is registered.

#include <vitreo/types.hpp>

#include <chrono>
#include <cstdio>
#include <string_view>

namespace vitreo::diagnostics::present_cpu {

/// Host monotonic time in milliseconds (std::chrono::steady_clock since its
/// epoch). Every meter line that prints `t_ms=` ([present-cpu],
/// [present-rate], [coreaudio-voice], [wasapi-clock], [wasapi-client]) uses
/// this one base, so their events can be correlated directly.
[[nodiscard]] inline double host_mono_ms() noexcept {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// True when VITREO_PRESENT_RATE is set (read once).
[[nodiscard]] bool enabled() noexcept;

/// CPU time (user + system) consumed so far by the thread whose Mach thread
/// port is @p mach_thread, in nanoseconds. Microsecond resolution (the
/// kernel's time_value_t). Returns false when the port names no live thread.
[[nodiscard]] bool thread_cpu_ns(unsigned int mach_thread, u64& out) noexcept;

/// The calling thread's Mach thread port (no reference is added).
[[nodiscard]] unsigned int current_thread_port() noexcept;

/// The calling thread presented a frame. No-op unless enabled().
void note_present() noexcept;

/// Guest thread @p guest_tid was named @p name. When @p mach_thread is
/// nonzero it is that thread's Mach port; the meter takes its own send right,
/// so the caller keeps its own. A zero port records the name only; a later
/// note_thread_port() for the same tid completes the entry. No-op unless
/// enabled(). Renaming a thread replaces its name.
void note_thread_name(u32 guest_tid, std::u16string_view name,
                      unsigned int mach_thread) noexcept;

/// The name the meter holds for guest thread @p guest_tid, as UTF-8 into
/// @p out (always terminated). False, with @p out empty, when the meter is
/// off or the thread was never named. For diagnostics that label threads.
[[nodiscard]] bool thread_name(u32 guest_tid, char* out,
                               usize capacity) noexcept;

/// The host thread executing guest thread @p guest_tid is the CALLING thread
/// (it has just been bound). Completes an entry whose name came first, e.g. a
/// thread named while it was still suspended. No-op for an unnamed tid.
void note_thread_port(u32 guest_tid) noexcept;

// -- Tests ----------------------------------------------------------------

/// Route the lines to @p out (nullptr: stderr), shorten the window from 1 s,
/// and enable the meter regardless of the environment.
void configure_for_test(std::FILE* out, u64 window_ns) noexcept;
/// Forget every registered thread and the open window.
void reset_for_test() noexcept;

}  // namespace vitreo::diagnostics::present_cpu
