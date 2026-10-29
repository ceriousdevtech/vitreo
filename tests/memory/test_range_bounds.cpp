// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_range_bounds.cpp
/// @brief Guest address-range bounds: no integer overflow may admit a bad address
///
/// A guest address near UINT64_MAX must never resolve to a host pointer. The
/// TLB fast path used to test `addr + size <= limit`, which wraps: with
/// addr = 0xFFFFFFFFFFFFFFFF and size = 4 the sum is 3, the bound passed, and
/// the caller received host_data + (addr - base) — a wild pointer that the
/// write then dereferenced, crashing the host with SIGBUS instead of reporting
/// a guest fault.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>

#include <limits>

using namespace vitreo;

namespace {

class RangeBoundsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mem_.map_region(kBase, kSize,
                                    MemoryProtection::ReadWrite).is_ok());
        // Warm the TLB so the fast path is the one under test.
        ASSERT_TRUE(mem_.write_u32(kBase, 0x11223344).is_ok());
    }

    static constexpr GuestAddr kBase = 0x10000;
    static constexpr usize     kSize = 0x2000;
    GuestMemory mem_;
};

TEST_F(RangeBoundsTest, WriteAtUint64MaxIsRejected) {
    // Must return an error, and must not touch host memory.
    EXPECT_TRUE(mem_.write_u32(std::numeric_limits<u64>::max(), 0).is_err());
}

TEST_F(RangeBoundsTest, WriteNearUint64MaxIsRejected) {
    for (u64 delta = 0; delta < 8; ++delta) {
        const u64 addr = std::numeric_limits<u64>::max() - delta;
        EXPECT_TRUE(mem_.write_u32(addr, 0).is_err())
            << "accepted address 0x" << std::hex << addr;
    }
}

TEST_F(RangeBoundsTest, ReadAtUint64MaxIsRejected) {
    EXPECT_TRUE(mem_.read_u32(std::numeric_limits<u64>::max()).is_err());
}

TEST_F(RangeBoundsTest, WriteStraddlingRegionEndIsRejected) {
    // Last 3 bytes of the region: a 4-byte write would run past the end.
    const GuestAddr last = kBase + kSize - 3;
    EXPECT_TRUE(mem_.write_u32(last, 0xDEADBEEF).is_err())
        << "a 4-byte write must not straddle the region end";
}

TEST_F(RangeBoundsTest, WriteEndingExactlyAtRegionEndIsAccepted) {
    const GuestAddr last = kBase + kSize - 4;
    EXPECT_TRUE(mem_.write_u32(last, 0xDEADBEEF).is_ok());
}

TEST_F(RangeBoundsTest, RangeWritableRejectsOverflowAndUnmapped) {
    // Wrapping range.
    EXPECT_FALSE(mem_.is_range_writable(std::numeric_limits<u64>::max(), 4, true));
    // Beyond the 32-bit guest address space.
    EXPECT_FALSE(mem_.is_range_writable(0x1'0000'0000ull, 4, /*is_64bit=*/false));
    // Unmapped.
    EXPECT_FALSE(mem_.is_range_writable(0x900000, 4, true));
    // Crossing out of a mapped region.
    EXPECT_FALSE(mem_.is_range_writable(kBase + kSize - 2, 8, true));
    // Fully inside.
    EXPECT_TRUE(mem_.is_range_writable(kBase, 16, true));
}

TEST_F(RangeBoundsTest, RangeWritableRejectsReadOnlySpan) {
    constexpr GuestAddr ro = 0x40000;
    ASSERT_TRUE(mem_.map_region(ro, 0x1000, MemoryProtection::Read).is_ok());
    EXPECT_FALSE(mem_.is_range_writable(ro, 4, true));
}

} // namespace
