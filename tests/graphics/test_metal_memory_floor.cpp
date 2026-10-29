// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_metal_memory_floor.cpp
/// @brief GPU resources respect the host memory floor.
///
/// GPU memory cannot be compressed or paged; Satisfactory's world load froze
/// the whole Mac by allocating past what the host had. At the floor the
/// backend refuses, and says so, so the D3D frontends can return the
/// documented E_OUTOFMEMORY instead of a generic failure.

#include <gtest/gtest.h>

#include "core/memory/host_memory_floor.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

using namespace vitreo;
using namespace vitreo::graphics;

TEST(MetalMemoryFloor, ResourcesAreRefusedAtTheFloorAndSayWhy) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendTexture2DDesc td{};
    td.width = td.height = 256;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.shader_resource = true;
    BackendBufferDesc bd{};
    bd.byte_width = 1u << 20;
    bd.shader_resource = true;

    memory::set_host_memory_floor_bytes(~u64{0} - 1);  // nothing fits
    EXPECT_EQ(device.create_texture_2d(td, nullptr, 0), nullptr);
    EXPECT_TRUE(memory::take_host_memory_refusal());
    EXPECT_FALSE(memory::take_host_memory_refusal()) << "taking clears it";
    EXPECT_EQ(device.create_buffer(bd, nullptr), nullptr);
    EXPECT_TRUE(memory::take_host_memory_refusal());

    memory::set_host_memory_floor_bytes(0);  // disabled
    EXPECT_NE(device.create_texture_2d(td, nullptr, 0), nullptr);
    EXPECT_FALSE(memory::take_host_memory_refusal());
    memory::set_host_memory_floor_bytes(~u64{0});
}

TEST(MetalMemoryFloor, CriticalPressureRefusesGpuResources) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    BackendBufferDesc bd{};
    bd.byte_width = 1u << 20;
    bd.shader_resource = true;
    memory::set_host_memory_floor_bytes(u64{1} << 30);
    const memory::HostMemoryReadings critical{
        u64{1} << 40, memory::HostMemoryPressure::critical};
    memory::set_host_memory_readings_override(&critical);
    EXPECT_EQ(device.create_buffer(bd, nullptr), nullptr);
    EXPECT_TRUE(memory::take_host_memory_refusal());
    BackendVideoMemoryInfo info{};
    ASSERT_TRUE(device.query_video_memory(info));
    EXPECT_EQ(info.budget, info.current_usage) << "no headroom when critical";
    memory::set_host_memory_readings_override(nullptr);
    memory::set_host_memory_floor_bytes(~u64{0});
}

TEST(MetalMemoryFloor, VideoMemoryUsageCountsAllocations) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    memory::set_host_memory_floor_bytes(0);
    BackendVideoMemoryInfo before{};
    ASSERT_TRUE(device.query_video_memory(before));
    EXPECT_EQ(before.budget, device.recommended_working_set_size())
        << "admission disabled: Metal's working set is the whole budget";
    BackendBufferDesc bd{};
    bd.byte_width = 16u << 20;
    bd.shader_resource = true;
    auto buffer = device.create_buffer(bd, nullptr);
    ASSERT_NE(buffer, nullptr);
    BackendVideoMemoryInfo after{};
    ASSERT_TRUE(device.query_video_memory(after));
    EXPECT_GT(after.current_usage, 0u);
    EXPECT_GE(after.current_usage, before.current_usage + bd.byte_width);
    memory::set_host_memory_floor_bytes(~u64{0});
}
