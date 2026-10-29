// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_copy_resource.cpp
/// @brief CopyResource between textures. Split out of d3d11.cpp as verbatim
///        code motion.
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

bool D3D11Manager::copy_texture_resource(Texture2D& destination,
                                         Texture2D& source) {
    const auto& dst = destination.desc;
    const auto& src = source.desc;
    const auto any_mapped = [](const Texture2D& texture) {
        return std::any_of(
            texture.subresources.begin(), texture.subresources.end(),
            [](const Texture2D::SubresourceStorage& storage) {
                return storage.mapped;
            });
    };
    // Learn (CopyResource): "identical dimensions" -- except a D3D10.1
    // reinterpretation pair, where one uncompressed texel is one 4x4 block:
    // a 4x4 R32G32_UINT texture copies to a 16x16 BC1 one (Learn, "Format
    // Conversion using Direct3D 10.1"; a06 probe CPY CopyResource lines).
    const bool reinterpret =
        !dxgi_copy_formats_compatible(dst.format, src.format) &&
        dxgi_reinterpret_copy_compatible(dst.format, src.format);
    const auto block_dimensions_match = [&] {
        const bool dst_blocks =
            graphics::format_table(backend_format(dst.format)).is_compressed;
        const auto& blocks = dst_blocks ? dst : src;
        const auto& texels = dst_blocks ? src : dst;
        for (u32 mip = 0; mip < blocks.mip_levels; ++mip) {
            const u32 bw = (std::max(1u, blocks.width >> mip) + 3u) / 4u;
            const u32 bh = (std::max(1u, blocks.height >> mip) + 3u) / 4u;
            if (bw != std::max(1u, texels.width >> mip) ||
                bh != std::max(1u, texels.height >> mip)) {
                return false;
            }
        }
        return true;
    };
    const bool compressed_side =
        graphics::format_table(backend_format(dst.format)).is_compressed !=
        graphics::format_table(backend_format(src.format)).is_compressed;
    const bool dimensions_ok =
        reinterpret && compressed_side
            ? block_dimensions_match()
            : dst.width == src.width && dst.height == src.height;
    if (&destination == &source || any_mapped(destination) ||
        any_mapped(source) ||
        dst.usage == D3D11_USAGE_IMMUTABLE ||
        destination.linear != source.linear || !dimensions_ok ||
        dst.mip_levels != src.mip_levels ||
        dst.array_size != src.array_size ||
        (!reinterpret &&
         !dxgi_copy_formats_compatible(dst.format, src.format)) ||
        dst.sample_count != src.sample_count ||
        dst.sample_quality != src.sample_quality) {
        return false;
    }

    const bool destination_staging = dst.usage == D3D11_USAGE_STAGING;
    const bool source_staging = src.usage == D3D11_USAGE_STAGING;
    if (destination.volume || source.volume) {
        // Learn CopyResource: both resources must be the same type with
        // identical dimensions, so a volume pairs only with a volume of the
        // same depth. Staging volumes live in the shadow (per-mip
        // subresources, DepthPitch-strided); GPU volumes copy on the recorded
        // timeline so the copy lands after the dispatch that filled them.
        if (!destination.volume || !source.volume ||
            destination.volume_desc.depth != source.volume_desc.depth) {
            return false;
        }
        if (destination_staging && source_staging) {
            if (destination.shadow.size() != source.shadow.size()) return false;
            acquire_shared_staging(source);
            acquire_shared_staging(destination);
            destination.shadow = source.shadow;
            if (destination.shared_memory) {
                destination.shared_memory->note_written();
            }
            return true;
        }
        if (destination_staging != source_staging &&
            (destination_staging ? destination.shared_memory
                                 : source.shared_memory)) {
            // Shared STAGING memory: GPU blits against it, level by level.
            Texture2D& staging_side = destination_staging ? destination
                                                          : source;
            Texture2D& gpu_side = destination_staging ? source : destination;
            if (gpu_side.backend_3d == nullptr ||
                staging_side.subresources.size() != src.mip_levels) {
                return false;
            }
            for (u32 mip = 0; mip < src.mip_levels; ++mip) {
                graphics::BackendTextureLocation gpu{};
                gpu.texture_3d = gpu_side.backend_3d.get();
                gpu.mip = mip;
                const auto& v = source.volume_desc;
                if (!copy_shared_staging(
                        staging_side, mip, destination_staging, gpu, 0, 0, 0,
                        std::max(1u, v.width >> mip),
                        std::max(1u, v.height >> mip),
                        std::max(1u, v.depth >> mip))) {
                    return false;
                }
            }
            return true;
        }
        if (destination_staging) {
            if (source.backend_3d == nullptr ||
                destination.subresources.size() != src.mip_levels) {
                return false;
            }
            for (u32 mip = 0; mip < src.mip_levels; ++mip) {
                const auto& storage = destination.subresources[mip];
                for (u32 w = 0; w < storage.depth; ++w) {
                    if (!source.backend_3d->readback_w_slice(
                            mip, w,
                            destination.shadow.data() + storage.offset +
                                w * storage.slice_pitch,
                            storage.row_pitch)) {
                        return false;
                    }
                }
            }
            return true;
        }
        if (source_staging) {
            if (destination.backend_3d == nullptr ||
                source.subresources.size() != src.mip_levels) {
                return false;
            }
            for (u32 mip = 0; mip < src.mip_levels; ++mip) {
                const auto& storage = source.subresources[mip];
                if (!destination.backend_3d->update(
                        mip, source.shadow.data() + storage.offset,
                        storage.row_pitch,
                        static_cast<u32>(storage.slice_pitch))) {
                    return false;
                }
            }
            return true;
        }
        if (destination.backend_3d == nullptr ||
            source.backend_3d == nullptr) {
            return false;
        }
        if (destination.backend_3d->desc().pixel_format ==
            source.backend_3d->desc().pixel_format) {
            return m_backend.copy_texture_3d(destination.backend_3d.get(),
                                             source.backend_3d.get());
        }
        // Members of one family stored in different host formats: the bytes
        // move through the backend's reinterpreting copy, level by level.
        for (u32 mip = 0; mip < src.mip_levels; ++mip) {
            graphics::BackendTextureLocation to{};
            graphics::BackendTextureLocation from{};
            to.texture_3d = destination.backend_3d.get();
            from.texture_3d = source.backend_3d.get();
            to.mip = from.mip = mip;
            const auto& v = source.volume_desc;
            if (!m_backend.copy_texture_elements(
                    to, from, std::max(1u, v.width >> mip),
                    std::max(1u, v.height >> mip),
                    std::max(1u, v.depth >> mip))) {
                return false;
            }
        }
        return true;
    }
    if (destination_staging != source_staging &&
        (destination_staging ? destination.shared_memory
                             : source.shared_memory)) {
        // Shared STAGING memory: GPU blits against it, subresource by
        // subresource (d3d11_sharing.cpp).
        Texture2D& staging_side = destination_staging ? destination : source;
        Texture2D& gpu_side = destination_staging ? source : destination;
        if (gpu_side.backend == nullptr ||
            staging_side.subresources.size() !=
                src.mip_levels * src.array_size) {
            return false;
        }
        for (u32 slice = 0; slice < src.array_size; ++slice) {
            for (u32 mip = 0; mip < src.mip_levels; ++mip) {
                graphics::BackendTextureLocation gpu{};
                gpu.texture_2d = gpu_side.backend.get();
                gpu.mip = mip;
                gpu.slice = slice;
                const u32 lw = std::max(1u, src.width >> mip);
                const u32 lh = std::max(1u, src.height >> mip);
                if (!copy_shared_staging(
                        staging_side, slice * src.mip_levels + mip,
                        destination_staging, gpu, 0, 0, 0,
                        lw / dxgi_pixels_per_texel(src.format), lh, 1)) {
                    return false;
                }
                if (dxgi_format_is_planar(src.format)) {
                    // The chroma plane, below the luma rows (x in luma
                    // columns, the same bytes).
                    gpu.texture_2d = gpu_side.backend->view_plane(
                        graphics::planar_chroma_format(
                            gpu_side.backend->desc().pixel_format));
                    if (!copy_shared_staging(
                            staging_side, slice * src.mip_levels + mip,
                            destination_staging, gpu, 0, lh, 0, lw / 2u,
                            lh / 2u, 1)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    if (source_staging) {
        acquire_shared_staging(source);
        resolve_staging_readbacks(source);
    }
    if (destination_staging && source_staging) {
        if (destination.shadow.size() != source.shadow.size()) return false;
        acquire_shared_staging(destination);
        resolve_staging_readbacks(destination);
        destination.shadow = source.shadow;
        if (destination.shared_memory) destination.shared_memory->note_written();
        return true;
    }
    if (destination_staging) {
        if (source.backend == nullptr || destination.subresources.size() !=
                                         source.desc.mip_levels *
                                             source.desc.array_size) {
            return false;
        }
        const bool packed = is_packed_depth_stencil(src.format);
        for (u32 slice = 0; slice < src.array_size; ++slice) {
            for (u32 mip = 0; mip < src.mip_levels; ++mip) {
                const usize index = static_cast<usize>(slice) *
                                        src.mip_levels + mip;
                auto& storage = destination.subresources[index];
                // The whole subresource is replaced: anything still queued
                // into it is superseded.
                const auto level_format = graphics::format_table(
                    source.backend->desc().pixel_format);
                const u32 lw = std::max(1u, src.width >> mip);
                const u32 lh = std::max(1u, src.height >> mip);
                const u32 ew = level_format.is_compressed ? (lw + 3u) / 4u : lw;
                const u32 eh = level_format.is_compressed
                    ? (lh + 3u) / 4u
                    : dxgi_subresource_rows(src.format, lh);
                storage.pending.clear();
                if (!packed &&
                    defer_staging_readback(destination,
                                           static_cast<u32>(index), source,
                                           mip, slice, 0, 0, 0, 0, ew, eh)) {
                    continue;
                }
                u8* const target = destination.shadow.data() + storage.offset;
                const bool read =
                    packed ? read_packed_depth_stencil(source, mip, slice,
                                                       target,
                                                       storage.row_pitch)
                           : source.backend->readback_slice(
                                 mip, slice, target, storage.row_pitch);
                if (!read) return false;
            }
        }
        return true;
    }
    if (source_staging) {
        if (destination.backend == nullptr || source.subresources.size() !=
                                              src.mip_levels *
                                                  src.array_size) {
            return false;
        }
        // A packed depth+stencil format is split into the host's depth and
        // stencil planes (Learn CopyResource: a depth-stencil destination is
        // legal at FL 10.1+; a06 probe C24).
        const bool packed = is_packed_depth_stencil(dst.format);
        for (u32 slice = 0; slice < src.array_size; ++slice) {
            for (u32 mip = 0; mip < src.mip_levels; ++mip) {
                const usize index = static_cast<usize>(slice) *
                                        src.mip_levels + mip;
                const auto& storage = source.subresources[index];
                if (packed) {
                    if (!write_packed_depth_stencil(
                            destination, mip, slice,
                            source.shadow.data() + storage.offset,
                            storage.row_pitch)) {
                        return false;
                    }
                    continue;
                }
                if (!destination.backend->update_slice(
                        mip, slice, source.shadow.data() + storage.offset,
                        storage.row_pitch)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (destination.backend == nullptr || source.backend == nullptr) {
        return false;
    }
    // A same-format pair blits; members of one family stored in different
    // host formats (SRGB/UNORM, FLOAT/UINT, ...) and the reinterpretation
    // pairs move their bytes through the backend's element copy.
    const bool blit = destination.backend->desc().pixel_format ==
                      source.backend->desc().pixel_format;
    for (u32 slice = 0; slice < dst.array_size; ++slice) {
        for (u32 mip = 0; mip < dst.mip_levels; ++mip) {
            if (blit) {
                // A packed 4:2:2 texture's host texels are pixel pairs.
                const u32 width = std::max(1u, dst.width >> mip) /
                                  dxgi_pixels_per_texel(dst.format);
                const u32 height = std::max(1u, dst.height >> mip);
                if (!m_backend.copy_texture_2d_region(
                        destination.backend.get(), 0, 0,
                        source.backend.get(), 0, 0, width, height, mip,
                        slice, mip, slice)) {
                    return false;
                }
                continue;
            }
            graphics::BackendTextureLocation to{};
            graphics::BackendTextureLocation from{};
            to.texture_2d = destination.backend.get();
            from.texture_2d = source.backend.get();
            to.mip = from.mip = mip;
            to.slice = from.slice = slice;
            if (!m_backend.copy_texture_elements(
                    to, from,
                    std::max(1u, src.width >> mip) /
                        dxgi_pixels_per_texel(src.format),
                    std::max(1u, src.height >> mip), 1)) {
                d3d11_unimplemented_once(
                    "CopyResource",
                    "the backend cannot copy between these formats");
                return false;
            }
        }
    }
    return true;
}

} // namespace vitreo::winapi
