#pragma once

/// @file metal_gpu_watch.hpp
/// @brief Name the GPU work that never finishes, before the Mac freezes.
///
/// A command buffer that runs forever -- a shader stuck in a loop -- pins the
/// GPU at 100%. WindowServer then cannot composite, the display stops, and
/// the whole machine looks frozen; killing the guest does not stop work the
/// GPU already accepted. One guest did this repeatedly with memory to
/// spare, and nothing in any log said which work it was.
///
/// Every command buffer the recorder commits is registered here with a
/// summary of what it holds (pipeline labels in encode order, render
/// passes). A watchdog thread reports the OLDEST unfinished one once it is
/// 2 s old -- the GPU retires a queue in order, so that is the stuck one --
/// and exits the process at 4 s so it stops queueing more work behind it.
/// VITREO_GPU_WATCHDOG=0 disables it.

#include <vitreo/common.hpp>

#include <string>
#include <vector>

namespace vitreo::graphics::gpu_watch {

[[nodiscard]] bool enabled() noexcept;

/// Append @p label to @p summary (a command buffer's contents), skipping an
/// immediate repeat and capping the summary's length.
void note(std::string& summary, const char* label);

/// An encodeWaitForEvent recorded into a command buffer. A buffer blocked on
/// one stays MTLCommandBufferStatusCommitted (measured: a buffer queued
/// behind a long kernel is Scheduled; only an unsatisfied event wait keeps
/// it at Committed), so the watchdog names the event and the value it is
/// waiting for next to the event's current value.
struct EventWait {
    void* event{nullptr};  // id<MTLEvent> / id<MTLSharedEvent>, retained by track
    u64 value{0};
};

/// Register @p command_buffer (an id<MTLCommandBuffer>, not yet committed)
/// with its contents. Installs a completion handler; call before -commit.
void track(void* command_buffer, std::string summary,
           std::vector<EventWait> waits = {});

/// Report @p command_buffer's GPU interval to graphics::note_gpu_interval
/// (the performance overlay) when it completes, if a reader wants it now.
/// track() does this itself; a command buffer committed without track()
/// calls it directly.
void time_busy(void* command_buffer);

}  // namespace vitreo::graphics::gpu_watch
