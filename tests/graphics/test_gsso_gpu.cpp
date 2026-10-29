// SPDX-License-Identifier: LGPL-2.1-only
//
// The GPU-timeline passes behind the geometry, stream-output and
// tessellation emulations, run on a real Metal device and held against their
// C++ models (which test_gsso_models.cpp holds against Windows) or against
// Windows directly (d3d11_gsso_probe):
//
//   vitreo_tess_patch / vitreo_tess_scan   the software tessellator;
//   vitreo_so_prepare / _compact / _drawauto  stream-output accounting;
//   vitreo_ia_resolve                      an indirect draw's primitive
//                                          assembly;
//   the geometry kernel's stream regions and maxvertexcount limit.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/tessellator.hpp"
#include "../oracle_data/d3d11_gsso_shaders.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

class GssoGpuTest : public ::testing::Test {
public:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
    }

    static GraphicsBackendDevice& device() { return MetalDevice::instance(); }

    [[nodiscard]] static std::unique_ptr<BackendBuffer> buffer(
        const void* data, usize bytes) {
        BackendBufferDesc desc{};
        desc.byte_width = bytes;
        desc.shader_resource = true;
        desc.unordered_access = true;
        return device().create_buffer(desc, data);
    }
    template <typename T>
    [[nodiscard]] static std::unique_ptr<BackendBuffer> buffer(
        const std::vector<T>& words) {
        return buffer(words.data(), words.size() * sizeof(T));
    }

    [[nodiscard]] static std::vector<u32> read(BackendBuffer* b, usize words) {
        std::vector<u32> out(words, 0xCDCDCDCDu);
        EXPECT_TRUE(device().wait_idle());
        if (words != 0) EXPECT_TRUE(b->read(0, out.data(), words * 4));
        return out;
    }

    [[nodiscard]] static std::unique_ptr<BackendComputePipeline> pipeline(
        const std::string& msl, const char* entry, u64 key) {
        BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = msl;
        desc.compute_entry_point = entry;
        desc.cache_key = key;
        auto p = device().create_compute_pipeline(desc);
        EXPECT_TRUE(p && p->valid()) << entry << "\n" << msl;
        return p;
    }
};

// ---------------------------------------------------------------------------
// The software tessellator: count -> scan -> write for a batch of patches of
// different factors must place exactly the points and indices the model
// makes, patch after patch.
// ---------------------------------------------------------------------------

struct TessBatch {
    u32 domain, partitioning, output_primitive;
    std::vector<std::array<float, 8>> factors;  ///< per patch
    u32 patches_per_instance;
};

void tessellation_matches_model(const TessBatch& batch) {
    auto& dev = MetalDevice::instance();
    const std::string msl = tess::tessellator_msl();
    BackendComputePipelineDesc desc{};
    desc.compute_shader_msl = msl;
    desc.cache_key = std::hash<std::string>{}(msl);
    desc.compute_entry_point = tess::kTessPatchEntryPoint;
    auto patch_kernel = dev.create_compute_pipeline(desc);
    desc.compute_entry_point = tess::kTessScanEntryPoint;
    desc.cache_key ^= 0x5CA11u;
    auto scan_kernel = dev.create_compute_pipeline(desc);
    ASSERT_TRUE(patch_kernel && patch_kernel->valid()) << msl;
    ASSERT_TRUE(scan_kernel && scan_kernel->valid()) << msl;

    const u32 patches = static_cast<u32>(batch.factors.size());
    const u32 vpp = batch.output_primitive == 1   ? 1u
                    : batch.output_primitive == 2 ? 2u
                                                  : 3u;
    // The model: every patch at the running offsets.
    std::vector<u32> points_model;
    std::vector<u32> indices_model;
    std::vector<u32> ids_model;
    for (u32 i = 0; i < patches; ++i) {
        tess::TessState count{};
        count.sink.write = 0;
        float f[8];
        std::memcpy(f, batch.factors[i].data(), sizeof(f));
        tess::tess_patch(count, batch.domain, batch.partitioning,
                         batch.output_primitive, f);
        const u32 point_base = static_cast<u32>(points_model.size() / 4);
        const u32 index_base = static_cast<u32>(indices_model.size());
        points_model.resize(points_model.size() + 4u * count.numPoints);
        indices_model.resize(indices_model.size() + count.numIndices);
        ids_model.resize(indices_model.size() / vpp);
        tess::TessState s{};
        s.sink.points = points_model.data();
        s.sink.indices = indices_model.data();
        s.sink.primitive_ids = ids_model.data();
        s.sink.point_base = point_base;
        s.sink.index_base = index_base;
        s.sink.patch_index = i;
        s.sink.primitive_id = i % batch.patches_per_instance;
        s.sink.write = 1;
        s.sink.write_primitive_ids = 1;
        tess::tess_patch(s, batch.domain, batch.partitioning,
                         batch.output_primitive, f);
    }
    const u32 total_points = static_cast<u32>(points_model.size() / 4);
    const u32 total_indices = static_cast<u32>(indices_model.size());
    ASSERT_GT(total_indices, 0u);

    std::vector<float> factors;
    for (const auto& f : batch.factors) factors.insert(factors.end(), f.begin(), f.end());
    auto factor_buffer = GssoGpuTest::buffer(factors);
    auto counts = GssoGpuTest::buffer(std::vector<u32>(2 * patches, 0xEEu));
    auto points = GssoGpuTest::buffer(std::vector<u32>(4 * total_points + 4, 0xEEu));
    auto indices = GssoGpuTest::buffer(std::vector<u32>(total_indices + 4, 0xEEu));
    auto ids = GssoGpuTest::buffer(std::vector<u32>(total_indices / vpp + 4, 0xEEu));
    auto block = GssoGpuTest::buffer(std::vector<u32>(tess::TESS_BLOCK_WORDS, 0u));
    auto args = GssoGpuTest::buffer(std::vector<u32>(tess::TESS_ARGS_WORDS, 0xEEu));

    tess::TessPatchParams params{};
    params.domain = batch.domain;
    params.partitioning = batch.partitioning;
    params.output_primitive = batch.output_primitive;
    params.patches = patches;
    params.patches_per_instance = batch.patches_per_instance;
    params.write_primitive_ids = 1;
    params.factor_words = 8;
    params.point_capacity = total_points;
    params.index_capacity = total_indices;
    const auto run_patches = [&](u32 mode) {
        params.mode = mode;
        BackendDispatchCall call{};
        call.pipeline = patch_kernel.get();
        call.threads_per_group_x = 64;
        call.group_count_x = (patches + 63) / 64;
        call.group_count_y = call.group_count_z = 1;
        call.constants = &params;
        call.constants_size = sizeof(params);
        call.buffers[tess::TESS_FACTORS_INDEX] = factor_buffer.get();
        call.buffers[tess::TESS_COUNTS_INDEX] = counts.get();
        call.buffers[tess::TESS_POINTS_INDEX] = points.get();
        call.buffers[tess::TESS_INDICES_INDEX] = indices.get();
        call.buffers[tess::TESS_PRIMITIVE_IDS_INDEX] = ids.get();
        call.buffers[tess::TESS_PATCH_COUNT_INDEX] = counts.get();
        ASSERT_TRUE(dev.dispatch(call));
    };
    run_patches(0);
    tess::TessScanParams scan{};
    scan.patches = patches;
    scan.vertices_per_primitive = vpp;
    scan.point_capacity = total_points;
    scan.index_capacity = total_indices;
    BackendDispatchCall call{};
    call.pipeline = scan_kernel.get();
    call.threads_per_group_x = tess::TESS_SCAN_THREADS;
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.constants = &scan;
    call.constants_size = sizeof(scan);
    call.buffers[tess::TESS_SCAN_COUNTS_INDEX] = counts.get();
    call.buffers[tess::TESS_SCAN_BLOCK_INDEX] = block.get();
    call.buffers[tess::TESS_SCAN_ARGS_INDEX] = args.get();
    ASSERT_TRUE(dev.dispatch(call));
    run_patches(1);

    const auto block_words = GssoGpuTest::read(block.get(), tess::TESS_BLOCK_WORDS);
    EXPECT_EQ(block_words[tess::TESS_BLOCK_POINTS], total_points);
    EXPECT_EQ(block_words[tess::TESS_BLOCK_INDICES], total_indices);
    EXPECT_EQ(block_words[tess::TESS_BLOCK_PRIMITIVES], total_indices / vpp);
    EXPECT_EQ(block_words[tess::TESS_BLOCK_OVERFLOW], 0u);
    const auto arg_words = GssoGpuTest::read(args.get(), tess::TESS_ARGS_WORDS);
    EXPECT_EQ(arg_words[tess::TESS_ARGS_DOMAIN_DISPATCH], (total_points + 63) / 64);
    EXPECT_EQ(arg_words[tess::TESS_ARGS_DRAW], total_indices);
    EXPECT_EQ(arg_words[tess::TESS_ARGS_DRAW + 1], 1u);
    const auto gpu_points = GssoGpuTest::read(points.get(), 4 * total_points);
    const auto gpu_indices = GssoGpuTest::read(indices.get(), total_indices);
    const auto gpu_ids = GssoGpuTest::read(ids.get(), total_indices / vpp);
    EXPECT_EQ(gpu_points, points_model);
    EXPECT_EQ(gpu_indices, indices_model);
    EXPECT_EQ(gpu_ids, ids_model);
}

TEST_F(GssoGpuTest, TessellatorKernelMatchesTheModel) {
    // Fractional factors exercise the fixed-point rounding and the
    // transition stitching, where a float that rounds differently on the
    // GPU would move a point.
    tessellation_matches_model(
        {2, 3, 3,
         {{2.5f, 3.3f, 1.2f, 2.7f}, {1, 1, 1, 1}, {0, 1, 1, 1},
          {7.9f, 13.1f, 4.4f, 9.6f}, {64, 64, 64, 64}},
         2});
    tessellation_matches_model(
        {3, 4, 4,
         {{2.5f, 1.5f, 3.1f, 1, 2.2f, 1.7f}, {5, 6, 7, 8, 9, 10},
          {1.2f, 33.3f, 2, 17, 5.5f, 1}},
         3});
    tessellation_matches_model(
        {2, 2, 3, {{3, 5, 2, 6}, {17, 1, 40, 9}}, 1});
    // Isoline: lines and points.
    tessellation_matches_model({1, 1, 2, {{3, 4}, {1, 64}, {12.5f, 7}}, 1});
    tessellation_matches_model({1, 4, 1, {{2, 3}, {5.5f, 2.2f}}, 1});
}

// ---------------------------------------------------------------------------
// Stream-output accounting.
// ---------------------------------------------------------------------------

TEST_F(GssoGpuTest, StreamOutPrepareMatchesTheModel) {
    auto kernel = pipeline(stream_out_repack_msl(),
                           kStreamOutPrepareEntryPoint, 0x50993Au);
    ASSERT_TRUE(kernel);
    struct Case {
        const char* label;
        SoPrepareParams params;
        u32 count_word;
        std::array<u32, 4> filled;
    };
    SoPrepareParams split{};
    // d3d11_gsso_probe "stream 2 split over slots 2 and 3": the smallest
    // slot decides how many primitives fit (6 needed, room for 4 in slot 2).
    split.primitives = 6;
    split.vertices_per_primitive = 1;
    split.stream = 2;
    split.slot_mask = 0b1100;
    split.strides = {0, 0, 8, 8};
    split.sizes = {0, 0, 32, 64};
    SoPrepareParams compacted = split;
    compacted.count_mode = 2;  // a compaction's vertex total
    compacted.vertices_per_primitive = 3;
    compacted.stream = 1;
    compacted.slot_mask = 0b0011;
    compacted.strides = {16, 12, 0, 0};
    compacted.sizes = {16 * 30, 12 * 100, 0, 0};
    SoPrepareParams nothing = split;
    nothing.write_nothing = 1;  // a declared slot unbound: needed, not written
    SoPrepareParams full = split;
    full.stream = 0;
    const std::vector<Case> cases{
        {"split", split, 0, {0, 0, 0, 0}},
        {"compacted, slot 0 decides", compacted, 3 * 13, {100, 0, 0, 0}},
        {"write nothing", nothing, 0, {0, 0, 0, 0}},
        {"already full", full, 0, {0, 0, 40, 0}},
    };
    for (const auto& c : cases) {
        auto count = buffer(std::vector<u32>{c.count_word, 0, 0, 0});
        std::array<std::unique_ptr<BackendBuffer>, 4> filled;
        for (u32 s = 0; s < 4; ++s) {
            filled[s] = buffer(std::vector<u32>{c.filled[s], 0, 0, 0});
        }
        auto op = buffer(std::vector<u32>(SO_OP_WORDS, 0xEEu));
        std::vector<u32> stats_init(SO_STATS_WORDS, 0);
        stats_init[2 * c.params.stream] = 5;  // accumulates over draws
        stats_init[2 * c.params.stream + 1] = 4;
        auto stats = buffer(stats_init);
        BackendDispatchCall call{};
        call.pipeline = kernel.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.constants = &c.params;
        call.constants_size = sizeof(c.params);
        call.buffers[SO_PREPARE_COUNT_INDEX] = count.get();
        for (u32 s = 0; s < 4; ++s) {
            call.buffers[SO_PREPARE_FILLED_BASE + s] = filled[s].get();
        }
        call.buffers[SO_PREPARE_OP_INDEX] = op.get();
        call.buffers[SO_PREPARE_STATS_INDEX] = stats.get();
        ASSERT_TRUE(device().dispatch(call));

        std::array<u32, 4> model_filled = c.filled;
        std::array<u32*, 4> filled_ptrs{};
        for (u32 s = 0; s < 4; ++s) filled_ptrs[s] = &model_filled[s];
        std::array<u32, SO_OP_WORDS> model_op{};
        std::array<u32, SO_STATS_WORDS> model_stats{};
        std::copy(stats_init.begin(), stats_init.end(), model_stats.begin());
        stream_out_prepare_model(c.params, c.count_word, filled_ptrs, model_op,
                                 model_stats);
        for (u32 s = 0; s < 4; ++s) {
            EXPECT_EQ(read(filled[s].get(), 1)[0], model_filled[s])
                << c.label << " slot " << s;
        }
        const auto gpu_op = read(op.get(), SO_OP_WORDS);
        EXPECT_TRUE(std::equal(gpu_op.begin(), gpu_op.end(), model_op.begin()))
            << c.label;
        const auto gpu_stats = read(stats.get(), SO_STATS_WORDS);
        EXPECT_TRUE(std::equal(gpu_stats.begin(), gpu_stats.end(),
                               model_stats.begin()))
            << c.label;
    }
    // And the model's numbers are Windows': "slot 2 room for 4" wrote 4 of
    // the 6 stream-2 points, SO_STATISTICS_STREAM2 {written=4 needed=6}.
    std::array<u32, 4> f{};
    std::array<u32*, 4> fp{&f[0], &f[1], &f[2], &f[3]};
    std::array<u32, SO_OP_WORDS> op{};
    std::array<u32, SO_STATS_WORDS> stats{};
    stream_out_prepare_model(split, 0, fp, op, stats);
    EXPECT_EQ(stats[4], 6u);
    EXPECT_EQ(stats[5], 4u);
    EXPECT_EQ(op[SO_OP_VERTEX_COUNT], 4u);
    EXPECT_EQ(f[2], 32u);
    EXPECT_EQ(f[3], 32u);
}

TEST_F(GssoGpuTest, StreamOutCompactMatchesTheModel) {
    auto kernel = pipeline(stream_out_repack_msl(),
                           kStreamOutCompactEntryPoint, 0x50C0A9u);
    ASSERT_TRUE(kernel);
    // More invocations than scan threads, counts past the capacity clamped.
    const u32 invocations = 777;
    std::vector<u32> counts(invocations);
    for (u32 i = 0; i < invocations; ++i) counts[i] = (i * 7 + 3) % 6;
    SoCompactParams params{};
    params.invocations = invocations;
    params.capacity = 4;
    params.region_base = 2 * invocations * 4;  // stream 2's region
    const auto model = stream_out_compact_model(params, invocations, counts);
    auto count_buffer = buffer(counts);
    auto order = buffer(std::vector<u32>(model.size() + 4, 0xEEu));
    auto total = buffer(std::vector<u32>{0xEEu, 0, 0, 0});
    BackendDispatchCall call{};
    call.pipeline = kernel.get();
    call.threads_per_group_x = SO_COMPACT_THREADS;
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.constants = &params;
    call.constants_size = sizeof(params);
    call.buffers[SO_COMPACT_COUNT_INDEX] = total.get();
    call.buffers[SO_COMPACT_COUNTS_INDEX] = count_buffer.get();
    call.buffers[SO_COMPACT_ORDER_INDEX] = order.get();
    call.buffers[SO_COMPACT_TOTAL_INDEX] = total.get();
    ASSERT_TRUE(device().dispatch(call));
    EXPECT_EQ(read(total.get(), 1)[0], model.size());
    EXPECT_EQ(read(order.get(), model.size()), model);
}

TEST_F(GssoGpuTest, StreamOutDrawAutoArguments) {
    auto kernel = pipeline(stream_out_repack_msl(),
                           kStreamOutDrawAutoEntryPoint, 0x50DA70u);
    ASSERT_TRUE(kernel);
    struct Case {
        u32 filled, offset, stride;
    };
    // (filled - offset) / stride, 0 for a buffer never streamed to
    // (d3d11_stream_out_probe [drawauto]).
    for (const Case c : {Case{100, 4, 16}, Case{48, 0, 16}, Case{0, 0, 16},
                         Case{16, 32, 16}, Case{64, 0, 0}}) {
        auto filled = buffer(std::vector<u32>{c.filled, 0, 0, 0});
        auto args = buffer(std::vector<u32>(4, 0xEEu));
        const std::array<u32, 2> p{c.offset, c.stride};
        BackendDispatchCall call{};
        call.pipeline = kernel.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.constants = p.data();
        call.constants_size = sizeof(p);
        call.buffers[SO_DRAWAUTO_FILLED_INDEX] = filled.get();
        call.buffers[SO_DRAWAUTO_ARGS_INDEX] = args.get();
        ASSERT_TRUE(device().dispatch(call));
        EXPECT_EQ(read(args.get(), 4),
                  (std::vector<u32>{stream_out_draw_auto_count(
                                        c.filled, c.offset, c.stride),
                                    1, 0, 0}))
            << c.filled << " " << c.offset << " " << c.stride;
    }
}

// ---------------------------------------------------------------------------
// An indirect draw's primitive assembly.
// ---------------------------------------------------------------------------

TEST_F(GssoGpuTest, IaResolveMatchesTheModel) {
    auto kernel = pipeline(ia_resolve_msl(), kIaResolveEntryPoint, 0x1A7E5u);
    ASSERT_TRUE(kernel);
    struct Case {
        const char* label;
        IaResolveParams params;
        std::array<u32, 5> args;
        std::vector<u32> indices;  ///< from index_first, as u32 values
    };
    IaResolveParams strip16{};
    strip16.indexed = 1;
    strip16.index_elements = 9;
    strip16.vertices_per_primitive = 3;
    strip16.assembly = 1;
    strip16.record_capacity = 4096;
    strip16.primitive_capacity = 4096;
    IaResolveParams list32 = strip16;
    list32.index_32bit = 1;
    list32.assembly = 0;
    list32.index_elements = 7;
    IaResolveParams adjacency{};
    adjacency.vertices_per_primitive = 6;
    adjacency.assembly = 2;
    adjacency.record_capacity = 4096;
    adjacency.primitive_capacity = 4096;
    IaResolveParams lines{};
    lines.vertices_per_primitive = 2;
    lines.assembly = 1;
    lines.record_capacity = 4096;
    lines.primitive_capacity = 4096;
    IaResolveParams tight = lines;
    tight.record_capacity = 5;
    const std::vector<Case> cases{
        {"R16 strip with a cut, 2 instances", strip16, {9, 2, 0, 10, 3},
         {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7}},
        {"R32 list, cut value mid-primitive, start 1", list32, {6, 3, 1, 0, 0},
         {9, 4, 3, 2, 0xFFFFFFFFu, 1, 0}},
        {"strip with adjacency, 3 instances", adjacency, {10, 3, 0, 0, 0}, {}},
        {"line strip, 300 instances", lines, {3, 300, 4, 0, 0}, {}},
        {"over capacity: dropped whole", tight, {3, 2, 4, 0, 0}, {}},
    };
    for (const auto& c : cases) {
        std::vector<u32> model_ids;
        const auto model = ia_resolve_model(c.params, c.args, c.indices, model_ids);
        // The index buffer as the GPU holds it: 16-bit values packed.
        std::vector<u32> ib_words;
        if (c.params.index_32bit != 0) {
            ib_words = c.indices;
        } else {
            ib_words.assign((c.indices.size() + 1) / 2, 0);
            for (usize i = 0; i < c.indices.size(); ++i) {
                ib_words[i / 2] |= (c.indices[i] & 0xFFFFu) << ((i & 1) * 16);
            }
        }
        ib_words.resize(ib_words.size() + 4, 0);
        auto ib = buffer(ib_words);
        auto args = buffer(std::vector<u32>(c.args.begin(), c.args.end()));
        auto ids = buffer(std::vector<u32>(model_ids.size() + 8, 0xEEu));
        auto out = buffer(std::vector<u32>(IA_RESOLVED_WORDS, 0xEEu));
        auto capture = buffer(std::vector<u32>(4, 0xEEu));
        BackendDispatchCall call{};
        call.pipeline = kernel.get();
        call.threads_per_group_x = IA_RESOLVE_THREADS;
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.constants = &c.params;
        call.constants_size = sizeof(c.params);
        call.buffers[IA_RESOLVE_ARGS_INDEX] = args.get();
        call.buffers[IA_RESOLVE_INDICES_INDEX] = ib.get();
        call.buffers[IA_RESOLVE_IDS_INDEX] = ids.get();
        call.buffers[IA_RESOLVE_OUT_INDEX] = out.get();
        call.buffers[IA_RESOLVE_CAPTURE_INDEX] = capture.get();
        ASSERT_TRUE(device().dispatch(call));
        const auto gpu_out = read(out.get(), IA_RESOLVED_WORDS);
        for (u32 w = 0; w < IA_RESOLVED_WORDS; ++w) {
            if (w >= 6 && w < IA_RESOLVED_CAPTURE_ARGS) continue;  // unused
            if (w >= IA_RESOLVED_CAPTURE_ARGS + 5) continue;
            EXPECT_EQ(gpu_out[w], model[w]) << c.label << " word " << w;
        }
        EXPECT_EQ(read(ids.get(), model_ids.size()), model_ids) << c.label;
        const auto header = read(capture.get(), 2);
        EXPECT_EQ(header[0], model[IA_RESOLVED_KEY_STRIDE]) << c.label;
        EXPECT_EQ(header[1], c.params.record_capacity) << c.label;
    }
}

// ---------------------------------------------------------------------------
// The geometry kernel: per-stream regions and counts, and maxvertexcount
// counting emits across every stream (d3d11_gsso_probe [streams] and
// [maxvertexcount]).
// ---------------------------------------------------------------------------

struct GsRun {
    u32 output_registers{0};
    u32 capacity{0};
    std::vector<u32> counts;  ///< [stream * invocations + invocation]
    std::vector<u32> records;  ///< gs_out, u32 words
};

GsRun run_geometry_kernel(const unsigned char* bytes, usize size,
                          u32 invocations) {
    GsRun run;
    const auto container = parse_dxbc(bytes, size);
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    // vs_ids: o0 SV_Position, o1 TEXCOORD0 = (vid, iid, 0, 0x1000).
    options.gs_input_record_registers = {0, 1};
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return run;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                          ShaderStage::Compute, flat);
    EXPECT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = std::hash<std::string>{}(pd.compute_shader_msl);
    auto kernel = MetalDevice::instance().create_compute_pipeline(pd);
    EXPECT_TRUE(kernel && kernel->valid()) << msl.msl_text();
    if (!kernel || !kernel->valid()) return run;

    std::vector<u32> records;
    for (u32 v = 0; v < invocations; ++v) {
        const float pos[4] = {0.0f, 0.0f, 0.5f, 1.0f};
        u32 pos_bits[4];
        std::memcpy(pos_bits, pos, sizeof(pos));
        records.insert(records.end(), pos_bits, pos_bits + 4);
        records.insert(records.end(), {v, 0u, 0u, 0x1000u});
    }
    run.output_registers = spirv.gs_output_registers;
    // An invocation's region holds its output as a LIST: a strip of
    // maxvertexcount vertices is (maxvertexcount - 2) triangles.
    run.capacity = gs_vertices_per_invocation(spirv.gs_output_topology,
                                              spirv.gs_max_output_vertices);
    const u32 streams = 4;
    const usize out_words = static_cast<usize>(streams) * invocations *
                            run.capacity * spirv.gs_output_registers * 4;
    auto gs_in = GssoGpuTest::buffer(records);
    auto gs_out = GssoGpuTest::buffer(std::vector<u32>(out_words + 4, 0xEEu));
    // Control: non-indexed, primitive stride 1 (points), stream regions
    // `invocations` invocation slots apart; junk where the counts go, which
    // the kernel must reset.
    std::vector<u32> control(kGsInvocationCountBase + streams * invocations,
                             0xDEADu);
    std::fill(control.begin(), control.begin() + kGsInvocationCountBase, 0u);
    control[4] = 1;
    control[kGsControlStreamStride] = invocations;
    auto counter = GssoGpuTest::buffer(control);
    BackendDispatchCall call{};
    call.pipeline = kernel.get();
    call.group_count_x = invocations;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 0] = gs_in.get();
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 1] = gs_out.get();
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 2] = counter.get();
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 3] = gs_in.get();
    EXPECT_TRUE(MetalDevice::instance().dispatch(call));
    const auto all = GssoGpuTest::read(counter.get(), control.size());
    run.counts.assign(all.begin() + kGsInvocationCountBase, all.end());
    run.records = GssoGpuTest::read(gs_out.get(), out_words);
    return run;
}

/// Whether record (stream, invocation, vertex) holds @p value in any of its
/// registers (a stream's registers are its own; the record is their union).
bool record_holds(const GsRun& run, u32 invocations, u32 stream, u32 invocation,
                  u32 vertex, const std::array<u32, 4>& value) {
    const usize slot =
        (static_cast<usize>(stream) * invocations + invocation) * run.capacity +
        vertex;
    for (u32 r = 0; r < run.output_registers; ++r) {
        const usize at = (slot * run.output_registers + r) * 4;
        if (std::equal(value.begin(), value.end(), run.records.begin() + at)) {
            return true;
        }
    }
    return false;
}

TEST_F(GssoGpuTest, GeometryKernelWritesEachStreamToItsOwnRegion) {
    // gs_streams4: per input point 1/2/3/4 points to streams 0/1/2/3,
    // emitted in the order s3, s0, s2, s1 -- each stream keeps its own
    // order and count.
    const u32 invocations = 2;
    const auto run = run_geometry_kernel(gsso_gs_streams4,
                                         sizeof(gsso_gs_streams4), invocations);
    ASSERT_EQ(run.counts.size(), 4u * invocations);
    for (u32 s = 0; s < 4; ++s) {
        for (u32 i = 0; i < invocations; ++i) {
            EXPECT_EQ(run.counts[s * invocations + i], s + 1)
                << "stream " << s << " invocation " << i;
            for (u32 k = 0; k <= s; ++k) {
                // (id, stream, k, 0x8000 + stream); stream 1's first
                // component is SV_PrimitiveID, the same number here.
                EXPECT_TRUE(record_holds(run, invocations, s, i, k,
                                         {i, s, k, 0x8000u + s}))
                    << "stream " << s << " invocation " << i << " vertex " << k;
            }
        }
    }
}

TEST_F(GssoGpuTest, MaxVertexCountCountsEmitsAcrossStreams) {
    // gs_ptmax: maxvertexcount(3), five emits alternating s0/s1 -- the
    // first three land (s0 k=0, s1 k=1, s0 k=2), the rest are discarded
    // (Windows: stream 0 {written=4 needed=4} and stream 1 {2, 2} for two
    // points).
    const u32 invocations = 2;
    const auto ptmax = run_geometry_kernel(gsso_gs_ptmax, sizeof(gsso_gs_ptmax),
                                           invocations);
    ASSERT_EQ(ptmax.counts.size(), 4u * invocations);
    for (u32 i = 0; i < invocations; ++i) {
        EXPECT_EQ(ptmax.counts[0 * invocations + i], 2u) << i;
        EXPECT_EQ(ptmax.counts[1 * invocations + i], 1u) << i;
        EXPECT_TRUE(record_holds(ptmax, invocations, 0, i, 0, {i, 0, 0, 0x8100}));
        EXPECT_TRUE(record_holds(ptmax, invocations, 0, i, 1, {i, 0, 2, 0x8100}));
        EXPECT_TRUE(record_holds(ptmax, invocations, 1, i, 0, {i, 1, 1, 0x8101}));
    }
    // gs_cutmax: three 3-vertex strips (9 emits) under maxvertexcount(7):
    // two whole triangles, the third strip's lone vertex makes none
    // (Windows: 12 records for two points).
    const auto cutmax = run_geometry_kernel(gsso_gs_cutmax,
                                            sizeof(gsso_gs_cutmax), invocations);
    ASSERT_GE(cutmax.counts.size(), invocations);
    for (u32 i = 0; i < invocations; ++i) {
        EXPECT_EQ(cutmax.counts[i], 6u) << i;
        for (u32 a = 0; a < 2; ++a) {
            for (u32 k = 0; k < 3; ++k) {
                EXPECT_TRUE(record_holds(cutmax, invocations, 0, i, a * 3 + k,
                                         {i, a, k, 0x7700}))
                    << i << " strip " << a << " vertex " << k;
            }
        }
    }
}

}  // namespace
