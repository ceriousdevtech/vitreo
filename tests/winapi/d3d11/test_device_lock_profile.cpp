// SPDX-License-Identifier: LGPL-2.1-only
#include <gtest/gtest.h>
#include "winapi/graphics/common/device_lock_profile.hpp"
#include <limits>

namespace vitreo::winapi::device_lock_profile {

TEST(DeviceLockProfileTimeTest, RationalTimebaseKeepsExactReportingBoundary) {
    // Apple QA1398: elapsed nanoseconds = ticks * numer / denom.
    // 125/3 ns per tick: 23 ticks = 958 ns, 24 = 1000 ns, 25 = 1041 ns.
    EXPECT_FALSE(detail::ticks_reach_ns(23, 1000, 125, 3));
    EXPECT_TRUE(detail::ticks_reach_ns(24, 1000, 125, 3));
    EXPECT_TRUE(detail::ticks_reach_ns(25, 1000, 125, 3));
    // Sub-nanosecond ticks must round the threshold upward, not downward.
    EXPECT_FALSE(detail::ticks_reach_ns(2, 1, 1, 3));
    EXPECT_TRUE(detail::ticks_reach_ns(3, 1, 1, 3));
    EXPECT_FALSE(detail::ticks_reach_ns(0, 1, 1, 1));
    EXPECT_TRUE(detail::ticks_reach_ns(0, 0, 1, 1));
}

TEST(DeviceLockProfileTimeTest, ProductsDoNotOverflowAtFullWidth) {
    constexpr u64 max = std::numeric_limits<u64>::max();
    constexpr u32 scale = std::numeric_limits<u32>::max();
    EXPECT_TRUE(detail::ticks_reach_ns(max, max, scale, scale));
    EXPECT_FALSE(detail::ticks_reach_ns(max - 1, max, scale, scale));
    EXPECT_TRUE(detail::ticks_reach_ns(max, max, scale, 1));
    EXPECT_FALSE(detail::ticks_reach_ns(max, max, 1, scale));
    EXPECT_FALSE(detail::ticks_reach_ns(max, max, 1, 2));
}

TEST(DeviceLockProfileTimeTest, ReportingIntervalsStayInTheTickDomain) {
    // The same boundary rule applies to the 20 ms hold and one-second window.
    constexpr u64 hold_ticks = 480000;
    constexpr u64 second_ticks = 24000000;
    EXPECT_FALSE(detail::ticks_reach_ns(hold_ticks - 1, 20000000, 125, 3));
    EXPECT_TRUE(detail::ticks_reach_ns(hold_ticks, 20000000, 125, 3));
    EXPECT_FALSE(detail::ticks_reach_ns(second_ticks - 1, 1000000000, 125, 3));
    EXPECT_TRUE(detail::ticks_reach_ns(second_ticks, 1000000000, 125, 3));
}

}  // namespace vitreo::winapi::device_lock_profile
