// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_command_list.cpp
/// @brief Deferred command recording: record without executing, then replay.
///
/// A command list's defining property is negative — nothing it records may reach
/// the GPU until it is submitted. A test that only checks the work eventually
/// happens would pass against an implementation that never deferred anything at
/// all, so every test here first asserts the work has *not* happened.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

using namespace vitreo::graphics;

namespace {

/// Compute pipeline that writes 1.0 into every texel of a u# texture.
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

[[nodiscard]] std::unique_ptr<BackendTexture2D> cleared_target(
    GraphicsBackendDevice& device) {
    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.unordered_access = true;
    td.render_target = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    if (texture) (void)texture->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f});
    return texture;
}

[[nodiscard]] BackendDispatchCall fill_call(BackendComputePipeline* pipeline,
                                            BackendTexture2D* target) {
    BackendDispatchCall call{};
    call.pipeline = pipeline;
    call.group_count_x = call.group_count_y = 4;
    call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.textures[8] = target;
    return call;
}

[[nodiscard]] std::uint8_t first_red(BackendTexture2D& texture) {
    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    if (!texture.readback(0, pixels.data(), 4u * 4u)) return 0xFFu;
    return pixels[0];
}

} // namespace

TEST(CommandList, RecordsWithoutExecutingUntilSubmitted) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_command_lists()) GTEST_SKIP() << "no command lists";

    auto pipeline = fill_pipeline(device);
    auto target = cleared_target(device);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(target, nullptr);

    auto list = device.create_command_list();
    ASSERT_NE(list, nullptr);
    ASSERT_TRUE(list->valid());
    EXPECT_EQ(list->operation_count(), 0u);

    {
        BackendCommandListCapture capture(list.get());
        ASSERT_TRUE(capture.active());
        ASSERT_TRUE(device.dispatch(fill_call(pipeline.get(), target.get())));
    }
    EXPECT_GT(list->operation_count(), 0u)
        << "the dispatch was not captured into the list";

    // The property that matters. Readback drains the queue, so if the dispatch
    // had gone to the immediate stream the texture would already be white.
    EXPECT_EQ(first_red(*target), 0u)
        << "recorded work executed before the list was submitted";

    // An open list may not be submitted: D3D11 hands one back only from
    // FinishCommandList and D3D12 requires Close.
    EXPECT_FALSE(device.submit_command_list(list.get()))
        << "an open list must not run";
    ASSERT_TRUE(list->close());
    EXPECT_TRUE(list->is_closed());

    ASSERT_TRUE(device.submit_command_list(list.get()));
    EXPECT_EQ(first_red(*target), 255u) << "the submitted list did not run";
}

// D3D11 permits executing one command list more than once, so the list must keep
// its resources alive across submissions. If submitting released them, the second
// run would read freed textures -- or crash.
TEST(CommandList, SurvivesBeingSubmittedTwice) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_command_lists()) GTEST_SKIP() << "no command lists";

    auto pipeline = fill_pipeline(device);
    auto target = cleared_target(device);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(target, nullptr);

    auto list = device.create_command_list();
    ASSERT_NE(list, nullptr);
    {
        BackendCommandListCapture capture(list.get());
        ASSERT_TRUE(capture.active());
        ASSERT_TRUE(device.dispatch(fill_call(pipeline.get(), target.get())));
    }
    ASSERT_TRUE(list->close());

    ASSERT_TRUE(device.submit_command_list(list.get()));
    EXPECT_EQ(first_red(*target), 255u);

    // Clear, then replay the same list. A list whose resources were released on
    // the first submission cannot do this.
    ASSERT_TRUE(target->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));
    EXPECT_EQ(first_red(*target), 0u);
    ASSERT_TRUE(device.submit_command_list(list.get()));
    EXPECT_EQ(first_red(*target), 255u) << "the second submission did nothing";

    EXPECT_GT(list->operation_count(), 0u)
        << "submitting must not consume the recording";
}

// Reset discards the recording and reopens the list, which is D3D12's Reset.
TEST(CommandList, ResetDiscardsTheRecordingAndReopens) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_command_lists()) GTEST_SKIP() << "no command lists";

    auto pipeline = fill_pipeline(device);
    auto target = cleared_target(device);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(target, nullptr);

    auto list = device.create_command_list();
    ASSERT_NE(list, nullptr);
    {
        BackendCommandListCapture capture(list.get());
        ASSERT_TRUE(capture.active());
        ASSERT_TRUE(device.dispatch(fill_call(pipeline.get(), target.get())));
    }
    ASSERT_TRUE(list->close());
    ASSERT_GT(list->operation_count(), 0u);

    ASSERT_TRUE(list->reset());
    EXPECT_EQ(list->operation_count(), 0u) << "reset must discard the recording";
    EXPECT_FALSE(list->is_closed()) << "reset must reopen the list";

    // Submitting the emptied list must do nothing observable.
    ASSERT_TRUE(list->close());
    ASSERT_TRUE(device.submit_command_list(list.get()));
    EXPECT_EQ(first_red(*target), 0u)
        << "a reset list still replayed its old operations";
}

// Capture is per thread, which is the threading contract both guest APIs state.
// Two threads must be able to build two lists without either seeing the other's
// operations -- and work recorded on a thread that is not capturing must still go
// straight to the GPU.
TEST(CommandList, CaptureIsPerThread) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_command_lists()) GTEST_SKIP() << "no command lists";

    auto pipeline = fill_pipeline(device);
    ASSERT_NE(pipeline, nullptr);
    auto captured_target = cleared_target(device);
    auto immediate_target = cleared_target(device);
    ASSERT_NE(captured_target, nullptr);
    ASSERT_NE(immediate_target, nullptr);

    auto list = device.create_command_list();
    ASSERT_NE(list, nullptr);

    // This thread captures; the other does not.
    BackendCommandListCapture capture(list.get());
    ASSERT_TRUE(capture.active());
    ASSERT_TRUE(device.dispatch(
        fill_call(pipeline.get(), captured_target.get())));

    bool other_dispatched = false;
    std::thread other([&] {
        other_dispatched =
            device.dispatch(fill_call(pipeline.get(), immediate_target.get()));
    });
    other.join();
    ASSERT_TRUE(other_dispatched);

    // The other thread was not capturing, so its work ran immediately.
    EXPECT_EQ(first_red(*immediate_target), 255u)
        << "a non-capturing thread's work was captured by another thread's list";
    // And this thread's work is still only in the list.
    EXPECT_EQ(first_red(*captured_target), 0u)
        << "captured work escaped to the immediate stream";
    EXPECT_EQ(list->operation_count(), 1u)
        << "the list captured the other thread's dispatch too";
}

// Nesting capture on one thread is refused rather than silently redirecting to
// the inner list, which would strand the outer list's operations.
TEST(CommandList, NestedCaptureOnOneThreadIsRefused) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_command_lists()) GTEST_SKIP() << "no command lists";

    auto outer = device.create_command_list();
    auto inner = device.create_command_list();
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);

    ASSERT_TRUE(outer->begin_capture());
    EXPECT_FALSE(inner->begin_capture()) << "nesting must be refused";
    EXPECT_TRUE(outer->end_capture());
    // Once the outer capture ends the inner one is free to start.
    EXPECT_TRUE(inner->begin_capture());
    EXPECT_TRUE(inner->end_capture());
}
