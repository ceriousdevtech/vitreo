// SPDX-License-Identifier: LGPL-2.1-only
/// @file indirect_count.hpp
/// @brief ExecuteIndirect's count buffer applied on the GPU timeline.
///
/// Learn (ID3D12GraphicsCommandList::ExecuteIndirect): with a count buffer,
/// the number of commands executed is min(*pCountBuffer, MaxCommandCount),
/// the count read when the GPU reaches the command. The CPU cannot know it
/// without waiting for the GPU. vitreo_indirect_count, recorded in command
/// order, copies every one of the MaxCommandCount argument blocks into a
/// packed scratch buffer and zeroes those at or past the count; each block
/// is then issued as an ordinary GPU-sourced draw or dispatch, and a zeroed
/// one draws (or dispatches) nothing.

#pragma once

#include <vitreo/common.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// Constants of vitreo_indirect_count (inline constant buffer, index 0).
struct IndirectCountParams {
    /// The signature's ByteStride / 4.
    u32 stride_words{0};
    /// Words of one argument block: 4 DRAW, 5 DRAW_INDEXED, 3 DISPATCH.
    u32 command_words{0};
    u32 max_count{0};
    /// 0: no count buffer (every block is copied).
    u32 has_count{0};
};
static_assert(sizeof(IndirectCountParams) == 16);

inline constexpr unsigned INDIRECT_COUNT_SOURCE_INDEX = 1;
inline constexpr unsigned INDIRECT_COUNT_COUNT_INDEX = 2;
inline constexpr unsigned INDIRECT_COUNT_DEST_INDEX = 3;
inline constexpr unsigned INDIRECT_COUNT_THREADS = 64;
inline constexpr const char* kIndirectCountEntryPoint = "vitreo_indirect_count";

/// The C++ model of vitreo_indirect_count.
[[nodiscard]] inline std::vector<u32> indirect_count_model(
    const IndirectCountParams& p, const std::vector<u32>& source, u32 count) {
    std::vector<u32> out(static_cast<usize>(p.max_count) * p.command_words, 0u);
    const u32 n = p.has_count != 0 ? std::min(count, p.max_count) : p.max_count;
    for (u32 t = 0; t < n; ++t) {
        for (u32 w = 0; w < p.command_words; ++w) {
            const usize at = static_cast<usize>(t) * p.stride_words + w;
            out[static_cast<usize>(t) * p.command_words + w] =
                at < source.size() ? source[at] : 0u;
        }
    }
    return out;
}

[[nodiscard]] inline std::string indirect_count_msl() {
    char source[2048];
    std::snprintf(source, sizeof(source), R"(#include <metal_stdlib>
using namespace metal;

struct IndirectCountParams {
    uint stride_words;
    uint command_words;
    uint max_count;
    uint has_count;
};

kernel void %s(
    constant IndirectCountParams& p [[buffer(0)]],
    device const uint* source [[buffer(%u)]],
    device const uint* count [[buffer(%u)]],
    device uint* dest [[buffer(%u)]],
    uint t [[thread_position_in_grid]])
{
    if (t >= p.max_count) return;
    const uint n = p.has_count != 0u ? min(count[0], p.max_count) : p.max_count;
    for (uint w = 0u; w < p.command_words; ++w) {
        dest[t * p.command_words + w] =
            t < n ? source[t * p.stride_words + w] : 0u;
    }
}
)",
                  kIndirectCountEntryPoint, INDIRECT_COUNT_SOURCE_INDEX,
                  INDIRECT_COUNT_COUNT_INDEX, INDIRECT_COUNT_DEST_INDEX);
    return source;
}

}  // namespace vitreo::graphics
