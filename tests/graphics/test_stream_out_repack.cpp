// SPDX-License-Identifier: LGPL-2.1-only
//
// The stream-out repack kernel, run on real Metal.
//
// Every expected table below is what Windows wrote for the same draw
// (tools/oracle_probe/d3d11_stream_out_probe.expected), with the captured
// record standing in for the vertex shader: record[vid] = (vid, vid*10+1, 2, 3)
// in vec4 slot 0 and (vid+100, 5, 0, 0) in slot 1, which is what the probe's
// vertex shader outputs as TEXCOORD0 and TEXCOORD1.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/stream_out_repack.hpp"

#include <array>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr float kSentinel = -7.0f;
constexpr u32 kCapturedVertices = 16;

[[nodiscard]] std::vector<float> captured_records() {
    std::vector<float> records(kCapturedVertices * 2 * 4);
    for (u32 vid = 0; vid < kCapturedVertices; ++vid) {
        float* r = records.data() + vid * 8;
        r[0] = static_cast<float>(vid);
        r[1] = static_cast<float>(vid * 10 + 1);
        r[2] = 2.0f;
        r[3] = 3.0f;
        r[4] = static_cast<float>(vid + 100);
        r[5] = 5.0f;
    }
    return records;
}

[[nodiscard]] std::unique_ptr<BackendBuffer> make_buffer(
    GraphicsBackendDevice& device, const void* data, usize bytes) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.vertex_buffer = true;
    return device.create_buffer(desc, data);
}

[[nodiscard]] std::unique_ptr<BackendBuffer> sentinel_target(
    GraphicsBackendDevice& device, u32 floats) {
    const std::vector<float> init(floats, kSentinel);
    return make_buffer(device, init.data(), init.size() * sizeof(float));
}

/// Floats of @p buffer through a compute copy into an R32Float texture.
[[nodiscard]] std::vector<float> read_floats(GraphicsBackendDevice& device,
                                             BackendBuffer* buffer,
                                             u32 count) {
    const std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(const device float* words [[buffer(" +
        std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n"
        "    texture2d<float, access::write> u0 [[texture(" +
        std::to_string(BACKEND_UAV_TEXTURE_BASE) + ")]],\n"
        "    uint3 id [[thread_position_in_grid]]) {\n"
        "    if (id.x < u0.get_width()) u0.write(float4(words[id.x]), "
        "uint2(id.x, 0));\n}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = source;
    pd.cache_key = 0x50EAD01u;
    auto pipeline = device.create_compute_pipeline(pd);
    BackendTexture2DDesc td{};
    td.width = count;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::R32Float;
    td.unordered_access = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    std::vector<float> out(count, -1.0f);
    if (!pipeline || !pipeline->valid() || !texture) return {};
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = count;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = buffer;
    call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
    if (!device.dispatch(call)) return {};
    if (!texture->readback(0, out.data(), count * sizeof(float))) return {};
    return out;
}

class StreamOutRepackTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        BackendComputePipelineDesc pd{};
        pd.compute_entry_point = kStreamOutRepackEntryPoint;
        pd.compute_shader_msl = stream_out_repack_msl();
        pd.cache_key = 0x50AE9ACCu;
        pipeline = device().create_compute_pipeline(pd);
        ASSERT_TRUE(pipeline != nullptr && pipeline->valid())
            << stream_out_repack_msl();
        const auto records = captured_records();
        capture = make_buffer(device(), records.data(),
                              records.size() * sizeof(float));
        ASSERT_NE(capture, nullptr);
    }

    static GraphicsBackendDevice& device() { return MetalDevice::instance(); }

    /// Runs the repack with @p targets bound at the target slots.
    void repack(const StreamOutRepackDesc& desc,
                std::array<BackendBuffer*, 4> targets,
                BackendBuffer* indices = nullptr) {
        const auto words = build_stream_out_control(desc);
        control = make_buffer(device(), words.data(),
                              words.size() * sizeof(u32));
        ASSERT_NE(control, nullptr);
        // What vitreo_so_prepare writes when every primitive fits.
        const auto op_words = build_stream_out_op(desc);
        op = make_buffer(device(), op_words.data(), sizeof(op_words));
        ASSERT_NE(op, nullptr);
        BackendDispatchCall call{};
        call.pipeline = pipeline.get();
        const u32 threads = desc.primitive_count * desc.vertices_per_primitive;
        call.threads_per_group_x = SO_REPACK_THREADS_PER_GROUP;
        call.group_count_x = (threads + SO_REPACK_THREADS_PER_GROUP - 1) /
                             SO_REPACK_THREADS_PER_GROUP;
        call.group_count_y = call.group_count_z = 1;
        call.buffers[SO_REPACK_CAPTURE_INDEX] = capture.get();
        call.buffers[SO_REPACK_CONTROL_INDEX] = control.get();
        call.buffers[SO_REPACK_INDICES_INDEX] =
            indices != nullptr ? indices : control.get();
        call.buffers[SO_REPACK_OP_INDEX] = op.get();
        for (u32 slot = 0; slot < 4; ++slot) {
            call.buffers[SO_REPACK_TARGET_BASE + slot] =
                targets[slot] != nullptr ? targets[slot] : control.get();
        }
        ASSERT_TRUE(device().dispatch(call));
    }

    [[nodiscard]] static StreamOutRepackDesc texcoord0_desc() {
        StreamOutRepackDesc desc{};
        desc.record_vec4s = 2;
        desc.strides[0] = 16;
        desc.entries.push_back({0, 0, 0, 0, 4});
        return desc;
    }

    std::unique_ptr<BackendComputePipeline> pipeline;
    std::unique_ptr<BackendBuffer> capture;
    std::unique_ptr<BackendBuffer> control;
    std::unique_ptr<BackendBuffer> op;
};

/// First component of each written record: the source vertex id.
[[nodiscard]] std::vector<float> vids(const std::vector<float>& floats) {
    std::vector<float> out;
    for (usize i = 0; i < floats.size(); i += 4) out.push_back(floats[i]);
    return out;
}

}  // namespace

TEST(StreamOutOrder, StripsAreWrittenAsListsInTheWindowsOrder) {
    // Windows: triangle strip of 5 -> (0,1,2) (1,3,2) (2,3,4).
    const std::vector<u32> tri{0, 1, 2, 1, 3, 2, 2, 3, 4};
    for (u32 i = 0; i < tri.size(); ++i) {
        EXPECT_EQ(stream_out_source_vertex(3, true, i), tri[i]) << i;
    }
    // Windows: line strip of 4 -> (0,1) (1,2) (2,3).
    const std::vector<u32> line{0, 1, 1, 2, 2, 3};
    for (u32 i = 0; i < line.size(); ++i) {
        EXPECT_EQ(stream_out_source_vertex(2, true, i), line[i]) << i;
    }
    EXPECT_EQ(stream_out_primitive_count(3, true, 5), 3u);
    EXPECT_EQ(stream_out_primitive_count(2, true, 4), 3u);
    // Windows: a triangle list of 5 vertices writes one whole triangle.
    EXPECT_EQ(stream_out_primitive_count(3, false, 5), 1u);
    EXPECT_EQ(stream_out_primitive_count(3, true, 2), 0u);
    EXPECT_EQ(stream_out_primitive_count(1, false, 7), 7u);
}

TEST_F(StreamOutRepackTest, TriangleStripMatchesWindows) {
    auto target = sentinel_target(device(), 4 * 10);
    auto desc = texcoord0_desc();
    desc.vertices_per_primitive = 3;
    desc.strip = true;
    desc.primitive_count = 3;
    repack(desc, {target.get(), nullptr, nullptr, nullptr});
    const auto floats = read_floats(device(), target.get(), 4 * 10);
    ASSERT_EQ(floats.size(), 40u);
    const std::vector<float> expected{0, 1, 2, 1, 3, 2, 2, 3, 4, kSentinel};
    EXPECT_EQ(vids(floats), expected);
    // The whole record, not just its first component.
    EXPECT_EQ(floats[4 * 4 + 1], 31.0f);
    EXPECT_EQ(floats[4 * 4 + 2], 2.0f);
    EXPECT_EQ(floats[4 * 4 + 3], 3.0f);
}

TEST_F(StreamOutRepackTest, GapEntryKeepsTheSkippedBytes) {
    // Windows: gap of 2 then TEXCOORD1.xy, stride 16, two points ->
    //   .  .  100 5 / .  .  101 5
    auto target = sentinel_target(device(), 4 * 3);
    StreamOutRepackDesc desc{};
    desc.record_vec4s = 2;
    desc.strides[0] = 16;
    desc.entries.push_back({0, 0, STREAM_OUT_GAP, 0, 2});
    desc.entries.push_back({0, 2, 1, 0, 2});
    desc.vertices_per_primitive = 1;
    desc.primitive_count = 2;
    repack(desc, {target.get(), nullptr, nullptr, nullptr});
    const auto floats = read_floats(device(), target.get(), 12);
    const std::vector<float> expected{kSentinel, kSentinel, 100, 5,
                                      kSentinel, kSentinel, 101, 5,
                                      kSentinel, kSentinel, kSentinel,
                                      kSentinel};
    EXPECT_EQ(floats, expected);
}

TEST_F(StreamOutRepackTest, IndexedDrawWritesInIndexOrder) {
    // Windows: points over idx[1..5] = {0,2,9,1,3} -> records 0,2,9,1,3.
    const std::array<u16, 6> idx{4, 0, 2, 9, 1, 3};
    auto indices = make_buffer(device(), idx.data(), 12);
    auto target = sentinel_target(device(), 4 * 6);
    auto desc = texcoord0_desc();
    desc.index_mode = StreamOutIndexMode::Uint16;
    desc.first = 1;
    desc.vertices_per_primitive = 1;
    desc.primitive_count = 5;
    repack(desc, {target.get(), nullptr, nullptr, nullptr}, indices.get());
    const std::vector<float> expected{0, 2, 9, 1, 3, kSentinel};
    EXPECT_EQ(vids(read_floats(device(), target.get(), 24)), expected);

    // The capture is keyed by the vertex id Metal generates, which includes
    // BaseVertexLocation, so the lookup must add it too.
    auto based = sentinel_target(device(), 4 * 6);
    desc.base_vertex = 3;
    repack(desc, {based.get(), nullptr, nullptr, nullptr}, indices.get());
    const std::vector<float> shifted{3, 5, 12, 4, 6, kSentinel};
    EXPECT_EQ(vids(read_floats(device(), based.get(), 24)), shifted);
}

TEST_F(StreamOutRepackTest, ThirtyTwoBitIndicesAndTwoTargetsAtOffsets) {
    const std::array<u32, 3> idx{7, 1, 4};
    auto indices = make_buffer(device(), idx.data(), sizeof(idx));
    auto slot0 = sentinel_target(device(), 4 * 5);
    auto slot1 = sentinel_target(device(), 2 * 4);
    StreamOutRepackDesc desc{};
    desc.index_mode = StreamOutIndexMode::Uint32;
    desc.record_vec4s = 2;
    desc.vertices_per_primitive = 1;
    desc.primitive_count = 3;
    // Appending after 2 records in slot 0 and 1 record in slot 1.
    desc.dst_offsets = {32, 8, 0, 0};
    desc.strides = {16, 8, 0, 0};
    desc.entries.push_back({0, 0, 0, 0, 4});
    desc.entries.push_back({1, 0, 1, 0, 2});
    repack(desc, {slot0.get(), slot1.get(), nullptr, nullptr}, indices.get());
    const std::vector<float> expected0{kSentinel, kSentinel, 7, 1, 4};
    EXPECT_EQ(vids(read_floats(device(), slot0.get(), 20)), expected0);
    const std::vector<float> expected1{kSentinel, kSentinel, 107, 5,
                                       101, 5, 104, 5};
    EXPECT_EQ(read_floats(device(), slot1.get(), 8), expected1);
}

// ---------------------------------------------------------------------------
// The geometry expansion kernel on a REAL FXC geometry shader (gs_expand from
// the D3D11 oracle suite: one triangle in, two out, via loops, RestartStrip
// and a per-vertex recolour). Records are what vs_main would capture for the
// oracle triangle: o0 position, o1 COLOR, o2 TEXCOORD.
// ---------------------------------------------------------------------------

#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "../oracle_data/d3d11_oracle/d3d11_shaders.h"

TEST_F(StreamOutRepackTest, RealGeometryShaderExpandsTheOracleTriangle) {
    const auto container = parse_dxbc(gs_expand, sizeof(gs_expand));
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    options.gs_input_record_registers = {0, 1, 2};
    const auto spirv = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                          ShaderStage::Compute, flat);
    ASSERT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = 0x6E0A00FFu;
    auto kernel = device().create_compute_pipeline(pd);
    ASSERT_TRUE(kernel && kernel->valid()) << msl.msl_text();

    const std::array<float, 36> records{
         0.0f,  0.7f, 0, 1,   0, 1, 0, 1,   0.5f, 0, 0, 0,
         0.7f, -0.7f, 0, 1,   0, 1, 0, 1,   1, 1, 0, 0,
        -0.7f, -0.7f, 0, 1,   0, 1, 0, 1,   0, 1, 0, 0};
    auto gs_in = make_buffer(device(), records.data(), sizeof(records));
    const std::vector<float> zeros(6 * spirv.gs_output_registers * 4, -7.0f);
    auto gs_out = make_buffer(device(), zeros.data(), zeros.size() * 4);
    // Control block, then invocation 0's written-vertex count (which the
    // kernel must reset from this junk value).
    std::array<u32, kGsInvocationCountBase + 1> control{0, 0, 0, 0, 3};
    control[kGsInvocationCountBase] = 0xDEADu;
    auto counter = make_buffer(device(), control.data(), sizeof(control));
    BackendDispatchCall call{};
    call.pipeline = kernel.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 0] = gs_in.get();
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 1] = gs_out.get();
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 2] = counter.get();
    call.buffers[BACKEND_GS_EXPANSION_BUFFER_BASE + 3] = gs_in.get();
    ASSERT_TRUE(device().dispatch(call));

    const auto count =
        read_floats(device(), counter.get(), kGsInvocationCountBase + 1);
    ASSERT_EQ(count.size(), kGsInvocationCountBase + 1u);
    u32 appended = 0;
    std::memcpy(&appended, &count[kGsInvocationCountBase], 4);
    EXPECT_EQ(appended, 6u) << "two triangles appended";
    const auto out = read_floats(device(), gs_out.get(),
                                 static_cast<u32>(zeros.size()));
    const u32 stride = spirv.gs_output_registers * 4;
    // Every vertex of both copies. The shader indexes input[i] in a loop, so
    // FXC keeps the vertex index DYNAMIC (v[r0.x][...]); checking only the
    // first vertex of each copy missed a kernel that read vertex 0 three times.
    // Left copy: x * 0.5 - 0.5, green. Right copy: x * 0.5 + 0.5, recoloured
    // (g, 0, r, 1) = (1, 0, 0, 1).
    const std::array<std::array<float, 2>, 3> in_xy{
        {{0.0f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}}};
    for (u32 v = 0; v < 3; ++v) {
        const float* left = out.data() + v * stride;
        const float* right = out.data() + (v + 3) * stride;
        EXPECT_FLOAT_EQ(left[0], in_xy[v][0] * 0.5f - 0.5f) << "left v" << v;
        EXPECT_FLOAT_EQ(left[1], in_xy[v][1]) << "left v" << v;
        EXPECT_FLOAT_EQ(left[5], 1.0f) << "left v" << v << " green";
        EXPECT_FLOAT_EQ(right[0], in_xy[v][0] * 0.5f + 0.5f) << "right v" << v;
        EXPECT_FLOAT_EQ(right[1], in_xy[v][1]) << "right v" << v;
        EXPECT_FLOAT_EQ(right[4], 1.0f) << "right v" << v << " red";
        EXPECT_FLOAT_EQ(right[5], 0.0f) << "right v" << v;
    }
    if (::testing::Test::HasFailure()) {
        std::string dump;
        for (usize i = 0; i < out.size(); ++i) {
            dump += std::to_string(out[i]) + ((i + 1) % stride ? " " : "\n");
        }
        ADD_FAILURE() << "gs_out:\n" << dump << "\nMSL:\n" << msl.msl_text();
    }
}
