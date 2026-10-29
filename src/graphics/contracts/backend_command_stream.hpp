// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_command_stream.hpp
/// @brief GPU command-stream objects: completion, timestamp and occlusion handles, command lists, and timeline fences.

#include <vitreo/common.hpp>

namespace vitreo::graphics {

/// Wait handle for GPU work submitted at a specific flush. Waiting must not
/// observe command buffers committed after this object was created.
class GpuCompletion {
public:
    virtual ~GpuCompletion() = default;
    [[nodiscard]] virtual bool wait() = 0;
};

/// A point in the GPU command stream: D3D11 End on a TIMESTAMP, EVENT or
/// TIMESTAMP_DISJOINT query. It becomes ready once the GPU has finished every
/// command recorded before it; gpu_time_ns() is then the host-clock time, in
/// nanoseconds, at which it did. Polling never blocks and never submits.
class GpuTimestamp {
public:
    virtual ~GpuTimestamp() = default;
    [[nodiscard]] virtual bool ready() const = 0;
    /// Valid only once ready() has returned true.
    [[nodiscard]] virtual u64 gpu_time_ns() const = 0;
};

/// A GPU sample counter: the number of samples that passed the depth and
/// stencil tests in every render pass created with it
/// (BackendRenderPassDesc::occlusion_counter), summed across passes and
/// command buffers. D3D11 OCCLUSION / OCCLUSION_PREDICATE queries (D3D11.3
/// 20.4.6: "the number of samples that passed the depth and stencil tests")
/// open one per Begin/End interval, so overlapping brackets each sum exactly
/// the intervals they span. Metal counts it with a visibility result buffer in
/// MTLVisibilityResultModeCounting, which counts samples, not pixels.
class GpuOcclusionCounter {
public:
    virtual ~GpuOcclusionCounter() = default;
    /// The count. Valid only once the GPU has completed every pass that
    /// counted into it: recorded_generation().
    [[nodiscard]] virtual u64 samples() const = 0;
    /// The newest submission generation (GraphicsBackendDevice::
    /// recording_generation) holding an op -- a BeginPass or a draw -- of a
    /// render pass that counted into this counter, taken by the backend from
    /// the recording each op went into (last_recorded_generation) -- never a
    /// generation before it, and always one that is submitted once the
    /// recording is flushed. samples() is final once it has completed. A
    /// caller must not substitute a generation it read before creating the
    /// pass or recording the draw: another thread can submit in between, and
    /// the op then lands in the next one. 0 when no pass on the immediate stream has counted
    /// into it (none yet, or only passes captured into a command list, whose
    /// generation is not known when they are recorded).
    [[nodiscard]] virtual u64 recorded_generation() const = 0;
};

/// GraphicsBackendDevice::draw_encode_counts.
struct BackendDrawEncodeCounts {
    u64 reopened{0};
    u64 dropped{0};
};

// =============================================================================
// Command lists
// =============================================================================
//
// The immediate path records operations into the backend's own stream and a
// worker replays them. A command list is the same recording given an identity: a
// frontend builds one, closes it, and submits it later — possibly more than once.
//
// Two guest contracts want exactly this: `ID3D11Device::CreateDeferredContext`
// with `FinishCommandList`/`ExecuteCommandList`, and every D3D12
// `ID3D12GraphicsCommandList`. Both record without executing, which is the
// defining property — nothing may reach the GPU until the list is submitted.
//
// Capture is per thread, because that is the threading contract both APIs state:
// a D3D11 deferred context and a D3D12 command list may each be recorded from one
// thread at a time. Two threads may therefore build two lists concurrently
// without either seeing the other's operations.

class BackendCommandList {
public:
    virtual ~BackendCommandList() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;

    /// Direct this thread's recorded operations into this list instead of the
    /// backend's immediate stream. Refused if this thread is already capturing,
    /// or if the list is closed.
    [[nodiscard]] virtual bool begin_capture() = 0;
    /// Stop directing operations here. Refused if this thread is not capturing
    /// into this list.
    [[nodiscard]] virtual bool end_capture() = 0;

    /// Stop accepting operations. D3D11 `FinishCommandList`, D3D12 `Close`.
    [[nodiscard]] virtual bool close() = 0;
    [[nodiscard]] virtual bool is_closed() const noexcept = 0;

    /// Discard the recording and accept operations again. D3D12 `Reset`.
    [[nodiscard]] virtual bool reset() = 0;

    /// Operations recorded so far. The honest way for a caller to tell an empty
    /// list from one that recorded nothing because recording was misdirected.
    [[nodiscard]] virtual usize operation_count() const noexcept = 0;
};

/// Scope guard for `begin_capture`/`end_capture`.
///
/// Forgetting `end_capture` would send every later operation on this thread into
/// a list nobody submits, so the work would silently never run. Prefer this to
/// calling the pair by hand.
class BackendCommandListCapture {
public:
    explicit BackendCommandListCapture(BackendCommandList* list)
        : m_list(list), m_active(list != nullptr && list->begin_capture()) {}
    ~BackendCommandListCapture() {
        if (m_active) (void)m_list->end_capture();
    }
    BackendCommandListCapture(const BackendCommandListCapture&) = delete;
    BackendCommandListCapture& operator=(const BackendCommandListCapture&) =
        delete;

    /// Whether capture actually started. False means operations are still going
    /// to the immediate stream.
    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    BackendCommandList* m_list;
    bool m_active;
};

// =============================================================================
// Timeline fences
// =============================================================================
//
// `GpuCompletion` waits for one submission. A timeline is different: a monotonic
// 64-bit counter that work signals and that other work — on the GPU, on this CPU,
// or in another process — waits on by value.
//
// Three guest contracts need exactly this and have none of it today:
// `ID3D11Device5::CreateFence` with `ID3D11DeviceContext4::Signal`/`Wait`, every
// `ID3D12Fence`, and cross-process resource handoff, where command-queue ordering
// does not cross the process boundary. Metal's `MTLSharedEvent` is a direct match
// and is shareable, which is why it backs all three.
//
// Ordering matters as much as the value. A signal enqueued between two draws must
// land between them, so signal and wait are recorded into the command stream
// rather than applied immediately; only the CPU-side operations act at once.

class BackendFence {
public:
    virtual ~BackendFence() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;

    /// The highest value the timeline has reached.
    [[nodiscard]] virtual u64 completed_value() const = 0;

    /// Signal @p value once all work recorded before this call has completed.
    /// Recorded in order with draws and dispatches, not applied immediately.
    [[nodiscard]] virtual bool signal(u64 value) = 0;

    /// Make work recorded after this call wait until the timeline reaches
    /// @p value. Also recorded in order.
    [[nodiscard]] virtual bool wait_gpu(u64 value) = 0;

    /// Set the timeline from the CPU. `ID3D11Fence` has no such operation, but
    /// `ID3D12Fence::Signal` does, and a CPU signal is how a waiting GPU is
    /// released by the host. The value is SET, even below the current one
    /// (ID3D12Fence::Signal does; d12core_facts_probe).
    [[nodiscard]] virtual bool signal_cpu(u64 value) = 0;

    /// Block the calling thread until the timeline reaches @p value, or the
    /// timeout elapses. Returns whether the value was reached.
    [[nodiscard]] virtual bool wait_cpu(u64 value, u64 timeout_ns) = 0;
};

} // namespace vitreo::graphics
