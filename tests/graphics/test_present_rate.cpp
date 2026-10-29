/// @file test_present_rate.cpp
/// @brief graphics::recent_presents, the performance overlay's frame rate.

#include <gtest/gtest.h>
#include "graphics/common/present_rate.hpp"

#include <thread>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

class RecentPresentsTest : public ::testing::Test {
protected:
    void SetUp() override { reset_present_rate_for_testing(); }
    void TearDown() override { reset_present_rate_for_testing(); }
};

TEST_F(RecentPresentsTest, NoFramesYet) {
    const RecentPresents recent = recent_presents(present_clock_ns());
    EXPECT_EQ(recent.frames, 0u);
    EXPECT_EQ(recent.width, 0u);
    EXPECT_EQ(recent.average_ms, 0.0);
}

TEST_F(RecentPresentsTest, CountsTheLastSecondAndItsIntervals) {
    for (int i = 0; i < 5; ++i) {
        note_present(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    note_present_size(2560, 1440);
    const RecentPresents recent = recent_presents(present_clock_ns());
    EXPECT_EQ(recent.frames, 5u);
    EXPECT_EQ(recent.width, 2560u);
    EXPECT_EQ(recent.height, 1440u);
    EXPECT_GE(recent.average_ms, 9.0);
    EXPECT_LT(recent.average_ms, 200.0);
    EXPECT_GE(recent.worst_ms, recent.average_ms);
}

// A second later the same frames are out of the window, and the interval
// still open -- the stall -- is the worst one.
TEST_F(RecentPresentsTest, FramesAgeOutAndAStallShowsAsTheWorstInterval) {
    note_present(false);
    note_present(false);
    const u64 later = present_clock_ns() + 1500000000ull;
    const RecentPresents recent = recent_presents(later);
    EXPECT_EQ(recent.frames, 0u);
    EXPECT_GE(recent.worst_ms, 1500.0);
}

// The graph's history: intervals oldest first, capped at the buffer.
TEST_F(RecentPresentsTest, FrameIntervalsReadOldestFirst) {
    for (int i = 0; i < 4; ++i) {
        note_present(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + 10 * i));
    }
    float intervals[8] = {};
    const usize n = recent_frame_intervals_ms(present_clock_ns(), 1000000000ull,
                                              intervals, 8);
    ASSERT_EQ(n, 3u);
    EXPECT_GE(intervals[0], 4.0f);
    EXPECT_LT(intervals[0], intervals[2]);

    float capped[2] = {};
    ASSERT_EQ(recent_frame_intervals_ms(present_clock_ns(), 1000000000ull,
                                        capped, 2), 2u);
    EXPECT_FLOAT_EQ(capped[1], intervals[2]);
}

}  // namespace
