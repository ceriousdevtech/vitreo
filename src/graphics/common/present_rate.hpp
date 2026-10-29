// SPDX-License-Identifier: LGPL-2.1-only

/// @file present_rate.hpp
/// @brief How often the guest's swapchain actually reaches the screen.
///
/// The frame rate is what a user calls "sluggish", and until this existed it
/// was the one number nobody had. The Metal *surface presenter* carries
/// perf_diag counters, but an out-of-process swapchain never goes through it:
/// the browser process's GDI framebuffer is never dirty,
/// the GPU process owns a CAMetalLayer, and `MetalSwapchain::present` had no
/// instrumentation at all. Twenty-five seconds of a running app produced no
/// presentation telemetry of any kind.
///
/// Recording is two relaxed atomics per present, which is nothing next to a
/// full-screen blit and a submit, so it is always on rather than env-gated —
/// a frame-rate question usually arrives after the interesting run, not
/// before it. The report is wired into the SIGUSR1 hang dump for the same
/// reason the input-latency table is: a GUI guest is killed and never reaches
/// std::atexit.

#pragma once

#include <vitreo/common.hpp>

#include <cstdio>

namespace vitreo::graphics {

/// One presented frame just went to the compositor.
void note_present(bool vsync) noexcept;

/// `next_drawable()` returned null and the caller dropped the frame. This is
/// the load-INDEPENDENT measure: an idle app presents almost nothing, so frame
/// counts cannot tell a pacing fix from a quiet minute, but the ratio of
/// dropped to delivered acquisitions characterises the pipeline either way.
void note_drawable_skip() noexcept;

/// The size of the frame just presented, for the performance overlay.
void note_present_size(u32 width, u32 height) noexcept;

/// The frames presented in the second before @p now_ns (present_clock_ns).
struct RecentPresents {
    u32 frames = 0;           ///< frames presented in that second
    double average_ms = 0.0;  ///< mean interval between consecutive frames
    double worst_ms = 0.0;    ///< longest interval, the one still open included
    u32 width = 0;            ///< size of the most recent frame (0: none yet)
    u32 height = 0;
};
[[nodiscard]] RecentPresents recent_presents(u64 now_ns) noexcept;

/// The intervals between the most recent presents, in milliseconds, oldest
/// first: at most @p max of them, none older than @p window_ns before
/// @p now_ns. Returns how many were written to @p out.
usize recent_frame_intervals_ms(u64 now_ns, u64 window_ns, float* out,
                                usize max) noexcept;

/// The monotonic clock presents are timed on, in nanoseconds.
[[nodiscard]] u64 present_clock_ns() noexcept;

/// Forget every recorded present (tests).
void reset_present_rate_for_testing() noexcept;

/// Frame count, elapsed wall time, average rate and an interval histogram.
/// Safe to call repeatedly and from a signal-servicing thread.
void present_rate_report(std::FILE* out) noexcept;

} // namespace vitreo::graphics
