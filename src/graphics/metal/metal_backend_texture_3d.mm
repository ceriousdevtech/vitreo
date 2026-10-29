// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_texture_3d.mm
/// @brief MetalBackendTexture3D and the volume creation members of MetalDevice

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
#include "graphics/metal/metal_backend_texture_3d.hpp"
#include "graphics/metal/metal_backend_texture_ops.hpp"

using namespace vitreo;

namespace vitreo::graphics {

/// The MetalTexture description of @p desc (as for
/// MetalBackendTexture2D::describe). False when it cannot be one.
[[nodiscard]] bool MetalBackendTexture3D::describe(
    const BackendTexture3DDesc& desc, bool has_initial_data,
    TextureDesc& metal_desc) {
    const FormatInfo format = format_table(desc.pixel_format);
    if (format.mtl_pixel_format == 0 || desc.width == 0 ||
        desc.height == 0 || desc.depth == 0 || desc.mip_levels == 0) {
        return false;
    }
    metal_desc.width = desc.width;
    metal_desc.height = desc.height;
    metal_desc.depth = desc.depth;
    metal_desc.mip_levels = desc.mip_levels;
    metal_desc.array_size = 1;
    metal_desc.texture_type = 7u; // MTLTextureType3D
    metal_desc.pixel_format = format.mtl_pixel_format;
    metal_desc.usage =
        (desc.shader_resource ? MTLTextureUsageShaderRead : 0) |
        (desc.render_target ? MTLTextureUsageRenderTarget : 0) |
        (desc.unordered_access ? MTLTextureUsageShaderWrite : 0) |
        (desc.unordered_access ? MTLTextureUsageShaderRead : 0) |
        // fill_texels writes through an unsigned-integer view.
        (desc.unordered_access ? MTLTextureUsagePixelFormatView : 0) |
        (desc.pixel_format_view ? MTLTextureUsagePixelFormatView : 0);
    metal_desc.storage_mode =
        (has_initial_data || desc.cpu_writable)
            ? MTLStorageModeShared
            : MTLStorageModePrivate;
    if (desc.heap != nullptr) {
        // Placed in a D3D12 heap: see MetalBackendTexture2D::describe.
        metal_desc.heap_placed = true;
        metal_desc.placement_heap = metal_heap_handle(desc.heap);
        metal_desc.heap_offset = desc.heap_offset;
        metal_desc.storage_mode = MTLStorageModePrivate;
    }
    return true;
}

MetalBackendTexture3D::MetalBackendTexture3D(
    void* device, void* command_queue, BackendTexture3DDesc desc,
    const void* initial_data, unsigned initial_row_pitch,
    unsigned initial_slice_pitch)
    : m_desc(desc)
    , m_command_queue(command_queue) {
    const FormatInfo format = format_table(desc.pixel_format);
    TextureDesc metal_desc{};
    if (!describe(desc, initial_data != nullptr, metal_desc)) return;
    m_texture = std::make_shared<MetalTexture>(device, metal_desc);
    if (m_texture->handle() == nullptr) {
        m_texture.reset();
        return;
    }
    m_device = device;
    if (desc.shareable) {
        // A volume has no IOSurface form: always mirrored through a store.
        SharedStoreRecord record = shared_record_for(desc);
        record.frontend = desc.shared_metadata;
        m_shared = std::make_shared<MetalSharedLocal>(device,
                                                      std::move(record));
        m_shared->texture = m_texture;
        MetalSharedRegistry::instance().add(m_shared, m_texture->handle());
    }
    if (initial_data != nullptr) {
        usize row_pitch = initial_row_pitch;
        usize slice_pitch = initial_slice_pitch;
        if (format.is_compressed) {
            const u32 block = compressed_block_bytes(desc.pixel_format);
            const u32 blocks_w = (desc.width + 3u) / 4u;
            const u32 blocks_h = (desc.height + 3u) / 4u;
            if (row_pitch == 0 && block != 0) {
                row_pitch = static_cast<usize>(blocks_w) * block;
            }
            if (slice_pitch == 0 && row_pitch != 0) {
                slice_pitch = row_pitch * blocks_h;
            }
        } else if (format.bytes_per_pixel != 0) {
            if (row_pitch == 0) {
                row_pitch = static_cast<usize>(desc.width) *
                            format.bytes_per_pixel;
            }
            if (slice_pitch == 0) {
                slice_pitch = row_pitch * desc.height;
            }
        }
        if (row_pitch != 0 && slice_pitch != 0) {
            m_texture->upload_volume(initial_data, row_pitch, slice_pitch);
        }
    } else if (metal_desc.storage_mode == MTLStorageModePrivate &&
               desc.heap == nullptr) {
        // Not a placed texture: those are windows of heap memory other
        // resources may have written (see MetalBackendTexture2D).
        zero_initialize(format);
    }
}

bool MetalBackendTexture3D::readback_w_slice(
    unsigned mip_level, unsigned w_slice, void* data, unsigned row_pitch) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    // A block-compressed W slice reads back as rows of 4x4 blocks, the
    // layout a D3D11 staging volume's Map exposes.
    const u32 block = format.is_compressed
        ? compressed_block_bytes(m_desc.pixel_format) : 0u;
    if (!valid() || data == nullptr ||
        (format.is_compressed ? block == 0
                              : format.bytes_per_pixel == 0) ||
        mip_level >= m_desc.mip_levels) {
        return false;
    }
    const u32 w = std::max(1u, m_desc.width >> mip_level);
    const u32 h = std::max(1u, m_desc.height >> mip_level);
    const u32 d = std::max(1u, m_desc.depth >> mip_level);
    if (w_slice >= d) return false;
    const usize row = format.is_compressed
        ? static_cast<usize>((w + 3u) / 4u) * block
        : static_cast<usize>(w) * format.bytes_per_pixel;
    const u32 rows = format.is_compressed ? (h + 3u) / 4u : h;
    if (row_pitch < row) return false;
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    if (queue_ptr == nullptr) return false;
    MetalSubmitQueue::instance().drain();
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)m_texture->handle();
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
        id<MTLBuffer> host =
            [texture.device newBufferWithLength:row * rows
                                        options:MTLResourceStorageModeShared];
        if (host == nil) return false;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:mip_level
                 sourceOrigin:MTLOriginMake(0, 0, w_slice)
                   sourceSize:MTLSizeMake(w, h, 1)
                     toBuffer:host
            destinationOffset:0
       destinationBytesPerRow:row
     destinationBytesPerImage:row * rows];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        const u8* src = static_cast<const u8*>(host.contents);
        for (u32 y = 0; y < rows; ++y) {
            std::memcpy(static_cast<u8*>(data) + static_cast<usize>(y) * row_pitch,
                        src + static_cast<usize>(y) * row, row);
        }
    }
    return true;
}

/// As MetalBackendTexture2D::zero_initialize: WDDM zero-fills new GPU
/// allocations, Metal private storage does not. A blit covers every
/// mip of the volume; committed immediately, like CPU uploads.
void MetalBackendTexture3D::zero_initialize(const FormatInfo& format) {
    const u32 block = format.is_compressed
        ? compressed_block_bytes(m_desc.pixel_format) : 0u;
    const u32 pixel = format.bytes_per_pixel;
    if (format.is_compressed ? block == 0 : pixel == 0) return;
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    if (queue_ptr == nullptr || !valid()) return;
    const auto row_of = [&](u32 w) {
        return format.is_compressed
            ? static_cast<usize>((w + 3u) / 4u) * block
            : static_cast<usize>(w) * pixel;
    };
    const auto rows_of = [&](u32 h) {
        return static_cast<usize>(format.is_compressed ? (h + 3u) / 4u : h);
    };
    const u32 w0 = m_desc.width, h0 = m_desc.height, d0 = m_desc.depth;
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)m_texture->handle();
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
        id<MTLBuffer> zeros = [texture.device
            newBufferWithLength:row_of(w0) * rows_of(h0) * d0
                        options:MTLResourceStorageModeShared];
        if (zeros == nil) return;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        for (unsigned mip = 0; mip < m_desc.mip_levels; ++mip) {
            const u32 w = std::max(1u, w0 >> mip);
            const u32 h = std::max(1u, h0 >> mip);
            const u32 d = std::max(1u, d0 >> mip);
            [blit copyFromBuffer:zeros
                    sourceOffset:0
               sourceBytesPerRow:row_of(w)
             sourceBytesPerImage:row_of(w) * rows_of(h)
                      sourceSize:MTLSizeMake(w, h, d)
                       toTexture:texture
                destinationSlice:0
                destinationLevel:mip
               destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding];
        [command commit];
    }
}

bool MetalBackendTexture3D::clear_render_target_slices(
    const std::array<float, 4>& color, unsigned mip_level,
    unsigned first_w_slice, unsigned w_size) {
    const unsigned mip_depth =
        std::max(1u, m_desc.depth >> mip_level);
    if (m_texture == nullptr || !m_desc.render_target ||
        mip_level >= m_desc.mip_levels || w_size == 0 ||
        first_w_slice >= mip_depth ||
        w_size > mip_depth - first_w_slice) {
        return false;
    }
    // Metal clears one depth plane per pass: a render pass names a single
    // attachment slice, and renderTargetArrayLength is for array layers,
    // not for the W axis of a 3D texture.
    for (unsigned i = 0; i < w_size; ++i) {
        CsBeginPass begin{};
        begin.colors[0] = m_texture->handle();
        begin.color_count = 1;
        begin.clear_color = true;
        begin.color_clear = color;
        begin.color_levels[0] = mip_level;
        begin.color_depth_planes[0] = first_w_slice + i;
        const u64 pass_id =
            MetalSubmitQueue::instance().record_begin_pass(begin);
        MetalSubmitQueue::instance().record_end_pass(pass_id);
    }
    return true;
}

bool MetalBackendTexture3D::update(unsigned mip_level, const void* data,
                                   unsigned row_pitch, unsigned slice_pitch) {
    if (!valid() || data == nullptr) return false;
    if (mip_level >= m_desc.mip_levels) return false;
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (format.mtl_pixel_format == 0) return false;
    unsigned level_width = m_desc.width >> mip_level;
    unsigned level_height = m_desc.height >> mip_level;
    unsigned level_depth = m_desc.depth >> mip_level;
    if (level_width == 0) level_width = 1;
    if (level_height == 0) level_height = 1;
    if (level_depth == 0) level_depth = 1;
    usize pitch = row_pitch;
    usize slice = slice_pitch;
    if (format.is_compressed) {
        const u32 block = compressed_block_bytes(m_desc.pixel_format);
        if (block == 0) return false;
        const u32 blocks_w = (level_width + 3u) / 4u;
        const u32 blocks_h = (level_height + 3u) / 4u;
        if (pitch == 0) pitch = static_cast<usize>(blocks_w) * block;
        if (slice == 0) slice = pitch * blocks_h;
    } else {
        if (format.bytes_per_pixel == 0) return false;
        if (pitch == 0) {
            pitch = static_cast<usize>(level_width) *
                    format.bytes_per_pixel;
        }
        if (slice == 0) slice = pitch * level_height;
    }
    if (m_desc.cpu_writable) {
        m_texture->upload_volume(data, pitch, slice, mip_level);
        return true;
    }
    return blit_cpu_to_texture(m_command_queue, m_texture->handle(),
                               mip_level, 0, data, pitch, level_width,
                               level_height, level_depth, 0, 0, slice,
                               format.is_compressed);
}

bool MetalBackendTexture3D::update_region(
    unsigned mip_level, const void* data, unsigned row_pitch,
    unsigned slice_pitch, unsigned x, unsigned y, unsigned z, unsigned width,
    unsigned height, unsigned depth) {
    if (!valid() || data == nullptr || width == 0 || height == 0 ||
        depth == 0 || mip_level >= m_desc.mip_levels) {
        return false;
    }
    const FormatInfo format = format_table(m_desc.pixel_format);
    const u32 block_bytes = compressed_block_bytes(m_desc.pixel_format);
    if (format.mtl_pixel_format == 0 ||
        (format.is_compressed ? block_bytes == 0
                              : format.bytes_per_pixel == 0)) {
        return false;
    }
    const unsigned level_width = std::max(1u, m_desc.width >> mip_level);
    const unsigned level_height = std::max(1u, m_desc.height >> mip_level);
    const unsigned level_depth = std::max(1u, m_desc.depth >> mip_level);
    if (x >= level_width || y >= level_height || z >= level_depth ||
        width > level_width - x || height > level_height - y ||
        depth > level_depth - z) {
        return false;
    }
    if (format.is_compressed &&
        ((x % 4u) != 0 || (y % 4u) != 0 ||
         ((width % 4u) != 0 && x + width != level_width) ||
         ((height % 4u) != 0 && y + height != level_height))) {
        return false;
    }
    const usize min_pitch =
        format.is_compressed
            ? static_cast<usize>((width + 3u) / 4u) * block_bytes
            : static_cast<usize>(width) * format.bytes_per_pixel;
    const usize rows = format.is_compressed ? (height + 3u) / 4u : height;
    const usize pitch = row_pitch != 0 ? row_pitch : min_pitch;
    const usize slice = slice_pitch != 0 ? slice_pitch : pitch * rows;
    if (pitch < min_pitch || slice < pitch * rows) return false;
    // Recorded like every other upload (blit_cpu_to_texture), one W
    // plane at a time so each reads its own slice_pitch-strided rows.
    for (unsigned plane = 0; plane < depth; ++plane) {
        if (!blit_cpu_to_texture(
                m_command_queue, m_texture->handle(), mip_level, 0,
                static_cast<const u8*>(data) + plane * slice, pitch,
                width, height, 1, x, y, 0, format.is_compressed,
                z + plane)) {
            return false;
        }
    }
    return true;
}

bool MetalBackendTexture3D::fill_texels(
    const void* texel, unsigned texel_bytes, unsigned mip_level, unsigned x,
    unsigned y, unsigned z, unsigned width, unsigned height, unsigned depth) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (!valid() || texel == nullptr || format.is_compressed ||
        texel_bytes == 0 || texel_bytes != format.bytes_per_pixel ||
        mip_level >= m_desc.mip_levels || width == 0 || height == 0 ||
        depth == 0) {
        return false;
    }
    const unsigned level_width = std::max(1u, m_desc.width >> mip_level);
    const unsigned level_height = std::max(1u, m_desc.height >> mip_level);
    const unsigned level_depth = std::max(1u, m_desc.depth >> mip_level);
    if (x >= level_width || y >= level_height || z >= level_depth ||
        width > level_width - x || height > level_height - y ||
        depth > level_depth - z) {
        return false;
    }
    if (m_desc.unordered_access) {
        void* view = fill_view(mip_level, texel_bytes);
        if (view != nullptr &&
            record_fill_dispatch(true, view, texel, texel_bytes, x, y, z,
                                 width, height, depth)) {
            return true;
        }
    }
    return record_fill_blit(m_texture->handle(), texel, texel_bytes,
                            mip_level, 0, 1, z, x, y, width, height,
                            depth);
}

BackendTexture3D* MetalBackendTexture3D::view(
    PixelFormat format, unsigned first_mip, unsigned mip_count) {
    if (!valid() || m_view_handle != nullptr) {
        // A view of a view is never asked for: the frontend views the
        // resource's own storage.
        return nullptr;
    }
    if (format == PixelFormat::Unknown) format = m_desc.pixel_format;
    if (mip_count == 0 || first_mip >= m_desc.mip_levels ||
        mip_count > m_desc.mip_levels - first_mip) {
        return nullptr;
    }
    if (format == m_desc.pixel_format && first_mip == 0 &&
        mip_count == m_desc.mip_levels) {
        return this;
    }
    const FormatInfo view_format = format_table(format);
    if (view_format.mtl_pixel_format == 0 ||
        view_format.bytes_per_pixel !=
            format_table(m_desc.pixel_format).bytes_per_pixel) {
        return nullptr;
    }
    const u64 key = (static_cast<u64>(format) << 32u) |
                    (static_cast<u64>(first_mip) << 16u) | mip_count;
    std::lock_guard<std::mutex> lock(m_views_mutex);
    if (const auto found = m_views.find(key); found != m_views.end()) {
        return found->second.get();
    }
    void* retained = nullptr;
    @autoreleasepool {
        id<MTLTexture> texture =
            (__bridge id<MTLTexture>)m_texture->handle();
        // Metal views a 3D texture only whole in W, and under another
        // pixel format only when it was created for that.
        if (format != m_desc.pixel_format &&
            (texture.usage & MTLTextureUsagePixelFormatView) == 0) {
            return nullptr;
        }
        id<MTLTexture> view = [texture
            newTextureViewWithPixelFormat:static_cast<MTLPixelFormat>(
                                              view_format.mtl_pixel_format)
                              textureType:MTLTextureType3D
                                   levels:NSMakeRange(first_mip,
                                                      mip_count)
                                   slices:NSMakeRange(0, 1)];
        if (view == nil) return nullptr;
        retained = (__bridge_retained void*)view;
    }
    BackendTexture3DDesc view_desc = m_desc;
    view_desc.pixel_format = format;
    view_desc.width = std::max(1u, m_desc.width >> first_mip);
    view_desc.height = std::max(1u, m_desc.height >> first_mip);
    view_desc.depth = std::max(1u, m_desc.depth >> first_mip);
    view_desc.mip_levels = mip_count;
    view_desc.shareable = false;
    view_desc.shared_metadata.clear();
    auto child = std::unique_ptr<MetalBackendTexture3D>(
        new MetalBackendTexture3D(*this, retained, std::move(view_desc)));
    BackendTexture3D* result = child.get();
    m_views.emplace(key, std::move(child));
    return result;
}

MetalBackendTexture3D::~MetalBackendTexture3D() {
    for (const auto& [key, view] : m_fill_views) {
        (void)key;
        CFRelease(view);
    }
    if (m_view_handle != nullptr) CFRelease(m_view_handle);
}

[[nodiscard]] SharedStoreRecord MetalBackendTexture3D::shared_record_for(
    const BackendTexture3DDesc& desc) {
    SharedStoreRecord record{};
    record.kind = SharedStoreKind::Texture3D;
    record.pixel_format = static_cast<u32>(desc.pixel_format);
    record.width = desc.width;
    record.height = desc.height;
    record.depth = desc.depth;
    record.mip_levels = desc.mip_levels;
    record.array_size = 1;
    record.sample_count = 1;
    record.flags =
        (desc.render_target ? SharedStoreRecord::kRenderTarget : 0u) |
        (desc.shader_resource ? SharedStoreRecord::kShaderResource : 0u) |
        (desc.unordered_access ? SharedStoreRecord::kUnorderedAccess
                               : 0u) |
        (desc.pixel_format_view ? SharedStoreRecord::kPixelFormatView
                                : 0u);
    return record;
}

[[nodiscard]] BackendTexture3DDesc MetalBackendTexture3D::desc_from_record(
    const SharedStoreRecord& record) {
    BackendTexture3DDesc desc{};
    desc.width = record.width;
    desc.height = record.height;
    desc.depth = record.depth;
    desc.mip_levels = record.mip_levels;
    desc.pixel_format = static_cast<PixelFormat>(record.pixel_format);
    desc.render_target =
        (record.flags & SharedStoreRecord::kRenderTarget) != 0;
    desc.shader_resource =
        (record.flags & SharedStoreRecord::kShaderResource) != 0;
    desc.unordered_access =
        (record.flags & SharedStoreRecord::kUnorderedAccess) != 0;
    desc.pixel_format_view =
        (record.flags & SharedStoreRecord::kPixelFormatView) != 0;
    desc.shared_metadata = record.frontend;
    return desc;
}

/// A one-level unsigned-integer texture3d view of `mip`, cached.
[[nodiscard]] void* MetalBackendTexture3D::fill_view(
    unsigned mip, unsigned texel_bytes) {
    const u32 key = (mip << 8u) | texel_bytes;
    const auto found = m_fill_views.find(key);
    if (found != m_fill_views.end()) return found->second;
    const FormatInfo view_format =
        format_table(uint_view_format(texel_bytes));
    if (view_format.mtl_pixel_format == 0) return nullptr;
    @autoreleasepool {
        id<MTLTexture> texture =
            (__bridge id<MTLTexture>)m_texture->handle();
        id<MTLTexture> view = [texture
            newTextureViewWithPixelFormat:static_cast<MTLPixelFormat>(
                                              view_format.mtl_pixel_format)
                              textureType:MTLTextureType3D
                                   levels:NSMakeRange(mip, 1)
                                   slices:NSMakeRange(0, 1)];
        if (view == nil) return nullptr;
        void* retained = (__bridge_retained void*)view;
        m_fill_views.emplace(key, retained);
        return retained;
    }
}

std::unique_ptr<BackendTexture3D> MetalDevice::create_texture_3d(
    const BackendTexture3DDesc& desc, const void* initial_data,
    unsigned initial_row_pitch, unsigned initial_slice_pitch) {
    if (!vitreo::memory::host_can_allocate_gpu(
            estimated_texture_bytes(desc.pixel_format, desc.width,
                                    desc.height, desc.depth, desc.mip_levels,
                                    1, 1, initial_data != nullptr),
            gpu_working_set(), "GPU texture3d")) {
        return nullptr;
    }
    auto texture = std::make_unique<MetalBackendTexture3D>(
        m_device, m_command_queue, desc, initial_data, initial_row_pitch,
        initial_slice_pitch);
    if (!texture->valid()) return nullptr;
    return texture;
}

bool MetalDevice::heap_texture_placement(const BackendTexture3DDesc& desc,
                                         u64& size, u64& alignment) {
    if (!available() || desc.shareable) return false;
    TextureDesc metal_desc{};
    if (!MetalBackendTexture3D::describe(desc, false, metal_desc)) return false;
    metal_desc.heap_placed = true;
    metal_desc.placement_heap = nullptr;
    return MetalTexture::placement_size_and_align(m_device, metal_desc, size,
                                                  alignment);
}

} // namespace vitreo::graphics
