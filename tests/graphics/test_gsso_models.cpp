// SPDX-License-Identifier: LGPL-2.1-only
//
// The C++ models of the GPU-timeline passes behind the geometry, stream-output
// and tessellation emulations (tessellator.hpp, ia_resolve.hpp), checked
// against Windows. The kernels are the same arithmetic, which
// test_gsso_gpu.cpp holds against these models on a real Metal device.

#include <gtest/gtest.h>

#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/tessellator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

// ---------------------------------------------------------------------------
// The tessellator (a port of the D3D11 reference tessellator) against WARP.
// d3d11_a09b_shader_probe [Tessellation ... stream output from the DS]
// streams every tessellated primitive with a domain shader that outputs
// (u, v, w, SV_PrimitiveID, info) and prints the primitive count and an
// order-independent hash: each primitive rotated to start at its smallest
// vertex, rounded to 1e-5, sorted, FNV-1a over the bytes.
// ---------------------------------------------------------------------------

struct Prim {
    float v[3][8];
};

struct TessCase {
    const char* label;  ///< the a09b row
    u32 domain, partitioning, output_primitive;
    std::vector<float> factors;
    /// The probe's DS info output (control-point id sum, last control
    /// point's vertex id, an edge factor, an inside factor), patch 0's.
    float info[4];
    u32 patches;
    u32 primitives;  ///< WARP
    u32 hash;        ///< WARP
};

[[nodiscard]] std::pair<u32, u32> tessellate_and_hash(const TessCase& c) {
    const u32 vpp = c.output_primitive == 1 ? 1u
                    : c.output_primitive == 2 ? 2u
                                              : 3u;
    std::vector<Prim> prims;
    for (u32 patch = 0; patch < c.patches; ++patch) {
        std::vector<u32> points(4 * 5000);
        std::vector<u32> indices(30000);
        tess::TessState s{};
        s.sink.points = points.data();
        s.sink.indices = indices.data();
        s.sink.write = 1;
        s.sink.primitive_id = patch;
        float f[8] = {};
        for (usize i = 0; i < c.factors.size(); ++i) f[i] = c.factors[i];
        tess::tess_patch(s, c.domain, c.partitioning, c.output_primitive, f);
        for (int i = 0; i + static_cast<int>(vpp) <= s.numIndices;
             i += static_cast<int>(vpp)) {
            float raw[3][8] = {};
            for (u32 k = 0; k < vpp; ++k) {
                const u32 p = indices[static_cast<usize>(i) + k];
                float u = 0.0f;
                float v = 0.0f;
                std::memcpy(&u, &points[p * 4], 4);
                std::memcpy(&v, &points[p * 4 + 1], 4);
                const float w = c.domain == 2 ? 1.0f - u - v : 0.0f;
                // info.y is the patch's last control point's vertex id:
                // patch p of a 3-point patch list is vertices 3p..3p+2.
                const float r[8] = {u, v, w, static_cast<float>(patch),
                                    c.info[0],
                                    c.info[1] + 3.0f * static_cast<float>(patch),
                                    c.info[2], c.info[3]};
                std::memcpy(raw[k], r, sizeof(r));
            }
            u32 best = 0;
            for (u32 v = 1; v < vpp; ++v) {
                if (std::memcmp(raw[v], raw[best], 32) < 0) best = v;
            }
            Prim prim{};
            for (u32 v = 0; v < vpp; ++v) {
                for (int k = 0; k < 8; ++k) {
                    prim.v[v][k] =
                        std::round(raw[(best + v) % vpp][k] * 1e5f) / 1e5f +
                        0.0f;
                }
            }
            prims.push_back(prim);
        }
    }
    std::sort(prims.begin(), prims.end(), [](const Prim& a, const Prim& b) {
        return std::memcmp(&a, &b, sizeof(Prim)) < 0;
    });
    u32 hash = 2166136261u;
    for (const auto& p : prims) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&p);
        for (usize k = 0; k < sizeof(Prim); ++k) {
            hash = (hash ^ bytes[k]) * 16777619u;
        }
    }
    return {static_cast<u32>(prims.size()), hash};
}

TEST(TessellatorModel, MatchesWarpForEveryDomainPartitioningAndOutput) {
    // DXBC: domain 1 isoline / 2 tri / 3 quad; partitioning 1 integer /
    // 2 pow2 / 3 fractional_odd / 4 fractional_even; output 1 point / 2 line
    // / 3 triangle_cw / 4 triangle_ccw.
    const std::vector<TessCase> cases{
        {"hs_tri_int tf=(1,1,1) inside=1 x2", 2, 1, 3, {1, 1, 1, 1},
         {100, 2, 1, 1}, 2, 2, 0xd9eb6928u},
        {"hs_tri_int tf=(2,3,4) inside=3", 2, 1, 3, {2, 3, 4, 3},
         {100, 2, 3, 3}, 1, 13, 0x8ea814fbu},
        {"hs_tri_int tf=(3,3,3) inside=0.5", 2, 1, 3, {3, 3, 3, 0.5f},
         {100, 2, 3, 0.5f}, 1, 9, 0xd8d96641u},
        // The probe's hull shader clamps 70 to its maxtessfactor(64).
        {"hs_tri_int tf=(70,70,70) inside=70", 2, 1, 3, {64, 64, 64, 64},
         {100, 2, 70, 70}, 1, 6144, 0x1ae77061u},
        {"hs_tri_odd tf=(2.5,3.3,1.2) inside=2.7", 2, 3, 3,
         {2.5f, 3.3f, 1.2f, 2.7f}, {100, 2, 3.3f, 2.7f}, 1, 15, 0x7a9edfdcu},
        {"hs_tri_odd tf=(1,1,1) inside=1", 2, 3, 3, {1, 1, 1, 1},
         {100, 2, 1, 1}, 1, 1, 0x50d40896u},
        {"hs_tri_even tf=(2.5,3.3,1.2) inside=2.7", 2, 4, 4,
         {2.5f, 3.3f, 1.2f, 2.7f}, {100, 2, 3.3f, 2.7f}, 1, 22, 0xf7670fedu},
        {"hs_tri_even tf=(1,1,1) inside=1", 2, 4, 4, {1, 1, 1, 1},
         {100, 2, 1, 1}, 1, 6, 0x25e300cdu},
        {"hs_tri_pow2 tf=(3,5,2) inside=6", 2, 2, 3, {3, 5, 2, 6},
         {100, 2, 5, 6}, 1, 46, 0x19cf7ccdu},
        {"hs_quad_int tf=(2,3,1,4) inside=(2,3)", 3, 1, 3, {2, 3, 1, 4, 2, 3},
         {70, 3, 4, 3}, 1, 12, 0x8a9a3759u},
        {"hs_quad_int tf=(1,1,1,1) inside=(1,1)", 3, 1, 3, {1, 1, 1, 1, 1, 1},
         {70, 3, 1, 1}, 1, 2, 0x3e115c35u},
        {"hs_quad_odd tf=(2.5,1.5,3.1,1) inside=(2.2,1.7)", 3, 3, 4,
         {2.5f, 1.5f, 3.1f, 1, 2.2f, 1.7f}, {70, 3, 1, 1.7f}, 1, 18,
         0xb7778d2eu},
        {"hs_iso_int tf=(3,4) lines", 1, 1, 2, {3, 4}, {10, 1, 3, 4}, 1, 12,
         0x9208bf28u},
        {"hs_iso_pt tf=(2,3) points", 1, 1, 1, {2, 3}, {10, 1, 2, 3}, 1, 8,
         0xc156c5a9u},
        // A zero or NaN edge factor culls the patch (D3D11.3 11.7.6).
        {"hs_tri_int tf=(0,1,1) inside=1", 2, 1, 3, {0, 1, 1, 1},
         {100, 2, 1, 1}, 1, 0, 0x811c9dc5u},
        {"hs_tri_int tf=(nan,1,1) inside=1", 2, 1, 3, {NAN, 1, 1, 1},
         {100, 2, 1, 1}, 1, 0, 0x811c9dc5u},
    };
    for (const auto& c : cases) {
        const auto [primitives, hash] = tessellate_and_hash(c);
        EXPECT_EQ(primitives, c.primitives) << c.label;
        EXPECT_EQ(hash, c.hash) << c.label << std::hex << " got " << hash;
    }
}

TEST(TessellatorModel, PatchBoundCoversEveryFactorUpToTheMaximum) {
    // The host sizes the point and index buffers from tess_patch_bound; a
    // patch that made more would be dropped as an overflow.
    for (u32 domain = 1; domain <= 3; ++domain) {
        for (u32 part = 1; part <= 4; ++part) {
            const u32 output = domain == 1 ? 2u : 3u;
            const auto bound = tess::tess_patch_bound(domain, part, output, 64);
            for (float f = 1.0f; f <= 64.0f; f += 3.7f) {
                float factors[8] = {f, f * 0.5f + 1, f, f, f, f * 0.7f + 1};
                tess::TessState s{};
                s.sink.write = 0;
                tess::tess_patch(s, domain, part, output, factors);
                EXPECT_LE(static_cast<u32>(s.numPoints), bound.points)
                    << domain << "/" << part << " f=" << f;
                EXPECT_LE(static_cast<u32>(s.numIndices), bound.indices)
                    << domain << "/" << part << " f=" << f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Primitive assembly of an indirect draw (ia_resolve). Expected keys are the
// d3d11_gsso_probe [indirect] rows: the geometry shader / stream output saw
// these vertices, keyed (vertex index or index value) + instance x stride.
// ---------------------------------------------------------------------------

TEST(IaResolveModel, IndexedStripWithACutMatchesWindows) {
    // "gs_tri3 TRIANGLESTRIP R16 {0,1,2,3,FFFF,4,5,6,7}
    //  DrawIndexedInstancedIndirect {9, 2, 0, 10, 3}": per instance
    // (0 1 2) (1 3 2) (4 5 6) (5 7 6) -- odd triangles flip winding, the cut
    // restarts the strip.
    IaResolveParams p{};
    p.indexed = 1;
    p.index_32bit = 0;
    p.index_elements = 9;
    p.vertices_per_primitive = 3;
    p.assembly = 1;
    p.record_capacity = 1024;
    p.primitive_capacity = 1024;
    const std::vector<u32> indices{0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
    std::vector<u32> ids;
    const auto out = ia_resolve_model(p, {9, 2, 0, 10, 3}, indices, ids);
    const std::vector<u32> instance0{0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6};
    std::vector<u32> expected = instance0;
    for (const u32 k : instance0) expected.push_back(k + 8);  // stride 8
    EXPECT_EQ(ids, expected);
    EXPECT_EQ(out[IA_RESOLVED_PRIMITIVES], 8u);
    EXPECT_EQ(out[IA_RESOLVED_PRIMITIVES_PER_INSTANCE], 4u);
    EXPECT_EQ(out[IA_RESOLVED_KEY_STRIDE], 8u);
    EXPECT_EQ(out[IA_RESOLVED_INSTANCES], 2u);
    EXPECT_EQ(out[IA_RESOLVED_OVERFLOW], 0u);
    EXPECT_EQ(out[IA_RESOLVED_VERTICES], 16u);
    for (u32 w = 0; w < 5; ++w) {
        EXPECT_EQ(out[IA_RESOLVED_CAPTURE_ARGS + w],
                  (std::array<u32, 5>{9, 2, 0, 10, 3})[w]);
    }
}

TEST(IaResolveModel, NonIndexedLineStripAndOverflow) {
    // "SO of the VS, LINESTRIP DrawInstancedIndirect {3, 2, 4, 0}":
    // (0 1) (1 2) per instance, keys draw-relative.
    IaResolveParams p{};
    p.vertices_per_primitive = 2;
    p.assembly = 1;
    p.record_capacity = 64;
    p.primitive_capacity = 64;
    std::vector<u32> ids;
    auto out = ia_resolve_model(p, {3, 2, 4, 0}, {}, ids);
    EXPECT_EQ(ids, (std::vector<u32>{0, 1, 1, 2, 3, 4, 4, 5}));
    EXPECT_EQ(out[IA_RESOLVED_PRIMITIVES], 4u);
    EXPECT_EQ(out[IA_RESOLVED_KEY_STRIDE], 3u);
    // A draw the capture cannot hold is dropped whole and flagged, never
    // drawn in part.
    p.record_capacity = 5;
    out = ia_resolve_model(p, {3, 2, 4, 0}, {}, ids);
    EXPECT_TRUE(ids.empty());
    EXPECT_EQ(out[IA_RESOLVED_PRIMITIVES], 0u);
    EXPECT_EQ(out[IA_RESOLVED_INSTANCES], 0u);
    EXPECT_EQ(out[IA_RESOLVED_OVERFLOW], 1u);
    EXPECT_EQ(out[IA_RESOLVED_CAPTURE_ARGS + 1], 0u);
}

}  // namespace
