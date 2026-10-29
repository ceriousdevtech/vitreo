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
};

/// A point-mip sampler whose min and mag filters match: Apple GPUs move to the
/// next mip only at a fraction of 0.515625, so its bias gets
/// graphics::kPointMipTieNudge for an implicit LOD's .5 tie to round up as
/// D3D11.3 7.18.10 requires (see dxbc_to_spirv.hpp, kPointMipNudgeMaskOffset).
[[nodiscard]] bool d3d_point_mip_tie_nudged(u32 filter);

/// The mip filter is POINT (bit 0 clear, not ANISOTROPIC).
[[nodiscard]] bool d3d_point_mip_filter(u32 filter);

/// The Metal sampler for @p fields; @p shader_lod_bias receives the MipLODBias
/// the translated shader adds (Metal samplers carry none).
void d3d_backend_sampler(const D3DSamplerFields& fields,
                         graphics::BackendSamplerDesc& out,
                         float* shader_lod_bias);

}  // namespace vitreo::winapi
