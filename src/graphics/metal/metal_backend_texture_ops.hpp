// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_texture_ops.hpp
/// @brief CPU uploads and texel fills recorded on the submit queue
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

namespace vitreo::graphics {

/// Copy CPU pixels into a Private MTLTexture. `replaceRegion:` cannot target
/// Private storage; a Shared staging buffer plus a blit is the Metal contract.
/// The recorded upload op retains staging until it executes, so this need not
/// wait.
[[nodiscard]] inline bool blit_cpu_to_texture(void* command_queue, void* texture_handle,
                                              unsigned mip, unsigned slice,
                                              const void* data, usize row_pitch,
                                              unsigned width, unsigned height,
                                              unsigned depth, unsigned dest_x,
                                              unsigned dest_y,
                                              usize slice_pitch, bool compressed,
                                              unsigned dest_z = 0) {
    if (command_queue == nullptr || texture_handle == nullptr ||
        data == nullptr || row_pitch == 0 || width == 0 || height == 0 ||
        depth == 0) {
        return false;
    }
    const usize rows = compressed ? static_cast<usize>((height + 3u) / 4u)
                                  : static_cast<usize>(height);
    if (rows == 0) return false;
    const usize transfer_pitch = (row_pitch + 255u) & ~usize{255u};
    if (transfer_pitch == 0 || transfer_pitch > SIZE_MAX / rows) return false;
    const usize transfer_slice = transfer_pitch * rows;
    if (transfer_slice > SIZE_MAX / depth) return false;
    const usize staging_len = transfer_slice * depth;
    const usize src_slice =
        slice_pitch != 0 ? slice_pitch : row_pitch * rows;

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_handle;
    if (queue == nil || texture == nil || queue.device == nil) return false;

    @autoreleasepool {
        id<MTLBuffer> staging =
            [queue.device newBufferWithLength:staging_len
                                      options:MTLResourceStorageModeShared];
        if (staging == nil) return false;
        auto* dest = static_cast<u8*>([staging contents]);
        const auto* src = static_cast<const u8*>(data);
        for (unsigned z = 0; z < depth; ++z) {
            for (usize y = 0; y < rows; ++y) {
                std::memcpy(dest + static_cast<usize>(z) * transfer_slice +
                                y * transfer_pitch,
                            src + static_cast<usize>(z) * src_slice +
                                y * row_pitch,
                            row_pitch);
            }
        }
        // Learn, ID3D11DeviceContext::UpdateSubresource: the update is
        // ordered in the command stream, so a draw recorded BEFORE it must
        // still sample the old texels. Committing a blit here ran it ahead
        // of every draw the submit queue had recorded but not yet committed
        // (measured with tools/oracle_probe/d3d11_upload_order_probe.c: the
        // first draw showed the new upload). Record the blit on the queue
        // instead; the op retains the staging buffer until it has executed.
        MetalSubmitQueue::instance().record_upload_texture(
            (__bridge void*)staging, (__bridge void*)texture,
            transfer_pitch, transfer_slice, width, height, depth, mip,
            slice, dest_x, dest_y, dest_z);
        return true;
    }
    return true;
}

/// The unsigned-integer format whose texel is `texel_bytes` long: a view in
/// it stores any texel's bits verbatim.
[[nodiscard]] inline PixelFormat uint_view_format(unsigned texel_bytes) {
    switch (texel_bytes) {
    case 1: return PixelFormat::R8Uint;
    case 2: return PixelFormat::R16Uint;
    case 4: return PixelFormat::R32Uint;
    case 8: return PixelFormat::RG32Uint;
    case 16: return PixelFormat::RGBA32Uint;
    default: return PixelFormat::Unknown;
    }
}

/// Inline constants of the fill kernels (kFillTexelsKernel).
struct FillTexelsArgs {
    std::array<u32, 4> texel{};
    std::array<u32, 4> origin{};
    std::array<u32, 4> extent{};
};

/// Record one fill dispatch into `view_handle`, an unsigned-integer view
/// (texture2d_array, or texture3d when `volume`) of the level being filled.
/// The box is in that level's texels; z is the array slice or the W plane.
[[nodiscard]] inline bool record_fill_dispatch(bool volume, void* view_handle,
                                               const void* texel,
                                               unsigned texel_bytes, unsigned x,
                                               unsigned y, unsigned z,
                                               unsigned width, unsigned height,
                                               unsigned depth) {
    void* pipeline = MetalDevice::instance().fill_texels_pipeline(volume);
    if (pipeline == nullptr || view_handle == nullptr || texel_bytes > 16) {
        return false;
    }
    FillTexelsArgs args{};
    std::memcpy(args.texel.data(), texel, texel_bytes);
    args.origin = {x, y, z, 0};
    args.extent = {width, height, depth, 0};
    CsCompute compute{};
    compute.pipeline = pipeline;
    compute.textures[0] = view_handle;
    compute.constants.resize(sizeof(args));
    std::memcpy(compute.constants.data(), &args, sizeof(args));
    compute.tx = 8;
    compute.ty = 8;
    compute.tz = 1;
    compute.gx = (width + 7u) / 8u;
    compute.gy = (height + 7u) / 8u;
    compute.gz = depth;
    MetalSubmitQueue::instance().record_compute(std::move(compute));
    return true;
}

/// Fill by blitting a buffer of repeated texels into each slice (or W plane)
/// of the box: for textures a kernel cannot write.
[[nodiscard]] inline bool record_fill_blit(void* texture_handle, const void* texel,
                                           unsigned texel_bytes, unsigned mip,
                                           unsigned first_slice, unsigned slices,
                                           unsigned first_z, unsigned x, unsigned y,
                                           unsigned width, unsigned height,
                                           unsigned depth = 1, u32 blit_options = 0) {
    if (texture_handle == nullptr || texel == nullptr || texel_bytes == 0) {
        return false;
    }
    const usize row = static_cast<usize>(width) * texel_bytes;
    const usize bytes = row * height;
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_handle;
        id<MTLBuffer> pattern =
            [texture.device newBufferWithLength:bytes
                                        options:MTLResourceStorageModeShared];
        if (pattern == nil) return false;
        auto* out = static_cast<u8*>([pattern contents]);
        for (usize at = 0; at < bytes; at += texel_bytes) {
            std::memcpy(out + at, texel, texel_bytes);
        }
        for (unsigned slice = first_slice; slice < first_slice + slices;
             ++slice) {
            for (unsigned z = first_z; z < first_z + depth; ++z) {
                MetalSubmitQueue::instance().record_upload_texture(
                    (__bridge void*)pattern, texture_handle, row, bytes,
                    width, height, 1, mip, slice, x, y, z, blit_options);
            }
        }
    }
    return true;
}

/// Bytes a texture occupies: every mip of every layer and sample. Block
/// formats count 4x4 blocks. Initial data adds the transient upload staging
/// copy, which is host memory too until the GPU retires the blit.
inline u64 estimated_texture_bytes(PixelFormat format, unsigned width,
                                   unsigned height, unsigned depth, unsigned mips,
                                   unsigned layers, unsigned samples,
                                   bool initial_data) {
    const FormatInfo info = format_table(format);
    const u32 block = info.is_compressed ? compressed_block_bytes(format) : 0;
    const u64 pixel = info.is_compressed ? 0 : std::max<u8>(1, info.bytes_per_pixel);
    u64 total = 0;
    for (unsigned mip = 0; mip < std::max(1u, mips); ++mip) {
        const u64 w = std::max(1u, width >> mip);
        const u64 h = std::max(1u, height >> mip);
        const u64 d = std::max(1u, depth >> mip);
        total += info.is_compressed
            ? ((w + 3) / 4) * ((h + 3) / 4) * d * std::max<u32>(block, 8)
            : w * h * d * pixel;
    }
    total *= std::max(1u, layers) * std::max(1u, samples);
    return initial_data ? total * 2 : total;
}

} // namespace vitreo::graphics
