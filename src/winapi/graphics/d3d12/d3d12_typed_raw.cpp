// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_typed_raw.cpp
/// @brief DXGI element layouts for typed buffer views read as raw memory.
///
/// Learn (DXGI_FORMAT): components are named in order from the least
/// significant bits ("R8G8B8A8": R in bits 0-7), a B-first name stores B
/// lowest, and the X of B8G8R8X8 is unused. The view reads them back in
/// R, G, B, A order; a component the name lacks reads 0, alpha 1 (D3D11.3
/// 19.1.3.3). graphics::typed_raw_channel encodes one component.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"

namespace vitreo::winapi::d3d12 {

namespace {

using graphics::TypedRawKind;
using graphics::typed_raw_channel;

struct Layout {
    u32 element_bytes{0};
    std::array<u32, 4> channels{};  ///< R, G, B, A
    bool shared_exponent{false};
};

/// DXGI_FORMAT_A4B4G4R4_UNORM (Learn DXGI_FORMAT: 191, added in the
/// Windows 10 SDK 10.0.20348 dxgiformat.h; the mingw-w64 headers the
/// constants are generated from predate it).
constexpr u32 kDxgiFormatA4B4G4R4Unorm = 191u;

/// @p count components of @p width bits each, R first, all of @p kind.
Layout uniform(u32 count, u32 width, TypedRawKind kind) {
    Layout layout{count * width / 8u, {}};
    for (u32 c = 0; c < count; ++c) {
        layout.channels[c] = typed_raw_channel(c * width, width, kind);
    }
    return layout;
}

Layout packed(u32 bytes, std::array<std::pair<u32, u32>, 4> rgba,
              TypedRawKind kind) {
    Layout layout{bytes, {}};
    for (u32 c = 0; c < 4; ++c) {
        if (rgba[c].second != 0) {
            layout.channels[c] =
                typed_raw_channel(rgba[c].first, rgba[c].second, kind);
        }
    }
    return layout;
}

bool layout_of(u32 format, Layout& out) {
    using K = TypedRawKind;
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: out = uniform(4, 32, K::Float); return true;
    case DXGI_FORMAT_R32G32B32A32_UINT: out = uniform(4, 32, K::Uint); return true;
    case DXGI_FORMAT_R32G32B32A32_SINT: out = uniform(4, 32, K::Sint); return true;
    case DXGI_FORMAT_R32G32B32_FLOAT: out = uniform(3, 32, K::Float); return true;
    case DXGI_FORMAT_R32G32B32_UINT: out = uniform(3, 32, K::Uint); return true;
    case DXGI_FORMAT_R32G32B32_SINT: out = uniform(3, 32, K::Sint); return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: out = uniform(4, 16, K::Float16); return true;
    case DXGI_FORMAT_R16G16B16A16_UNORM: out = uniform(4, 16, K::Unorm); return true;
    case DXGI_FORMAT_R16G16B16A16_UINT: out = uniform(4, 16, K::Uint); return true;
    case DXGI_FORMAT_R16G16B16A16_SNORM: out = uniform(4, 16, K::Snorm); return true;
    case DXGI_FORMAT_R16G16B16A16_SINT: out = uniform(4, 16, K::Sint); return true;
    case DXGI_FORMAT_R32G32_FLOAT: out = uniform(2, 32, K::Float); return true;
    case DXGI_FORMAT_R32G32_UINT: out = uniform(2, 32, K::Uint); return true;
    case DXGI_FORMAT_R32G32_SINT: out = uniform(2, 32, K::Sint); return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        out = packed(4, {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}, K::Unorm);
        return true;
    case DXGI_FORMAT_R10G10B10A2_UINT:
        out = packed(4, {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}, K::Uint);
        return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        out = Layout{4,
                     {typed_raw_channel(0, 11, K::Float11),
                      typed_raw_channel(11, 11, K::Float11),
                      typed_raw_channel(22, 10, K::Float10), 0}};
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: out = uniform(4, 8, K::Unorm); return true;
    case DXGI_FORMAT_R8G8B8A8_UINT: out = uniform(4, 8, K::Uint); return true;
    case DXGI_FORMAT_R8G8B8A8_SNORM: out = uniform(4, 8, K::Snorm); return true;
    case DXGI_FORMAT_R8G8B8A8_SINT: out = uniform(4, 8, K::Sint); return true;
    case DXGI_FORMAT_R16G16_FLOAT: out = uniform(2, 16, K::Float16); return true;
    case DXGI_FORMAT_R16G16_UNORM: out = uniform(2, 16, K::Unorm); return true;
    case DXGI_FORMAT_R16G16_UINT: out = uniform(2, 16, K::Uint); return true;
    case DXGI_FORMAT_R16G16_SNORM: out = uniform(2, 16, K::Snorm); return true;
    case DXGI_FORMAT_R16G16_SINT: out = uniform(2, 16, K::Sint); return true;
    case DXGI_FORMAT_R32_FLOAT: out = uniform(1, 32, K::Float); return true;
    case DXGI_FORMAT_R32_UINT: out = uniform(1, 32, K::Uint); return true;
    case DXGI_FORMAT_R32_SINT: out = uniform(1, 32, K::Sint); return true;
    case DXGI_FORMAT_R8G8_UNORM: out = uniform(2, 8, K::Unorm); return true;
    case DXGI_FORMAT_R8G8_UINT: out = uniform(2, 8, K::Uint); return true;
    case DXGI_FORMAT_R8G8_SNORM: out = uniform(2, 8, K::Snorm); return true;
    case DXGI_FORMAT_R8G8_SINT: out = uniform(2, 8, K::Sint); return true;
    case DXGI_FORMAT_R16_FLOAT: out = uniform(1, 16, K::Float16); return true;
    case DXGI_FORMAT_R16_UNORM: out = uniform(1, 16, K::Unorm); return true;
    case DXGI_FORMAT_R16_UINT: out = uniform(1, 16, K::Uint); return true;
    case DXGI_FORMAT_R16_SNORM: out = uniform(1, 16, K::Snorm); return true;
    case DXGI_FORMAT_R16_SINT: out = uniform(1, 16, K::Sint); return true;
    case DXGI_FORMAT_R8_UNORM: out = uniform(1, 8, K::Unorm); return true;
    case DXGI_FORMAT_R8_UINT: out = uniform(1, 8, K::Uint); return true;
    case DXGI_FORMAT_R8_SNORM: out = uniform(1, 8, K::Snorm); return true;
    case DXGI_FORMAT_R8_SINT: out = uniform(1, 8, K::Sint); return true;
    case DXGI_FORMAT_A8_UNORM:
        out = Layout{1, {0, 0, 0, typed_raw_channel(0, 8, K::Unorm)}};
        return true;
    case DXGI_FORMAT_B5G6R5_UNORM:
        out = packed(2, {{{11, 5}, {5, 6}, {0, 5}, {0, 0}}}, K::Unorm);
        return true;
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        out = packed(2, {{{10, 5}, {5, 5}, {0, 5}, {15, 1}}}, K::Unorm);
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        out = packed(4, {{{16, 8}, {8, 8}, {0, 8}, {24, 8}}}, K::Unorm);
        return true;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        out = packed(4, {{{16, 8}, {8, 8}, {0, 8}, {0, 0}}}, K::Unorm);
        return true;
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        out = packed(2, {{{8, 4}, {4, 4}, {0, 4}, {12, 4}}}, K::Unorm);
        return true;
    case kDxgiFormatA4B4G4R4Unorm:
        out = packed(2, {{{12, 4}, {8, 4}, {4, 4}, {0, 4}}}, K::Unorm);
        return true;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        // Three 9-bit mantissas and the shared exponent in bits 27-31
        // (graphics::kTypedRawSharedExponent).
        out = Layout{4,
                     {typed_raw_channel(0, 9, K::Float),
                      typed_raw_channel(9, 9, K::Float),
                      typed_raw_channel(18, 9, K::Float), 0},
                     true};
        return true;
    default:
        return false;
    }
}

}  // namespace

u32 typed_raw_element_bytes(u32 dxgi_format) {
    Layout layout;
    if (!layout_of(dxgi_format, layout)) return 0;
    // A format both tables know must agree on its size.
    const u32 element = dxgi_element_bytes(dxgi_format);
    if (element != 0 && element != layout.element_bytes) return 0;
    return layout.element_bytes;
}

bool typed_raw_layout(u32 dxgi_format, u64 device_address, u32 num_elements,
                      u32 component_mapping, std::array<u32, 7>& words) {
    Layout layout;
    if (!layout_of(dxgi_format, layout) ||
        typed_raw_element_bytes(dxgi_format) == 0) {
        return false;
    }
    words = {static_cast<u32>(device_address),
             static_cast<u32>(device_address >> 32),
             num_elements,
             layout.element_bytes,
             layout.channels[0] | (layout.channels[1] << 16),
             layout.channels[2] | (layout.channels[3] << 16),
             (component_mapping & 0x1FFFu) |
                 (layout.shared_exponent ? graphics::kTypedRawSharedExponent
                                         : 0u)};
    return true;
}

}  // namespace vitreo::winapi::d3d12
