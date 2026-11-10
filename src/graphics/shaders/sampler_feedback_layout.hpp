// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file sampler_feedback_layout.hpp
/// @brief Vitreo's opaque storage of a D3D12 sampler feedback map
/// (DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE 189 /
/// SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE 190), shared by the D3D12
/// frontend, which creates, clears and transcodes it, and the DXIL lowering
/// of WriteSamplerFeedback* (dxil_lower_feedback.cpp), which writes it.
///
/// DirectX-Specs "Sampler Feedback": "The actual underlying storage of
/// feedback maps may vary across GPUs and is opaque to the application";
/// only the decoded R8_UINT form is a contract. Vitreo keeps one storage for
/// both formats: per array slice and per mip m of the paired texture, one
/// bit per mip-m region (a region is the map's SamplerFeedbackMipRegion in
/// texels of that mip), set when a feedback write requested mip m there.
///  - MIP_REGION_USED decodes mip m's bits directly (255 / 0).
///  - MIN_MIP decodes mip-0 region (x, y) as the least m whose bit
///    (x >> m, y >> m) is set, 0xFF when none: a mip-m request shows in
///    the 2^m x 2^m group of decoded texels its mip-m region covers. The
///    spec permits exactly that ("the "01" may appear in the top left group
///    of decoded texels, or in the top left texel only"), and it is what the
///    RTX 4080 SUPER decodes (tools/oracle_probe/d3d12_a41_feedback_probe,
///    winoracle-gpu); WARP keeps per-texel values instead.
///
/// The backing is a buffer bound as a raw UAV. Word layout:
///  [0, kHeaderWords)  the header below, written at creation and never
///                     changed (a clear or an encode rewrites only data);
///  [kHeaderWords, ..) slice s, mip m: words_per_slice * s + mip offset,
///                     bit b = y * grid_width + x (word b / 32, bit b % 32).
/// The paired texture's dimensions are the map's own (a feedback map is
/// created with the paired resource's width, height, mips and array size).

#include <vitreo/common.hpp>

namespace vitreo::graphics::sampler_feedback {

inline constexpr u32 kHeaderWords = 80;
inline constexpr u32 kWidth = 0;
inline constexpr u32 kHeight = 1;
inline constexpr u32 kMipCount = 2;
inline constexpr u32 kSliceCount = 3;
inline constexpr u32 kLog2RegionWidth = 4;
inline constexpr u32 kLog2RegionHeight = 5;
inline constexpr u32 kWordsPerSlice = 6;
inline constexpr u32 kFormat = 7;
/// Mip m's record: words kMipRecords + 4m .. + 3 = (data word offset within
/// a slice, grid width, grid height, 0). 15 mips (D3D12_REQ_MIP_LEVELS) fit.
inline constexpr u32 kMipRecords = 16;
inline constexpr u32 kMaxMips = 15;
static_assert(kMipRecords + 4 * kMaxMips <= kHeaderWords);

struct Geometry {
    u32 width{0};
    u32 height{0};
    u32 mips{0};
    u32 slices{0};
    u32 log2_region_width{0};
    u32 log2_region_height{0};
    u32 words_per_slice{0};
    u32 mip_offset[kMaxMips]{};
    u32 grid_width[kMaxMips]{};
    u32 grid_height[kMaxMips]{};
    [[nodiscard]] u64 total_words() const {
        return kHeaderWords + static_cast<u64>(words_per_slice) * slices;
    }
};

[[nodiscard]] inline u32 log2_exact(u32 v) {
    u32 r = 0;
    while ((1u << r) < v) ++r;
    return r;
}

/// The layout of a map; @p region_width / @p region_height are powers of two.
[[nodiscard]] inline Geometry geometry(u32 width, u32 height, u32 mips, u32 slices,
                                       u32 region_width, u32 region_height) {
    Geometry g{};
    g.width = width;
    g.height = height;
    g.mips = mips > kMaxMips ? kMaxMips : mips;
    g.slices = slices;
    g.log2_region_width = log2_exact(region_width);
    g.log2_region_height = log2_exact(region_height);
    u32 offset = 0;
    for (u32 m = 0; m < g.mips; ++m) {
        const u32 w = (width >> m) != 0 ? width >> m : 1u;
        const u32 h = (height >> m) != 0 ? height >> m : 1u;
        g.grid_width[m] = (w + region_width - 1) >> g.log2_region_width;
        g.grid_height[m] = (h + region_height - 1) >> g.log2_region_height;
        g.mip_offset[m] = offset;
        offset += (g.grid_width[m] * g.grid_height[m] + 31u) / 32u;
    }
    g.words_per_slice = offset;
    return g;
}

/// The header words of @p g (kHeaderWords of them) for a map of @p format.
inline void write_header(const Geometry& g, u32 format, u32* words) {
    for (u32 i = 0; i < kHeaderWords; ++i) words[i] = 0;
    words[kWidth] = g.width;
    words[kHeight] = g.height;
    words[kMipCount] = g.mips;
    words[kSliceCount] = g.slices;
    words[kLog2RegionWidth] = g.log2_region_width;
    words[kLog2RegionHeight] = g.log2_region_height;
    words[kWordsPerSlice] = g.words_per_slice;
    words[kFormat] = format;
    for (u32 m = 0; m < g.mips; ++m) {
        words[kMipRecords + 4 * m] = g.mip_offset[m];
        words[kMipRecords + 4 * m + 1] = g.grid_width[m];
        words[kMipRecords + 4 * m + 2] = g.grid_height[m];
    }
}

}  // namespace vitreo::graphics::sampler_feedback
