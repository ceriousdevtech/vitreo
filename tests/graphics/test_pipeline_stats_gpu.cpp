// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_pipeline_stats_gpu.cpp
/// @brief D3D11_QUERY_PIPELINE_STATISTICS counting on real Metal, against
///        Windows (WARP).
///
/// Every expected value is a line of
/// tools/oracle_probe/d3d11_pstats_probe.expected; the shaders are that
/// probe's fxc output (d3d11_pstats_shaders.h, d3d11_pstats.hlsl). Covered:
/// the PSInvocations-counting pixel shader variant (translated DXBC, drawn),
/// and the four counting kernels of graphics/shaders/pipeline_stats.hpp run
/// over inputs laid out as the D3D11 frontend lays them out.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/pipeline_stats.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pstats {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../oracle_data/d3d11_pstats_shaders.h"
#pragma clang diagnostic pop
}  // namespace pstats

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

MetalDevice& device() { return MetalDevice::instance(); }

std::string translate(const unsigned char* bytecode, usize size,
                      ShaderStage stage, bool count_pixels) {
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.fixed_function_push = true;
    options.count_pixel_invocations = count_pixels;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? std::string(msl.msl_text()) : std::string{};
}

std::unique_ptr<BackendBuffer> buffer(usize bytes, const void* data) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.vertex_buffer = true;
    return device().create_buffer(desc, data);
}

std::unique_ptr<BackendBuffer> record() {
    const std::array<u32, STATS_RECORD_WORDS> zero{};
    return buffer(sizeof(zero), zero.data());
}

std::array<u32, STATS_RECORD_WORDS> read_record(BackendBuffer& r) {
    std::array<u32, STATS_RECORD_WORDS> words{};
    EXPECT_TRUE(r.read(0, words.data(), sizeof(words)));
    return words;
}

u64 value(const std::array<u32, STATS_RECORD_WORDS>& words, u32 counter) {
    return stats_record_value(words.data(), counter);
}

float nx(float px) { return px / 32.0f - 1.0f; }
float ny(float py) { return 1.0f - py / 32.0f; }

/// d3d11_pstats_probe's quad(): two triangles over an NDC rect.
std::vector<float> quad(float x0, float y0, float x1, float y1) {
    const float c[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    std::vector<float> v;
    for (const auto& k : c) {
        v.insert(v.end(), {x0 + (x1 - x0) * k[0], y0 + (y1 - y0) * k[1], 0.5f,
                           1.0f});
    }
    return v;
}

/// Draws @p vertices (float4 clip-space positions) through vs_pos and @p ps
/// translated with count_pixel_invocations into a 64x64 target of
/// @p samples samples; returns PSInvocations.
u64 draw_counting(const unsigned char* ps, usize ps_size,
                  const std::vector<float>& vertices, unsigned samples) {
    auto& dev = device();
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = translate(pstats::g_vs_pos, sizeof(pstats::g_vs_pos),
                                     ShaderStage::Vertex, false);
    pd.fragment_shader_msl = translate(ps, ps_size, ShaderStage::Fragment, true);
    EXPECT_NE(pd.fragment_shader_msl.find("vitreo_pipeline_statistics"),
              std::string::npos);
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0}};
    pd.vertex_stride = pd.vertex_buffer_strides[0] = 16;
    pd.color_format = PixelFormat::RGBA8Unorm;
    pd.sample_count = samples;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0x957A7000ull + samples * 0x10000ull + ps_size;
    auto pipeline = dev.create_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << pd.fragment_shader_msl;
    if (!pipeline || !pipeline->valid()) return ~0ull;
    BackendTexture2DDesc td{};
    td.width = td.height = 64;
    td.mip_levels = td.array_size = 1;
    td.sample_count = samples;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = dev.create_texture_2d(td, nullptr, 0);
    auto vb = buffer(vertices.size() * 4, vertices.data());
    auto stats = record();
    EXPECT_TRUE(target && vb && stats);
    if (!target || !vb || !stats) return ~0ull;
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    pass_desc.preserve_color_contents = true;
    auto pass = dev.create_render_pass_standalone(pass_desc);
    EXPECT_TRUE(pass && pass->valid());
    if (!pass || !pass->valid()) return ~0ull;
    std::array<u32, kPushBlockBytes / 4> push{};
    std::array<u32, kPushBlockBytes / 4> fragment_push{};
    // Push member 5: the render target's sample count.
    fragment_push[kRasterizerSampleCountOffset / 4] = samples;
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = static_cast<unsigned>(vertices.size() / 4);
    call.instance_count = 1;
    call.vertex_buffer = vb.get();
    call.vertex_buffers[0] = vb.get();
    call.viewport = {0, 0, 64, 64, 0, 1};
    call.vertex_constants = push.data();
    call.vertex_constants_size = sizeof(push);
    call.fragment_constants = fragment_push.data();
    call.fragment_constants_size = sizeof(fragment_push);
    call.fragment_statistics_buffer = stats.get();
    EXPECT_TRUE(pass->draw(call) && pass->end());
    EXPECT_TRUE(dev.wait_idle());
    return value(read_record(*stats), STAT_PS_INVOCATIONS);
}

BackendComputePipeline* kernel(const char* entry) {
    static std::array<std::unique_ptr<BackendComputePipeline>, 4> kernels;
    const char* entries[4] = {kStatsVerticesEntryPoint,
                              kStatsPrimitivesEntryPoint,
                              kStatsGeometryEntryPoint,
                              kStatsTessellationEntryPoint};
    for (usize i = 0; i < 4; ++i) {
        if (std::strcmp(entries[i], entry) != 0) continue;
        if (!kernels[i]) {
            BackendComputePipelineDesc desc{};
            desc.compute_shader_msl = pipeline_stats_msl();
            desc.compute_entry_point = entry;
            desc.cache_key = 0x57A75000ull + i;
            kernels[i] = device().create_compute_pipeline(desc);
        }
        return kernels[i] && kernels[i]->valid() ? kernels[i].get() : nullptr;
    }
    return nullptr;
}

bool dispatch(const char* entry, const StatsKernelParams& params, u32 threads,
              u32 per_group,
              std::initializer_list<std::pair<unsigned, BackendBuffer*>>
                  buffers) {
    BackendComputePipeline* pipeline = kernel(entry);
    EXPECT_NE(pipeline, nullptr) << entry;
    if (pipeline == nullptr) return false;
    BackendDispatchCall call{};
    call.pipeline = pipeline;
    call.threads_per_group_x = per_group;
    call.group_count_x = (threads + per_group - 1) / per_group;
    call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    for (const auto& [index, b] : buffers) call.buffers[index] = b;
    return device().dispatch(call);
}

}  // namespace

// [ps]: every lane of each 2x2 quad holding a covered pixel counts, helper
// pixels included, and a discard or a failing depth test does not stop it.
TEST(PipelineStatsGpu, PixelInvocationsCountQuadsOnASingleSampleTarget) {
    if (!device().available()) GTEST_SKIP() << "no Metal device";
    const auto plain = [&](const std::vector<float>& v) {
        return draw_counting(pstats::g_ps_plain, sizeof(pstats::g_ps_plain), v,
                             1);
    };
    EXPECT_EQ(plain(quad(-1, -1, 1, 1)), 4224u) << "full quad";
    EXPECT_EQ(plain(quad(-1, -1, 0, 0)), 1088u) << "quarter quad";
    auto full = quad(-1, -1, 1, 1);
    full.resize(12);
    EXPECT_EQ(plain(full), 2112u) << "one diagonal triangle";
    EXPECT_EQ(plain({nx(10.6f), ny(10.6f), 0.5f, 1, nx(10.9f), ny(10.6f), 0.5f,
                     1, nx(10.6f), ny(10.9f), 0.5f, 1}),
              0u)
        << "tiny triangle, no pixel centre";
    EXPECT_EQ(plain({nx(10.2f), ny(10.2f), 0.5f, 1, nx(11.2f), ny(10.2f), 0.5f,
                     1, nx(10.2f), ny(11.2f), 0.5f, 1}),
              4u)
        << "triangle over one pixel centre";
    EXPECT_EQ(plain({nx(11.2f), ny(11.2f), 0.5f, 1, nx(13.4f), ny(11.2f), 0.5f,
                     1, nx(11.2f), ny(12.2f), 0.5f, 1}),
              8u)
        << "triangle over pixel centres (11,11),(12,11): two stamps";
    EXPECT_EQ(draw_counting(pstats::g_ps_discard_all,
                            sizeof(pstats::g_ps_discard_all),
                            quad(-1, -1, 1, 1), 1),
              4224u)
        << "PS discards every pixel: still invoked";
}

// On a 4x target Vitreo counts each covered pixel once (a memory-writing
// fragment function runs per sample on Metal, and its helper lanes carry no
// coverage, so a stamp's invocations cannot be joined across samples):
// WARP's 4224 less its helper pixels. The full quad's 4096 pixels plus the
// 64 on the diagonal both triangles cover a sample of (standard 4x pattern)
// are 4160; the probe's "triangle over one sample" covers 1 pixel. D3D11.3
// 20.4.7: "may include or exclude helper pixels".
TEST(PipelineStatsGpu, PixelInvocationsCountPixelsOnAMultisampledTarget) {
    if (!device().available()) GTEST_SKIP() << "no Metal device";
    if (!device().supports_texture_sample_count(4)) GTEST_SKIP() << "no 4x";
    EXPECT_EQ(draw_counting(pstats::g_ps_plain, sizeof(pstats::g_ps_plain),
                            quad(-1, -1, 1, 1), 4),
              4160u);
    EXPECT_EQ(draw_counting(pstats::g_ps_sample, sizeof(pstats::g_ps_sample),
                            quad(-1, -1, 1, 1), 4),
              4160u)
        << "a sample-frequency shader counts pixels too (WARP)";
    EXPECT_EQ(draw_counting(pstats::g_ps_plain, sizeof(pstats::g_ps_plain),
                            {nx(10.55f), ny(10.0f), 0.5f, 1, nx(11.0f),
                             ny(10.0f), 0.5f, 1, nx(11.0f), ny(10.9f), 0.5f,
                             1},
                            4),
              1u);
}

// [clip]: vitreo_stats_primitives over captured positions (key mode 1: the
// CPU's keys of instance 0, a draw-relative capture with its header).
TEST(PipelineStatsGpu, ClipperKernelMatchesWarp) {
    if (!device().available()) GTEST_SKIP() << "no Metal device";
    struct Case {
        const char* label;
        std::array<float, 12> v;
        u32 primitives;
        bool depth_clip;
    };
    const std::vector<Case> cases{
        {"inside", {-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 0.5f, 1, 0, 0.5f, 0.5f, 1}, 1, true},
        {"one vertex past +x", {0, -0.5f, 0.5f, 1, 1.6f, -0.2f, 0.5f, 1, 0, 0.5f, 0.5f, 1}, 2, true},
        {"two vertices past +x", {0.3f, 0.1f, 0.5f, 1, 1.6f, -0.5f, 0.5f, 1, 1.7f, 0.6f, 0.5f, 1}, 1, true},
        {"corner cut, one vertex past +x and +y", {0.2f, 0.3f, 0.5f, 1, 1.5f, 1.4f, 0.5f, 1, -0.1f, 0.6f, 0.5f, 1}, 3, true},
        {"fullscreen triangle", {-1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1}, 3, true},
        {"huge triangle", {-1, -1, 0.5f, 1, 100, -1, 0.5f, 1, -1, 100, 0.5f, 1}, 2, true},
        {"one vertex z>w", {-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 1.5f, 1, 0, 0.5f, 0.5f, 1}, 2, true},
        {"one vertex z>w, DepthClipEnable FALSE", {-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 1.5f, 1, 0, 0.5f, 0.5f, 1}, 1, false},
        {"one vertex w<0", {-0.5f, -0.5f, 0.5f, 1, 0.5f, -0.5f, 0.5f, 1, 0.1f, 0.2f, 0.5f, -1}, 2, true},
        {"fully past +x", {1.5f, -0.5f, 0.5f, 1, 2.5f, -0.5f, 0.5f, 1, 2, 0.5f, 0.5f, 1}, 0, true},
        {"outside, no single plane", {0.8f, 1.5f, 0.5f, 1, 1.5f, 0.8f, 0.5f, 1, 1.6f, 1.6f, 0.5f, 1}, 0, true},
        {"edge on x=w", {1, -0.5f, 0.5f, 1, 1, 0.5f, 0.5f, 1, 0, 0, 0.5f, 1}, 1, true},
        {"zero area, collinear, crossing +x", {0, 0, 0.5f, 1, 1, 0.5f, 0.5f, 1, 2, 1, 0.5f, 1}, 2, true},
    };
    for (const Case& c : cases) {
        // Header {stride 3, capacity 3}, then one vec4 record per vertex.
        std::vector<u32> capture(4 + 12, 0u);
        capture[0] = 3;
        capture[1] = 3;
        std::memcpy(capture.data() + 4, c.v.data(), sizeof(c.v));
        const std::array<u32, 3> ids{0, 1, 2};
        auto records = buffer(capture.size() * 4, capture.data());
        auto keys = buffer(sizeof(ids), ids.data());
        auto stats = record();
        StatsKernelParams p{};
        p.vertices_per_primitive = 3;
        p.flags = STATS_FLAG_CLIP | (c.depth_clip ? STATS_FLAG_DEPTH_CLIP : 0u);
        p.key_mode = 1;
        p.primitives_per_instance = 1;
        p.instances = 1;
        p.key_stride = 3;
        p.record_vec4s = 1;
        p.record_base = 1;
        p.position_slot = 0;
        p.capacity = 1;
        ASSERT_TRUE(dispatch(kStatsPrimitivesEntryPoint, p, 1, STATS_THREADS,
                             {{STATS_RECORD_INDEX, stats.get()},
                              {STATS_COUNTS_INDEX, stats.get()},
                              {STATS_IDS_INDEX, keys.get()},
                              {STATS_RECORDS_INDEX, records.get()}}));
        ASSERT_TRUE(device().wait_idle());
        EXPECT_EQ(value(read_record(*stats), STAT_C_PRIMITIVES), c.primitives)
            << c.label;
    }
}

// [vs] / [vsbatch] / [indirect]: vitreo_ia_resolve assembles an index buffer
// on the GPU and vitreo_stats_vertices counts WARP's reuse batches.
TEST(PipelineStatsGpu, VertexKernelCountsReuseBatchesFromAResolve) {
    if (!device().available()) GTEST_SKIP() << "no Metal device";
    BackendComputePipelineDesc rd{};
    rd.compute_shader_msl = ia_resolve_msl();
    rd.compute_entry_point = kIaResolveEntryPoint;
    rd.cache_key = 0x57A75100ull;
    auto resolve = device().create_compute_pipeline(rd);
    ASSERT_TRUE(resolve && resolve->valid());
    struct Case {
        const char* label;
        std::vector<u32> indices;
        u32 vpp, assembly, instances;
        u64 ia_vertices, ia_primitives, vs;
    };
    std::vector<u32> grid;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 a = y * 17 + x, b = a + 1, c = a + 17, d = c + 1;
            grid.insert(grid.end(), {a, b, c, c, b, d});
        }
    }
    const std::vector<Case> cases{
        {"DrawIndexedInstancedIndirect 17x17 grid", grid, 3, 0, 1, 1536, 512, 328},
        {"trilist x1533", std::vector<u32>(1533, 0u), 3, 0, 1, 1533, 511, 4},
        {"tristrip x1024", std::vector<u32>(1024, 0u), 3, 1, 1, 1024, 1022, 7},
        {"{0,1,2,2,1,5} x3", {0, 1, 2, 2, 1, 5}, 3, 0, 3, 18, 6, 12},
    };
    for (const Case& c : cases) {
        auto ib = buffer(c.indices.size() * 4, c.indices.data());
        const std::array<u32, 5> args{static_cast<u32>(c.indices.size()),
                                      c.instances, 0, 0, 0};
        auto arguments = buffer(sizeof(args), args.data());
        std::vector<u32> zero(IA_RESOLVED_WORDS, 0u);
        auto resolved = buffer(zero.size() * 4, zero.data());
        const u32 capacity =
            static_cast<u32>(c.indices.size()) * c.instances;
        std::vector<u32> ids_init(capacity * c.vpp, 0u);
        auto ids = buffer(ids_init.size() * 4, ids_init.data());
        auto header = buffer(16, zero.data());
        auto stats = record();
        IaResolveParams params{};
        params.indexed = 1;
        params.index_32bit = 1;
        params.index_elements = static_cast<u32>(c.indices.size());
        params.vertices_per_primitive = c.vpp;
        params.assembly = c.assembly;
        params.record_capacity = 0xFFFFFFFFu;
        params.primitive_capacity = capacity;
        BackendDispatchCall call{};
        call.pipeline = resolve.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = IA_RESOLVE_THREADS;
        call.constants = &params;
        call.constants_size = sizeof(params);
        call.buffers[IA_RESOLVE_ARGS_INDEX] = arguments.get();
        call.buffers[IA_RESOLVE_INDICES_INDEX] = ib.get();
        call.buffers[IA_RESOLVE_IDS_INDEX] = ids.get();
        call.buffers[IA_RESOLVE_OUT_INDEX] = resolved.get();
        call.buffers[IA_RESOLVE_CAPTURE_INDEX] = header.get();
        ASSERT_TRUE(device().dispatch(call));
        StatsKernelParams p{};
        p.vertices_per_primitive = c.vpp;
        p.batch = stats_vertex_batch(c.vpp);
        p.flags = STATS_FLAG_ADD_IA_VERTICES | STATS_FLAG_ADD_IA_PRIMITIVES |
                  STATS_FLAG_ADD_VS_INVOCATIONS | STATS_FLAG_ADD_C_INVOCATIONS;
        const u32 groups = (capacity + p.batch - 1) / p.batch;
        ASSERT_TRUE(dispatch(kStatsVerticesEntryPoint, p,
                             groups * STATS_BATCH_THREADS, STATS_BATCH_THREADS,
                             {{STATS_RECORD_INDEX, stats.get()},
                              {STATS_COUNTS_INDEX, resolved.get()},
                              {STATS_IDS_INDEX, ids.get()},
                              {STATS_ARGS_INDEX, arguments.get()}}));
        ASSERT_TRUE(device().wait_idle());
        const auto words = read_record(*stats);
        EXPECT_EQ(value(words, STAT_IA_VERTICES), c.ia_vertices) << c.label;
        EXPECT_EQ(value(words, STAT_IA_PRIMITIVES), c.ia_primitives) << c.label;
        EXPECT_EQ(value(words, STAT_VS_INVOCATIONS), c.vs) << c.label;
        EXPECT_EQ(value(words, STAT_C_INVOCATIONS), c.ia_primitives) << c.label;
        EXPECT_EQ(words[STATS_FLAGS_WORD], 0u) << c.label;
    }
}

// [gs]: vitreo_stats_geometry over an expansion's counter block and records.
TEST(PipelineStatsGpu, GeometryKernelCountsEveryStream) {
    if (!device().available()) GTEST_SKIP() << "no Metal device";
    const auto run = [&](u32 vpp, u32 max_vertices, u32 topology,
                         u32 stream_mask, u32 raster,
                         std::array<u32, 2> written) {
        constexpr u32 kInvocations = 2;  // the full quad's two triangles
        const u32 vpi = gs_vertices_per_invocation(topology, max_vertices);
        std::vector<u32> counter(kGsInvocationCountBase + 4 * kInvocations, 0u);
        counter[kGsControlStreamStride] = kInvocations;
        for (u32 s = 0; s < 2; ++s) {
            for (u32 i = 0; i < kInvocations; ++i) {
                counter[kGsInvocationCountBase + s * kInvocations + i] =
                    written[s];
            }
        }
        // Every record a position inside the target.
        std::vector<float> records(4 * 4 * kInvocations * vpi, 0.0f);
        for (usize r = 0; r < records.size(); r += 4) {
            records[r] = 0.25f * static_cast<float>(r % 3);
            records[r + 1] = -0.25f;
            records[r + 2] = 0.5f;
            records[r + 3] = 1.0f;
        }
        auto counts = buffer(counter.size() * 4, counter.data());
        auto expanded = buffer(records.size() * 4, records.data());
        auto stats = record();
        StatsKernelParams p{};
        p.vertices_per_primitive = vpp;
        p.invocations = kInvocations;
        p.gs_instances = 1;
        p.vertices_per_invocation = vpi;
        p.stream_mask = stream_mask;
        p.raster_stream = raster;
        p.record_vec4s = 1;
        p.flags = raster < 4 ? STATS_FLAG_CLIP | STATS_FLAG_DEPTH_CLIP : 0u;
        p.capacity = kInvocations;
        EXPECT_TRUE(dispatch(kStatsGeometryEntryPoint, p, kInvocations,
                             STATS_THREADS,
                             {{STATS_RECORD_INDEX, stats.get()},
                              {STATS_COUNTS_INDEX, counts.get()},
                              {STATS_RECORDS_INDEX, expanded.get()}}));
        EXPECT_TRUE(device().wait_idle());
        return read_record(*stats);
    };
    // "5-vertex strip, cut, 3-vertex strip": 3 + 1 triangles an invocation,
    // written as 12 list vertices: GS 2 GSP 8 C 8 CP 8.
    auto w = run(3, 8, 5, 1, 0, {12, 0});
    EXPECT_EQ(value(w, STAT_GS_INVOCATIONS), 2u);
    EXPECT_EQ(value(w, STAT_GS_PRIMITIVES), 8u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 8u);
    EXPECT_EQ(value(w, STAT_C_PRIMITIVES), 8u);
    // "2 vertices + cut (incomplete)": nothing written, GS 2 GSP 0 C 0.
    w = run(3, 3, 5, 1, 0, {0, 0});
    EXPECT_EQ(value(w, STAT_GS_INVOCATIONS), 2u);
    EXPECT_EQ(value(w, STAT_GS_PRIMITIVES), 0u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 0u);
    // "two streams, SO both, rasterized 0 / 1 / none": one point to stream
    // 0 and two to stream 1 an invocation -- GSP 6 always, C 2 / 4 / 0.
    w = run(1, 3, 1, 3, 0, {1, 2});
    EXPECT_EQ(value(w, STAT_GS_PRIMITIVES), 6u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 2u);
    EXPECT_EQ(value(w, STAT_C_PRIMITIVES), 2u);
    w = run(1, 3, 1, 3, 1, {1, 2});
    EXPECT_EQ(value(w, STAT_GS_PRIMITIVES), 6u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 4u);
    w = run(1, 3, 1, 3, 4, {1, 2});
    EXPECT_EQ(value(w, STAT_GS_PRIMITIVES), 6u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 0u);
    EXPECT_EQ(value(w, STAT_C_PRIMITIVES), 0u);
}

// [tess]: vitreo_stats_tessellation over a software-tessellator batch.
TEST(PipelineStatsGpu, TessellationKernelCountsEveryPrimitiveVertex) {
    if (!device().available()) GTEST_SKIP() << "no Metal device";
    const auto run = [&](u32 vpp, u32 primitives, bool rasterized) {
        std::vector<u32> block(16, 0u);
        block[STATS_TESS_BLOCK_INDICES] = primitives * vpp;
        block[STATS_TESS_BLOCK_PRIMITIVES] = primitives;
        std::vector<u32> indices(primitives * vpp);
        std::vector<float> records;
        for (u32 i = 0; i < primitives * vpp; ++i) {
            indices[i] = i;
            records.insert(records.end(),
                           {-0.5f + 0.01f * static_cast<float>(i % 7),
                            0.3f * static_cast<float>(i % 3) - 0.3f, 0.5f,
                            1.0f});
        }
        auto blocks = buffer(block.size() * 4, block.data());
        auto ids = buffer(indices.size() * 4, indices.data());
        auto recs = buffer(records.size() * 4, records.data());
        auto stats = record();
        StatsKernelParams p{};
        p.vertices_per_primitive = vpp;
        p.flags = STATS_FLAG_ADD_DS_INVOCATIONS |
                  (rasterized ? STATS_FLAG_CLIP | STATS_FLAG_DEPTH_CLIP : 0u);
        p.record_vec4s = 1;
        p.capacity = primitives;
        EXPECT_TRUE(dispatch(kStatsTessellationEntryPoint, p, primitives,
                             STATS_THREADS,
                             {{STATS_RECORD_INDEX, stats.get()},
                              {STATS_COUNTS_INDEX, blocks.get()},
                              {STATS_IDS_INDEX, ids.get()},
                              {STATS_RECORDS_INDEX, recs.get()}}));
        EXPECT_TRUE(device().wait_idle());
        return read_record(*stats);
    };
    // "tri integer factor 2": 2 patches x 6 triangles, DS 36 C 12 CP 12.
    auto w = run(3, 12, true);
    EXPECT_EQ(value(w, STAT_DS_INVOCATIONS), 36u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 12u);
    EXPECT_EQ(value(w, STAT_C_PRIMITIVES), 12u);
    // "tri integer factor 2, point output": 2 x 7 unique points, DS 14 C 14.
    w = run(1, 14, true);
    EXPECT_EQ(value(w, STAT_DS_INVOCATIONS), 14u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 14u);
    // "isoline detail 2 density 3": 6 segments, DS 12 C 6 CP 6.
    w = run(2, 6, true);
    EXPECT_EQ(value(w, STAT_DS_INVOCATIONS), 12u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 6u);
    EXPECT_EQ(value(w, STAT_C_PRIMITIVES), 6u);
    // "tri integer factor 2 + pass-through GS": the DS's primitives go to
    // the geometry shader, not the clipper -- DS 36, no C from this pass.
    w = run(3, 12, false);
    EXPECT_EQ(value(w, STAT_DS_INVOCATIONS), 36u);
    EXPECT_EQ(value(w, STAT_C_INVOCATIONS), 0u);
}
