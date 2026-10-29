// SPDX-License-Identifier: LGPL-2.1-only

/// @file gpu_busy.hpp
/// @brief How much of the last second the GPU spent on this process's work,
///        for the performance overlay.
///
/// Each committed command buffer reports its GPUStartTime/GPUEndTime; the
/// busy time is the union of those intervals, so command buffers from
/// several queues that overlap on the GPU are not counted twice. Recording
/// costs a completion handler per command buffer, so it only happens while
/// a reader (the overlay) asks for it: set_gpu_busy_wanted.

#pragma once

#include <vitreo/common.hpp>

namespace vitreo::graphics {

/// Whether command buffers should report their GPU interval now. One
/// relaxed load: the commit path checks it on every command buffer.
[[nodiscard]] bool gpu_busy_wanted() noexcept;
void set_gpu_busy_wanted(bool wanted) noexcept;

/// One command buffer ran on the GPU from @p start_s to @p end_s, in
/// seconds of gpu_busy_clock_s (Metal's GPUStartTime/GPUEndTime clock).
/// @p device_allocated_bytes is the device's currentAllocatedSize then.
void note_gpu_interval(double start_s, double end_s,
                       u64 device_allocated_bytes) noexcept;

struct RecentGpuBusy {
    double busy_ms = 0.0;          ///< GPU busy time in the window
    double window_ms = 0.0;        ///< length of the window
    u32 command_buffers = 0;       ///< command buffers that ran in it
    u64 allocated_bytes = 0;       ///< latest device allocation (0: unknown)
};

/// The second before @p now_s (gpu_busy_clock_s).
[[nodiscard]] RecentGpuBusy recent_gpu_busy(double now_s) noexcept;

/// mach_absolute_time in seconds: the clock CACurrentMediaTime and Metal's
/// GPUStartTime/GPUEndTime are on.
[[nodiscard]] double gpu_busy_clock_s() noexcept;

/// Forget every recorded interval (tests).
void reset_gpu_busy_for_testing() noexcept;

} // namespace vitreo::graphics
