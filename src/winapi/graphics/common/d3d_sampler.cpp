// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d_sampler.cpp
/// @brief Shared D3D11 / D3D12 sampler translation (d3d_sampler.hpp).

#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"

#include <algorithm>
#include <limits>

namespace vitreo::winapi {
namespace {

[[nodiscard]] graphics::BackendCompare compare_from_d3d(u32 v) {
    switch (v) {
    case 1: return graphics::BackendCompare::Never;
    case 2: return graphics::BackendCompare::Less;
    case 3: return graphics::BackendCompare::Equal;
    case 4: return graphics::BackendCompare::LessEqual;
    case 5: return graphics::BackendCompare::Greater;
    case 6: return graphics::BackendCompare::NotEqual;
    case 7: return graphics::BackendCompare::GreaterEqual;
    case 8: return graphics::BackendCompare::Always;
    default: return graphics::BackendCompare::LessEqual;
    }
}

}  // namespace

bool d3d_point_mip_tie_nudged(u32 filter) {
    // A MINIMUM_ / MAXIMUM_ filter selects its mips in the shader, exactly
    // (DxbcTranslationOptions::minmax_samplers).
    if (((filter >> 7u) & 0x3u) >= 2u) return false;
    const u32 filter_base = filter & 0x7fu;
    if (filter_base == 0x55u) return false;           // anisotropic
    if ((filter_base & 0x1u) != 0) return false;      // linear mip
    return ((filter_base >> 4) & 1u) == ((filter_base >> 2) & 1u);
}

bool d3d_point_mip_filter(u32 filter) {
    // D3D11_FILTER bit 0 is the mip filter; ANISOTROPIC (low seven bits
    // 0x55) always selects mips linearly (7.18.10).
    const u32 filter_base = filter & 0x7fu;
    return filter_base != 0x55u && (filter_base & 0x1u) == 0u;
}

void d3d_backend_sampler(const D3DSamplerFields& fields,
                         graphics::BackendSamplerDesc& backend_sampler,
                         float* shader_lod_bias) {
    backend_sampler.mipmaps_enabled = true;
    // D3D11_FILTER encodes mip/mag/min filtering independently in
    // bits 0/2/4. ANISOTROPIC (low seven bits == 0x55) uses
    // linear filtering for all three axes and additionally enables
    // the requested anisotropy in the backend sampler.
    const u32 filter_base = fields.filter & 0x7fu;
    const bool anisotropic = filter_base == 0x55u;
    backend_sampler.min_filter =
        (anisotropic || (filter_base & 0x10u) != 0)
            ? graphics::BackendFilter::Linear
            : graphics::BackendFilter::Nearest;
    backend_sampler.mag_filter =
        (anisotropic || (filter_base & 0x4u) != 0)
            ? graphics::BackendFilter::Linear
            : graphics::BackendFilter::Nearest;
    backend_sampler.mip_filter =
        (anisotropic || (filter_base & 0x1u) != 0)
            ? graphics::BackendFilter::Linear
            : graphics::BackendFilter::Nearest;
    backend_sampler.comparison_enabled = (fields.filter & 0x180u) == 0x80u;
    // MINIMUM_ / MAXIMUM_ (D3D11.3 5.9.4.5.6): the translated shader fetches
    // the filter's texels one at a time, at their centres and explicit mips,
    // and reduces them (DxbcTranslationOptions::minmax_samplers); the Metal
    // sampler point-samples with the state's address modes and border. Its
    // LOD range and anisotropy are applied by the shader (clamping the mips
    // it asks for here would move them).
    const bool reducing = ((fields.filter >> 7u) & 0x3u) >= 2u;
    if (reducing) {
        backend_sampler.min_filter = graphics::BackendFilter::Nearest;
        backend_sampler.mag_filter = graphics::BackendFilter::Nearest;
        backend_sampler.mip_filter = graphics::BackendFilter::Nearest;
    }
    backend_sampler.comparison = compare_from_d3d(fields.comparison_func);
    const auto address_mode = [](u32 mode) {
        switch (mode) {
        case 1u: return graphics::BackendAddressMode::Wrap;
        case 2u: return graphics::BackendAddressMode::Mirror;
        case 4u: return graphics::BackendAddressMode::Border;
        case 5u: return graphics::BackendAddressMode::MirrorOnce;
        default: return graphics::BackendAddressMode::Clamp;
        }
    };
    backend_sampler.address_u = address_mode(fields.address_u);
    backend_sampler.address_v = address_mode(fields.address_v);
    backend_sampler.address_w = address_mode(fields.address_w);
    // Metal's three border colours carry the three D3D ones exactly. Any
    // other colour binds transparent black here and is completed in the
    // shader by the border twin (border_twin_mask). A comparison filter sees
    // only the red channel: 1 binds white, and any red but 0 and 1 binds 0
    // and is compared in the shader.
    const auto& border = fields.border;
    const bool comparison = (fields.filter & 0x180u) == 0x80u;
    if (comparison) {
        backend_sampler.border_color = border[0] == 1.0f ? 0xffffffffu : 0u;
    } else if (border[0] == 1.0f && border[1] == 1.0f &&
               border[2] == 1.0f && border[3] == 1.0f) {
        backend_sampler.border_color = 0xffffffffu;
    } else {
        backend_sampler.border_color = 0u;
    }
    // D3D11_MIN_MAXANISOTROPY is 0 (CreateSamplerState accepts it for an
    // anisotropic filter); Metal's maxAnisotropy starts at 1, the same
    // footprint.
    backend_sampler.max_anisotropy =
        anisotropic && !reducing ? std::max(1u, fields.max_anisotropy) : 1u;
    // D3D11.3 7.18.11: MinLOD takes precedence over a smaller MaxLOD.
    float lod_min = fields.min_lod;
    float lod_max = std::max(fields.max_lod, lod_min);
    float lod_bias = fields.mip_lod_bias;
    // The point-mip tie nudge (d3d_point_mip_tie_nudged) shifts both clamps
    // with the bias the shader adds.
    if (d3d_point_mip_tie_nudged(fields.filter)) {
        lod_min += graphics::kPointMipTieNudge;
        lod_max += graphics::kPointMipTieNudge;
        lod_bias += graphics::kPointMipTieNudge;
    }
    if (reducing) {
        lod_min = 0.0f;
        lod_max = std::numeric_limits<float>::max();
    }
    backend_sampler.lod_min_clamp = lod_min;
    backend_sampler.lod_max_clamp = lod_max;
    backend_sampler.lod_bias = lod_bias;
    if (shader_lod_bias != nullptr) *shader_lod_bias = lod_bias;
}

}  // namespace vitreo::winapi
