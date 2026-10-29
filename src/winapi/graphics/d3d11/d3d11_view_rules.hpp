// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_view_rules.hpp
/// @brief The view-format rules the ID3D11Device Create*View creators share.
///
/// Private to the D3D11 frontend: d3d11_device_srv.cpp, d3d11_device_uav.cpp
/// and d3d11_device_rtv_dsv.cpp include it. These were file-local to
/// d3d11_device_methods.cpp before the view creators moved out of it.

#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"

#include <cstdio>

namespace vitreo::winapi {

/// D3D11.3 functional spec §5.3 (the DDI view structs): a view's Format is
/// "fully qualified" -- never a TYPELESS family head -- and must support the
/// use the view puts it to. That support is what CheckFormatSupport reports
/// (Learn D3D11_FORMAT_SUPPORT), so every creator asks the one capability
/// table: a texture SRV needs SHADER_LOAD or SHADER_SAMPLE, a buffer SRV
/// BUFFER, an RTV RENDER_TARGET, a UAV TYPED_UNORDERED_ACCESS_VIEW and a DSV
/// DEPTH_STENCIL. Windows refuses every miss with E_INVALIDARG -- D formats
/// as SRVs, sRGB UAVs, typeless, sRGB, block-compressed and depth typed
/// buffers alike (measured, d3d11_a03_views_probe).
///
/// The refusal matches what this device's CheckFormatSupport reports. Where
/// Windows (the Windows answer) supports the use and this implementation
/// does not, the gap is also reported once as UNIMPLEMENTED.
[[nodiscard]] inline bool view_format_supports(
    u32 format, u32 any_of, const d3d11::FormatCapsContext& context) {
    if (format == 0 || dxgi_format_is_typeless(format)) return false;
    const auto answer = d3d11::check_format_support(format, context);
    if (answer.hr == d3d11::S_OK && (answer.mask & any_of) != 0) return true;
    if ((d3d11::unhonoured_format_support(format, context) & any_of) != 0) {
        char text[96];
        std::snprintf(text, sizeof(text),
                      "a view of DXGI format %u for use 0x%x (Windows "
                      "supports it)", format, any_of);
        d3d11_unimplemented_once("Create*View", text);
    }
    return false;
}

/// Bytes in one element of a typed buffer view of @p format. R32G32B32 is a
/// required Buffer<> format (Learn, "Format support for Direct3D Feature
/// Level 11.0 hardware": BUFFER) with no texel form in Metal, so it is sized
/// here rather than by the texture-buffer table.
[[nodiscard]] inline u32 typed_buffer_view_element_bytes(u32 format) noexcept {
    if (format >= 6u && format <= 8u) return 12u;
    return d3d11::typed_buffer_element_bytes(format);
}

/// Measured (d3d11_a03_views_probe "cross-device",
/// d3d11_a03_cross_device_probe): creating a view of a resource that belongs
/// to another device fails with E_OUTOFMEMORY, not E_INVALIDARG, for all
/// four kinds and buffers alike -- while a validate-only call (ppView NULL)
/// judges the description alone and answers S_FALSE.
inline constexpr u32 VIEW_OF_FOREIGN_RESOURCE = 0x8007000Eu;

} // namespace vitreo::winapi
