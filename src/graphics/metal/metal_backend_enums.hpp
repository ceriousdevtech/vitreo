// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_enums.hpp
/// @brief Shared-contract enumerations to their Metal equivalents
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"

#include <limits>

namespace vitreo::graphics {

/// @p base followed by a D3D12 Shader4ComponentMapping (BackendTextureView
/// Desc::component_mapping): output component i reads source component
/// (mapping >> 3i) & 7 of @p base, or constant 0 / 1.
[[nodiscard]] inline MTLTextureSwizzleChannels metal_component_mapping(
    MTLTextureSwizzleChannels base, u16 mapping) {
    if (mapping == 0) return base;
    const MTLTextureSwizzle source[4] = {base.red, base.green, base.blue,
                                         base.alpha};
    MTLTextureSwizzle out[4];
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned v = (mapping >> (3u * i)) & 7u;
        out[i] = v < 4u   ? source[v]
                 : v == 4u ? MTLTextureSwizzleZero
                           : MTLTextureSwizzleOne;
    }
    return MTLTextureSwizzleChannelsMake(out[0], out[1], out[2], out[3]);
}

// -----------------------------------------------------------------------------
// Shared-contract enumerations to Metal
// -----------------------------------------------------------------------------

inline MTLPrimitiveType metal_primitive(BackendPrimitiveTopology topology) {
    switch (topology) {
    case BackendPrimitiveTopology::PointList:     return MTLPrimitiveTypePoint;
    case BackendPrimitiveTopology::LineList:      return MTLPrimitiveTypeLine;
    case BackendPrimitiveTopology::LineStrip:     return MTLPrimitiveTypeLineStrip;
    case BackendPrimitiveTopology::TriangleList:  return MTLPrimitiveTypeTriangle;
    case BackendPrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

inline MTLBlendFactor metal_blend_factor(BackendBlendFactor factor) {
    switch (factor) {
    case BackendBlendFactor::Zero: return MTLBlendFactorZero;
    case BackendBlendFactor::One:  return MTLBlendFactorOne;
    case BackendBlendFactor::SourceColor: return MTLBlendFactorSourceColor;
    case BackendBlendFactor::OneMinusSourceColor:
        return MTLBlendFactorOneMinusSourceColor;
    case BackendBlendFactor::SourceAlpha: return MTLBlendFactorSourceAlpha;
    case BackendBlendFactor::OneMinusSourceAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case BackendBlendFactor::DestinationColor:
        return MTLBlendFactorDestinationColor;
    case BackendBlendFactor::OneMinusDestinationColor:
        return MTLBlendFactorOneMinusDestinationColor;
    case BackendBlendFactor::DestinationAlpha:
        return MTLBlendFactorDestinationAlpha;
    case BackendBlendFactor::OneMinusDestinationAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case BackendBlendFactor::SourceAlphaSaturated:
        return MTLBlendFactorSourceAlphaSaturated;
    case BackendBlendFactor::BlendColor:
        return MTLBlendFactorBlendColor;
    case BackendBlendFactor::OneMinusBlendColor:
        return MTLBlendFactorOneMinusBlendColor;
    case BackendBlendFactor::Source1Color: return MTLBlendFactorSource1Color;
    case BackendBlendFactor::OneMinusSource1Color:
        return MTLBlendFactorOneMinusSource1Color;
    case BackendBlendFactor::Source1Alpha: return MTLBlendFactorSource1Alpha;
    case BackendBlendFactor::OneMinusSource1Alpha:
        return MTLBlendFactorOneMinusSource1Alpha;
    }
    return MTLBlendFactorOne;
}

inline MTLBlendOperation metal_blend_operation(BackendBlendOperation operation) {
    switch (operation) {
    case BackendBlendOperation::Add: return MTLBlendOperationAdd;
    case BackendBlendOperation::Subtract: return MTLBlendOperationSubtract;
    case BackendBlendOperation::ReverseSubtract:
        return MTLBlendOperationReverseSubtract;
    case BackendBlendOperation::Min: return MTLBlendOperationMin;
    case BackendBlendOperation::Max: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

inline MTLCompareFunction metal_compare(BackendCompare compare) {
    switch (compare) {
    case BackendCompare::Never:        return MTLCompareFunctionNever;
    case BackendCompare::Less:         return MTLCompareFunctionLess;
    case BackendCompare::Equal:        return MTLCompareFunctionEqual;
    case BackendCompare::LessEqual:    return MTLCompareFunctionLessEqual;
    case BackendCompare::Greater:      return MTLCompareFunctionGreater;
    case BackendCompare::NotEqual:     return MTLCompareFunctionNotEqual;
    case BackendCompare::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case BackendCompare::Always:       return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionLessEqual;
}

inline MTLStencilOperation metal_stencil_operation(BackendStencilOperation op) {
    switch (op) {
    case BackendStencilOperation::Keep: return MTLStencilOperationKeep;
    case BackendStencilOperation::Zero: return MTLStencilOperationZero;
    case BackendStencilOperation::Replace: return MTLStencilOperationReplace;
    case BackendStencilOperation::IncrementClamp:
        return MTLStencilOperationIncrementClamp;
    case BackendStencilOperation::DecrementClamp:
        return MTLStencilOperationDecrementClamp;
    case BackendStencilOperation::Invert: return MTLStencilOperationInvert;
    case BackendStencilOperation::IncrementWrap:
        return MTLStencilOperationIncrementWrap;
    case BackendStencilOperation::DecrementWrap:
        return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

inline MTLSamplerAddressMode metal_address_mode(BackendAddressMode mode) {
    switch (mode) {
    case BackendAddressMode::Wrap:   return MTLSamplerAddressModeRepeat;
    case BackendAddressMode::Mirror: return MTLSamplerAddressModeMirrorRepeat;
    case BackendAddressMode::Clamp:  return MTLSamplerAddressModeClampToEdge;
    case BackendAddressMode::Border: return MTLSamplerAddressModeClampToBorderColor;
    case BackendAddressMode::MirrorOnce:
        return MTLSamplerAddressModeMirrorClampToEdge;
    }
    return MTLSamplerAddressModeRepeat;
}

/// Map guest D3DCOLOR (`0xAARRGGBB`) to the nearest Metal border color.
/// Apple GPUs only expose TransparentBlack, OpaqueBlack, and OpaqueWhite —
/// arbitrary RGBA borders are quantized by Euclidean distance in normalized
/// RGBA space. The full guest value remains in BackendSamplerDesc for
/// GetSamplerState round-trip at the D3D9 layer.
inline MTLSamplerBorderColor metal_border_color(u32 d3d_color) {
    const float a = static_cast<float>((d3d_color >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((d3d_color >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((d3d_color >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(d3d_color & 0xFFu) / 255.0f;

    struct Candidate {
        float r, g, b, a;
        MTLSamplerBorderColor metal;
    };
    constexpr Candidate kCandidates[] = {
        {0.f, 0.f, 0.f, 0.f, MTLSamplerBorderColorTransparentBlack},
        {0.f, 0.f, 0.f, 1.f, MTLSamplerBorderColorOpaqueBlack},
        {1.f, 1.f, 1.f, 1.f, MTLSamplerBorderColorOpaqueWhite},
    };

    float best_dist = std::numeric_limits<float>::max();
    MTLSamplerBorderColor best = MTLSamplerBorderColorTransparentBlack;
    for (const Candidate& c : kCandidates) {
        const float dr = r - c.r;
        const float dg = g - c.g;
        const float db = b - c.b;
        const float da = a - c.a;
        const float dist = dr * dr + dg * dg + db * db + da * da;
        if (dist < best_dist) {
            best_dist = dist;
            best = c.metal;
        }
    }
    return best;
}

/// 2-bit cache tag matching metal_border_color quantization order.
inline u32 metal_border_color_tag(u32 d3d_color) {
    switch (metal_border_color(d3d_color)) {
    case MTLSamplerBorderColorOpaqueBlack: return 1;
    case MTLSamplerBorderColorOpaqueWhite: return 2;
    case MTLSamplerBorderColorTransparentBlack:
    default:
        return 0;
    }
}

} // namespace vitreo::graphics
