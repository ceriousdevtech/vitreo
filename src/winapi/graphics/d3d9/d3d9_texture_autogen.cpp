// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_texture_autogen.cpp
/// @brief D3DUSAGE_AUTOGENMIPMAP sublevel generation for D3D9 textures,
///        cube textures and volume textures.

#include "winapi/graphics/d3d9/d3d9_resources_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_resources_detail;

namespace {

/// Nearest-neighbor downsample: take the top-left texel of each 2×2 (×2) block.
void downsample_point_2d(const u8* src, u32 src_w, u32 src_h, u32 src_pitch,
                         u8* dst, u32 dst_w, u32 dst_h, u32 dst_pitch,
                         u32 bpp) {
    for (u32 y = 0; y < dst_h; ++y) {
        const u32 sy = std::min(y * 2u, src_h - 1u);
        const u8* src_row = src + static_cast<usize>(sy) * src_pitch;
        u8* dst_row = dst + static_cast<usize>(y) * dst_pitch;
        for (u32 x = 0; x < dst_w; ++x) {
            const u32 sx = std::min(x * 2u, src_w - 1u);
            std::memcpy(dst_row + static_cast<usize>(x) * bpp,
                        src_row + static_cast<usize>(sx) * bpp, bpp);
        }
    }
}

void downsample_point_3d(const u8* src, u32 src_w, u32 src_h, u32 src_d,
                         u32 src_row_pitch, u32 src_slice_pitch, u8* dst,
                         u32 dst_w, u32 dst_h, u32 dst_d, u32 dst_row_pitch,
                         u32 dst_slice_pitch, u32 bpp) {
    for (u32 z = 0; z < dst_d; ++z) {
        const u32 sz = std::min(z * 2u, src_d - 1u);
        const u8* src_slice =
            src + static_cast<usize>(sz) * src_slice_pitch;
        u8* dst_slice = dst + static_cast<usize>(z) * dst_slice_pitch;
        downsample_point_2d(src_slice, src_w, src_h, src_row_pitch, dst_slice,
                            dst_w, dst_h, dst_row_pitch, bpp);
    }
}

[[nodiscard]] bool upload_autogen_level_2d(
    graphics::BackendTexture2D* backend, unsigned level, unsigned face,
    u32 format, u32 width, u32 height, const u8* pixels, u32 pitch,
    bool cube) {
    if (backend == nullptr || pixels == nullptr) return false;
    std::vector<u8> expanded;
    const u8* upload = pixels;
    u32 upload_pitch = pitch;
    const bool expanded_bump = d3d9_expand_special_bump_to_rgba32f(
        format, width, height, pixels, pitch, expanded);
    if (expanded_bump ||
        d3d9_expand_lock_to_bgra8(format, width, height, pixels, pitch,
                                  expanded)) {
        upload = expanded.data();
        upload_pitch = width * (expanded_bump ? 16u : 4u);
    }
    if (cube) {
        return backend->update_slice(level, face, upload, upload_pitch);
    }
    return backend->update(level, upload, upload_pitch);
}

} // namespace

void D3D9Manager::generate_autogen_mips(Marshaler& m, TextureState& texture,
                                        u32 device, u32 container) {
    if (texture.levels <= 1) return;

    if (texture.auto_gen_filter == D3DTEXF_LINEAR) {
        if (texture.volume) {
            if (texture.backend_3d && m_backend != nullptr &&
                m_backend->available()) {
                if (!m_backend->generate_mips(texture.backend_3d.get())) {
                    VITREO_TRACE("[d3d9] AUTOGEN LINEAR generate_mips (3D) "
                                 "failed\n");
                }
            }
            return;
        }
        if (texture.backend && m_backend != nullptr && m_backend->available()) {
            if (!m_backend->generate_mips(texture.backend.get())) {
                VITREO_TRACE("[d3d9] AUTOGEN LINEAR generate_mips failed\n");
            }
        }
        return;
    }

    if (texture.auto_gen_filter != D3DTEXF_POINT) {
        return;
    }

    // POINT: CPU nearest-neighbor. Metal blit has no POINT/LINEAR knob, so
    // do not silently fall back to the LINEAR GPU path.
    const u32 bpp = d3d9_format_bytes_per_pixel(texture.format);
    if (bpp == 0 || d3d9_is_palettized_format(texture.format)) {
        VITREO_TRACE("[d3d9] AUTOGEN POINT mips unsupported for format %u "
                     "(skipping GPU linear blit)\n",
                     texture.format);
        return;
    }

    const auto surface_for = [&](u32 face, u32 level) -> SurfaceState* {
        if (level >= texture.levels || device == 0 || container == 0) {
            return nullptr;
        }
        const usize index = texture.cube
                                ? static_cast<usize>(face) * texture.levels +
                                      level
                                : static_cast<usize>(level);
        if (index >= texture.level_surfaces.size()) return nullptr;
        if (texture.level_surfaces[index] == 0) {
            u32 level_width = texture.width >> level;
            u32 level_height = texture.height >> level;
            if (level_width == 0) level_width = 1;
            if (level_height == 0) level_height = 1;
            const u32 surface = create_surface_object(
                device, level_width, level_height, texture.format,
                texture.usage, texture.pool,
                static_cast<int>(SurfaceState::Kind::TextureLevel), container,
                level, face);
            if (surface == 0) return nullptr;
            texture.level_surfaces[index] = surface;
        }
        ComObject* surface_entry =
            find(texture.level_surfaces[index], D3D9InterfaceId::Surface);
        if (surface_entry == nullptr || !surface_entry->surface) {
            return nullptr;
        }
        return surface_entry->surface.get();
    };

    if (texture.volume) {
        const auto volume_for = [&](u32 level) -> VolumeState* {
            if (level >= texture.levels ||
                level >= texture.level_surfaces.size() || device == 0 ||
                container == 0) {
                return nullptr;
            }
            if (texture.level_surfaces[level] == 0) {
                u32 level_width = texture.width >> level;
                u32 level_height = texture.height >> level;
                u32 level_depth = texture.depth >> level;
                if (level_width == 0) level_width = 1;
                if (level_height == 0) level_height = 1;
                if (level_depth == 0) level_depth = 1;
                const u32 volume = create_volume_object(
                    device, level_width, level_height, level_depth,
                    texture.format, texture.usage, texture.pool, container,
                    level);
                if (volume == 0) return nullptr;
                texture.level_surfaces[level] = volume;
            }
            ComObject* volume_entry =
                find(texture.level_surfaces[level], D3D9InterfaceId::Volume);
            if (volume_entry == nullptr || !volume_entry->volume) {
                return nullptr;
            }
            return volume_entry->volume.get();
        };

        VolumeState* level0 = volume_for(0);
        if (level0 == nullptr || level0->pixels == 0) {
            VITREO_TRACE("[d3d9] AUTOGEN POINT volume: missing level 0\n");
            return;
        }
        std::vector<u8> src(level0->pixel_bytes);
        if (m.memory()
                .read_bytes(level0->pixels, src.data(), src.size())
                .is_err()) {
            return;
        }
        u32 src_w = level0->width;
        u32 src_h = level0->height;
        u32 src_d = level0->depth;
        u32 src_row = level0->row_pitch;
        u32 src_slice = level0->slice_pitch;

        for (u32 level = 1; level < texture.levels; ++level) {
            VolumeState* dst_vol = volume_for(level);
            if (dst_vol == nullptr || dst_vol->pixels == 0) {
                VITREO_TRACE("[d3d9] AUTOGEN POINT volume: missing level %u\n",
                             level);
                return;
            }
            std::vector<u8> dst(dst_vol->pixel_bytes);
            downsample_point_3d(src.data(), src_w, src_h, src_d, src_row,
                                src_slice, dst.data(), dst_vol->width,
                                dst_vol->height, dst_vol->depth,
                                dst_vol->row_pitch, dst_vol->slice_pitch, bpp);
            if (m.memory()
                    .write_bytes(dst_vol->pixels, dst.data(), dst.size())
                    .is_err()) {
                return;
            }
            if (texture.backend_3d && texture.backend_3d->valid()) {
                const u8* upload = dst.data();
                u32 upload_row = dst_vol->row_pitch;
                u32 upload_slice = dst_vol->slice_pitch;
                std::vector<u8> expanded;
                std::vector<u8> first_slice;
                const bool expanded_bump =
                    d3d9_expand_special_bump_to_rgba32f(
                        texture.format, dst_vol->width, dst_vol->height,
                        dst.data(), dst_vol->row_pitch, first_slice);
                if (expanded_bump || d3d9_expand_lock_to_bgra8(
                        texture.format, dst_vol->width, dst_vol->height,
                        dst.data(), dst_vol->row_pitch, first_slice)) {
                    const u32 expanded_bpp = expanded_bump ? 16u : 4u;
                    expanded.resize(static_cast<usize>(dst_vol->width) *
                                    dst_vol->height * expanded_bpp *
                                    dst_vol->depth);
                    const u32 dst_slice =
                        dst_vol->width * dst_vol->height * expanded_bpp;
                    std::memcpy(expanded.data(), first_slice.data(),
                                first_slice.size());
                    for (u32 z = 1; z < dst_vol->depth; ++z) {
                        std::vector<u8> slice;
                        const u8* slice_src =
                            dst.data() +
                            static_cast<usize>(z) * dst_vol->slice_pitch;
                        const bool slice_ok = expanded_bump
                            ? d3d9_expand_special_bump_to_rgba32f(
                                  texture.format, dst_vol->width,
                                  dst_vol->height, slice_src,
                                  dst_vol->row_pitch, slice)
                            : d3d9_expand_lock_to_bgra8(
                                  texture.format, dst_vol->width,
                                  dst_vol->height, slice_src,
                                  dst_vol->row_pitch, slice);
                        if (!slice_ok) {
                            return;
                        }
                        std::memcpy(expanded.data() +
                                        static_cast<usize>(z) * dst_slice,
                                    slice.data(), slice.size());
                    }
                    upload = expanded.data();
                    upload_row = dst_vol->width * expanded_bpp;
                    upload_slice = dst_slice;
                }
                if (!texture.backend_3d->update(level, upload, upload_row,
                                                upload_slice)) {
                    VITREO_TRACE("[d3d9] AUTOGEN POINT volume upload failed "
                                 "level=%u\n",
                                 level);
                    return;
                }
            }
            src = std::move(dst);
            src_w = dst_vol->width;
            src_h = dst_vol->height;
            src_d = dst_vol->depth;
            src_row = dst_vol->row_pitch;
            src_slice = dst_vol->slice_pitch;
        }
        return;
    }

    // 2D / cube: require level surfaces already allocated (CreateTexture does).
    const u32 face_count = texture.cube ? D3DCUBE_FACE_COUNT : 1u;
    for (u32 face = 0; face < face_count; ++face) {
        SurfaceState* level0 = surface_for(face, 0);
        if (level0 == nullptr || level0->pixels == 0) {
            VITREO_TRACE("[d3d9] AUTOGEN POINT: missing level 0 face=%u\n",
                         face);
            return;
        }
        std::vector<u8> src(level0->pixel_bytes);
        if (m.memory()
                .read_bytes(level0->pixels, src.data(), src.size())
                .is_err()) {
            return;
        }
        u32 src_w = level0->width;
        u32 src_h = level0->height;
        u32 src_pitch = level0->pitch;

        for (u32 level = 1; level < texture.levels; ++level) {
            SurfaceState* dst_surf = surface_for(face, level);
            if (dst_surf == nullptr || dst_surf->pixels == 0) {
                VITREO_TRACE("[d3d9] AUTOGEN POINT: missing level %u face=%u\n",
                             level, face);
                return;
            }
            std::vector<u8> dst(dst_surf->pixel_bytes);
            downsample_point_2d(src.data(), src_w, src_h, src_pitch, dst.data(),
                                dst_surf->width, dst_surf->height,
                                dst_surf->pitch, bpp);
            if (m.memory()
                    .write_bytes(dst_surf->pixels, dst.data(), dst.size())
                    .is_err()) {
                return;
            }
            if (texture.backend && texture.backend->valid()) {
                if (!upload_autogen_level_2d(
                        texture.backend.get(), level, face, texture.format,
                        dst_surf->width, dst_surf->height, dst.data(),
                        dst_surf->pitch, texture.cube)) {
                    VITREO_TRACE("[d3d9] AUTOGEN POINT upload failed level=%u "
                                 "face=%u\n",
                                 level, face);
                    return;
                }
            }
            src = std::move(dst);
            src_w = dst_surf->width;
            src_h = dst_surf->height;
            src_pitch = dst_surf->pitch;
        }
    }
}

} // namespace vitreo::winapi
