// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_texture_2d_transfer.mm
/// @brief MetalBackendTexture2D CPU uploads and readbacks

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
#include "graphics/metal/metal_backend_texture_ops.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

/// A texture level copied into a Shared buffer by a recorded blit. Ready once
/// the submission holding the blit has retired.
class MetalReadback final : public BackendReadback {
public:
    MetalReadback(void* buffer, u64 generation, usize row_bytes, u32 rows,
                  usize pitch)
        : m_buffer(buffer)
        , m_generation(generation)
        , m_row_bytes(row_bytes)
        , m_rows(rows)
        , m_pitch(pitch) {}
    ~MetalReadback() override {
        if (m_buffer != nullptr) CFRelease(m_buffer);
    }
    MetalReadback(const MetalReadback&) = delete;
    MetalReadback& operator=(const MetalReadback&) = delete;

    bool ready() override {
        auto& queue = MetalSubmitQueue::instance();
        if (queue.completed_generation() >= m_generation) return true;
        // Still in the recording: hand it over so the poll can see it retire.
        if (m_generation >= queue.recording_generation()) {
            (void)queue.submit();
        }
        return queue.completed_generation() >= m_generation;
    }
    bool wait() override {
        return MetalDevice::instance().wait_generation(m_generation);
    }
    bool read(void* data, unsigned row_pitch) override {
        if (data == nullptr || row_pitch < m_row_bytes || !ready()) {
            return false;
        }
        const auto* src = static_cast<const u8*>(
            [(__bridge id<MTLBuffer>)m_buffer contents]);
        for (u32 y = 0; y < m_rows; ++y) {
            std::memcpy(static_cast<u8*>(data) + static_cast<usize>(y) *
                                                     row_pitch,
                        src + static_cast<usize>(y) * m_pitch, m_row_bytes);
        }
        return true;
    }

private:
    void* m_buffer{nullptr};  // retained id<MTLBuffer>
    u64 m_generation{0};
    usize m_row_bytes{0};
    u32 m_rows{0};
    usize m_pitch{0};
};

/// A planar video subresource read back as its guest layout: the luma
/// plane's rows, then the chroma plane's at the same row pitch (Learn
/// DXGI_FORMAT NV12: "The first (SysMemPitch * height) bytes are the Y
/// plane, the remaining (SysMemPitch * (height / 2)) bytes are the UV
/// plane").
class PlanarReadback final : public BackendReadback {
public:
    PlanarReadback(std::shared_ptr<BackendReadback> luma,
                   std::shared_ptr<BackendReadback> chroma, u32 luma_rows)
        : m_luma(std::move(luma)), m_chroma(std::move(chroma)),
          m_luma_rows(luma_rows) {}
    bool ready() override { return m_luma->ready() && m_chroma->ready(); }
    bool wait() override { return m_luma->wait() && m_chroma->wait(); }
    bool read(void* data, unsigned row_pitch) override {
        return data != nullptr && m_luma->read(data, row_pitch) &&
               m_chroma->read(static_cast<u8*>(data) +
                                  static_cast<usize>(row_pitch) * m_luma_rows,
                              row_pitch);
    }

private:
    std::shared_ptr<BackendReadback> m_luma;
    std::shared_ptr<BackendReadback> m_chroma;
    u32 m_luma_rows{0};
};

} // namespace

bool MetalBackendTexture2D::update(unsigned mip_level, const void* data,
                                   unsigned row_pitch) {
    return update_slice(mip_level, 0, data, row_pitch);
}

bool MetalBackendTexture2D::update_slice(unsigned mip_level, unsigned slice,
                                         const void* data, unsigned row_pitch) {
    if (m_plane1) {
        // Planar: the luma rows, then the chroma rows at the same pitch.
        if (!valid() || data == nullptr || mip_level != 0) return false;
        const unsigned pitch = row_pitch != 0
            ? row_pitch
            : m_desc.width * format_table(m_desc.pixel_format)
                                 .bytes_per_pixel;
        return update_plane_slice(mip_level, slice, data, pitch) &&
               m_plane1->update_slice(
                   mip_level, slice,
                   static_cast<const u8*>(data) +
                       static_cast<usize>(pitch) * m_desc.height,
                   pitch);
    }
    return update_plane_slice(mip_level, slice, data, row_pitch);
}

bool MetalBackendTexture2D::update_plane_slice(
    unsigned mip_level, unsigned slice, const void* data, unsigned row_pitch) {
    if (!valid() || data == nullptr) return false;
    if (mip_level >= m_desc.mip_levels) return false;
    if (slice >= m_desc.array_size) return false;
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (format.mtl_pixel_format == 0) return false;
    unsigned level_width = m_desc.width >> mip_level;
    unsigned level_height = m_desc.height >> mip_level;
    if (level_width == 0) level_width = 1;
    if (level_height == 0) level_height = 1;
    usize pitch = row_pitch;
    if (pitch == 0) {
        // Compressed formats report bytes_per_pixel == 0; derive the
        // Metal bytesPerRow from BC block layout instead of rejecting.
        if (format.is_compressed) {
            const u32 block = compressed_block_bytes(m_desc.pixel_format);
            if (block == 0) return false;
            const u32 blocks_w = (level_width + 3u) / 4u;
            pitch = static_cast<usize>(blocks_w) * block;
        } else {
            if (format.bytes_per_pixel == 0) return false;
            pitch = static_cast<usize>(level_width) *
                    format.bytes_per_pixel;
        }
    }
    if (m_desc.cpu_writable && m_texture) {
        // Fresh storage from discard_storage() is referenced by nothing
        // recorded yet, so the CPU may write it without waiting.
        if (!m_storage_fresh) MetalSubmitQueue::instance().wait_encoded();
        m_storage_fresh = false;
        m_texture->upload(data, pitch, mip_level, slice);
        return true;
    }
    // Borrowed swap-chain colour views have no owned m_texture. The
    // same hole crashed GetRenderTargetData until readback() used
    // native_handle(); UpdateSurface to the back buffer hit it next.
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    return blit_cpu_to_texture(queue_ptr, native_handle(),
                               mip_level, slice, data, pitch, level_width,
                               level_height, 1, 0, 0, 0,
                               format.is_compressed);
}

bool MetalBackendTexture2D::readback(unsigned mip_level, void* data,
                                     unsigned row_pitch) {
    return readback_slice(mip_level, 0, data, row_pitch);
}

bool MetalBackendTexture2D::readback_slice(unsigned mip_level, unsigned slice,
                                           void* data, unsigned row_pitch) {
    if (m_plane1) {
        return data != nullptr && readback_plane_slice(
                                      mip_level, slice, data, row_pitch) &&
               m_plane1->readback_slice(
                   mip_level, slice,
                   static_cast<u8*>(data) +
                       static_cast<usize>(row_pitch) * m_desc.height,
                   row_pitch);
    }
    return readback_plane_slice(mip_level, slice, data, row_pitch);
}

bool MetalBackendTexture2D::readback_plane_slice(
    unsigned mip_level, unsigned slice, void* data, unsigned row_pitch) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (format.mtl_pixel_format == 0) return false;
    if (format.is_compressed) {
        // A block-compressed subresource reads back as rows of 4x4
        // blocks -- the layout D3D11 staging Map exposes (RowPitch
        // spans a row of blocks). Learn CopyResource: BC textures copy
        // to STAGING like any other format.
        const u32 block = compressed_block_bytes(m_desc.pixel_format);
        if (block == 0) return false;
        return blit_to_host(mip_level, slice, 0, MTLBlitOptionNone, data,
                            row_pitch, block);
    }
    if (format.bytes_per_pixel == 0) return false;
    return blit_to_host(mip_level, slice, format.bytes_per_pixel,
                        MTLBlitOptionNone, data, row_pitch);
}

bool MetalBackendTexture2D::update_depth_stencil_plane(
    unsigned mip_level, unsigned slice, bool stencil, const void* data,
    unsigned row_pitch) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    void* handle = native_handle();
    if (!valid() || handle == nullptr || data == nullptr ||
        format.mtl_pixel_format != MTLPixelFormatDepth32Float_Stencil8 ||
        mip_level >= m_desc.mip_levels || slice >= m_desc.array_size ||
        m_desc.sample_count > 1) {
        return false;
    }
    const unsigned width = std::max(1u, m_desc.width >> mip_level);
    const unsigned height = std::max(1u, m_desc.height >> mip_level);
    const usize row = static_cast<usize>(width) * (stencil ? 1u : 4u);
    if (row_pitch < row) return false;
    const usize pitch = (row + 255u) & ~usize{255u};
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)handle;
        id<MTLBuffer> staging =
            [texture.device newBufferWithLength:pitch * height
                                        options:MTLResourceStorageModeShared];
        if (staging == nil) return false;
        auto* out = static_cast<u8*>([staging contents]);
        for (unsigned y = 0; y < height; ++y) {
            std::memcpy(out + y * pitch,
                        static_cast<const u8*>(data) +
                            static_cast<usize>(y) * row_pitch,
                        row);
        }
        // A blit into a combined depth+stencil texture names the plane.
        MetalSubmitQueue::instance().record_upload_texture(
            (__bridge void*)staging, handle, pitch, pitch * height, width,
            height, 1, mip_level, slice, 0, 0, 0,
            static_cast<u32>(stencil ? MTLBlitOptionStencilFromDepthStencil
                                     : MTLBlitOptionDepthFromDepthStencil));
    }
    return true;
}

std::shared_ptr<BackendReadback> MetalBackendTexture2D::readback_slice_async(
    unsigned mip_level, unsigned slice) {
    if (m_plane1) {
        auto luma = readback_plane_slice_async(mip_level, slice);
        auto chroma = m_plane1->readback_slice_async(mip_level, slice);
        if (!luma || !chroma) return nullptr;
        return std::make_shared<PlanarReadback>(
            std::move(luma), std::move(chroma),
            std::max(1u, m_desc.height >> mip_level));
    }
    return readback_plane_slice_async(mip_level, slice);
}

std::shared_ptr<BackendReadback>
MetalBackendTexture2D::readback_plane_slice_async(unsigned mip_level,
                                                  unsigned slice) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    const u32 block = format.is_compressed
        ? compressed_block_bytes(m_desc.pixel_format) : 0u;
    if (!valid() || MetalSubmitQueue::thread_capturing() ||
        format.is_depth ||
        m_desc.sample_count > 1 || mip_level >= m_desc.mip_levels ||
        slice >= m_desc.array_size ||
        (format.is_compressed ? block == 0
                              : format.bytes_per_pixel == 0)) {
        return nullptr;
    }
    const unsigned width = std::max(1u, m_desc.width >> mip_level);
    const unsigned height = std::max(1u, m_desc.height >> mip_level);
    const u32 rows = format.is_compressed ? (height + 3u) / 4u : height;
    const usize row_bytes =
        format.is_compressed
            ? static_cast<usize>((width + 3u) / 4u) * block
            : static_cast<usize>(width) * format.bytes_per_pixel;
    const usize pitch = (row_bytes + 255u) & ~usize{255u};
    void* handle = native_handle();
    if (handle == nullptr) return nullptr;
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)handle;
        id<MTLBuffer> host =
            [texture.device newBufferWithLength:pitch * rows
                                        options:MTLResourceStorageModeShared];
        if (host == nil) return nullptr;
        auto& queue = MetalSubmitQueue::instance();
        queue.record_download_texture(
            handle, mip_level, slice, 0, 0, 0, width, height, 1,
            (__bridge void*)host, pitch, pitch * rows);
        // The generation the copy joined, taken under the queue lock as
        // it was appended (last_recorded_generation). A
        // recording_generation() read after recording named the NEXT
        // recording whenever another thread submitted in between: one
        // that holds nothing and is never submitted until something else
        // is recorded. ready() then never turned true (Map DO_NOT_WAIT
        // answered WAS_STILL_DRAWING forever), and after wait() -- which
        // only waits for submitted work -- read() refused, so a blocking
        // Map READ answered S_OK with the staging shadow's old bytes.
        u64 generation = MetalSubmitQueue::last_recorded_generation();
        if (generation == 0) generation = queue.recording_generation();
        return std::make_shared<MetalReadback>(
            (__bridge_retained void*)host, generation, row_bytes, rows,
            pitch);
    }
}

bool MetalBackendTexture2D::readback_depth_stencil_plane(
    unsigned mip_level, unsigned slice, bool stencil, void* data,
    unsigned row_pitch) {
    // Depth32Float_Stencil8 keeps depth and stencil in separate planes, and
    // a blit of the combined format must name the plane it reads: four
    // float bytes per texel for depth, one byte for stencil. Every guest
    // depth+stencil format maps here (format_converter.hpp).
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (format.mtl_pixel_format != MTLPixelFormatDepth32Float_Stencil8) {
        return false;
    }
    return blit_to_host(mip_level, slice, stencil ? 1u : 4u,
                        stencil ? MTLBlitOptionStencilFromDepthStencil
                                : MTLBlitOptionDepthFromDepthStencil,
                        data, row_pitch);
}

/// Copy one subresource (or one plane of it) into @p data, @p row_pitch
/// bytes per row, after everything already recorded against it.
/// With @p block_bytes non-zero the format is block-compressed: rows are
/// rows of 4x4 blocks of that many bytes, @p bytes_per_texel is unused.
bool MetalBackendTexture2D::blit_to_host(
    unsigned mip_level, unsigned slice, usize bytes_per_texel,
    MTLBlitOption options, void* data, unsigned row_pitch, u32 block_bytes) {
    if (!valid() || data == nullptr || mip_level >= m_desc.mip_levels ||
        slice >= m_desc.array_size) {
        return false;
    }
    // MSAA attachments cannot blit to a buffer. The swap chain resolves
    // first and reads the 1x target.
    if (m_desc.sample_count > 1) {
        return false;
    }
    unsigned width = m_desc.width >> mip_level;
    unsigned height = m_desc.height >> mip_level;
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    // The blit copies width x height texels (a compressed level smaller
    // than a block is copied whole: Metal allows a region that reaches
    // the level's edge). The host layout is in rows of texels, or rows
    // of blocks for a compressed format.
    const unsigned rows = block_bytes != 0 ? (height + 3u) / 4u : height;
    const usize row_bytes =
        block_bytes != 0
            ? static_cast<usize>((width + 3u) / 4u) * block_bytes
            : static_cast<usize>(width) * bytes_per_texel;
    if (row_pitch < row_bytes || row_pitch > SIZE_MAX / rows) {
        return false;
    }

    // Swap-chain colour views borrow the Metal texture and have no
    // owned m_texture. Using that pointer crashed GetRenderTargetData
    // after every D3D9 oracle scene that left the back buffer bound.
    void* handle = native_handle();
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    if (handle == nullptr || queue_ptr == nullptr) return false;

    id<MTLTexture> texture = (__bridge id<MTLTexture>)handle;
    id<MTLDevice> device = texture.device;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
    if (texture == nil || device == nil || queue == nil) return false;

    // D3D11 CopySubresourceRegion GPU→staging (and Map of that staging
    // texture) must observe draws already encoded against this resource.
    // Clears/draws now live on the submit queue; commit_frame alone
    // leaves the texture uninitialized (magenta) and the blit wins the
    // race. drain() submits the queue and waits before this blit.
    MetalSubmitQueue::instance().drain();
    const usize transfer_pitch = (row_bytes + 255u) & ~usize{255u};
    if (transfer_pitch > SIZE_MAX / rows) return false;
    @autoreleasepool {
        id<MTLBuffer> readback =
            [device newBufferWithLength:transfer_pitch * rows
                                options:MTLResourceStorageModeShared];
        if (readback == nil) return false;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture
                  sourceSlice:slice
                  sourceLevel:mip_level
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:readback
            destinationOffset:0
       destinationBytesPerRow:transfer_pitch
     destinationBytesPerImage:transfer_pitch * rows
                      options:options];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            return false;
        }
        const auto* source =
            static_cast<const u8*>([readback contents]);
        auto* destination = static_cast<u8*>(data);
        for (unsigned y = 0; y < rows; ++y) {
            std::memcpy(destination + static_cast<usize>(y) * row_pitch,
                        source + static_cast<usize>(y) * transfer_pitch,
                        row_bytes);
        }
    }
    return true;
}

bool MetalBackendTexture2D::update_rows(unsigned mip_level, const void* data,
                                        unsigned row_pitch, unsigned y,
                                        unsigned height) {
    if (!valid() || data == nullptr || height == 0) return false;
    if (mip_level >= m_desc.mip_levels) return false;
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (format.mtl_pixel_format == 0) return false;
    if (format.is_compressed) return false;
    unsigned level_width = m_desc.width >> mip_level;
    unsigned level_height = m_desc.height >> mip_level;
    if (level_width == 0) level_width = 1;
    if (level_height == 0) level_height = 1;
    if (y >= level_height) return false;
    if (y + height > level_height) return false;
    usize pitch = row_pitch;
    if (pitch == 0) {
        if (format.bytes_per_pixel == 0) return false;
        pitch = static_cast<usize>(level_width) * format.bytes_per_pixel;
    }
    if (m_desc.cpu_writable && m_texture) {
        m_texture->upload_rows(data, pitch, mip_level, 0, y, height);
        return true;
    }
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    return blit_cpu_to_texture(queue_ptr, native_handle(),
                               mip_level, 0, data, pitch, level_width,
                               height, 1, 0, y, 0, false);
}

bool MetalBackendTexture2D::update_region(
    unsigned mip_level, unsigned slice, const void* data, unsigned row_pitch,
    unsigned x, unsigned y, unsigned width, unsigned height) {
    if (m_plane1) {
        // Planar: a box in luma texels, even on every edge; the source
        // holds the box's luma rows, then its chroma rows at the same
        // pitch (measured, d3d11_nv12_probe [layout] "box").
        if (data == nullptr || (x | y | width | height) % 2u != 0) {
            return false;
        }
        const unsigned pitch = row_pitch != 0
            ? row_pitch
            : width * format_table(m_desc.pixel_format).bytes_per_pixel;
        return update_plane_region(mip_level, slice, data, pitch, x, y,
                                   width, height) &&
               m_plane1->update_region(
                   mip_level, slice,
                   static_cast<const u8*>(data) +
                       static_cast<usize>(pitch) * height,
                   pitch, x / 2u, y / 2u, width / 2u, height / 2u);
    }
    return update_plane_region(mip_level, slice, data, row_pitch, x, y,
                               width, height);
}

bool MetalBackendTexture2D::update_plane_region(
    unsigned mip_level, unsigned slice, const void* data, unsigned row_pitch,
    unsigned x, unsigned y, unsigned width, unsigned height) {
    if (!valid() || data == nullptr || width == 0 || height == 0 ||
        mip_level >= m_desc.mip_levels || slice >= m_desc.array_size) {
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
    if (x >= level_width || y >= level_height ||
        width > level_width - x || height > level_height - y) {
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
    const usize pitch = row_pitch != 0 ? row_pitch : min_pitch;
    if (pitch < min_pitch) return false;
    if (m_desc.cpu_writable && m_texture) {
        MetalSubmitQueue::instance().wait_encoded();
        m_texture->upload_region(data, pitch, mip_level, slice,
                                 x, y, width, height);
        return true;
    }
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    return blit_cpu_to_texture(queue_ptr, native_handle(), mip_level,
                               slice, data, pitch, width, height, 1,
                               x, y, 0, format.is_compressed);
}

} // namespace vitreo::graphics
