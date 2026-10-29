// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_gpu_busy.cpp
/// @brief graphics::recent_gpu_busy, the performance overlay's GPU time.

#include <gtest/gtest.h>
#include "graphics/common/gpu_busy.hpp"

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

class GpuBusyTest : public ::testing::Test {
protected:
    void SetUp() override { reset_gpu_busy_for_testing(); }
    void TearDown() override { reset_gpu_busy_for_testing(); }
};

TEST_F(GpuBusyTest, NothingRecorded) {
    const RecentGpuBusy busy = recent_gpu_busy(100.0);
    EXPECT_EQ(busy.busy_ms, 0.0);
    EXPECT_EQ(busy.command_buffers, 0u);
    EXPECT_EQ(busy.allocated_bytes, 0u);
    EXPECT_EQ(busy.window_ms, 1000.0);
}

TEST_F(GpuBusyTest, OverlappingIntervalsCountOnce) {
    note_gpu_interval(99.10, 99.20, 0);   // 100 ms
    note_gpu_interval(99.15, 99.25, 0);   // overlaps by 50 ms: +50 ms
    note_gpu_interval(99.50, 99.51, 4096);  // disjoint: +10 ms
    const RecentGpuBusy busy = recent_gpu_busy(100.0);
    EXPECT_NEAR(busy.busy_ms, 160.0, 1e-6);
    EXPECT_EQ(busy.command_buffers, 3u);
    EXPECT_EQ(busy.allocated_bytes, 4096u);
}

TEST_F(GpuBusyTest, ClipsToTheLastSecond) {
    note_gpu_interval(98.0, 98.5, 0);    // entirely before the window
    note_gpu_interval(98.9, 99.1, 0);    // straddles its start: 100 ms in
    const RecentGpuBusy busy = recent_gpu_busy(100.0);
    EXPECT_NEAR(busy.busy_ms, 100.0, 1e-6);
    EXPECT_EQ(busy.command_buffers, 1u);
}

TEST_F(GpuBusyTest, IgnoresEmptyAndInvertedIntervals) {
    note_gpu_interval(0.0, 0.0, 0);
    note_gpu_interval(99.5, 99.4, 0);
    EXPECT_EQ(recent_gpu_busy(100.0).command_buffers, 0u);
}

TEST_F(GpuBusyTest, WantedFlagRoundTrips) {
    set_gpu_busy_wanted(true);
    EXPECT_TRUE(gpu_busy_wanted());
    set_gpu_busy_wanted(false);
    EXPECT_FALSE(gpu_busy_wanted());
}

TEST_F(GpuBusyTest, ClockIsMonotonic) {
    const double a = gpu_busy_clock_s();
    const double b = gpu_busy_clock_s();
    EXPECT_GT(a, 0.0);
    EXPECT_GE(b, a);
}

}  // namespace
