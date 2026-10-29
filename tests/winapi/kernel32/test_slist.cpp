// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_slist.cpp
/// @brief SList layouts from winnt.h (10.0.26100) for both bitnesses.

#include "winapi/kernel32/synchronization/slist.hpp"
#include "core/memory/guest_memory.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace vitreo;
namespace slist = vitreo::winapi::slist;

namespace {
constexpr GuestAddr kHead = 0x10000;
}

TEST(SList, X64HeaderLayoutAndFullWidthPointers) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kHead, 0x1000, MemoryProtection::ReadWrite).is_ok());
    const GuestAddr high = 0x0000'7FF1'2345'6700ull;  // above 4 GiB, 16-aligned
    ASSERT_TRUE(memory.map_region(high & ~0xFFFull, 0x1000, MemoryProtection::ReadWrite).is_ok());
    const GuestAddr e1 = high, e2 = high + 0x40;
    slist::initialize(memory, kHead, true);
    EXPECT_EQ(slist::push(memory, kHead, e1, true), 0u);
    EXPECT_EQ(slist::push(memory, kHead, e2, true), e1);
    // winnt.h HeaderX64: Depth bits 0-15, Sequence 16-63; NextEntry in the
    // second quadword above 4 reserved bits.
    EXPECT_EQ(memory.read_u64(kHead).value() & 0xFFFF, 2u);
    // Sequence: +2 per push (handles_waits_probe.expected seq_push lines).
    EXPECT_EQ(memory.read_u64(kHead).value() >> 16, 4u);
    EXPECT_EQ(memory.read_u64(kHead + 8).value(), e2);
    EXPECT_EQ(memory.read_u64(e2).value(), e1);  // SLIST_ENTRY.Next is 8 bytes
    EXPECT_EQ(slist::depth(memory, kHead, true), 2u);
    EXPECT_EQ(slist::pop(memory, kHead, true), e2);
    EXPECT_EQ(slist::flush(memory, kHead, true), e1);
    EXPECT_EQ(slist::depth(memory, kHead, true), 0u);
    EXPECT_EQ(slist::pop(memory, kHead, true), 0u);
}

TEST(SList, X86HeaderLayout) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kHead, 0x1000, MemoryProtection::ReadWrite).is_ok());
    slist::initialize(memory, kHead, false);
    EXPECT_EQ(slist::push(memory, kHead, kHead + 0x100, false), 0u);
    EXPECT_EQ(memory.read_u32(kHead).value(), kHead + 0x100);   // Next
    EXPECT_EQ(memory.read_u16(kHead + 4).value(), 1u);          // Depth
    EXPECT_EQ(memory.read_u16(kHead + 6).value(), 0u);          // Sequence: never bumped on x86 (probe)
}

// handles_waits_probe.expected (x64): push,push,pop,pop,pop(empty),push,
// flush,flush(empty),push_list(2) read back as these Depth|Sequence words.
TEST(SList, X64SequenceMatchesWindows) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kHead, 0x1000, MemoryProtection::ReadWrite).is_ok());
    (void)memory.write_u64(kHead, 0xCCCCCCCCCCCCCCCCull);
    (void)memory.write_u64(kHead + 8, 0xCCCCCCCCCCCCCCCCull);
    slist::initialize(memory, kHead, true);
    auto q0 = [&] { return memory.read_u64(kHead).value(); };
    EXPECT_EQ(q0(), 0u);
    EXPECT_EQ(memory.read_u64(kHead + 8).value(), 0u);
    const GuestAddr t0 = kHead + 0x100, t1 = kHead + 0x110;
    (void)slist::push(memory, kHead, t0, true); EXPECT_EQ(q0(), 0x20001u);
    (void)slist::push(memory, kHead, t1, true); EXPECT_EQ(q0(), 0x40002u);
    (void)slist::pop(memory, kHead, true);      EXPECT_EQ(q0(), 0x40001u);
    (void)slist::pop(memory, kHead, true);      EXPECT_EQ(q0(), 0x40000u);
    (void)slist::pop(memory, kHead, true);      EXPECT_EQ(q0(), 0x40000u);
    (void)slist::push(memory, kHead, t0, true); EXPECT_EQ(q0(), 0x60001u);
    (void)slist::flush(memory, kHead, true);    EXPECT_EQ(q0(), 0x60000u);
    (void)slist::flush(memory, kHead, true);    EXPECT_EQ(q0(), 0x60000u);
    (void)memory.write_u64(t0, t1);
    EXPECT_EQ(slist::push_list(memory, kHead, t0, t1, 2, true), 0u);
    EXPECT_EQ(q0(), 0x80002u);
    EXPECT_EQ(memory.read_u64(t1).value(), 0u);
}

TEST(SList, X86SequenceNeverChanges) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kHead, 0x1000, MemoryProtection::ReadWrite).is_ok());
    slist::initialize(memory, kHead, false);
    const GuestAddr t0 = kHead + 0x100, t1 = kHead + 0x110;
    (void)slist::push(memory, kHead, t0, false);
    (void)slist::push(memory, kHead, t1, false);
    (void)slist::pop(memory, kHead, false);
    (void)memory.write_u32(t0, t1);
    (void)memory.write_u32(t1, 0);
    (void)slist::flush(memory, kHead, false);
    (void)slist::push_list(memory, kHead, t0, t1, 2, false);
    EXPECT_EQ(memory.read_u16(kHead + 4).value(), 2u);  // Depth
    EXPECT_EQ(memory.read_u16(kHead + 6).value(), 0u);  // Sequence
}

TEST(SList, ConcurrentPushPopFlushLoseNothing) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kHead, 0x20000, MemoryProtection::ReadWrite).is_ok());
    slist::initialize(memory, kHead, true);
    constexpr int kThreads = 4, kPer = 500;
    std::vector<std::thread> threads;
    std::atomic<int> popped{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                const GuestAddr e = kHead + 0x100 + static_cast<GuestAddr>((t * kPer + i) * 16);
                (void)slist::push(memory, kHead, e, true);
                if (i % 2 == 1 && slist::pop(memory, kHead, true) != 0) ++popped;
            }
        });
    }
    for (auto& th : threads) th.join();
    int remaining = 0;
    for (GuestAddr e = slist::flush(memory, kHead, true); e != 0;
         e = static_cast<GuestAddr>(memory.read_u64(e).value())) {
        ++remaining;
    }
    EXPECT_EQ(popped + remaining, kThreads * kPer);
}
