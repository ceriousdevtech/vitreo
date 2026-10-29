// SPDX-License-Identifier: LGPL-2.1-only
//
// CRT malloc/free reuse. Contract (UCRT): free() returns a block to the heap
// for later allocations; a pointer that did not come from malloc/realloc is
// not the heap's to recycle; freeing a block twice is undefined for the
// caller but must not corrupt the heap for everyone else.
#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/msvcrt/memory/crt_memory.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
class CrtHeapReuseTest : public ::testing::Test {
protected:
    GuestMemory memory;
    CrtMemoryManager crt{memory};
};
} // namespace

TEST_F(CrtHeapReuseTest, FreedBlockServesTheNextRequestOfItsClass) {
    const GuestAddr p = crt.crt_malloc(100);
    ASSERT_NE(p, 0u);
    crt.crt_free(p);
    EXPECT_EQ(crt.crt_malloc(100), p);
    // A smaller request in the same 16-byte class reuses it too.
    crt.crt_free(p);
    EXPECT_EQ(crt.crt_malloc(97), p);
}

TEST_F(CrtHeapReuseTest, DoubleFreeListsTheBlockOnce) {
    const GuestAddr p = crt.crt_malloc(64);
    ASSERT_NE(p, 0u);
    crt.crt_free(p);
    crt.crt_free(p);   // must be ignored
    const GuestAddr a = crt.crt_malloc(64);
    const GuestAddr b = crt.crt_malloc(64);
    EXPECT_EQ(a, p);
    EXPECT_NE(b, p) << "the second free handed the same block out twice";
}

TEST_F(CrtHeapReuseTest, ForeignPointerIsNotRecycled) {
    // A readable word before a pointer is not proof the block is ours.
    constexpr GuestAddr region = 0x20000000;
    ASSERT_TRUE(memory.map_region(region, 0x1000, MemoryProtection::ReadWrite).is_ok());
    const GuestAddr foreign = region + 0x100;   // 16-byte aligned
    ASSERT_TRUE(memory.write_u32(foreign - 4, 64).is_ok());   // looks like a header
    crt.crt_free(foreign);
    const GuestAddr q = crt.crt_malloc(64);
    EXPECT_NE(q, foreign);
}

TEST_F(CrtHeapReuseTest, VaryingSizesWithBoundedLiveSetReuseAcrossClasses) {
    constexpr int kLive = 128;
    constexpr int kIters = 20000;
    GuestAddr ring[kLive] = {};
    u32 lcg = 777;
    GuestAddr high_water = 0;
    for (int i = 0; i < kIters; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const usize size = 16 + (lcg >> 8) % 4096;
        const int slot = i % kLive;
        if (ring[slot] != 0) crt.crt_free(ring[slot]);
        ring[slot] = crt.crt_malloc(size);
        ASSERT_NE(ring[slot], 0u) << "iteration " << i;
        high_water = std::max(high_water, ring[slot]);
    }
    // Live set <= 128 * 4 KB = 512 KB. With per-class reuse the arena's
    // high-water mark stays within a few MB of its base; the old exact-size
    // free lists grew it by roughly one block per iteration (~40 MB here).
    EXPECT_LT(high_water - 0x7A000000ull, 8u * 1024 * 1024);
}
