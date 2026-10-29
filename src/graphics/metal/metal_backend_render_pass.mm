// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_render_pass.mm
/// @brief Standalone render passes and occlusion counting

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

#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_3d.hpp"
#include "graphics/metal/metal_backend_render_pass.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

/// Eight-byte visibility-result slots for GpuOcclusionCounter, carved out of
/// Shared MTLBuffers that live for the process (a pass names its slot's
/// buffer as visibilityResultBuffer, so a slot never moves). A released slot
/// is reused only once the GPU has retired the last submission that counted
/// into it; it is zeroed on the CPU when handed out again.
class MetalVisibilityPool {
public:
    struct Slot {
        void* buffer{nullptr};  ///< id<MTLBuffer>, owned by the pool
        u8* contents{nullptr};  ///< CPU view of the slot (Shared storage)
        usize offset{0};
        u32 index{0};
    };

    static MetalVisibilityPool& instance() {
        // Leaked: counters can be released during static destruction.
        static auto* pool = new MetalVisibilityPool();
        return *pool;
    }

    [[nodiscard]] bool acquire(Slot& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const u64 completed =
            MetalSubmitQueue::instance().completed_generation();
        for (auto it = m_retiring.begin(); it != m_retiring.end();) {
            if (it->second <= completed) {
                m_free.push_back(it->first);
                it = m_retiring.erase(it);
            } else {
                ++it;
            }
        }
        if (m_free.empty() && !grow()) return false;
        const u32 index = m_free.back();
        m_free.pop_back();
        out = slot(index);
        std::memset(out.contents, 0, kSlotBytes);
        return true;
    }

    /// @p last_generation: the newest submission generation a pass counting
    /// into the slot was recorded in (0: never used by a pass).
    void release(u32 index, u64 last_generation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (last_generation == 0) {
            m_free.push_back(index);
        } else {
            m_retiring.emplace_back(index, last_generation);
        }
    }

private:
    static constexpr usize kSlotBytes = 8;  // one UINT64 per slot
    static constexpr u32 kSlotsPerChunk = 4096;

    [[nodiscard]] Slot slot(u32 index) const {
        Slot s{};
        const u32 chunk = index / kSlotsPerChunk;
        s.buffer = m_chunks[chunk];
        s.offset = static_cast<usize>(index % kSlotsPerChunk) * kSlotBytes;
        s.contents = static_cast<u8*>(
            [(__bridge id<MTLBuffer>)m_chunks[chunk] contents]) + s.offset;
        s.index = index;
        return s;
    }

    [[nodiscard]] bool grow() {
        id<MTLDevice> device =
            (__bridge id<MTLDevice>)MetalDevice::instance().device_ptr();
        if (device == nil) return false;
        id<MTLBuffer> buffer =
            [device newBufferWithLength:kSlotsPerChunk * kSlotBytes
                                options:MTLResourceStorageModeShared];
        if (buffer == nil) return false;
        buffer.label = @"vitreo.visibility";
        const u32 base = static_cast<u32>(m_chunks.size()) * kSlotsPerChunk;
        m_chunks.push_back((__bridge_retained void*)buffer);
        for (u32 i = kSlotsPerChunk; i > 0; --i) m_free.push_back(base + i - 1);
        return true;
    }

    std::mutex m_mutex;
    std::vector<void*> m_chunks;
    std::vector<u32> m_free;
    std::vector<std::pair<u32, u64>> m_retiring;
};

/// MetalDevice::set_occlusion_reset_visibility_for_test.
std::atomic<bool> g_occlusion_reset_for_test{false};

/// GpuOcclusionCounter on Metal. With MTLVisibilityResultTypeAccumulate
/// (macOS 14) every pass counting into it adds to one slot; before that a pass
/// replaces the value at its offset, so each pass gets a slot of its own and
/// samples() sums them.
class MetalOcclusionCounter final : public GpuOcclusionCounter {
public:
    ~MetalOcclusionCounter() override {
        // A pass captured into a command list runs at a generation not known
        // yet; its slots are never recycled rather than recycled too early.
        const u64 retire = m_captured
                               ? std::numeric_limits<u64>::max()
                               : m_generation.load(std::memory_order_relaxed);
        auto& pool = MetalVisibilityPool::instance();
        for (const auto& slot : m_slots) {
            pool.release(slot.index, retire);
        }
    }

    u64 samples() const override {
        u64 total = 0;
        for (const auto& slot : m_slots) {
            u64 value = 0;
            std::memcpy(&value, slot.contents, sizeof(value));
            total += value;
        }
        return total;
    }

    u64 recorded_generation() const override {
        return m_generation.load(std::memory_order_relaxed);
    }

    /// Point @p begin's draws at this counter, before its BeginPass is
    /// recorded.
    [[nodiscard]] bool bind(CsBeginPass& begin) {
        bool accumulate = false;
        if (@available(macOS 14.0, *)) accumulate = true;
        if (g_occlusion_reset_for_test.load(std::memory_order_relaxed)) {
            accumulate = false;  // the macOS 13 path, on any macOS
        }
        if (m_slots.empty() || !accumulate) {
            MetalVisibilityPool::Slot slot{};
            if (!MetalVisibilityPool::instance().acquire(slot)) return false;
            m_slots.push_back(slot);
        }
        const auto& slot = m_slots.back();
        begin.visibility_buffer = slot.buffer;
        begin.visibility_offset = slot.offset;
        begin.visibility_accumulate = accumulate;
        return true;
    }

    /// An op of a pass bound to this counter -- its BeginPass, or one of its
    /// draws -- has just been recorded by this thread. The counter takes the
    /// generation that op went into, which the queue noted under its lock
    /// as the op joined the recording (last_recorded_generation), never one
    /// read before: that names the open recording, and another thread can
    /// submit it before the op joins, putting the op in the next one -- the
    /// slot would then be recycled, and the count read as final, one
    /// generation early. Taken per draw, not only at the BeginPass: a submit
    /// between the BeginPass and a draw puts the draw (which the encoder
    /// counts by reopening the pass) a generation later than the BeginPass.
    void recorded() {
        if (MetalSubmitQueue::thread_capturing()) {
            m_captured = true;
            return;
        }
        const u64 generation = MetalSubmitQueue::last_recorded_generation();
        if (generation > m_generation.load(std::memory_order_relaxed)) {
            m_generation.store(generation, std::memory_order_relaxed);
        }
    }

private:
    /// Usually one slot: with accumulating visibility (macOS 14) a counter
    /// keeps a single slot however many passes it spans.
    std::vector<MetalVisibilityPool::Slot,
                RecyclingAllocator<MetalVisibilityPool::Slot, 16>>
        m_slots;
    /// The newest generation a pass counting here was recorded in, on the
    /// immediate stream (recorded_generation). Written by the recording
    /// thread, read by whoever polls the query.
    std::atomic<u64> m_generation{0};
    /// A pass counting here was captured into a command list.
    bool m_captured{false};
};

/// MetalDevice::set_occlusion_pass_hook_for_test.
std::atomic<MetalDevice::OcclusionPassHook> g_occlusion_pass_hook{nullptr};
std::atomic<void*> g_occlusion_pass_hook_context{nullptr};

void occlusion_pass_hook(bool after) {
    if (const auto hook =
            g_occlusion_pass_hook.load(std::memory_order_acquire)) {
        hook(after,
             g_occlusion_pass_hook_context.load(std::memory_order_acquire));
    }
}

/// Standalone render pass targeting explicit MRT + depth textures. Not tied
/// to a swapchain; used for D3D11 CreateRenderTargetView + OMSetRenderTargets.
class MetalBackendRenderPassStandalone final : public BackendRenderPass {
public:
    MetalBackendRenderPassStandalone(void* device,
                                     const BackendRenderPassDesc& desc)
        : m_cache(device) {
        CsBeginPass begin{};
        unsigned rt_count = 0;
        for (unsigned i = 0; i < BACKEND_MAX_RENDER_TARGETS; ++i) {
            // A volume attachment renders into one W slice, which Metal
            // selects with depthPlane; slice stays 0, as it names an array
            // element and a 3D texture has none.
            if (desc.color_targets_3d[i] != nullptr &&
                desc.color_targets_3d[i]->valid()) {
                const auto* metal_volume =
                    static_cast<const MetalBackendTexture3D*>(
                        desc.color_targets_3d[i]);
                if (!metal_volume->desc().render_target) continue;
                begin.colors[i] = metal_volume->native_handle();
                begin.color_levels[i] = desc.color_levels[i];
                begin.color_slices[i] = 0;
                begin.color_depth_planes[i] = desc.color_depth_planes[i];
                // Layered rendering over the view's W range. Measured on
                // Apple silicon: render_target_array_index is relative to
                // depthPlane, and an index at or past renderTargetArrayLength
                // renders to depthPlane itself -- D3D's rule for
                // SV_RenderTargetArrayIndex against a Texture3D RTV.
                begin.render_target_array_length =
                    std::max(begin.render_target_array_length,
                             desc.color_array_sizes[i]);
                rt_count = i + 1;
                continue;
            }
            if (desc.color_targets[i] == nullptr ||
                !desc.color_targets[i]->valid()) {
                continue;
            }
            const auto* metal_tex =
                static_cast<const MetalBackendTexture2D*>(
                    desc.color_targets[i]);
            if (!metal_tex->desc().render_target) continue;
            begin.colors[i] = metal_tex->native_render_target_handle(
                desc.color_formats[i]);
            if (begin.colors[i] == nullptr) continue;
            begin.color_levels[i] = desc.color_levels[i];
            begin.color_slices[i] = desc.color_slices[i];
            begin.render_target_array_length =
                std::max(begin.render_target_array_length,
                         desc.color_array_sizes[i]);
            rt_count = i + 1;
        }
        begin.color_count = rt_count;
        begin.preserve = desc.preserve_color_contents;
        begin.clear_depth = desc.clear_depth;
        begin.depth_clear = desc.depth_clear_value;
        begin.center_samples = desc.center_sample_positions;

        if (desc.depth_stencil_target != nullptr &&
            desc.depth_stencil_target->valid()) {
            const auto* metal_tex =
                static_cast<const MetalBackendTexture2D*>(
                    desc.depth_stencil_target);
            if (metal_tex->desc().depth_stencil) {
                begin.depth = metal_tex->native_handle();
                begin.depth_level = desc.depth_level;
                begin.depth_slice = desc.depth_slice;
                begin.render_target_array_length = std::max(
                    begin.render_target_array_length,
                    desc.depth_array_size);
                const FormatInfo format =
                    format_table(metal_tex->desc().pixel_format);
                begin.stencil = format.mtl_pixel_format == 255 ||
                                format.mtl_pixel_format == 260;
                begin.clear_stencil = desc.clear_depth && begin.stencil;
                m_has_depth = true;
            }
        }

        if (rt_count == 0 && !m_has_depth) {
            if (desc.attachmentless_width == 0 ||
                desc.attachmentless_height == 0) {
                return;
            }
            begin.default_width = desc.attachmentless_width;
            begin.default_height = desc.attachmentless_height;
            begin.default_samples = std::max(1u, desc.attachmentless_samples);
            begin.render_target_array_length =
                std::max(1u, desc.attachmentless_layers);
        }

        auto* const counter =
            static_cast<MetalOcclusionCounter*>(desc.occlusion_counter);
        if (counter != nullptr && !counter->bind(begin)) {
            // No slot for the count: refuse the pass rather than draw
            // uncounted and report a wrong number.
            return;
        }

        // Record-only: same as MetalBackendRenderPass — no GPU wait.
        if (counter != nullptr) occlusion_pass_hook(false);
        m_pass_id = MetalSubmitQueue::instance().record_begin_pass(begin);
        if (counter != nullptr) {
            occlusion_pass_hook(true);
            counter->recorded();
        }
        m_counter = counter;
        m_open = true;
    }

    ~MetalBackendRenderPassStandalone() override {
        if (m_open) {
            MetalSubmitQueue::instance().record_end_pass(m_pass_id);
            m_open = false;
        }
    }

    bool valid() const noexcept override { return m_open; }

    bool draw(const BackendDrawCall& call) override {
        if (!m_open) return false;
        if (!record_backend_draw(call, m_has_depth, m_cache, m_pass_id)) {
            return false;
        }
        // The draw's generation, which a submit since the BeginPass makes
        // later than the BeginPass's.
        if (m_counter != nullptr) m_counter->recorded();
        return true;
    }

    bool end() override {
        if (!m_open) return false;
        MetalSubmitQueue::instance().record_end_pass(m_pass_id);
        m_open = false;
        return true;
    }

    // pass-continue (delta_record.hpp): a D3D11 draw makes one of these per
    // draw, so their storage comes from a per-thread free list instead of
    // the heap. Every block is a global operator new allocation, whichever
    // path made it, so either path may free it.
    static void* operator new(std::size_t size) {
        PassPool& pool = pass_pool();
        if (size == sizeof(MetalBackendRenderPassStandalone) &&
            !pool.blocks.empty() &&
            delta_record::active(delta_record::Part::PassContinue)) {
            void* block = pool.blocks.back();
            pool.blocks.pop_back();
            return block;
        }
        return ::operator new(size);
    }
    static void operator delete(void* block, std::size_t size) noexcept {
        PassPool& pool = pass_pool();
        if (size == sizeof(MetalBackendRenderPassStandalone) &&
            pool.blocks.size() < PassPool::kMax) {
            pool.blocks.push_back(block);
            return;
        }
        ::operator delete(block);
    }

private:
    struct PassPool {
        static constexpr usize kMax = 8;
        std::vector<void*> blocks;
        PassPool() { blocks.reserve(kMax); }
    };
    static PassPool& pass_pool() {
        // Immortal (at most kMax blocks per thread): a pass may be freed
        // during the thread's own teardown.
        static thread_local PassPool* pool = new PassPool();
        return *pool;
    }

    MetalPassStateCache m_cache;
    u64 m_pass_id{0};
    /// The counter this pass's draws count into (the frontend keeps it alive
    /// while the pass records), or null.
    MetalOcclusionCounter* m_counter{nullptr};
    bool m_open{false};
    bool m_has_depth{false};
};

} // namespace

std::unique_ptr<BackendRenderPass> MetalDevice::create_render_pass_standalone(
    const BackendRenderPassDesc& desc) {
    if (!available()) return nullptr;
    auto pass = std::make_unique<MetalBackendRenderPassStandalone>(
        m_device, desc);
    if (!pass->valid()) return nullptr;
    return pass;
}

std::shared_ptr<GpuOcclusionCounter> MetalDevice::create_occlusion_counter() {
    if (!available()) return nullptr;
    // A counter lives for one Begin/End bracket and the renderer makes
    // hundreds a frame, each released at its query's next Begin; a released
    // counter's storage builds the next one rather than a malloc and free
    // per bracket (block_recycler.hpp).
    return std::allocate_shared<MetalOcclusionCounter>(
        RecyclingAllocator<MetalOcclusionCounter, 16>{});
}

void MetalDevice::set_occlusion_pass_hook_for_test(OcclusionPassHook hook,
                                                   void* context) noexcept {
    g_occlusion_pass_hook_context.store(context, std::memory_order_release);
    g_occlusion_pass_hook.store(hook, std::memory_order_release);
}

void MetalDevice::set_occlusion_reset_visibility_for_test(bool reset) noexcept {
    g_occlusion_reset_for_test.store(reset, std::memory_order_relaxed);
}

} // namespace vitreo::graphics
