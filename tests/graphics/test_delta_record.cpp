// SPDX-License-Identifier: LGPL-2.1-only
//
// Track R round 3 (graphics/contracts/delta_record.hpp): the delta draw
// record, native sampler / depth-stencil states and pass-continue.
//
// A draw recorded as a delta must reach the encoder as exactly the state the
// full record carries. The golden test records one seeded random stream of
// draws twice -- every part off, then every part on -- and compares the hash
// of each draw's applied state at the encoder (cs_draw_state_hash) and the
// pixels. Its RED arms remove a keyframe on purpose and must be caught.
// The counter tests check that the parts engage: references taken once per
// recording rather than per draw, one queue op for a run of folded draws,
// EndPass ops never recorded, and buffers stamped once per generation.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <array>
#include <memory>
#include <random>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kSide = 8;

constexpr const char* kVertex = R"(
    #include <metal_stdlib>
    using namespace metal;
    struct main0_in { float2 position [[attribute(0)]]; };
    struct main0_out { float4 position [[position]]; };
    vertex main0_out main0(main0_in in [[stage_in]]) {
        main0_out out = {};
        out.position = float4(in.position, 0.5, 1.0);
        return out;
    }
)";

constexpr const char* kSolid = R"(
    #include <metal_stdlib>
    using namespace metal;
    fragment float4 main0(constant float4& colour [[buffer(0)]]) {
        return colour;
    }
)";

// The colour, times the 2x2 texture sampled across the target.
constexpr const char* kTextured = R"(
    #include <metal_stdlib>
    using namespace metal;
    fragment float4 main0(float4 position [[position]],
                          constant float4& colour [[buffer(0)]],
                          texture2d<float> t [[texture(0)]],
                          sampler s [[sampler(0)]]) {
        return colour * t.sample(s, position.xy / 8.0);
    }
)";

class DeltaRecordTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
        static const float positions[]{-1, -1, 3, -1, -1, 3};
        BackendBufferDesc vb{};
        vb.byte_width = sizeof(positions);
        vb.vertex_buffer = true;
        vertices[0] = device().create_buffer(vb, positions);
        vertices[1] = device().create_buffer(vb, positions);
        BackendTexture2DDesc td{};
        td.width = td.height = kSide;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::BGRA8Unorm;
        td.render_target = true;
        target = device().create_texture_2d(td, nullptr, 0);
        BackendTexture2DDesc dd{};
        dd.width = dd.height = kSide;
        dd.mip_levels = dd.array_size = 1;
        dd.pixel_format = PixelFormat::D32Float;
        dd.depth_stencil = true;
        depth = device().create_texture_2d(dd, nullptr, 0);
        BackendTexture2DDesc sd{};
        sd.width = sd.height = 2;
        sd.mip_levels = sd.array_size = 1;
        sd.pixel_format = PixelFormat::BGRA8Unorm;
        sd.shader_resource = true;
        const u32 texels[4]{0xFF2040FFu, 0xFFFF8020u, 0xFF80FF10u,
                            0xFF102030u};
        sampled = device().create_texture_2d(sd, texels, 8);
        ASSERT_TRUE(vertices[0] && vertices[1] && target && depth && sampled);
        solid = pipeline(kSolid, 1);
        textured = pipeline(kTextured, 2);
        ASSERT_TRUE(solid && textured);
    }

    void TearDown() override {
        MetalSubmitQueue::set_encode_hash_sink_for_test(nullptr);
        MetalSubmitQueue::set_delta_mutation_for_test(
            MetalSubmitQueue::DeltaMutation::None);
        delta_record::force_all_for_test(delta_record::Force::None);
    }

    static GraphicsBackendDevice& device() { return MetalDevice::instance(); }

    static std::unique_ptr<BackendPipeline> pipeline(const char* fragment,
                                                     u32 key) {
        BackendPipelineDesc pdesc{};
        pdesc.vertex_shader_msl = kVertex;
        pdesc.fragment_shader_msl = fragment;
        pdesc.vertex_entry_point = "main0";
        pdesc.fragment_entry_point = "main0";
        pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
        pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
        pdesc.color_format = PixelFormat::BGRA8Unorm;
        pdesc.depth_attachment = true;
        pdesc.depth_format = PixelFormat::D32Float;
        pdesc.cache_key = 0x7233000u + key;
        auto made = device().create_pipeline(pdesc);
        EXPECT_TRUE(made && made->valid());
        return made;
    }

    /// One pass holding one draw, the way the D3D11 frontend records them.
    void pass(BackendDrawCall call, bool clear_depth = false) {
        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets[0] = target.get();
        pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
        pass_desc.preserve_color_contents = true;
        pass_desc.depth_stencil_target = depth.get();
        pass_desc.clear_depth = clear_depth;
        pass_desc.depth_clear_value = 1.0f;
        auto open = device().create_render_pass_standalone(pass_desc);
        ASSERT_TRUE(open && open->valid());
        call.topology = BackendPrimitiveTopology::TriangleList;
        if (call.vertex_count == 0) call.vertex_count = 3;
        if (call.vertex_buffers[0] == nullptr) {
            call.vertex_buffers[0] = vertices[0].get();
        }
        call.vertex_buffer = call.vertex_buffers[0];
        if (call.viewport[2] == 0.0f) {
            call.viewport = {0, 0, static_cast<float>(kSide),
                             static_cast<float>(kSide), 0, 1};
        }
        ASSERT_TRUE(open->draw(call));
        ASSERT_TRUE(open->end());
    }

    BackendDrawCall solid_call(const float* colour) const {
        BackendDrawCall call{};
        call.pipeline = solid.get();
        call.fragment_constants = colour;
        call.fragment_constants_size = 16;
        return call;
    }

    std::vector<u32> pixels() {
        std::vector<u32> out(kSide * kSide, 0xDEADBEEFu);
        EXPECT_TRUE(target->readback(0, out.data(), kSide * 4));
        return out;
    }

    void clear() {
        ASSERT_TRUE(target->clear_render_target_view(
            {0, 0, 0, 1}, PixelFormat::BGRA8Unorm, 0, 0, 1));
        (void)pixels();
    }

    /// The seeded stream: every field class changing at its own rate,
    /// submits cutting it into recordings, full draws breaking the delta
    /// chain, and native states named on some draws only.
    void record_stream(u32 seed, u32 draws, bool full_draws_between) {
        std::mt19937 rng(seed);
        const auto pick = [&](u32 n) {
            return static_cast<u32>(rng() % n);
        };
        static const float colours[4][4]{{1, 0, 0, 1},
                                         {0, 1, 0, 1},
                                         {0.25f, 0.5f, 1, 1},
                                         {0.5f, 0.5f, 0.5f, 0.5f}};
        BackendSamplerDesc samplers[2]{};
        samplers[0].min_filter = samplers[0].mag_filter = BackendFilter::Nearest;
        samplers[1].min_filter = samplers[1].mag_filter = BackendFilter::Linear;
        samplers[1].address_u = samplers[1].address_v =
            BackendAddressMode::Clamp;
        if (!native_samplers[0]) {
            for (int i = 0; i < 2; ++i) {
                native_samplers[i] = device().create_sampler_state(samplers[i]);
            }
        }
        float colour_storage[4];
        bool first = true;
        for (u32 n = 0; n < draws; ++n) {
            const bool textured_draw = pick(3) == 0;
            BackendDrawCall call{};
            BackendNativeStates states{};
            call.native_states = &states;
            call.pipeline = textured_draw ? textured.get() : solid.get();
            // The caller's bytes change under the next draw: the record
            // must have copied them.
            std::memcpy(colour_storage, colours[pick(4)], 16);
            call.fragment_constants = colour_storage;
            call.fragment_constants_size = 16;
            call.vertex_buffers[0] = vertices[pick(2)].get();
            if (pick(5) == 0) {
                call.viewport = {0, 0, kSide / 2.0f, static_cast<float>(kSide),
                                 0, 1};
            }
            if (pick(4) == 0) {
                call.scissor_enabled = true;
                call.scissor = {static_cast<int>(pick(4)),
                                static_cast<int>(pick(4)), 4, 4};
            }
            if (pick(6) == 0) call.blend_color = {0.5f, 0.5f, 0.5f, 1.0f};
            if (pick(2) == 0) call.instance_count = 2;
            const u32 s = pick(2);
            if (textured_draw) {
                call.textures[0] = sampled.get();
                call.samplers[0] = samplers[s];
                if (pick(2) == 0) states.samplers[0] = native_samplers[s].get();
            }
            BackendDepthStencilDesc dss{};
            dss.depth_test_enabled = pick(2) == 0;
            dss.depth_write_enabled = pick(2) == 0;
            dss.depth_compare =
                pick(2) == 0 ? BackendCompare::LessEqual : BackendCompare::Always;
            apply_depth_stencil_desc(call, dss);
            if (pick(2) == 0) {
                auto state = device().create_depth_stencil_state(dss);
                states.depth_stencil = state.get();
                pass(call, first);
                native_dss.push_back(std::move(state));
            } else {
                const bool full = full_draws_between && pick(23) == 0;
                if (full) {
                    delta_record::force_for_test(delta_record::Part::DeltaRecord,
                                                 delta_record::Force::Off);
                }
                pass(call, first);
                if (full) {
                    delta_record::force_for_test(delta_record::Part::DeltaRecord,
                                                 forced_delta);
                }
            }
            first = false;
            std::memset(colour_storage, 0xCD, sizeof(colour_storage));
            if (pick(17) == 0) (void)MetalSubmitQueue::instance().submit();
        }
    }

    struct Run {
        std::vector<u64> hashes;
        std::vector<u32> image;
    };
    Run run_arm(delta_record::Force arm, u32 seed, u32 draws,
                bool full_draws_between,
                MetalSubmitQueue::DeltaMutation mutation =
                    MetalSubmitQueue::DeltaMutation::None) {
        Run run;
        clear();
        forced_delta = arm;
        delta_record::force_all_for_test(arm);
        MetalSubmitQueue::set_delta_mutation_for_test(mutation);
        MetalSubmitQueue::set_encode_hash_sink_for_test(&run.hashes);
        record_stream(seed, draws, full_draws_between);
        run.image = pixels();  // drains: the sink is complete
        MetalSubmitQueue::set_encode_hash_sink_for_test(nullptr);
        MetalSubmitQueue::set_delta_mutation_for_test(
            MetalSubmitQueue::DeltaMutation::None);
        delta_record::force_all_for_test(delta_record::Force::None);
        return run;
    }

    std::array<std::unique_ptr<BackendBuffer>, 2> vertices;
    std::unique_ptr<BackendTexture2D> target;
    std::unique_ptr<BackendTexture2D> depth;
    std::unique_ptr<BackendTexture2D> sampled;
    std::unique_ptr<BackendPipeline> solid;
    std::unique_ptr<BackendPipeline> textured;
    std::array<std::unique_ptr<BackendSamplerState>, 2> native_samplers;
    std::vector<std::unique_ptr<BackendDepthStencilState>> native_dss;
    delta_record::Force forced_delta{delta_record::Force::None};
};

TEST_F(DeltaRecordTest, DeltaDrawsEncodeExactlyTheFullRecordsState) {
    for (const u32 seed : {1u, 7u, 1234u}) {
        SCOPED_TRACE(seed);
        const Run full = run_arm(delta_record::Force::Off, seed, 240, true);
        const u64 before = MetalSubmitQueue::instance().delta_counters().draws;
        const Run delta = run_arm(delta_record::Force::On, seed, 240, true);
        const auto counters = MetalSubmitQueue::instance().delta_counters();
        ASSERT_EQ(full.hashes.size(), 240u);
        EXPECT_EQ(delta.hashes, full.hashes)
            << "a delta draw reached the encoder with other state";
        EXPECT_EQ(delta.image, full.image);
        EXPECT_GT(counters.draws - before, 150u) << "the delta arm engaged";
    }
}

// RED: without the keyframe at the start of a recording, the first draw of
// each recording after a submit is relative to a state its list never had.
TEST_F(DeltaRecordTest, TheGoldenCatchesAMissingRecordingKeyframe) {
    const Run full = run_arm(delta_record::Force::Off, 7, 240, false);
    const Run broken =
        run_arm(delta_record::Force::On, 7, 240, false,
                MetalSubmitQueue::DeltaMutation::NoRecordingKeyframe);
    EXPECT_NE(broken.hashes, full.hashes);
}

// RED: without the keyframe after a full draw joined the stream, the next
// delta applies to a state the encoder dropped.
TEST_F(DeltaRecordTest, TheGoldenCatchesAMissingChainKeyframe) {
    const Run full = run_arm(delta_record::Force::Off, 7, 240, true);
    const Run broken = run_arm(delta_record::Force::On, 7, 240, true,
                               MetalSubmitQueue::DeltaMutation::NoChainKeyframe);
    EXPECT_NE(broken.hashes, full.hashes);
}

// One recording of folded same-attachment draws with unchanged bindings:
// one keyframe, every later draw carrying only its colour, the handles
// referenced once, one DrawRun op, and no EndPass recorded between them.
TEST_F(DeltaRecordTest, ARunOfDrawsTakesItsReferencesOnceAndOneOp) {
    clear();
    delta_record::force_all_for_test(delta_record::Force::On);
    auto& q = MetalSubmitQueue::instance();
    (void)q.submit();
    const auto before = q.delta_counters();
    const u64 elided = q.ends_elided();
    static const float colours[2][4]{{1, 0, 0, 1}, {0, 0, 1, 1}};
    constexpr u32 kDraws = 64;
    for (u32 n = 0; n < kDraws; ++n) pass(solid_call(colours[n & 1]));
    const std::vector<u32> image = pixels();
    const auto after = q.delta_counters();
    EXPECT_EQ(after.draws - before.draws, kDraws);
    EXPECT_EQ(after.keyframes - before.keyframes, 1u);
    EXPECT_EQ(after.runs - before.runs, 1u) << "one DrawRun op for the run";
    EXPECT_LE(after.retains - before.retains, 4u)
        << "pipeline and vertex buffer, once each";
    EXPECT_LT(after.bytes - before.bytes, kDraws * 160u);
    EXPECT_EQ(q.ends_elided() - elided, kDraws - 1)
        << "pass-continue: every EndPass but the last never recorded";
    for (const u32 pixel : image) EXPECT_EQ(pixel, 0xFF0000FFu);  // blue last
}

// The same run with pass-continue off records (and the fold drops) every
// EndPass, and the pixels agree.
TEST_F(DeltaRecordTest, PassContinueOffRecordsEveryEndPass) {
    clear();
    delta_record::force_all_for_test(delta_record::Force::On);
    delta_record::force_for_test(delta_record::Part::PassContinue,
                                 delta_record::Force::Off);
    auto& q = MetalSubmitQueue::instance();
    const u64 elided = q.ends_elided();
    static const float red[4]{1, 0, 0, 1};
    for (u32 n = 0; n < 16; ++n) pass(solid_call(red));
    const std::vector<u32> image = pixels();
    EXPECT_EQ(q.ends_elided(), elided);
    for (const u32 pixel : image) EXPECT_EQ(pixel, 0xFFFF0000u);  // red
}

// A buffer the application frees after its draw was recorded stays alive
// until that draw is encoded: the delta took a reference on it.
TEST_F(DeltaRecordTest, AFreedVertexBufferOutlivesItsRecordedDraw) {
    clear();
    delta_record::force_all_for_test(delta_record::Force::On);
    static const float positions[]{-1, -1, 3, -1, -1, 3};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto doomed = device().create_buffer(vb, positions);
    static const float green[4]{0, 1, 0, 1};
    BackendDrawCall call = solid_call(green);
    call.vertex_buffers[0] = doomed.get();
    pass(call);
    doomed.reset();
    for (const u32 pixel : pixels()) EXPECT_EQ(pixel, 0xFF00FF00u);
}

// A buffer stays bound across a submit: its stamp must reach the second
// generation too, or NO_OVERWRITE / DISCARD reclaim would treat it as idle.
TEST_F(DeltaRecordTest, ABufferBoundAcrossASubmitIsStampedPerGeneration) {
    auto& q = MetalSubmitQueue::instance();
    MetalBuffer buffer(MetalDevice::instance().native_device(), 256,
                       BufferUsage::Storage);
    const u64 gen = q.recording_generation();
    buffer.note_gpu_bind_once(gen);
    EXPECT_EQ(buffer.storage_gen_for_test(), gen);
    buffer.note_gpu_bind_once(gen);  // the same generation: nothing new
    EXPECT_EQ(buffer.storage_gen_for_test(), gen);
    q.record_debug_marker(MetalSubmitQueue::DebugMarker::Signpost, "r3-stamp");
    (void)q.submit();
    const u64 next = q.recording_generation();
    ASSERT_GT(next, gen);
    buffer.note_gpu_bind_once(next);
    EXPECT_EQ(buffer.storage_gen_for_test(), next);
    q.wait_encoded();
}

// R3c in the backend: a draw naming native states draws what the same draw
// naming only the descs draws.
TEST_F(DeltaRecordTest, NativeStatesDrawWhatTheirDescsDraw) {
    BackendSamplerDesc linear{};
    linear.address_u = linear.address_v = BackendAddressMode::Clamp;
    auto sampler = device().create_sampler_state(linear);
    BackendDepthStencilDesc dss{};
    dss.depth_test_enabled = true;
    dss.depth_compare = BackendCompare::LessEqual;
    auto depth_state = device().create_depth_stencil_state(dss);
    ASSERT_TRUE(sampler && depth_state);
    EXPECT_TRUE(same_sampler_desc(sampler->desc(), linear));
    static const float white[4]{1, 1, 1, 1};
    std::vector<u32> image[2];
    for (int named = 0; named < 2; ++named) {
        clear();
        delta_record::force_all_for_test(delta_record::Force::On);
        BackendDrawCall call{};
        call.pipeline = textured.get();
        call.fragment_constants = white;
        call.fragment_constants_size = 16;
        call.textures[0] = sampled.get();
        call.samplers[0] = linear;
        apply_depth_stencil_desc(call, dss);
        BackendNativeStates states{};
        if (named != 0) {
            states.samplers[0] = sampler.get();
            states.depth_stencil = depth_state.get();
            call.native_states = &states;
        }
        pass(call, true);
        image[named] = pixels();
    }
    EXPECT_EQ(image[1], image[0]);
}

}  // namespace
