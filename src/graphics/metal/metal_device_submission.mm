// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_device_submission.mm
/// @brief MetalDevice command lists, fences and the submission timeline

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <thread>
#include <deque>
#include <unistd.h>
#include <condition_variable>
#include <vector>

using namespace vitreo;

namespace vitreo::graphics {

namespace {

/// A command list over one of the submit queue's recordings.
///
/// The queue owns the operations because `Op` is private to it; this is the
/// guest-facing identity and the state machine around it.
class MetalBackendCommandList final : public BackendCommandList {
public:
    MetalBackendCommandList()
        : m_id(MetalSubmitQueue::instance().create_command_list()) {}

    ~MetalBackendCommandList() override {
        if (m_id != 0) MetalSubmitQueue::instance().destroy_command_list(m_id);
    }

    [[nodiscard]] bool valid() const noexcept override { return m_id != 0; }

    [[nodiscard]] bool begin_capture() override {
        if (m_id == 0 || m_closed) return false;
        return MetalSubmitQueue::instance().begin_capture(m_id);
    }

    [[nodiscard]] bool end_capture() override {
        if (m_id == 0) return false;
        return MetalSubmitQueue::instance().end_capture(m_id);
    }

    [[nodiscard]] bool close() override {
        if (m_id == 0 || m_closed) return false;
        // Closing while still capturing would leave the thread appending into a
        // list that no longer accepts operations.
        if (!MetalSubmitQueue::instance().end_capture(m_id)) {
            // Not capturing on this thread, which is the normal case.
        }
        m_closed = true;
        return true;
    }

    [[nodiscard]] bool is_closed() const noexcept override { return m_closed; }

    [[nodiscard]] bool reset() override {
        if (m_id == 0) return false;
        MetalSubmitQueue::instance().reset_command_list(m_id);
        m_closed = false;
        return true;
    }

    [[nodiscard]] usize operation_count() const noexcept override {
        if (m_id == 0) return 0;
        return MetalSubmitQueue::instance().command_list_size(m_id);
    }

    [[nodiscard]] MetalSubmitQueue::CommandListId id() const noexcept {
        return m_id;
    }

private:
    MetalSubmitQueue::CommandListId m_id{0};
    bool m_closed{false};
};

/// A timeline fence on MTLSharedEvent.
///
/// Signal and wait are RECORDED into the command stream so they order against the
/// draws around them; only the CPU-side operations act immediately. A signal
/// applied straight to `signaledValue` would be observed before the work it is
/// meant to follow, which is the whole thing a fence exists to prevent.
class MetalBackendFence final : public BackendFence {
public:
    MetalBackendFence(void* device, u64 initial_value) {
        @autoreleasepool {
            id<MTLSharedEvent> event =
                [(__bridge id<MTLDevice>)device newSharedEvent];
            if (event == nil) return;
            event.signaledValue = initial_value;
            m_event = (__bridge_retained void*)event;
        }
    }

    ~MetalBackendFence() override {
        if (m_event != nullptr) CFRelease(m_event);
    }

    [[nodiscard]] bool valid() const noexcept override {
        return m_event != nullptr;
    }

    [[nodiscard]] u64 completed_value() const override {
        if (m_event == nullptr) return 0;
        @autoreleasepool {
            return static_cast<u64>(
                ((__bridge id<MTLSharedEvent>)m_event).signaledValue);
        }
    }

    [[nodiscard]] bool signal(u64 value) override {
        if (m_event == nullptr) return false;
        MetalSubmitQueue::instance().record_signal_fence(m_event, value);
        return true;
    }

    [[nodiscard]] bool wait_gpu(u64 value) override {
        if (m_event == nullptr) return false;
        MetalSubmitQueue::instance().record_wait_fence(m_event, value);
        return true;
    }

    [[nodiscard]] bool signal_cpu(u64 value) override {
        if (m_event == nullptr) return false;
        @autoreleasepool {
            id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)m_event;
            // An MTLSharedEvent never runs backwards: a lower value would be
            // ignored, so it is refused, and a frontend whose API lowers a
            // fence (ID3D12Fence::Signal) replaces the timeline instead.
            if (value < static_cast<u64>(event.signaledValue)) return false;
            event.signaledValue = value;
        }
        return true;
    }

    [[nodiscard]] bool wait_cpu(u64 value, u64 timeout_ns) override {
        if (m_event == nullptr) return false;
        @autoreleasepool {
            id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)m_event;
            if (static_cast<u64>(event.signaledValue) >= value) return true;
            // The GPU cannot reach the value while the work that signals it is
            // still sitting in the recording, so submit first. `submit()` and not
            // `drain()`: drain waits for GPU completion, which deadlocks the
            // legitimate case of a GPU wait released by a later CPU signal.
            (void)MetalSubmitQueue::instance().submit();
            const u64 timeout_ms = timeout_ns / 1000000ull;
            return [event waitUntilSignaledValue:value
                                       timeoutMS:timeout_ms == 0 && timeout_ns > 0
                                                     ? 1u
                                                     : timeout_ms];
        }
    }

private:
    void* m_event{nullptr};
};

} // namespace

bool MetalDevice::supports_command_lists() const noexcept {
    return available();
}

std::unique_ptr<BackendCommandList> MetalDevice::create_command_list() {
    if (!available()) return nullptr;
    auto list = std::make_unique<MetalBackendCommandList>();
    if (!list->valid()) return nullptr;
    return list;
}

bool MetalDevice::submit_command_list(BackendCommandList* list) {
    if (!available() || list == nullptr || !list->valid()) return false;
    // A list must be closed before it runs. D3D11 hands one back only from
    // FinishCommandList and D3D12 requires Close, so submitting an open one is a
    // caller error -- and an open list may still be mid-recording.
    if (!list->is_closed()) return false;
    auto* metal = static_cast<MetalBackendCommandList*>(list);
    return MetalSubmitQueue::instance().submit_command_list(metal->id());
}

bool MetalDevice::supports_fences() const noexcept { return available(); }

std::unique_ptr<BackendFence> MetalDevice::create_fence(u64 initial_value) {
    if (!available()) return nullptr;
    auto fence = std::make_unique<MetalBackendFence>(native_device(),
                                                     initial_value);
    if (!fence->valid()) return nullptr;
    return fence;
}

bool MetalDevice::wants_early_submit() const {
    if (!available()) return false;
    auto& queue = MetalSubmitQueue::instance();
    // recording_generation() is the last submission + 1; everything up to
    // completed_generation() has retired. At most one list left in flight:
    // the GPU will be idle once it finishes, unless more is submitted.
    const u64 submitted = queue.recording_generation() - 1;
    return submitted - queue.completed_generation() <= 1;
}

void MetalDevice::flush() {
    if (!available()) return;
    // Learn, ID3D11DeviceContext::Flush: "Sends queued-up commands in the
    // command buffer to the GPU." It does not wait for them. GetData without
    // D3D11_ASYNC_GETDATA_DONOTFLUSH is the same submit (the poll loop then
    // sees S_FALSE until the GPU retires the query). A caller that must
    // observe completion uses flush_completion()/wait_generation. This used
    // to drain() the whole GPU, which hid a CPU/GPU race in DISCARD renaming
    // and cost ~40% of menu frame rate (the render thread sat in drain()
    // under the device lock).
    (void)MetalSubmitQueue::instance().submit();
}

namespace {

class MetalGenerationCompletion final : public GpuCompletion {
public:
    explicit MetalGenerationCompletion(u64 generation)
        : m_generation(generation) {}
    bool wait() override {
        return MetalSubmitQueue::instance().wait_for_generation(m_generation);
    }

private:
    u64 m_generation{0};
};

} // namespace

std::shared_ptr<GpuCompletion> MetalDevice::flush_completion() {
    if (!available()) {
        return nullptr;
    }
    const u64 generation = MetalSubmitQueue::instance().submit();
    return std::make_shared<MetalGenerationCompletion>(generation);
}

namespace {

class MetalGpuTimestamp final : public GpuTimestamp {
public:
    explicit MetalGpuTimestamp(std::shared_ptr<MetalTimestampCell> cell)
        : m_cell(std::move(cell)) {}
    bool ready() const override {
        return m_cell->done.load(std::memory_order_acquire);
    }
    u64 gpu_time_ns() const override {
        return m_cell->gpu_ns.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<MetalTimestampCell> m_cell;
};

} // namespace

std::shared_ptr<GpuTimestamp> MetalDevice::record_timestamp() {
    if (!available()) return nullptr;
    auto cell = std::make_shared<MetalTimestampCell>();
    MetalSubmitQueue::instance().record_timestamp(cell);
    return std::make_shared<MetalGpuTimestamp>(std::move(cell));
}

u64 MetalDevice::last_recorded_generation() {
    return MetalSubmitQueue::last_recorded_generation();
}

BackendDrawEncodeCounts MetalDevice::draw_encode_counts() const {
    const auto& queue = MetalSubmitQueue::instance();
    return {queue.draws_reopened(), queue.draws_dropped()};
}

u64 MetalDevice::recording_generation() {
    // Captured work joins the immediate stream only when the command list
    // is executed, at a generation not known yet.
    if (!available() || MetalSubmitQueue::thread_capturing()) return 0;
    return MetalSubmitQueue::instance().recording_generation();
}

bool MetalDevice::generation_complete(u64 generation) {
    if (!available() || generation == 0) return false;
    return MetalSubmitQueue::instance().completed_generation() >= generation;
}

bool MetalDevice::wait_generation(u64 generation) {
    if (!available()) return false;
    if (generation == 0) return wait_idle();
    auto& queue = MetalSubmitQueue::instance();
    if (queue.completed_generation() >= generation) return true;
    // submit() hands over the recording (if any) and returns the newest
    // submitted generation. A generation above it was never given work --
    // the recording it named was empty -- so everything up to the newest
    // submission is all there is to wait for.
    const u64 submitted = queue.submit();
    return queue.wait_for_generation(std::min(generation, submitted));
}

bool MetalDevice::wait_idle() {
    if (!available()) return false;
    MetalSubmitQueue::instance().drain();
    @autoreleasepool {
        id<MTLCommandQueue> queue =
            (__bridge id<MTLCommandQueue>)m_command_queue;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (command == nil) return false;
        [command commit];
        [command waitUntilCompleted];
        return command.status == MTLCommandBufferStatusCompleted;
    }
}

} // namespace vitreo::graphics
