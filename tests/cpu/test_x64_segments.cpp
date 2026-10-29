// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_x64_segments.cpp
/// @brief LDT segment allocation for the native x86_64 backend.
///
/// x86_64 only -- these tests execute segment loads and read through GS, so
/// they must run on real Intel hardware. Building them is not evidence.

#include <gtest/gtest.h>

#include "core/cpu/x64/x64_segments.hpp"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::cpu::x64;

namespace {

/// A stand-in TEB placed below 4GB, as a segment base requires.
/// Only the fields early Win64 code touches are laid out.
struct FakeTeb {
    u64 pad00[6];   // 0x00..0x2F
    u64 self;       // 0x30  NT_TIB.Self
    u64 pad38[5];   // 0x38..0x5F
    u64 peb;        // 0x60
};

constexpr u64 kLowTebAddr = 0x31000000;

FakeTeb* map_low_teb(u64 addr) {
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
                   0x1000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    auto* teb = static_cast<FakeTeb*>(p);
    *teb = FakeTeb{};
    teb->self = addr;
    teb->peb  = 0xDEADBEEF;
    return teb;
}

} // namespace

TEST(X64SegmentsTest, RejectsABaseAtOrAboveFourGB) {
    // Descriptor bases are 32 bits. Truncating would yield a segment pointing
    // at unrelated memory and a TEB read returning plausible garbage, so this
    // must fail loudly rather than succeed wrongly.
    EXPECT_TRUE(SegmentTable::allocate_data_segment(kSegmentBaseLimit).is_err());
    EXPECT_TRUE(SegmentTable::allocate_data_segment(0x1'0000'0000ULL).is_err());
    EXPECT_TRUE(SegmentTable::allocate_data_segment(~0ULL).is_err());
}

TEST(X64SegmentsTest, AcceptsABaseBelowFourGB) {
    auto sel = SegmentTable::allocate_data_segment(0x30000000);
    ASSERT_TRUE(sel.is_ok()) << "i386_set_ldt refused a valid descriptor";
    // TI=1 (LDT) and RPL=3 (user) are what make the selector usable from
    // user mode at all.
    EXPECT_EQ(sel.value() & 0x7, 0x7);
}

// Entering guest GS is one-way, so the whole experiment runs in a forked
// child: the parent keeps a usable pthread TLS pointer and can report.
TEST(X64SegmentsTest, GuestTebIsReadableThroughGs) {
    FakeTeb* teb = map_low_teb(kLowTebAddr);
    ASSERT_NE(teb, nullptr) << "could not map a TEB below 4GB -- is the "
                               "binary linked with -pagezero_size?";

    auto sel = SegmentTable::allocate_data_segment(kLowTebAddr);
    ASSERT_TRUE(sel.is_ok());

    auto* shared = static_cast<volatile u64*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(shared, MAP_FAILED);
    shared[0] = 0;  // gs:[0x30]
    shared[1] = 0;  // gs:[0x60]
    shared[2] = 0;  // reached the end

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        enter_guest_gs(sel.value());
        // Only raw reads from here on. Any libc call would resolve TLS
        // through the guest TEB and fault.
        u64 a = 0, b = 0;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(a));
        __asm__ volatile("movq %%gs:0x60, %0" : "=r"(b));
        shared[0] = a;
        shared[1] = b;
        shared[2] = 1;
        _exit(0);   // _exit, not exit: no atexit handlers, no libc teardown.
    }
    int status = 0;
    waitpid(pid, &status, 0);

    EXPECT_EQ(shared[2], 1u) << "child died before completing the reads";
    EXPECT_EQ(shared[0], kLowTebAddr);      // NT_TIB.Self
    EXPECT_EQ(shared[1], 0xDEADBEEFu);      // PEB

    munmap(const_cast<u64*>(shared), 4096);
    munmap(teb, 0x1000);
}

// Guards the constraint that shapes the whole Win64 design. If some future
// macOS makes GS restorable, this test fails and the Win64 GS fork can be
// revisited -- which is exactly when it should be.
TEST(X64SegmentsTest, HostGsBaseCannotBeRestored) {
    auto* shared = static_cast<volatile u64*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANON, -1, 0));
    ASSERT_NE(shared, MAP_FAILED);
    shared[0] = 0;  // gs:[0x00] after reloading the host's own selector
    shared[1] = 0;  // 1 = the read survived

    u64 before = 0;
    __asm__ volatile("movq %%gs:0x00, %0" : "=r"(before));
    ASSERT_NE(before, 0u) << "host pthread TLS pointer not readable";

    const u16 host_gs = current_gs();
    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        // A no-op in principle: reload the selector already in GS.
        set_gs(host_gs);
        u64 after = 0;
        __asm__ volatile("movq %%gs:0x00, %0" : "=r"(after));
        shared[0] = after;
        shared[1] = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    // Either the read faulted, or it came back as something other than the
    // host's pthread pointer. Both mean the base did not survive.
    const bool survived = (shared[1] == 1) && (shared[0] == before);
    EXPECT_FALSE(survived)
        << "GS now round-trips on this OS -- revisit the Win64 GS fork";

    munmap(const_cast<u64*>(shared), 4096);
}

TEST(X64SegmentsTest, Compat32PairIsFlatAndCached) {
    u16 code_a = 0, data_a = 0;
    ASSERT_TRUE(SegmentTable::allocate_compat32_pair(&code_a, &data_a).is_ok());
    EXPECT_NE(code_a, 0);
    EXPECT_NE(data_a, 0);
    EXPECT_EQ(code_a & 0x7, 0x7);
    EXPECT_EQ(data_a & 0x7, 0x7);
    // Allocated as an adjacent pair.
    EXPECT_EQ(data_a, code_a + 8);

    // LDT entries are a finite per-process resource, so the pair must be
    // allocated once and reused, not re-allocated per caller.
    u16 code_b = 0, data_b = 0;
    ASSERT_TRUE(SegmentTable::allocate_compat32_pair(&code_b, &data_b).is_ok());
    EXPECT_EQ(code_b, code_a);
    EXPECT_EQ(data_b, data_a);
}

TEST(X64SegmentsTest, Compat32PairRejectsNullOutputs) {
    u16 sel = 0;
    EXPECT_TRUE(SegmentTable::allocate_compat32_pair(nullptr, &sel).is_err());
    EXPECT_TRUE(SegmentTable::allocate_compat32_pair(&sel, nullptr).is_err());
}
