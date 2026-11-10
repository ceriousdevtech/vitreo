#pragma once
/// @file d3d_sampler.hpp
/// @brief The D3D11 / D3D12 sampler state as the backend binds it.
///
/// D3D11_SAMPLER_DESC and D3D12_SAMPLER_DESC carry the same members with the
/// same encodings (the D3D12 filter, address-mode and comparison enums number
/// their values as D3D11's do), so both frontends translate a sampler here --
/// including the measured rules for point-mip ties and border colours Metal
/// cannot express.

#include "graphics/contracts/backend_device.hpp"

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::winapi {

/// The members of a D3D11_SAMPLER_DESC / D3D12_SAMPLER_DESC.
struct D3DSamplerFields {
    u32 filter{0};
    u32 address_u{0};
    u32 address_v{0};
    u32 address_w{0};
    float mip_lod_bias{0.0f};
    u32 max_anisotropy{0};
    u32 comparison_func{0};
    std::array<float, 4> border{};
    float min_lod{0.0f};
    float max_lod{0.0f};
    /// D3D12_SAMPLER_FLAGS. 0 for a D3D11 sampler. Bit 0 is
    /// UINT_BORDER_COLOR (border[] holds uint bits, not floats). Bit 1 is
    /// NON_NORMALIZED_COORDINATES.
    u32 flags{0};
};

/// D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR.
inline constexpr u32 kD3DSamplerFlagUintBorder = 0x1u;
/// D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES.
inline constexpr u32 kD3DSamplerFlagNonNormalized = 0x2u;

/// A point-mip sampler whose min and mag filters match: Apple GPUs move to the
/// next mip only at a fraction of 0.515625, so its bias gets
/// graphics::kPointMipTieNudge for an implicit LOD's .5 tie to round up as
/// D3D11.3 7.18.10 requires (see dxbc_to_spirv.hpp, kPointMipNudgeMaskOffset).
[[nodiscard]] bool d3d_point_mip_tie_nudged(u32 filter);

/// The mip filter is POINT (bit 0 clear, not ANISOTROPIC).
[[nodiscard]] bool d3d_point_mip_filter(u32 filter);

/// The Metal sampler for @p fields; @p shader_lod_bias receives the MipLODBias
/// the translated shader adds (Metal samplers carry none).
///
/// D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES is not a Metal field here
/// (BackendSamplerDesc has none). The sampler-state record carries it and
/// the shader divides by the view size. A UINT border of (0,0,0,0),
/// (0,0,0,1) or (1,1,1,1) is one of Metal's three colours; any other UINT
/// border is not honoured (d3d_sampler_honoured).
void d3d_backend_sampler(const D3DSamplerFields& fields,
                         graphics::BackendSamplerDesc& out,
                         float* shader_lod_bias);

/// True when every flag and border in @p fields can be applied. An arbitrary
/// UINT border colour cannot: Metal has three borders and the float twin
/// blend is not an integer border.
[[nodiscard]] bool d3d_sampler_honoured(const D3DSamplerFields& fields);

/// The non-normalized flag is unset, or the sampler meets the D3D12
/// restrictions (min == mag, point mip, no comparison, no anisotropy, zero
/// LODs and bias, U and V clamp or border).
[[nodiscard]] bool d3d_sampler_non_normalized_legal(const D3DSamplerFields& fields);

/// True when the border is not one of the colours the Metal sampler bound
/// for @p fields already returns (D3D11.3 7.18.9 / 7.18.9.1). Opaque black
/// (0,0,0,1) is one of those: Metal's opaque black gives an A8 view alpha 0.
[[nodiscard]] bool d3d_sampler_needs_border_twin(const D3DSamplerFields& fields);

/// kSamplerStateRecordWords for @p fields. @p shader_lod_bias is the value
/// d3d_backend_sampler reported (the nudge included, when it applied).
void d3d_write_sampler_state_record(const D3DSamplerFields& fields,
                                    float shader_lod_bias, u32* words);

/// D3D12_DECODE_FILTER_REDUCTION(filter) is MINIMUM (2) or MAXIMUM (3)
/// (d3d12.h: D3D12_FILTER_REDUCTION_TYPE_SHIFT 7, mask 3; D3D11 the same
/// bits). The record's kSamplerStateMinMax is set for such a filter.
[[nodiscard]] inline bool d3d_minmax_reduction_filter(u32 filter) {
    return ((filter >> 7u) & 0x3u) >= 2u;
}

}  // namespace vitreo::winapi
