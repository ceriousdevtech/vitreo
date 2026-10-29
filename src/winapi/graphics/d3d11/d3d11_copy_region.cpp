// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_copy_region.cpp
/// @brief CopySubresourceRegion: planar and general texture region copies.
///        Split out of d3d11.cpp as verbatim code motion.
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

const char* D3D11Manager::copy_planar_region(Texture2D& destination,
                                             u32 dst_sub, u32 dst_x,
                                             u32 dst_y, Texture2D& source,
                                             u32 src_sub, u32 left, u32 top,
                                             u32 width, u32 height) {
    // Planar video (NV12 / P010 / P016): a box in luma texels copies the
    // luma texels and the chroma samples it covers. Measured
    // (d3d11_nv12_probe [layout] "copy@"): a box or destination with an odd
    // edge copies nothing. In the guest layout (staging memory) the chroma
    // rows sit below the luma rows at the same pitch, and a chroma sample
    // pair spans the byte columns of the two luma texels it covers, so the
    // chroma part of a box is rows [H + top/2, H + bottom/2) of the same
    // byte columns.
    const auto& sd = source.desc;
    const auto& dd = destination.desc;
    if (!dxgi_format_is_planar(sd.format) ||
        !dxgi_format_is_planar(dd.format) ||
        ((left | top | width | height | dst_x | dst_y) & 1u) != 0) {
        return "planar box not 2x2 aligned";
    }
    const u32 src_mip = src_sub % sd.mip_levels;
    const u32 src_slice = src_sub / sd.mip_levels;
    const u32 dst_mip = dst_sub % dd.mip_levels;
    const u32 dst_slice = dst_sub / dd.mip_levels;
    const u32 src_h = std::max(1u, sd.height >> src_mip);
    const u32 dst_h = std::max(1u, dd.height >> dst_mip);
    const bool dst_staging = dd.usage == D3D11_USAGE_STAGING;
    const bool src_staging = sd.usage == D3D11_USAGE_STAGING;
    const auto pixel = backend_format(sd.format);
    const u32 elem = graphics::format_table(pixel).bytes_per_pixel;
    const usize row_bytes = static_cast<usize>(width) * elem;
    if (!dst_staging && !src_staging) {
        // GPU to GPU: the backend copies both planes.
        if (destination.backend == nullptr || source.backend == nullptr) {
            return "planar texture without storage";
        }
        return m_backend.copy_texture_2d_region(
                   destination.backend.get(), dst_x, dst_y,
                   source.backend.get(), left, top, width, height, dst_mip,
                   dst_slice, src_mip, src_slice)
                   ? nullptr
                   : "backend planar copy failed";
    }
    // The two row ranges of the box: {source row, destination row, rows}.
    struct Rows {
        u32 src_row, dst_row, count, src_x, dst_x;
    };
    const Rows ranges[2] = {
        {top, dst_y, height, left, dst_x},
        {src_h + top / 2u, dst_h + dst_y / 2u, height / 2u, left, dst_x},
    };
    if (dst_staging && !src_staging) {
        if (destination.shared_memory) {
            graphics::BackendTextureLocation gpu{};
            gpu.texture_2d = source.backend.get();
            gpu.mip = src_mip;
            gpu.slice = src_slice;
            gpu.x = left;
            gpu.y = top;
            if (!copy_shared_staging(destination, dst_sub, true, gpu, dst_x,
                                     dst_y, 0, width, height, 1)) {
                return "shared staging copy";
            }
            gpu.texture_2d = source.backend->view_plane(
                graphics::planar_chroma_format(pixel));
            gpu.x = left / 2u;
            gpu.y = top / 2u;
            return copy_shared_staging(destination, dst_sub, true, gpu,
                                       dst_x, dst_h + dst_y / 2u, 0,
                                       width / 2u, height / 2u, 1)
                       ? nullptr
                       : "shared staging copy";
        }
        for (const Rows& r : ranges) {
            if (!defer_staging_readback(destination, dst_sub, source, src_mip,
                                        src_slice, r.src_x, r.src_row,
                                        r.dst_x, r.dst_row, width, r.count)) {
                return "planar staging readback";
            }
        }
        return nullptr;
    }
    if (src_staging && !dst_staging && source.shared_memory) {
        graphics::BackendTextureLocation gpu{};
        gpu.texture_2d = destination.backend.get();
        gpu.mip = dst_mip;
        gpu.slice = dst_slice;
        gpu.x = dst_x;
        gpu.y = dst_y;
        if (!copy_shared_staging(source, src_sub, false, gpu, left, top, 0,
                                 width, height, 1)) {
            return "shared staging copy";
        }
        gpu.texture_2d = destination.backend->view_plane(
            graphics::planar_chroma_format(pixel));
        gpu.x = dst_x / 2u;
        gpu.y = dst_y / 2u;
        return copy_shared_staging(source, src_sub, false, gpu, left,
                                   src_h + top / 2u, 0, width / 2u,
                                   height / 2u, 1)
                   ? nullptr
                   : "shared staging copy";
    }
    // A CPU side: the source rows of both ranges, from the staging memory.
    acquire_shared_staging(source);
    resolve_staging_readbacks(source, src_sub);
    const auto& in = source.subresources[src_sub];
    if (in.offset > source.shadow.size() ||
        in.size > source.shadow.size() - in.offset) {
        return "staging source layout";
    }
    std::vector<u8> box(row_bytes * (height + height / 2u));
    usize at = 0;
    for (const Rows& r : ranges) {
        for (u32 row = 0; row < r.count; ++row, at += row_bytes) {
            std::memcpy(box.data() + at,
                        source.shadow.data() + in.offset +
                            static_cast<usize>(r.src_row + row) *
                                in.row_pitch +
                            static_cast<usize>(r.src_x) * elem,
                        row_bytes);
        }
    }
    if (!dst_staging) {
        // Into GPU storage, recorded in stream order: the box's luma rows
        // then its chroma rows (the backend's planar update layout).
        return destination.backend != nullptr &&
                       destination.backend->update_region(
                           dst_mip, dst_slice, box.data(),
                           static_cast<u32>(row_bytes), dst_x, dst_y, width,
                           height)
                   ? nullptr
                   : "backend planar upload failed";
    }
    acquire_shared_staging(destination);
    resolve_staging_readbacks(destination, dst_sub);
    const auto& out = destination.subresources[dst_sub];
    if (out.offset > destination.shadow.size() ||
        out.size > destination.shadow.size() - out.offset) {
        return "staging destination layout";
    }
    at = 0;
    for (const Rows& r : ranges) {
        for (u32 row = 0; row < r.count; ++row, at += row_bytes) {
            std::memcpy(destination.shadow.data() + out.offset +
                            static_cast<usize>(r.dst_row + row) *
                                out.row_pitch +
                            static_cast<usize>(r.dst_x) * elem,
                        box.data() + at, row_bytes);
        }
    }
    if (destination.shared_memory) destination.shared_memory->note_written();
    return nullptr;
}

const char* D3D11Manager::copy_texture_region(
    Texture2D& destination, u32 dst_sub, u32 dst_x, u32 dst_y, u32 dst_z,
    Texture2D& source, u32 src_sub, const std::array<u32, 6>* box) {
    // Learn (ID3D11DeviceContext::CopySubresourceRegion): the two
    // subresources may belong to one resource, even be the same subresource
    // with overlapping boxes (D3D11.3 5.6.2.1: "must handle overlapping
    // copies"); they share a resource type and a copy-compatible format --
    // one typeless family, or a D3D10.1 reinterpretation pair; the
    // destination is not IMMUTABLE; a depth-stencil or multisampled pair
    // copies whole subresources only, with pSrcBox NULL and DstX/Y/Z 0.
    // What Windows drops, measured on Windows, is dropped here too
    // (a06 probe CPY lines).
    const bool volume = source.volume;
    if (destination.volume != volume || destination.linear != source.linear) {
        return "resource types differ";
    }
    const auto& sd = source.desc;
    const auto& dd = destination.desc;
    const u32 src_mips = volume ? source.volume_desc.mip_levels : sd.mip_levels;
    const u32 dst_mips =
        volume ? destination.volume_desc.mip_levels : dd.mip_levels;
    const u32 src_count = volume ? src_mips : src_mips * sd.array_size;
    const u32 dst_count = volume ? dst_mips : dst_mips * dd.array_size;
    if (src_mips == 0 || dst_mips == 0 || src_sub >= src_count ||
        dst_sub >= dst_count) {
        return "subresource out of range";
    }
    if (dd.usage == D3D11_USAGE_IMMUTABLE) return "IMMUTABLE destination";
    // A mapped destination is refused. A mapped source is undefined by Learn
    // ("cannot be mapped"); Windows copies its current contents (a06 probe
    // C20), which is what reading the shadow does.
    if (dd.usage == D3D11_USAGE_STAGING &&
        dst_sub < destination.subresources.size() &&
        destination.subresources[dst_sub].mapped) {
        return "mapped staging destination";
    }
    const bool same_family =
        dxgi_copy_formats_compatible(dd.format, sd.format);
    if (!same_family &&
        !dxgi_reinterpret_copy_compatible(dd.format, sd.format)) {
        return "formats not copy-compatible";
    }
    if (dd.sample_count != sd.sample_count ||
        dd.sample_quality != sd.sample_quality) {
        return "sample counts differ";
    }
    const bool whole_only =
        sd.sample_count > 1 ||
        ((sd.bind_flags | dd.bind_flags) & D3D11_BIND_DEPTH_STENCIL) != 0;
    if (whole_only && (box != nullptr || dst_x != 0 || dst_y != 0 ||
                       dst_z != 0)) {
        return "depth-stencil/multisample region is not the whole subresource";
    }

    const u32 src_mip = src_sub % src_mips;
    const u32 src_slice = src_sub / src_mips;
    const u32 dst_mip = dst_sub % dst_mips;
    const u32 dst_slice = dst_sub / dst_mips;
    const auto level = [](u32 extent, u32 mip) {
        return std::max(1u, extent >> mip);
    };
    const u32 src_w = level(volume ? source.volume_desc.width : sd.width, src_mip);
    const u32 src_h = level(volume ? source.volume_desc.height : sd.height, src_mip);
    const u32 src_d = volume ? level(source.volume_desc.depth, src_mip) : 1u;
    const u32 dst_w =
        level(volume ? destination.volume_desc.width : dd.width, dst_mip);
    const u32 dst_h =
        level(volume ? destination.volume_desc.height : dd.height, dst_mip);
    const u32 dst_d =
        volume ? level(destination.volume_desc.depth, dst_mip) : 1u;

    std::array<u32, 6> region{0, 0, 0, src_w, src_h, src_d};
    if (box != nullptr) region = *box;
    const u32 left = region[0], top = region[1], front = region[2];
    const u32 right = region[3], bottom = region[4], back = region[5];
    if (left >= right || top >= bottom || front >= back) return "empty box";

    // Copy elements: texels, or the 4x4 blocks of a compressed format. The
    // two sides' elements are the same size (identical formats, one family,
    // or a reinterpretation pair whose texel is one block).
    const auto sf = graphics::format_table(backend_format(sd.format));
    const auto df = graphics::format_table(backend_format(dd.format));
    const u32 s_elem = sf.is_compressed
        ? graphics::compressed_block_bytes(backend_format(sd.format))
        : staging_bytes_per_pixel(sd.format, sf.bytes_per_pixel);
    const u32 d_elem = df.is_compressed
        ? graphics::compressed_block_bytes(backend_format(dd.format))
        : staging_bytes_per_pixel(dd.format, df.bytes_per_pixel);
    if (s_elem == 0 || s_elem != d_elem) return "element sizes differ";
    const u32 elem = s_elem;
    // D3D11.3 19.5.1: a compressed level's storage is rounded up to whole
    // blocks, and a region must be block aligned or reach the level's edge,
    // so (0,0)-(4,4) is how a 2x2 or 1x1 BC mip is copied -- UE's texture
    // streaming copies its mip tails that way, and Windows copies them
    // (tools/oracle_probe/d3d11_bc_mip_tail_copy_probe.c).
    const auto physical = [](bool compressed, u32 extent) {
        return compressed ? (extent + 3u) & ~3u : extent;
    };
    const u32 copy_w = right - left;
    const u32 copy_h = bottom - top;
    const u32 depth = back - front;
    if (right > physical(sf.is_compressed, src_w) ||
        bottom > physical(sf.is_compressed, src_h) || back > src_d) {
        return "source box outside the level";
    }
    if (sf.is_compressed &&
        ((left % 4u) != 0 || (top % 4u) != 0 ||
         ((right % 4u) != 0 && right != src_w) ||
         ((bottom % 4u) != 0 && bottom != src_h))) {
        return "compressed source box not block aligned";
    }
    const u32 elements_w = sf.is_compressed ? (copy_w + 3u) / 4u : copy_w;
    const u32 elements_h = sf.is_compressed ? (copy_h + 3u) / 4u : copy_h;
    const u32 dst_extent_w = df.is_compressed ? elements_w * 4u : elements_w;
    const u32 dst_extent_h = df.is_compressed ? elements_h * 4u : elements_h;
    if (df.is_compressed && ((dst_x % 4u) != 0 || (dst_y % 4u) != 0)) {
        return "compressed destination not block aligned";
    }
    if (dst_x >= physical(df.is_compressed, dst_w) ||
        dst_y >= physical(df.is_compressed, dst_h) || dst_z >= dst_d ||
        dst_extent_w > physical(df.is_compressed, dst_w) - dst_x ||
        dst_extent_h > physical(df.is_compressed, dst_h) - dst_y ||
        depth > dst_d - dst_z) {
        return "destination box outside the level";
    }
    if (sf.is_compressed && df.is_compressed &&
        ((copy_w % 4u) != 0 && dst_x + copy_w != dst_w)) {
        return "partial block into the middle of a compressed level";
    }
    if (sf.is_compressed && df.is_compressed &&
        ((copy_h % 4u) != 0 && dst_y + copy_h != dst_h)) {
        return "partial block into the middle of a compressed level";
    }
    // A packed 4:2:2 region covers whole pixel pairs: an odd left or right
    // edge, or an odd DstX, copies nothing (measured,
    // d3d11_video_formats_probe [layout] "copy@1,1", "copy@0,0<-1,1-5,3",
    // "copy@0,0<-0,0-3,2"). The host textures address pairs (ppt).
    const u32 ppt = dxgi_pixels_per_texel(sd.format);
    if (ppt != 1u && ((left | right | dst_x) % ppt) != 0) {
        return "packed 4:2:2 region with an odd edge";
    }
    // Element origins and the element-row layout both sides share.
    const u32 src_ex = sf.is_compressed ? left / 4u : left;
    const u32 src_ey = sf.is_compressed ? top / 4u : top;
    const u32 dst_ex = df.is_compressed ? dst_x / 4u : dst_x;
    const u32 dst_ey = df.is_compressed ? dst_y / 4u : dst_y;
    const usize row_bytes = static_cast<usize>(elements_w) * elem;
    const usize src_level_pitch =
        static_cast<usize>(sf.is_compressed ? (src_w + 3u) / 4u : src_w) * elem;
    const u32 src_level_rows = sf.is_compressed ? (src_h + 3u) / 4u : src_h;

    const bool dst_staging = dd.usage == D3D11_USAGE_STAGING;
    const bool src_staging = sd.usage == D3D11_USAGE_STAGING;
    if (dxgi_format_is_planar(sd.format) || dxgi_format_is_planar(dd.format)) {
        return copy_planar_region(destination, dst_sub, dst_x, dst_y, source,
                                  src_sub, left, top, copy_w, copy_h);
    }
    // Shared STAGING memory (every opening's storage): a GPU side copies
    // into or out of it with a GPU blit against the memory itself, so the
    // copy reaches every opening (d3d11_sharing.cpp).
    if (dst_staging != src_staging &&
        (dst_staging ? destination.shared_memory : source.shared_memory)) {
        const u32 texels_w = std::min(copy_w, src_w - left);
        const u32 texels_h = std::min(copy_h, src_h - top);
        graphics::BackendTextureLocation gpu{};
        Texture2D& device_side = dst_staging ? source : destination;
        if (volume) {
            gpu.texture_3d = device_side.backend_3d.get();
        } else {
            gpu.texture_2d = device_side.backend.get();
        }
        if (dst_staging) {
            gpu.mip = src_mip;
            gpu.slice = src_slice;
            gpu.x = left / ppt;
            gpu.y = top;
            gpu.z = front;
            return copy_shared_staging(destination, dst_sub, true, gpu,
                                       dst_x, dst_y, dst_z, texels_w / ppt,
                                       texels_h, depth)
                       ? nullptr
                       : "shared staging copy";
        }
        gpu.mip = dst_mip;
        gpu.slice = dst_slice;
        gpu.x = dst_x / ppt;
        gpu.y = dst_y;
        gpu.z = dst_z;
        return copy_shared_staging(source, src_sub, false, gpu, left, top,
                                   front, texels_w / ppt, texels_h, depth)
                   ? nullptr
                   : "shared staging copy";
    }
    // A CPU copy between staging resources reads and writes shared memory
    // after the GPU work and publications pending on it.
    if (src_staging) acquire_shared_staging(source);
    if (dst_staging) acquire_shared_staging(destination);
    // A CPU read of a staging shadow folds in the copies still queued into it.
    if (src_staging) resolve_staging_readbacks(source, src_sub);
    if (dst_staging && !src_staging && !volume &&
        defer_staging_readback(destination, dst_sub, source, src_mip,
                               src_slice, src_ex, src_ey, dst_ex, dst_ey,
                               elements_w, elements_h)) {
        return nullptr;
    }
    if (dst_staging) {
        // To the CPU shadow, after the copies already queued into it. The
        // source's rows come from its own shadow, or from the GPU one W
        // slice at a time.
        resolve_staging_readbacks(destination, dst_sub);
        const auto& out = destination.subresources[dst_sub];
        if (out.offset > destination.shadow.size() ||
            out.size > destination.shadow.size() - out.offset ||
            out.row_pitch < static_cast<usize>(dst_ex) * elem + row_bytes) {
            return "staging destination layout";
        }
        std::vector<u8> plane;
        for (u32 w = 0; w < depth; ++w) {
            const u8* rows = nullptr;
            usize pitch = 0;
            if (src_staging) {
                const auto& in = source.subresources[src_sub];
                if (in.offset + in.size > source.shadow.size()) {
                    return "staging source layout";
                }
                rows = source.shadow.data() + in.offset +
                       static_cast<usize>(front + w) * in.slice_pitch;
                pitch = in.row_pitch;
            } else {
                plane.resize(src_level_pitch * src_level_rows);
                bool read = false;
                if (volume) {
                    read = source.backend_3d != nullptr &&
                           source.backend_3d->readback_w_slice(
                               src_mip, front + w, plane.data(),
                               static_cast<u32>(src_level_pitch));
                } else if (is_packed_depth_stencil(sd.format)) {
                    read = read_packed_depth_stencil(
                        source, src_mip, src_slice, plane.data(),
                        static_cast<u32>(src_level_pitch));
                } else {
                    read = source.backend != nullptr &&
                           source.backend->readback_slice(
                               src_mip, src_slice, plane.data(),
                               static_cast<u32>(src_level_pitch));
                }
                if (!read) return "GPU readback failed";
                rows = plane.data();
                pitch = src_level_pitch;
            }
            u8* target = destination.shadow.data() + out.offset +
                         static_cast<usize>(dst_z + w) * out.slice_pitch;
            // memmove: a staging texture may copy within itself.
            for (u32 row = 0; row < elements_h; ++row) {
                std::memmove(target + (dst_ey + row) * out.row_pitch +
                                 static_cast<usize>(dst_ex) * elem,
                             rows + (src_ey + row) * pitch +
                                 static_cast<usize>(src_ex) * elem,
                             row_bytes);
            }
        }
        if (destination.shared_memory) destination.shared_memory->note_written();
        return nullptr;
    }
    if (src_staging) {
        // From the CPU shadow into GPU storage, recorded in stream order.
        const auto& in = source.subresources[src_sub];
        if (is_packed_depth_stencil(dd.format)) {
            // Whole subresources only (checked above): split into planes.
            if (volume || in.offset + in.size > source.shadow.size()) {
                return "staging source layout";
            }
            return write_packed_depth_stencil(
                       destination, dst_mip, dst_slice,
                       source.shadow.data() + in.offset, in.row_pitch)
                       ? nullptr
                       : "backend depth/stencil plane upload failed";
        }
        if (in.offset > source.shadow.size() ||
            in.size > source.shadow.size() - in.offset) {
            return "staging source layout";
        }
        std::vector<u8> rows(row_bytes * elements_h * depth);
        for (u32 w = 0; w < depth; ++w) {
            for (u32 row = 0; row < elements_h; ++row) {
                std::memcpy(rows.data() + (w * elements_h + row) * row_bytes,
                            source.shadow.data() + in.offset +
                                static_cast<usize>(front + w) *
                                    in.slice_pitch +
                                (src_ey + row) * in.row_pitch +
                                static_cast<usize>(src_ex) * elem,
                            row_bytes);
            }
        }
        // The texels that exist: a block box may run into the padding past
        // the destination's virtual edge.
        const u32 w_texels = std::min(dst_extent_w, dst_w - dst_x);
        const u32 h_texels = std::min(dst_extent_h, dst_h - dst_y);
        const bool ok =
            volume
                ? destination.backend_3d != nullptr &&
                      destination.backend_3d->update_region(
                          dst_mip, rows.data(), static_cast<u32>(row_bytes),
                          static_cast<u32>(row_bytes * elements_h), dst_x,
                          dst_y, dst_z, w_texels, h_texels, depth)
                : destination.backend != nullptr &&
                      destination.backend->update_region(
                          dst_mip, dst_slice, rows.data(),
                          static_cast<u32>(row_bytes), dst_x / ppt, dst_y,
                          w_texels / ppt, h_texels);
        return ok ? nullptr : "backend upload failed";
    }
    // GPU to GPU, on the recorded timeline. Only texels that exist in the
    // source are read; the backend maps them to the destination's units.
    const u32 texels_w = std::min(copy_w, src_w - left);
    const u32 texels_h = std::min(copy_h, src_h - top);
    const bool plain = !volume && &destination != &source &&
                       destination.backend != nullptr &&
                       source.backend != nullptr &&
                       destination.backend->desc().pixel_format ==
                           source.backend->desc().pixel_format;
    if (plain) {
        const bool ok = m_backend.copy_texture_2d_region(
            destination.backend.get(), dst_x / ppt, dst_y,
            source.backend.get(), left / ppt, top,
            std::min(texels_w, dst_w - dst_x) / ppt,
            std::min(texels_h, dst_h - dst_y), dst_mip, dst_slice, src_mip,
            src_slice);
        return ok ? nullptr : "backend blit failed";
    }
    graphics::BackendTextureLocation to{};
    graphics::BackendTextureLocation from{};
    to.texture_2d = destination.backend.get();
    to.texture_3d = volume ? destination.backend_3d.get() : nullptr;
    from.texture_2d = source.backend.get();
    from.texture_3d = volume ? source.backend_3d.get() : nullptr;
    if (volume ? (to.texture_3d == nullptr || from.texture_3d == nullptr)
               : (to.texture_2d == nullptr || from.texture_2d == nullptr)) {
        return "no backend texture";
    }
    if (volume) {
        to.texture_2d = nullptr;
        from.texture_2d = nullptr;
    }
    to.mip = dst_mip;
    to.slice = volume ? 0u : dst_slice;
    to.x = dst_x / ppt;
    to.y = dst_y;
    to.z = dst_z;
    from.mip = src_mip;
    from.slice = volume ? 0u : src_slice;
    from.x = left / ppt;
    from.y = top;
    from.z = front;
    if (!m_backend.copy_texture_elements(to, from, texels_w / ppt, texels_h,
                                         depth)) {
        d3d11_unimplemented_once(
            "CopySubresourceRegion",
            "the backend cannot copy between these formats/subresources");
        return "backend element copy failed";
    }
    return nullptr;
}

} // namespace vitreo::winapi
