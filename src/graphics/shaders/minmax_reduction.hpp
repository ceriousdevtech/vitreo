// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file minmax_reduction.hpp
/// @brief Whether the process has a MINIMUM_ / MAXIMUM_ sampler, which
///        decides how descriptor-table shaders compile their reduction.
///
/// A descriptor-table translation cannot know at creation whether a sampler
/// it reads will reduce: every sample, sample_b, sample_l and sample_d
/// carries the reduction behind the sampler-state record's flag
/// (dxbc_to_spirv_minmax_sample.cpp). The translator also ANDs that branch
/// with the specialization constant kMinMaxReductionSpecName, which
/// spirv_to_msl turns into the preprocessor switch kMinMaxReductionMacro
/// (default 0) and fences the reduction branches with, so the MSL is the
/// same either way and only the Metal compile differs:
///  - while no MINIMUM_ / MAXIMUM_ sampler exists, a library compiles with
///    the switch 0 and the reduction is not in the program at all (the
///    record flag can only be set by such a sampler);
///  - the first one (D3D12 CreateSampler / CreateSampler2, a root
///    signature's static sampler) calls enable_minmax_reduction() before its
///    descriptor or root signature exists, so no draw can see the record
///    flag first. From then on libraries compile with the switch 1, and
///    every pipeline object built earlier from a marked library rebuilds
///    its state from the same MSL before its next draw or dispatch
///    (metal_backend_pipeline.mm, metal_backend_compute.mm,
///    metal_mesh_pipeline.mm).
/// The switch only ever goes from 0 to 1.

#include "graphics/shaders/sampler_features.hpp"

namespace vitreo::graphics {

[[nodiscard]] inline bool minmax_reduction_enabled() noexcept {
    return (enabled_sampler_features() & static_cast<unsigned>(SamplerFeature::MinMax)) != 0;
}

/// Irreversible. Returns true for the call that turned it on.
inline bool enable_minmax_reduction() noexcept {
    return enable_sampler_feature(SamplerFeature::MinMax);
}

/// The MSL carries the switch (spirv_to_msl's fenced reduction).
[[nodiscard]] inline bool msl_has_minmax_switch(const std::string& msl) noexcept {
    return msl.find(kMinMaxReductionMacro) != std::string::npos;
}

/// spirv_to_msl's rewrite of the specialization constant into the switch
/// and its fences (msl_minmax_fence.cpp). No-op for MSL without it.
void fence_minmax_reduction(std::string& msl);

}  // namespace vitreo::graphics
