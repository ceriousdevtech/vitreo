// SPDX-License-Identifier: LGPL-2.1-only
//
// WS5 Task 9 and 10 on real Metal: a geometry-shader draw on the mesh path
// (in-pass capture + one mesh draw) gives bit-identical pixels to the
// three-pass path, the selector keeps order-dependent output three-pass,
// and the GPU-time gate (VITREO_WS5_BENCH=1).

#include <gtest/gtest.h>

#include "d3d11_test_metal_fixture.hpp"
#include "d3d11_test_streamout.hpp"  // dxbc_with_signatures
#include "../../graphics/gs_sm5_shaders.h"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/d3d11/d3d11_gs_mesh_select.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "winapi/graphics/d3d11/d3d11_buffer_ring.hpp"

#include <algorithm>
#include <chrono>
#include <bit>
#include <stdlib.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

using gs_mesh::Force;

class D3D11GsMeshMetalTest : public D3D11MetalTest {
protected:
    void TearDown() override {
        gs_mesh::force_for_test(Force::None);
        d3d11_ws6_override_for_test(std::nullopt);
        graphics::MetalSubmitQueue::instance().drain();
    }

    bool mesh_capable() const {
        return graphics::MetalDevice::instance().supports_mesh_pipelines() &&
               !gs_mesh::kill_switch_off();
    }

    GuestAddr texture_2d(u32 w, u32 h, u32 format, u32 bind, u32 usage = 0,
                         u32 cpu = 0, u32 array = 1) {
        // D3D11_TEXTURE2D_DESC
        const std::array<u32, 10> desc{w, h, 1, array, format, 1, 0, usage, bind, cpu};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(5, {put(desc), 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    GuestAddr render_target(u32 w, u32 h, GuestAddr& texture) {
        texture = texture_2d(w, h, 28 /* R8G8B8A8_UNORM */, 0x20 /* RENDER_TARGET */);
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(9, {texture, 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    GuestAddr depth_target(u32 w, u32 h, GuestAddr& texture) {
        texture = texture_2d(w, h, 40 /* D32_FLOAT */, 0x40 /* DEPTH_STENCIL */);
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(10, {texture, 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    /// Every texel of a 4-byte-per-texel 2D texture.
    std::vector<u32> read_texels(GuestAddr texture, u32 w, u32 h, u32 format,
                                 u32 slices = 1, u32 slice = 0) {
        const GuestAddr staging = texture_2d(w, h, format, 0, 3 /* STAGING */,
                                             0x20000 /* READ */, slices);
        context_call(47, {staging, texture});  // CopyResource
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        EXPECT_EQ(context_call(14, {staging, slice, 1, 0, mapped}), d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        const u32 pitch = memory.read_u32(mapped + 8).value();
        std::vector<u32> texels(static_cast<usize>(w) * h, 0);
        for (u32 y = 0; y < h && data != 0; ++y) {
            for (u32 x = 0; x < w; ++x) {
                texels[y * w + x] = memory.read_u32(data + y * pitch + x * 4).value();
            }
        }
        context_call(15, {staging, slice});  // Unmap
        return texels;
    }

    /// D3D11_BLEND_DESC with RT0 = (src, dst, op) for colour and alpha.
    GuestAddr blend_state(u32 src, u32 dst, u32 op) {
        std::array<u32, 66> desc{};
        desc[2] = 1;  // BlendEnable
        desc[3] = src;
        desc[4] = dst;
        desc[5] = op;
        desc[6] = src;
        desc[7] = dst;
        desc[8] = op;
        desc[9] = 0xF;  // RenderTargetWriteMask
        for (u32 rt = 1; rt < 8; ++rt) desc[2 + rt * 8 + 7] = 0xF;
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(20, {put(desc), out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    void set_blend(GuestAddr state) {
        const std::array<float, 4> factor{1, 1, 1, 1};
        context_call(35, {state, put(factor), 0xFFFFFFFFu});
    }

    /// D3D11_DEPTH_STENCIL_DESC: depth on, writes on, `func`; stencil off.
    void set_depth(u32 func) {
        std::array<u32, 13> desc{1, 1, func, 0, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8};
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(21, {put(desc), out}), d3d11::S_OK);
        context_call(36, {memory.read_u64(out).value(), 0});
    }

    void viewport(float w, float h) {
        const std::array<float, 6> vp{0, 0, w, h, 0, 1};
        context_call(44, {1, put(vp)});
    }

    /// The pass-through VS's input layout: float4 position, float4 colour.
    GuestAddr pass_layout() {
        const auto container = graphics::parse_dxbc(gs5_vs_pass, sizeof(gs5_vs_pass));
        EXPECT_GE(container.input_signature.size(), 2u);
        m_semantics[0] = container.input_signature[0].semantic_name;
        m_semantics[1] = container.input_signature[1].semantic_name;
        return input_layout({{m_semantics[0].c_str(), container.input_signature[0].semantic_index,
                              2 /* R32G32B32A32_FLOAT */, 0, 0, 0},
                             {m_semantics[1].c_str(), container.input_signature[1].semantic_index,
                              2, 16, 0, 0}},
                            gs5_vs_pass, sizeof(gs5_vs_pass));
    }

    GuestAddr buffer(const void* bytes, u32 size, u32 bind) {
        const std::array<u32, 6> bd{size, 0, bind, 0, 0, 0};
        const GuestAddr init = put(std::array<u64, 2>{put(bytes, size), 0});
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(3, {put(bd), init, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    void bind_vertices(GuestAddr vb) {
        const std::array<u64, 1> buffers{vb};
        const std::array<u32, 1> strides{32};
        const std::array<u32, 1> offsets{0};
        context_call(18, {0, 1, put(buffers), put(strides), put(offsets)});
    }

    struct Arms {
        std::vector<u32> three_pass, mesh;
        u64 mesh_draws_in_three_pass{0}, mesh_draws_in_mesh{0};
    };

    /// Runs `draw` (which clears and draws, then returns the texels) once
    /// forced three-pass and once on the default (mesh-eligible) choice.
    Arms both_paths(const std::function<std::vector<u32>()>& draw) {
        Arms a{};
        gs_mesh::force_for_test(Force::ThreePass);
        u64 before = d3d11.debug_gs_mesh_draws();
        a.three_pass = draw();
        a.mesh_draws_in_three_pass = d3d11.debug_gs_mesh_draws() - before;
        gs_mesh::force_for_test(Force::Mesh);
        before = d3d11.debug_gs_mesh_draws();
        a.mesh = draw();
        a.mesh_draws_in_mesh = d3d11.debug_gs_mesh_draws() - before;
        gs_mesh::force_for_test(Force::None);
        return a;
    }

    void expect_same(const Arms& a, bool expect_mesh, const char* scene) {
        EXPECT_EQ(a.mesh_draws_in_three_pass, 0u) << scene;
        if (expect_mesh && mesh_capable()) {
            EXPECT_GT(a.mesh_draws_in_mesh, 0u) << scene << ": the mesh path did not run";
        } else {
            EXPECT_EQ(a.mesh_draws_in_mesh, 0u) << scene;
        }
        ASSERT_EQ(a.mesh.size(), a.three_pass.size());
        usize differ = 0, lit = 0;
        for (usize i = 0; i < a.mesh.size(); ++i) {
            differ += a.mesh[i] != a.three_pass[i];
            lit += a.three_pass[i] != 0;
        }
        EXPECT_EQ(differ, 0u) << scene << ": texels differ between the paths";
        EXPECT_GT(lit, 0u) << scene << ": the scene drew nothing";
    }

    std::string m_semantics[2];
};

constexpr u32 kW = 64;
constexpr u32 kH = 64;

/// Points across the target with distinct colours.
std::vector<float> points(u32 count, float alpha, float x0 = -0.95f, float step = 0.11f) {
    std::vector<float> v;
    for (u32 i = 0; i < count; ++i) {
        const float x = x0 + step * static_cast<float>(i);
        v.insert(v.end(), {x, 0.0f, 0.5f, 1.0f,
                           static_cast<float>((i * 37) % 11) / 10.0f,
                           static_cast<float>((i * 53) % 7) / 6.0f,
                           static_cast<float>(i % 5) / 4.0f, alpha});
    }
    return v;
}

TEST_F(D3D11GsMeshMetalTest, QuadExpansionInstancedMaxBlend) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_quad, sizeof(gs5_gs_quad));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    ASSERT_TRUE(vs && gs && ps);
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kW, kH, tex);
    const auto v = points(16, 1.0f);
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {1});  // POINTLIST
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 5));  // ONE, ONE, MAX
    viewport(kW, kH);
    const auto arms = both_paths([&] {
        const std::array<float, 4> clear{0, 0, 0, 0};
        context_call(50, {rtv, put(clear)});
        context_call(21, {16, 72, 0, 0});  // DrawInstanced(16 points, 72 instances)
        return read_texels(tex, kW, kH, 28);
    });
    expect_same(arms, true, "quad expansion, 16 points x 72 instances");
}

// gs5_gs_layered: instance(4), SV_PrimitiveID and SV_GSInstanceID in the
// colour, SV_RenderTargetArrayIndex = instance, SV_ViewportArrayIndex =
// primitive & 1, SV_ClipDistance0 = x (G5, G6, G8, G9).
TEST_F(D3D11GsMeshMetalTest, LayeredInstancedToATextureArray) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_layered, sizeof(gs5_gs_layered));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    ASSERT_TRUE(vs && gs && ps);
    constexpr u32 kSlices = 4;
    const GuestAddr tex = texture_2d(kW, kH, 28, 0x20, 0, 0, kSlices);
    const std::array<u32, 5> view{28, 5 /* D3D11_RTV_DIMENSION_TEXTURE2DARRAY */, 0, 0, kSlices};
    GuestAddr out = out_slot();
    ASSERT_EQ(device_call(9, {tex, put(view), out}), d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(out).value();
    // Two triangles straddling x = 0 (so the clip distance cuts them).
    std::vector<float> v;
    for (const auto& p : {std::array<float, 2>{-0.8f, -0.8f}, {-0.8f, 0.8f}, {0.8f, -0.8f},
                          {-0.6f, 0.8f}, {0.9f, 0.9f}, {0.9f, -0.6f}}) {
        v.insert(v.end(), {p[0], p[1], 0.5f, 1.0f, 0, 0, 0, 1});
    }
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {4});  // TRIANGLELIST
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 5));
    const std::array<float, 12> vps{0, 0, kW, kH, 0, 1, 0, 0, kW / 2, kH / 2, 0, 1};
    context_call(44, {2, put(vps)});
    const auto arms = both_paths([&] {
        const std::array<float, 4> clear{0, 0, 0, 0};
        context_call(50, {rtv, put(clear)});
        context_call(13, {6, 0});
        std::vector<u32> all;
        for (u32 slice = 0; slice < kSlices; ++slice) {
            const auto t = read_texels(tex, kW, kH, 28, kSlices, slice);
            all.insert(all.end(), t.begin(), t.end());
        }
        return all;
    });
    expect_same(arms, true, "layered, instanced, two viewports, clip distance");
    // Every slice is reached: the instance picks the slice.
    for (u32 slice = 0; slice < kSlices; ++slice) {
        usize lit = 0;
        for (u32 i = 0; i < kW * kH; ++i) lit += arms.three_pass[slice * kW * kH + i] != 0;
        EXPECT_GT(lit, 0u) << "slice " << slice;
        std::printf("[ws5] layered slice %u: %zu texels written\n", slice, lit);
    }
}

// The layered scene's readback (above) sees slice 0 only, on both paths.
// This separates the readback from the drawing: each slice is cleared
// through its own one-slice RTV and read back the same way.
TEST_F(D3D11GsMeshMetalTest, TextureArraySlicesReadBackIndependently) {
    constexpr u32 kSlices = 4;
    const GuestAddr tex = texture_2d(kW, kH, 28, 0x20, 0, 0, kSlices);
    for (u32 slice = 0; slice < kSlices; ++slice) {
        const std::array<u32, 5> view{28, 5 /* TEXTURE2DARRAY */, 0, slice, 1};
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(9, {tex, put(view), out}), d3d11::S_OK);
        const std::array<float, 4> colour{0.25f * float(slice + 1), 0, 0, 1};
        context_call(50, {memory.read_u64(out).value(), put(colour)});
    }
    for (u32 slice = 0; slice < kSlices; ++slice) {
        const auto t = read_texels(tex, kW, kH, 28, kSlices, slice);
        const u32 want_r = static_cast<u32>(0.25f * float(slice + 1) * 255.0f + 0.5f);
        EXPECT_EQ(t[0] & 0xFFu, want_r) << "slice " << slice << " texel " << std::hex << t[0];
    }
}

TEST_F(D3D11GsMeshMetalTest, BackToBackDrawsReuseTheRing) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_quad, sizeof(gs5_gs_quad));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kW, kH, tex);
    // Two vertex buffers: the second's quads start further right.
    const auto a = points(3, 1.0f, -0.9f, 0.05f);
    const auto b = points(3, 1.0f, 0.4f, 0.05f);
    const GuestAddr vb_a = buffer(a.data(), static_cast<u32>(a.size() * 4), 1);
    const GuestAddr vb_b = buffer(b.data(), static_cast<u32>(b.size() * 4), 1);
    context_call(17, {pass_layout()});
    context_call(24, {1});
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 4));  // MIN
    viewport(kW, kH);
    const auto arms = both_paths([&] {
        const std::array<float, 4> clear{1, 1, 1, 1};
        context_call(50, {rtv, put(clear)});
        bind_vertices(vb_a);
        context_call(13, {3, 0});  // Draw
        bind_vertices(vb_b);
        context_call(13, {3, 0});
        return read_texels(tex, kW, kH, 28);
    });
    expect_same(arms, true, "back-to-back draws");
    EXPECT_GE(arms.mesh_draws_in_mesh, mesh_capable() ? 2u : 0u);
}

TEST_F(D3D11GsMeshMetalTest, TrianglesWithAdjacencyAndIndexedCuts) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr adj = shader(13, gs5_gs_triadj, sizeof(gs5_gs_triadj));
    const GuestAddr tri = shader(13, gs5_gs_tri_pass, sizeof(gs5_gs_tri_pass));
    const GuestAddr line = shader(13, gs5_gs_lineadj, sizeof(gs5_gs_lineadj));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    ASSERT_TRUE(adj && tri && line);
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kW, kH, tex);
    std::vector<float> v;
    for (u32 i = 0; i < 12; ++i) {
        const float x = -0.9f + 0.15f * static_cast<float>(i);
        // Corners 0, 2, 4 of every six (the adjacency primitives' triangle)
        // and consecutive strip vertices all span an area.
        const float y = i % 4 == 0 ? -0.7f : i % 4 == 2 ? 0.6f : (i % 8 == 1 ? 0.1f : -0.2f);
        v.insert(v.end(), {x, y, 0.5f, 1.0f, (i % 3) / 2.0f, (i % 4) / 3.0f, 0.5f, 1.0f});
    }
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    const std::array<u16, 12> idx{0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7, 8, 9, 10};
    const GuestAddr ib = buffer(idx.data(), sizeof(idx), 2 /* INDEX_BUFFER */);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(11, {vs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 5));
    viewport(kW, kH);
    const struct {
        const char* name;
        GuestAddr gs;
        u32 topology;
        bool indexed;
        u32 count;
    } scenes[] = {
        {"TRIANGLELIST_ADJ", adj, 12, false, 12},
        {"TRIANGLESTRIP_ADJ", adj, 13, false, 8},
        {"LINELIST_ADJ", line, 10, false, 12},
        {"TRIANGLESTRIP + 16-bit cut", tri, 5, true, 12},
    };
    for (const auto& s : scenes) {
        context_call(23, {s.gs, 0, 0});
        context_call(24, {s.topology});
        if (s.indexed) context_call(19, {ib, 57 /* R16_UINT */, 0});
        const auto arms = both_paths([&] {
            const std::array<float, 4> clear{0, 0, 0, 0};
            context_call(50, {rtv, put(clear)});
            if (s.indexed) {
                context_call(12, {s.count, 0, 0});  // DrawIndexed
            } else {
                context_call(13, {s.count, 0});
            }
            return read_texels(tex, kW, kH, 28);
        });
        expect_same(arms, true, s.name);
    }
}

// Order-dependent blending (SrcAlpha / InvSrcAlpha over overlapping quads).
// D3D11 rasterizes in input-primitive order. A draw whose invocations all fit
// ONE mesh threadgroup is one mesh, rasterized in primitive-index order,
// which the mesh lays out in that same order: it takes the mesh path and must
// match the three-pass pixels and D3D's order. A draw that needs several
// threadgroups has no documented order between them and stays three-pass.
struct OrderedBlendScene {
    u32 points;
    float step;
    bool expect_mesh;
    const char* name;
};

TEST_F(D3D11GsMeshMetalTest, OverlappingBlendedQuadsKeepD3DOrderOnEitherPath) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_quad, sizeof(gs5_gs_quad));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kW, kH, tex);
    const u32 k = d3d11.debug_gs_mesh_invocations_per_group(gs);
    ASSERT_GT(k, 8u) << "the eight-quad scene must fit one threadgroup";
    const OrderedBlendScene scenes[] = {
        {8, 0.1f, true, "one mesh: ordered"},
        {k, 1.6f / static_cast<float>(k), true, "exactly one threadgroup"},
        {k + 1, 1.6f / static_cast<float>(k + 1), false, "two threadgroups: three-pass"},
    };
    for (const OrderedBlendScene& scene : scenes) {
        SCOPED_TRACE(scene.name);
        // Overlapping quads (each spans [x, 1]), half transparent.
        const auto v = points(scene.points, 0.5f, -0.8f, scene.step);
        const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
        context_call(17, {pass_layout()});
        bind_vertices(vb);
        context_call(24, {1});
        context_call(11, {vs, 0, 0});
        context_call(23, {gs, 0, 0});
        context_call(9, {ps, 0, 0});
        const std::array<u64, 1> rtvs{rtv};
        context_call(33, {1, put(rtvs), 0});
        set_blend(blend_state(5, 6, 1));  // SRC_ALPHA, INV_SRC_ALPHA, ADD
        viewport(kW, kH);
        const auto arms = both_paths([&] {
            const std::array<float, 4> clear{0, 0, 0, 0};
            context_call(50, {rtv, put(clear)});
            context_call(13, {scene.points, 0});
            return read_texels(tex, kW, kH, 28);
        });
        expect_same(arms, scene.expect_mesh, scene.name);
        // The right edge is covered by every quad: D3D order blends the
        // last primitive's colour over the others.
        float r = 0, g = 0, b = 0;
        for (u32 i = 0; i < scene.points; ++i) {
            r = v[i * 8 + 4] * 0.5f + r * 0.5f;
            g = v[i * 8 + 5] * 0.5f + g * 0.5f;
            b = v[i * 8 + 6] * 0.5f + b * 0.5f;
        }
        const auto near = [](u32 got, float want) {
            const int e = static_cast<int>(want * 255.0f + 0.5f);
            return std::abs(static_cast<int>(got) - e) <= 2;
        };
        for (const auto* path : {&arms.three_pass, &arms.mesh}) {
            const u32 texel = (*path)[32 * kW + (kW - 2)];
            EXPECT_TRUE(near(texel & 0xFF, r) && near((texel >> 8) & 0xFF, g) &&
                        near((texel >> 16) & 0xFF, b))
                << std::hex << texel;
        }
    }
}

// UE4's volumetric-fog voxelization: the geometry shader picks the slice
// (SV_RenderTargetArrayIndex) and the PIXEL shader reads it back as an input.
// The mesh writes the layer per primitive ([[render_target_array_index]]),
// and the fragment stage reads the same builtin on either path.
TEST_F(D3D11GsMeshMetalTest, PixelShaderReadsTheLayerTheGeometryShaderChose) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_layered, sizeof(gs5_gs_layered));
    const GuestAddr ps = shader(15, gs5_ps_layer, sizeof(gs5_ps_layer));
    ASSERT_TRUE(vs && gs && ps);
    constexpr u32 kSlices = 4;
    const GuestAddr tex = texture_2d(kW, kH, 28, 0x20, 0, 0, kSlices);
    const std::array<u32, 5> view{28, 5 /* TEXTURE2DARRAY */, 0, 0, kSlices};
    GuestAddr out = out_slot();
    ASSERT_EQ(device_call(9, {tex, put(view), out}), d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(out).value();
    std::vector<float> v;
    for (const auto& p : {std::array<float, 2>{-0.8f, -0.8f}, {-0.8f, 0.8f}, {0.8f, -0.8f},
                          {-0.6f, 0.8f}, {0.9f, 0.9f}, {0.9f, -0.6f}}) {
        v.insert(v.end(), {p[0], p[1], 0.5f, 1.0f, 0, 0, 0, 1});
    }
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {4});  // TRIANGLELIST
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    // ONE, ONE, MAX: the second triangle's viewport overlaps the first, and
    // within a slice both write the same blue.
    set_blend(blend_state(2, 2, 5));
    const std::array<float, 12> vps{0, 0, kW, kH, 0, 1, 0, 0, kW / 2, kH / 2, 0, 1};
    context_call(44, {2, put(vps)});
    const auto arms = both_paths([&] {
        const std::array<float, 4> clear{0, 0, 0, 0};
        context_call(50, {rtv, put(clear)});
        context_call(13, {6, 0});
        std::vector<u32> all;
        for (u32 slice = 0; slice < kSlices; ++slice) {
            const auto t = read_texels(tex, kW, kH, 28, kSlices, slice);
            all.insert(all.end(), t.begin(), t.end());
        }
        return all;
    });
    expect_same(arms, true, "the pixel shader reads the layer");
    // Blue is (layer + 1) / 4 wherever the slice was drawn: each slice holds
    // its OWN index, on both paths.
    for (const auto* path : {&arms.three_pass, &arms.mesh}) {
        for (u32 slice = 0; slice < kSlices; ++slice) {
            const u32 want = static_cast<u32>((slice + 1) * 255.0f / 4.0f + 0.5f);
            usize lit = 0, wrong = 0;
            for (u32 i = 0; i < kW * kH; ++i) {
                const u32 texel = (*path)[slice * kW * kH + i];
                if (texel == 0) continue;
                ++lit;
                const int blue = static_cast<int>((texel >> 16) & 0xFF);
                wrong += std::abs(blue - static_cast<int>(want)) > 1;
            }
            EXPECT_GT(lit, 0u) << "slice " << slice;
            EXPECT_EQ(wrong, 0u) << "slice " << slice << " holds another slice's index";
        }
    }
}

TEST_F(D3D11GsMeshMetalTest, OverlappingDepthOnlyIsOrderIndependent) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_tri_pass, sizeof(gs5_gs_tri_pass));
    GuestAddr depth_tex = 0;
    const GuestAddr dsv = depth_target(kW, kH, depth_tex);
    std::vector<float> v;
    for (u32 t = 0; t < 8; ++t) {
        const float z = 0.1f + 0.1f * static_cast<float>((t * 5) % 8);
        const float x = -0.9f + 0.1f * static_cast<float>(t);
        for (const auto& p : {std::array<float, 2>{x, -0.9f}, {x, 0.9f}, {x + 0.9f, -0.9f}}) {
            v.insert(v.end(), {p[0], p[1], z, 1.0f, 0, 0, 0, 1});
        }
    }
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {4});  // TRIANGLELIST
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {0, 0, 0});  // no pixel shader
    context_call(33, {0, 0, dsv});
    set_depth(4 /* LESS_EQUAL */);
    viewport(kW, kH);
    const auto arms = both_paths([&] {
        cpu.sse.xmm[3].u32_[0] = std::bit_cast<u32>(1.0f);  // Depth (XMM3)
        context_call(53, {dsv, 1 /* CLEAR_DEPTH */, 0, 0});
        context_call(13, {24, 0});
        auto texels = read_texels(depth_tex, kW, kH, 40);
        if (std::getenv("VITREO_WS5_TEST_DUMP")) {
            std::fprintf(stderr, "depth centre %08x corner %08x\n", texels[32 * kW + 32], texels[0]);
        }
        // Count depth below the clear value as "drawn".
        for (u32& t : texels) t = t == std::bit_cast<u32>(1.0f) ? 0u : t;
        return texels;
    });
    expect_same(arms, true, "depth-only LessEqual");
}

// gs_4_0, point in, triangle strip out, maxvertexcount 8: five emits (three
// triangles), RestartStrip, then two emits -- an incomplete strip, which D3D
// silently discards (Learn, GS stage). o1 = v[0][1].
std::vector<u8> restart_strip_shader() {
    const auto f = [](float v) { return std::bit_cast<u32>(v); };
    std::vector<u32> t{
        0x00020040u, 0u,
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,  // dcl_input_siv v[1][0], position
        0x0400005Fu, 0x002010F2u, 1u, 1u,      // dcl_input v[1][1]
        0x0100085Du,                           // dcl_inputprimitive point
        0x0100285Cu,                           // dcl_outputtopology trianglestrip
        0x04000067u, 0x001020F2u, 0u, 1u,      // dcl_output_siv o0, position
        0x03000065u, 0x001020F2u, 1u,          // dcl_output o1
        0x0200005Eu, 8u,                       // dcl_maxout 8
    };
    const auto emit = [&](float x, float y) {
        t.insert(t.end(), {0x08000036u, 0x001020F2u, 0u, 0x00004002u, f(x), f(y), f(0.5f),
                           f(1.0f),                                   // mov o0, l(x, y, .5, 1)
                           0x06000036u, 0x001020F2u, 1u, 0x00201E46u, 0u, 1u,  // mov o1, v[0][1]
                           0x01000013u});                              // emit
    };
    emit(-0.8f, -0.5f);
    emit(-0.8f, 0.5f);
    emit(-0.4f, -0.5f);
    emit(-0.4f, 0.5f);
    emit(0.0f, -0.5f);
    t.push_back(0x01000009u);  // cut
    emit(0.4f, -0.5f);
    emit(0.4f, 0.5f);
    t.push_back(0x0100003Eu);  // ret
    t[1] = static_cast<u32>(t.size());
    return dxbc_with_signatures(
        t, {{"SV_Position", 0, 1, 3, 0, 0xF}, {"COLOR", 0, 0, 3, 1, 0xF}},
        {{"SV_Position", 0, 1, 3, 0, 0xF}, {"COLOR", 0, 0, 3, 1, 0xF}});
}

TEST_F(D3D11GsMeshMetalTest, RestartStripAndPartialStrip) {
    const auto blob = restart_strip_shader();
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, blob.data(), blob.size());
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    ASSERT_TRUE(vs && gs && ps);
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kW, kH, tex);
    const auto v = points(2, 1.0f);
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {1});
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 5));
    viewport(kW, kH);
    // No culling: the strip's triangles alternate winding.
    const std::array<u32, 10> rs{3, 1, 0, 0, 0, 0, 1, 0, 0, 0};
    const GuestAddr out = out_slot();
    ASSERT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
    context_call(43, {memory.read_u64(out).value()});
    const auto arms = both_paths([&] {
        const std::array<float, 4> clear{0, 0, 0, 0};
        context_call(50, {rtv, put(clear)});
        context_call(13, {2, 0});
        return read_texels(tex, kW, kH, 28);
    });
    expect_same(arms, true, "restart strip + discarded partial strip");
    // NDC x 0.4 is pixel 44: only the discarded tail would cover it.
    EXPECT_EQ(arms.three_pass[32 * kW + 46], 0u) << "the 2-vertex tail must not draw";
    EXPECT_NE(arms.three_pass[32 * kW + 16], 0u) << "the first strip draws";
}

TEST_F(D3D11GsMeshMetalTest, DepthBiasOffsetMatches) {
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_tri_pass, sizeof(gs5_gs_tri_pass));
    GuestAddr depth_tex = 0;
    const GuestAddr dsv = depth_target(kW, kH, depth_tex);
    std::vector<float> v;
    for (const auto& p : {std::array<float, 3>{-0.9f, -0.9f, 0.3f}, {-0.9f, 0.9f, 0.5f},
                          {0.9f, -0.9f, 0.7f}}) {
        v.insert(v.end(), {p[0], p[1], p[2], 1.0f, 0, 0, 0, 1});
    }
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {4});
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {0, 0, 0});
    context_call(33, {0, 0, dsv});
    set_depth(4 /* LESS_EQUAL */);
    viewport(kW, kH);
    // DepthBias 1000 (D3D11_RASTERIZER_DESC.DepthBias), cull none.
    const std::array<u32, 10> rs{3, 1, 0, 1000, 0, 0, 1, 0, 0, 0};
    const GuestAddr out = out_slot();
    ASSERT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
    context_call(43, {memory.read_u64(out).value()});
    const auto arms = both_paths([&] {
        cpu.sse.xmm[3].u32_[0] = std::bit_cast<u32>(1.0f);
        context_call(53, {dsv, 1, 0, 0});
        context_call(13, {3, 0});
        auto texels = read_texels(depth_tex, kW, kH, 40);
        for (u32& t : texels) t = t == std::bit_cast<u32>(1.0f) ? 0u : t;
        return texels;
    });
    expect_same(arms, true, "depth bias 1000 on D32");
}

// WS5 x WS6: the GS input is a DYNAMIC vertex buffer renamed by
// Map(WRITE_DISCARD) before every draw (Learn, D3D11_MAP_WRITE_DISCARD: work
// already queued keeps reading the previous contents). Every draw is queued
// behind a GPU fence before any runs, so a mesh draw or in-pass capture that
// read the buffer through a stale ring window would draw the last write's
// points N times. Run with the ring off (shadow) and on, three-pass and mesh:
// all four arms give the same pixels and every draw's own points are lit.
TEST_F(D3D11GsMeshMetalTest, DynamicDiscardEveryDrawWithRingAndShadow) {
    auto& dev = graphics::MetalDevice::instance();
    ASSERT_TRUE(dev.supports_fences());
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_quad, sizeof(gs5_gs_quad));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    ASSERT_TRUE(vs && gs && ps);
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kW, kH, tex);
    constexpr u32 kDraws = 6;
    constexpr u32 kPointsPerDraw = 1;
    constexpr u32 kBytes = kPointsPerDraw * 32;
    // D3D11_BUFFER_DESC: DYNAMIC, VERTEX_BUFFER, CPU_ACCESS_WRITE.
    const std::array<u32, 6> bd{kBytes, 2 /* DYNAMIC */, 1 /* VERTEX_BUFFER */,
                                0x10000 /* CPU_ACCESS_WRITE */, 0, 0};
    GuestAddr out = out_slot();
    ASSERT_EQ(device_call(3, {put(bd), 0, out}), d3d11::S_OK);
    const GuestAddr vb = memory.read_u64(out).value();
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {1});  // POINTLIST
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 5));  // ONE, ONE, MAX: order-independent
    viewport(kW, kH);
    // gs5_gs_quad covers [point.x, 1] x [-1, 1]. Draw d's point is at
    // x_d = -0.85 + 0.3 d with red (d + 1) / 8, so under MAX blending the
    // band [x_d, x_d+1) holds exactly draw d's red: a draw that read another
    // DISCARD's bytes leaves its band with the wrong red (or none).
    const auto x_of = [](u32 d) { return -0.85f + 0.3f * static_cast<float>(d); };
    const auto red_of = [](u32 d) { return static_cast<float>(d + 1) / 8.0f; };
    const auto draw_points = [&](u32 d) {
        return std::vector<float>{x_of(d), 0.0f, 0.5f, 1.0f, red_of(d), 0.0f, 0.0f, 1.0f};
    };
    const auto scene = [&] {
        const std::array<float, 4> clear{0, 0, 0, 0};
        context_call(50, {rtv, put(clear)});
        auto hold = dev.create_fence(0);
        EXPECT_TRUE(hold && hold->valid());
        EXPECT_TRUE(hold && hold->wait_gpu(1));
        for (u32 d = 0; d < kDraws; ++d) {
            const auto v = draw_points(d);
            const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
            EXPECT_EQ(context_call(14, {vb, 0, 4 /* WRITE_DISCARD */, 0, mapped}),
                      d3d11::S_OK);
            const GuestAddr data = memory.read_u64(mapped).value();
            EXPECT_NE(data, 0u);
            if (data != 0) {
                EXPECT_TRUE(memory.write_bytes(data, reinterpret_cast<const Byte*>(v.data()), kBytes));
            }
            context_call(15, {vb, 0});         // Unmap
            context_call(13, {kPointsPerDraw, 0});  // Draw
        }
        if (hold) EXPECT_TRUE(hold->signal_cpu(1));
        auto texels = read_texels(tex, kW, kH, 28);
        graphics::MetalSubmitQueue::instance().drain();
        return texels;
    };
    std::vector<u32> reference;
    for (const bool ring : {false, true}) {
        SCOPED_TRACE(ring ? "ring (VITREO_WS6 on)" : "shadow (VITREO_WS6 off)");
        d3d11_ws6_override_for_test(ring);
        const auto arms = both_paths(scene);
        expect_same(arms, true, "DYNAMIC VB, WRITE_DISCARD before every GS draw");
        // Every draw read its own DISCARD: the first column whose centre is
        // right of x_d (and left of x_{d+1}) holds draw d's red.
        for (u32 d = 0; d < kDraws; ++d) {
            const u32 px = static_cast<u32>((x_of(d) + 1.0f) * 0.5f * kW) + 1;
            const u32 want = static_cast<u32>(red_of(d) * 255.0f + 0.5f);
            for (const auto* arm : {&arms.three_pass, &arms.mesh}) {
                const u32 texel = (*arm)[(kH / 2) * kW + px];
                const u32 red = texel & 0xFFu;
                EXPECT_LE(red > want ? red - want : want - red, 1u)
                    << "draw " << d << " column " << px << " texel " << std::hex << texel
                    << " on the " << (arm == &arms.mesh ? "mesh" : "three-pass") << " path";
            }
        }
        if (reference.empty()) {
            reference = arms.three_pass;
        } else {
            EXPECT_EQ(arms.three_pass, reference) << "ring and shadow differ";
        }
    }
    d3d11_ws6_override_for_test(std::nullopt);
}

// ---- Task 10: the GPU-time gate (VITREO_WS5_BENCH=1) -------------------------

TEST_F(D3D11GsMeshMetalTest, InstancedGsGpuTimeBench) {
    if (const char* on = std::getenv("VITREO_WS5_BENCH"); on == nullptr || on[0] != '1') {
        GTEST_SKIP() << "the WS5 GPU-time gate runs with VITREO_WS5_BENCH=1";
    }
    if (!mesh_capable()) GTEST_SKIP() << "no mesh pipelines";
    auto& dev = graphics::MetalDevice::instance();
    const GuestAddr vs = shader(12, gs5_vs_pass, sizeof(gs5_vs_pass));
    const GuestAddr gs = shader(13, gs5_gs_quad, sizeof(gs5_gs_quad));
    const GuestAddr ps = shader(15, gs5_ps_color, sizeof(gs5_ps_color));
    constexpr u32 kSize = 512;
    GuestAddr tex = 0;
    const GuestAddr rtv = render_target(kSize, kSize, tex);
    // One point, narrow quads: the plan's W1 is DrawInstanced(1, 1152) x 64.
    const std::vector<float> v{0.97f, 0.0f, 0.5f, 1.0f, 0.3f, 0.6f, 0.9f, 1.0f};
    const GuestAddr vb = buffer(v.data(), static_cast<u32>(v.size() * 4), 1);
    context_call(17, {pass_layout()});
    bind_vertices(vb);
    context_call(24, {1});
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(9, {ps, 0, 0});
    const std::array<u64, 1> rtvs{rtv};
    context_call(33, {1, put(rtvs), 0});
    set_blend(blend_state(2, 2, 5));
    viewport(kSize, kSize);
    // W1: the plan's workload. gs5_gs_quad's quads span [x, 1] x [-1, 1], so
    // all 1,152 instances cover the same ~8 x 512 pixels and the draw is
    // bound by blending into them, a cost both paths share. W1s scissors the
    // target to 8 x 8 pixels (about the probe's per-quad coverage), so the
    // geometry stage is what is measured.
    std::array<double, 2> ratios{}, stage_ratios{};
    for (int workload = 0; workload < 2; ++workload) {
    const char* name = workload == 0 ? "W1" : "W1s";
    if (workload == 1) {
        const std::array<u32, 10> rs{3, 1, 0, 0, 0, 0, 1, 1, 0, 0};  // SOLID, NONE, scissor
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
        context_call(43, {memory.read_u64(out).value()});
        const std::array<i32, 4> rect{kSize - 8, kSize / 2 - 4, kSize, kSize / 2 + 4};
        context_call(45, {1, put(rect)});
    }
    const u32 draws = 64;
    auto& queue = graphics::MetalSubmitQueue::instance();
    struct Counters {
        u64 continued, restarted, reopened, folded, split;
    };
    const auto counters = [&] {
        return Counters{queue.passes_continued(), queue.passes_restarted(),
                        queue.draws_reopened(), queue.passes_folded(), queue.folds_split()};
    };
    struct Sample {
        double gpu_ms, cpu_ms;
        Counters delta;
    };
    // Arm 0: three-pass, 1: mesh, 2: the same draws with no geometry shader
    // (the fixed per-draw cost both GS arms share).
    const auto rep = [&](int arm) {
        gs_mesh::force_for_test(arm == 1 ? Force::Mesh : Force::ThreePass);
        context_call(23, {arm == 2 ? 0 : gs, 0, 0});
        dev.flush();
        EXPECT_TRUE(dev.wait_idle());
        const Counters c0 = counters();
        const auto w0 = std::chrono::steady_clock::now();
        const auto t0 = dev.record_timestamp();
        for (u32 d = 0; d < draws; ++d) context_call(21, {1, 1152, 0, 0});
        const auto t1 = dev.record_timestamp();
        const auto w1 = std::chrono::steady_clock::now();
        dev.flush();
        EXPECT_TRUE(dev.wait_idle());
        const Counters c1 = counters();
        Sample out{};
        out.gpu_ms = (t0 && t1) ? static_cast<double>(t1->gpu_time_ns() - t0->gpu_time_ns()) / 1e6
                                : 0.0;
        out.cpu_ms = std::chrono::duration<double, std::milli>(w1 - w0).count();
        out.delta = {c1.continued - c0.continued, c1.restarted - c0.restarted,
                     c1.reopened - c0.reopened, c1.folded - c0.folded, c1.split - c0.split};
        return out;
    };
    for (int w = 0; w < 2; ++w) {
        for (int arm = 0; arm < 3; ++arm) (void)rep(arm);
    }
    const u32 reps = [] {
        const char* r = std::getenv("VITREO_WS5_BENCH_REPS");
        return r ? static_cast<u32>(std::max(1, std::atoi(r))) : 21u;
    }();
    std::vector<Sample> samples[3];
    for (u32 r = 0; r < reps; ++r) {
        for (int arm = 0; arm < 3; ++arm) samples[arm].push_back(rep(arm));
    }
    gs_mesh::force_for_test(Force::None);
    context_call(23, {gs, 0, 0});
    const auto median = [](std::vector<double> x) {
        std::sort(x.begin(), x.end());
        return x[x.size() / 2];
    };
    const auto med = [&](int arm, bool gpu) {
        std::vector<double> x;
        for (const Sample& s : samples[arm]) x.push_back(gpu ? s.gpu_ms : s.cpu_ms);
        return median(x);
    };
    const double m3 = med(0, true), mm = med(1, true), mb = med(2, true);
    double load[3] = {0, 0, 0};
    (void)getloadavg(load, 3);
    std::printf("[ws5-bench] %s 64 x DrawInstanced(1, 1152) Max blend 512x512, %u reps, "
                "load1=%.1f: three-pass %.3f ms, mesh %.3f ms, ratio %.2fx\n",
                name, reps, load[0], m3, mm, m3 / mm);
    std::printf("[ws5-bench] %s no-GS floor %.3f ms; GS-stage-only ratio (three-floor)/(mesh-floor) "
                "= %.2fx\n", name, mb, (m3 - mb) / std::max(mm - mb, 1e-6));
    const char* names[3] = {"three-pass", "mesh", "no-GS"};
    for (int arm = 0; arm < 3; ++arm) {
        const Counters& d = samples[arm].back().delta;
        std::printf("[ws5-bench] %s %-10s gpu %.3f ms cpu %.3f ms (%.1f us/draw cpu); per frame: "
                    "continued %llu folded %llu split %llu restarted %llu reopened %llu\n",
                    name, names[arm], med(arm, true), med(arm, false), med(arm, false) * 1000.0 / draws,
                    (unsigned long long)d.continued, (unsigned long long)d.folded,
                    (unsigned long long)d.split, (unsigned long long)d.restarted,
                    (unsigned long long)d.reopened);
    }
    ratios[workload] = m3 / mm;
    stage_ratios[workload] = (m3 - mb) / std::max(mm - mb, 1e-6);
    EXPECT_GT(mm, 0.0);
    }
    // The keep rule on the workload whose cost is the geometry stage; W1 is
    // reported (its blend-bound share dilutes any GS-stage change).
    EXPECT_GE(ratios[1], 1.2) << "WS5 keep rule: the mesh path must be >= 1.2x (W1s)";
    std::printf("[ws5-bench] summary: W1 %.2fx (GS-stage %.2fx), W1s %.2fx (GS-stage %.2fx)\n",
                ratios[0], stage_ratios[0], ratios[1], stage_ratios[1]);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
