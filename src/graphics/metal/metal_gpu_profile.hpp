#pragma once

/// @file metal_gpu_profile.hpp
/// @brief Where the GPU's time goes, one encoder at a time.
///
/// `VITREO_GPU_WATCH_STATS` says how busy the GPU is. This says which work
/// keeps it busy. With `VITREO_GPU_PROFILE=1`, every render, compute and blit
/// encoder the recorder opens is created with a timestamp counter sample
/// buffer attached at its stage boundaries (the only sampling point Apple
/// GPUs support). No command buffer is split and no draw is reordered, so the
/// frame being measured is the frame that ships.
///
/// When a command buffer completes, its samples are resolved and added to a
/// table keyed by what the pass IS: its attachments (size, pixel formats,
/// load/store actions) and its first pipeline, or the kernel for compute.
/// Every `VITREO_GPU_PROFILE_SECONDS` (default 5) the table is written to
/// stderr as `[gpu-profile]` lines, heaviest first, and reset.
///
/// Pipeline labels are `vs:<hash>/fs:<hash>` or `cs:<hash>`, the same tags
/// that `VITREO_DUMP_MSL_DIR` names the translated MSL files with.
///
/// Two limits of the measurement:
/// - A render pass's time is its footprint, from its first stage start to its
///   last stage end. On a tile-based GPU one pass's vertex work can overlap
///   the previous pass's fragment work, so footprints can add up to more than
///   the GPU-busy total.
/// - A pass with no draws may carry no valid samples. It is counted, with no
///   time.

#include <vitreo/common.hpp>

#include <memory>

namespace vitreo::graphics::gpu_profile {

[[nodiscard]] bool enabled() noexcept;

/// The profiled encoders of one command buffer, owned by the recorder while
/// it records and handed to attach() when the buffer commits.
struct Recording;
struct RecordingDeleter {
    void operator()(Recording* recording) const noexcept;
};
using RecordingPtr = std::unique_ptr<Recording, RecordingDeleter>;

/// Create an encoder on @p command_buffer with timestamp samples attached.
/// @p queue is the recorder's id<MTLCommandQueue>. Each returns a +1 retained
/// encoder (take it with __bridge_transfer), or nullptr when sampling is
/// unavailable or this buffer's samples are used up; the caller then creates
/// its encoder the ordinary way and that pass goes unmeasured.
[[nodiscard]] void* begin_render(RecordingPtr& recording, void* queue,
                                 void* command_buffer,
                                 void* render_pass_descriptor);
[[nodiscard]] void* begin_compute(RecordingPtr& recording, void* queue,
                                  void* command_buffer);
[[nodiscard]] void* begin_blit(RecordingPtr& recording, void* queue,
                               void* command_buffer);

/// The pipeline bound in the current pass changed to @p label.
void note_pipeline(Recording* recording, const char* label);
/// One draw, dispatch or blit operation in the current pass.
void note_work(Recording* recording) noexcept;

/// Resolve @p recording when @p command_buffer (not yet committed)
/// completes, and fold it into the report. Accepts a null recording.
void attach(void* command_buffer, RecordingPtr recording);

}  // namespace vitreo::graphics::gpu_profile
