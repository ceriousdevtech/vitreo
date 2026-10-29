// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_gpu_timestamp.cpp
/// @brief GpuTimestamp: a point in the command stream that reports when the
///        GPU finished the work recorded before it.
///
/// D3D11 EVENT and TIMESTAMP queries are answered from these points. Stamping
/// the host clock when the guest called End measured command RECORDING, not
/// GPU execution, and gave UE's GPU benchmark a result ~100x too slow. The
/// properties that matter: recording does not submit, a point becomes ready
/// only after a flush, and the times are ordered and inside the host-clock
/// window (GPUEndTime is on the CACurrentMediaTime / mach_absolute_time clock).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <mach/mach_time.h>

#include <chrono>
#include <thread>

using namespace vitreo::graphics;

namespace {

vitreo::u64 host_now_ns() {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

bool wait_ready(const GpuTimestamp& point) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!point.ready()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST(GpuTimestamp, PointsBecomeReadyAfterFlushInOrderOnTheHostClock) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    device.flush();

    const vitreo::u64 before = host_now_ns();
    const auto first = device.record_timestamp();
    const auto second = device.record_timestamp();
    ASSERT_TRUE(first && second);

    // Recording alone submits nothing: GetData must flush to make progress.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(first->ready()) << "a recorded point was submitted without a "
                                    "flush";

    device.flush();
    ASSERT_TRUE(wait_ready(*first));
    ASSERT_TRUE(wait_ready(*second));
    const vitreo::u64 after = host_now_ns();

    EXPECT_LE(before, first->gpu_time_ns());
    EXPECT_LE(first->gpu_time_ns(), second->gpu_time_ns());
    EXPECT_LE(second->gpu_time_ns(), after);
}
