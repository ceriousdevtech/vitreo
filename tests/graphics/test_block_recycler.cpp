// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_block_recycler.cpp
/// @brief Large equal-sized allocations recycled between threads
///        (BlockRecycler / RecyclingAllocator).
///
/// The Metal submit queue records its op lists into a std::deque whose blocks
/// are ~54 KB each. The recording thread allocates them and the encode thread
/// frees them, hundreds of times a frame. These tests use the same shape: a
/// deque of large elements is built on one thread and destroyed on another,
/// and the next deque must be built from the blocks the first one returned.

#include <gtest/gtest.h>

#include "graphics/metal/block_recycler.hpp"

#include <vitreo/ab_cycle.hpp>

#include <deque>
#include <thread>
#include <utility>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

/// Large enough that libc++ gives a deque of it 16-element blocks of 48 KB.
struct Big {
    u8 bytes[3000];
};
using BigList = std::deque<Big, RecyclingAllocator<Big>>;

BigList build(usize n) {
    BigList list;
    for (usize i = 0; i < n; ++i) list.emplace_back();
    return list;
}

void destroy_on_another_thread(BigList list) {
    std::thread([moved = std::move(list)]() mutable { BigList gone = std::move(moved); })
        .join();
}

class BlockRecyclerTest : public ::testing::Test {
protected:
    void SetUp() override {
        BlockRecycler::drain_for_test();
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::On);
    }
    void TearDown() override {
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
        BlockRecycler::drain_for_test();
    }
};

// What the encode thread frees, the recording thread's next list reuses: no
// fresh large allocation at all for a list no longer than the first.
TEST_F(BlockRecyclerTest, BlocksFreedOnOneThreadBuildTheNextList) {
    destroy_on_another_thread(build(200));
    const BlockRecycler::Stats before = BlockRecycler::stats();
    EXPECT_GT(before.cached_bytes, 0u);
    BigList second = build(200);
    const BlockRecycler::Stats after = BlockRecycler::stats();
    EXPECT_EQ(after.fresh, before.fresh) << "the second list allocated anew";
    EXPECT_GT(after.reused, before.reused);
    for (Big& b : second) b.bytes[sizeof(b.bytes) - 1] = 1;  // usable memory
}

// A cached block only ever satisfies a request of exactly its own size.
TEST_F(BlockRecyclerTest, ABlockIsNeverHandedOutAtAnotherSize) {
    RecyclingAllocator<u8> bytes;
    u8* const a = bytes.allocate(48 * 1024);
    bytes.deallocate(a, 48 * 1024);
    const BlockRecycler::Stats before = BlockRecycler::stats();
    u8* const b = bytes.allocate(64 * 1024);
    EXPECT_EQ(BlockRecycler::stats().fresh, before.fresh + 1);
    EXPECT_EQ(BlockRecycler::stats().reused, before.reused);
    b[64 * 1024 - 1] = 7;
    bytes.deallocate(b, 64 * 1024);
    u8* const c = bytes.allocate(48 * 1024);
    EXPECT_EQ(c, a) << "the 48 KB request reuses the 48 KB block";
    bytes.deallocate(c, 48 * 1024);
}

// The cache holds at most kMaxCachedBytes; a block beyond that is freed.
TEST_F(BlockRecyclerTest, TheCacheIsBounded) {
    constexpr usize kBlock = 1u << 20;
    RecyclingAllocator<u8> bytes;
    std::vector<u8*> blocks;
    const usize n = BlockRecycler::kMaxCachedBytes / kBlock + 8;
    for (usize i = 0; i < n; ++i) blocks.push_back(bytes.allocate(kBlock));
    const BlockRecycler::Stats before = BlockRecycler::stats();
    for (u8* p : blocks) bytes.deallocate(p, kBlock);
    const BlockRecycler::Stats after = BlockRecycler::stats();
    EXPECT_LE(after.cached_bytes, BlockRecycler::kMaxCachedBytes);
    EXPECT_EQ(after.released - before.released, 8u);
}

// Small allocations stay with malloc, which caches them already.
TEST_F(BlockRecyclerTest, SmallAllocationsBypassTheCache) {
    const BlockRecycler::Stats before = BlockRecycler::stats();
    {
        std::deque<int, RecyclingAllocator<int>> small;
        for (int i = 0; i < 10000; ++i) small.push_back(i);
    }
    const BlockRecycler::Stats after = BlockRecycler::stats();
    EXPECT_EQ(after.fresh, before.fresh);
    EXPECT_EQ(after.cached_bytes, before.cached_bytes);
}

// A container of small, fixed-size blocks (the submit queue's per-draw push
// constants, ~1 KB each) opts in with a lower threshold and is reused the same
// way, while the default threshold still leaves that size to malloc.
TEST_F(BlockRecyclerTest, ALowerThresholdRecyclesSmallFixedBlocks) {
    using Push = std::vector<u8, RecyclingAllocator<u8, 256>>;
    const Push::value_type fill = 0x5A;
    std::thread([&] { Push gone(1024, fill); }).join();
    const BlockRecycler::Stats before = BlockRecycler::stats();
    EXPECT_EQ(before.cached_bytes, 1024u);
    Push next(1024, 0x11);
    EXPECT_EQ(BlockRecycler::stats().reused, before.reused + 1);
    EXPECT_EQ(next[1023], 0x11);

    RecyclingAllocator<u8> large_only;
    u8* const plain = large_only.allocate(1024);
    large_only.deallocate(plain, 1024);
    EXPECT_EQ(BlockRecycler::stats().cached_bytes, 0u)
        << "the default threshold cached a 1 KB block";
}

// The A/B arm that turns the recycler off is plain malloc and free.
TEST_F(BlockRecyclerTest, TheOffArmAllocatesAndFreesPlainly) {
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::Off);
    destroy_on_another_thread(build(200));
    const BlockRecycler::Stats before = BlockRecycler::stats();
    EXPECT_EQ(before.cached_bytes, 0u);
    destroy_on_another_thread(build(200));
    EXPECT_EQ(BlockRecycler::stats().reused, before.reused);
    EXPECT_EQ(BlockRecycler::stats().cached_bytes, 0u)
        << "the off arm kept a block instead of freeing it";
}

}  // namespace
