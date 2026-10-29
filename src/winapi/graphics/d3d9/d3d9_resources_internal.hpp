// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9_resources_internal.hpp
/// @brief Private surface shared by the D3D9 resource parts:
///        d3d9_resources.cpp, d3d9_texture_create.cpp,
///        d3d9_texture_autogen.cpp, d3d9_texture.cpp,
///        d3d9_cube_texture.cpp and d3d9_volume.cpp.
///
/// Private to the D3D9 frontend. These were file-local helpers in an anonymous
/// namespace at the top of d3d9_resources.cpp before that file was split by
/// COM interface; they are here so the parts can still see them, not to
/// publish them. Each consumer writes `using namespace d3d9_resources_detail;`
/// inside `namespace vitreo::winapi`, which gives the same lookup the
/// anonymous namespace gave.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi::d3d9_resources_detail {

inline constexpr u32 D3DUSAGE_RENDERTARGET = 0x00000001;
inline constexpr u32 D3DUSAGE_DYNAMIC = 0x00000200;
inline constexpr u32 D3DUSAGE_AUTOGENMIPMAP = 0x00000400;

// Learn: D3DTEXTUREFILTERTYPE — NONE is invalid for SetAutoGenFilterType.
inline constexpr u32 D3DTEXF_NONE = 0;
inline constexpr u32 D3DTEXF_POINT = 1;
inline constexpr u32 D3DTEXF_LINEAR = 2;

[[nodiscard]] inline bool is_supported_autogen_filter(u32 filter) noexcept {
    // Driver MINF caps gate filter support; Vitreo always exposes POINT and
    // LINEAR. Learn excludes D3DTEXF_NONE from autogeneration.
    if (filter == D3DTEXF_NONE) return false;
    return filter == D3DTEXF_POINT || filter == D3DTEXF_LINEAR;
}

[[nodiscard]] inline bool d3d9_texture_lifetime_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_TEXTURE_LIFETIME_DIAG") != nullptr;
    return enabled;
}

[[nodiscard]] inline bool d3d9_lock_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_LOCK_DIAG") != nullptr;
    return enabled;
}

[[nodiscard]] inline bool graphics_perf_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_GRAPHICS_PERF_DIAG") != nullptr;
    return enabled;
}

inline void report_slow_resource_call(const char* operation,
                                      std::chrono::steady_clock::time_point started,
                                      u32 width, u32 height) {
    if (!graphics_perf_diag_enabled()) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    if (elapsed < std::chrono::milliseconds(5)) return;
    std::fprintf(stderr, "[graphics-perf] d3d9 %s %ux%u %.3f ms\n",
                 operation, width, height,
                 static_cast<double>(elapsed.count()) / 1000.0);
}

[[nodiscard]] inline bool is_large_texture(u32 width, u32 height) noexcept {
    return static_cast<u64>(width) * static_cast<u64>(height) >= 1000000u;
}

[[nodiscard]] inline const u8* resolve_current_palette_entries(
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

[[nodiscard]] inline u32 dxt_block_bytes(u32 format) noexcept {
    switch (format) {
    case 0x31545844u: // DXT1
        return 8;
    case 0x32545844u: // DXT2
    case 0x33545844u: // DXT3
    case 0x34545844u: // DXT4
    case 0x35545844u: // DXT5
        return 16;
    default:
        return 0;
    }
}

/// Resolve the guest pointer returned through D3DLOCKED_RECT. D3D9 retains
/// the full-level pitch for a subrectangle and offsets pBits to its first
/// texel (or compressed block).
[[nodiscard]] inline bool locked_rect_view(Marshaler& m,
                                           const D3D9Manager::SurfaceState& surface,
                                           GuestAddr rect_in, u32 flags,
                                           GuestAddr& bits_out) {
    bits_out = surface.pixels;
    if (rect_in == 0) return true;
    if ((flags & D3DLOCK_DISCARD) != 0) return false;

    std::array<i32, 4> rect{};
    if (m.memory()
            .read_bytes(rect_in, reinterpret_cast<Byte*>(rect.data()),
                        sizeof(rect))
            .is_err()) {
        return false;
    }
    const i32 left = rect[0];
    const i32 top = rect[1];
    const i32 right = rect[2];
    const i32 bottom = rect[3];
    if (d3d9_lock_diag_enabled()) {
        std::fprintf(stderr,
                     "[d3d9-lock-diag] %ux%u fmt=0x%08x "
                     "rect=(%d,%d)-(%d,%d) flags=0x%x\n",
                     surface.width, surface.height, surface.format, left, top,
                     right, bottom, flags);
    }
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        right > static_cast<i32>(surface.width) ||
        bottom > static_cast<i32>(surface.height)) {
        return false;
    }

    usize offset = 0;
    const u32 bytes_per_pixel = d3d9_format_bytes_per_pixel(surface.format);
    if (bytes_per_pixel != 0) {
        offset = static_cast<usize>(top) * surface.pitch +
                 static_cast<usize>(left) * bytes_per_pixel;
    } else {
        const u32 block_bytes = dxt_block_bytes(surface.format);
        if (block_bytes == 0) return false;
        // DXT rectangles address whole 4x4 blocks. A right/bottom edge may
        // be partial only when it is the mip-level boundary.
        if ((left & 3) != 0 || (top & 3) != 0 ||
            ((right & 3) != 0 && right != static_cast<i32>(surface.width)) ||
            ((bottom & 3) != 0 &&
             bottom != static_cast<i32>(surface.height))) {
            return false;
        }
        offset = static_cast<usize>(top / 4) * surface.pitch +
                 static_cast<usize>(left / 4) * block_bytes;
    }
    if (offset >= surface.pixel_bytes) return false;
    bits_out = surface.pixels + offset;
    return true;
}

/// Write a D3DSURFACE_DESC for @p surface.
inline void write_surface_desc(std::array<u32, 8>& desc,
                               const D3D9Manager::SurfaceState& surface) {
    desc[0] = surface.format;
    desc[1] = D3DRTYPE_SURFACE;
    desc[2] = surface.usage;
    desc[3] = surface.pool;
    desc[4] = surface.multi_sample_type;
    desc[5] = surface.multi_sample_quality;
    desc[6] = surface.width;
    desc[7] = surface.height;
}

} // namespace vitreo::winapi::d3d9_resources_detail
