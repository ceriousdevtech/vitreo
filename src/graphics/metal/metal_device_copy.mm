// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_device_copy.mm
/// @brief MetalDevice copies and resolves

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
#include "graphics/metal/metal_backend_texture_3d.hpp"
#include "graphics/metal/metal_transcode.hpp"
#include "graphics/contracts/format_422.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

/// D3D12 / D3D11 boxes and footprints of a block-compressed level are whole
/// blocks, so one may end at the block edge past the level's texel extent
/// (a small mip, or a top level that is not a multiple of 4: D3D11.3
/// 19.5.1, Learn D3D12_FEATURE_DATA_D3D12_OPTIONS8). Metal copies the
/// level's own texels: such an extent is cut at the level's edge. Any
/// other extent is left as given (and refused by the bounds checks).
unsigned clamp_block_extent(unsigned origin, unsigned extent, unsigned level) {
    if (origin >= level) return extent;
    const unsigned padded = (level + 3u) & ~3u;
    if (extent > level - origin && extent <= padded - origin) {
        return level - origin;
    }
    return extent;
}

}  // namespace

bool MetalDevice::copy_buffer_texture_row(BackendBuffer* buffer, u64 offset,
                                          BackendTexture2D* texture,
                                          unsigned width, bool to_texture) {
    if (!available() || buffer == nullptr || texture == nullptr ||
        !buffer->valid() || !texture->valid() || width == 0) {
        return false;
    }
    const auto& td = texture->desc();
    const FormatInfo format = format_table(td.pixel_format);
    const u64 texel = format.bytes_per_pixel;
    const u64 bytes = texel * width;
    // A blit moves whole texels at a texel-aligned buffer offset (measured
    // to hold down to the texel size on Apple GPUs, with validation on).
    if (texel == 0 || format.is_compressed || format.is_depth ||
        td.sample_count > 1 || width > td.width || offset % texel != 0 ||
        offset > buffer->desc().byte_width ||
        bytes > buffer->desc().byte_width - offset) {
        return false;
    }
    auto* metal_buffer = static_cast<MetalBackendBuffer*>(buffer);
    auto* metal_texture = static_cast<MetalBackendTexture2D*>(texture);
    // A GPU write: the storage leaves any pool slice first (as
    // copy_buffer_region), before its handle and offset are taken.
    if (!to_texture) metal_buffer->note_gpu_write();
    void* buffer_handle = metal_buffer->native_handle();
    void* texture_handle = metal_texture->native_handle();
    if (buffer_handle == nullptr || texture_handle == nullptr) return false;
    const usize at =
        static_cast<usize>(offset) + metal_buffer->native_offset();
    if (to_texture) {
        // Read in command order: a later rename must not move it.
        metal_buffer->note_gpu_bind();
        MetalSubmitQueue::instance().record_upload_texture(
            buffer_handle, texture_handle, static_cast<usize>(bytes),
            static_cast<usize>(bytes), width, 1, 1, 0, 0, 0, 0, 0, 0, at);
    } else {
        MetalSubmitQueue::instance().record_download_texture(
            texture_handle, 0, 0, 0, 0, 0, width, 1, 1, buffer_handle,
            static_cast<usize>(bytes), static_cast<usize>(bytes), at);
    }
    return true;
}

bool MetalDevice::copy_texture_buffer_region(
    BackendTexture2D* texture, unsigned level, unsigned slice, unsigned x,
    unsigned y, unsigned width, unsigned height, BackendBuffer* buffer,
    u64 offset, u64 row_pitch, bool to_texture) {
    if (!available() || texture == nullptr || buffer == nullptr ||
        !texture->valid() || !buffer->valid() || width == 0 || height == 0) {
        return false;
    }
    const auto& td = texture->desc();
    const FormatInfo format = format_table(td.pixel_format);
    // A multisampled texture has one level and slice here (tiled 4x); a blit
    // cannot move it, a kernel and a per-sample draw do (below).
    const unsigned samples = std::max(1u, td.sample_count);
    if (level >= td.mip_levels || slice >= std::max(1u, td.array_size) ||
        (samples > 1 && (level != 0 || format.is_compressed))) {
        return false;
    }
    const unsigned level_width = std::max(1u, td.width >> level);
    const unsigned level_height = std::max(1u, td.height >> level);
    if (is_422_storage(td.pixel_format)) {
        // A box or footprint is whole 2x1 blocks; one may end at the block
        // edge past an odd level width.
        if (x % 2u != 0 || samples > 1) return false;
        if (x < level_width && width == level_width - x + 1u &&
            (level_width - x) % 2u != 0) {
            width = level_width - x;
        }
        if (x >= level_width || y >= level_height ||
            width > level_width - x || height > level_height - y) {
            return false;
        }
        return copy_422_buffer_region(
            static_cast<MetalBackendTexture2D*>(texture)->native_handle(),
            td.pixel_format, level, slice, x, y, 0, width, height, 1, buffer,
            offset, row_pitch, 0, to_texture);
    }
    if (format.is_compressed) {
        width = clamp_block_extent(x, width, level_width);
        height = clamp_block_extent(y, height, level_height);
    }
    if (x >= level_width || y >= level_height || width > level_width - x ||
        height > level_height - y) {
        return false;
    }
    const u64 element = format.is_compressed
                            ? compressed_block_bytes(td.pixel_format)
                            : format.bytes_per_pixel;
    if (element == 0) return false;
    const u64 row_elements =
        format.is_compressed ? (width + 3u) / 4u : width;
    const u64 rows = format.is_compressed ? (height + 3u) / 4u : height;
    const u64 row_bytes = row_elements * element * samples;
    const u64 span = (rows - 1) * row_pitch + row_bytes;
    const u64 buffer_bytes = buffer->desc().byte_width;
    if (row_pitch < row_bytes || row_pitch % element != 0 ||
        offset > buffer_bytes || span > buffer_bytes - offset) {
        return false;
    }
    auto* metal_buffer = static_cast<MetalBackendBuffer*>(buffer);
    auto* metal_texture = static_cast<MetalBackendTexture2D*>(texture);
    void* texture_handle = metal_texture->native_handle();
    if (texture_handle == nullptr) return false;
    auto& queue = MetalSubmitQueue::instance();
    // A blit takes a buffer offset that is a whole number of elements; any
    // other offset goes through an aligned scratch copy.
    std::unique_ptr<BackendBuffer> scratch;
    if (offset % element != 0) {
        BackendBufferDesc scratch_desc{};
        scratch_desc.byte_width = static_cast<usize>(span);
        scratch = create_buffer(scratch_desc, nullptr);
        if (!scratch) return false;
        if (to_texture &&
            !copy_buffer_region(scratch.get(), 0, buffer, offset, span)) {
            return false;
        }
    }
    auto* blit_buffer = scratch
                            ? static_cast<MetalBackendBuffer*>(scratch.get())
                            : metal_buffer;
    const u64 blit_offset = scratch ? 0 : offset;
    if (to_texture) {
        blit_buffer->note_gpu_bind();
    } else {
        blit_buffer->note_gpu_write();
    }
    void* buffer_handle = blit_buffer->native_handle();
    if (buffer_handle == nullptr) return false;
    if (samples > 1) {
        const bool ok = MetalTiled::instance().copy_multisampled(
            texture_handle, slice, static_cast<u32>(element),
            td.pixel_format == PixelFormat::D16Unorm, x, y, width, height,
            buffer_handle, blit_buffer->native_offset(), blit_offset,
            row_pitch, to_texture);
        if (!ok) return false;
        if (!to_texture && scratch) {
            return copy_buffer_region(buffer, offset, scratch.get(), 0, span);
        }
        return true;
    }
    const usize at =
        static_cast<usize>(blit_offset) + blit_buffer->native_offset();
    const usize image = static_cast<usize>(row_pitch * rows);
    if (to_texture) {
        queue.record_upload_texture(buffer_handle, texture_handle,
                                    static_cast<usize>(row_pitch), image,
                                    width, height, 1, level, slice, x, y, 0,
                                    0, at);
        return true;
    }
    queue.record_download_texture(texture_handle, level, slice, x, y, 0,
                                  width, height, 1, buffer_handle,
                                  static_cast<usize>(row_pitch), image, at);
    if (scratch) {
        return copy_buffer_region(buffer, offset, scratch.get(), 0, span);
    }
    return true;
}

bool MetalDevice::copy_texture_3d_buffer_region(
    BackendTexture3D* texture, unsigned level, unsigned x, unsigned y,
    unsigned z, unsigned width, unsigned height, unsigned depth,
    BackendBuffer* buffer, u64 offset, u64 row_pitch, u64 slice_pitch,
    bool to_texture) {
    if (!available() || texture == nullptr || buffer == nullptr ||
        !texture->valid() || !buffer->valid() || width == 0 || height == 0 ||
        depth == 0) {
        return false;
    }
    const auto& td = texture->desc();
    const FormatInfo format = format_table(td.pixel_format);
    if (level >= td.mip_levels) return false;
    const unsigned level_width = std::max(1u, td.width >> level);
    const unsigned level_height = std::max(1u, td.height >> level);
    const unsigned level_depth = std::max(1u, td.depth >> level);
    if (is_422_storage(td.pixel_format)) {
        if (x % 2u != 0) return false;
        if (x < level_width && width == level_width - x + 1u &&
            (level_width - x) % 2u != 0) {
            width = level_width - x;
        }
        if (x >= level_width || y >= level_height || z >= level_depth ||
            width > level_width - x || height > level_height - y ||
            depth > level_depth - z) {
            return false;
        }
        return copy_422_buffer_region(
            static_cast<MetalBackendTexture3D*>(texture)->native_handle(),
            td.pixel_format, level, 0, x, y, z, width, height, depth, buffer,
            offset, row_pitch, slice_pitch, to_texture);
    }
    if (format.is_compressed) {
        width = clamp_block_extent(x, width, level_width);
        height = clamp_block_extent(y, height, level_height);
    }
    if (x >= level_width || y >= level_height || z >= level_depth ||
        width > level_width - x || height > level_height - y ||
        depth > level_depth - z) {
        return false;
    }
    const u64 element = format.is_compressed
                            ? compressed_block_bytes(td.pixel_format)
                            : format.bytes_per_pixel;
    if (element == 0) return false;
    const u64 row_elements = format.is_compressed ? (width + 3u) / 4u : width;
    const u64 rows = format.is_compressed ? (height + 3u) / 4u : height;
    const u64 row_bytes = row_elements * element;
    const u64 image_bytes = (rows - 1) * row_pitch + row_bytes;
    const u64 span = (depth - 1) * slice_pitch + image_bytes;
    const u64 buffer_bytes = buffer->desc().byte_width;
    if (row_pitch < row_bytes || row_pitch % element != 0 ||
        (depth > 1 && slice_pitch < image_bytes) || offset > buffer_bytes ||
        span > buffer_bytes - offset) {
        return false;
    }
    auto* metal_texture = static_cast<MetalBackendTexture3D*>(texture);
    void* texture_handle = metal_texture->native_handle();
    if (texture_handle == nullptr) return false;
    // A blit takes a buffer offset that is a whole number of elements; any
    // other offset goes through an aligned scratch copy (as the 2D form).
    std::unique_ptr<BackendBuffer> scratch;
    if (offset % element != 0) {
        BackendBufferDesc scratch_desc{};
        scratch_desc.byte_width = static_cast<usize>(span);
        scratch = create_buffer(scratch_desc, nullptr);
        if (!scratch) return false;
        if (to_texture &&
            !copy_buffer_region(scratch.get(), 0, buffer, offset, span)) {
            return false;
        }
    }
    auto* blit_buffer = static_cast<MetalBackendBuffer*>(
        scratch ? scratch.get() : buffer);
    const u64 blit_offset = scratch ? 0 : offset;
    if (to_texture) {
        blit_buffer->note_gpu_bind();
    } else {
        blit_buffer->note_gpu_write();
    }
    void* buffer_handle = blit_buffer->native_handle();
    if (buffer_handle == nullptr) return false;
    const usize at =
        static_cast<usize>(blit_offset) + blit_buffer->native_offset();
    auto& queue = MetalSubmitQueue::instance();
    if (to_texture) {
        queue.record_upload_texture(buffer_handle, texture_handle,
                                    static_cast<usize>(row_pitch),
                                    static_cast<usize>(slice_pitch), width,
                                    height, depth, level, 0, x, y, z, 0, at);
        return true;
    }
    queue.record_download_texture(texture_handle, level, 0, x, y, z, width,
                                  height, depth, buffer_handle,
                                  static_cast<usize>(row_pitch),
                                  static_cast<usize>(slice_pitch), at);
    if (scratch) {
        return copy_buffer_region(buffer, offset, scratch.get(), 0, span);
    }
    return true;
}

bool MetalDevice::copy_422_buffer_region(
    void* texture, PixelFormat format, unsigned level, unsigned slice,
    unsigned x, unsigned y, unsigned z, unsigned width, unsigned height,
    unsigned depth, BackendBuffer* buffer, u64 offset, u64 row_pitch,
    u64 slice_pitch, bool to_texture) {
    if (texture == nullptr || width == 0 || height == 0 || depth == 0) {
        return false;
    }
    // The guest side: (width + 1) / 2 blocks of 4 bytes per row.
    const u64 row_bytes = static_cast<u64>((width + 1u) / 2u) * 4u;
    const u64 image_bytes = (height - 1) * row_pitch + row_bytes;
    if (depth == 1) slice_pitch = image_bytes;
    const u64 span = (depth - 1) * slice_pitch + image_bytes;
    const u64 buffer_bytes = buffer->desc().byte_width;
    if (row_pitch < row_bytes || (depth > 1 && slice_pitch < image_bytes) ||
        offset > buffer_bytes || span > buffer_bytes - offset) {
        return false;
    }
    // The storage side: tightly packed RGBA8 texels.
    const u64 texel_row = static_cast<u64>(width) * 4u;
    const u64 texel_image = texel_row * height;
    BackendBufferDesc scratch_desc{};
    scratch_desc.byte_width = static_cast<usize>(texel_image * depth);
    std::unique_ptr<BackendBuffer> scratch = create_buffer(scratch_desc, nullptr);
    if (!scratch) return false;
    auto* guest = static_cast<MetalBackendBuffer*>(buffer);
    auto* texels = static_cast<MetalBackendBuffer*>(scratch.get());
    if (to_texture) {
        guest->note_gpu_bind();
    } else {
        guest->note_gpu_write();
    }
    texels->note_gpu_write();
    void* guest_handle = guest->native_handle();
    void* texel_handle = texels->native_handle();
    if (guest_handle == nullptr || texel_handle == nullptr) return false;
    const usize guest_at = static_cast<usize>(offset) + guest->native_offset();
    const usize texel_at = texels->native_offset();
    Transcode422Region region{};
    region.width = width;
    region.height = height;
    region.depth = depth;
    auto& queue = MetalSubmitQueue::instance();
    if (to_texture) {
        region.in_row = row_pitch;
        region.in_slice = slice_pitch;
        region.out_row = texel_row;
        region.out_slice = texel_image;
        if (!record_transcode_422(format, true, guest_handle, guest_at,
                                  texel_handle, texel_at, region)) {
            return false;
        }
        queue.record_upload_texture(texel_handle, texture,
                                    static_cast<usize>(texel_row),
                                    static_cast<usize>(texel_image), width,
                                    height, depth, level, slice, x, y, z, 0,
                                    texel_at);
        return true;
    }
    queue.record_download_texture(texture, level, slice, x, y, z, width,
                                  height, depth, texel_handle,
                                  static_cast<usize>(texel_row),
                                  static_cast<usize>(texel_image), texel_at);
    region.in_row = texel_row;
    region.in_slice = texel_image;
    region.out_row = row_pitch;
    region.out_slice = slice_pitch;
    return record_transcode_422(format, false, texel_handle, texel_at,
                                guest_handle, guest_at, region);
}

bool MetalDevice::copy_texture_2d(BackendTexture2D* dst,
                                  const BackendTexture2D* src) {
    if (!available() || dst == nullptr || src == nullptr) return false;
    if (!dst->valid() || !src->valid()) return false;
    const auto& dd = dst->desc();
    const auto& sd = src->desc();
    if (dd.width != sd.width || dd.height != sd.height ||
        dd.mip_levels != sd.mip_levels ||
        dd.pixel_format != sd.pixel_format) {
        return false;
    }
    if (sd.sample_count > 1 && dd.sample_count <= 1) {
        return resolve_texture_2d(
            dst, 0, 0, src, 0, 0, dd.pixel_format);
    }
    return copy_texture_2d_region(dst, 0, 0, src, 0, 0, dd.width, dd.height);
}

namespace {

/// Average of every sample, written into a single-sample depth or stencil
/// attachment. Learn (D3D12_RESOLVE_MODE_AVERAGE) defines that reduction for
/// the depth plane; the stencil plane uses the same reduction of the values
/// its samples store. Metal's depth resolve filter is sample 0, min, or max,
/// which is not this average, so the samples are read and combined here.
constexpr const char* kDepthResolveSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut depth_resolve_vs(uint vid [[vertex_id]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
struct DepthOut { float depth [[depth(any)]]; };
fragment DepthOut depth_resolve_average(
    VOut in [[stage_in]],
    depth2d_ms<float, access::read> src [[texture(0)]]) {
    DepthOut o;
    uint n = src.get_num_samples();
    float sum = 0.0;
    for (uint s = 0; s < n; ++s) {
        sum += src.read(uint2(in.pos.xy), s);
    }
    o.depth = n == 0u ? 0.0 : sum / float(n);
    return o;
}
struct StencilOut { uint stencil [[stencil]]; };
fragment StencilOut stencil_resolve_average(
    VOut in [[stage_in]],
    texture2d_ms<uint, access::read> src [[texture(0)]]) {
    StencilOut o;
    uint n = src.get_num_samples();
    uint sum = 0u;
    for (uint s = 0; s < n; ++s) {
        sum += src.read(uint2(in.pos.xy), s).x;
    }
    o.stencil = n == 0u ? 0u : (sum + n / 2u) / n;
    return o;
}
)MSL";

class DepthResolvePrograms {
public:
    static DepthResolvePrograms& instance() {
        static DepthResolvePrograms* programs = new DepthResolvePrograms();
        return *programs;
    }

    void* pipeline(void* device, bool stencil, u32 depth_format,
                   u32 stencil_format) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!load(device)) return nullptr;
        const u64 key = (static_cast<u64>(stencil) << 63) |
                        (static_cast<u64>(depth_format) << 16) |
                        stencil_format;
        void*& slot = m_pipelines[key];
        if (slot != nullptr) return slot;
        @autoreleasepool {
            id<MTLLibrary> library = (__bridge id<MTLLibrary>)m_library;
            MTLRenderPipelineDescriptor* desc =
                [[MTLRenderPipelineDescriptor alloc] init];
            desc.vertexFunction =
                [library newFunctionWithName:@"depth_resolve_vs"];
            desc.fragmentFunction = [library
                newFunctionWithName:stencil ? @"stencil_resolve_average"
                                            : @"depth_resolve_average"];
            desc.rasterSampleCount = 1;
            desc.depthAttachmentPixelFormat =
                static_cast<MTLPixelFormat>(depth_format);
            desc.stencilAttachmentPixelFormat =
                static_cast<MTLPixelFormat>(stencil_format);
            NSError* error = nil;
            id<MTLRenderPipelineState> pso =
                desc.vertexFunction != nil && desc.fragmentFunction != nil
                    ? [(__bridge id<MTLDevice>)device
                          newRenderPipelineStateWithDescriptor:desc
                                                         error:&error]
                    : nil;
            if (pso == nil) {
                static std::atomic<bool> told{false};
                if (!told.exchange(true)) {
                    std::fprintf(stderr,
                                 "[metal] depth resolve pipeline failed: %s\n",
                                 error != nil ? error.localizedDescription.UTF8String
                                              : "unknown");
                }
                return nullptr;
            }
            slot = (__bridge_retained void*)pso;
        }
        return slot;
    }

    /// Always-pass. @p stencil replaces the stencil from the shader and
    /// leaves depth stored; otherwise depth is replaced and stencil is not.
    void* depth_state(void* device, bool stencil) {
        std::lock_guard<std::mutex> lock(m_mutex);
        void*& slot = stencil ? m_stencil_state : m_depth_state;
        if (slot != nullptr) return slot;
        @autoreleasepool {
            MTLDepthStencilDescriptor* desc =
                [[MTLDepthStencilDescriptor alloc] init];
            desc.depthCompareFunction = MTLCompareFunctionAlways;
            desc.depthWriteEnabled = !stencil;
            if (stencil) {
                MTLStencilDescriptor* face = [[MTLStencilDescriptor alloc] init];
                face.stencilCompareFunction = MTLCompareFunctionAlways;
                face.depthStencilPassOperation = MTLStencilOperationReplace;
                face.depthFailureOperation = MTLStencilOperationReplace;
                face.stencilFailureOperation = MTLStencilOperationReplace;
                face.readMask = 0xFFu;
                face.writeMask = 0xFFu;
                desc.frontFaceStencil = face;
                desc.backFaceStencil = face;
            }
            id<MTLDepthStencilState> state =
                [(__bridge id<MTLDevice>)device
                    newDepthStencilStateWithDescriptor:desc];
            if (state != nil) slot = (__bridge_retained void*)state;
        }
        return slot;
    }

private:
    bool load(void* device) {
        if (m_library != nullptr) return true;
        if (m_failed || device == nullptr) return false;
        @autoreleasepool {
            NSError* error = nil;
            id<MTLLibrary> library = [(__bridge id<MTLDevice>)device
                newLibraryWithSource:[NSString
                                         stringWithUTF8String:kDepthResolveSource]
                             options:nil
                               error:&error];
            if (library == nil) {
                m_failed = true;
                std::fprintf(stderr,
                             "[metal] depth resolve shaders did not compile: "
                             "%s\n",
                             error != nil ? error.localizedDescription.UTF8String
                                          : "unknown");
                return false;
            }
            m_library = (__bridge_retained void*)library;
        }
        return true;
    }

    std::mutex m_mutex;
    void* m_library{nullptr};
    bool m_failed{false};
    std::unordered_map<u64, void*> m_pipelines;
    void* m_depth_state{nullptr};
    void* m_stencil_state{nullptr};
};

}  // namespace

bool MetalDevice::resolve_depth_stencil_2d(BackendTexture2D* dst,
                                           unsigned dst_mip, unsigned dst_slice,
                                           const BackendTexture2D* src,
                                           unsigned src_mip, unsigned src_slice,
                                           bool stencil) {
    if (!available() || dst == nullptr || src == nullptr || !dst->valid() ||
        !src->valid()) {
        return false;
    }
    const auto& dd = dst->desc();
    const auto& sd = src->desc();
    if (dd.pixel_format != sd.pixel_format || dst_mip >= dd.mip_levels ||
        src_mip >= sd.mip_levels || dst_slice >= dd.array_size ||
        src_slice >= sd.array_size || dd.sample_count > 1 ||
        sd.sample_count <= 1) {
        return false;
    }
    const unsigned width = std::max(1u, dd.width >> dst_mip);
    const unsigned height = std::max(1u, dd.height >> dst_mip);
    if (width != std::max(1u, sd.width >> src_mip) ||
        height != std::max(1u, sd.height >> src_mip)) {
        return false;
    }
    const FormatInfo info = format_table(sd.pixel_format);
    if (!info.is_depth || info.mtl_pixel_format == 0) return false;
    const bool combined =
        info.mtl_pixel_format == MTLPixelFormatDepth32Float_Stencil8;
    if (stencil && !combined) return false;
    if (info.mtl_pixel_format != MTLPixelFormatDepth32Float &&
        info.mtl_pixel_format != MTLPixelFormatDepth16Unorm && !combined) {
        return false;
    }
    auto* metal_dst = static_cast<MetalBackendTexture2D*>(dst);
    const auto* metal_src = static_cast<const MetalBackendTexture2D*>(src);
    void* dst_handle = metal_dst->native_handle();
    void* src_handle = metal_src->native_handle();
    if (dst_handle == nullptr || src_handle == nullptr) return false;
    void* device = device_ptr();
    if (device == nullptr) return false;

    @autoreleasepool {
        id<MTLTexture> dst_texture = (__bridge id<MTLTexture>)dst_handle;
        id<MTLTexture> src_texture = (__bridge id<MTLTexture>)src_handle;
        if ((src_texture.usage & MTLTextureUsageShaderRead) == 0 ||
            (dst_texture.usage & MTLTextureUsageRenderTarget) == 0) {
            return false;
        }
        id<MTLTexture> samples = src_texture;
        if (stencil ||
            src_texture.textureType != MTLTextureType2DMultisample ||
            src_slice != 0 || src_mip != 0) {
            const MTLPixelFormat view_format =
                stencil ? MTLPixelFormatX32_Stencil8 : src_texture.pixelFormat;
            samples = [src_texture
                newTextureViewWithPixelFormat:view_format
                                   textureType:MTLTextureType2DMultisample
                                        levels:NSMakeRange(src_mip, 1)
                                        slices:NSMakeRange(src_slice, 1)];
            if (samples == nil) return false;
        }
        const u32 depth_format = info.mtl_pixel_format;
        const u32 stencil_format = combined ? depth_format : 0u;
        void* pipeline = DepthResolvePrograms::instance().pipeline(
            device, stencil, depth_format, stencil_format);
        void* state =
            DepthResolvePrograms::instance().depth_state(device, stencil);
        if (pipeline == nullptr || state == nullptr) return false;

        CsBeginPass begin{};
        begin.depth = dst_handle;
        begin.depth_level = dst_mip;
        begin.depth_slice = dst_slice;
        begin.stencil = combined;
        begin.preserve = true;
        auto& queue = MetalSubmitQueue::instance();
        const u64 pass = queue.record_begin_pass(begin);
        if (pass == 0) return false;
        CsDraw draw{};
        draw.pipeline = pipeline;
        CFRetain(draw.pipeline);
        draw.depth_stencil = state;
        CFRetain(draw.depth_stencil);
        draw.textures[0] = (__bridge void*)samples;
        CFRetain(draw.textures[0]);
        draw.writes_color = false;
        draw.writes_depth_stencil = true;
        draw.viewport = {0.0f, 0.0f, static_cast<float>(width),
                         static_cast<float>(height), 0.0f, 1.0f};
        draw.primitive = MTLPrimitiveTypeTriangle;
        draw.vertex_count = 3;
        draw.instance_count = 1;
        draw.cull_mode = MTLCullModeNone;
        queue.record_draw(std::move(draw), pass);
        queue.record_end_pass(pass);
    }
    return true;
}

bool MetalDevice::resolve_texture_2d(BackendTexture2D* dst,
                                     unsigned dst_mip,
                                     unsigned dst_slice,
                                     const BackendTexture2D* src,
                                     unsigned src_mip,
                                     unsigned src_slice,
                                     PixelFormat resolve_format) {
    if (!available() || dst == nullptr || src == nullptr || !dst->valid() ||
        !src->valid()) {
        return false;
    }
    const auto& dd = dst->desc();
    const auto& sd = src->desc();
    if (resolve_format == PixelFormat::Unknown || dd.sample_count > 1 ||
        sd.sample_count <= 1 || dst_mip >= dd.mip_levels ||
        src_mip >= sd.mip_levels || dst_slice >= dd.array_size ||
        src_slice >= sd.array_size) {
        return false;
    }
    const unsigned dst_width = std::max(1u, dd.width >> dst_mip);
    const unsigned dst_height = std::max(1u, dd.height >> dst_mip);
    const unsigned src_width = std::max(1u, sd.width >> src_mip);
    const unsigned src_height = std::max(1u, sd.height >> src_mip);
    if (dst_width != src_width || dst_height != src_height) return false;

    if ((dd.pixel_format != resolve_format && !dd.pixel_format_view) ||
        (sd.pixel_format != resolve_format && !sd.pixel_format_view)) {
        return false;
    }
    const auto* metal_dst = static_cast<MetalBackendTexture2D*>(dst);
    const auto* metal_src = static_cast<const MetalBackendTexture2D*>(src);
    void* dst_handle = metal_dst->native_handle();
    void* src_handle = metal_src->native_handle();
    if (dst_handle == nullptr || src_handle == nullptr) return false;
    @autoreleasepool {
        id<MTLTexture> dst_texture = (__bridge id<MTLTexture>)dst_handle;
        id<MTLTexture> src_texture = (__bridge id<MTLTexture>)src_handle;
        const auto resolve_info = format_table(resolve_format);
        const MTLPixelFormat metal_format =
            static_cast<MTLPixelFormat>(resolve_info.mtl_pixel_format);
        if (dd.pixel_format != resolve_format) {
            dst_texture = [dst_texture
                newTextureViewWithPixelFormat:metal_format];
        }
        if (sd.pixel_format != resolve_format) {
            src_texture = [src_texture
                newTextureViewWithPixelFormat:metal_format];
        }
        if (dst_texture == nil || src_texture == nil) return false;
        MetalSubmitQueue::instance().record_resolve(
            (__bridge void*)src_texture, (__bridge void*)dst_texture,
            src_mip, src_slice, dst_mip, dst_slice);
    }
    return true;
}

bool MetalDevice::copy_texture_2d_region(BackendTexture2D* dst, unsigned dst_x,
                                         unsigned dst_y,
                                         const BackendTexture2D* src,
                                         unsigned src_x, unsigned src_y,
                                         unsigned width, unsigned height,
                                         unsigned dst_mip,
                                         unsigned dst_slice,
                                         unsigned src_mip,
                                         unsigned src_slice) {
    if (!available() || dst == nullptr || src == nullptr) return false;
    if (!dst->valid() || !src->valid() || width == 0 || height == 0) {
        return false;
    }
    const auto& dd = dst->desc();
    const auto& sd = src->desc();
    if (dd.pixel_format != sd.pixel_format || dst_mip >= dd.mip_levels ||
        src_mip >= sd.mip_levels || dst_slice >= dd.array_size ||
        src_slice >= sd.array_size) {
        return false;
    }
    const unsigned src_width = std::max(1u, sd.width >> src_mip);
    const unsigned src_height = std::max(1u, sd.height >> src_mip);
    const unsigned dst_width = std::max(1u, dd.width >> dst_mip);
    const unsigned dst_height = std::max(1u, dd.height >> dst_mip);
    if (src_x + width > src_width || src_y + height > src_height ||
        dst_x + width > dst_width || dst_y + height > dst_height) {
        return false;
    }
    const unsigned src_samples = sd.sample_count > 1 ? sd.sample_count : 1;
    const unsigned dst_samples = dd.sample_count > 1 ? dd.sample_count : 1;
    if (src_samples != dst_samples) {
        if (src_samples > 1 && dst_samples <= 1 && dst_x == 0 && dst_y == 0 &&
            src_x == 0 && src_y == 0 && dst_mip == 0 && src_mip == 0 &&
            dst_slice == 0 && src_slice == 0 && width == dd.width &&
            height == dd.height && width == sd.width && height == sd.height) {
            return copy_texture_2d(dst, src);
        }
        if (src_samples <= 1 && dst_samples > 1) {
            return stretch_texture_2d_region(
                dst, dst_x, dst_y, width, height, src, src_x, src_y, width,
                height, BackendFilter::Nearest, dst_slice, src_slice);
        }
        return false;
    }
    auto* metal_dst = static_cast<MetalBackendTexture2D*>(dst);
    const auto* metal_src = static_cast<const MetalBackendTexture2D*>(src);
    void* dst_handle = metal_dst->native_handle();
    void* src_handle = metal_src->native_handle();
    if (dst_handle == nullptr || src_handle == nullptr) return false;
    if (metal_dst->chroma_plane() != nullptr) {
        // Planar video: the luma box and the chroma box it covers, every
        // edge even (measured: odd boxes are dropped, d3d11_nv12_probe
        // [layout] "copy@").
        if (((dst_x | dst_y | src_x | src_y | width | height) & 1u) != 0 ||
            metal_src->chroma_plane() == nullptr ||
            !copy_texture_2d_region(
                metal_dst->chroma_plane(), dst_x / 2u, dst_y / 2u,
                const_cast<MetalBackendTexture2D*>(metal_src->chroma_plane()),
                src_x / 2u, src_y / 2u, width / 2u, height / 2u, dst_mip,
                dst_slice, src_mip, src_slice)) {
            return false;
        }
    }
    MetalSubmitQueue::instance().record_blit_region(
        src_handle, src_x, src_y, dst_handle, dst_x, dst_y, width, height,
        src_mip, src_slice, dst_mip, dst_slice);
    return true;
}

bool MetalDevice::copy_texture_elements(const BackendTextureLocation& dst,
                                        const BackendTextureLocation& src,
                                        unsigned width, unsigned height,
                                        unsigned depth) {
    if (!available() || width == 0 || height == 0 || depth == 0) return false;
    struct Side {
        void* handle{nullptr};
        PixelFormat format{PixelFormat::Unknown};
        unsigned level_width{0};
        unsigned level_height{0};
        unsigned level_depth{1};
        unsigned samples{1};
    };
    const auto describe = [](const BackendTextureLocation& at, Side& side) {
        if (at.texture_3d != nullptr) {
            const auto* volume =
                static_cast<const MetalBackendTexture3D*>(at.texture_3d);
            const auto& d = volume->desc();
            if (!volume->valid() || at.mip >= d.mip_levels || at.slice != 0) {
                return false;
            }
            side.handle = volume->native_handle();
            side.format = d.pixel_format;
            side.level_width = std::max(1u, d.width >> at.mip);
            side.level_height = std::max(1u, d.height >> at.mip);
            side.level_depth = std::max(1u, d.depth >> at.mip);
            return side.handle != nullptr;
        }
        if (at.texture_2d == nullptr) return false;
        const auto* texture =
            static_cast<const MetalBackendTexture2D*>(at.texture_2d);
        const auto& d = texture->desc();
        if (!texture->valid() || at.mip >= d.mip_levels ||
            at.slice >= d.array_size || at.z != 0) {
            return false;
        }
        side.handle = texture->native_handle();
        side.format = d.pixel_format;
        side.level_width = std::max(1u, d.width >> at.mip);
        side.level_height = std::max(1u, d.height >> at.mip);
        side.samples = std::max(1u, d.sample_count);
        return side.handle != nullptr;
    };
    Side s{};
    Side d{};
    if (!describe(src, s) || !describe(dst, d)) return false;
    const FormatInfo sf = format_table(s.format);
    const FormatInfo df = format_table(d.format);
    const u32 s_element = sf.is_compressed ? compressed_block_bytes(s.format)
                                           : sf.bytes_per_pixel;
    const u32 d_element = df.is_compressed ? compressed_block_bytes(d.format)
                                           : df.bytes_per_pixel;
    if (s_element == 0 || s_element != d_element || s.samples != d.samples ||
        (s.format != d.format && (sf.is_depth || df.is_depth))) {
        return false;
    }
    // The source box, in its own texels: whole blocks, or up to the edge.
    if (sf.is_compressed) {
        width = clamp_block_extent(src.x, width, s.level_width);
        height = clamp_block_extent(src.y, height, s.level_height);
    }
    if (src.x >= s.level_width || src.y >= s.level_height ||
        src.z >= s.level_depth || width > s.level_width - src.x ||
        height > s.level_height - src.y || depth > s.level_depth - src.z) {
        return false;
    }
    if (sf.is_compressed &&
        ((src.x % 4u) != 0 || (src.y % 4u) != 0 ||
         ((width % 4u) != 0 && src.x + width != s.level_width) ||
         ((height % 4u) != 0 && src.y + height != s.level_height))) {
        return false;
    }
    const unsigned elements_w = sf.is_compressed ? (width + 3u) / 4u : width;
    const unsigned elements_h = sf.is_compressed ? (height + 3u) / 4u : height;
    // The same elements in the destination's units (19.5.1: a block box may
    // run into the padding past a small mip's virtual edge).
    unsigned dst_width = elements_w;
    unsigned dst_height = elements_h;
    if (df.is_compressed) {
        if ((dst.x % 4u) != 0 || (dst.y % 4u) != 0 ||
            dst.x >= d.level_width || dst.y >= d.level_height) {
            return false;
        }
        dst_width = std::min(elements_w * 4u, d.level_width - dst.x);
        dst_height = std::min(elements_h * 4u, d.level_height - dst.y);
        if ((dst_width + 3u) / 4u != elements_w ||
            (dst_height + 3u) / 4u != elements_h) {
            return false;
        }
    }
    if (dst.x >= d.level_width || dst.y >= d.level_height ||
        dst.z >= d.level_depth || dst_width > d.level_width - dst.x ||
        dst_height > d.level_height - dst.y ||
        depth > d.level_depth - dst.z) {
        return false;
    }
    if (s.format == d.format && s.handle != d.handle) {
        MetalSubmitQueue::instance().record_blit_region(
            s.handle, src.x, src.y, d.handle, dst.x, dst.y, width, height,
            src.mip, src.slice, dst.mip, dst.slice, depth, src.z, dst.z);
        return true;
    }
    // Different formats, or one texture copying into itself: a blit cannot
    // reinterpret or overlap, but the bytes can go through a buffer. The
    // whole source box lands in the buffer before any of it is written back,
    // which is the memmove result D3D11.3 5.6.2.1 asks of an overlap.
    if (s.samples > 1) return false;
    const usize row = static_cast<usize>(elements_w) * s_element;
    const usize image = row * elements_h;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        id<MTLBuffer> staging =
            [device newBufferWithLength:image * depth
                                options:MTLResourceStorageModePrivate];
        if (staging == nil) return false;
        void* buffer = (__bridge void*)staging;
        MetalSubmitQueue::instance().record_download_texture(
            s.handle, src.mip, src.slice, src.x, src.y, src.z, width, height,
            depth, buffer, row, image);
        MetalSubmitQueue::instance().record_upload_texture(
            buffer, d.handle, row, image, dst_width, dst_height, depth,
            dst.mip, dst.slice, dst.x, dst.y, dst.z);
    }
    return true;
}

bool MetalDevice::copy_texture_3d(BackendTexture3D* dst,
                                  const BackendTexture3D* src) {
    if (!available() || dst == nullptr || src == nullptr) return false;
    if (!dst->valid() || !src->valid() || dst == src) return false;
    const auto& dd = dst->desc();
    const auto& sd = src->desc();
    if (dd.width != sd.width || dd.height != sd.height ||
        dd.depth != sd.depth || dd.mip_levels != sd.mip_levels ||
        dd.pixel_format != sd.pixel_format) {
        return false;
    }
    void* dst_handle = static_cast<MetalBackendTexture3D*>(dst)->native_handle();
    void* src_handle =
        static_cast<const MetalBackendTexture3D*>(src)->native_handle();
    if (dst_handle == nullptr || src_handle == nullptr) return false;
    for (unsigned mip = 0; mip < dd.mip_levels; ++mip) {
        MetalSubmitQueue::instance().record_blit_region(
            src_handle, 0, 0, dst_handle, 0, 0, std::max(1u, dd.width >> mip),
            std::max(1u, dd.height >> mip), mip, 0, mip, 0,
            std::max(1u, dd.depth >> mip));
    }
    return true;
}

} // namespace vitreo::graphics
