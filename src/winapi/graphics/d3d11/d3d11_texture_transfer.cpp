// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_texture_transfer.cpp
/// @brief CPU-side transfers between the guest layout and host textures: packed
///        depth+stencil planes, staging readbacks and the UpdateSubresource box
///        path. Split out of d3d11.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// The host keeps a packed depth+stencil format as Metal's
// Depth32Float_Stencil8: a float depth plane and a byte stencil plane. The
// guest-visible staging layout packs them, measured on Windows
// (d3d11_packed_depth_readback_probe): D24_UNORM_S8_UINT is one little-endian
// word, stencil in the top byte over 24-bit unorm depth (0.25, 0x5A ->
// 0x5A400000); D32_FLOAT_S8X24_UINT is the float, the stencil byte, and three
// zero bytes.
bool D3D11Manager::write_packed_depth_stencil(Texture2D& destination,
                                              u32 mip, u32 slice,
                                              const u8* in, usize in_pitch) {
    if (destination.backend == nullptr || in == nullptr) return false;
    const u32 width = std::max(1u, destination.desc.width >> mip);
    const u32 height = std::max(1u, destination.desc.height >> mip);
    const u32 texel_bytes =
        staging_bytes_per_pixel(destination.desc.format, 0);
    if (texel_bytes != 4 && texel_bytes != 8) return false;
    if (in_pitch < static_cast<usize>(width) * texel_bytes) return false;
    std::vector<float> depth(static_cast<usize>(width) * height);
    std::vector<u8> stencil(static_cast<usize>(width) * height);
    for (u32 y = 0; y < height; ++y) {
        const u8* row = in + static_cast<usize>(y) * in_pitch;
        for (u32 x = 0; x < width; ++x) {
            const usize i = static_cast<usize>(y) * width + x;
            const u8* texel = row + static_cast<usize>(x) * texel_bytes;
            if (texel_bytes == 8) {
                std::memcpy(&depth[i], texel, sizeof(float));
                stencil[i] = texel[4];
                continue;
            }
            // UNORM24 -> FLOAT (D3D11.3 3.2.3.5): n / (2^24 - 1). The host
            // plane is 32-bit float, which holds every 24-bit code apart.
            u32 word = 0;
            std::memcpy(&word, texel, sizeof(word));
            depth[i] = static_cast<float>(
                static_cast<double>(word & 0x00FFFFFFu) / 16777215.0);
            stencil[i] = static_cast<u8>(word >> 24);
        }
    }
    return destination.backend->update_depth_stencil_plane(
               mip, slice, false, depth.data(), width * sizeof(float)) &&
           destination.backend->update_depth_stencil_plane(
               mip, slice, true, stencil.data(), width);
}

bool D3D11Manager::read_packed_depth_stencil(const Texture2D& source,
                                             u32 mip, u32 slice, u8* out,
                                             usize out_pitch) {
    if (source.backend == nullptr) return false;
    const u32 width = std::max(1u, source.desc.width >> mip);
    const u32 height = std::max(1u, source.desc.height >> mip);
    const u32 texel_bytes = staging_bytes_per_pixel(source.desc.format, 0);
    if (texel_bytes != 4 && texel_bytes != 8) return false;
    if (out_pitch < static_cast<usize>(width) * texel_bytes) return false;
    std::vector<float> depth(static_cast<usize>(width) * height);
    std::vector<u8> stencil(static_cast<usize>(width) * height);
    if (!source.backend->readback_depth_stencil_plane(
            mip, slice, false, depth.data(), width * sizeof(float)) ||
        !source.backend->readback_depth_stencil_plane(
            mip, slice, true, stencil.data(), width)) {
        return false;
    }
    for (u32 y = 0; y < height; ++y) {
        u8* row = out + static_cast<usize>(y) * out_pitch;
        for (u32 x = 0; x < width; ++x) {
            const usize i = static_cast<usize>(y) * width + x;
            u8* texel = row + static_cast<usize>(x) * texel_bytes;
            if (texel_bytes == 8) {
                std::memcpy(texel, &depth[i], sizeof(float));
                texel[4] = stencil[i];
                texel[5] = texel[6] = texel[7] = 0;
                continue;
            }
            // FLOAT -> UNORM: clamp, NaN to 0, scale by 2^24 - 1, round to
            // nearest (the host stores 32-bit float, the guest reads 24 bits).
            const float d = std::isnan(depth[i])
                ? 0.0f : std::clamp(depth[i], 0.0f, 1.0f);
            const u32 unorm = static_cast<u32>(
                std::lrint(static_cast<double>(d) * 16777215.0));
            const u32 word = unorm | (static_cast<u32>(stencil[i]) << 24);
            std::memcpy(texel, &word, sizeof(word));
        }
    }
    return true;
}

bool D3D11Manager::defer_staging_readback(
    Texture2D& destination, u32 dst_sub, Texture2D& source, u32 src_mip,
    u32 src_slice, u32 src_ex, u32 src_ey, u32 dst_ex, u32 dst_ey,
    u32 elements_w, u32 elements_h) {
    // Learn (CopyResource): "an asynchronous call, which may be added to the
    // command-buffer queue", so Map(DO_NOT_WAIT) on the staging copy can
    // answer WAS_STILL_DRAWING (a06 gpu probe: DNW texture). The copy is
    // recorded now and folded into the shadow when the CPU next touches it.
    if (source.backend == nullptr || source.volume ||
        is_packed_depth_stencil(source.desc.format) ||
        dst_sub >= destination.subresources.size()) {
        return false;
    }
    auto readback = source.backend->readback_slice_async(src_mip, src_slice);
    if (readback == nullptr) return false;
    const auto format =
        graphics::format_table(source.backend->desc().pixel_format);
    const u32 w = std::max(1u, source.desc.width >> src_mip);
    const u32 h = std::max(1u, source.desc.height >> src_mip);
    Texture2D::SubresourceStorage::PendingReadback pending{};
    pending.readback = std::move(readback);
    // The guest layout's element: a block, or a pixel (half a host texel of
    // a packed 4:2:2 format, staging_bytes_per_pixel).
    pending.elem = format.is_compressed
        ? graphics::compressed_block_bytes(source.backend->desc().pixel_format)
        : staging_bytes_per_pixel(source.desc.format, format.bytes_per_pixel);
    pending.level_row_bytes = static_cast<usize>(
        format.is_compressed ? (w + 3u) / 4u : w) * pending.elem;
    pending.level_rows = format.is_compressed
        ? (h + 3u) / 4u
        : dxgi_subresource_rows(source.desc.format, h);
    pending.src_ex = src_ex;
    pending.src_ey = src_ey;
    pending.dst_ex = dst_ex;
    pending.dst_ey = dst_ey;
    pending.elements_w = elements_w;
    pending.elements_h = elements_h;
    destination.subresources[dst_sub].pending.push_back(std::move(pending));
    return true;
}

bool D3D11Manager::staging_readbacks_ready(Texture2D& texture, u32 sub) {
    if (sub >= texture.subresources.size()) return true;
    for (auto& pending : texture.subresources[sub].pending) {
        if (!pending.readback->ready()) return false;
    }
    return true;
}

void D3D11Manager::resolve_staging_readbacks(Texture2D& texture, u32 sub) {
    for (u32 index = 0; index < texture.subresources.size(); ++index) {
        if (sub != ~0u && index != sub) continue;
        auto& storage = texture.subresources[index];
        std::vector<u8> level;
        for (auto& pending : storage.pending) {
            level.resize(pending.level_row_bytes * pending.level_rows);
            if ((!pending.readback->ready() && !pending.readback->wait()) ||
                !pending.readback->read(
                    level.data(), static_cast<u32>(pending.level_row_bytes))) {
                d3d11_diag("staging readback lost; the shadow keeps its "
                           "previous bytes");
                continue;
            }
            const usize row = static_cast<usize>(pending.elements_w) *
                              pending.elem;
            for (u32 r = 0; r < pending.elements_h; ++r) {
                const usize at = storage.offset +
                    (pending.dst_ey + r) * static_cast<usize>(storage.row_pitch) +
                    static_cast<usize>(pending.dst_ex) * pending.elem;
                if (at + row > texture.shadow.size()) break;
                std::memcpy(texture.shadow.data() + at,
                            level.data() +
                                (pending.src_ey + r) * pending.level_row_bytes +
                                static_cast<usize>(pending.src_ex) *
                                    pending.elem,
                            row);
            }
        }
        storage.pending.clear();
    }
}

bool D3D11Manager::update_texture_box(Texture2D& texture, u32 subresource,
                                      GuestAddr box_address, GuestAddr data,
                                      u32 row_pitch, u32 depth_pitch) {
    // UpdateSubresource into a Texture3D, or a block-compressed texture. Learn
    // (UpdateSubresource, pDstBox and the SrcRowPitch/SrcDepthPitch diagram):
    // only the box is written, its rows SrcRowPitch and its W slices
    // SrcDepthPitch apart in pSrcData, which holds nothing past the box. A
    // compressed box is in texels, block aligned or reaching the level's
    // edge, and its rows are rows of 4x4 blocks (D3D11.3 19.5.1; a 2x2 mip
    // updates as (0,0)-(4,4)). A box outside the level is undefined --
    // WARP removes the device (a06 probe U14) -- so it is dropped.
    const bool volume = texture.volume;
    const u32 dxgi_format = volume ? texture.volume_desc.format
                                   : texture.desc.format;
    const u32 mips = volume ? texture.volume_desc.mip_levels
                            : texture.desc.mip_levels;
    const u32 count = volume ? mips : mips * texture.desc.array_size;
    if (mips == 0 || subresource >= count) return false;
    const u32 mip = subresource % mips;
    const u32 slice = subresource / mips;
    const u32 w = std::max(1u, (volume ? texture.volume_desc.width
                                       : texture.desc.width) >> mip);
    const u32 h = std::max(1u, (volume ? texture.volume_desc.height
                                       : texture.desc.height) >> mip);
    const u32 d = volume ? std::max(1u, texture.volume_desc.depth >> mip) : 1u;
    std::array<u32, 6> box{0, 0, 0, w, h, d};
    if (box_address != 0) {
        for (usize i = 0; i < box.size(); ++i) {
            const auto field = m_memory.read_u32(box_address + i * sizeof(u32));
            if (field.is_err()) return false;
            box[i] = field.value();
        }
    }
    const u32 left = box[0], top = box[1], front = box[2];
    const u32 right = box[3], bottom = box[4], back = box[5];
    if (left >= right || top >= bottom || front >= back) return false;
    const auto format = graphics::format_table(backend_format(dxgi_format));
    const u32 block = format.is_compressed
        ? graphics::compressed_block_bytes(backend_format(dxgi_format)) : 0u;
    const u32 elem = format.is_compressed ? block : format.bytes_per_pixel;
    if (elem == 0) return false;
    const auto physical = [&](u32 extent) {
        return format.is_compressed ? (extent + 3u) & ~3u : extent;
    };
    if (right > physical(w) || bottom > physical(h) || back > d ||
        (format.is_compressed &&
         ((left % 4u) != 0 || (top % 4u) != 0 ||
          ((right % 4u) != 0 && right != w) ||
          ((bottom % 4u) != 0 && bottom != h)))) {
        d3d11_diag("UpdateSubresource box (%u,%u,%u)-(%u,%u,%u) outside or "
                   "misaligned for level %ux%ux%u -> dropped",
                   left, top, front, right, bottom, back, w, h, d);
        return false;
    }
    const u32 elements_w = format.is_compressed ? (right - left + 3u) / 4u
                                                : right - left;
    const u32 rows = format.is_compressed ? (bottom - top + 3u) / 4u
                                          : bottom - top;
    const u32 depth = back - front;
    const usize row_bytes = static_cast<usize>(elements_w) * elem;
    const usize pitch = row_pitch != 0 ? row_pitch : row_bytes;
    const usize plane = depth_pitch != 0 ? depth_pitch : pitch * rows;
    if (pitch < row_bytes || (depth > 1 && plane < pitch * (rows - 1) + row_bytes)) {
        return false;
    }
    std::vector<u8> packed(row_bytes * rows * depth);
    for (u32 z = 0; z < depth; ++z) {
        for (u32 r = 0; r < rows; ++r) {
            if (m_memory.read_bytes(data + z * plane + r * pitch,
                                    packed.data() +
                                        (static_cast<usize>(z) * rows + r) *
                                            row_bytes,
                                    row_bytes).is_err()) {
                return false;
            }
        }
    }
    // The texels that exist; a block box may run into the padding.
    const u32 texels_w = std::min(right, w) - left;
    const u32 texels_h = std::min(bottom, h) - top;
    const bool ok =
        volume ? texture.backend_3d != nullptr &&
                     texture.backend_3d->update_region(
                         mip, packed.data(), static_cast<u32>(row_bytes),
                         static_cast<u32>(row_bytes * rows), left, top, front,
                         texels_w, texels_h, depth)
               : texture.backend != nullptr &&
                     texture.backend->update_region(
                         mip, slice, packed.data(),
                         static_cast<u32>(row_bytes), left, top, texels_w,
                         texels_h);
    d3d11_diag("UpdateSubresource %s box (%u,%u,%u)-(%u,%u,%u) sub=%u -> %s",
               volume ? "volume" : "compressed", left, top, front, right,
               bottom, back, subresource, ok ? "ok" : "failed");
    return ok;
}

} // namespace vitreo::winapi
