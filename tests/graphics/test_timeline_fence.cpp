// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_timeline_fence.cpp
/// @brief Timeline fences: the value, and the ordering.
///
/// `GpuCompletion` waits for one submission. A timeline is a monotonic 64-bit
/// counter that work signals and that other work waits on by value — what
/// `ID3D11Device5::CreateFence`, every `ID3D12Fence`, and cross-process resource
/// handoff all need.
///
/// The value is the easy half. The half worth testing is ORDERING: a signal
/// recorded after a dispatch must not be observable until that dispatch has run.
/// An implementation that set `signaledValue` directly would pass any test that
/// only checked the number.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

using namespace vitreo::graphics;

namespace {

constexpr vitreo::u64 kSecond = 1000000000ull;

/// A compute pipeline that writes 0xFF into every texel of a u# texture.
[[nodiscard]] std::unique_ptr<BackendComputePipeline> fill_pipeline(
    GraphicsBackendDevice& device) {
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(
            texture2d<float, access::write> u0 [[texture(8)]],
            uint3 id [[thread_position_in_grid]]) {
            if (id.x < u0.get_width() && id.y < u0.get_height()) {
                u0.write(float4(1.0, 1.0, 1.0, 1.0), id.xy);
            }
        }
    )";
    return device.create_compute_pipeline(pd);
}

} // namespace

TEST(TimelineFence, StartsAtItsInitialValueAndRefusesToRunBackwards) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";

    auto fence = device.create_fence(7);
    ASSERT_NE(fence, nullptr);
    ASSERT_TRUE(fence->valid());
    EXPECT_EQ(fence->completed_value(), 7u);

    EXPECT_TRUE(fence->signal_cpu(9));
    EXPECT_EQ(fence->completed_value(), 9u);

    // An MTLSharedEvent never runs backwards, so a lower value is refused
    // rather than silently ignored; D3D12's lowering Signal (measured S_OK on
    // Windows) is the frontend's to build, by replacing the timeline.
    EXPECT_FALSE(fence->signal_cpu(8));
    EXPECT_EQ(fence->completed_value(), 9u)
        << "a refused signal must not move the timeline";

    // Signalling the value it already holds is not backwards.
    EXPECT_TRUE(fence->signal_cpu(9));
}

TEST(TimelineFence, WaitingOnAValueAlreadyReachedReturnsImmediately) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";

    auto fence = device.create_fence(4);
    ASSERT_NE(fence, nullptr);
    EXPECT_TRUE(fence->wait_cpu(4, kSecond));
    EXPECT_TRUE(fence->wait_cpu(1, kSecond));
}

TEST(TimelineFence, WaitingOnAValueNothingWillSignalTimesOut) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";

    auto fence = device.create_fence(0);
    ASSERT_NE(fence, nullptr);
    // Nothing signals this, so the wait must give up rather than hang. A fence
    // whose wait cannot time out turns a guest's lost signal into a deadlock.
    EXPECT_FALSE(fence->wait_cpu(1, 50000000ull));  // 50 ms
}

// The test that distinguishes a real fence from a counter. The signal is
// recorded AFTER a dispatch, so it may only be observable once that dispatch has
// actually run — which the written texels prove independently.
TEST(TimelineFence, GpuSignalIsOrderedAfterTheWorkRecordedBeforeIt) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";

    auto pipeline = fill_pipeline(device);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    BackendTexture2DDesc td{};
    td.width = td.height = 8;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.unordered_access = true;
    td.render_target = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    auto fence = device.create_fence(0);
    ASSERT_NE(fence, nullptr);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = 8;
    call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.textures[8] = texture.get();
    ASSERT_TRUE(device.dispatch(call));

    ASSERT_TRUE(fence->signal(100));
    // The discriminator. A signal is RECORDED, so the timeline must not have
    // moved yet -- the GPU has not run the dispatch it was ordered behind. An
    // implementation that assigned `signaledValue` directly would already read
    // 100 here, and would still pass the pixel check below, because readback
    // drains the queue and writes the texels either way.
    EXPECT_LT(fence->completed_value(), 100u)
        << "signal() applied immediately instead of being recorded in order";

    ASSERT_TRUE(fence->wait_cpu(100, 5 * kSecond))
        << "the GPU signal never arrived";
    EXPECT_GE(fence->completed_value(), 100u);

    // The dispatch the signal was ordered behind must have run.
    std::vector<std::uint8_t> pixels(8u * 8u * 4u, 0);
    ASSERT_TRUE(texture->readback(0, pixels.data(), 8u * 4u));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        EXPECT_EQ(pixels[i], 255u)
            << "texel " << (i / 4)
            << ": the signal was reached without the work before it running";
    }
}

// A GPU wait blocks submitted work until the CPU releases it. This is the shape
// a guest uses to hand a resource between queues or processes, and it is also
// the case that deadlocks if a CPU-side wait drains the queue instead of merely
// submitting it.
TEST(TimelineFence, GpuWaitIsReleasedByALaterCpuSignal) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";

    auto pipeline = fill_pipeline(device);
    ASSERT_NE(pipeline, nullptr);

    BackendTexture2DDesc td{};
    td.width = td.height = 8;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.unordered_access = true;
    td.render_target = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    auto gate = device.create_fence(0);
    auto done = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    ASSERT_NE(done, nullptr);

    // Hold the dispatch behind `gate`, and have it signal `done` when through.
    ASSERT_TRUE(gate->wait_gpu(1));
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = 8;
    call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.textures[8] = texture.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(done->signal(1));

    // Release the gate from another thread so the wait below is a real wait
    // rather than an already-satisfied one.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)gate->signal_cpu(1);
    });
    const bool completed = done->wait_cpu(1, 5 * kSecond);
    releaser.join();

    ASSERT_TRUE(completed) << "the gated work never completed";
    std::vector<std::uint8_t> pixels(8u * 8u * 4u, 0);
    ASSERT_TRUE(texture->readback(0, pixels.data(), 8u * 4u));
    EXPECT_EQ(pixels[0], 255u) << "the gated dispatch did not run";
}

// A signal (or wait) can be the first operation of a submission -- nothing
// recorded before it since the last submit. The recorder had no command
// buffer begun at that point and dropped the op silently, so the fence never
// reached its value and a guest waiting on it hung.
TEST(TimelineFence, SignalThatOpensASubmissionStillLands) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_fences()) GTEST_SKIP() << "no timeline fences";

    auto done = device.create_fence(0);
    ASSERT_NE(done, nullptr);
    device.flush();  // leave nothing recorded
    ASSERT_TRUE(done->signal(1));
    EXPECT_TRUE(done->wait_cpu(1, 5 * kSecond))
        << "a signal recorded first in its submission was dropped";

    auto gate = device.create_fence(0);
    ASSERT_NE(gate, nullptr);
    device.flush();
    ASSERT_TRUE(gate->wait_gpu(1));  // first op of the next submission
    ASSERT_TRUE(done->signal(2));
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)gate->signal_cpu(1);
    });
    const bool reached = done->wait_cpu(2, 5 * kSecond);
    releaser.join();
    EXPECT_TRUE(reached) << "work behind a first-op wait never ran";
}
