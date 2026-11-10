// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"

#include <array>
#include <limits>

namespace vitreo::winapi::d3d11 {

enum class VertexComponentKind : u8 { Float, UInt, SInt, UNorm, SNorm };

struct VertexFormatInfo {
    u32 dxgi_format;
    graphics::BackendVertexFormat backend_format;
    u8 byte_size;
    VertexComponentKind component_kind;
};

// Fully typed, non-depth DXGI formats with an exact Metal vertex-fetch form.
// Unsupported formats stay unsupported: treating one as Float4 would change
// both its byte width and conversion contract.
inline constexpr std::array kVertexFormats{
    VertexFormatInfo{2, graphics::BackendVertexFormat::Float4, 16, VertexComponentKind::Float},
    VertexFormatInfo{3, graphics::BackendVertexFormat::UInt4, 16, VertexComponentKind::UInt},
    VertexFormatInfo{4, graphics::BackendVertexFormat::SInt4, 16, VertexComponentKind::SInt},
    VertexFormatInfo{6, graphics::BackendVertexFormat::Float3, 12, VertexComponentKind::Float},
    VertexFormatInfo{7, graphics::BackendVertexFormat::UInt3, 12, VertexComponentKind::UInt},
    VertexFormatInfo{8, graphics::BackendVertexFormat::SInt3, 12, VertexComponentKind::SInt},
    VertexFormatInfo{10, graphics::BackendVertexFormat::Half4, 8, VertexComponentKind::Float},
    VertexFormatInfo{11, graphics::BackendVertexFormat::UShort4Normalized, 8, VertexComponentKind::UNorm},
    VertexFormatInfo{12, graphics::BackendVertexFormat::UShort4, 8, VertexComponentKind::UInt},
    VertexFormatInfo{13, graphics::BackendVertexFormat::Short4Normalized, 8, VertexComponentKind::SNorm},
    VertexFormatInfo{14, graphics::BackendVertexFormat::Short4, 8, VertexComponentKind::SInt},
    VertexFormatInfo{16, graphics::BackendVertexFormat::Float2, 8, VertexComponentKind::Float},
    VertexFormatInfo{17, graphics::BackendVertexFormat::UInt2, 8, VertexComponentKind::UInt},
    VertexFormatInfo{18, graphics::BackendVertexFormat::SInt2, 8, VertexComponentKind::SInt},
    VertexFormatInfo{24, graphics::BackendVertexFormat::UInt1010102Normalized, 4, VertexComponentKind::UNorm},
    // R10G10B10A2_UINT is an IA-required format at every D3D11 feature level
    // (Learn, "Format support for Direct3D feature level 11.0 hardware").
    // Fetched normalized and restored to integers by the vertex shader.
    VertexFormatInfo{25, graphics::BackendVertexFormat::UInt1010102, 4, VertexComponentKind::UInt},
    VertexFormatInfo{26, graphics::BackendVertexFormat::FloatRG11B10, 4, VertexComponentKind::Float},
    VertexFormatInfo{28, graphics::BackendVertexFormat::UByte4Normalized, 4, VertexComponentKind::UNorm},
    VertexFormatInfo{30, graphics::BackendVertexFormat::UByte4, 4, VertexComponentKind::UInt},
    VertexFormatInfo{31, graphics::BackendVertexFormat::SByte4Normalized, 4, VertexComponentKind::SNorm},
    VertexFormatInfo{32, graphics::BackendVertexFormat::SByte4, 4, VertexComponentKind::SInt},
    VertexFormatInfo{34, graphics::BackendVertexFormat::Half2, 4, VertexComponentKind::Float},
    VertexFormatInfo{35, graphics::BackendVertexFormat::UShort2Normalized, 4, VertexComponentKind::UNorm},
    VertexFormatInfo{36, graphics::BackendVertexFormat::UShort2, 4, VertexComponentKind::UInt},
    VertexFormatInfo{37, graphics::BackendVertexFormat::Short2Normalized, 4, VertexComponentKind::SNorm},
    VertexFormatInfo{38, graphics::BackendVertexFormat::Short2, 4, VertexComponentKind::SInt},
    VertexFormatInfo{41, graphics::BackendVertexFormat::Float1, 4, VertexComponentKind::Float},
    VertexFormatInfo{42, graphics::BackendVertexFormat::UInt1, 4, VertexComponentKind::UInt},
    VertexFormatInfo{43, graphics::BackendVertexFormat::SInt1, 4, VertexComponentKind::SInt},
    VertexFormatInfo{49, graphics::BackendVertexFormat::UByte2Normalized, 2, VertexComponentKind::UNorm},
    VertexFormatInfo{50, graphics::BackendVertexFormat::UByte2, 2, VertexComponentKind::UInt},
    VertexFormatInfo{51, graphics::BackendVertexFormat::SByte2Normalized, 2, VertexComponentKind::SNorm},
    VertexFormatInfo{52, graphics::BackendVertexFormat::SByte2, 2, VertexComponentKind::SInt},
    VertexFormatInfo{54, graphics::BackendVertexFormat::Half1, 2, VertexComponentKind::Float},
    // dxgiformat.h: 55 is D16_UNORM (no IA support); R16_UNORM/UINT/SNORM/
    // SINT are 56..59. These four rows were one number low, so R16_UNORM
    // fetched as integers, R16_SINT was refused and D16 was accepted.
    VertexFormatInfo{56, graphics::BackendVertexFormat::UShort1Normalized, 2, VertexComponentKind::UNorm},
    VertexFormatInfo{57, graphics::BackendVertexFormat::UShort1, 2, VertexComponentKind::UInt},
    VertexFormatInfo{58, graphics::BackendVertexFormat::Short1Normalized, 2, VertexComponentKind::SNorm},
    VertexFormatInfo{59, graphics::BackendVertexFormat::Short1, 2, VertexComponentKind::SInt},
    VertexFormatInfo{61, graphics::BackendVertexFormat::UByte1Normalized, 1, VertexComponentKind::UNorm},
    VertexFormatInfo{62, graphics::BackendVertexFormat::UByte1, 1, VertexComponentKind::UInt},
    VertexFormatInfo{63, graphics::BackendVertexFormat::SByte1Normalized, 1, VertexComponentKind::SNorm},
    VertexFormatInfo{64, graphics::BackendVertexFormat::SByte1, 1, VertexComponentKind::SInt},
    // No R9G9B9E5_SHAREDEXP (67): Learn's FL11 format-support table gives it no
    // IA vertex-buffer support, so an input layout naming it is invalid.
    VertexFormatInfo{87, graphics::BackendVertexFormat::UByte4NormalizedBGRA, 4, VertexComponentKind::UNorm},
};

[[nodiscard]] constexpr const VertexFormatInfo*
vertex_format_info(u32 dxgi_format) noexcept {
    for (const auto& info : kVertexFormats) {
        if (info.dxgi_format == dxgi_format) return &info;
    }
    return nullptr;
}

[[nodiscard]] constexpr graphics::BackendVertexFormat
vertex_format_from_dxgi(u32 dxgi_format) noexcept {
    const auto* info = vertex_format_info(dxgi_format);
    return info != nullptr ? info->backend_format
                           : graphics::BackendVertexFormat::Invalid;
}

[[nodiscard]] constexpr unsigned
vertex_format_stride(graphics::BackendVertexFormat format) noexcept {
    for (const auto& info : kVertexFormats) {
        if (info.backend_format == format) return info.byte_size;
    }
    return format == graphics::BackendVertexFormat::Dec3N ? 4u : 0u;
}

[[nodiscard]] constexpr unsigned vertex_format_stride_from_dxgi(
    u32 dxgi_format) noexcept {
    const auto* info = vertex_format_info(dxgi_format);
    return info != nullptr ? info->byte_size : 0u;
}

[[nodiscard]] constexpr graphics::BackendVertexComponentType
vertex_storage_component_type(u32 dxgi_format) noexcept {
    const auto* info = vertex_format_info(dxgi_format);
    if (info == nullptr) return graphics::BackendVertexComponentType::Float;
    if (info->component_kind == VertexComponentKind::UInt) {
        return graphics::BackendVertexComponentType::UInt;
    }
    if (info->component_kind == VertexComponentKind::SInt) {
        return graphics::BackendVertexComponentType::SInt;
    }
    return graphics::BackendVertexComponentType::Float;
}

/// Channels of an IA vertex format (dxgiformat.h: the R/G/B/A of each
/// name); 0 for a format kVertexFormats does not list.
[[nodiscard]] constexpr u32 vertex_format_components(u32 dxgi_format) noexcept {
    switch (dxgi_format) {
    case 2: case 3: case 4: case 10: case 11: case 12: case 13: case 14:
    case 24: case 25: case 28: case 30: case 31: case 32: case 87:
        return 4;
    case 6: case 7: case 8: case 26:
        return 3;
    case 16: case 17: case 18: case 34: case 35: case 36: case 37: case 38:
    case 49: case 50: case 51: case 52:
        return 2;
    case 41: case 42: case 43: case 54: case 56: case 57: case 58: case 59:
    case 61: case 62: case 63: case 64:
        return 1;
    default:
        return 0;
    }
}

/// The element size of an IA format that has no Metal vertex format and is
/// read only by vertex pulling (graphics::VertexPullKind::UNormB5G6R5 ..
/// UNormBGRX8): B5G6R5, B5G5R5A1 and B4G4R4A4 (2 bytes), B8G8R8X8 (4);
/// 0 for any other format.
[[nodiscard]] constexpr u32 pulled_only_vertex_format_bytes(
    u32 dxgi_format) noexcept {
    switch (dxgi_format) {
    case 85: case 86: case 115: return 2;
    case 88: return 4;
    default: return 0;
    }
}

/// The vertex-pull form of an IA format (graphics::VertexPullElement kind,
/// components, bits, storage type); false for a format the IA cannot read.
[[nodiscard]] constexpr bool vertex_pull_format(
    u32 dxgi_format, graphics::VertexPullElement& out) noexcept {
    using graphics::VertexPullKind;
    if (pulled_only_vertex_format_bytes(dxgi_format) != 0) {
        out.storage_type = 3u;
        out.components = dxgi_format == 85 || dxgi_format == 88 ? 3u : 4u;
        out.bits = 0;
        out.kind = dxgi_format == 85   ? VertexPullKind::UNormB5G6R5
                   : dxgi_format == 86 ? VertexPullKind::UNormB5G5R5A1
                   : dxgi_format == 115 ? VertexPullKind::UNormB4G4R4A4
                                        : VertexPullKind::UNormBGRX8;
        return true;
    }
    const auto* info = vertex_format_info(dxgi_format);
    const u32 components = vertex_format_components(dxgi_format);
    if (info == nullptr || components == 0) return false;
    out.components = static_cast<u8>(components);
    out.bits = static_cast<u8>(info->byte_size * 8u / components);
    out.storage_type =
        info->component_kind == VertexComponentKind::UInt   ? 1u
        : info->component_kind == VertexComponentKind::SInt ? 2u
                                                            : 3u;
    switch (info->backend_format) {
    case graphics::BackendVertexFormat::UInt1010102Normalized:
        out.kind = VertexPullKind::UNorm1010102;
        return true;
    case graphics::BackendVertexFormat::UInt1010102:
        out.kind = VertexPullKind::UInt1010102;
        return true;
    case graphics::BackendVertexFormat::FloatRG11B10:
        out.kind = VertexPullKind::Float111110;
        return true;
    case graphics::BackendVertexFormat::UByte4NormalizedBGRA:
        out.kind = VertexPullKind::UNormBGRA8;
        return true;
    default:
        break;
    }
    switch (info->component_kind) {
    case VertexComponentKind::Float:
        out.kind = out.bits == 16 ? VertexPullKind::Half : VertexPullKind::Float;
        break;
    case VertexComponentKind::UInt: out.kind = VertexPullKind::UInt; break;
    case VertexComponentKind::SInt: out.kind = VertexPullKind::SInt; break;
    case VertexComponentKind::UNorm: out.kind = VertexPullKind::UNorm; break;
    case VertexComponentKind::SNorm: out.kind = VertexPullKind::SNorm; break;
    }
    return true;
}

/// The pull form of the IA format whose fetch form is @p format.
[[nodiscard]] constexpr bool vertex_pull_format_of_backend(
    graphics::BackendVertexFormat format,
    graphics::VertexPullElement& out) noexcept {
    for (const auto& info : kVertexFormats) {
        if (info.backend_format == format) {
            return vertex_pull_format(info.dxgi_format, out);
        }
    }
    return false;
}

/// Every IA format has a pull form (checked against the table).
[[nodiscard]] constexpr bool every_vertex_format_pulls() noexcept {
    for (const auto& info : kVertexFormats) {
        graphics::VertexPullElement element{};
        if (!vertex_pull_format(info.dxgi_format, element)) return false;
        if (element.bits * element.components != info.byte_size * 8u &&
            element.kind != graphics::VertexPullKind::Float111110 &&
            element.kind != graphics::VertexPullKind::UNorm1010102 &&
            element.kind != graphics::VertexPullKind::UInt1010102) {
            return false;
        }
    }
    return true;
}
static_assert(every_vertex_format_pulls());

inline constexpr u32 kAppendAlignedElement = 0xffffffffu;

struct VertexLayoutPackingState {
    std::array<u32, 32> append_cursor{};
    std::array<u32, 32> slot_extent{};
};

[[nodiscard]] constexpr bool resolve_vertex_element_offset(
    u32 input_slot, u32 requested_offset, u32 byte_size,
    VertexLayoutPackingState& packing, u32& resolved_offset) noexcept {
    if (input_slot >= packing.append_cursor.size() || byte_size == 0) {
        return false;
    }
    resolved_offset = requested_offset == kAppendAlignedElement
        ? packing.append_cursor[input_slot] : requested_offset;
    if (resolved_offset > std::numeric_limits<u32>::max() - byte_size) {
        return false;
    }
    const u32 end = resolved_offset + byte_size;
    packing.append_cursor[input_slot] = end;
    if (end > packing.slot_extent[input_slot]) {
        packing.slot_extent[input_slot] = end;
    }
    return true;
}

} // namespace vitreo::winapi::d3d11
