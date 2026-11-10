// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_device.mm
/// @brief Metal GPU device singleton — Objective-C++ implementation

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

#include "graphics/metal/metal_backend_buffer.hpp"
#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_state.hpp"
#include "graphics/metal/metal_backend_descriptor_table.hpp"

using namespace vitreo;

namespace vitreo::graphics {

MetalDevice::MetalDevice() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "[metal] FATAL: no Metal device found\n");
            return;
        }

        m_device = (__bridge_retained void*)device;
        m_device_name = [[device name] UTF8String];
        m_recommended_working_set_size =
            static_cast<u64>([device recommendedMaxWorkingSetSize]);

        // -- Command queue ---------------------------------------------------
        id<MTLCommandQueue> queue = [device newCommandQueue];
        m_command_queue = (__bridge_retained void*)queue;

        // -- Capability queries ----------------------------------------------
        m_max_buffer_size   = static_cast<u64>([device maxBufferLength]);
        m_max_texture_size  = 16384; // Apple Silicon minimum guaranteed

        // maxTotalThreadsPerThreadgroup
        m_max_threads = static_cast<u32>(1024); // Apple Silicon default

        // Mesh shaders (Apple7+)
        m_mesh_shaders = [device supportsFamily:MTLGPUFamilyApple7];

        // Argument buffers tier 2
        m_arg_buf_tier2 =
            ([device argumentBuffersSupport] >= MTLArgumentBuffersTier2);

        // 32-bit float filtering / MSAA (Apple family)
        m_msaa_32bit = [device supportsFamily:MTLGPUFamilyApple1];

        std::fprintf(stderr,
            "[metal] device : %s\n"
            "[metal] maxBuf : %llu MB\n"
            "[metal] maxTex : %u\n"
            "[metal] mesh   : %s\n"
            "[metal] argBuf2: %s\n"
            "[metal] msaa32 : %s\n",
            m_device_name.c_str(),
            static_cast<unsigned long long>(m_max_buffer_size / (1024 * 1024)),
            m_max_texture_size,
            m_mesh_shaders ? "yes" : "no",
            m_arg_buf_tier2 ? "yes" : "no",
            m_msaa_32bit ? "yes" : "no");
    }
}

MetalDevice::~MetalDevice() {
    @autoreleasepool {
        MetalSubmitQueue::instance().drain();
        m_gamma_active.store(false, std::memory_order_relaxed);
        if (m_gamma_compute_pso) {
            CFRelease(m_gamma_compute_pso);
            m_gamma_compute_pso = nullptr;
        }
        if (m_gamma_lut) {
            CFRelease(m_gamma_lut);
            m_gamma_lut = nullptr;
        }
        if (m_command_queue) {
            CFRelease(m_command_queue);
            m_command_queue = nullptr;
        }
        if (m_device) {
            CFRelease(m_device);
            m_device = nullptr;
        }
    }
}

MetalDevice& MetalDevice::instance() {
    static MetalDevice s_instance;
    return s_instance;
}

void* MetalDevice::device_ptr() const {
    return m_device;
}

void* MetalDevice::command_queue_ptr() const {
    return m_command_queue;
}

memory::GpuWorkingSet MetalDevice::gpu_working_set() const noexcept {
    if (m_device == nullptr) return {};
    // MTLDevice.h: all resources allocated by this device. Reading the device
    // counter once includes in-flight resources without double-counting views.
    return {static_cast<u64>(
                ((__bridge id<MTLDevice>)m_device).currentAllocatedSize),
            m_recommended_working_set_size};
}

bool MetalDevice::query_video_memory(BackendVideoMemoryInfo& info) const noexcept {
    if (m_device == nullptr || m_recommended_working_set_size == 0) return false;
    const memory::GpuWorkingSet working_set = gpu_working_set();
    info = {memory::host_gpu_budget(working_set), working_set.allocated};
    return true;
}

u32 MetalDevice::simd_group_width() const {
    // Metal exposes the SIMD-group width only on a compiled compute
    // pipeline (threadExecutionWidth), so ask one: every kernel on a device
    // shares it.
    static u32 width = 0;
    static std::once_flag once;
    std::call_once(once, [this] {
        if (!available()) return;
        @autoreleasepool {
            id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
            NSError* error = nil;
            id<MTLLibrary> library = [device
                newLibraryWithSource:@"kernel void vitreo_simd_probe() {}"
                             options:nil
                               error:&error];
            id<MTLFunction> function =
                [library newFunctionWithName:@"vitreo_simd_probe"];
            if (function == nil) return;
            id<MTLComputePipelineState> pipeline =
                [device newComputePipelineStateWithFunction:function
                                                      error:&error];
            if (pipeline != nil) {
                width = static_cast<u32>(pipeline.threadExecutionWidth);
            }
        }
    });
    return width;
}

bool MetalDevice::supports_texture_sample_count(unsigned count) const {
    if (count <= 1) return true;
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device supportsTextureSampleCount:count];
    }
}

bool MetalDevice::supports_float32_filtering() const {
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device supports32BitFloatFiltering];
    }
}

bool MetalDevice::supports_float32_multisample_resolve() const {
    // Metal Feature Set Tables, "Texture capabilities by pixel format":
    // R32Float, RG32Float and RGBA32Float list Resolve from Apple9 only.
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device supportsFamily:MTLGPUFamilyApple9];
    }
}

u64 MetalDevice::max_buffer_length() const {
    if (m_device == nullptr) return 0;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return static_cast<u64>([device maxBufferLength]);
    }
}

bool MetalDevice::supports_framebuffer_fetch() const {
    // Metal Feature Set Tables: programmable blending (colour attachment
    // reads in a fragment function) is an Apple-family feature, absent from
    // the Mac2 family of Intel/AMD GPUs.
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device supportsFamily:MTLGPUFamilyApple1];
    }
}

bool MetalDevice::supports_raster_order_groups() const {
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device areRasterOrderGroupsSupported];
    }
}

bool MetalDevice::supports_read_write_texture_tier2() const {
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device readWriteTextureSupport] == MTLReadWriteTextureTier2;
    }
}

bool MetalDevice::has_unified_memory() const {
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device hasUnifiedMemory];
    }
}

void MetalDevice::push_debug_group(const std::string& label) {
    if (!available()) return;
    MetalSubmitQueue::instance().record_debug_marker(
        MetalSubmitQueue::DebugMarker::Push, label);
}

void MetalDevice::pop_debug_group() {
    if (!available()) return;
    MetalSubmitQueue::instance().record_debug_marker(
        MetalSubmitQueue::DebugMarker::Pop, {});
}

void MetalDevice::insert_debug_signpost(const std::string& label) {
    if (!available()) return;
    MetalSubmitQueue::instance().record_debug_marker(
        MetalSubmitQueue::DebugMarker::Signpost, label);
}

bool MetalDevice::supports_programmable_sample_positions() const {
    if (m_device == nullptr) return false;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        return [device areProgrammableSamplePositionsSupported];
    }
}

bool MetalDevice::supports_shader_stencil_export() const {
    // A fragment function's [[stencil]] output is core Metal Shading Language
    // from MSL 2.1, and the shader pipeline emits MSL 2.3 or newer
    // (spirv_to_msl.cpp); the render pipeline then takes the exported value as
    // the stencil reference of that pixel's test and write. There is no
    // device query: the whole path is exercised by the end-to-end
    // StencilRefPixelShaderWritesTheStencilBuffer test.
    return m_device != nullptr;
}

bool MetalDevice::supports_descriptor_tables() const noexcept {
    if (!available()) return false;
    // Tier 2 is the requirement: it is what allows writable images inside the
    // argument buffer, which D3D's u# space needs, and the higher resource
    // counts that are the whole reason for using one. A Tier 1 device reports
    // false and its frontends stay on the flat path with its narrower register
    // counts, which is honest rather than silently wrong.
    id<MTLDevice> device = (__bridge id<MTLDevice>)native_device();
    if (device == nil) return false;
    return device.argumentBuffersSupport == MTLArgumentBuffersTier2;
}

std::unique_ptr<BackendDescriptorTable> MetalDevice::create_descriptor_table(
    const BackendDescriptorTableDesc& desc) {
    if (!supports_descriptor_tables() || desc.slot_count == 0) return nullptr;
    auto table = std::make_unique<MetalBackendDescriptorTable>(
        native_device(), desc);
    if (!table->valid()) return nullptr;
    return table;
}

std::unique_ptr<BackendSamplerState> MetalDevice::create_sampler_state(
    const BackendSamplerDesc& desc) {
    if (!available()) return nullptr;
    auto state = std::make_unique<MetalBackendSamplerState>(native_device(), desc);
    if (!state->valid()) return nullptr;
    return state;
}

std::unique_ptr<BackendDepthStencilState>
MetalDevice::create_depth_stencil_state(const BackendDepthStencilDesc& desc) {
    if (!available()) return nullptr;
    auto state =
        std::make_unique<MetalBackendDepthStencilState>(native_device(), desc);
    if (!state->valid()) return nullptr;
    return state;
}

// -- Tiled resources (metal_tiled.hpp) ---------------------------------------

bool MetalDevice::supports_tiled_resources() const {
    return available() && MetalTiled::instance().available();
}

bool MetalDevice::tiled_texture_tile_shape(PixelFormat format,
                                           unsigned samples, unsigned& width,
                                           unsigned& height) const {
    const FormatInfo info = format_table(format);
    return supports_tiled_resources() &&
           MetalTiled::instance().tile_shape(info.mtl_pixel_format, samples,
                                             width, height);
}

std::unique_ptr<BackendTilePool> MetalDevice::create_tile_pool() {
    if (!supports_tiled_resources()) return nullptr;
    return MetalTiled::instance().create_pool();
}

bool MetalDevice::tiled_texture_layout(const BackendTexture2D* texture,
                                       BackendTiledLayout& layout) {
    if (texture == nullptr || !texture->valid() || !texture->desc().tiled) {
        return false;
    }
    const auto* metal = static_cast<const MetalBackendTexture2D*>(texture);
    unsigned first = 0;
    unsigned packed = 0;
    if (!MetalTiled::instance().texture_tail(metal->native_handle(), first,
                                             packed) ||
        !tiled_texture_tile_shape(texture->desc().pixel_format,
                                  texture->desc().sample_count,
                                  layout.tile_width, layout.tile_height)) {
        return false;
    }
    layout.first_packed_level = first;
    layout.packed_tiles = packed;
    return true;
}

bool MetalDevice::update_tile_mappings(BackendTexture2D* texture,
                                       BackendTilePool* pool,
                                       const BackendTileMapping* mappings,
                                       usize count, bool self_aliased) {
    if (texture == nullptr || !texture->valid() || !texture->desc().tiled) {
        return false;
    }
    BackendTiledLayout layout{};
    if (!tiled_texture_layout(texture, layout)) return false;
    auto* metal = static_cast<MetalBackendTexture2D*>(texture);
    if (!MetalTiled::instance().record_update(
            metal->native_handle(), true, layout.first_packed_level, pool,
            mappings, count, self_aliased)) {
        return false;
    }
    MetalBackendTexture2D* companion = metal->residency_companion();
    if (companion == nullptr || count == 0) return true;
    // The residency companion mirrors the update in the same stream: each
    // tile mapped here shows the companion's page of 1.0, each unmapped one
    // nothing (reads 0). attach_residency_companion matched the tile grid
    // and the packed tail, so the coordinates are the same.
    PixelFormat format = PixelFormat::Unknown;
    u64 pattern = 0;
    BackendTilePool* ones = nullptr;
    if (metal_residency_format(texture->desc().pixel_format, format, pattern)) {
        ones = MetalTiled::instance().residency_pool(pattern);
    }
    if (ones == nullptr) return false;
    BackendTiledLayout companion_layout{};
    if (!tiled_texture_layout(companion, companion_layout)) return false;
    std::vector<BackendTileMapping> mirrored;
    mirrored.reserve(count);
    // A slice's packed mips are one unit (D3D11.3 5.9.2.8.5 packs them
    // together; the companion's tail may hold a different number of
    // tiles): the companion's whole tail shows 1.0 while every tile of the
    // texture's tail of that slice is mapped, else nothing.
    const unsigned tail = layout.packed_tiles;
    std::vector<u8>& state = metal->residency_tail_state();
    const unsigned slices = std::max(1u, texture->desc().array_size) *
                            (texture->desc().cube && texture->desc().array_size == 1 ? 6u : 1u);
    if (tail != 0 && state.size() != static_cast<usize>(tail) * slices) {
        state.assign(static_cast<usize>(tail) * slices, 0u);
    }
    const auto whole = [&](unsigned slice) {
        for (unsigned j = 0; j < tail; ++j) {
            if (state[static_cast<usize>(slice) * tail + j] == 0u) return false;
        }
        return true;
    };
    std::vector<std::pair<unsigned, bool>> tails_before;
    for (usize i = 0; i < count; ++i) {
        BackendTileMapping mapping = mappings[i];
        const bool mapped = mapping.pool_tile != BACKEND_TILE_UNMAPPED;
        if (!mapping.tile.packed) {
            mapping.pool_tile = mapped ? 0u : BACKEND_TILE_UNMAPPED;
            mirrored.push_back(mapping);
            continue;
        }
        const unsigned slice = mapping.tile.slice;
        if (tail == 0 || slice >= slices || mapping.tile.x >= tail) return false;
        bool seen = false;
        for (const auto& entry : tails_before) seen = seen || entry.first == slice;
        if (!seen) tails_before.emplace_back(slice, whole(slice));
        state[static_cast<usize>(slice) * tail + mapping.tile.x] = mapped ? 1u : 0u;
    }
    for (const auto& [slice, was] : tails_before) {
        const bool now = whole(slice);
        if (now == was) continue;
        for (unsigned j = 0; j < companion_layout.packed_tiles; ++j) {
            BackendTileMapping unit{};
            unit.tile.packed = true;
            unit.tile.slice = slice;
            unit.tile.x = j;
            unit.pool_tile = now ? 0u : BACKEND_TILE_UNMAPPED;
            mirrored.push_back(unit);
        }
    }
    if (mirrored.empty()) return true;
    return MetalTiled::instance().record_update(
        companion->native_handle(), true, companion_layout.first_packed_level,
        ones, mirrored.data(), mirrored.size(), false);
}

bool MetalDevice::update_tile_mappings(BackendBuffer* buffer,
                                       BackendTilePool* pool,
                                       const BackendTileMapping* mappings,
                                       usize count, bool self_aliased) {
    if (buffer == nullptr || !buffer->valid() || !buffer->desc().tiled) {
        return false;
    }
    auto* metal = static_cast<MetalBackendBuffer*>(buffer);
    return MetalTiled::instance().record_update(metal->native_handle(), false,
                                                0, pool, mappings, count,
                                                self_aliased);
}

void MetalDevice::attachment_barrier(BackendTexture2D* texture) {
    void* scope = nullptr;
    if (texture != nullptr && texture->valid()) {
        auto* metal = static_cast<MetalBackendTexture2D*>(texture);
        const auto& local = metal->local_texture();
        const auto& desc = texture->desc();
        id<MTLTexture> native = (__bridge id<MTLTexture>)metal->native_handle();
        // Standalone private allocations and ordinary placement heaps have
        // stable native allocation identities/ranges. Shared, sparse,
        // linear and borrowed storage keep the unconditional boundary.
        const bool known_storage = local &&
            (native.heap != nil
                 ? native.heap.type == MTLHeapTypePlacement
                 : desc.heap == nullptr && !local->desc().heap_placed);
        if (local && !metal->shared_local() && !desc.shareable &&
            !desc.tiled && known_storage &&
            [native respondsToSelector:@selector(isSparse)] && !native.isSparse &&
            !local->desc().shareable &&
            !local->desc().placement_sparse &&
            native.storageMode == MTLStorageModePrivate &&
            native.buffer == nil && native.iosurface == nullptr) {
            scope = metal->native_handle();
        }
    }
    MetalSubmitQueue::instance().record_attachment_barrier(scope);
}

bool MetalDevice::tiled_resource_barrier() {
    if (!supports_tiled_resources()) return false;
    MetalSubmitQueue::instance().record_tiled_barrier();
    return true;
}

} // namespace vitreo::graphics

#include "graphics/metal/metal_raytracing.hpp"

namespace vitreo::graphics {
BackendRaytracing* MetalDevice::raytracing() noexcept {
    return m_device != nullptr ? metal_raytracing(m_device) : nullptr;
}
}  // namespace vitreo::graphics
