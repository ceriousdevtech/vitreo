// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_transfers.cpp
/// @brief IDirect3DDevice9 surface transfer contracts.
///
/// UpdateSurface, UpdateTexture, StretchRect, ColorFill,
/// GetRenderTargetData, and GetFrontBufferData. Prefer host GPU blit
/// (`copy_texture_2d` / `stretch_texture_2d_region` / `readback` /
/// `clear_render_target`) when both sides have backend textures; otherwise
/// copy through guest lockable memory and upload.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_cs.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 D3DUSAGE_RENDERTARGET = 0x00000001;
constexpr u32 D3DUSAGE_DEPTHSTENCIL = 0x00000002;
constexpr u32 D3DTEXF_NONE = 0;
constexpr u32 D3DTEXF_POINT = 1;
constexpr u32 D3DTEXF_LINEAR = 2;

struct GuestRect {
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
};

struct GuestPoint {
    i32 x{0};
    i32 y{0};
};

[[nodiscard]] bool read_rect(Marshaler& m, GuestAddr addr, GuestRect& out) {
    if (addr == 0) return false;
    std::array<i32, 4> raw{};
    if (m.memory()
            .read_bytes(addr, reinterpret_cast<Byte*>(raw.data()),
                        sizeof(raw))
            .is_err()) {
        return false;
    }
    out.left = raw[0];
    out.top = raw[1];
    out.right = raw[2];
    out.bottom = raw[3];
    return out.right > out.left && out.bottom > out.top;
}

[[nodiscard]] bool read_point(Marshaler& m, GuestAddr addr, GuestPoint& out) {
    if (addr == 0) return false;
    std::array<i32, 2> raw{};
    if (m.memory()
            .read_bytes(addr, reinterpret_cast<Byte*>(raw.data()),
                        sizeof(raw))
            .is_err()) {
        return false;
    }
    out.x = raw[0];
    out.y = raw[1];
    return true;
}

[[nodiscard]] bool rect_inside(const GuestRect& r, u32 width, u32 height) {
    return r.left >= 0 && r.top >= 0 && r.right <= static_cast<i32>(width) &&
           r.bottom <= static_cast<i32>(height);
}

[[nodiscard]] u32 rect_width(const GuestRect& r) {
    return static_cast<u32>(r.right - r.left);
}

[[nodiscard]] u32 rect_height(const GuestRect& r) {
    return static_cast<u32>(r.bottom - r.top);
}

[[nodiscard]] bool is_depth_format(u32 format) {
    switch (format) {
    case 71:
    case 73:
    case 75:
    case 77:
    case 80:
    case 82:
    case 83:
    case 84:
    case 111:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool copy_guest_rect(Marshaler& m,
                                   const D3D9Manager::SurfaceState& src,
                                   const GuestRect& src_rect,
                                   D3D9Manager::SurfaceState& dst, i32 dst_x,
                                   i32 dst_y) {
    const u32 bpp = d3d9_format_bytes_per_pixel(src.format);
    if (src.pixels == 0 || dst.pixels == 0) return false;
    const u32 w = rect_width(src_rect);
    const u32 h = rect_height(src_rect);
    if (dst_x < 0 || dst_y < 0 ||
        dst_x + static_cast<i32>(w) > static_cast<i32>(dst.width) ||
        dst_y + static_cast<i32>(h) > static_cast<i32>(dst.height)) {
        return false;
    }
    u32 row_bytes = w * bpp;
    u32 rows = h;
    u32 src_x_bytes = static_cast<u32>(src_rect.left) * bpp;
    u32 src_y = static_cast<u32>(src_rect.top);
    u32 dst_x_bytes = static_cast<u32>(dst_x) * bpp;
    u32 dst_row_y = static_cast<u32>(dst_y);
    if (bpp == 0) {
        // DXT stores 4x4 texels per block: 8 bytes for DXT1, 16 for DXT2-5.
        // Whole mip edges may be smaller than a block; interior edges must
        // align so neighboring compressed texels cannot be overwritten.
        u32 block_bytes = 0;
        switch (src.format) {
        case 0x31545844u: block_bytes = 8; break;
        case 0x32545844u: case 0x33545844u:
        case 0x34545844u: case 0x35545844u: block_bytes = 16; break;
        default: return false;
        }
        if ((src_rect.left % 4) || (src_rect.top % 4) || (dst_x % 4) || (dst_y % 4) ||
            ((src_rect.right % 4) && src_rect.right != static_cast<i32>(src.width)) ||
            ((src_rect.bottom % 4) && src_rect.bottom != static_cast<i32>(src.height)) ||
            (((dst_x + w) % 4) && dst_x + w != dst.width) ||
            (((dst_y + h) % 4) && dst_y + h != dst.height)) return false;
        row_bytes = ((w + 3u) / 4u) * block_bytes;
        rows = (h + 3u) / 4u;
        src_x_bytes = (static_cast<u32>(src_rect.left) / 4u) * block_bytes;
        src_y /= 4u;
        dst_x_bytes = (static_cast<u32>(dst_x) / 4u) * block_bytes;
        dst_row_y /= 4u;
    }
    std::vector<u8> row(row_bytes);
    for (u32 y = 0; y < rows; ++y) {
        const GuestAddr src_row = src.pixels +
            static_cast<GuestAddr>(src_y + y) * src.pitch + src_x_bytes;
        const GuestAddr dst_row = dst.pixels +
            static_cast<GuestAddr>(dst_row_y + y) * dst.pitch + dst_x_bytes;
        if (m.memory().read_bytes(src_row, row.data(), row.size()).is_err() ||
            m.memory().write_bytes(dst_row, row.data(), row.size()).is_err()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const u8* resolve_current_palette_entries(
    const D3D9Manager::DeviceState& state) noexcept {
    static const D3D9Manager::DeviceState::TexturePalette kDefault = [] {
        D3D9Manager::DeviceState::TexturePalette entries{};
        entries.fill(0xFFu);
        return entries;
    }();
    if (const auto it =
            state.texture_palettes.find(state.current_texture_palette);
        it != state.texture_palettes.end()) {
        return it->second.data();
    }
    return kDefault.data();
}

/// Upload guest surface pixels into a DEFAULT backend texture. Palettized
/// formats expand through the device's current texture palette (Learn:
/// Texture Palettes) before upload — same contract as Unlock/bind.
[[nodiscard]] bool upload_surface_level(
    graphics::BackendTexture2D* backend, Marshaler& m,
    D3D9Manager::SurfaceState& surface,
    D3D9Manager::DeviceState* device_state,
    D3D9Manager::TextureState* texture) {
    if (backend == nullptr || surface.pixels == 0) return false;
    std::vector<u8> pixels(surface.pixel_bytes);
    if (m.memory()
            .read_bytes(surface.pixels, pixels.data(), pixels.size())
            .is_err()) {
        return false;
    }
    std::vector<u8> expanded;
    const u8* upload = pixels.data();
    u32 upload_pitch = surface.pitch;
    if (d3d9_is_palettized_format(surface.format)) {
        const u8* palette =
            device_state != nullptr
                ? resolve_current_palette_entries(*device_state)
                : nullptr;
        if (d3d9_expand_palettized_to_bgra8(
                surface.format, surface.width, surface.height, pixels.data(),
                surface.pitch, palette, expanded)) {
            upload = expanded.data();
            upload_pitch = surface.width * 4u;
            if (texture != nullptr && device_state != nullptr) {
                texture->palettized_upload_serial =
                    device_state->texture_palette_serial;
            }
        }
    } else {
        const bool expanded_bump =
            d3d9_expand_special_bump_to_rgba32f(
                surface.format, surface.width, surface.height, pixels.data(),
                surface.pitch, expanded);
        if (expanded_bump || d3d9_expand_lock_to_bgra8(
                surface.format, surface.width, surface.height, pixels.data(),
                surface.pitch, expanded)) {
            upload = expanded.data();
            upload_pitch = surface.width * (expanded_bump ? 16u : 4u);
        }
    }
    if (surface.kind == D3D9Manager::SurfaceState::Kind::TextureLevel &&
        (surface.face != 0 || backend->desc().cube ||
         backend->desc().array_size > 1)) {
        return backend->update_slice(surface.level, surface.face, upload,
                                     upload_pitch);
    }
    return backend->update(surface.level, upload, upload_pitch);
}

[[nodiscard]] bool stretch_guest_rect(Marshaler& m,
                                      const D3D9Manager::SurfaceState& src,
                                      const GuestRect& src_rect,
                                      D3D9Manager::SurfaceState& dst,
                                      const GuestRect& dst_rect,
                                      bool bilinear) {
    const u32 bpp = d3d9_format_bytes_per_pixel(src.format);
    if (bpp == 0 || bpp > 16 || src.pixels == 0 || dst.pixels == 0) {
        return false;
    }
    const u32 sw = rect_width(src_rect);
    const u32 sh = rect_height(src_rect);
    const u32 dw = rect_width(dst_rect);
    const u32 dh = rect_height(dst_rect);
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return false;

    std::vector<u8> src_pixels(static_cast<usize>(src.pitch) * src.height);
    if (m.memory()
            .read_bytes(src.pixels, src_pixels.data(),
                        static_cast<usize>(src.pitch) * src.height)
            .is_err()) {
        return false;
    }

    auto sample = [&](i32 sx, i32 sy, u8* out_px) {
        sx = std::clamp(sx, src_rect.left, src_rect.right - 1);
        sy = std::clamp(sy, src_rect.top, src_rect.bottom - 1);
        const u8* p = src_pixels.data() + static_cast<usize>(sy) * src.pitch +
                      static_cast<usize>(sx) * bpp;
        std::memcpy(out_px, p, bpp);
    };

    std::vector<u8> dst_row(static_cast<usize>(dw) * bpp);
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            if (!bilinear || bpp != 4) {
                const i32 sx =
                    src_rect.left +
                    static_cast<i32>((static_cast<u64>(x) * sw) / dw);
                const i32 sy =
                    src_rect.top +
                    static_cast<i32>((static_cast<u64>(y) * sh) / dh);
                sample(sx, sy, dst_row.data() + static_cast<usize>(x) * bpp);
            } else {
                const float fx = static_cast<float>(x + 0.5f) *
                                     static_cast<float>(sw) /
                                     static_cast<float>(dw) -
                                 0.5f;
                const float fy = static_cast<float>(y + 0.5f) *
                                     static_cast<float>(sh) /
                                     static_cast<float>(dh) -
                                 0.5f;
                const i32 x0 = src_rect.left + static_cast<i32>(std::floor(fx));
                const i32 y0 = src_rect.top + static_cast<i32>(std::floor(fy));
                const float tx = fx - std::floor(fx);
                const float ty = fy - std::floor(fy);
                u8 p00[4], p10[4], p01[4], p11[4];
                sample(x0, y0, p00);
                sample(x0 + 1, y0, p10);
                sample(x0, y0 + 1, p01);
                sample(x0 + 1, y0 + 1, p11);
                for (u32 c = 0; c < 4; ++c) {
                    const float top = static_cast<float>(p00[c]) * (1.0f - tx) +
                                      static_cast<float>(p10[c]) * tx;
                    const float bot = static_cast<float>(p01[c]) * (1.0f - tx) +
                                      static_cast<float>(p11[c]) * tx;
                    dst_row[static_cast<usize>(x) * 4u + c] =
                        static_cast<u8>(top * (1.0f - ty) + bot * ty + 0.5f);
                }
            }
        }
        const GuestAddr dst_addr =
            dst.pixels +
            static_cast<GuestAddr>(
                (dst_rect.top + static_cast<i32>(y)) * dst.pitch +
                dst_rect.left * static_cast<i32>(bpp));
        if (m.memory()
                .write_bytes(dst_addr, dst_row.data(), dst_row.size())
                .is_err()) {
            return false;
        }
    }
    return true;
}

void pack_d3dcolor_bgra(u32 color, u8* out4) {
    out4[0] = static_cast<u8>(color & 0xFFu);
    out4[1] = static_cast<u8>((color >> 8) & 0xFFu);
    out4[2] = static_cast<u8>((color >> 16) & 0xFFu);
    out4[3] = static_cast<u8>((color >> 24) & 0xFFu);
}

/// Pack D3DCOLOR (A8R8G8B8) into little-endian D3DFMT_R5G6B5 bytes.
void pack_d3dcolor_r5g6b5(u32 color, u8* out2) {
    const u32 r = (color >> 16) & 0xFFu;
    const u32 g = (color >> 8) & 0xFFu;
    const u32 b = color & 0xFFu;
    const u16 packed = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) |
                                        (b >> 3));
    out2[0] = static_cast<u8>(packed & 0xFFu);
    out2[1] = static_cast<u8>((packed >> 8) & 0xFFu);
}

/// Pack D3DCOLOR into D3DFMT_X1R5G5B5 / D3DFMT_A1R5G5B5 (little-endian).
/// For X1 the high bit is written as 1 (don't-care / opaque on Windows).
void pack_d3dcolor_x1a1r5g5b5(u32 color, bool alpha, u8* out2) {
    const u32 a = (color >> 24) & 0xFFu;
    const u32 r = (color >> 16) & 0xFFu;
    const u32 g = (color >> 8) & 0xFFu;
    const u32 b = color & 0xFFu;
    u16 packed = static_cast<u16>(((r >> 3) << 10) | ((g >> 3) << 5) |
                                  (b >> 3));
    if (!alpha || a >= 0x80u) {
        packed = static_cast<u16>(packed | 0x8000u);
    }
    out2[0] = static_cast<u8>(packed & 0xFFu);
    out2[1] = static_cast<u8>((packed >> 8) & 0xFFu);
}

/// Convert one host BGRA8 pixel (backend readback native) to D3DCOLOR.
[[nodiscard]] u32 d3dcolor_from_bgra(const u8 bgra[4]) {
    return (static_cast<u32>(bgra[3]) << 24) |
           (static_cast<u32>(bgra[2]) << 16) |
           (static_cast<u32>(bgra[1]) << 8) | static_cast<u32>(bgra[0]);
}

/// Pack a BGRA8 readback buffer into a SYSTEMMEM surface of a supported
/// colour format. Host backends expose BGRA8Unorm for the common RT formats
/// (including R5G6B5 / A1R5G5B5 / X1R5G5B5 storage via BGRA8).
[[nodiscard]] bool write_bgra_readback_to_surface(
    Marshaler& m, const u8* bgra, D3D9Manager::SurfaceState& dst) {
    constexpr u32 D3DFMT_R5G6B5 = 23;
    constexpr u32 D3DFMT_X1R5G5B5 = 24;
    constexpr u32 D3DFMT_A1R5G5B5 = 25;
    if (bgra == nullptr || dst.pixels == 0) {
        return false;
    }
    switch (dst.format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        for (u32 y = 0; y < dst.height; ++y) {
            const u8* row = bgra + static_cast<usize>(y) * dst.width * 4u;
            const GuestAddr dst_row =
                dst.pixels + static_cast<GuestAddr>(y) * dst.pitch;
            if (m.memory()
                    .write_bytes(dst_row, row, dst.width * 4u)
                    .is_err()) {
                return false;
            }
        }
        return true;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        for (u32 y = 0; y < dst.height; ++y) {
            for (u32 x = 0; x < dst.width; ++x) {
                const u8* px =
                    bgra + (static_cast<usize>(y) * dst.width + x) * 4u;
                const u32 color = d3dcolor_from_bgra(px);
                u8 packed[2]{};
                if (dst.format == D3DFMT_R5G6B5) {
                    pack_d3dcolor_r5g6b5(color, packed);
                } else {
                    pack_d3dcolor_x1a1r5g5b5(
                        color, dst.format == D3DFMT_A1R5G5B5, packed);
                }
                const GuestAddr addr =
                    dst.pixels + static_cast<GuestAddr>(y) * dst.pitch +
                    static_cast<GuestAddr>(x) * 2u;
                if (m.memory().write_bytes(addr, packed, 2).is_err()) {
                    return false;
                }
            }
        }
        return true;
    default:
        return false;
    }
}

std::array<float, 4> d3dcolor_to_rgba(u32 color) {
    return {static_cast<float>((color >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((color >> 8) & 0xFFu) / 255.0f,
            static_cast<float>(color & 0xFFu) / 255.0f,
            static_cast<float>((color >> 24) & 0xFFu) / 255.0f};
}

} // namespace

graphics::BackendTexture2D*
D3D9Manager::resolve_surface_color_backend(SurfaceState& surface) {
    if (surface.backend && surface.backend->valid()) {
        return surface.backend.get();
    }
    if (surface.swap_chain_color != nullptr &&
        surface.swap_chain_color->valid()) {
        return surface.swap_chain_color;
    }
    if (surface.kind != SurfaceState::Kind::TextureLevel ||
        surface.container == 0) {
        return nullptr;
    }
    ComObject* texture_entry =
        find(surface.container, D3D9InterfaceId::Texture);
    if (texture_entry == nullptr) {
        texture_entry =
            find(surface.container, D3D9InterfaceId::CubeTexture);
    }
    if (texture_entry == nullptr || !texture_entry->texture ||
        !texture_entry->texture->backend ||
        !texture_entry->texture->backend->valid()) {
        return nullptr;
    }
    return texture_entry->texture->backend.get();
}

[[nodiscard]] D3D9Manager::DeviceState* D3D9Manager::device_state_for(
    u32 device) {
    ComObject* device_entry = find(device, D3D9InterfaceId::Device);
    if (device_entry == nullptr || !device_entry->device_state) {
        return nullptr;
    }
    return device_entry->device_state.get();
}

[[nodiscard]] D3D9Manager::TextureState*
D3D9Manager::texture_state_for_surface(const SurfaceState& surface) {
    if (surface.kind != SurfaceState::Kind::TextureLevel ||
        surface.container == 0) {
        return nullptr;
    }
    ComObject* texture_entry =
        find(surface.container, D3D9InterfaceId::Texture);
    if (texture_entry == nullptr) {
        texture_entry =
            find(surface.container, D3D9InterfaceId::CubeTexture);
    }
    if (texture_entry == nullptr || !texture_entry->texture) {
        return nullptr;
    }
    return texture_entry->texture.get();
}

void D3D9Manager::update_surface(Marshaler& m, u32 device) {
    // Learn: UpdateSurface — SYSTEMMEM → DEFAULT, same format, no stretch.
    const u32 src_obj = static_cast<u32>(m.arg(1));
    const GuestAddr src_rect_addr = m.arg_ptr(2);
    const u32 dst_obj = static_cast<u32>(m.arg(3));
    const GuestAddr dst_point_addr = m.arg_ptr(4);

    ComObject* src_entry = find(src_obj, D3D9InterfaceId::Surface);
    ComObject* dst_entry = find(dst_obj, D3D9InterfaceId::Surface);
    if (src_entry == nullptr || dst_entry == nullptr || !src_entry->surface ||
        !dst_entry->surface || src_obj == dst_obj) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    SurfaceState& src = *src_entry->surface;
    SurfaceState& dst = *dst_entry->surface;

    if (src.pool != D3DPOOL_SYSTEMMEM || dst.pool != D3DPOOL_DEFAULT ||
        src.format != dst.format || src.locked || dst.locked ||
        src.multi_sample_type != D3DMULTISAMPLE_NONE ||
        dst.multi_sample_type != D3DMULTISAMPLE_NONE ||
        (src.usage & D3DUSAGE_DEPTHSTENCIL) != 0 ||
        (dst.usage & D3DUSAGE_DEPTHSTENCIL) != 0 ||
        is_depth_format(src.format)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    GuestRect src_rect{0, 0, static_cast<i32>(src.width),
                       static_cast<i32>(src.height)};
    if (src_rect_addr != 0 && !read_rect(m, src_rect_addr, src_rect)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (!rect_inside(src_rect, src.width, src.height)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    GuestPoint dst_point{0, 0};
    if (dst_point_addr != 0 && !read_point(m, dst_point_addr, dst_point)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    const u32 copy_w = rect_width(src_rect);
    const u32 copy_h = rect_height(src_rect);
    if (dst_point.x < 0 || dst_point.y < 0 ||
        dst_point.x + static_cast<i32>(copy_w) >
            static_cast<i32>(dst.width) ||
        dst_point.y + static_cast<i32>(copy_h) >
            static_cast<i32>(dst.height)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    if (!copy_guest_rect(m, src, src_rect, dst, dst_point.x, dst_point.y)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    if (graphics::BackendTexture2D* backend =
            resolve_surface_color_backend(dst)) {
        if (!upload_surface_level(backend, m, dst, device_state_for(device),
                                  texture_state_for_surface(dst))) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::update_texture(Marshaler& m, u32 device) {
    // Learn: UpdateTexture — SYSTEMMEM source into DEFAULT destination.
    // Cubes use the same pool/format/size rules, then copy every face × level
    // (level_surfaces is face * levels + level). A Texture-only lookup leaves
    // DEFAULT cubes empty after a successful-looking guest call.
    const u32 src_obj = static_cast<u32>(m.arg(1));
    const u32 dst_obj = static_cast<u32>(m.arg(2));
    ComObject* src_entry = find(src_obj, D3D9InterfaceId::Texture);
    if (src_entry == nullptr) {
        src_entry = find(src_obj, D3D9InterfaceId::CubeTexture);
    }
    ComObject* dst_entry = find(dst_obj, D3D9InterfaceId::Texture);
    if (dst_entry == nullptr) {
        dst_entry = find(dst_obj, D3D9InterfaceId::CubeTexture);
    }
    if (src_entry == nullptr || dst_entry == nullptr || !src_entry->texture ||
        !dst_entry->texture) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    TextureState& src = *src_entry->texture;
    TextureState& dst = *dst_entry->texture;
    if (src.pool != D3DPOOL_SYSTEMMEM || dst.pool != D3DPOOL_DEFAULT ||
        src.format != dst.format || src.width != dst.width ||
        src.height != dst.height || src.cube != dst.cube) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    DeviceState* device_state = device_state_for(device);
    const auto ensure_level = [&](TextureState& texture, u32 texture_obj,
                                  u32 level, u32 face) -> SurfaceState* {
        const usize index = texture.cube
                                ? static_cast<usize>(face) * texture.levels +
                                      level
                                : static_cast<usize>(level);
        if (level >= texture.levels ||
            index >= texture.level_surfaces.size()) {
            return nullptr;
        }
        if (texture.level_surfaces[index] == 0) {
            u32 level_width = texture.width >> level;
            u32 level_height = texture.height >> level;
            if (level_width == 0) level_width = 1;
            if (level_height == 0) level_height = 1;
            const u32 surface = create_surface_object(
                device, level_width, level_height, texture.format,
                texture.usage, texture.pool,
                static_cast<int>(SurfaceState::Kind::TextureLevel),
                texture_obj, level, face);
            if (surface == 0) return nullptr;
            texture.level_surfaces[index] = surface;
        }
        ComObject* surf =
            find(texture.level_surfaces[index], D3D9InterfaceId::Surface);
        if (surf == nullptr || !surf->surface) return nullptr;
        return surf->surface.get();
    };

    const u32 faces = src.cube ? D3DCUBE_FACE_COUNT : 1u;
    const u32 levels = std::min(src.levels, dst.levels);
    for (u32 face = 0; face < faces; ++face) {
        for (u32 level = 0; level < levels; ++level) {
            SurfaceState* ss = ensure_level(src, src_obj, level, face);
            SurfaceState* ds = ensure_level(dst, dst_obj, level, face);
            if (ss == nullptr || ds == nullptr) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            GuestRect full{0, 0, static_cast<i32>(ss->width),
                           static_cast<i32>(ss->height)};
            if (!copy_guest_rect(m, *ss, full, *ds, 0, 0)) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            if (graphics::BackendTexture2D* backend =
                    resolve_surface_color_backend(*ds)) {
                if (!upload_surface_level(backend, m, *ds, device_state,
                                          &dst)) {
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
            }
        }
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::get_render_target_data(Marshaler& m, u32 device) {
    // Learn: GetRenderTargetData — DEFAULT RT → SYSTEMMEM, same size/format.
    const u32 src_obj = static_cast<u32>(m.arg(1));
    const u32 dst_obj = static_cast<u32>(m.arg(2));
    ComObject* src_entry = find(src_obj, D3D9InterfaceId::Surface);
    ComObject* dst_entry = find(dst_obj, D3D9InterfaceId::Surface);
    if (src_entry == nullptr || dst_entry == nullptr || !src_entry->surface ||
        !dst_entry->surface) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    SurfaceState& src = *src_entry->surface;
    SurfaceState& dst = *dst_entry->surface;
    if (src.pool != D3DPOOL_DEFAULT || dst.pool != D3DPOOL_SYSTEMMEM ||
        src.width != dst.width || src.height != dst.height ||
        src.format != dst.format || src.locked || dst.locked ||
        src.multi_sample_type != D3DMULTISAMPLE_NONE ||
        (src.usage & D3DUSAGE_DEPTHSTENCIL) != 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    ComObject* device_entry = find(device, D3D9InterfaceId::Device);
    if (device_entry != nullptr && device_entry->device_state) {
        flush_render_pass(*device_entry->device_state);
    }

    // Host colour backends store common RT formats as BGRA8Unorm. Pack into
    // the matching SYSTEMMEM format (A8/X8R8G8B8 and 16-bit colour variants).
    constexpr u32 D3DFMT_R5G6B5 = 23;
    constexpr u32 D3DFMT_X1R5G5B5 = 24;
    constexpr u32 D3DFMT_A1R5G5B5 = 25;
    const bool packable_from_bgra =
        dst.format == D3DFMT_A8R8G8B8 || dst.format == D3DFMT_X8R8G8B8 ||
        dst.format == D3DFMT_R5G6B5 || dst.format == D3DFMT_X1R5G5B5 ||
        dst.format == D3DFMT_A1R5G5B5;
    graphics::BackendTexture2D* backend = resolve_surface_color_backend(src);
    if (backend != nullptr && backend->valid() && dst.pixels != 0 &&
        packable_from_bgra) {
        std::vector<u8> bgra(static_cast<usize>(src.width) * src.height * 4u);
        const bool sliced =
            backend->desc().cube || backend->desc().array_size > 1;
        const bool read_ok =
            sliced ? backend->readback_slice(src.level, src.face, bgra.data(),
                                             src.width * 4u)
                   : backend->readback(src.level, bgra.data(), src.width * 4u);
        if (read_ok) {
            if (!write_bgra_readback_to_surface(m, bgra.data(), dst)) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            m.set_return(D3D_OK);
            return;
        }
    }

    // The swap chain's back buffer lives on the GPU and has no standalone
    // BackendTexture2D, so the readback above cannot reach it. Take it from
    // the swap chain *before* falling back to guest memory: the guest-side
    // shadow of a GPU-rendered target is never written by rendering, so
    // copy_guest_rect would report D3D_OK over a zero-filled buffer and
    // silently shadow the only correct source.
    if (device_entry != nullptr && device_entry->device_state &&
        src_obj == device_entry->device_state->back_buffer_surface &&
        device_entry->device_state->swap_chain &&
        (dst.format == D3DFMT_A8R8G8B8 || dst.format == D3DFMT_X8R8G8B8) &&
        dst.pixels != 0) {
        std::vector<u8> rgba;
        d3d9_cs_drain();
        if (device_entry->device_state->swap_chain->readback_texture_rgba(
                nullptr, rgba) &&
            rgba.size() >= static_cast<usize>(dst.width) * dst.height * 4u) {
            for (u32 y = 0; y < dst.height; ++y) {
                for (u32 x = 0; x < dst.width; ++x) {
                    const usize o =
                        (static_cast<usize>(y) * dst.width + x) * 4u;
                    const u8 px[4] = {rgba[o + 2], rgba[o + 1], rgba[o + 0],
                                      rgba[o + 3]};
                    const GuestAddr addr =
                        dst.pixels +
                        static_cast<GuestAddr>(y) * dst.pitch + x * 4u;
                    if (m.memory().write_bytes(addr, px, 4).is_err()) {
                        m.set_return(D3DERR_INVALIDCALL);
                        return;
                    }
                }
            }
            m.set_return(D3D_OK);
            return;
        }
    }

    // Last resort: a surface with no GPU backing at all (for example one the
    // guest filled through Lock). Its guest pixels are then the only copy.
    if (src.pixels != 0 && dst.pixels != 0) {
        GuestRect full{0, 0, static_cast<i32>(src.width),
                       static_cast<i32>(src.height)};
        if (copy_guest_rect(m, src, full, dst, 0, 0)) {
            m.set_return(D3D_OK);
            return;
        }
    }

    VITREO_TRACE("[d3d9] GetRenderTargetData -> E_NOTIMPL "
                 "(no readable backend for %ux%u fmt=%u)\n",
                 src.width, src.height, src.format);
    m.set_return(E_NOTIMPL_D3D);
}

void D3D9Manager::get_front_buffer_data(Marshaler& m, u32 device,
                                        u32 swap_chain_index,
                                        u32 dst_obj) {
    // Learn: IDirect3DDevice9::GetFrontBufferData(iSwapChain, pDestSurface).
    ComObject* device_entry = find(device, D3D9InterfaceId::Device);
    if (device_entry == nullptr || !device_entry->device_state ||
        swap_chain_index != 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *device_entry->device_state;
    get_front_buffer_data(m, state, state.swap_chain.get(),
                          state.back_buffer_surface, dst_obj);
}

void D3D9Manager::get_front_buffer_data(Marshaler& m, DeviceState& state,
                                        graphics::BackendSwapChain* chain,
                                        u32 back_buffer_surface,
                                        u32 dst_obj) {
    // Learn: GetFrontBufferData — copy the device/swapchain front buffer into
    // a SYSTEMMEM surface as D3DFMT_A8R8G8B8. Dest size is the desktop
    // (windowed) or screen (fullscreen). This path is intentionally slow.
    //
    // Windowed approximation: Vitreo's Metal present path has no distinct
    // readable front buffer after Present. The presentable back buffer (last
    // Clear/Draw contents that Present blits) is the honest guest-visible
    // framebuffer, so readback uses that when available. Do not invent a
    // solid-black success when no pixels can be read.
    ComObject* dst_entry = find(dst_obj, D3D9InterfaceId::Surface);
    if (dst_entry == nullptr || !dst_entry->surface) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (state.lost) {
        m.set_return(D3DERR_DEVICELOST);
        return;
    }
    SurfaceState& dst = *dst_entry->surface;
    if (dst.pool != D3DPOOL_SYSTEMMEM || dst.locked || dst.pixels == 0 ||
        dst.format != D3DFMT_A8R8G8B8) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    std::array<u32, 4> mode{};
    if (!current_display_mode(mode) || dst.width != mode[0] ||
        dst.height != mode[1]) {
        // Fullscreen present parameters also define the screen size when the
        // display-mode resolver is unavailable or reports a different desktop.
        const u32 bb_w = chain != nullptr ? chain->desc().width
                                          : state.back_buffer_width();
        const u32 bb_h = chain != nullptr ? chain->desc().height
                                          : state.back_buffer_height();
        if (dst.width != bb_w || dst.height != bb_h) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    }

    flush_render_pass(state);
    d3d9_cs_drain();

    const auto write_bgra_rows =
        [&](const std::vector<u8>& rgba, u32 src_w, u32 src_h) -> bool {
        if (rgba.size() < static_cast<usize>(src_w) * src_h * 4u) {
            return false;
        }
        // Zero the dest first so a smaller front image on a desktop-sized
        // surface leaves clear margins (windowed capture contract).
        for (u32 y = 0; y < dst.height; ++y) {
            const GuestAddr row =
                dst.pixels + static_cast<GuestAddr>(y) * dst.pitch;
            std::vector<u8> zeros(static_cast<usize>(dst.width) * 4u, 0);
            if (m.memory().write_bytes(row, zeros.data(), zeros.size()).is_err()) {
                return false;
            }
        }
        const u32 copy_w = std::min(src_w, dst.width);
        const u32 copy_h = std::min(src_h, dst.height);
        for (u32 y = 0; y < copy_h; ++y) {
            for (u32 x = 0; x < copy_w; ++x) {
                const usize o = (static_cast<usize>(y) * src_w + x) * 4u;
                const u8 px[4] = {rgba[o + 2], rgba[o + 1], rgba[o + 0],
                                  rgba[o + 3]};
                const GuestAddr addr =
                    dst.pixels + static_cast<GuestAddr>(y) * dst.pitch +
                    x * 4u;
                if (m.memory().write_bytes(addr, px, 4).is_err()) {
                    return false;
                }
            }
        }
        return true;
    };

    // Prefer swap-chain readback of the presentable back buffer (null source).
    // Same helper GetRenderTargetData uses for back-buffer capture.
    if (chain != nullptr && chain->valid()) {
        std::vector<u8> rgba;
        if (chain->readback_texture_rgba(nullptr, rgba)) {
            const u32 src_w = chain->desc().width;
            const u32 src_h = chain->desc().height;
            if (write_bgra_rows(rgba, src_w, src_h)) {
                m.set_return(D3D_OK);
                return;
            }
        }
    }

    if (back_buffer_surface != 0) {
        ComObject* bb = find(back_buffer_surface, D3D9InterfaceId::Surface);
        if (bb != nullptr && bb->surface) {
            SurfaceState& src = *bb->surface;
            graphics::BackendTexture2D* backend =
                resolve_surface_color_backend(src);
            if (backend != nullptr && backend->valid()) {
                std::vector<u8> bgra(
                    static_cast<usize>(src.width) * src.height * 4u);
                if (backend->readback(src.level, bgra.data(), src.width * 4u)) {
                    // Backend readback is already BGRA for A8R8G8B8 surfaces;
                    // convert to RGBA for the shared writer.
                    std::vector<u8> rgba(bgra.size());
                    for (usize i = 0; i + 3 < bgra.size(); i += 4) {
                        rgba[i + 0] = bgra[i + 2];
                        rgba[i + 1] = bgra[i + 1];
                        rgba[i + 2] = bgra[i + 0];
                        rgba[i + 3] = bgra[i + 3];
                    }
                    if (write_bgra_rows(rgba, src.width, src.height)) {
                        m.set_return(D3D_OK);
                        return;
                    }
                }
                // Guest Texture2D present path: read the same backend texture
                // through the swap chain's readback helper.
                if (chain != nullptr && chain->valid()) {
                    std::vector<u8> rgba;
                    if (chain->readback_texture_rgba(backend, rgba) &&
                        write_bgra_rows(rgba, src.width, src.height)) {
                        m.set_return(D3D_OK);
                        return;
                    }
                }
            }
            if (src.pixels != 0 && src.width == dst.width &&
                src.height == dst.height &&
                (src.format == D3DFMT_A8R8G8B8 ||
                 src.format == D3DFMT_X8R8G8B8)) {
                GuestRect full{0, 0, static_cast<i32>(src.width),
                               static_cast<i32>(src.height)};
                if (copy_guest_rect(m, src, full, dst, 0, 0)) {
                    m.set_return(D3D_OK);
                    return;
                }
            }
        }
    }

    VITREO_TRACE("[d3d9] GetFrontBufferData -> E_NOTIMPL "
                 "(no readable front buffer for %ux%u)\n",
                 dst.width, dst.height);
    m.set_return(E_NOTIMPL_D3D);
}

void D3D9Manager::stretch_rect(Marshaler& m, u32 device) {
    // Learn: StretchRect — DEFAULT → DEFAULT; optional stretch + filter.
    const u32 src_obj = static_cast<u32>(m.arg(1));
    const GuestAddr src_rect_addr = m.arg_ptr(2);
    const u32 dst_obj = static_cast<u32>(m.arg(3));
    const GuestAddr dst_rect_addr = m.arg_ptr(4);
    const u32 filter = static_cast<u32>(m.arg(5));

    ComObject* src_entry = find(src_obj, D3D9InterfaceId::Surface);
    ComObject* dst_entry = find(dst_obj, D3D9InterfaceId::Surface);
    if (src_entry == nullptr || dst_entry == nullptr || !src_entry->surface ||
        !dst_entry->surface || src_obj == dst_obj) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    SurfaceState& src = *src_entry->surface;
    SurfaceState& dst = *dst_entry->surface;
    if (src.pool != D3DPOOL_DEFAULT || dst.pool != D3DPOOL_DEFAULT ||
        src.locked || dst.locked ||
        (src.usage & D3DUSAGE_DEPTHSTENCIL) != 0 ||
        (dst.usage & D3DUSAGE_DEPTHSTENCIL) != 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (filter != D3DTEXF_NONE && filter != D3DTEXF_POINT &&
        filter != D3DTEXF_LINEAR) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    GuestRect src_rect{0, 0, static_cast<i32>(src.width),
                       static_cast<i32>(src.height)};
    GuestRect dst_rect{0, 0, static_cast<i32>(dst.width),
                       static_cast<i32>(dst.height)};
    if (src_rect_addr != 0 && !read_rect(m, src_rect_addr, src_rect)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (dst_rect_addr != 0 && !read_rect(m, dst_rect_addr, dst_rect)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (!rect_inside(src_rect, src.width, src.height) ||
        !rect_inside(dst_rect, dst.width, dst.height)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    ComObject* device_entry = find(device, D3D9InterfaceId::Device);
    if (device_entry != nullptr && device_entry->device_state) {
        flush_render_pass(*device_entry->device_state);
    }

    const bool same_size = rect_width(src_rect) == rect_width(dst_rect) &&
                           rect_height(src_rect) == rect_height(dst_rect);
    // Learn / Direct3D 9 StretchRect: D3DTEXF_NONE is a same-size copy
    // filter. POINT and LINEAR may stretch. Scaling with NONE fills the
    // destination with a smeared source (loading text stripped across the
    // frame). Reject rather than stretch.
    if (!same_size && filter == D3DTEXF_NONE) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    graphics::BackendTexture2D* src_backend =
        resolve_surface_color_backend(src);
    graphics::BackendTexture2D* dst_backend =
        resolve_surface_color_backend(dst);

    // Prefer host blit for equal-size copies (full resource or sub-rect).
    // Scaled copies use a render-pass stretch: Metal blit cannot scale, and
    // guest lock memory on CreateRenderTarget surfaces is stale after GPU
    // ColorFill/Clear/Draw.
    if (src_backend != nullptr && dst_backend != nullptr &&
        m_backend != nullptr) {
        if (same_size) {
            const u32 copy_w = static_cast<u32>(rect_width(src_rect));
            const u32 copy_h = static_cast<u32>(rect_height(src_rect));
            const bool full =
                src_rect.left == 0 && src_rect.top == 0 && copy_w == src.width &&
                copy_h == src.height && dst_rect.left == 0 &&
                dst_rect.top == 0 && copy_w == dst.width &&
                copy_h == dst.height;
            const bool sliced = src_backend->desc().cube ||
                                dst_backend->desc().cube ||
                                src_backend->desc().array_size > 1 ||
                                dst_backend->desc().array_size > 1;
            // A full-resource copy would smear every cube face onto slice 0.
            if (full && !sliced &&
                m_backend->copy_texture_2d(dst_backend, src_backend)) {
                m.set_return(D3D_OK);
                return;
            }
            if (m_backend->copy_texture_2d_region(
                    dst_backend, static_cast<unsigned>(dst_rect.left),
                    static_cast<unsigned>(dst_rect.top), src_backend,
                    static_cast<unsigned>(src_rect.left),
                    static_cast<unsigned>(src_rect.top), copy_w, copy_h,
                    dst.level, dst.face, src.level, src.face)) {
                m.set_return(D3D_OK);
                return;
            }
        } else if (m_backend->stretch_texture_2d_region(
                       dst_backend, static_cast<unsigned>(dst_rect.left),
                       static_cast<unsigned>(dst_rect.top),
                       static_cast<unsigned>(rect_width(dst_rect)),
                       static_cast<unsigned>(rect_height(dst_rect)),
                       src_backend, static_cast<unsigned>(src_rect.left),
                       static_cast<unsigned>(src_rect.top),
                       static_cast<unsigned>(rect_width(src_rect)),
                       static_cast<unsigned>(rect_height(src_rect)),
                       filter == D3DTEXF_LINEAR
                           ? graphics::BackendFilter::Linear
                           : graphics::BackendFilter::Nearest,
                       dst.face, src.face)) {
            m.set_return(D3D_OK);
            return;
        }
    }

    if (src.pixels == 0 || dst.pixels == 0) {
        VITREO_TRACE("[d3d9] StretchRect -> E_NOTIMPL "
                     "(need lockable pixels or matching GPU textures) "
                     "%ux%u -> %ux%u\n",
                     rect_width(src_rect), rect_height(src_rect),
                     rect_width(dst_rect), rect_height(dst_rect));
        m.set_return(E_NOTIMPL_D3D);
        return;
    }

    if (same_size) {
        if (!copy_guest_rect(m, src, src_rect, dst, dst_rect.left,
                             dst_rect.top)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    } else if (src.format != dst.format) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    } else if (!stretch_guest_rect(m, src, src_rect, dst, dst_rect,
                                   filter == D3DTEXF_LINEAR)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    if (dst_backend != nullptr &&
        !upload_surface_level(dst_backend, m, dst, device_state_for(device),
                              texture_state_for_surface(dst))) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    m.set_return(D3D_OK);
}

void D3D9Manager::color_fill(Marshaler& m, u32 device) {
    // Learn: ColorFill — fill a DEFAULT RT / offscreen / RT-texture rect.
    const u32 surface_obj = static_cast<u32>(m.arg(1));
    const GuestAddr rect_addr = m.arg_ptr(2);
    const u32 color = static_cast<u32>(m.arg(3));

    ComObject* entry = find(surface_obj, D3D9InterfaceId::Surface);
    if (entry == nullptr || !entry->surface) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    SurfaceState& surface = *entry->surface;
    if (surface.pool != D3DPOOL_DEFAULT || surface.locked ||
        (surface.usage & D3DUSAGE_DEPTHSTENCIL) != 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    GuestRect rect{0, 0, static_cast<i32>(surface.width),
                   static_cast<i32>(surface.height)};
    if (rect_addr != 0 && !read_rect(m, rect_addr, rect)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (!rect_inside(rect, surface.width, surface.height)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    ComObject* device_entry = find(device, D3D9InterfaceId::Device);
    DeviceState* device_state =
        device_entry != nullptr ? device_entry->device_state.get() : nullptr;

    const bool full = rect.left == 0 && rect.top == 0 &&
                      rect_width(rect) == surface.width &&
                      rect_height(rect) == surface.height;
    graphics::BackendTexture2D* backend =
        resolve_surface_color_backend(surface);
    // Learn: ColorFill applies to DEFAULT RT / RT-texture / offscreen plain.
    // Prefer a host GPU clear when the surface has a clearable 2D backend
    // (full rect only — clear_render_target is whole-attachment).
    if (full && backend != nullptr && backend->valid() &&
        backend->desc().render_target) {
        D3d9ColorFillSnapshot snap{};
        snap.manager = this;
        snap.backend = backend;
        snap.color = d3dcolor_to_rgba(color);
        snap.mip_level = surface.level;
        snap.slice = surface.face;
        if (device_state != nullptr && device_state->render_pass) {
            snap.pass_to_end = std::move(device_state->render_pass);
            device_state->render_pass_color = nullptr;
        }
        (void)add_ref(surface_obj);
        snap.retained.push_back(surface_obj);
        D3d9SubmitQueue::instance().enqueue_color_fill(std::move(snap));
        m.set_return(D3D_OK);
        return;
    }

    if (device_state != nullptr) {
        flush_render_pass(*device_state);
    }

    // CPU fill for common color formats (Learn REF/SW list includes these).
    // Depth/stencil already rejected; remaining formats stay E_NOTIMPL.
    constexpr u32 D3DFMT_R5G6B5 = 23;
    constexpr u32 D3DFMT_X1R5G5B5 = 24;
    constexpr u32 D3DFMT_A1R5G5B5 = 25;
    const u32 bpp = d3d9_format_bytes_per_pixel(surface.format);
    u8 px[4]{};
    u32 px_bytes = 0;
    switch (surface.format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        pack_d3dcolor_bgra(color, px);
        px_bytes = 4;
        break;
    case D3DFMT_R5G6B5:
        pack_d3dcolor_r5g6b5(color, px);
        px_bytes = 2;
        break;
    case D3DFMT_X1R5G5B5:
        pack_d3dcolor_x1a1r5g5b5(color, false, px);
        px_bytes = 2;
        break;
    case D3DFMT_A1R5G5B5:
        pack_d3dcolor_x1a1r5g5b5(color, true, px);
        px_bytes = 2;
        break;
    default:
        break;
    }
    if (surface.pixels == 0 || bpp == 0 || px_bytes == 0 || px_bytes != bpp) {
        VITREO_TRACE("[d3d9] ColorFill -> E_NOTIMPL "
                     "(unsupported format %u or non-lockable surface)\n",
                     surface.format);
        m.set_return(E_NOTIMPL_D3D);
        return;
    }

    for (i32 y = rect.top; y < rect.bottom; ++y) {
        for (i32 x = rect.left; x < rect.right; ++x) {
            const GuestAddr addr =
                surface.pixels + static_cast<GuestAddr>(y) * surface.pitch +
                static_cast<GuestAddr>(x) * px_bytes;
            if (m.memory().write_bytes(addr, px, px_bytes).is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
    }
    if (backend != nullptr &&
        !upload_surface_level(backend, m, surface, device_state_for(device),
                              texture_state_for_surface(surface))) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    m.set_return(D3D_OK);
}

} // namespace vitreo::winapi
