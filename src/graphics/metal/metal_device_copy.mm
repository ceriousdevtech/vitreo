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

using namespace vitreo;

namespace vitreo::graphics {

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
