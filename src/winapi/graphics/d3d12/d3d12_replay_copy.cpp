// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_copy.cpp
/// @brief Copies and resolves.
///        Split out of d3d12_replay.cpp as verbatim code motion.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::d3d12 {

namespace {

/// The depth-stencil formats with a stencil plane (and their typeless
/// families): DXGI R32G8X24 (19-22) and R24G8 (44-47).
[[nodiscard]] bool is_depth_stencil_family(u32 format) {
    return (format >= DXGI_FORMAT_R32G8X24_TYPELESS &&
            format <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) ||
           (format >= DXGI_FORMAT_R24G8_TYPELESS &&
            format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT);
}

[[nodiscard]] u32 plane_count_of(u32 format) {
    return format_plane_count(format);
}

/// Subresource index -> (mip, array slice, plane) (Learn, "Subresources":
/// MipSlice + ArraySlice * MipLevels + PlaneSlice * MipLevels * ArraySize).
void split_subresource(const Resource& r, u32 index, u32& mip, u32& slice,
                       u32& plane) {
    const u32 mips = std::max(1u, r.desc.mip_levels);
    const u32 slices = std::max(1u, r.array_size());
    mip = index % mips;
    slice = (index / mips) % slices;
    plane = index / (mips * slices);
}

}  // namespace

void Replayer::copy_volume_footprint(const cmd::CopyTextureRegion& c,
                                     Resource& buffer, Resource& texture,
                                     bool to_buffer) {
    const CopyLocation& footprint = to_buffer ? c.dst : c.src;
    const CopyLocation& location = to_buffer ? c.src : c.dst;
    const u32 mip = location.subresource % std::max(1u, texture.desc.mip_levels);
    const FormatBlock block = format_block_info(footprint.format);
    const u32 bw = std::max(1u, block.width);
    const u32 bh = std::max(1u, block.height);
    // Footprint rows are rows of blocks; its slices are Height/bh rows.
    const u64 slice_pitch =
        static_cast<u64>(footprint.row_pitch) * ((footprint.height + bh - 1) / bh);
    u32 x = 0, y = 0, z = 0;
    u32 width = footprint.width, height = footprint.height, depth = footprint.depth;
    u64 offset = buffer.offset + footprint.offset;
    if (to_buffer) {
        if (c.has_box) {
            x = c.box[0];
            y = c.box[1];
            z = c.box[2];
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
            depth = c.box[5] - c.box[2];
        }
        offset += static_cast<u64>(c.z) * slice_pitch +
                  static_cast<u64>(c.y / bh) * footprint.row_pitch +
                  static_cast<u64>(c.x / bw) * block.bytes;
    } else {
        x = c.x;
        y = c.y;
        z = c.z;
        if (c.has_box) {
            offset += static_cast<u64>(c.box[2]) * slice_pitch +
                      static_cast<u64>(c.box[1] / bh) * footprint.row_pitch +
                      static_cast<u64>(c.box[0] / bw) * block.bytes;
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
            depth = c.box[5] - c.box[2];
        }
    }
    if (width == 0 || height == 0 || depth == 0) return;
    if (!f.m_backend.copy_texture_3d_buffer_region(
            texture.texture3d.get(), mip, x, y, z, width, height, depth,
            buffer.buffer(), offset, footprint.row_pitch, slice_pitch,
            /*to_texture=*/!to_buffer)) {
        unimplemented("CopyTextureRegion",
                      "a Texture3D footprint copy the backend refused");
    }
}

void Replayer::copy_depth_stencil_plane(const cmd::CopyTextureRegion& c,
                                        Resource& buffer, Resource& texture,
                                        u32 mip, u32 slice, u32 plane,
                                        bool to_buffer) {
    // D3D12 addresses a depth-stencil format's planes apart: plane 0 is the
    // depth (R32_TYPELESS footprints: a D24 value in bits 0-23, a D32 float
    // as is), plane 1 the stencil (R8_TYPELESS) (d3d12_layout.cpp). The
    // backend keeps them as a float depth and a byte stencil, which it reads
    // and writes whole (readback_ / update_depth_stencil_plane), so the
    // bytes are converted on the CPU once the GPU has finished with the
    // texture -- a wait here -- and written back in order.
    const CopyLocation& footprint = to_buffer ? c.dst : c.src;
    const bool stencil = plane == 1;
    const bool d24 = is_d24_family(texture.desc.format);
    const u32 level_w = static_cast<u32>(std::max<u64>(1, texture.desc.width >> mip));
    const u32 level_h = std::max(1u, texture.desc.height >> mip);
    const u32 texel = stencil ? 1u : 4u;
    std::vector<u8> level(static_cast<usize>(level_w) * level_h * texel);
    const unsigned level_pitch = level_w * texel;
    if (auto completion = f.m_backend.flush_completion()) {
        (void)completion->wait();
    } else {
        (void)f.m_backend.wait_idle();
    }
    if (!texture.texture->readback_depth_stencil_plane(mip, slice, stencil,
                                                       level.data(), level_pitch)) {
        unimplemented("CopyTextureRegion", "a depth-stencil plane the backend "
                                           "cannot read");
        return;
    }
    // The box within the level, and where it lands in the footprint.
    u32 x = 0, y = 0, width = footprint.width, height = footprint.height;
    u32 fx = 0, fy = 0;
    if (to_buffer) {
        if (c.has_box) {
            x = c.box[0];
            y = c.box[1];
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
        }
        fx = c.x;
        fy = c.y;
    } else {
        x = c.x;
        y = c.y;
        if (c.has_box) {
            fx = c.box[0];
            fy = c.box[1];
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
        }
    }
    if (width == 0 || height == 0 || x >= level_w || y >= level_h) return;
    width = std::min(width, level_w - x);
    height = std::min(height, level_h - y);
    const u64 row_pitch = footprint.row_pitch;
    const u64 base = footprint.offset + static_cast<u64>(fy) * row_pitch +
                     static_cast<u64>(fx) * texel;
    const usize span = static_cast<usize>((height - 1) * row_pitch + width * texel);
    std::vector<u8> rows(span);
    if (to_buffer) {
        for (u32 r = 0; r < height; ++r) {
            for (u32 col = 0; col < width; ++col) {
                const u8* from = level.data() +
                                 (static_cast<usize>(y + r) * level_w + x + col) * texel;
                u8* to = rows.data() + r * row_pitch + col * texel;
                if (stencil) {
                    *to = *from;
                    continue;
                }
                float depth = 0.0f;
                std::memcpy(&depth, from, 4);
                u32 stored = 0;
                if (d24) {
                    // UNORM24 from a float32 product, rounded to nearest even
                    // (measured, a05 clear "D24S8 clear 0.3": 004ccccc).
                    const float scaled =
                        std::clamp(depth, 0.0f, 1.0f) * 16777215.0f;
                    stored = static_cast<u32>(std::nearbyint(scaled));
                } else {
                    std::memcpy(&stored, &depth, 4);
                }
                std::memcpy(to, &stored, 4);
            }
        }
        graphics::BackendBuffer* staging = nullptr;
        u64 staging_offset = 0;
        u8* host = nullptr;
        std::unique_ptr<graphics::BackendBuffer> large;
        if (span <= CONSTANT_CHUNK_BYTES / 4 &&
            reserve(span, staging, staging_offset, host)) {
            std::memcpy(host, rows.data(), span);
        } else {
            graphics::BackendBufferDesc desc{};
            desc.byte_width = static_cast<unsigned>(span);
            large = f.m_backend.create_buffer(desc, rows.data());
            staging = large.get();
            staging_offset = 0;
        }
        if (staging == nullptr ||
            !f.m_backend.copy_buffer_region(buffer.buffer(), buffer.offset + base,
                                            staging, staging_offset, span)) {
            d3d12_diag("CopyTextureRegion: the plane copy was refused");
        }
        return;
    }
    // Buffer -> plane: the footprint's bytes into the level, written back.
    if (!gpu_read(buffer, base, rows.data(), span)) return;
    for (u32 r = 0; r < height; ++r) {
        for (u32 col = 0; col < width; ++col) {
            const u8* from = rows.data() + r * row_pitch + col * texel;
            u8* to = level.data() +
                     (static_cast<usize>(y + r) * level_w + x + col) * texel;
            if (stencil) {
                *to = *from;
                continue;
            }
            u32 stored = 0;
            std::memcpy(&stored, from, 4);
            float depth = 0.0f;
            if (d24) {
                depth = static_cast<float>(stored & 0xFFFFFFu) / 16777215.0f;
            } else {
                std::memcpy(&depth, &stored, 4);
            }
            std::memcpy(to, &depth, 4);
        }
    }
    if (!texture.texture->update_depth_stencil_plane(mip, slice, stencil,
                                                     level.data(), level_pitch)) {
        unimplemented("CopyTextureRegion", "a depth-stencil plane the backend "
                                           "cannot write");
    }
}

void Replayer::resolve(Resource& dst, Resource& src,
                       const cmd::ResolveSubresource& c) {
    u32 dmip = 0, dslice = 0, dplane = 0;
    u32 smip = 0, sslice = 0, splane = 0;
    split_subresource(dst, c.dst_sub, dmip, dslice, dplane);
    split_subresource(src, c.src_sub, smip, sslice, splane);
    const auto format = graphics::pixel_format_from_dxgi(c.format);
    const u32 sw = static_cast<u32>(std::max<u64>(1, src.desc.width >> smip));
    const u32 sh = std::max(1u, src.desc.height >> smip);
    if (!c.has_rect && c.dst_x == 0 && c.dst_y == 0) {
        (void)f.m_backend.resolve_texture_2d(dst.texture.get(), dmip, dslice,
                                             src.texture.get(), smip, sslice,
                                             format);
        return;
    }
    // ResolveSubresourceRegion: the source averaged whole into a scratch
    // target, then the rectangle copied to (DstX, DstY).
    i32 left = 0, top = 0, right = static_cast<i32>(sw), bottom = static_cast<i32>(sh);
    if (c.has_rect) {
        left = c.rect[0];
        top = c.rect[1];
        right = c.rect[2];
        bottom = c.rect[3];
    }
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        right > static_cast<i32>(sw) || bottom > static_cast<i32>(sh)) {
        return;
    }
    graphics::BackendTexture2DDesc desc{};
    desc.width = sw;
    desc.height = sh;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = format;
    desc.render_target = true;
    desc.shader_resource = true;
    auto scratch = f.m_backend.create_texture_2d(desc, nullptr, 0);
    if (!scratch ||
        !f.m_backend.resolve_texture_2d(scratch.get(), 0, 0, src.texture.get(),
                                        smip, sslice, format)) {
        unimplemented("ResolveSubresourceRegion", "a scratch resolve the "
                                                  "backend refused");
        return;
    }
    graphics::BackendTextureLocation to{};
    to.texture_2d = dst.texture.get();
    to.mip = dmip;
    to.slice = dslice;
    to.x = c.dst_x;
    to.y = c.dst_y;
    graphics::BackendTextureLocation from{};
    from.texture_2d = scratch.get();
    from.x = static_cast<unsigned>(left);
    from.y = static_cast<unsigned>(top);
    if (!f.m_backend.copy_texture_elements(to, from,
                                           static_cast<unsigned>(right - left),
                                           static_cast<unsigned>(bottom - top), 1)) {
        d3d12_diag("ResolveSubresourceRegion: the backend refused the copy");
    }
    // Kept until the GPU has run the copy.
    rr.scratch_textures.push_back(
        {std::move(scratch), f.m_backend.recording_generation()});
}

void Replayer::copy_texture_region(const cmd::CopyTextureRegion& c) {
    Resource* dst = resource(c.dst.resource, c.dst.serial);
    Resource* src = resource(c.src.resource, c.src.serial);
    if (dst == nullptr || src == nullptr) return;
    const bool dst_buffer =
        c.dst.type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    const bool src_buffer =
        c.src.type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    if (dst_buffer == src_buffer) {
        if (dst_buffer) return;  // footprint to footprint is invalid
        // Texture to texture.
        u32 dmip = 0, dslice = 0, dplane = 0, smip = 0, sslice = 0, splane = 0;
        split_subresource(*dst, c.dst.subresource, dmip, dslice, dplane);
        split_subresource(*src, c.src.subresource, smip, sslice, splane);
        if (dplane != 0 || splane != 0) {
            unimplemented("CopyTextureRegion", "stencil-plane copies");
            return;
        }
        graphics::BackendTextureLocation to{};
        graphics::BackendTextureLocation from{};
        to.texture_2d = dst->texture.get();
        to.texture_3d = dst->texture3d.get();
        to.mip = dmip;
        to.slice = dslice;
        to.x = c.x;
        to.y = c.y;
        to.z = c.z;
        from.texture_2d = src->texture.get();
        from.texture_3d = src->texture3d.get();
        from.mip = smip;
        from.slice = sslice;
        u32 width = 0, height = 0, depth = 1;
        if (c.has_box) {
            from.x = c.box[0];
            from.y = c.box[1];
            from.z = c.box[2];
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
            depth = c.box[5] - c.box[2];
        } else {
            const u32 w = static_cast<u32>(src->desc.width);
            width = std::max(1u, w >> smip);
            height = std::max(1u, src->desc.height >> smip);
            depth = src->texture3d ? std::max(1u, src->desc.depth_or_array_size >>
                                                      smip)
                                   : 1u;
        }
        if (width == 0 || height == 0 || depth == 0) return;
        if (!f.m_backend.copy_texture_elements(to, from, width, height, depth)) {
            d3d12_diag("CopyTextureRegion: backend refused the copy");
        }
        return;
    }
    // Buffer <-> texture through a placed footprint.
    const CopyLocation& footprint = dst_buffer ? c.dst : c.src;
    Resource* buffer_resource = dst_buffer ? dst : src;
    Resource* texture_resource = dst_buffer ? src : dst;
    const CopyLocation& texture_location = dst_buffer ? c.src : c.dst;
    if (!buffer_resource->is_buffer()) return;
    if (texture_resource->texture3d) {
        copy_volume_footprint(c, *buffer_resource, *texture_resource, dst_buffer);
        return;
    }
    if (!texture_resource->texture) return;
    u32 mip = 0, slice = 0, plane = 0;
    split_subresource(*texture_resource, texture_location.subresource, mip,
                      slice, plane);
    if (plane_count_of(texture_resource->desc.format) == 2 &&
        is_depth_stencil_family(texture_resource->desc.format)) {
        copy_depth_stencil_plane(c, *buffer_resource, *texture_resource, mip,
                                 slice, plane, dst_buffer);
        return;
    }
    if (plane != 0) {
        unimplemented("CopyTextureRegion", "plane footprint copies of this format");
        return;
    }
    // The texel rectangle: the box (source texture) or the footprint's size.
    u32 x = 0, y = 0, width = footprint.width, height = footprint.height;
    u64 offset = buffer_resource->offset + footprint.offset;
    const FormatBlock block = format_block_info(footprint.format);
    if (dst_buffer) {
        if (c.has_box) {
            x = c.box[0];
            y = c.box[1];
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
        }
        // The destination footprint is written at (DstX, DstY) within it.
        offset += static_cast<u64>(c.y / std::max(1u, block.height)) *
                      footprint.row_pitch +
                  static_cast<u64>(c.x / std::max(1u, block.width)) *
                      block.bytes;
    } else {
        x = c.x;
        y = c.y;
        if (c.has_box) {
            offset += static_cast<u64>(c.box[1] / std::max(1u, block.height)) *
                          footprint.row_pitch +
                      static_cast<u64>(c.box[0] / std::max(1u, block.width)) *
                          block.bytes;
            width = c.box[3] - c.box[0];
            height = c.box[4] - c.box[1];
        }
    }
    if (width == 0 || height == 0) return;
    if (!f.m_backend.copy_texture_buffer_region(
            texture_resource->texture.get(), mip, slice, x, y, width, height,
            buffer_resource->buffer(), offset, footprint.row_pitch,
            /*to_texture=*/!dst_buffer)) {
        d3d12_diag("CopyTextureRegion: backend refused the footprint copy");
    }
}

void Replayer::copy_resource(const cmd::CopyResource& c) {
    Resource* dst = resource(c.dst, c.dst_serial);
    Resource* src = resource(c.src, c.src_serial);
    if (dst == nullptr || src == nullptr) return;
    if (dst->is_buffer()) {
        if (!f.m_backend.copy_buffer_region(dst->buffer(), dst->offset,
                                            src->buffer(), src->offset,
                                            dst->desc.width)) {
            d3d12_diag("CopyResource: backend refused the buffer copy");
        }
        return;
    }
    if (dst->texture3d && src->texture3d) {
        (void)f.m_backend.copy_texture_3d(dst->texture3d.get(),
                                          src->texture3d.get());
        return;
    }
    if (dst->texture && src->texture) {
        (void)f.m_backend.copy_texture_2d(dst->texture.get(), src->texture.get());
    }
}

}  // namespace vitreo::winapi::d3d12
