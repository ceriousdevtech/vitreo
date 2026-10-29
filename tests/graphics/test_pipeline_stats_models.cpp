// SPDX-License-Identifier: LGPL-2.1-only
//
// The C++ models of the PIPELINE_STATISTICS counting (pipeline_stats.hpp)
// against Windows: every expected value is a line of
// tools/oracle_probe/d3d11_pstats_probe.expected (Windows, WARP; x86 = x64).
// The kernels are the same arithmetic, which test_pipeline_stats_gpu.cpp
// holds against these values on a real Metal device.

#include <gtest/gtest.h>

#include "graphics/shaders/pipeline_stats.hpp"
#include "graphics/shaders/stream_out_repack.hpp"

#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kCut16 = 0xFFFFu;
constexpr u32 kCut32 = 0xFFFFFFFFu;

/// VSInvocations and IAPrimitives of one instance of an indexed draw.
std::pair<u64, u64> indexed(const std::vector<u32>& indices, u32 cut, u32 vpp,
                            PrimitiveAssembly assembly) {
    const auto keys =
        assemble_primitive_vertices(indices, true, cut, 0, vpp, assembly);
    return {stats_vertex_invocations(keys, vpp), keys.size() / vpp};
}

std::vector<u32> zeros(u32 n) { return std::vector<u32>(n, 0u); }

}  // namespace

// [vs] and [vsbatch]: WARP shades each distinct vertex of a batch of
// 512 / (vertices per primitive) assembled primitives once.
TEST(PipelineStatsModel, VertexInvocationsFollowWarpsReuseBatches) {
    using A = PrimitiveAssembly;
    // "list of 12 indices, all 0": IAP 4, VS 1.
    EXPECT_EQ(indexed(zeros(12), kCut16, 3, A::List),
              (std::pair<u64, u64>{1, 4}));
    // "17x17 grid, 512 triangles, row-major": VS 328 (289 distinct).
    std::vector<u32> grid;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 a = y * 17 + x, b = a + 1, c = a + 17, d = c + 1;
            grid.insert(grid.end(), {a, b, c, c, b, d});
        }
    }
    EXPECT_EQ(indexed(grid, kCut32, 3, A::List),
              (std::pair<u64, u64>{328, 512}));
    // "tri 0,1,2; 200 fresh tris; tri 0,1,2 again": 606.
    std::vector<u32> far{0, 1, 2};
    for (u32 i = 0; i < 200; ++i) {
        far.insert(far.end(), {3 + i * 3, 4 + i * 3, 5 + i * 3});
    }
    far.insert(far.end(), {0, 1, 2});
    EXPECT_EQ(indexed(far, kCut32, 3, A::List).first, 606u);
    // "100 x {0,1,2}": 3.
    std::vector<u32> repeat;
    for (u32 i = 0; i < 300; ++i) repeat.push_back(i % 3);
    EXPECT_EQ(indexed(repeat, kCut32, 3, A::List).first, 3u);
    // Cuts: "strip {0,1,2,3,cut,4,5,6,7}" IAP 4 VS 8; the same as a list,
    // whose primitive holding the cut is dropped, IAP 2 VS 6; "line strip
    // {0,1,cut,2,3,4}" IAP 3 VS 5.
    const std::vector<u32> cut{0, 1, 2, 3, kCut16, 4, 5, 6, 7};
    EXPECT_EQ(indexed(cut, kCut16, 3, A::Strip),
              (std::pair<u64, u64>{8, 4}));
    EXPECT_EQ(indexed(cut, kCut16, 3, A::List),
              (std::pair<u64, u64>{6, 2}));
    EXPECT_EQ(indexed({0, 1, kCut16, 2, 3, 4}, kCut16, 2, A::Strip),
              (std::pair<u64, u64>{5, 3}));
    // "{0,1,2,2,1,5} x 3 instances" VS 12 = 4 per instance.
    EXPECT_EQ(indexed({0, 1, 2, 2, 1, 5}, kCut16, 3, A::List).first, 4u);

    // [vsbatch]: an index stream of zeros shades one vertex per batch.
    const auto batches = [&](u32 n, u32 vpp, A assembly) {
        return indexed(zeros(n), kCut32, vpp, assembly).first;
    };
    const std::vector<std::pair<u32, u64>> trilist{
        {507, 1}, {510, 1}, {513, 2}, {1020, 2}, {1023, 3}, {1026, 3},
        {1530, 3}, {1533, 4}};
    for (const auto& [n, vs] : trilist) {
        EXPECT_EQ(batches(n, 3, A::List), vs) << "trilist x" << n;
    }
    const std::vector<std::pair<u32, u64>> linelist{
        {510, 1}, {512, 1}, {514, 2}, {1024, 2}, {1026, 3}};
    for (const auto& [n, vs] : linelist) {
        EXPECT_EQ(batches(n, 2, A::List), vs) << "linelist x" << n;
    }
    const std::vector<std::pair<u32, u64>> pointlist{
        {511, 1}, {512, 1}, {513, 2}, {1024, 2}, {1025, 3}};
    for (const auto& [n, vs] : pointlist) {
        EXPECT_EQ(batches(n, 1, A::List), vs) << "pointlist x" << n;
    }
    const std::vector<std::pair<u32, u64>> tristrip{
        {510, 3}, {511, 3}, {512, 3}, {513, 4}, {514, 4}, {1024, 7},
        {1026, 7}};
    for (const auto& [n, vs] : tristrip) {
        EXPECT_EQ(batches(n, 3, A::Strip), vs) << "tristrip x" << n;
    }
    const std::vector<std::pair<u32, u64>> linestrip{
        {511, 2}, {512, 2}, {513, 2}, {514, 3}};
    for (const auto& [n, vs] : linestrip) {
        EXPECT_EQ(batches(n, 2, A::Strip), vs) << "linestrip x" << n;
    }
    const std::vector<std::pair<u32, u64>> trilist_adj{
        {12, 1}, {1020, 2}, {1026, 3}};
    for (const auto& [n, vs] : trilist_adj) {
        EXPECT_EQ(batches(n, 6, A::List), vs) << "trilist_adj x" << n;
    }
    // "tristrip / trilist {0,0,0,cut} x300": 300 primitives, VS 2 -- a batch
    // counts assembled primitives, not index slots.
    std::vector<u32> segments;
    for (u32 i = 0; i < 1200; ++i) segments.push_back(i % 4 == 3 ? kCut32 : 0);
    EXPECT_EQ(indexed(segments, kCut32, 3, A::Strip),
              (std::pair<u64, u64>{2, 300}));
    EXPECT_EQ(indexed(segments, kCut32, 3, A::List),
              (std::pair<u64, u64>{2, 300}));
}

// Non-indexed vertices are all distinct: a list shades its complete
// primitives' vertices (a spare one is never shaded), a strip re-shades the
// vertices a batch boundary splits.
TEST(PipelineStatsModel, NonIndexedVertexInvocations) {
    // "list Draw(7) (one vertex spare)": VS 6. "non-indexed trilist
    // Draw(1024)": IAP 341, VS 1023.
    EXPECT_EQ(stats_vertex_invocations_non_indexed(2, 3, false), 6u);
    EXPECT_EQ(stats_vertex_invocations_non_indexed(341, 3, false), 1023u);
    // "strip Draw(6)" VS 6; "non-indexed tristrip Draw(1024)" 1022
    // primitives, VS 1036; "linestrip Draw(1)" / "tristrip Draw(2)" 0.
    EXPECT_EQ(stats_vertex_invocations_non_indexed(4, 3, true), 6u);
    EXPECT_EQ(stats_vertex_invocations_non_indexed(1022, 3, true), 1036u);
    EXPECT_EQ(stats_vertex_invocations_non_indexed(0, 2, true), 0u);
    EXPECT_EQ(stats_vertex_invocations_non_indexed(0, 3, true), 0u);
    // "TRIANGLELIST_ADJ Draw(12)" VS 12, "LINELIST_ADJ Draw(8)" VS 8:
    // adjacency vertices are shaded too.
    EXPECT_EQ(stats_vertex_invocations_non_indexed(2, 6, false), 12u);
    EXPECT_EQ(stats_vertex_invocations_non_indexed(2, 4, false), 8u);
    // "non-indexed trilist_adj Draw(13)": 2 primitives, VS 12.
    std::vector<u32> ids(13);
    for (u32 i = 0; i < 13; ++i) ids[i] = i;
    const auto keys = assemble_primitive_vertices(ids, false, 0, 0, 6,
                                                  PrimitiveAssembly::List);
    EXPECT_EQ(keys.size() / 6, 2u);
    EXPECT_EQ(stats_vertex_invocations(keys, 6), 12u);
}

namespace {

StatsClipVertex v4(float x, float y, float z, float w) {
    StatsClipVertex v{};
    v.position = {x, y, z, w};
    return v;
}

u32 tri(float ax, float ay, float az, float aw, float bx, float by, float bz,
        float bw, float cx, float cy, float cz, float cw,
        bool depth_clip = true) {
    const StatsClipVertex v[3] = {v4(ax, ay, az, aw), v4(bx, by, bz, bw),
                                  v4(cx, cy, cz, cw)};
    return stats_clip_primitives(v, 3, 0, depth_clip);
}

/// The two triangles d3d11_pstats_probe's quad() draws over an NDC rect.
u32 quad(float x0, float y0, float x1, float y1, u32 distances = 0) {
    const float c[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    StatsClipVertex v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = v4(x0 + (x1 - x0) * c[i][0], y0 + (y1 - y0) * c[i][1], 0.5f,
                  1.0f);
        v[i].distances[0] = v[i].position[0];  // vs_clip: SV_ClipDistance0 = x
    }
    return stats_clip_primitives(v, 3, distances, true) +
           stats_clip_primitives(v + 3, 3, distances, true);
}

}  // namespace

// [clip]: CPrimitives of each case (CInvocations is 1 per primitive in all).
TEST(PipelineStatsModel, ClipperMatchesWarp) {
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 0.5f, 1, 0, 0.5f, 0.5f, 1), 1u);
    EXPECT_EQ(tri(0, -0.5f, 0.5f, 1, 1.6f, -0.2f, 0.5f, 1, 0, 0.5f, 0.5f, 1), 2u);
    EXPECT_EQ(tri(0, -0.5f, 0.5f, 1, -1.6f, -0.2f, 0.5f, 1, 0, 0.5f, 0.5f, 1), 2u);
    EXPECT_EQ(tri(-0.5f, 0, 0.5f, 1, 0.2f, 1.6f, 0.5f, 1, 0.5f, 0, 0.5f, 1), 2u);
    EXPECT_EQ(tri(0.3f, 0.1f, 0.5f, 1, 1.6f, -0.5f, 0.5f, 1, 1.7f, 0.6f, 0.5f, 1), 1u);
    EXPECT_EQ(tri(0.5f, 0.4f, 0.5f, 1, 1.7f, 0.6f, 0.5f, 1, 0.6f, 1.8f, 0.5f, 1), 2u);
    EXPECT_EQ(tri(0.2f, 0.3f, 0.5f, 1, 1.5f, 1.4f, 0.5f, 1, -0.1f, 0.6f, 0.5f, 1), 3u);
    // "fullscreen triangle": its edge passes through the viewport corner,
    // which the clipper keeps twice -- 3, not the square's 2.
    EXPECT_EQ(tri(-1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1), 3u);
    EXPECT_EQ(tri(-1, 1, 0.5f, 1, 3, 1, 0.5f, 1, -1, -3, 0.5f, 1), 3u);
    EXPECT_EQ(tri(-1.3f, -1.2f, 0.5f, 1, 3.4f, -1.1f, 0.5f, 1, -1.2f, 3.3f, 0.5f, 1), 2u);
    EXPECT_EQ(tri(-1, -1, 0.5f, 1, 100, -1, 0.5f, 1, -1, 100, 0.5f, 1), 2u);
    EXPECT_EQ(tri(-5, -5, 0.5f, 1, 9, -5, 0.5f, 1, -5, 9, 0.5f, 1), 2u);
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, -0.5f, 1, 0, 0.5f, 0.5f, 1), 2u);
    EXPECT_EQ(tri(-0.5f, -0.5f, -0.5f, 1, 0.5f, -0.5f, -0.5f, 1, 0, 0.5f, 0.5f, 1), 1u);
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 1.5f, 1, 0, 0.5f, 0.5f, 1), 2u);
    // DepthClipEnable FALSE: no near/far planes.
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 1.5f, 1, 0, 0.5f, 0.5f, 1,
                  false), 1u);
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, -0.5f, 1, 0, 0.5f, 0.5f, 1,
                  false), 1u);
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 0.5f, 1, 0.1f, 0.2f, 0.5f, -1), 2u);
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, -1, 0.5f, -0.5f, 0.5f, -1, 0, 0.5f, 0.5f, -1), 0u);
    EXPECT_EQ(tri(1.5f, -0.5f, 0.5f, 1, 2.5f, -0.5f, 0.5f, 1, 2, 0.5f, 0.5f, 1), 0u);
    EXPECT_EQ(tri(-0.5f, -1.5f, 0.5f, 1, 0.5f, -1.5f, 0.5f, 1, 0, -2.5f, 0.5f, 1), 0u);
    EXPECT_EQ(tri(0.8f, 1.5f, 0.5f, 1, 1.5f, 0.8f, 0.5f, 1, 1.6f, 1.6f, 0.5f, 1), 0u);
    // A vertex or an edge exactly on x = w is inside.
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 1, 0, 0.5f, 1, 0, 0.5f, 0.5f, 1), 1u);
    EXPECT_EQ(tri(1, -0.5f, 0.5f, 1, 1, 0.5f, 0.5f, 1, 0, 0, 0.5f, 1), 1u);
    // Degenerate triangles still reach and leave the clipper.
    EXPECT_EQ(tri(-0.5f, -0.5f, 0.5f, 1, 0, 0, 0.5f, 1, 0.5f, 0.5f, 0.5f, 1), 1u);
    EXPECT_EQ(tri(0.1f, 0.1f, 0.5f, 1, 0.1f, 0.1f, 0.5f, 1, 0.1f, 0.1f, 0.5f, 1), 1u);
    EXPECT_EQ(tri(0, 0, 0.5f, 1, 1, 0.5f, 0.5f, 1, 2, 1, 0.5f, 1), 2u);
    // "quad (0,0)-(2,2) half off-screen" 4, "(0.1,0.2)-(2.3,2.1)" 2,
    // "(-2,-2)-(2,2) around the target" 4.
    EXPECT_EQ(quad(0, 0, 2, 2), 4u);
    EXPECT_EQ(quad(0.1f, 0.2f, 2.3f, 2.1f), 2u);
    EXPECT_EQ(quad(-2, -2, 2, 2), 4u);
    // SV_ClipDistance0 = x: "full quad" 3, "quad in x<0" 0, "in x>=0" 2.
    EXPECT_EQ(quad(-1, -1, 1, 1, 1), 3u);
    EXPECT_EQ(quad(-1, -1, -0.5f, 1, 1), 0u);
    EXPECT_EQ(quad(0, -1, 1, 1, 1), 2u);
}

TEST(PipelineStatsModel, ClipperLinesAndPoints) {
    const auto line = [](float ax, float ay, float bx, float by) {
        const StatsClipVertex v[2] = {v4(ax, ay, 0.5f, 1), v4(bx, by, 0.5f, 1)};
        return stats_clip_primitives(v, 2, 0, true);
    };
    const auto point = [](float x, float y) {
        const StatsClipVertex v = v4(x, y, 0.5f, 1);
        return stats_clip_primitives(&v, 1, 0, true);
    };
    EXPECT_EQ(line(-0.5f, 0, 0.5f, 0.2f), 1u);  // "line inside"
    EXPECT_EQ(line(-0.5f, 0, 1.5f, 0.2f), 1u);  // "line crossing +x"
    EXPECT_EQ(line(1.5f, 0, 2.5f, 0.2f), 0u);   // "line fully past +x"
    EXPECT_EQ(point(0.1f, 0.1f), 1u);           // "point inside"
    EXPECT_EQ(point(1.5f, 0.1f), 0u);           // "point past +x"
    EXPECT_EQ(point(1.0f, 0.1f), 1u);           // "point on x=w"
}

TEST(PipelineStatsModel, RecordIsSixtyFourBitSums) {
    std::array<u32, STATS_RECORD_WORDS> words{};
    words[2 * STAT_PS_INVOCATIONS] = 0x10u;
    words[2 * STAT_PS_INVOCATIONS + 1] = 0x2u;
    EXPECT_EQ(stats_record_value(words.data(), STAT_PS_INVOCATIONS),
              0x200000010ull);
    EXPECT_EQ(stats_vertex_batch(3), 170u);
    EXPECT_EQ(stats_vertex_batch(2), 256u);
    EXPECT_EQ(stats_vertex_batch(1), 512u);
    EXPECT_EQ(stats_vertex_batch(6), 85u);
}
