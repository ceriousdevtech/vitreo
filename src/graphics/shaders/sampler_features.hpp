// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <atomic>
#include <string>

namespace vitreo::graphics {

// Compiler features are monotonic within a process. A producer enables a
// feature before publishing records that need it; pipelines compiled without
// that feature must rebuild before using those records.
enum class SamplerFeature : unsigned {
    MinMax = 1u << 0,
    CustomBorder = 1u << 1,
    NonNormalized = 1u << 2,
};

inline constexpr const char* kMinMaxReductionMacro = "VITREO_MINMAX_REDUCTION";
inline constexpr const char* kMinMaxReductionSpecName = "vitreoMinMaxReduction";
inline constexpr unsigned kMinMaxReductionSpecId = 4242u;
inline constexpr const char* kCustomBorderMacro = "VITREO_CUSTOM_BORDER";
inline constexpr const char* kCustomBorderSpecName = "vitreoCustomBorder";
inline constexpr unsigned kCustomBorderSpecId = kMinMaxReductionSpecId + 1u;
inline constexpr const char* kNonNormalizedMacro = "VITREO_NON_NORMALIZED";
inline constexpr const char* kNonNormalizedSpecName = "vitreoNonNormalized";
inline constexpr unsigned kNonNormalizedSpecId = kCustomBorderSpecId + 1u;

namespace detail {
inline std::atomic<unsigned>& sampler_feature_flags() {
    static std::atomic<unsigned> flags{0};
    return flags;
}
}

[[nodiscard]] inline unsigned enabled_sampler_features() noexcept {
    return detail::sampler_feature_flags().load(std::memory_order_acquire);
}

// True only for the first producer of this feature; never clears other bits.
inline bool enable_sampler_feature(SamplerFeature feature) noexcept {
    const auto bit = static_cast<unsigned>(feature);
    return (detail::sampler_feature_flags().fetch_or(bit, std::memory_order_acq_rel) & bit) == 0;
}

[[nodiscard]] inline unsigned sampler_features_in_msl(const std::string& msl) noexcept {
    unsigned features = 0;
    if (msl.find(kMinMaxReductionMacro) != std::string::npos)
        features |= static_cast<unsigned>(SamplerFeature::MinMax);
    if (msl.find(kCustomBorderMacro) != std::string::npos)
        features |= static_cast<unsigned>(SamplerFeature::CustomBorder);
    if (msl.find(kNonNormalizedMacro) != std::string::npos)
        features |= static_cast<unsigned>(SamplerFeature::NonNormalized);
    return features;
}

void fence_sampler_features(std::string& msl);

} // namespace vitreo::graphics
