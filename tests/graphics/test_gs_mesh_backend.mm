// SPDX-License-Identifier: LGPL-2.1-only
//
// WS5 on a real Metal device: the mesh limits against what Metal accepts
// (Task 2), and every in-tree SM5 geometry shader compiled to an
// object+mesh library and a mesh pipeline (Task 6).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include "graphics/shaders/msl_mesh_emit.hpp"
#include "gs_sm5_shaders.h"

#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "test_gs_mesh_backend.mm is ARC"
#endif

using namespace vitreo;
using namespace vitreo::graphics;

std::string gs_mesh_msl_for_test(const unsigned char* blob, usize size,
                                 GsMeshLayout* layout_out);

namespace {

id<MTLDevice> mesh_device() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7]) return nil;
    return device;
}

const char* kFlatFragment =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "fragment float4 vitreo_test_fragment() { return float4(1.0); }\n";

/// A mesh pipeline from `mesh_lib` (object + mesh functions) and a constant
/// fragment; nil with `error` set when Metal refuses.
id<MTLRenderPipelineState> mesh_pipeline(id<MTLDevice> device, id<MTLLibrary> mesh_lib,
                                         NSString* object, NSString* mesh,
                                         u32 mesh_threads, NSError** error) {
    id<MTLLibrary> frag = [device newLibraryWithSource:@(kFlatFragment) options:nil error:error];
    if (frag == nil) return nil;
    MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
    d.objectFunction = object ? [mesh_lib newFunctionWithName:object] : nil;
    d.meshFunction = [mesh_lib newFunctionWithName:mesh];
    d.fragmentFunction = [frag newFunctionWithName:@"vitreo_test_fragment"];
    d.payloadMemoryLength = kMeshPayloadBytes;
    d.maxTotalThreadsPerObjectThreadgroup = 1;
    d.maxTotalThreadsPerMeshThreadgroup = mesh_threads;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (d.meshFunction == nil) return nil;
    return [device newRenderPipelineStateWithMeshDescriptor:d
                                                    options:MTLPipelineOptionNone
                                                 reflection:nil
                                                      error:error];
}

}  // namespace

TEST(GsMeshLimits, FitAgreesWithMetal) {
    id<MTLDevice> device = mesh_device();
    if (device == nil) GTEST_SKIP() << "mesh shading needs MTLGPUFamilyApple7";
    for (const u32 registers : {1u, 2u, 4u, 8u, 16u, 30u}) {
        for (const u32 topology : {1u, 3u, 5u}) {
            for (const u32 maxv : {1u, 2u, 3u, 4u, 8u, 16u, 32u, 64u}) {
                std::vector<GsPassthroughOutput> outputs;
                for (u32 r = 0; r < registers; ++r) {
                    GsPassthroughOutput o{};
                    o.register_num = r;
                    o.is_position = r == 0;
                    outputs.push_back(o);
                }
                const GsMeshFit fit = gs_mesh_fit(outputs, topology, maxv);
                if (!fit.fits) continue;
                GsMeshLayout layout{};
                layout.outputs = outputs;
                layout.output_topology = topology;
                layout.vertices_per_invocation =
                    gs_vertices_per_invocation(topology, maxv);
                layout.invocations_per_group = fit.invocations_per_group;
                layout.record_vec4s = registers;
                // The real types and epilogue over a threadgroup scratch of
                // the fit's size, as the translator declares it.
                std::string msl = "#include <metal_stdlib>\nusing namespace metal;\n";
                for (const auto& l : gs_mesh_preamble(layout)) msl += l + "\n";
                msl += "[[mesh]] void main0(" + gs_mesh_entry_parameters(layout) +
                       "constant float* vitreo_push [[buffer(0)]])\n{\n";
                const u32 k = fit.invocations_per_group;
                msl += "    threadgroup float4 scratch[" +
                       std::to_string(k * layout.vertices_per_invocation * registers) +
                       "];\n    threadgroup uint counts[" + std::to_string(k) + "];\n";
                msl += "    counts[vitreo_tid] = 0u;\n"
                       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n";
                for (const auto& l : gs_mesh_epilogue(layout, "scratch", "counts",
                                                      "vitreo_push[0]")) {
                    msl += l + "\n";
                }
                msl += "}\n";
                NSError* err = nil;
                id<MTLLibrary> lib = [device newLibraryWithSource:@(msl.c_str())
                                                          options:nil
                                                            error:&err];
                ASSERT_NE(lib, nil) << registers << " regs, topology " << topology
                                    << ", maxvertexcount " << maxv << ": "
                                    << (err ? err.localizedDescription.UTF8String : "");
                id<MTLRenderPipelineState> pso = mesh_pipeline(
                    device, lib, @(kGsMeshObjectEntryPoint), @"main0", k, &err);
                EXPECT_NE(pso, nil) << registers << " regs, topology " << topology
                                    << ", maxvertexcount " << maxv << " (K " << k
                                    << ", mesh " << fit.mesh_bytes << " B): "
                                    << (err ? err.localizedDescription.UTF8String : "");
            }
        }
    }
    // One past the vertex limit is refused by the Metal compiler itself.
    const char* over =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; };\n"
        "struct P { bool c [[primitive_culled]]; };\n"
        "[[mesh]] void m(metal::mesh<V, P, 257, 1, metal::topology::point> o) {}\n";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(over) options:nil error:&err];
    EXPECT_EQ(lib, nil) << "a 257-vertex mesh compiled";
}

TEST(GsMeshEmit, EveryGsSm5ShaderCompilesToAMeshLibrary) {
    id<MTLDevice> device = mesh_device();
    if (device == nil) GTEST_SKIP() << "mesh shading needs MTLGPUFamilyApple7";
    const struct {
        const char* name;
        const unsigned char* blob;
        usize size;
    } shaders[] = {
        {"gs5_gs_quad", gs5_gs_quad, sizeof(gs5_gs_quad)},
        {"gs5_gs_layered", gs5_gs_layered, sizeof(gs5_gs_layered)},
        {"gs5_gs_triadj", gs5_gs_triadj, sizeof(gs5_gs_triadj)},
        {"gs5_gs_lineadj", gs5_gs_lineadj, sizeof(gs5_gs_lineadj)},
        {"gs5_gs_tri_pass", gs5_gs_tri_pass, sizeof(gs5_gs_tri_pass)},
    };
    for (const auto& s : shaders) {
        GsMeshLayout layout{};
        const std::string msl = gs_mesh_msl_for_test(s.blob, s.size, &layout);
        ASSERT_FALSE(msl.empty()) << s.name << ": translation or MSL failed";
        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(msl.c_str())
                                                  options:nil
                                                    error:&err];
        ASSERT_NE(lib, nil) << s.name << ": "
                            << (err ? err.localizedDescription.UTF8String : "") << "\n"
                            << msl;
        id<MTLRenderPipelineState> pso =
            mesh_pipeline(device, lib, @(kGsMeshObjectEntryPoint), @"main0",
                          layout.invocations_per_group, &err);
        EXPECT_NE(pso, nil) << s.name << ": "
                            << (err ? err.localizedDescription.UTF8String : "");
    }
}

// ---- Tasks 3 and 4: the backend contract on Metal ---------------------------

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_mesh_pipeline.hpp"

namespace {

GraphicsBackendDevice& backend() { return MetalDevice::instance(); }

std::string u(unsigned v) { return std::to_string(v) + "u"; }

/// Object fn launching [[buffer(0)]] mesh groups in chunks of 1024, and a
/// one-thread mesh fn per group: TRIANGLE reads three float4 positions from
/// the expansion window (records 3g..3g+2); POINT draws one pixel per group
/// at column g of a `width`-wide target.
std::string mesh_test_library(bool triangle, unsigned width) {
    std::string s = "#include <metal_stdlib>\nusing namespace metal;\n";
    s += std::string("struct V { float4 p [[position]];") +
         (triangle ? "" : " float size [[point_size]];") + " };\n"
         "struct P { bool c [[primitive_culled]]; };\n"
         "struct Payload { uint base; };\n";
    s += std::string("using M = metal::mesh<V, P, ") + (triangle ? "3, 1, metal::topology::triangle"
                                                             : "1, 1, metal::topology::point") + ">;\n";
    s += "[[object]] void obj(object_data Payload& pl [[payload]], mesh_grid_properties g,\n"
         "    uint otg [[threadgroup_position_in_grid]], constant uint& total [[buffer(0)]]) {\n"
         "    pl.base = otg * 1024u;\n"
         "    g.set_threadgroups_per_grid(uint3(min(1024u, total - pl.base), 1, 1));\n}\n";
    s += "[[mesh]] void msh(M m, const object_data Payload& pl [[payload]],\n"
         "    uint3 tg [[threadgroup_position_in_grid]],\n"
         "    const device float4* rec [[buffer(" + std::to_string(BACKEND_GS_EXPANSION_BUFFER_BASE) + ")]]) {\n"
         "    const uint group = pl.base + tg.x;\n";
    if (triangle) {
        s += "    for (uint i = 0; i < 3u; ++i) { V v; v.p = rec[group * 3u + i];\n"
             "        m.set_vertex(i, v); m.set_index(i, uchar(i)); }\n";
    } else {
        s += "    V v; v.p = float4((float(group) + 0.5) / " + std::to_string(width) +
             ".0 * 2.0 - 1.0, 0.0, 0.5, 1.0); v.size = 1.0;\n"
             "    m.set_vertex(0, v); m.set_index(0, 0);\n";
    }
    s += "    P q; q.c = false; m.set_primitive(0, q); m.set_primitive_count(1);\n}\n";
    return s;
}

const char* kRedFragment =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "fragment float4 red() { return float4(1.0, 0.0, 0.0, 1.0); }\n";

std::unique_ptr<BackendPipeline> test_mesh_pipeline(bool triangle, unsigned width,
                                                    const char* mesh_entry = "msh") {
    BackendMeshPipelineDesc d{};
    d.render.vertex_shader_msl = mesh_test_library(triangle, width);
    d.render.fragment_shader_msl = kRedFragment;
    d.render.fragment_entry_point = "red";
    d.render.color_format = PixelFormat::RGBA8Unorm;
    d.object_entry_point = "obj";
    d.mesh_entry_point = mesh_entry;
    d.threads_per_mesh_threadgroup = 1;
    return backend().create_mesh_pipeline(d);
}

std::unique_ptr<BackendTexture2D> target(unsigned w, unsigned h) {
    BackendTexture2DDesc td{};
    td.width = w;
    td.height = h;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto t = backend().create_texture_2d(td, nullptr, 0);
    if (t) (void)t->clear_render_target_level({0.0f, 0.0f, 0.0f, 0.0f}, 0, 0, 1);
    return t;
}

}  // namespace

TEST(GsMeshBackend, CreatesAndRefusesHonestly) {
    if (!backend().available() || !backend().supports_mesh_pipelines()) {
        GTEST_SKIP() << "mesh shading needs MTLGPUFamilyApple7";
    }
    auto good = test_mesh_pipeline(true, 1);
    ASSERT_TRUE(good);
    EXPECT_TRUE(good->valid());
    EXPECT_TRUE(good->is_mesh());
    auto bad = test_mesh_pipeline(true, 1, "no_such_mesh_function");
    ASSERT_TRUE(bad);  // returned so the caller can read why
    EXPECT_FALSE(bad->valid());
    EXPECT_FALSE(static_cast<const MetalMeshPipeline*>(bad.get())->failure().empty());
}

TEST(GsMeshBackend, InPassCaptureThenMeshReadsWhatVertexWrote) {
    if (!backend().available() || !backend().supports_mesh_pipelines()) {
        GTEST_SKIP() << "mesh shading needs MTLGPUFamilyApple7";
    }
    // A raster-off vertex pipeline copying b0's three positions into the
    // capture buffer, as an in-pass capture does.
    BackendPipelineDesc cap{};
    cap.vertex_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex void capture(uint vid [[vertex_id]],\n"
        "    constant float4* src [[buffer(" + std::to_string(BACKEND_CONSTANT_BUFFER_BASE) + ")]],\n"
        "    device float4* out [[buffer(" + std::to_string(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX) + ")]]) {\n"
        "    out[vid] = src[vid];\n}\n";
    cap.vertex_entry_point = "capture";
    cap.rasterization_enabled = false;
    cap.color_format = PixelFormat::RGBA8Unorm;
    cap.cache_key = 0x5755'0001u;
    auto capture = backend().create_pipeline(cap);
    ASSERT_TRUE(capture && capture->valid());
    auto mesh = test_mesh_pipeline(true, 1);
    ASSERT_TRUE(mesh && mesh->valid());

    // Left triangle around x -0.5, right one around x +0.5 (64x64 target).
    const std::array<float, 12> left{-0.9f, -0.5f, 0.5f, 1.0f, -0.1f, -0.5f, 0.5f, 1.0f,
                                     -0.5f, 0.5f, 0.5f, 1.0f};
    const std::array<float, 12> right{0.1f, -0.5f, 0.5f, 1.0f, 0.9f, -0.5f, 0.5f, 1.0f,
                                      0.5f, 0.5f, 0.5f, 1.0f};
    BackendBufferDesc cb{};
    cb.byte_width = 256;
    cb.constant_buffer = true;
    std::array<float, 64> left_data{}, right_data{};
    std::copy(left.begin(), left.end(), left_data.begin());
    std::copy(right.begin(), right.end(), right_data.begin());
    auto left_cb = backend().create_buffer(cb, left_data.data());
    auto right_cb = backend().create_buffer(cb, right_data.data());
    BackendBufferDesc rec{};
    rec.byte_width = 48;
    rec.shader_resource = rec.unordered_access = true;
    auto records = backend().create_buffer(rec, nullptr);
    auto rt = target(64, 64);
    ASSERT_TRUE(left_cb && right_cb && records && rt);

    BackendRenderPassDesc pd{};
    pd.color_targets[0] = rt.get();
    pd.color_formats[0] = PixelFormat::RGBA8Unorm;
    pd.preserve_color_contents = true;
    auto pass = backend().create_render_pass_standalone(pd);
    ASSERT_TRUE(pass);
    for (const BackendBuffer* source : {left_cb.get(), right_cb.get()}) {
        BackendDrawCall c{};
        c.pipeline = capture.get();
        c.topology = BackendPrimitiveTopology::PointList;
        c.vertex_count = 3;
        c.vertex_constant_buffers[0] = source;
        c.vertex_capture_buffer = records.get();
        c.viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        ASSERT_TRUE(pass->draw(c));
        BackendDrawCall m{};
        BackendMeshDraw dispatch{};
        m.pipeline = mesh.get();
        dispatch.object_threadgroups = 1;
        dispatch.mesh_groups = 1;
        dispatch.expansion_buffers[0] = records.get();
        dispatch.barrier_before_mesh = true;
        m.mesh = &dispatch;
        m.viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        ASSERT_TRUE(pass->draw(m));
    }
    ASSERT_TRUE(pass->end());
    std::vector<u32> px(64 * 64, 0);
    ASSERT_TRUE(rt->readback_slice(0, 0, px.data(), 64 * 4));
    // NDC (-0.5, 0) and (0.5, 0) are pixels (16, 32) and (48, 32).
    EXPECT_EQ(px[32 * 64 + 16], 0xFF0000FFu) << "the first draw's triangle";
    EXPECT_EQ(px[32 * 64 + 48], 0xFF0000FFu) << "the second draw's triangle";
    EXPECT_EQ(px[2 * 64 + 2], 0u);
    EXPECT_EQ(px[2 * 64 + 61], 0u);
}

TEST(GsMeshBackend, MeshGridOver1024GroupsChunksThroughTheObjectStage) {
    if (!backend().available() || !backend().supports_mesh_pipelines()) {
        GTEST_SKIP() << "mesh shading needs MTLGPUFamilyApple7";
    }
    constexpr unsigned kGroups = 3000;
    auto mesh = test_mesh_pipeline(false, kGroups);
    ASSERT_TRUE(mesh && mesh->valid());
    auto rt = target(kGroups, 1);
    ASSERT_TRUE(rt);
    BackendRenderPassDesc pd{};
    pd.color_targets[0] = rt.get();
    pd.color_formats[0] = PixelFormat::RGBA8Unorm;
    pd.preserve_color_contents = true;
    auto pass = backend().create_render_pass_standalone(pd);
    ASSERT_TRUE(pass);
    BackendDrawCall m{};
    m.pipeline = mesh.get();
    m.topology = BackendPrimitiveTopology::PointList;
    BackendMeshDraw dispatch{};
    dispatch.mesh_groups = kGroups;
    dispatch.object_threadgroups = (kGroups + 1023) / 1024;
    m.mesh = &dispatch;
    m.viewport = {0.0f, 0.0f, float(kGroups), 1.0f, 0.0f, 1.0f};
    ASSERT_TRUE(pass->draw(m) && pass->end());
    std::vector<u32> px(kGroups, 0);
    ASSERT_TRUE(rt->readback_slice(0, 0, px.data(), kGroups * 4));
    unsigned lit = 0;
    for (u32 p : px) lit += p == 0xFF0000FFu;
    EXPECT_EQ(lit, kGroups);
}

TEST(GsMeshBackend, MeshDispatchOnAVertexPipelineIsRejected) {
    if (!backend().available()) GTEST_SKIP() << "no Metal device";
    BackendPipelineDesc cap{};
    cap.vertex_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v() { return float4(0.0); }\n";
    cap.vertex_entry_point = "v";
    cap.color_format = PixelFormat::RGBA8Unorm;
    cap.cache_key = 0x5755'0002u;
    auto vertex = backend().create_pipeline(cap);
    ASSERT_TRUE(vertex && vertex->valid());
    auto rt = target(4, 4);
    BackendRenderPassDesc pd{};
    pd.color_targets[0] = rt.get();
    pd.color_formats[0] = PixelFormat::RGBA8Unorm;
    auto pass = backend().create_render_pass_standalone(pd);
    ASSERT_TRUE(pass);
    BackendDrawCall c{};
    c.pipeline = vertex.get();
    c.vertex_count = 3;
    BackendMeshDraw dispatch{};
    dispatch.object_threadgroups = 1;
    dispatch.mesh_groups = 1;
    c.mesh = &dispatch;
    EXPECT_FALSE(pass->draw(c));
    if (backend().supports_mesh_pipelines()) {
        auto mesh = test_mesh_pipeline(true, 1);
        BackendDrawCall m{};
        m.pipeline = mesh.get();  // a mesh pipeline without a mesh dispatch
        m.vertex_count = 3;
        EXPECT_FALSE(pass->draw(m));
    }
    EXPECT_TRUE(pass->end());
}
