// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_x64_high_arena.cpp
/// @brief Placement of the x64 guest's high arenas against the host map.
///
/// The regression these guard: the three arenas used to be fixed constants at
/// 0x7FFA/0x7FFB/0x7FFC. That is free on arm64 macOS and occupied by the dyld
/// shared cache on x86_64 macOS, where every claim failed and the guest
/// faulted on its first system-module reference. The anchor must therefore be
/// a window the *host* actually has free, on whichever host is running.

#include <gtest/gtest.h>

#include "core/memory/x64_high_arena.hpp"

#include <mach/mach.h>
#include <mach/mach_vm.h>

using namespace vitreo;
using namespace vitreo::memory;

namespace {

/// Address of the first host mapping at or after `addr`, or 0 when the rest of
/// the task's address space is empty.
u64 first_mapping_at_or_after(u64 addr) {
    mach_vm_address_t probe = addr;
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    const kern_return_t kr = mach_vm_region_recurse(
        mach_task_self(), &probe, &size, &depth,
        reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
    return kr == KERN_SUCCESS ? static_cast<u64>(probe) : 0;
}

} // namespace

/// The whole 12 GiB span must be clear of host mappings -- this is the
/// property whose absence broke x64 guests on Intel.
TEST(X64HighArenaTest, AnchorWindowIsFreeOnThisHost) {
    const u64 base = x64_high_arena_base();
    const u64 next = first_mapping_at_or_after(base);
    EXPECT_TRUE(next == 0 || next >= base + kX64HighArenaSpan)
        << "host mapping at 0x" << std::hex << next
        << " intersects the arena window 0x" << base << "-0x"
        << (base + kX64HighArenaSpan);
}

/// Relocation moves the block as a unit: the dynamic, thunk and synthetic
/// arenas keep their original order and 4 GiB spacing wherever they land.
TEST(X64HighArenaTest, DerivedBasesKeepOrderAndSpacing) {
    const u64 base = x64_high_arena_base();
    EXPECT_EQ(x64_dynamic_base(), base);
    EXPECT_EQ(x64_thunk_base(), base + kX64HighArenaStride);
    EXPECT_EQ(x64_synthetic_arena_base(), base + 2 * kX64HighArenaStride);
    EXPECT_LT(x64_dynamic_base(), x64_thunk_base());
    EXPECT_LT(x64_thunk_base(), x64_synthetic_arena_base());
}

/// The anchor is frozen after the first call. Process construction places the
/// arenas once; a later caller that disagreed would hand out module handles
/// pointing at a different window.
TEST(X64HighArenaTest, AnchorIsStableAcrossCalls) {
    EXPECT_EQ(x64_high_arena_base(), x64_high_arena_base());
}

/// Relocation only ever steps down from the preferred base, and never below
/// the floor -- a guest address must stay inside the 47-bit user window.
TEST(X64HighArenaTest, AnchorStaysWithinTheSearchRange) {
    const u64 base = x64_high_arena_base();
    EXPECT_LE(base, kX64HighArenaDefaultBase);
    EXPECT_GE(base, kX64HighArenaFloor);
    EXPECT_EQ(base % kX64HighArenaStride, 0u);
}

/// Inside a guest-base reservation the arenas take the same relative shape
/// they have below the 128 TiB ceiling: dynamic DLLs 24 GiB below the end,
/// thunks 20 GiB, synthetic modules 16 GiB -- and the whole 12 GiB span lies
/// inside the window, so every byte is reservation-backed.
TEST(X64HighArenaTest, InWindowAnchorKeepsTheCeilingRelativeShape) {
    for (const u64 window : {u64{1} << 46, u64{1} << 44, u64{1} << 42,
                             u64{1} << 40, u64{1} << 39}) {
        const u64 base = x64_high_arena_anchor_for(window);
        EXPECT_EQ(base, window - 0x6'00000000ULL) << std::hex << window;
        EXPECT_EQ(base % kX64HighArenaStride, 0u);
        EXPECT_LE(base + kX64HighArenaSpan, window);
    }
    // The 64 TiB production window puts dynamic DLLs at 0x3FFA'00000000.
    EXPECT_EQ(x64_high_arena_anchor_for(u64{1} << 46), 0x00003FFA'00000000ULL);
}

/// No reservation (or one too small to hold the arenas) keeps the host-probed
/// placement, so the scattered model is unchanged.
TEST(X64HighArenaTest, NoWindowKeepsTheHostProbedAnchor) {
    for (const u64 window : {u64{0}, kX64HighArenaMinWindow - 1}) {
        const u64 base = x64_high_arena_anchor_for(window);
        EXPECT_LE(base, kX64HighArenaDefaultBase);
        EXPECT_GE(base, kX64HighArenaFloor);
    }
}
