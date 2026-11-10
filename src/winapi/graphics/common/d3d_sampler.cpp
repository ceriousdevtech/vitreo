// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d_sampler.cpp
/// @brief Shared D3D11 / D3D12 sampler translation (d3d_sampler.hpp).

#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include "graphics/shaders/sampler_features.hpp"

#include <algorithm>
#include <cstring>
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

[[nodiscard]] bool uses_border(const D3DSamplerFields& fields) {
    return fields.address_u == 4u || fields.address_v == 4u ||
           fields.address_w == 4u;
}

[[nodiscard]] bool comparison_filter(u32 filter) {
    return (filter & 0x180u) == 0x80u;
}

[[nodiscard]] bool reducing_filter(u32 filter) {
    return ((filter >> 7u) & 0x3u) >= 2u;
}

[[nodiscard]] u32 float_bits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}

[[nodiscard]] u32 border_bits(const D3DSamplerFields& fields, u32 channel) {
    u32 bits = 0;
    std::memcpy(&bits, &fields.border[channel], 4);
    return bits;
}

/// (0,0,0,0), (0,0,0,1) or (1,1,1,1) as uints: Metal's three borders.
[[nodiscard]] bool metal_uint_border(const u32 colour[4]) {
    const bool rgb0 = colour[0] == 0u && colour[1] == 0u && colour[2] == 0u;
    if (rgb0 && (colour[3] == 0u || colour[3] == 1u)) return true;
    return colour[0] == 1u && colour[1] == 1u && colour[2] == 1u &&
           colour[3] == 1u;
}

/// The power of two at or below MaxAnisotropy (WARP; 0 filters as 1).
[[nodiscard]] u32 effective_anisotropy(u32 max_anisotropy) {
    u32 aniso = 1u;
    while (aniso * 2u <= max_anisotropy) aniso *= 2u;
    return aniso;
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
    // and is compared in the shader. A UINT border's bits are integers;
    // (0,0,0,1) is Metal's opaque black and (1,1,1,1) its opaque white.
    const auto& border = fields.border;
    const bool comparison = comparison_filter(fields.filter);
    const bool uint_border = (fields.flags & kD3DSamplerFlagUintBorder) != 0;
    if (uint_border) {
        u32 u[4] = {};
        for (u32 c = 0; c < 4; ++c) u[c] = border_bits(fields, c);
        if (comparison) {
            backend_sampler.border_color = u[0] == 1u ? 0xffffffffu : 0u;
        } else if (u[0] == 1u && u[1] == 1u && u[2] == 1u && u[3] == 1u) {
            backend_sampler.border_color = 0xffffffffu;
        } else if (u[0] == 0u && u[1] == 0u && u[2] == 0u && u[3] == 1u) {
            backend_sampler.border_color = 0xFF000000u;
        } else {
            backend_sampler.border_color = 0u;
        }
    } else if (comparison) {
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
    // with the bias the shader adds. A non-normalized sampler's LODs and
    // bias are required to be 0; the nudge would move them.
    const bool non_normalized =
        (fields.flags & kD3DSamplerFlagNonNormalized) != 0;
    if (!non_normalized && d3d_point_mip_tie_nudged(fields.filter)) {
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

bool d3d_sampler_non_normalized_legal(const D3DSamplerFields& fields) {
    if ((fields.flags & kD3DSamplerFlagNonNormalized) == 0u) return true;
    // The same restrictions static-sampler validation already applies
    // (D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES): MIN == MAG, MIP
    // point, neither comparison nor anisotropic, zero LODs and bias, U and
    // V clamp or border.
    const u32 f = fields.filter;
    const bool filter_ok = !comparison_filter(f) && (f & 0x40u) == 0u &&
                           ((f >> 4) & 3u) == ((f >> 2) & 3u) && (f & 3u) == 0u;
    if (!filter_ok) return false;
    if (fields.min_lod != 0.0f || fields.max_lod != 0.0f ||
        fields.mip_lod_bias != 0.0f) {
        return false;
    }
    const auto clamp_or_border = [](u32 mode) {
        return mode == 3u || mode == 4u;
    };
    return clamp_or_border(fields.address_u) &&
           clamp_or_border(fields.address_v);
}

bool d3d_sampler_needs_border_twin(const D3DSamplerFields& fields) {
    if (!uses_border(fields)) return false;
    if ((fields.flags & kD3DSamplerFlagUintBorder) != 0u) {
        // Integer borders that are not Metal's three have no float blend.
        // Comparison uses the red alone; 0 and 1 are Metal borders.
        u32 u[4] = {};
        for (u32 c = 0; c < 4; ++c) u[c] = border_bits(fields, c);
        if (comparison_filter(fields.filter)) {
            return u[0] != 0u && u[0] != 1u;
        }
        return !metal_uint_border(u);
    }
    const float* c = fields.border.data();
    if (comparison_filter(fields.filter)) {
        return c[0] != 0.0f && c[0] != 1.0f;
    }
    const bool metal = (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f &&
                        c[3] == 0.0f) ||
                       (c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f &&
                        c[3] == 1.0f);
    return !metal;
}

bool d3d_sampler_honoured(const D3DSamplerFields& fields) {
    if ((fields.flags & ~0x3u) != 0u) return false;
    if ((fields.flags & kD3DSamplerFlagUintBorder) == 0u) return true;
    if (!uses_border(fields)) return true;
    // A comparison border is one red. Any other UINT colour is not a Metal
    // border and is not a float the twin blend can turn into an integer.
    if (comparison_filter(fields.filter)) return true;
    u32 u[4] = {};
    for (u32 c = 0; c < 4; ++c) u[c] = border_bits(fields, c);
    return metal_uint_border(u);
}

void d3d_write_sampler_state_record(const D3DSamplerFields& fields,
                                    float shader_lod_bias, u32* words) {
    for (u32 i = 0; i < graphics::kSamplerStateRecordWords; ++i) words[i] = 0u;
    words[graphics::kSamplerStateBiasWord] = float_bits(shader_lod_bias);
    u32 flags = 0;
    const bool non_normalized =
        (fields.flags & kD3DSamplerFlagNonNormalized) != 0u;
    if (d3d_point_mip_filter(fields.filter)) {
        flags |= graphics::kSamplerStatePointMip;
    }
    if (!non_normalized && d3d_point_mip_tie_nudged(fields.filter)) {
        flags |= graphics::kSamplerStateBiasNudged;
    }
    if (non_normalized) {
        graphics::enable_sampler_feature(graphics::SamplerFeature::NonNormalized);
        graphics::compile_profile::note(graphics::compile_profile::SamplerRecordNonNormalized);
        flags |= graphics::kSamplerStateNonNormalized;
    }
    if (d3d_sampler_needs_border_twin(fields)) {
        flags |= graphics::kSamplerStateCustomBorder;
        // Before this flag can be uploaded or read by a draw. Cached records
        // therefore also imply that the feature was enabled already.
        graphics::enable_sampler_feature(graphics::SamplerFeature::CustomBorder);
        graphics::compile_profile::note(graphics::compile_profile::SamplerRecordBorderTwin);
        if ((fields.flags & kD3DSamplerFlagUintBorder) != 0u &&
            comparison_filter(fields.filter)) {
            const float red = static_cast<float>(border_bits(fields, 0));
            words[graphics::kSamplerStateBorderWord] = float_bits(red);
            words[graphics::kSamplerStateBorderWord + 1] =
                float_bits(static_cast<float>(fields.comparison_func));
        } else if (comparison_filter(fields.filter)) {
            words[graphics::kSamplerStateBorderWord] =
                float_bits(fields.border[0]);
            words[graphics::kSamplerStateBorderWord + 1] =
                float_bits(static_cast<float>(fields.comparison_func));
        } else {
            for (u32 c = 0; c < 4; ++c) {
                words[graphics::kSamplerStateBorderWord + c] =
                    float_bits(fields.border[c]);
            }
        }
    }
    // Gather ignores the reduction and a comparison filter cannot be one.
    // The shader still checks both; the flag is only set for a real one.
    if (reducing_filter(fields.filter) &&
        !comparison_filter(fields.filter)) {
        flags |= graphics::kSamplerStateMinMax;
    }
    // Filter, anisotropy and the LOD clamps for every sampler: the min/max
    // reduction reads them, and so does an LOD computed in the shader
    // (VX_TEX LodGrad, D3D11.3 7.18.11: maxAniso, MinLOD, MaxLOD).
    flags |= (fields.filter & graphics::kSamplerStateFilterMask)
             << graphics::kSamplerStateFilterShift;
    flags |= (effective_anisotropy(fields.max_anisotropy) &
              graphics::kSamplerStateAnisoMask)
             << graphics::kSamplerStateAnisoShift;
    flags |= (std::min(16u, std::max(1u, fields.max_anisotropy)) &
              graphics::kSamplerStateAnisoMask)
             << graphics::kSamplerStateFlagsMaxAnisoShift;
    words[graphics::kSamplerStateMinLodWord] = float_bits(fields.min_lod);
    words[graphics::kSamplerStateMaxLodWord] = float_bits(fields.max_lod);
    words[graphics::kSamplerStateFlagsWord] = flags;
    // The whole filter state, for the sampler feedback writes.
    const u32 aniso = std::min(fields.max_anisotropy, 16u);
    words[graphics::kSamplerStateFilterStateWord] =
        (fields.filter & graphics::kSamplerStateFilterMask) |
        ((fields.address_u & graphics::kSamplerStateAddressMask)
         << graphics::kSamplerStateAddressUShift) |
        ((fields.address_v & graphics::kSamplerStateAddressMask)
         << graphics::kSamplerStateAddressVShift) |
        ((fields.address_w & graphics::kSamplerStateAddressMask)
         << graphics::kSamplerStateAddressWShift) |
        (aniso << graphics::kSamplerStateMaxAnisoShift);
    words[graphics::kSamplerStateFullMinLodWord] = float_bits(fields.min_lod);
    words[graphics::kSamplerStateFullMaxLodWord] = float_bits(fields.max_lod);
}

}  // namespace vitreo::winapi
