// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_record_ops.mm
/// @brief Recording the non-draw ops: blits, compute, fences, timestamps,
///        markers and tile mappings.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <pthread.h>

#include <cmath>
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_submit_queue_internal.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <vitreo/present_cpu.hpp>
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace vitreo::graphics {

void MetalSubmitQueue::record_resolve(void* src, void* dst, u32 src_level,
                                      u32 src_slice, u32 dst_level,
                                      u32 dst_slice) {
    Op op{};
    op.kind = OpKind::Resolve;
    op.blit_src = src;
    op.blit_dst = dst;
    op.blit_src_level = src_level;
    op.blit_src_slice = src_slice;
    op.blit_dst_level = dst_level;
    op.blit_dst_slice = dst_slice;
    retain(op.blit_src);
    retain(op.blit_dst);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_blit_region(void* src, u32 src_x, u32 src_y,
                                          void* dst, u32 dst_x, u32 dst_y,
                                          u32 width, u32 height,
                                          u32 src_level, u32 src_slice,
                                          u32 dst_level, u32 dst_slice,
                                          u32 depth, u32 src_z, u32 dst_z) {
    Op op{};
    op.kind = OpKind::BlitRegion;
    op.blit_depth = depth;
    op.blit_src_z = src_z;
    op.blit_dst_z = dst_z;
    op.blit_src = src;
    op.blit_dst = dst;
    op.blit_src_x = src_x;
    op.blit_src_y = src_y;
    op.blit_dst_x = dst_x;
    op.blit_dst_y = dst_y;
    op.blit_src_level = src_level;
    op.blit_src_slice = src_slice;
    op.blit_dst_level = dst_level;
    op.blit_dst_slice = dst_slice;
    op.width = width;
    op.height = height;
    retain(op.blit_src);
    retain(op.blit_dst);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_copy_buffer(void* src, void* dst, usize size,
                                          usize src_offset, usize dst_offset) {
    Op op{};
    op.kind = OpKind::CopyBuffer;
    op.blit_src = src;
    op.blit_dst = dst;
    op.blit_size = size;
    op.blit_src_offset = src_offset;
    op.blit_dst_offset = dst_offset;
    retain(op.blit_src);
    retain(op.blit_dst);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_fill_buffer(void* dst, usize offset, usize size,
                                          u8 value) {
    Op op{};
    op.kind = OpKind::FillBuffer;
    op.blit_dst = dst;
    op.blit_size = size;
    op.blit_dst_offset = offset;
    op.fill_value = value;
    retain(op.blit_dst);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_upload_texture(void* staging, void* texture,
                                             usize bytes_per_row,
                                             usize bytes_per_image, u32 width,
                                             u32 height, u32 depth, u32 level,
                                             u32 slice, u32 x, u32 y, u32 z,
                                             u32 options,
                                             usize staging_offset) {
    Op op{};
    op.kind = OpKind::UploadTexture;
    op.upload_options = options;
    op.blit_src_offset = staging_offset;
    op.blit_src = staging;
    op.blit_dst = texture;
    op.upload_bytes_per_row = bytes_per_row;
    op.upload_bytes_per_image = bytes_per_image;
    op.upload_width = width;
    op.upload_height = height;
    op.upload_depth = depth;
    op.blit_dst_level = level;
    op.blit_dst_slice = slice;
    op.blit_dst_x = x;
    op.blit_dst_y = y;
    op.upload_dst_z = z;
    retain(op.blit_src);
    retain(op.blit_dst);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_download_texture(void* texture, u32 level,
                                               u32 slice, u32 x, u32 y,
                                               u32 z, u32 width, u32 height,
                                               u32 depth, void* buffer,
                                               usize bytes_per_row,
                                               usize bytes_per_image,
                                               usize buffer_offset,
                                               u32 options) {
    Op op{};
    op.kind = OpKind::DownloadTexture;
    op.blit_dst_offset = buffer_offset;
    op.upload_options = options;
    op.blit_src = texture;
    op.blit_dst = buffer;
    op.blit_src_level = level;
    op.blit_src_slice = slice;
    op.blit_src_x = x;
    op.blit_src_y = y;
    op.blit_src_z = z;
    op.upload_width = width;
    op.upload_height = height;
    op.upload_depth = depth;
    op.upload_bytes_per_row = bytes_per_row;
    op.upload_bytes_per_image = bytes_per_image;
    retain(op.blit_src);
    retain(op.blit_dst);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_generate_mips(void* texture) {
    Op op{};
    op.kind = OpKind::GenerateMips;
    op.blit_src = texture;
    retain(op.blit_src);
    append_blit(std::move(op));
}

void MetalSubmitQueue::record_compute(CsCompute&& compute) {
    Op op{};
    op.kind = OpKind::Compute;
    op.compute = std::move(compute);
    retain(op.compute->pipeline);
    for (void* buffer : op.compute->buffers) {
        retain(buffer);
    }
    for (void* texture : op.compute->textures) {
        retain(texture);
    }
    for (void* sampler : op.compute->samplers) {
        retain(sampler);
    }
    append(std::move(op));
}

void MetalSubmitQueue::record_signal_fence(void* event, u64 value) {
    Op op{};
    op.kind = OpKind::SignalFence;
    op.fence_event = event;
    op.fence_value = value;
    retain(op.fence_event);
    append(std::move(op));
}

void MetalSubmitQueue::record_timestamp(
    std::shared_ptr<MetalTimestampCell> cell) {
    Op op{};
    op.kind = OpKind::Timestamp;
    op.timestamp = std::move(cell);
    append(std::move(op));
}

void MetalSubmitQueue::record_debug_marker(DebugMarker kind,
                                           std::string label) {
    Op op{};
    op.kind = OpKind::DebugMarker;
    op.debug_marker = kind;
    op.debug_label = std::move(label);
    append(std::move(op));
}
void MetalSubmitQueue::append_blit(Op&& op) {
    // A tiled resource with two tiles on one pool tile: Metal orders blits by
    // the ranges they touch, not the memory behind them, so fence the blit
    // against everything before and after it (metal_tiled.hpp).
    auto& tiled = MetalTiled::instance();
    const bool fence =
        tiled.self_aliased(op.blit_src) || tiled.self_aliased(op.blit_dst);
    if (fence) {
        Op barrier{};
        barrier.kind = OpKind::TiledBarrier;
        append(std::move(barrier));
    }
    append(std::move(op));
    if (fence) {
        Op barrier{};
        barrier.kind = OpKind::TiledBarrier;
        append(std::move(barrier));
    }
}

void MetalSubmitQueue::record_tile_mapping(
    std::shared_ptr<MetalTileMappingBatch> batch) {
    Op op{};
    op.kind = OpKind::TileMapping;
    op.tiles = std::move(batch);
    append(std::move(op));
}

void MetalSubmitQueue::record_tiled_barrier() {
    Op op{};
    op.kind = OpKind::TiledBarrier;
    append(std::move(op));
}

void metal_record_tile_mapping(std::shared_ptr<MetalTileMappingBatch> batch) {
    MetalSubmitQueue::instance().record_tile_mapping(std::move(batch));
}

void MetalSubmitQueue::record_wait_fence(void* event, u64 value) {
    Op op{};
    op.kind = OpKind::WaitFence;
    op.fence_event = event;
    op.fence_value = value;
    retain(op.fence_event);
    append(std::move(op));
}

} // namespace vitreo::graphics
