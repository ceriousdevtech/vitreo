// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_occlusion_gpu.cpp
/// @brief GpuOcclusionCounter on real Metal counts the samples WARP counts.
///
/// Expected values are lines of
/// tools/oracle_probe/d3d11_a10_queries_probe.expected [occlusion] (Windows,
/// WARP, 64x64 target): a full-target quad passes 4096 samples, 16384 at 4x
/// MSAA, and 4096 with no render target and no depth target at all; two
/// draws in one bracket sum ("full + half in one bracket" = 6144) even when
/// they land in different command buffers; a counter no pass used reads 0.
///
/// The depth, stencil and sample-mask cases below are lines of the same
/// [occlusion] block, and follow from D3D11.3 20.4.6 (D3D11_QUERY_OCCLUSION:
/// the samples that pass the depth and stencil tests; a disabled test counts
/// as passed) and Learn D3D11_QUERY ("If a depth or stencil test is disabled,
/// then each of those tests will be counted as a pass").

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/block_recycler.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr unsigned kSize = 64;

std::unique_ptr<BackendPipeline> pipeline(GraphicsBackendDevice& device,
                                          unsigned samples,
                                          bool attachmentless) {
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = R"(
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
    desc.fragment_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 main0() { return float4(0.0, 1.0, 0.0, 1.0); }
    )";
    desc.vertex_entry_point = "main0";
    desc.fragment_entry_point = "main0";
    desc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
    desc.vertex_stride = desc.vertex_buffer_strides[0] = 8;
    desc.color_format =
        attachmentless ? PixelFormat::Unknown : PixelFormat::RGBA8Unorm;
    desc.attachmentless = attachmentless;
    desc.sample_count = samples;
    desc.cache_key = 0x4f43434c00ull + samples * 2 + (attachmentless ? 1 : 0);
    return device.create_pipeline(desc);
}

struct Scene {
    std::unique_ptr<BackendPipeline> pso;
    std::unique_ptr<BackendTexture2D> target;
    std::unique_ptr<BackendBuffer> vertices;
};

/// Draws the quad (x0..x1 in clip space, full height) into a new pass that
/// counts into @p counter.
void draw(GraphicsBackendDevice& device, Scene& scene,
          GpuOcclusionCounter* counter, float x1, unsigned samples = 1) {
    const std::array<float, 12> quad{-1, -1, x1, -1, -1, 1,
                                     -1, 1,  x1, -1, x1, 1};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(quad);
    vb.vertex_buffer = true;
    scene.vertices = device.create_buffer(vb, quad.data());
    ASSERT_NE(scene.vertices, nullptr);
    BackendRenderPassDesc pass_desc{};
    if (scene.target) {
        pass_desc.color_targets[0] = scene.target.get();
        pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    } else {
        pass_desc.attachmentless_width = kSize;
        pass_desc.attachmentless_height = kSize;
        pass_desc.attachmentless_samples = samples;
    }
    pass_desc.preserve_color_contents = true;
    pass_desc.occlusion_counter = counter;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    ASSERT_TRUE(pass->valid());
    BackendDrawCall call{};
    call.pipeline = scene.pso.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 6;
    call.instance_count = 1;
    call.vertex_buffer = scene.vertices.get();
    call.vertex_buffers[0] = scene.vertices.get();
    call.viewport = {0, 0, static_cast<float>(kSize),
                     static_cast<float>(kSize), 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());
}

Scene scene(GraphicsBackendDevice& device, unsigned samples,
            bool attachmentless) {
    Scene s{};
    s.pso = pipeline(device, samples, attachmentless);
    if (!attachmentless) {
        BackendTexture2DDesc td{};
        td.width = td.height = kSize;
        td.mip_levels = td.array_size = 1;
        td.sample_count = samples;
        td.pixel_format = PixelFormat::RGBA8Unorm;
        td.render_target = true;
        s.target = device.create_texture_2d(td, nullptr, 0);
    }
    return s;
}

// -- Depth, stencil and sample-mask scenes ----------------------------------

/// What a counted pass's pipeline runs after the vertex stage.
enum class Fragment : u8 {
    Green,  ///< a pixel shader writing one colour
    None,   ///< no pixel shader (PSSetShader(NULL))
    /// Writes [[sample_mask]] from fragment constant 0: how the D3D11
    /// frontend applies an OMSetBlendState SampleMask that clears one of the
    /// target's samples (Metal has no pipeline sample mask; see
    /// d3d11_pipeline.cpp, D3D11.3 17.17).
    Mask,
};

struct Spec {
    unsigned samples{1};
    bool colour{true};
    PixelFormat depth{PixelFormat::Unknown};
    Fragment fragment{Fragment::Green};
};

/// A pipeline drawing float3 (clip x, y, z) vertices.
std::unique_ptr<BackendPipeline> pipeline(GraphicsBackendDevice& device,
                                          const Spec& spec) {
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float3 position [[attribute(0)]]; };
        struct main0_out { float4 position [[position]]; };
        vertex main0_out main0(main0_in in [[stage_in]]) {
            main0_out out = {};
            out.position = float4(in.position, 1.0);
            return out;
        }
    )";
    switch (spec.fragment) {
    case Fragment::Green:
        desc.fragment_shader_msl = R"(
            #include <metal_stdlib>
            using namespace metal;
            fragment float4 main0() { return float4(0.0, 1.0, 0.0, 1.0); }
        )";
        break;
    case Fragment::None:
        break;
    case Fragment::Mask:
        desc.fragment_shader_msl = R"(
            #include <metal_stdlib>
            using namespace metal;
            struct main0_out {
                float4 colour [[color(0)]];
                uint mask [[sample_mask]];
            };
            fragment main0_out main0(constant uint& mask [[buffer(0)]]) {
                main0_out out = {};
                out.colour = float4(0.0, 1.0, 0.0, 1.0);
                out.mask = mask;
                return out;
            }
        )";
        break;
    }
    desc.vertex_entry_point = "main0";
    desc.fragment_entry_point = "main0";
    desc.vertex_attributes = {{0, 0, BackendVertexFormat::Float3, 0}};
    desc.vertex_stride = desc.vertex_buffer_strides[0] = 12;
    desc.color_format =
        spec.colour ? PixelFormat::RGBA8Unorm : PixelFormat::Unknown;
    desc.depth_attachment = spec.depth != PixelFormat::Unknown;
    desc.depth_format = spec.depth;
    desc.sample_count = spec.samples;
    desc.cache_key = 0x0CC1D5ull << 40 | u64{spec.samples} << 24 |
                     static_cast<u64>(spec.depth) << 8 |
                     static_cast<u64>(spec.fragment) << 1 |
                     (spec.colour ? 1u : 0u);
    return device.create_pipeline(desc);
}

/// A kSize x kSize colour (optional) and depth (optional) target.
struct Target {
    std::unique_ptr<BackendTexture2D> colour;
    std::unique_ptr<BackendTexture2D> depth;
    /// Every vertex buffer a pass drew, kept until the test ends.
    std::vector<std::unique_ptr<BackendBuffer>> vertices;
};

Target target(GraphicsBackendDevice& device, unsigned samples, bool colour,
              PixelFormat depth) {
    Target t{};
    BackendTexture2DDesc td{};
    td.width = td.height = kSize;
    td.mip_levels = td.array_size = 1;
    td.sample_count = samples;
    if (colour) {
        td.pixel_format = PixelFormat::RGBA8Unorm;
        td.render_target = true;
        t.colour = device.create_texture_2d(td, nullptr, 0);
    }
    if (depth != PixelFormat::Unknown) {
        td.pixel_format = depth;
        td.render_target = false;
        td.depth_stencil = true;
        t.depth = device.create_texture_2d(td, nullptr, 0);
    }
    return t;
}

/// The depth-stencil state of a draw, as the D3D11 frontend fills it in.
BackendDrawCall depth_state(bool test, BackendCompare compare,
                            bool write = false) {
    BackendDrawCall call{};
    call.depth_test_enabled = test;
    call.depth_compare = compare;
    call.depth_write_enabled = write;
    return call;
}

/// One pass on @p t counting into @p counter (may be null). With
/// @p clear_value set, the pass first clears depth to it (and stencil to
/// 0). With @p pso set it draws a quad over clip x0..x1 (full height) at
/// depth @p z, with @p call's depth, stencil, scissor and constant state.
void quad_pass(GraphicsBackendDevice& device, Target& t,
               GpuOcclusionCounter* counter, std::optional<float> clear_value,
               BackendPipeline* pso = nullptr, float x0 = -1.0f,
               float x1 = 1.0f, float z = 0.5f, BackendDrawCall call = {}) {
    BackendRenderPassDesc pass_desc{};
    if (t.colour) {
        pass_desc.color_targets[0] = t.colour.get();
        pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    }
    pass_desc.depth_stencil_target = t.depth.get();
    pass_desc.preserve_color_contents = true;
    pass_desc.clear_depth = clear_value.has_value();
    pass_desc.depth_clear_value = clear_value.value_or(1.0f);
    pass_desc.occlusion_counter = counter;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    ASSERT_TRUE(pass->valid());
    if (pso != nullptr) {
        const std::array<float, 18> quad{x0, -1, z, x1, -1, z, x0, 1, z,
                                         x0, 1,  z, x1, -1, z, x1, 1, z};
        BackendBufferDesc vb{};
        vb.byte_width = sizeof(quad);
        vb.vertex_buffer = true;
        t.vertices.push_back(device.create_buffer(vb, quad.data()));
        ASSERT_NE(t.vertices.back(), nullptr);
        call.pipeline = pso;
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = 6;
        call.instance_count = 1;
        call.vertex_buffer = t.vertices.back().get();
        call.vertex_buffers[0] = t.vertices.back().get();
        call.viewport = {0, 0, static_cast<float>(kSize),
                         static_cast<float>(kSize), 0, 1};
        ASSERT_TRUE(pass->draw(call));
    }
    ASSERT_TRUE(pass->end());
}

/// The samples one counted draw passes, after the GPU has run it.
u64 count_one(GraphicsBackendDevice& device, Target& t, BackendPipeline* pso,
              std::optional<float> clear_value, float x0, float x1, float z,
              const BackendDrawCall& call) {
    if (clear_value) quad_pass(device, t, nullptr, clear_value);
    auto counter = device.create_occlusion_counter();
    EXPECT_NE(counter, nullptr);
    if (!counter) return ~0ull;
    quad_pass(device, t, counter.get(), std::nullopt, pso, x0, x1, z, call);
    EXPECT_TRUE(device.wait_idle());
    EXPECT_TRUE(device.generation_complete(counter->recorded_generation()));
    return counter->samples();
}

/// MetalDevice::set_occlusion_pass_hook_for_test: at the chosen point of
/// the next counted pass, a second thread records work of its own and
/// submits the recording -- then, optionally, records a GPU wait on @p hold
/// into the next one. The recording thread waits for it (join), so the
/// interleaving is the same on every run.
struct OtherThreadSubmit {
    GraphicsBackendDevice* device{nullptr};
    BackendBuffer* source{nullptr};
    BackendBuffer* destination{nullptr};
    BackendFence* hold{nullptr};
    u64 hold_value{0};
    bool after{false};
    bool fired{false};

    static void hook(bool after, void* context) {
        auto& self = *static_cast<OtherThreadSubmit*>(context);
        if (after != self.after || self.fired) return;
        self.fired = true;
        std::thread other([&self] {
            EXPECT_TRUE(self.device->copy_buffer_region(
                self.destination, 0, self.source, 0, 16));
            self.device->flush();
            if (self.hold != nullptr) {
                EXPECT_TRUE(self.hold->wait_gpu(self.hold_value));
            }
        });
        other.join();
    }
};

}  // namespace

TEST(OcclusionGpu, CountsSamplesThatPassAcrossPassesAndCommandBuffers) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    Scene s = scene(device, 1, false);
    ASSERT_TRUE(s.pso && s.pso->valid() && s.target);

    auto full = device.create_occlusion_counter();
    auto sum = device.create_occlusion_counter();
    auto unused = device.create_occlusion_counter();
    ASSERT_TRUE(full && sum && unused);
    draw(device, s, full.get(), 1.0f);     // "full z=.5 ... depth 1.0" 4096
    draw(device, s, sum.get(), 1.0f);      // "full + half in one bracket"
    device.flush();                        // the half lands in a later buffer
    draw(device, s, sum.get(), 0.0f);
    draw(device, s, nullptr, 1.0f);        // counts into nothing
    ASSERT_TRUE(device.wait_idle());
    EXPECT_EQ(full->samples(), 4096u);
    EXPECT_EQ(sum->samples(), 6144u);
    EXPECT_EQ(unused->samples(), 0u) << "\"empty bracket\" = 0";
}

TEST(OcclusionGpu, CountsSamplesNotPixelsAndWithoutAttachments) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_texture_sample_count(4)) {
        GTEST_SKIP() << "no 4x MSAA";
    }
    Scene msaa = scene(device, 4, false);
    ASSERT_TRUE(msaa.pso && msaa.pso->valid() && msaa.target);
    auto full = device.create_occlusion_counter();
    auto half = device.create_occlusion_counter();
    draw(device, msaa, full.get(), 1.0f, 4);
    draw(device, msaa, half.get(), 0.0f, 4);

    // "full, no RTV and no DSV": the draw still rasterizes and counts.
    Scene bare = scene(device, 1, true);
    ASSERT_TRUE(bare.pso && bare.pso->valid());
    auto none = device.create_occlusion_counter();
    draw(device, bare, none.get(), 1.0f, 1);
    ASSERT_TRUE(device.wait_idle());
    EXPECT_EQ(full->samples(), 16384u) << "MSAA x4: full quad";
    EXPECT_EQ(half->samples(), 8192u) << "MSAA x4: left half";
    EXPECT_EQ(none->samples(), 4096u) << "full, no RTV and no DSV";
}

// An occlusion counter lives for one Begin/End bracket: the renderer makes
// hundreds a frame, each released at the query's next Begin. A released
// counter's storage builds the next one (block_recycler.hpp) instead of a
// malloc and free per bracket.
TEST(OcclusionCounterStorage, AReleasedCounterIsRebuiltInItsOwnStorage) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    std::shared_ptr<GpuOcclusionCounter> first =
        device.create_occlusion_counter();
    ASSERT_TRUE(first);
    const void* const storage = first.get();
    first.reset();
    const BlockRecycler::Stats before = BlockRecycler::stats();
    std::shared_ptr<GpuOcclusionCounter> second =
        device.create_occlusion_counter();
    ASSERT_TRUE(second);
    EXPECT_EQ(second.get(), storage);
    EXPECT_GT(BlockRecycler::stats().reused, before.reused);
    EXPECT_EQ(second->samples(), 0u) << "a reused counter starts empty";
    EXPECT_EQ(second->recorded_generation(), 0u)
        << "a reused counter has counted no pass";
}

// [occlusion] "full z=.7 LESS over depth .5 (all fail)" = 0, "full z=.3 LESS
// over depth .5 (all pass)" = 4096, "full z=.5 LESS, depth 1.0" = 4096,
// "full z=.6 over half .5 / half 1.0" = 2048 and "full, NULL PS, depth LESS"
// = 4096: D3D11.3 20.4.6 counts the samples that pass the depth test, and a
// draw with no pixel shader still runs it (15.2: rasterization is disabled
// only when the depth and stencil tests are off as well).
TEST(OcclusionGpu, CountsOnlySamplesThatPassTheDepthTest) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    Target t = target(device, 1, true, PixelFormat::D32Float);
    ASSERT_TRUE(t.colour && t.depth);
    auto green = pipeline(device, Spec{1, true, PixelFormat::D32Float,
                                       Fragment::Green});
    auto null_ps = pipeline(device, Spec{1, true, PixelFormat::D32Float,
                                         Fragment::None});
    ASSERT_TRUE(green && green->valid() && null_ps && null_ps->valid());
    const BackendDrawCall less = depth_state(true, BackendCompare::Less);

    EXPECT_EQ(count_one(device, t, green.get(), 0.5f, -1, 1, 0.7f, less), 0u)
        << "full z=.7 LESS over depth .5 (all fail)";
    EXPECT_EQ(count_one(device, t, green.get(), 0.5f, -1, 1, 0.3f, less),
              4096u)
        << "full z=.3 LESS over depth .5 (all pass)";
    EXPECT_EQ(count_one(device, t, green.get(), 1.0f, -1, 1, 0.5f, less),
              4096u)
        << "full z=.5 LESS, depth 1.0";

    // Depth 1.0, then .5 written over the left half by an uncounted draw.
    quad_pass(device, t, nullptr, 1.0f);
    quad_pass(device, t, nullptr, std::nullopt, green.get(), -1, 0, 0.5f,
              depth_state(true, BackendCompare::Always, true));
    EXPECT_EQ(count_one(device, t, green.get(), std::nullopt, -1, 1, 0.6f,
                        less),
              2048u)
        << "full z=.6 over half .5 / half 1.0";

    EXPECT_EQ(count_one(device, t, null_ps.get(), 1.0f, -1, 1, 0.5f, less),
              4096u)
        << "full, NULL PS, depth LESS";
}

// [occlusion] "full, stencil func NEVER" = 0: 20.4.6 counts samples that
// pass the depth AND stencil tests. The ALWAYS line shows the same pass
// counts once the stencil test lets the samples through.
TEST(OcclusionGpu, CountsOnlySamplesThatPassTheStencilTest) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    Target t = target(device, 1, true, PixelFormat::D32Float_S8Uint);
    ASSERT_TRUE(t.colour && t.depth);
    auto green = pipeline(device, Spec{1, true, PixelFormat::D32Float_S8Uint,
                                       Fragment::Green});
    ASSERT_TRUE(green && green->valid());
    const auto stencil = [](BackendCompare compare) {
        BackendDrawCall call = depth_state(true, BackendCompare::Less);
        call.stencil_test_enabled = true;
        call.front_stencil.compare = compare;
        call.back_stencil.compare = compare;
        return call;
    };
    EXPECT_EQ(count_one(device, t, green.get(), 1.0f, -1, 1, 0.5f,
                        stencil(BackendCompare::Never)),
              0u)
        << "full, stencil func NEVER";
    EXPECT_EQ(count_one(device, t, green.get(), 1.0f, -1, 1, 0.5f,
                        stencil(BackendCompare::Always)),
              4096u)
        << "full, stencil func ALWAYS";
}

// [occlusion] "MSAA x4: full, SampleMask 0x1" = 4096 and "0x0" = 0, beside
// "MSAA x4: full quad" = 16384. D3D11.3 20.4.6 counts SAMPLES, so a full
// quad on a 4x target is 4 x 4096. Where the SampleMask (like alpha-to-
// coverage or oMask) turns a covered sample's output off, 20.4.6 lets the
// count include it or not and recommends that implementations "count 0 in
// this case for consistency with known implementations" -- what WARP does:
// mask 0x1 leaves one sample of each pixel, 4096.
TEST(OcclusionGpu, CountsTheSamplesTheSampleMaskLeaves) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_texture_sample_count(4)) {
        GTEST_SKIP() << "no 4x MSAA";
    }
    Target t = target(device, 4, true, PixelFormat::Unknown);
    ASSERT_TRUE(t.colour);
    auto masked = pipeline(device, Spec{4, true, PixelFormat::Unknown,
                                        Fragment::Mask});
    auto plain = pipeline(device, Spec{4, true, PixelFormat::Unknown,
                                       Fragment::Green});
    ASSERT_TRUE(masked && masked->valid() && plain && plain->valid());
    const auto with_mask = [](const u32& mask) {
        BackendDrawCall call{};
        call.fragment_constants = &mask;
        call.fragment_constants_size = sizeof(mask);
        return call;
    };
    const u32 one = 0x1u;
    const u32 none = 0x0u;
    const u32 all = 0xFu;
    EXPECT_EQ(count_one(device, t, masked.get(), std::nullopt, -1, 1, 0.5f,
                        with_mask(one)),
              4096u)
        << "MSAA x4: full, SampleMask 0x1";
    EXPECT_EQ(count_one(device, t, masked.get(), std::nullopt, -1, 1, 0.5f,
                        with_mask(none)),
              0u)
        << "MSAA x4: full, SampleMask 0x0";
    EXPECT_EQ(count_one(device, t, masked.get(), std::nullopt, -1, 1, 0.5f,
                        with_mask(all)),
              16384u)
        << "MSAA x4: full, SampleMask 0xF written by the shader";
    // A SampleMask covering every sample takes no mask-writing variant.
    EXPECT_EQ(count_one(device, t, plain.get(), std::nullopt, -1, 1, 0.5f,
                        BackendDrawCall{}),
              16384u)
        << "MSAA x4: full quad (SampleMask 0xFFFFFFFF)";
}

// The count is final once the generation the counted pass was RECORDED in
// has completed (GetData's S_OK: "the data at pData is available"), and the
// counter's slot is recycled only then. Another thread submitting just
// before the pass's BeginPass is recorded puts the pass in the next
// generation; a generation taken before the record named the earlier one.
TEST(OcclusionGpu, CounterTakesTheGenerationItsPassWasRecordedIn) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    Scene s = scene(device, 1, false);
    ASSERT_TRUE(s.pso && s.pso->valid() && s.target);
    BackendBufferDesc bd{};
    bd.byte_width = 16;
    bd.shader_resource = true;
    const std::array<u32, 4> words{1, 2, 3, 4};
    auto source = device.create_buffer(bd, words.data());
    auto destination = device.create_buffer(bd, nullptr);
    auto hold = device.create_fence(0);
    ASSERT_TRUE(source && destination && hold && hold->valid());
    ASSERT_TRUE(device.wait_idle());

    // 1. Submitted just BEFORE the BeginPass: the pass is in the next
    //    generation, which the other thread also holds on the fence.
    OtherThreadSubmit before{&device, source.get(), destination.get(),
                             hold.get(), 1, false};
    const u64 open = device.recording_generation();
    MetalDevice::set_occlusion_pass_hook_for_test(&OtherThreadSubmit::hook,
                                                  &before);
    auto raced = device.create_occlusion_counter();
    ASSERT_TRUE(raced);
    draw(device, s, raced.get(), 1.0f);
    MetalDevice::set_occlusion_pass_hook_for_test(nullptr, nullptr);
    ASSERT_TRUE(before.fired);
    EXPECT_EQ(raced->recorded_generation(), open + 1)
        << "the pass went into the recording after the one the other "
           "thread submitted";
    device.flush();
    EXPECT_TRUE(device.wait_generation(open)) << "the other thread's list";
    EXPECT_FALSE(device.generation_complete(raced->recorded_generation()))
        << "S_FALSE: the GPU has not run the counted pass";
    ASSERT_TRUE(hold->signal_cpu(1));
    ASSERT_TRUE(device.wait_generation(raced->recorded_generation()));
    EXPECT_EQ(raced->samples(), 4096u);

    // 2. The same race, then the counter is released while its pass is held
    //    on the GPU. Its slot must not serve a new counter before that pass
    //    has run: the new counter would read the old pass's samples.
    OtherThreadSubmit again{&device, source.get(), destination.get(),
                            hold.get(), 2, false};
    const u64 open2 = device.recording_generation();
    MetalDevice::set_occlusion_pass_hook_for_test(&OtherThreadSubmit::hook,
                                                  &again);
    auto released = device.create_occlusion_counter();
    ASSERT_TRUE(released);
    draw(device, s, released.get(), 1.0f);
    MetalDevice::set_occlusion_pass_hook_for_test(nullptr, nullptr);
    ASSERT_TRUE(again.fired);
    device.flush();
    EXPECT_TRUE(device.wait_generation(open2));
    released.reset();
    auto probe = device.create_occlusion_counter();
    ASSERT_TRUE(probe);
    draw(device, s, probe.get(), -1.0f);  // zero width: counts nothing
    device.flush();
    ASSERT_TRUE(hold->signal_cpu(2));
    ASSERT_TRUE(device.wait_idle());
    EXPECT_EQ(probe->samples(), 0u)
        << "the probe was given the released counter's slot while the "
           "released counter's pass was still to run";

    // 3. Submitted just AFTER the BeginPass, before the draw: the BeginPass
    //    goes to the GPU in the other thread's list and the draw in the next
    //    one. The counter takes the draw's generation -- later than the
    //    pass's, never earlier -- which is submitted (the draw is in it), so
    //    polling it completes. And the draw is counted and rendered: the
    //    encoder reopens the pass for it (D3D11.3: every command issued on
    //    the immediate context executes, in order).
    ASSERT_TRUE(s.target->clear_render_target({0.0f, 0.0f, 0.0f, 0.0f}));
    OtherThreadSubmit after{&device, source.get(), destination.get(), nullptr,
                            0, true};
    auto& queue = MetalSubmitQueue::instance();
    const u64 reopened = queue.draws_reopened();
    const u64 dropped = queue.draws_dropped();
    const u64 open3 = device.recording_generation();
    MetalDevice::set_occlusion_pass_hook_for_test(&OtherThreadSubmit::hook,
                                                  &after);
    auto late = device.create_occlusion_counter();
    ASSERT_TRUE(late);
    draw(device, s, late.get(), 1.0f);
    MetalDevice::set_occlusion_pass_hook_for_test(nullptr, nullptr);
    ASSERT_TRUE(after.fired);
    EXPECT_EQ(late->recorded_generation(), open3 + 1);
    device.flush();
    EXPECT_TRUE(device.wait_generation(late->recorded_generation()));
    EXPECT_TRUE(device.generation_complete(late->recorded_generation()));
    EXPECT_EQ(late->samples(), 4096u)
        << "the draw recorded after the other thread's submit is counted";
    EXPECT_EQ(queue.draws_reopened() - reopened, 1u);
    EXPECT_EQ(queue.draws_dropped() - dropped, 0u);
    std::vector<u32> pixels(kSize * kSize, 0u);
    ASSERT_TRUE(s.target->readback(0, pixels.data(), kSize * 4));
    usize green = 0;
    for (const u32 pixel : pixels) green += pixel == 0xFF00FF00u ? 1u : 0u;
    EXPECT_EQ(green, usize{kSize} * kSize)
        << "the draw is rendered: every pixel the full quad covers is green";
}

namespace {

/// A full-height quad over clip x0..x1 drawn into @p pass with scene @p s's
/// pipeline; its vertex buffer is kept in @p keep.
void draw_into(GraphicsBackendDevice& device, Scene& s, BackendRenderPass& pass,
               float x0, float x1,
               std::vector<std::unique_ptr<BackendBuffer>>& keep) {
    const std::array<float, 12> quad{x0, -1, x1, -1, x0, 1,
                                     x0, 1,  x1, -1, x1, 1};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(quad);
    vb.vertex_buffer = true;
    keep.push_back(device.create_buffer(vb, quad.data()));
    ASSERT_NE(keep.back(), nullptr);
    BackendDrawCall call{};
    call.pipeline = s.pso.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 6;
    call.instance_count = 1;
    call.vertex_buffer = keep.back().get();
    call.vertex_buffers[0] = keep.back().get();
    call.viewport = {0, 0, static_cast<float>(kSize),
                     static_cast<float>(kSize), 0, 1};
    ASSERT_TRUE(pass.draw(call));
}

/// Another thread records a copy and submits, then exits.
void submit_from_another_thread(GraphicsBackendDevice& device,
                                BackendBuffer* destination,
                                BackendBuffer* source) {
    std::thread other([&] {
        EXPECT_TRUE(device.copy_buffer_region(destination, 0, source, 0, 16));
        device.flush();
    });
    other.join();
}

}  // namespace

// D3D11.3: every command issued on the immediate context executes, in order.
// Another thread's submit between a render pass's BeginPass and its draws --
// and again between two of its draws -- puts the BeginPass and each draw in
// a different list. The encoder reopens the pass for each draw, loading
// what the pass stored so far (never re-running its clear), counting into
// the pass's occlusion slot: the left half (2048 samples) and the next
// quarter (1024) are drawn and counted, and the last quarter keeps the
// clear the pass's BeginPass loaded. With Reset visibility (the macOS 13
// path, forced) a second encoder replaces the value at its offset, so each
// reopened encoder counts into a carry slot added in when its list
// completes: the count is the same 3072.
TEST(OcclusionGpu, DrawsSplitFromTheirPassByAnotherThreadsSubmitsAreRenderedAndCounted) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    auto& queue = MetalSubmitQueue::instance();
    BackendBufferDesc bd{};
    bd.byte_width = 16;
    bd.shader_resource = true;
    const std::array<u32, 4> words{1, 2, 3, 4};
    auto source = device.create_buffer(bd, words.data());
    auto destination = device.create_buffer(bd, nullptr);
    ASSERT_TRUE(source && destination);
    struct ResetOff {
        ~ResetOff() { MetalDevice::set_occlusion_reset_visibility_for_test(false); }
    } reset_off;
    for (const bool reset : {false, true}) {
        SCOPED_TRACE(reset ? "Reset visibility" : "Accumulate visibility");
        Scene s = scene(device, 1, false);
        ASSERT_TRUE(s.pso && s.pso->valid() && s.target);
        ASSERT_TRUE(device.wait_idle());
        MetalDevice::set_occlusion_reset_visibility_for_test(reset);
        // Recorded as a clear-only pass, folded into the BeginPass below as
        // its colour load clear (fold_trailing_clears_locked).
        ASSERT_TRUE(s.target->clear_render_target({0.0f, 0.0f, 1.0f, 1.0f}));
        auto counter = device.create_occlusion_counter();
        ASSERT_TRUE(counter);
        const u64 reopened = queue.draws_reopened();
        const u64 dropped = queue.draws_dropped();
        const u64 open = device.recording_generation();
        std::vector<std::unique_ptr<BackendBuffer>> keep;
        {
            BackendRenderPassDesc pass_desc{};
            pass_desc.color_targets[0] = s.target.get();
            pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
            pass_desc.preserve_color_contents = true;
            pass_desc.occlusion_counter = counter.get();
            auto pass = device.create_render_pass_standalone(pass_desc);
            ASSERT_TRUE(pass && pass->valid());
            submit_from_another_thread(device, destination.get(), source.get());
            draw_into(device, s, *pass, -1.0f, 0.0f, keep);  // left half
            submit_from_another_thread(device, destination.get(), source.get());
            draw_into(device, s, *pass, 0.0f, 0.5f, keep);   // third quarter
            ASSERT_TRUE(pass->end());
        }
        MetalDevice::set_occlusion_reset_visibility_for_test(false);
        EXPECT_EQ(counter->recorded_generation(), open + 2)
            << "the last draw's generation, two submits after the BeginPass";
        device.flush();
        ASSERT_TRUE(device.wait_generation(counter->recorded_generation()));
        EXPECT_EQ(counter->samples(), 3072u) << "2048 + 1024 samples";
        EXPECT_EQ(queue.draws_reopened() - reopened, 2u);
        EXPECT_EQ(queue.draws_dropped() - dropped, 0u);
        std::vector<u32> pixels(kSize * kSize, 0u);
        ASSERT_TRUE(s.target->readback(0, pixels.data(), kSize * 4));
        usize wrong = 0;
        for (unsigned y = 0; y < kSize; ++y) {
            for (unsigned x = 0; x < kSize; ++x) {
                // RGBA8 little-endian: green 0xFF00FF00, blue 0xFFFF0000.
                const u32 want = x < kSize * 3 / 4 ? 0xFF00FF00u : 0xFFFF0000u;
                wrong += pixels[y * kSize + x] != want ? 1u : 0u;
            }
        }
        EXPECT_EQ(wrong, 0u) << "first pixel 0x" << std::hex << pixels[0]
                             << ", last 0x" << pixels.back();
    }
}

// 256 OCCLUSION brackets interleaved inside ONE depth-only encoder: every
// counted pass after the first continues the encoder of the pass before it
// (RenderPassMergeTest's mechanism -- same attachments, nothing cleared,
// nothing written to memory -- with every slot in one visibility buffer),
// each counter spanning two passes with the other 255 counters' passes in
// between. Three frames, each held on the GPU by a fence until the test
// releases it: until then no counter's generation has completed (GetData's
// S_FALSE) and nothing was written; afterwards every count is exact. A
// frame's counters are released once it completed, so the next frame's
// counters reuse their slots while the other frame is still in flight.
TEST(OcclusionGpu, InterleavedQueriesInOneMergedDepthOnlyEncoderAcrossFrames) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";
    Target t = target(device, 1, false, PixelFormat::D32Float);
    ASSERT_TRUE(t.depth);
    auto depth_only = pipeline(device, Spec{1, false, PixelFormat::D32Float,
                                            Fragment::None});
    ASSERT_TRUE(depth_only && depth_only->valid());
    auto hold = device.create_fence(0);
    ASSERT_TRUE(hold && hold->valid());
    MetalSubmitQueue::set_pass_merge_for_test(true);
    ASSERT_TRUE(device.wait_idle());
    auto& queue = MetalSubmitQueue::instance();
    const u64 continued = queue.passes_continued();
    const u64 restarted = queue.passes_restarted();

    constexpr u32 kQueries = 256;
    constexpr u32 kFrames = 3;
    constexpr int n = static_cast<int>(kSize);
    // Scissor rectangles (left, top, right, bottom) varying with the query
    // and the frame, so a slot holding an older count shows. The quad at
    // z=.5 passes LESS against depth 1.0 at every covered sample.
    const auto first = [](u32 k, u32 f) -> std::array<int, 4> {
        const int w = 1 + static_cast<int>((k + 13 * f) % kSize);
        const int h = 1 + static_cast<int>((k / kSize + f) % 4);
        return {0, 0, w, h};
    };
    const auto second = [](u32 k, u32 f) -> std::array<int, 4> {
        const int w = 1 + static_cast<int>((7 * k + f) % kSize);
        const int h = 1 + static_cast<int>(k % 3);
        return {n - w, n - h, n, n};
    };
    const auto area = [](const std::array<int, 4>& r) {
        return static_cast<u64>((r[2] - r[0]) * (r[3] - r[1]));
    };
    struct Frame {
        std::vector<std::shared_ptr<GpuOcclusionCounter>> counters;
        std::vector<u64> expected;
    };
    const auto counted = [&](GpuOcclusionCounter* counter,
                             const std::array<int, 4>& rect) {
        BackendDrawCall call = depth_state(true, BackendCompare::Less);
        call.scissor_enabled = true;
        call.scissor = rect;
        quad_pass(device, t, counter, std::nullopt, depth_only.get(), -1, 1,
                  0.5f, call);
    };
    const auto record = [&](u32 f) {
        Frame frame;
        EXPECT_TRUE(hold->wait_gpu(f + 1));  // held until finish(f)
        quad_pass(device, t, nullptr, 1.0f);  // depth 1.0
        for (u32 k = 0; k < kQueries; ++k) {
            frame.counters.push_back(device.create_occlusion_counter());
            EXPECT_TRUE(frame.counters.back());
            frame.expected.push_back(area(first(k, f)) + area(second(k, f)));
        }
        for (u32 k = 0; k < kQueries; ++k) {
            counted(frame.counters[k].get(), first(k, f));
        }
        for (u32 k = kQueries; k-- > 0;) {
            counted(frame.counters[k].get(), second(k, f));
        }
        device.flush();
        return frame;
    };
    const auto pending = [&](const Frame& frame, u32 f) {
        for (u32 k = 0; k < kQueries; ++k) {
            const auto& counter = frame.counters[k];
            ASSERT_NE(counter->recorded_generation(), 0u);
            EXPECT_FALSE(
                device.generation_complete(counter->recorded_generation()))
                << "frame " << f << " query " << k << ": S_FALSE";
            EXPECT_EQ(counter->samples(), 0u)
                << "frame " << f << " query " << k << ": nothing ran yet";
        }
    };
    const auto finish = [&](Frame& frame, u32 f) {
        ASSERT_TRUE(hold->signal_cpu(f + 1));
        for (const auto& counter : frame.counters) {
            ASSERT_TRUE(
                device.wait_generation(counter->recorded_generation()));
        }
        for (u32 k = 0; k < kQueries; ++k) {
            const auto& counter = frame.counters[k];
            EXPECT_TRUE(
                device.generation_complete(counter->recorded_generation()));
            EXPECT_EQ(counter->samples(), frame.expected[k])
                << "frame " << f << " query " << k;
        }
        frame.counters.clear();  // the slots retire, then serve later frames
    };

    Frame f0 = record(0);
    pending(f0, 0);
    Frame f1 = record(1);
    pending(f1, 1);
    finish(f0, 0);
    pending(f1, 1);
    Frame f2 = record(2);  // reuses frame 0's slots, frame 1 in flight
    pending(f2, 2);
    finish(f1, 1);
    pending(f2, 2);
    finish(f2, 2);
    ASSERT_TRUE(device.wait_idle());

    EXPECT_EQ(queue.passes_continued() - continued,
              u64{kFrames} * (2 * kQueries - 1))
        << "every counted pass after a frame's first shares its encoder";
    EXPECT_EQ(queue.passes_restarted() - restarted, 0u);
    MetalSubmitQueue::set_pass_merge_for_test(std::nullopt);
}
