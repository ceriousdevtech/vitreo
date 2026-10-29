// SPDX-License-Identifier: LGPL-2.1-only
//
// Consecutive render passes on the same attachments share one Metal encoder.
//
// The D3D11 frontend records one pass per draw, and a Metal pass loads and
// stores every attachment, so on a tile-based GPU each draw paid a full read
// and write of every render target (Satisfactory: ~15,000 passes/s at
// 2560x1440, VITREO_GPU_PROFILE). MetalSubmitQueue now continues a pass in the
// open encoder when its attachments match and it clears nothing.
//
// What must not change is what the passes produce. Separate encoders order
// everything; one encoder orders attachment writes but not shader writes, and
// an attachment read through a texture sees memory, not the tile. So these
// tests check the three cases: plain draws merge and keep their order, a draw
// that samples its own depth attachment still sees the earlier pass's depth,
// and no pass joins an encoder after a draw that wrote a UAV. The counters
// make each test fail if merging never happens, not only if it goes wrong.
//
// Since WP-R2 (graphics/contracts/slim_record.hpp, "pass-fold") most merges
// are decided when the passes are recorded rather than when they are encoded:
// a pass the encoder would continue reopens the pass recorded before it. The
// passes sharing an encoder with the pass before them are then
// passes_continued + passes_folded - folds_split (merged() below), the same
// number either way, and the pixels must be the pass-merge-off reference.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <functional>

#include <array>
#include <chrono>
#include <thread>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kSide = 4;

const char* vertex_at(float z) {
    // One full-target triangle at a fixed depth.
    static std::string text[2];
    const int i = z < 0.5f ? 0 : 1;
    text[i] = std::string(R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float2 position [[attribute(0)]]; };
        struct main0_out { float4 position [[position]]; };
        vertex main0_out main0(main0_in in [[stage_in]]) {
            main0_out out = {};
            out.position = float4(in.position, )") +
              std::to_string(z) + R"(, 1.0);
            return out;
        }
    )";
    return text[i].c_str();
}

constexpr const char* kSolidColour = R"(
    #include <metal_stdlib>
    using namespace metal;
    fragment float4 main0(constant float4& colour [[buffer(0)]]) {
        return colour;
    }
)";

// Red = the depth attachment's value at this pixel, read through a texture.
constexpr const char* kDepthToRed = R"(
    #include <metal_stdlib>
    using namespace metal;
    fragment float4 main0(float4 position [[position]],
                          depth2d<float> depth [[texture(0)]]) {
        return float4(depth.read(uint2(position.xy)), 0.0, 0.0, 1.0);
    }
)";

class RenderPassMergeTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        static const float positions[]{-1, -1, 3, -1, -1, 3};
        BackendBufferDesc vb{};
        vb.byte_width = sizeof(positions);
        vb.vertex_buffer = true;
        vertices = device().create_buffer(vb, positions);
        BackendTexture2DDesc td{};
        td.width = td.height = kSide;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::BGRA8Unorm;
        td.render_target = true;
        // Sampled too: the split-rule tests read the attachment.
        td.shader_resource = true;
        target = device().create_texture_2d(td, nullptr, 0);
        ASSERT_TRUE(vertices && target && target->valid());
    }

    static GraphicsBackendDevice& device() { return MetalDevice::instance(); }

    static std::unique_ptr<BackendPipeline> pipeline(const char* vertex,
                                                     const std::string& fragment,
                                                     u32 key, bool depth,
                                                     unsigned samples = 1) {
        BackendPipelineDesc pdesc{};
        pdesc.vertex_shader_msl = vertex;
        pdesc.fragment_shader_msl = fragment;
        pdesc.vertex_entry_point = "main0";
        pdesc.fragment_entry_point = "main0";
        pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
        pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
        pdesc.color_format = PixelFormat::BGRA8Unorm;
        pdesc.depth_attachment = depth;
        if (depth) pdesc.depth_format = PixelFormat::D32Float;
        pdesc.sample_count = samples;
        pdesc.cache_key = key;
        auto made = device().create_pipeline(pdesc);
        EXPECT_TRUE(made && made->valid()) << fragment;
        return made;
    }

    void TearDown() override {
        slim_record::force_all_for_test(slim_record::Force::None);
        delta_record::force_all_for_test(delta_record::Force::None);
        MetalSubmitQueue::set_pass_merge_for_test(std::nullopt);
    }

    /// One pass holding one draw, the way the D3D11 frontend records them.
    void pass(BackendDrawCall call, BackendTexture2D* depth = nullptr,
              bool clear_depth = false,
              GpuOcclusionCounter* occlusion = nullptr) {
        pass_on(target.get(), std::move(call), depth, clear_depth, occlusion,
                nullptr);
    }

    /// pass() on @p colour, running @p between (when set) after the pass is
    /// begun and before its draw: an op recorded inside an open pass.
    void pass_on(BackendTexture2D* colour, BackendDrawCall call,
                 BackendTexture2D* depth, bool clear_depth,
                 GpuOcclusionCounter* occlusion,
                 const std::function<void()>& between) {
        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets[0] = colour;
        pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
        pass_desc.preserve_color_contents = true;
        pass_desc.depth_stencil_target = depth;
        pass_desc.clear_depth = clear_depth;
        pass_desc.depth_clear_value = 1.0f;
        pass_desc.occlusion_counter = occlusion;
        auto open = device().create_render_pass_standalone(pass_desc);
        ASSERT_TRUE(open && open->valid());
        if (between) between();
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = 3;
        call.vertex_buffer = vertices.get();
        call.vertex_buffers[0] = vertices.get();
        call.viewport = {0, 0, static_cast<float>(kSide),
                         static_cast<float>(kSide), 0, 1};
        ASSERT_TRUE(open->draw(call));
        ASSERT_TRUE(open->end());
    }

    /// BGRA bytes of every pixel, row-major.
    std::vector<u32> pixels() {
        std::vector<u32> out(kSide * kSide, 0xDEADBEEFu);
        EXPECT_TRUE(target->readback(0, out.data(), kSide * 4));
        return out;
    }

    static u64 continued() {
        return MetalSubmitQueue::instance().passes_continued();
    }
    static u64 restarted() {
        return MetalSubmitQueue::instance().passes_restarted();
    }

    /// The queue's pass counters at one moment. Read them only with the
    /// queue drained (after pixels()): the encoder counts as it encodes.
    struct Counts {
        u64 continued{0}, restarted{0}, folded{0}, split{0};
        u64 begins{0}, ends{0}, copied{0}, shared{0}, clears{0};
    };
    static Counts counts() {
        auto& q = MetalSubmitQueue::instance();
        return Counts{q.passes_continued(), q.passes_restarted(),
                      q.passes_folded(),    q.folds_split(),
                      q.begin_passes_encoded(), q.end_passes_encoded(),
                      q.push_blocks_copied(), q.push_blocks_shared(),
                      q.clears_folded()};
    }
    /// Passes that shared an encoder with the pass before them, between two
    /// snapshots, wherever that was decided.
    static u64 merged(const Counts& a, const Counts& b) {
        return (b.continued - a.continued) + (b.folded - a.folded) -
               (b.split - a.split);
    }

    std::unique_ptr<BackendBuffer> vertices;
    std::unique_ptr<BackendTexture2D> target;
};

constexpr u32 kRed = 0xFFFF0000u;    // BGRA8 little-endian: A R G B
constexpr u32 kGreen = 0xFF00FF00u;
constexpr u32 kBlue = 0xFF0000FFu;

TEST_F(RenderPassMergeTest, ConsecutivePassesShareAnEncoderAndKeepTheirOrder) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5100u, false);
    ASSERT_TRUE(solid);
    (void)pixels();
    const Counts before = counts();

    // Red everywhere, then green over the left half, then blue over the top
    // row: each later pass must land on what the earlier ones left.
    const float red[4]{1, 0, 0, 1};
    const float green[4]{0, 1, 0, 1};
    const float blue[4]{0, 0, 1, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = red;
    call.fragment_constants_size = sizeof(red);
    pass(call);
    call.fragment_constants = green;
    call.scissor_enabled = true;
    call.scissor = {0, 0, 2, static_cast<int>(kSide)};
    pass(call);
    call.fragment_constants = blue;
    call.scissor = {0, 0, static_cast<int>(kSide), 1};
    pass(call);

    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            const u32 expected = y < 1 ? kBlue : x < 2 ? kGreen : kRed;
            EXPECT_EQ(image[y * kSide + x], expected) << "pixel " << x << "," << y;
        }
    }
    const Counts after = counts();
    EXPECT_EQ(merged(before, after), 2u)
        << "the second and third pass should continue the first's encoder";
    EXPECT_EQ(after.restarted - before.restarted, 0u);
}

TEST_F(RenderPassMergeTest, DrawSamplingItsDepthAttachmentSeesTheEarlierPass) {
    BackendTexture2DDesc dd{};
    dd.width = dd.height = kSide;
    dd.mip_levels = dd.array_size = 1;
    dd.pixel_format = PixelFormat::D32Float;
    dd.depth_stencil = true;
    dd.shader_resource = true;
    auto depth = device().create_texture_2d(dd, nullptr, 0);
    ASSERT_TRUE(depth && depth->valid());
    auto write_depth =
        pipeline(vertex_at(0.25f), kSolidColour, 0xC0DE5101u, true);
    auto read_depth = pipeline(vertex_at(0.25f), kDepthToRed, 0xC0DE5102u, true);
    ASSERT_TRUE(write_depth && read_depth);
    (void)pixels();
    const Counts before = counts();

    // Pass 1 clears depth to 1 and writes 0.25 everywhere; pass 2, on the same
    // attachments, reads it back through a texture (D3D11's read-only DSV).
    // Metal does not define a texture read of an attachment the same encoder
    // is still writing. On an M4 Pro a 4x4 target happened to read 0.25 even
    // with the split disabled (negative control, 2026-09-29), so the pixels
    // alone do not catch a missing boundary; the restart count does.
    const float black[4]{0, 0, 0, 1};
    BackendDrawCall first{};
    first.pipeline = write_depth.get();
    first.fragment_constants = black;
    first.fragment_constants_size = sizeof(black);
    first.depth_test_enabled = true;
    first.depth_write_enabled = true;
    first.depth_compare = BackendCompare::Always;
    pass(first, depth.get(), /*clear_depth=*/true);
    BackendDrawCall second{};
    second.pipeline = read_depth.get();
    second.textures[0] = depth.get();
    pass(second, depth.get());

    const std::vector<u32> image = pixels();
    for (u32 i = 0; i < kSide * kSide; ++i) {
        const u32 red = (image[i] >> 16) & 0xFFu;
        EXPECT_NEAR(static_cast<double>(red), 0.25 * 255.0, 1.0)
            << "pixel " << i << " = 0x" << std::hex << image[i];
    }
    const Counts after = counts();
    EXPECT_EQ(merged(before, after), 1u);
    EXPECT_EQ(after.restarted - before.restarted, 1u)
        << "the sampling draw must get an encoder boundary before it";
}

TEST_F(RenderPassMergeTest, NoPassJoinsAnEncoderAfterAUavWrite) {
    const std::string writes_uav =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 main0(float4 position [[position]],\n"
        "    device uint* words [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) + ")]]) {\n"
        "    words[uint(position.y) * 4u + uint(position.x)] = 7u;\n"
        "    return float4(0.0, 1.0, 0.0, 1.0);\n}\n";
    auto writer = pipeline(vertex_at(0.5f), writes_uav, 0xC0DE5103u, false);
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5100u, false);
    ASSERT_TRUE(writer && solid);
    BackendBufferDesc bd{};
    bd.byte_width = kSide * kSide * 4;
    bd.shader_resource = true;
    bd.unordered_access = true;
    const std::vector<u32> zeros(kSide * kSide, 0u);
    auto words = device().create_buffer(bd, zeros.data());
    ASSERT_TRUE(words);
    (void)pixels();
    const Counts before = counts();

    BackendDrawCall first{};
    first.pipeline = writer.get();
    first.fragment_uav_buffers[0] = words.get();
    pass(first);
    const float blue[4]{0, 0, 1, 1};
    BackendDrawCall second{};
    second.pipeline = solid.get();
    second.fragment_constants = blue;
    second.fragment_constants_size = sizeof(blue);
    second.scissor_enabled = true;
    second.scissor = {0, 0, static_cast<int>(kSide), 1};
    pass(second);

    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            EXPECT_EQ(image[y * kSide + x], y < 1 ? kBlue : kGreen)
                << "pixel " << x << "," << y;
        }
    }
    EXPECT_EQ(merged(before, counts()), 0u)
        << "Metal does not order a UAV write against the next draw inside "
           "one encoder";
}

// A table-mode shader (past the 8 flat texture slots) binds its t# through an
// argument buffer. Only the u# table can hold a writable view, so read-only
// tables must not stop merging: UE's base-pass vertex shaders are table-mode,
// and treating every table as a write left the G-buffer at one draw per pass.
std::string table_fragment(const char* type, const char* body) {
    return std::string("#include <metal_stdlib>\nusing namespace metal;\n"
                       "struct Srvs { ") +
           type + " t [[id(0)]]; };\n"
                  "fragment float4 main0(float4 position [[position]],\n"
                  "    constant Srvs& srvs [[buffer(" +
           std::to_string(BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1) +
           ")]]) {\n" + body + "\n}\n";
}

BackendTextureViewDesc whole_2d() {
    BackendTextureViewDesc view{};
    view.dimension = BackendTextureViewDimension::Texture2D;
    view.mip_levels = 1;
    view.array_size = 1;
    return view;
}

TEST_F(RenderPassMergeTest, ReadOnlyTableDrawsStillShareAnEncoder) {
    if (!device().supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }
    BackendTexture2DDesc vd{};
    vd.width = vd.height = 1;
    vd.mip_levels = vd.array_size = 1;
    vd.pixel_format = PixelFormat::BGRA8Unorm;
    vd.shader_resource = true;
    const u32 green = kGreen;
    auto value = device().create_texture_2d(vd, &green, 4);
    ASSERT_TRUE(value && value->valid());
    auto table = device().create_descriptor_table(
        {1, BackendDescriptorClass::Texture});
    ASSERT_TRUE(table && table->set_texture(0, value.get(), whole_2d()));
    auto reader = pipeline(
        vertex_at(0.5f),
        table_fragment("texture2d<float>", "    return srvs.t.read(uint2(0, 0));"),
        0xC0DE5104u, false);
    ASSERT_TRUE(reader);
    (void)pixels();
    const Counts before = counts();

    BackendDrawCall call{};
    call.pipeline = reader.get();
    call.fragment_descriptor_tables[1] = table.get();
    pass(call);
    pass(call);

    for (const u32 pixel : pixels()) EXPECT_EQ(pixel, kGreen);
    const Counts after = counts();
    EXPECT_EQ(merged(before, after), 1u)
        << "a t# table is read-only and must not end the encoder";
    EXPECT_EQ(after.restarted - before.restarted, 0u);
}

TEST_F(RenderPassMergeTest, DepthAttachmentReadThroughATableGetsItsOwnEncoder) {
    if (!device().supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }
    BackendTexture2DDesc dd{};
    dd.width = dd.height = kSide;
    dd.mip_levels = dd.array_size = 1;
    dd.pixel_format = PixelFormat::D32Float;
    dd.depth_stencil = true;
    dd.shader_resource = true;
    auto depth = device().create_texture_2d(dd, nullptr, 0);
    ASSERT_TRUE(depth && depth->valid());
    auto table = device().create_descriptor_table(
        {1, BackendDescriptorClass::Texture});
    ASSERT_TRUE(table && table->set_texture(0, depth.get(), whole_2d()));
    auto write_depth =
        pipeline(vertex_at(0.25f), kSolidColour, 0xC0DE5101u, true);
    auto read_depth = pipeline(
        vertex_at(0.25f),
        table_fragment("depth2d<float>",
                       "    return float4(srvs.t.read(uint2(position.xy)), "
                       "0.0, 0.0, 1.0);"),
        0xC0DE5105u, true);
    ASSERT_TRUE(write_depth && read_depth);
    const u64 restarted_before = restarted();

    const float black[4]{0, 0, 0, 1};
    BackendDrawCall first{};
    first.pipeline = write_depth.get();
    first.fragment_constants = black;
    first.fragment_constants_size = sizeof(black);
    first.depth_test_enabled = true;
    first.depth_write_enabled = true;
    first.depth_compare = BackendCompare::Always;
    pass(first, depth.get(), /*clear_depth=*/true);
    BackendDrawCall second{};
    second.pipeline = read_depth.get();
    second.fragment_descriptor_tables[1] = table.get();
    pass(second, depth.get());

    for (const u32 pixel : pixels()) {
        EXPECT_NEAR(static_cast<double>((pixel >> 16) & 0xFFu), 0.25 * 255.0,
                    1.0)
            << "0x" << std::hex << pixel;
    }
    EXPECT_EQ(restarted() - restarted_before, 1u)
        << "the table reaches the depth attachment; the draw needs an "
           "encoder boundary";
}

// D3D11 samples a depth buffer only through a READ-ONLY depth-stencil view,
// and UE does it in long runs of draws (decals, translucency, fog). While
// nothing in the open encoder has changed the depth attachment -- no clear or
// discard when it began, no depth- or stencil-writing draw since -- the texture
// in memory is exactly what the attachment holds, so the sampling draw stays in
// the encoder. Restarting there cost Satisfactory ~130 extra encoders a frame.
TEST_F(RenderPassMergeTest, DepthSampledInAnEncoderThatNeverChangedItStaysInIt) {
    BackendTexture2DDesc dd{};
    dd.width = dd.height = kSide;
    dd.mip_levels = dd.array_size = 1;
    dd.pixel_format = PixelFormat::D32Float;
    dd.depth_stencil = true;
    dd.shader_resource = true;
    auto depth = device().create_texture_2d(dd, nullptr, 0);
    ASSERT_TRUE(depth && depth->valid());
    auto write_depth =
        pipeline(vertex_at(0.25f), kSolidColour, 0xC0DE5101u, true);
    auto read_depth = pipeline(vertex_at(0.25f), kDepthToRed, 0xC0DE5102u, true);
    ASSERT_TRUE(write_depth && read_depth);

    // Depth = 0.25 everywhere, in a submission of its own.
    const float black[4]{0, 0, 0, 1};
    BackendDrawCall fill{};
    fill.pipeline = write_depth.get();
    fill.fragment_constants = black;
    fill.fragment_constants_size = sizeof(black);
    fill.depth_test_enabled = true;
    fill.depth_write_enabled = true;
    fill.depth_compare = BackendCompare::Always;
    pass(fill, depth.get(), /*clear_depth=*/true);
    (void)pixels();

    const Counts before = counts();
    // A depth-tested draw that does not write depth, then a pass on the same
    // attachments that samples the depth texture.
    const float green[4]{0, 1, 0, 1};
    BackendDrawCall tested{};
    tested.pipeline = write_depth.get();
    tested.fragment_constants = green;
    tested.fragment_constants_size = sizeof(green);
    tested.depth_test_enabled = true;
    tested.depth_write_enabled = false;
    tested.depth_compare = BackendCompare::LessEqual;
    pass(tested, depth.get());
    BackendDrawCall sample{};
    sample.pipeline = read_depth.get();
    sample.textures[0] = depth.get();
    pass(sample, depth.get());

    for (const u32 pixel : pixels()) {
        EXPECT_NEAR(static_cast<double>((pixel >> 16) & 0xFFu), 0.25 * 255.0,
                    1.0)
            << "0x" << std::hex << pixel;
    }
    const Counts after = counts();
    EXPECT_EQ(merged(before, after), 1u);
    EXPECT_EQ(after.restarted - before.restarted, 0u)
        << "nothing in the encoder changed the depth attachment";
}

// The other half of the rule: a draw that WRITES depth earlier in the encoder
// (no clear involved) still puts a boundary before the draw that samples it,
// and the sample sees the written value, not the one loaded at begin.
TEST_F(RenderPassMergeTest, DepthWrittenEarlierInTheEncoderStillGetsABoundary) {
    BackendTexture2DDesc dd{};
    dd.width = dd.height = kSide;
    dd.mip_levels = dd.array_size = 1;
    dd.pixel_format = PixelFormat::D32Float;
    dd.depth_stencil = true;
    dd.shader_resource = true;
    auto depth = device().create_texture_2d(dd, nullptr, 0);
    ASSERT_TRUE(depth && depth->valid());
    auto write_depth =
        pipeline(vertex_at(0.25f), kSolidColour, 0xC0DE5101u, true);
    auto read_depth = pipeline(vertex_at(0.25f), kDepthToRed, 0xC0DE5102u, true);
    ASSERT_TRUE(write_depth && read_depth);

    // Depth = 1.0 (the clear value), in a submission of its own.
    const float black[4]{0, 0, 0, 1};
    BackendDrawCall clear_only{};
    clear_only.pipeline = write_depth.get();
    clear_only.fragment_constants = black;
    clear_only.fragment_constants_size = sizeof(black);
    pass(clear_only, depth.get(), /*clear_depth=*/true);
    (void)pixels();

    const u64 restarted_before = restarted();
    BackendDrawCall write{};
    write.pipeline = write_depth.get();
    write.fragment_constants = black;
    write.fragment_constants_size = sizeof(black);
    write.depth_test_enabled = true;
    write.depth_write_enabled = true;
    write.depth_compare = BackendCompare::Always;
    pass(write, depth.get());
    BackendDrawCall sample{};
    sample.pipeline = read_depth.get();
    sample.textures[0] = depth.get();
    pass(sample, depth.get());

    for (const u32 pixel : pixels()) {
        EXPECT_NEAR(static_cast<double>((pixel >> 16) & 0xFFu), 0.25 * 255.0,
                    1.0)
            << "0x" << std::hex << pixel;
    }
    EXPECT_EQ(restarted() - restarted_before, 1u)
        << "a draw in the encoder wrote depth; the sampler needs a boundary";
}

// D3D11 creates shaders long before it draws with them, so the backend is
// told at creation (prefetch_shader) and compiles in the background. The draw
// must then get exactly the function it would have compiled itself: waiting
// for a compile still in flight, and failing -- not hanging -- when the
// prefetched source does not compile.
TEST_F(RenderPassMergeTest, PrefetchedShadersServeThePipelineThatNeedsThem) {
    // Unique sources, so no earlier test has put these keys in the cache.
    const std::string fragment =
        std::string(kSolidColour) + "\n// prefetch-test " +
        std::to_string(reinterpret_cast<uintptr_t>(this)) + "\n";
    const std::string vertex = std::string(vertex_at(0.5f)) +
                               "\n// prefetch-test " +
                               std::to_string(reinterpret_cast<uintptr_t>(this)) +
                               "\n";
    device().prefetch_shader(vertex, "main0", BackendMathMode::Safe, false);
    device().prefetch_shader(fragment, "main0", BackendMathMode::Safe, false);
    // Immediately: the compiles are most likely still running, so this waits.
    BackendPipelineDesc pdesc{};
    pdesc.vertex_shader_msl = vertex;
    pdesc.fragment_shader_msl = fragment;
    pdesc.vertex_entry_point = "main0";
    pdesc.fragment_entry_point = "main0";
    pdesc.vertex_math_mode = BackendMathMode::Safe;
    pdesc.fragment_math_mode = BackendMathMode::Safe;
    pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
    pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
    pdesc.color_format = PixelFormat::BGRA8Unorm;
    pdesc.cache_key = 0xC0DE5106u;
    auto solid = device().create_pipeline(pdesc);
    ASSERT_TRUE(solid && solid->valid());
    const float blue[4]{0, 0, 1, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = blue;
    call.fragment_constants_size = sizeof(blue);
    pass(call);
    for (const u32 pixel : pixels()) EXPECT_EQ(pixel, kBlue);

    // A prefetch that fails to compile leaves nothing behind: the pipeline
    // that needs it compiles again, fails, and says so.
    const std::string broken = "#include <metal_stdlib>\nthis is not MSL;\n";
    device().prefetch_shader(broken, "main0", BackendMathMode::Safe, false);
    BackendPipelineDesc bad = pdesc;
    bad.fragment_shader_msl = broken;
    bad.cache_key = 0xC0DE5107u;
    EXPECT_EQ(device().create_pipeline(bad), nullptr);
}

// ClearRenderTargetView records a clear-only pass; the pass that then renders
// to that view takes the clear as its load action instead
// (MetalSubmitQueue::fold_trailing_clears_locked). What it produces must not
// change: the clear lands after everything recorded before it and under
// everything drawn after it.
TEST_F(RenderPassMergeTest, AClearFoldsIntoThePassThatRendersToItsView) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5109u, false);
    ASSERT_TRUE(solid);
    const u64 folded_before = MetalSubmitQueue::instance().clears_folded();
    const float blue[4]{0, 0, 1, 1};
    const float green[4]{0, 1, 0, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = blue;
    call.fragment_constants_size = sizeof(blue);
    pass(call);
    ASSERT_TRUE(target->clear_render_target_view({1, 0, 0, 1},
                                                 PixelFormat::BGRA8Unorm, 0, 0, 1));
    call.fragment_constants = green;
    call.scissor_enabled = true;
    call.scissor = {0, 0, 2, static_cast<int>(kSide)};
    pass(call);

    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            EXPECT_EQ(image[y * kSide + x], x < 2 ? kGreen : kRed)
                << "pixel " << x << "," << y;
        }
    }
    EXPECT_EQ(MetalSubmitQueue::instance().clears_folded() - folded_before, 1u)
        << "the clear should have become the draw pass's load action";
}

// Two clears of one view before the pass that renders to it: the later one is
// what the pass starts from.
TEST_F(RenderPassMergeTest, TheLaterOfTwoFoldedClearsOfOneViewWins) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE510Au, false);
    ASSERT_TRUE(solid);
    const u64 folded_before = MetalSubmitQueue::instance().clears_folded();
    ASSERT_TRUE(target->clear_render_target_view({1, 0, 0, 1},
                                                 PixelFormat::BGRA8Unorm, 0, 0, 1));
    ASSERT_TRUE(target->clear_render_target_view({0, 0, 1, 1},
                                                 PixelFormat::BGRA8Unorm, 0, 0, 1));
    const float green[4]{0, 1, 0, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = green;
    call.fragment_constants_size = sizeof(green);
    call.scissor_enabled = true;
    call.scissor = {0, 0, static_cast<int>(kSide), 1};
    pass(call);

    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            EXPECT_EQ(image[y * kSide + x], y < 1 ? kGreen : kBlue)
                << "pixel " << x << "," << y;
        }
    }
    EXPECT_EQ(MetalSubmitQueue::instance().clears_folded() - folded_before, 2u);
}

// A clear that no pass renders after is still performed: a readback sees it.
TEST_F(RenderPassMergeTest, AClearNoPassFollowsIsStillPerformed) {
    const u64 folded_before = MetalSubmitQueue::instance().clears_folded();
    ASSERT_TRUE(target->clear_render_target_view({1, 0, 0, 1},
                                                 PixelFormat::BGRA8Unorm, 0, 0, 1));
    for (const u32 pixel : pixels()) EXPECT_EQ(pixel, kRed);
    EXPECT_EQ(MetalSubmitQueue::instance().clears_folded(), folded_before);
}

// A recorded list's deque blocks (~54 KB each) come back from the encode
// thread and build the next list: a second list as long as the first takes no
// fresh large allocation, and still draws what it recorded.
TEST_F(RenderPassMergeTest, ANewListIsRecordedInTheBlocksTheEncoderFreed) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5108u, false);
    ASSERT_TRUE(solid);
    const float colours[2][4]{{1, 0, 0, 1}, {0, 1, 0, 1}};
    const auto round = [&](const float* last) {
        BackendDrawCall call{};
        call.pipeline = solid.get();
        call.fragment_constants_size = sizeof(colours[0]);
        for (int i = 0; i < 300; ++i) {
            call.fragment_constants = i == 299 ? last : colours[i & 1];
            pass(call);
        }
        return pixels();
    };
    BlockRecycler::drain_for_test();
    (void)round(colours[0]);
    // The encode thread frees the list after encoding it; readback only waits
    // for the GPU.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (BlockRecycler::stats().cached_bytes == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const BlockRecycler::Stats before = BlockRecycler::stats();
    ASSERT_GT(before.cached_bytes, 0u) << "the encoded list's blocks were freed, not kept";
    const std::vector<u32> image = round(colours[1]);
    const BlockRecycler::Stats after = BlockRecycler::stats();
    EXPECT_EQ(after.fresh, before.fresh)
        << "the second list allocated blocks the first had returned";
    EXPECT_GT(after.reused, before.reused);
    for (u32 v : image) EXPECT_EQ(v, kGreen);
}

// ---------------------------------------------------------------------------
// WP-R2, the slim per-draw record (graphics/contracts/slim_record.hpp). The
// first three run with the switch as the environment sets it -- on by
// default -- so under VITREO_METAL_SLIM_RECORD=0 each fails: their RED.

TEST_F(RenderPassMergeTest, ConsecutiveSameAttachmentDrawsRecordOneBeginAndEndPass) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5110u, false);
    ASSERT_TRUE(solid);
    (void)pixels();
    const Counts before = counts();
    const float red[4]{1, 0, 0, 1};
    const float green[4]{0, 1, 0, 1};
    const float blue[4]{0, 0, 1, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = red;
    call.fragment_constants_size = sizeof(red);
    pass(call);
    call.fragment_constants = green;
    call.scissor_enabled = true;
    call.scissor = {0, 0, 2, static_cast<int>(kSide)};
    pass(call);
    call.fragment_constants = blue;
    call.scissor = {0, 0, static_cast<int>(kSide), 1};
    pass(call);

    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            EXPECT_EQ(image[y * kSide + x], y < 1 ? kBlue : x < 2 ? kGreen : kRed)
                << "pixel " << x << "," << y;
        }
    }
    const Counts after = counts();
    EXPECT_EQ(after.begins - before.begins, 1u)
        << "three passes on one attachment set are recorded as one";
    EXPECT_EQ(after.ends - before.ends, 1u);
    EXPECT_EQ(after.folded - before.folded, 2u);
    EXPECT_EQ(after.continued - before.continued, 0u)
        << "nothing is left for the encoder to continue";
    EXPECT_EQ(merged(before, after), 2u);
}

TEST_F(RenderPassMergeTest, IdenticalPushBlocksShareOneArenaCopy) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5111u, false);
    ASSERT_TRUE(solid);
    (void)pixels();
    const Counts before = counts();
    // Equal bytes at three different addresses: shared by contents.
    const float red_a[4]{1, 0, 0, 1};
    const float red_b[4]{1, 0, 0, 1};
    const float red_c[4]{1, 0, 0, 1};
    const float green[4]{0, 1, 0, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants_size = sizeof(red_a);
    for (const float* block : {red_a, red_b, red_c}) {
        call.fragment_constants = block;
        pass(call);
    }
    call.fragment_constants = green;
    call.scissor_enabled = true;
    call.scissor = {0, 0, 2, static_cast<int>(kSide)};
    pass(call);

    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            EXPECT_EQ(image[y * kSide + x], x < 2 ? kGreen : kRed)
                << "pixel " << x << "," << y;
        }
    }
    const Counts after = counts();
    EXPECT_EQ(after.copied - before.copied, 2u)
        << "one copy of the red block, one of the green";
    EXPECT_EQ(after.shared - before.shared, 2u)
        << "the second and third red draws share the first's copy";
}

// Red = the attachment's depth, green and blue = the push block's.
constexpr const char* kDepthAndColour = R"(
    #include <metal_stdlib>
    using namespace metal;
    fragment float4 main0(float4 position [[position]],
                          constant float4& colour [[buffer(0)]],
                          depth2d<float> depth [[texture(0)]]) {
        return float4(depth.read(uint2(position.xy)), colour.g, colour.b, 1.0);
    }
)";

// A folded pass whose next draw samples the depth attachment is split again,
// and the encoder gives that draw an encoder of its own. Its push block --
// the same bytes as the draw before, so the same arena copy -- must still be
// bound in the new encoder.
TEST_F(RenderPassMergeTest, ASplitPassStillBindsItsPushBlocks) {
    BackendTexture2DDesc dd{};
    dd.width = dd.height = kSide;
    dd.mip_levels = dd.array_size = 1;
    dd.pixel_format = PixelFormat::D32Float;
    dd.depth_stencil = true;
    dd.shader_resource = true;
    auto depth = device().create_texture_2d(dd, nullptr, 0);
    ASSERT_TRUE(depth && depth->valid());
    auto write_depth =
        pipeline(vertex_at(0.25f), kSolidColour, 0xC0DE5101u, true);
    auto sample = pipeline(vertex_at(0.25f), kDepthAndColour, 0xC0DE5112u, true);
    ASSERT_TRUE(write_depth && sample);
    (void)pixels();
    const Counts before = counts();

    const float colour[4]{0, 1, 0, 1};
    BackendDrawCall first{};
    first.pipeline = write_depth.get();
    first.fragment_constants = colour;
    first.fragment_constants_size = sizeof(colour);
    first.depth_test_enabled = true;
    first.depth_write_enabled = true;
    first.depth_compare = BackendCompare::Always;
    pass(first, depth.get(), /*clear_depth=*/true);
    BackendDrawCall second{};
    second.pipeline = sample.get();
    second.fragment_constants = colour;
    second.fragment_constants_size = sizeof(colour);
    second.textures[0] = depth.get();
    pass(second, depth.get());

    for (const u32 pixel : pixels()) {
        EXPECT_NEAR(static_cast<double>((pixel >> 16) & 0xFFu), 0.25 * 255.0,
                    1.0)
            << "0x" << std::hex << pixel;
        EXPECT_EQ((pixel >> 8) & 0xFFu, 0xFFu)
            << "the push block's green, bound in the new encoder: 0x"
            << std::hex << pixel;
        EXPECT_EQ(pixel & 0xFFu, 0u) << "0x" << std::hex << pixel;
    }
    const Counts after = counts();
    EXPECT_EQ(after.folded - before.folded, 1u);
    EXPECT_EQ(after.split - before.split, 1u)
        << "the sampling draw splits the folded pass";
    EXPECT_EQ(after.restarted - before.restarted, 1u)
        << "and the encoder gives it a boundary (the depth was written)";
    EXPECT_GE(after.shared - before.shared, 1u)
        << "both draws carry one copy of the block";
}

// Every split rule, three ways: pass merging off (VITREO_METAL_PASS_MERGE=0,
// the reference: one encoder per pass), merging decided by the encoder alone
// (slim record off), and folding at record time (slim record on). The pixels
// and the GPU-written results are the reference's, and the two merging arms
// share the same passes and restart the same draws.
TEST_F(RenderPassMergeTest, FoldedPassesMatchThePassMergeOffReferenceAcrossEverySplitRule) {
    const std::string writes_uav =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 main0(float4 position [[position]],\n"
        "    device uint* words [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) + ")]]) {\n"
        "    words[uint(position.y) * 4u + uint(position.x)] += 1u;\n"
        "    return float4(0.0, 1.0, 0.0, 1.0);\n}\n";
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5120u, false);
    auto writer = pipeline(vertex_at(0.5f), writes_uav, 0xC0DE5121u, false);
    auto write_depth =
        pipeline(vertex_at(0.25f), kSolidColour, 0xC0DE5122u, true);
    auto test_depth = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5123u, true);
    auto read_depth = pipeline(vertex_at(0.25f), kDepthToRed, 0xC0DE5124u, true);
    ASSERT_TRUE(solid && writer && write_depth && test_depth && read_depth);
    const bool tables = device().supports_descriptor_tables();
    std::unique_ptr<BackendPipeline> table_depth;
    if (tables) {
        table_depth = pipeline(
            vertex_at(0.25f),
            table_fragment("depth2d<float>",
                           "    return float4(srvs.t.read(uint2(position.xy)), "
                           "0.0, 0.0, 1.0);"),
            0xC0DE5125u, true);
        ASSERT_TRUE(table_depth);
    }
    // Reads the colour attachment at the fragment: red and blue from it.
    constexpr const char* kReadColour = R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 main0(float4 position [[position]],
                              texture2d<float> t [[texture(0)]]) {
            const float4 c = t.read(uint2(position.xy));
            return float4(c.r, 1.0, c.b, 1.0);
        }
    )";
    // Reads the colour attachment in the VERTEX stage (vertex_textures).
    constexpr const char* kVertexReadsTexture = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float2 position [[attribute(0)]]; };
        struct main0_out {
            float4 position [[position]];
            float4 colour [[user(locn0)]];
        };
        vertex main0_out main0(main0_in in [[stage_in]],
                               texture2d<float> t [[texture(0)]]) {
            main0_out out = {};
            out.position = float4(in.position, 0.5, 1.0);
            out.colour = float4(t.read(uint2(0, 0)).r, 0.5, 1.0, 1.0);
            return out;
        }
    )";
    constexpr const char* kVaryingColour = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float4 colour [[user(locn0)]]; };
        fragment float4 main0(main0_in in [[stage_in]]) { return in.colour; }
    )";
    auto read_colour = pipeline(vertex_at(0.5f), kReadColour, 0xC0DE5126u, false);
    auto vertex_read =
        pipeline(kVertexReadsTexture, kVaryingColour, 0xC0DE5127u, false);
    ASSERT_TRUE(read_colour && vertex_read);
    std::unique_ptr<BackendPipeline> table_colour;
    if (tables) {
        table_colour = pipeline(
            vertex_at(0.5f),
            table_fragment("texture2d<float>",
                           "    const float4 c = srvs.t.read(uint2(position.xy));\n"
                           "    return float4(c.r, c.g, 1.0, 1.0);"),
            0xC0DE5128u, false);
        ASSERT_TRUE(table_colour);
    }
    const bool msaa_ok = device().supports_texture_sample_count(4);
    std::unique_ptr<BackendPipeline> msaa_solid;
    if (msaa_ok) {
        msaa_solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5129u,
                              false, 4);
        ASSERT_TRUE(msaa_solid);
    }

    // Fresh per arm, so every arm starts from the same memory.
    std::unique_ptr<BackendTexture2D> depth;
    std::unique_ptr<BackendTexture2D> msaa;
    std::unique_ptr<BackendBuffer> words;
    std::unique_ptr<BackendBuffer> source;
    std::unique_ptr<BackendDescriptorTable> table;
    std::unique_ptr<BackendDescriptorTable> colour_table;
    std::shared_ptr<GpuOcclusionCounter> occlusion;
    const auto fresh = [&] {
        if (msaa_ok) {
            BackendTexture2DDesc md{};
            md.width = md.height = kSide;
            md.mip_levels = md.array_size = 1;
            md.pixel_format = PixelFormat::BGRA8Unorm;
            md.render_target = true;
            md.sample_count = 4;
            msaa = device().create_texture_2d(md, nullptr, 0);
            ASSERT_TRUE(msaa && msaa->valid());
        }
        BackendBufferDesc sd{};
        sd.byte_width = 16;
        sd.shader_resource = true;
        const u32 pattern[4]{0x11u, 0x22u, 0x33u, 0x44u};
        source = device().create_buffer(sd, pattern);
        ASSERT_TRUE(source);
        if (tables) {
            BackendTextureViewDesc rgb1 = whole_2d();
            rgb1.swizzle = BackendViewSwizzle::RGB1;
            colour_table = device().create_descriptor_table(
                {1, BackendDescriptorClass::Texture});
            ASSERT_TRUE(colour_table &&
                        colour_table->set_texture(0, target.get(), rgb1));
        }
        BackendTexture2DDesc dd{};
        dd.width = dd.height = kSide;
        dd.mip_levels = dd.array_size = 1;
        dd.pixel_format = PixelFormat::D32Float;
        dd.depth_stencil = true;
        dd.shader_resource = true;
        depth = device().create_texture_2d(dd, nullptr, 0);
        BackendBufferDesc bd{};
        bd.byte_width = kSide * kSide * 4;
        bd.shader_resource = true;
        bd.unordered_access = true;
        const std::vector<u32> zeros(kSide * kSide, 0u);
        words = device().create_buffer(bd, zeros.data());
        if (tables) {
            table = device().create_descriptor_table(
                {1, BackendDescriptorClass::Texture});
            ASSERT_TRUE(table && table->set_texture(0, depth.get(), whole_2d()));
        }
        occlusion = device().create_occlusion_counter();
        ASSERT_TRUE(depth && words);
    };
    const float red[4]{1, 0, 0, 1};
    const float green[4]{0, 1, 0, 1};
    const float blue[4]{0, 0, 1, 1};
    const auto solid_call = [&](const float* colour, int sx, int sy, int sw,
                                int sh) {
        BackendDrawCall call{};
        call.pipeline = solid.get();
        call.fragment_constants = colour;
        call.fragment_constants_size = 16;
        call.scissor_enabled = true;
        call.scissor = {sx, sy, sw, sh};
        return call;
    };
    const int n = static_cast<int>(kSide);
    const auto depth_write = [&](bool clear) {
        BackendDrawCall call{};
        call.pipeline = write_depth.get();
        call.fragment_constants = blue;
        call.fragment_constants_size = 16;
        call.depth_test_enabled = true;
        call.depth_write_enabled = true;
        call.depth_compare = BackendCompare::Always;
        pass(call, depth.get(), clear);
    };
    const auto depth_sample = [&] {
        BackendDrawCall call{};
        call.pipeline = read_depth.get();
        call.textures[0] = depth.get();
        pass(call, depth.get());
    };
    const auto uav_write = [&] {
        BackendDrawCall call{};
        call.pipeline = writer.get();
        call.fragment_uav_buffers[0] = words.get();
        pass(call);
    };

    struct Scenario {
        const char* name;
        std::function<void()> record;
        bool folds;  ///< the fold arm must fold at least one pass
    };
    std::vector<Scenario> scenarios{
        {"plain passes keep their order",
         [&] {
             pass(solid_call(red, 0, 0, n, n));
             pass(solid_call(green, 0, 0, 2, n));
             pass(solid_call(blue, 0, 0, n, 1));
         },
         true},
        {"depth sampled after a depth write (dirty)",
         [&] {
             depth_write(true);
             depth_sample();
             pass(solid_call(green, 0, 0, 1, n), depth.get());
         },
         true},
        {"depth sampled while nothing changed it (clean)",
         [&] {
             depth_write(true);
             (void)pixels();
             BackendDrawCall tested = solid_call(green, 0, 0, n, n);
             tested.pipeline = test_depth.get();
             tested.depth_test_enabled = true;
             tested.depth_write_enabled = false;
             tested.depth_compare = BackendCompare::LessEqual;
             pass(tested, depth.get());
             depth_sample();
             depth_sample();
         },
         true},
        {"a UAV write, then plain passes",
         [&] {
             uav_write();
             pass(solid_call(blue, 0, 0, n, 1));
             pass(solid_call(red, 0, 0, 1, n));
         },
         true},
        {"plain, a UAV write, plain",
         [&] {
             pass(solid_call(red, 0, 0, n, n));
             uav_write();
             pass(solid_call(blue, 0, 0, n, 1));
             uav_write();
         },
         true},
        {"a clear folded into the pass after it",
         [&] {
             pass(solid_call(blue, 0, 0, n, n));
             ASSERT_TRUE(target->clear_render_target_view(
                 {1, 0, 0, 1}, PixelFormat::BGRA8Unorm, 0, 0, 1));
             pass(solid_call(green, 0, 0, 2, n));
             pass(solid_call(blue, 0, 0, n, 1));
         },
         true},
        {"occlusion-counted passes",
         [&] {
             BackendDrawCall a = solid_call(red, 0, 0, n, n);
             pass(a, nullptr, false, occlusion.get());
             pass(solid_call(green, 0, 0, 2, n), nullptr, false,
                  occlusion.get());
             pass(solid_call(blue, 0, 0, n, 1));
             pass(solid_call(red, 0, 0, 1, 1));
         },
         true},
        {"attachments that differ",
         [&] {
             pass(solid_call(red, 0, 0, n, n));
             depth_write(true);
             pass(solid_call(green, 0, 0, 2, n));
             pass(solid_call(blue, 0, 0, n, 1), depth.get());
         },
         false},
    };
    // A colour attachment read inside a folded pass: the encoder's "a colour
    // attachment always needs a boundary".
    const auto read_colour_call = [&](BackendTextureViewDesc view) {
        BackendDrawCall call = solid_call(red, 0, 0, 2, n);
        call.pipeline = read_colour.get();
        call.fragment_constants = nullptr;
        call.fragment_constants_size = 0;
        call.textures[0] = target.get();
        call.texture_views[0] = view;
        return call;
    };
    scenarios.push_back(
        {"the colour attachment sampled in a folded pass",
         [&] {
             pass(solid_call(blue, 0, 0, n, n));
             pass(solid_call(red, 0, 0, n, 1));
             pass(read_colour_call({}));
             pass(solid_call(green, 3, 0, 1, n));
         },
         true});
    {
        BackendTextureViewDesc rgb1 = whole_2d();
        rgb1.swizzle = BackendViewSwizzle::RGB1;  // a real view: parentTexture
        scenarios.push_back(
            {"the colour attachment sampled through a view of it",
             [&, rgb1] {
                 pass(solid_call(blue, 0, 0, n, n));
                 pass(solid_call(red, 0, 0, n, 1));
                 pass(read_colour_call(rgb1));
                 pass(solid_call(green, 3, 0, 1, n));
             },
             true});
    }
    scenarios.push_back(
        {"the colour attachment read by the vertex stage",
         [&] {
             pass(solid_call(red, 0, 0, n, n));
             pass(solid_call(green, 0, 0, 1, 1));
             BackendDrawCall call = solid_call(red, 0, 0, n, 2);
             call.pipeline = vertex_read.get();
             call.fragment_constants = nullptr;
             call.fragment_constants_size = 0;
             call.vertex_textures[0] = target.get();
             pass(call);
             pass(solid_call(blue, 0, 3, n, 1));
         },
         true});
    // At function scope: the scenarios run after this block, and capture it
    // by reference.
    const auto msaa_call = [&](const float* colour, int sx, int sy, int sw,
                               int sh) {
        BackendDrawCall call = solid_call(colour, sx, sy, sw, sh);
        call.pipeline = msaa_solid.get();
        return call;
    };
    if (msaa_ok) {
        // MSAA passes fold like any other; the resolve is an op of its own,
        // which ends the folded pass first.
        scenarios.push_back(
            {"MSAA passes and their resolve",
             [&] {
                 pass_on(msaa.get(), msaa_call(red, 0, 0, n, n), nullptr,
                         false, nullptr, nullptr);
                 pass_on(msaa.get(), msaa_call(green, 0, 0, 2, n), nullptr,
                         false, nullptr, nullptr);
                 ASSERT_TRUE(device().resolve_texture_2d(
                     target.get(), 0, 0, msaa.get(), 0, 0,
                     PixelFormat::BGRA8Unorm));
                 pass_on(msaa.get(), msaa_call(blue, 0, 0, n, 1), nullptr,
                         false, nullptr, nullptr);
                 pass_on(msaa.get(), msaa_call(red, 3, 0, 1, n), nullptr,
                         false, nullptr, nullptr);
                 ASSERT_TRUE(device().resolve_texture_2d(
                     target.get(), 0, 0, msaa.get(), 0, 0,
                     PixelFormat::BGRA8Unorm));
             },
             true});
    }
    // fold_settle_locked: another op recorded inside a pass that was folded
    // in -- between its BeginPass and its draw. The pass is split first, so
    // the op lands where it would have without the fold.
    const auto settle = [&](const char* name,
                            std::function<void()> foreign) {
        scenarios.push_back(
            {name,
             [&, foreign] {
                 pass(solid_call(red, 0, 0, n, n));
                 pass_on(target.get(), solid_call(green, 0, 0, 2, n),
                         nullptr, false, nullptr, foreign);
                 pass(solid_call(blue, 0, 0, n, 1));
             },
             true});
    };
    settle("a debug marker inside a folded pass", [] {
        MetalSubmitQueue::instance().record_debug_marker(
            MetalSubmitQueue::DebugMarker::Signpost, "wpr2-settle");
    });
    settle("a buffer copy inside a folded pass", [&] {
        ASSERT_TRUE(device().copy_buffer_region(words.get(), 4, source.get(),
                                                0, 8));
    });
    settle("a timestamp inside a folded pass",
           [&] { (void)device().record_timestamp(); });
    // A submission while a folded-in pass is open: its draw then reaches a
    // list of its own, as the unfolded stream's would (where the encoder
    // drops it, the pass having ended with the list before).
    settle("a submit inside a folded pass",
           [] { (void)MetalSubmitQueue::instance().submit(); });
    if (tables) {
        scenarios.push_back(
            {"the colour attachment read through a table's view",
             [&] {
                 pass(solid_call(blue, 0, 0, n, n));
                 pass(solid_call(red, 0, 0, n, 1));
                 BackendDrawCall call = solid_call(red, 0, 0, 2, n);
                 call.pipeline = table_colour.get();
                 call.fragment_constants = nullptr;
                 call.fragment_constants_size = 0;
                 call.fragment_descriptor_tables[1] = colour_table.get();
                 pass(call);
                 pass(solid_call(green, 3, 0, 1, n));
             },
             true});
        scenarios.push_back(
            {"depth read through a descriptor table",
             [&] {
                 depth_write(true);
                 BackendDrawCall call{};
                 call.pipeline = table_depth.get();
                 call.fragment_descriptor_tables[1] = table.get();
                 pass(call, depth.get());
                 pass(call, depth.get());
             },
             true});
    }

    struct Arm {
        const char* name;
        bool merge;
        slim_record::Force slim;
        delta_record::Force delta;
    };
    // The fourth arm is Track R round 3 (delta_record.hpp): delta draw
    // records, native states and pass-continue on top of the fold.
    constexpr int kArms = 4;
    const Arm arms[kArms]{
        {"merge off", false, slim_record::Force::Off, delta_record::Force::Off},
        {"encoder merges", true, slim_record::Force::Off,
         delta_record::Force::Off},
        {"record-time fold", true, slim_record::Force::On,
         delta_record::Force::Off},
        {"fold + r3 delta record", true, slim_record::Force::On,
         delta_record::Force::On}};
    for (const Scenario& scenario : scenarios) {
        SCOPED_TRACE(scenario.name);
        std::vector<u32> image[kArms];
        std::vector<u32> written[kArms];
        u64 samples[kArms]{};
        u64 merges[kArms]{};
        u64 restarts[kArms]{};
        u64 folds[kArms]{};
        u64 deltas[kArms]{};
        for (int a = 0; a < kArms; ++a) {
            SCOPED_TRACE(arms[a].name);
            (void)pixels();  // drained: the arm changes between lists
            MetalSubmitQueue::set_pass_merge_for_test(arms[a].merge);
            slim_record::force_all_for_test(arms[a].slim);
            delta_record::force_all_for_test(arms[a].delta);
            fresh();
            ASSERT_TRUE(target->clear_render_target_view(
                {0, 0, 0, 1}, PixelFormat::BGRA8Unorm, 0, 0, 1));
            (void)pixels();
            const Counts before = counts();
            const u64 deltas_before =
                MetalSubmitQueue::instance().delta_counters().draws;
            scenario.record();
            image[a] = pixels();
            const Counts after = counts();
            deltas[a] = MetalSubmitQueue::instance().delta_counters().draws -
                        deltas_before;
            written[a].assign(kSide * kSide, 0u);
            ASSERT_TRUE(words->read(0, written[a].data(), kSide * kSide * 4));
            samples[a] = occlusion ? occlusion->samples() : 0;
            merges[a] = merged(before, after);
            restarts[a] = after.restarted - before.restarted;
            folds[a] = after.folded - before.folded;
        }
        EXPECT_EQ(image[1], image[0]) << "encoder merging changed the pixels";
        EXPECT_EQ(image[2], image[0]) << "record-time folding changed the pixels";
        EXPECT_EQ(image[3], image[0]) << "the delta record changed the pixels";
        EXPECT_EQ(written[1], written[0]);
        EXPECT_EQ(written[2], written[0]) << "UAV results";
        EXPECT_EQ(written[3], written[0]) << "UAV results (delta record)";
        EXPECT_EQ(samples[1], samples[0]);
        EXPECT_EQ(samples[2], samples[0]) << "occlusion samples";
        EXPECT_EQ(samples[3], samples[0]) << "occlusion samples (delta record)";
        EXPECT_EQ(merges[0], 0u);
        EXPECT_EQ(folds[0], 0u);
        EXPECT_EQ(folds[1], 0u);
        EXPECT_EQ(merges[2], merges[1])
            << "folding merges exactly the passes the encoder would";
        EXPECT_EQ(restarts[2], restarts[1])
            << "and the encoder restarts exactly the same draws";
        EXPECT_EQ(merges[3], merges[1]) << "the delta record merges alike";
        EXPECT_EQ(restarts[3], restarts[1]);
        EXPECT_EQ(folds[3], folds[2]) << "and folds alike";
        EXPECT_EQ(deltas[0] + deltas[1] + deltas[2], 0u);
        EXPECT_GT(deltas[3], 0u) << "the delta arm recorded no delta draw";
        if (scenario.folds) {
            EXPECT_GT(folds[2], 0u) << "the fold arm never folded";
        } else {
            EXPECT_EQ(folds[2], 0u);
        }
    }
    MetalSubmitQueue::set_pass_merge_for_test(std::nullopt);
    slim_record::force_all_for_test(slim_record::Force::None);
    delta_record::force_all_for_test(delta_record::Force::None);
}

// push-intern's fallbacks. A draw whose list outlives the recording's arena
// (a captured command list, replayed later and more than once), a draw
// appended outside the in-place path (a live shared resource), and a block
// larger than an arena chunk each copy their bytes into the draw's own
// vectors at record time. Each test changes the caller's bytes after
// recording: a draw still pointing at them would draw the new colour.
TEST_F(RenderPassMergeTest, CapturedDrawsCarryTheirOwnPushBlocks) {
    if (!device().supports_command_lists()) GTEST_SKIP() << "no command lists";
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5130u, false);
    ASSERT_TRUE(solid);
    (void)pixels();
    const Counts before = counts();
    auto list = device().create_command_list();
    ASSERT_TRUE(list && list->valid());
    std::vector<float> red{1, 0, 0, 1};
    std::vector<float> green{0, 1, 0, 1};
    {
        BackendCommandListCapture capture(list.get());
        ASSERT_TRUE(capture.active());
        BackendDrawCall call{};
        call.pipeline = solid.get();
        call.fragment_constants = red.data();
        call.fragment_constants_size = 16;
        pass(call);
        call.fragment_constants = green.data();
        call.scissor_enabled = true;
        call.scissor = {0, 0, 2, static_cast<int>(kSide)};
        pass(call);
    }
    ASSERT_TRUE(list->close());
    red = {0, 0, 1, 1};    // what a draw pointing at the caller's bytes
    green = {0, 0, 1, 1};  // would now draw
    for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE(round);
        ASSERT_TRUE(target->clear_render_target_view(
            {0, 0, 0, 1}, PixelFormat::BGRA8Unorm, 0, 0, 1));
        (void)pixels();  // the recordings around it encoded and recycled
        ASSERT_TRUE(device().submit_command_list(list.get()));
        const std::vector<u32> image = pixels();
        for (u32 y = 0; y < kSide; ++y) {
            for (u32 x = 0; x < kSide; ++x) {
                EXPECT_EQ(image[y * kSide + x], x < 2 ? kGreen : kRed)
                    << "pixel " << x << "," << y;
            }
        }
    }
    const Counts after = counts();
    EXPECT_EQ(after.copied - before.copied, 0u)
        << "a captured draw's blocks are its own, never the arena's";
    EXPECT_EQ(after.shared - before.shared, 0u);
}

TEST_F(RenderPassMergeTest, APushBlockLargerThanAnArenaChunkKeepsItsOwnCopy) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5131u, false);
    ASSERT_TRUE(solid);
    (void)pixels();
    const Counts before = counts();
    // 300 KiB: over MetalSubmitQueue::PushArena::kChunkBytes (256 KiB).
    std::vector<float> block(300u * 1024u / sizeof(float), 0.0f);
    block[1] = block[3] = 1.0f;  // green
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = block.data();
    call.fragment_constants_size = block.size() * sizeof(float);
    pass(call);
    std::fill(block.begin(), block.begin() + 4, 0.0f);
    block[2] = block[3] = 1.0f;  // blue, after the draw was recorded
    for (const u32 pixel : pixels()) EXPECT_EQ(pixel, kGreen);
    const Counts after = counts();
    EXPECT_EQ(after.copied - before.copied, 0u)
        << "the oversized block went to the draw's vector, not the arena";
}

// Last in the file: while a shared resource lives, every op of the process
// takes the append path.
TEST_F(RenderPassMergeTest, SharedResourceStreamDrawsCarryTheirOwnPushBlocks) {
    auto solid = pipeline(vertex_at(0.5f), kSolidColour, 0xC0DE5132u, false);
    ASSERT_TRUE(solid);
    BackendBufferDesc sd{};
    sd.byte_width = 256;
    sd.shader_resource = true;
    sd.shareable = true;
    auto shared = device().create_buffer(sd, nullptr);
    if (!shared || MetalSharedRegistry::instance().empty()) {
        GTEST_SKIP() << "no live shared resource: the append path is not taken";
    }
    (void)pixels();
    const Counts before = counts();
    std::vector<float> red{1, 0, 0, 1};
    std::vector<float> green{0, 1, 0, 1};
    BackendDrawCall call{};
    call.pipeline = solid.get();
    call.fragment_constants = red.data();
    call.fragment_constants_size = 16;
    pass(call);
    call.fragment_constants = green.data();
    call.scissor_enabled = true;
    call.scissor = {0, 0, 2, static_cast<int>(kSide)};
    pass(call);
    red = {0, 0, 1, 1};
    green = {0, 0, 1, 1};
    const std::vector<u32> image = pixels();
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            EXPECT_EQ(image[y * kSide + x], x < 2 ? kGreen : kRed)
                << "pixel " << x << "," << y;
        }
    }
    const Counts after = counts();
    EXPECT_EQ(after.copied - before.copied, 0u)
        << "an appended draw's blocks are its own";
    EXPECT_EQ(after.folded - before.folded, 0u)
        << "the append path never folds";
    shared.reset();
    EXPECT_TRUE(MetalSharedRegistry::instance().empty())
        << "the shared resource is gone; later tests take the in-place path";
}

}  // namespace
