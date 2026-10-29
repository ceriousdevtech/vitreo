// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_identity_mapping.cpp
/// @brief Identity mapping and reserved holes (Intel native backend, P2).
///
/// Identity mapping is what a natively-executing backend needs: guest code
/// emits absolute addresses that the CPU consumes directly, so the guest must
/// live where it thinks it lives. The reserved-hole list records the ranges
/// the host process owns and will not give up -- measured on the target
/// machine as the main thread stack at 0x800000-0x1000000, which macOS places
/// low once __PAGEZERO is shrunk and which no linker flag can relocate.
///
/// The address arithmetic is host-architecture independent, so it is tested
/// on both backends.

#include <gtest/gtest.h>

#include "core/memory/guest_memory.hpp"

#include <cstring>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>

using namespace vitreo;

namespace {

// The measured low-memory layout on the Intel target. Used so the tests
// exercise the real shape rather than invented numbers.
constexpr GuestAddr kStackHoleLo = 0x00800000;
constexpr GuestAddr kStackHoleHi = 0x01000000;
constexpr GuestAddr kDyldHoleLo  = 0x00001000;
constexpr GuestAddr kDyldHoleHi  = 0x00061000;

/// The measured low-memory map of the Intel target (§5.1), reproduced exactly
/// so the tests answer the question the real machine asks.
void apply_measured_intel_holes(GuestMemory& mem) {
    mem.add_reserved_hole(kDyldHoleLo, kDyldHoleHi);   // dyld
    mem.add_reserved_hole(0x100000, 0x200000);
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi); // main thread stack
}

} // namespace

TEST(IdentityMappingTest, DefaultsToNeitherLinearNorIdentity) {
    GuestMemory mem;
    EXPECT_FALSE(mem.is_identity());
    EXPECT_FALSE(mem.is_linear());
}

TEST(IdentityMappingTest, EnableSucceedsOnAFreshInstance) {
    GuestMemory mem;
    EXPECT_TRUE(mem.enable_identity_mapping());
    EXPECT_TRUE(mem.is_identity());
    // is_linear() must report true so every existing linear fast path stays
    // engaged -- identity is a linear model with a base of zero.
    EXPECT_TRUE(mem.is_linear());
}

TEST(IdentityMappingTest, HostAddressEqualsGuestAddress) {
    GuestMemory mem;
    ASSERT_TRUE(mem.enable_identity_mapping());
    for (GuestAddr a : {GuestAddr{0x400000}, GuestAddr{0x140000000},
                        GuestAddr{0x7FFFFFFF}}) {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(mem.linear_host(a)),
                  static_cast<uintptr_t>(a));
    }
}

TEST(IdentityMappingTest, IsMutuallyExclusiveWithLinearReservation) {
    GuestMemory a;
    ASSERT_TRUE(a.enable_linear_reservation(1u << 20));
    EXPECT_FALSE(a.enable_identity_mapping());

    GuestMemory b;
    ASSERT_TRUE(b.enable_identity_mapping());
    EXPECT_FALSE(b.enable_linear_reservation(1u << 20));
}

TEST(IdentityMappingTest, CannotEnableTwice) {
    GuestMemory mem;
    ASSERT_TRUE(mem.enable_identity_mapping());
    EXPECT_FALSE(mem.enable_identity_mapping());
}

TEST(IdentityMappingTest, EnableFailsOnceARegionExists) {
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite));
    EXPECT_FALSE(mem.enable_identity_mapping());
}

// ---------------------------------------------------------------------------
// Reserved holes
// ---------------------------------------------------------------------------

TEST(ReservedHoleTest, NoHolesMeansNothingOverlaps) {
    GuestMemory mem;
    EXPECT_FALSE(mem.overlaps_reserved_hole(0, 0x100000000ull));
}

TEST(ReservedHoleTest, DetectsEveryOverlapShape) {
    GuestMemory mem;
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);

    EXPECT_TRUE(mem.overlaps_reserved_hole(kStackHoleLo, 0x1000));       // at start
    EXPECT_TRUE(mem.overlaps_reserved_hole(kStackHoleHi - 0x1000, 0x1000)); // at end
    EXPECT_TRUE(mem.overlaps_reserved_hole(0x900000, 0x1000));           // inside
    EXPECT_TRUE(mem.overlaps_reserved_hole(0x400000, 0x800000));         // spanning
    EXPECT_TRUE(mem.overlaps_reserved_hole(kStackHoleLo - 1, 2));        // straddles lo
}

TEST(ReservedHoleTest, AdjacentRangesDoNotOverlap) {
    GuestMemory mem;
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);

    // [lo, hi) is half-open: ending exactly at lo, or starting exactly at hi,
    // must both be allowed. Getting this wrong silently costs a usable page.
    EXPECT_FALSE(mem.overlaps_reserved_hole(kStackHoleLo - 0x1000, 0x1000));
    EXPECT_FALSE(mem.overlaps_reserved_hole(kStackHoleHi, 0x1000));
}

TEST(ReservedHoleTest, ZeroSizeNeverOverlaps) {
    GuestMemory mem;
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);
    EXPECT_FALSE(mem.overlaps_reserved_hole(0x900000, 0));
}

TEST(ReservedHoleTest, EmptyAndInvertedRangesAreIgnored) {
    GuestMemory mem;
    mem.add_reserved_hole(0x1000, 0x1000);   // empty
    mem.add_reserved_hole(0x5000, 0x2000);   // hi < lo
    EXPECT_FALSE(mem.overlaps_reserved_hole(0x1000, 0x8000));
}

TEST(ReservedHoleTest, OverlappingHolesCoalesce) {
    GuestMemory mem;
    mem.add_reserved_hole(0x100000, 0x300000);
    mem.add_reserved_hole(0x200000, 0x400000);   // overlaps the first
    EXPECT_TRUE(mem.overlaps_reserved_hole(0x150000, 0x1000));
    EXPECT_TRUE(mem.overlaps_reserved_hole(0x350000, 0x1000));
    EXPECT_FALSE(mem.overlaps_reserved_hole(0x400000, 0x1000));
}

TEST(ReservedHoleTest, HolesAddedOutOfOrderStaySorted) {
    GuestMemory mem;
    // Reverse order on purpose: overlaps_reserved_hole() relies on sortedness
    // to stop early, so an unsorted insert would make it miss later holes.
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);
    mem.add_reserved_hole(kDyldHoleLo, kDyldHoleHi);

    EXPECT_TRUE(mem.overlaps_reserved_hole(kDyldHoleLo, 0x1000));
    EXPECT_TRUE(mem.overlaps_reserved_hole(kStackHoleLo, 0x1000));
    EXPECT_FALSE(mem.overlaps_reserved_hole(0x200000, 0x100000));
}

// ---------------------------------------------------------------------------
// Relocation search
// ---------------------------------------------------------------------------

TEST(ReservedHoleTest, ClearAddressIsReturnedUnchanged) {
    GuestMemory mem;
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);
    EXPECT_EQ(mem.next_address_clear_of_holes(0x200000, 0x1000),
              GuestAddr{0x200000});
}

TEST(ReservedHoleTest, CollidingAddressMovesPastTheHole) {
    GuestMemory mem;
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);
    // A PE based at 0x400000 that is large enough to run into the stack must
    // be told to rebase above it, not merely refused.
    EXPECT_EQ(mem.next_address_clear_of_holes(0x400000, 0x600000),
              kStackHoleHi);
}

TEST(ReservedHoleTest, SearchSkipsConsecutiveHoles) {
    GuestMemory mem;
    mem.add_reserved_hole(0x100000, 0x200000);
    mem.add_reserved_hole(0x200000, 0x300000);
    mem.add_reserved_hole(0x400000, 0x500000);

    // Must clear all three, including the pair that coalesced into one.
    const GuestAddr got = mem.next_address_clear_of_holes(0x100000, 0x180000);
    EXPECT_EQ(got, GuestAddr{0x500000});
    EXPECT_FALSE(mem.overlaps_reserved_hole(got, 0x180000));
}

TEST(ReservedHoleTest, TypicalWin32ImageStillFitsBelowTheStack) {
    GuestMemory mem;
    mem.add_reserved_hole(kDyldHoleLo, kDyldHoleHi);
    mem.add_reserved_hole(0x100000, 0x200000);
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);

    // The measured 6MB gap at 0x200000-0x800000 is what lets a classic
    // 0x400000-based Win32 image load at its preferred base. Guard the
    // property that motivated accepting the immovable stack at all.
    EXPECT_FALSE(mem.overlaps_reserved_hole(0x400000, 0x400000));
    EXPECT_EQ(mem.next_address_clear_of_holes(0x400000, 0x400000),
              GuestAddr{0x400000});
}

// ---------------------------------------------------------------------------
// Host occupancy capture
// ---------------------------------------------------------------------------

TEST(HostOccupancyTest, DoesNothingWithoutIdentityMapping) {
    // The translated (ARM) path must never acquire holes: they would wall off
    // guest addresses that are perfectly usable behind a translation base.
    GuestMemory mem;
    EXPECT_EQ(mem.capture_host_occupancy_below_4gb(), 0u);
    EXPECT_TRUE(mem.reserved_holes().empty());
}

TEST(HostOccupancyTest, RefusesOnceARegionExists) {
    GuestMemory mem;
    ASSERT_TRUE(mem.enable_identity_mapping());

    // Identity mapping means host address == guest address, so the scan cannot
    // tell our own region from a host occupant. Capturing late would reserve
    // the guest's own memory against it.
    //
    // The guest address has to be one the host has already claimed — that is
    // the whole premise of identity mapping — so let the kernel pick a free
    // host range and use it as the guest address. It is deliberately not
    // munmap'd: identity map_region commits by mprotect inside an existing
    // reservation.
    void* host = mmap(nullptr, 0x1000, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    const auto addr = static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(host));
    ASSERT_TRUE(mem.map_region(addr, 0x1000, MemoryProtection::ReadWrite));

    EXPECT_EQ(mem.capture_host_occupancy_below_4gb(), 0u);
    EXPECT_TRUE(mem.reserved_holes().empty());
}

TEST(HostOccupancyTest, RecordsSortedDisjointHoles) {
    GuestMemory mem;
    ASSERT_TRUE(mem.enable_identity_mapping());
    const usize recorded = mem.capture_host_occupancy_below_4gb();
    ASSERT_GT(recorded, 0u);   // every Mach-O has at least __PAGEZERO

    const auto holes = mem.reserved_holes();
    ASSERT_FALSE(holes.empty());
    GuestAddr prev_hi = 0;
    for (const auto& [lo, hi] : holes) {
        EXPECT_LT(lo, hi);
        EXPECT_GE(lo, prev_hi);           // sorted and disjoint after coalescing
        EXPECT_LE(hi, GuestAddr{0x100000000ull});   // clipped to 4 GiB
        prev_hi = hi;
    }
}

TEST(HostOccupancyTest, ThisTestBinarysPageZeroCoversTheGuestSpace) {
    // The test executable is linked normally, so it keeps the default 4 GiB
    // __PAGEZERO — the exact condition §5.1 identifies as the reason identity
    // mapping looks like a kernel restriction. Asserting it here keeps the
    // capture honest: it must report the low addresses as unavailable rather
    // than assuming a flat 4 GiB.
    GuestMemory mem;
    ASSERT_TRUE(mem.enable_identity_mapping());
    ASSERT_GT(mem.capture_host_occupancy_below_4gb(), 0u);
    EXPECT_TRUE(mem.overlaps_reserved_hole(0x400000, 0x1000));
}

// ---------------------------------------------------------------------------
// Allocation must also avoid the holes
// ---------------------------------------------------------------------------

TEST(ReservedHoleTest, FindFreeGapSkipsReservedHoles) {
    GuestMemory mem;
    apply_measured_intel_holes(mem);
    // VirtualAlloc searching from a low address must not be handed the main
    // thread stack; map_region would then refuse and the guest would see an
    // allocation failure with no explanation.
    const GuestAddr got = mem.find_free_gap(0x700000, 0x200000);
    ASSERT_NE(got, GuestAddr{0});
    EXPECT_FALSE(mem.overlaps_reserved_hole(got, 0x200000));
    EXPECT_GE(got, kStackHoleHi);
}

TEST(ReservedHoleTest, FindFreeGapClearsHolesAndRegionsTogether) {
    GuestMemory mem;
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi);
    // A region immediately after the hole: clearing the hole must not stop the
    // scan from then clearing the region.
    ASSERT_TRUE(mem.map_region(kStackHoleHi, 0x100000,
                               MemoryProtection::ReadWrite));

    const GuestAddr got = mem.find_free_gap(0x900000, 0x100000);
    ASSERT_NE(got, GuestAddr{0});
    EXPECT_FALSE(mem.overlaps_reserved_hole(got, 0x100000));
    EXPECT_GE(got, kStackHoleHi + 0x100000);
}

TEST(ReservedHoleTest, FindFreeGapReturnsZeroWhenEverythingIsReserved) {
    GuestMemory mem;
    mem.add_reserved_hole(0x1000, 0xFFFF0000);
    EXPECT_EQ(mem.find_free_gap(0x10000, 0x1000), GuestAddr{0});
}

// ---------------------------------------------------------------------------
// Identity commit must not memset host pages
// ---------------------------------------------------------------------------

TEST(IdentityMappingTest, ReserveThenCommitMakesPagesWritable) {
    // The RCT3 crash: VirtualAlloc MEM_RESERVE of a span, then MEM_COMMIT of
    // a 64 KiB run inside it. identity_host_protect used to return success
    // after a failed mprotect, and the subsequent memset SIGSEGV'd.
    constexpr usize kSpan = 0x20000;
    void* host = mmap(nullptr, kSpan, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    const auto addr = static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(host));

    {
        GuestMemory mem;
        ASSERT_TRUE(mem.enable_identity_mapping());
        ASSERT_TRUE(mem.map_region(addr, kSpan, MemoryProtection::None,
                                   /*committed=*/false));
        ASSERT_TRUE(mem.commit_region(addr + 0x10000, 0x10000,
                                      MemoryProtection::ReadWrite));
        Byte* p = mem.host_ptr_mut(addr + 0x10000, 0x10000);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x5A, 0x10000);
        EXPECT_EQ(p[0], 0x5A);
        EXPECT_EQ(p[0xFFFF], 0x5A);
    }
    munmap(host, kSpan);
}

TEST(IdentityMappingTest, RefusesToStealHostWritablePages) {
    void* host = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    std::memset(host, 0xCC, 0x1000);
    const auto addr = static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(host));

    GuestMemory mem;
    ASSERT_TRUE(mem.enable_identity_mapping());
    EXPECT_FALSE(mem.map_region(addr, 0x1000, MemoryProtection::ReadWrite));
    EXPECT_EQ(static_cast<unsigned char*>(host)[0], 0xCC);
    munmap(host, 0x1000);
}

TEST(IdentityMappingTest, RefusesHostMallocTinyAndRecordsAHole) {
    // libmalloc tiny/guard pages are PROT_NONE with a non-guest tag. A new
    // reservation used to adopt them, then MEM_COMMIT mprotect/memset crashed
    // or returned ERROR_NOT_ENOUGH_MEMORY. The span must be refused and
    // recorded as a reserved hole so the next search skips it.
    constexpr mach_vm_size_t kSpan = 0x10000;
    mach_vm_address_t addr = 0;
    const int flags = VM_FLAGS_ANYWHERE |
                      VM_MAKE_TAG(static_cast<int>(VM_MEMORY_MALLOC_TINY));
    ASSERT_EQ(mach_vm_allocate(mach_task_self(), &addr, kSpan, flags),
              KERN_SUCCESS);
    (void)mach_vm_protect(mach_task_self(), addr, kSpan, FALSE, VM_PROT_NONE);

    {
        GuestMemory mem;
        ASSERT_TRUE(mem.enable_identity_mapping());
        EXPECT_FALSE(mem.map_region(static_cast<GuestAddr>(addr),
                                    static_cast<usize>(kSpan),
                                    MemoryProtection::None,
                                    /*committed=*/false));
        EXPECT_TRUE(mem.overlaps_reserved_hole(static_cast<GuestAddr>(addr),
                                               static_cast<usize>(kSpan)));
        const GuestAddr gap = mem.find_free_gap(
            static_cast<GuestAddr>(addr), static_cast<usize>(kSpan));
        EXPECT_TRUE(gap == 0 || gap >= static_cast<GuestAddr>(addr + kSpan) ||
                    gap + kSpan <= static_cast<GuestAddr>(addr));
    }
    (void)mach_vm_deallocate(mach_task_self(), addr, kSpan);
}

TEST(IdentityMappingTest, UnmapRestoresReservation) {
    // munmap used to hand the VA back to the host. The next map at the same
    // address then hit a foreign occupant (ERROR_NOT_ENOUGH_MEMORY).
    constexpr usize kSpan = 0x10000;
    void* host = mmap(nullptr, kSpan, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    const auto addr = static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(host));

    {
        GuestMemory mem;
        ASSERT_TRUE(mem.enable_identity_mapping());
        ASSERT_TRUE(mem.map_region(addr, kSpan, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.unmap_region(addr, kSpan));
        ASSERT_TRUE(mem.map_region(addr, kSpan, MemoryProtection::ReadWrite));
        Byte* p = mem.host_ptr_mut(addr, kSpan);
        ASSERT_NE(p, nullptr);
        p[0] = 0x5A;
        EXPECT_EQ(p[0], 0x5A);
    }
    (void)mach_vm_deallocate(mach_task_self(),
                             static_cast<mach_vm_address_t>(addr), kSpan);
}

TEST(IdentityMappingTest, BackedViewUnmapRestoresReservation) {
    constexpr usize kSpan = 0x10000;
    void* placeholder = mmap(nullptr, kSpan, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* source = mmap(nullptr, kSpan, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(placeholder, MAP_FAILED);
    ASSERT_NE(source, MAP_FAILED);
    std::memset(source, 0x3C, kSpan);
    const auto addr =
        static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(placeholder));

    {
        GuestMemory mem;
        ASSERT_TRUE(mem.enable_identity_mapping());
        ASSERT_TRUE(mem.map_region_backed(addr, kSpan, source,
                                          MemoryProtection::ReadWrite));
        Byte* view = mem.host_ptr_mut(addr, kSpan);
        ASSERT_NE(view, nullptr);
        EXPECT_EQ(view[0], static_cast<Byte>(0x3C));
        view[0] = 0x7E;
        EXPECT_EQ(static_cast<unsigned char*>(source)[0], 0x7E);
        ASSERT_TRUE(mem.unmap_region(addr, kSpan));
        ASSERT_TRUE(mem.map_region(addr, kSpan, MemoryProtection::ReadWrite));
        Byte* p = mem.host_ptr_mut(addr, kSpan);
        ASSERT_NE(p, nullptr);
        p[0] = 0x5A;
        EXPECT_EQ(p[0], 0x5A);
        EXPECT_EQ(static_cast<unsigned char*>(source)[0], 0x7E);
    }
    (void)mach_vm_deallocate(mach_task_self(),
                             static_cast<mach_vm_address_t>(addr), kSpan);
    munmap(source, kSpan);
}

TEST(IdentityMappingTest, CommitRefillsUnmappedHoleInReservation) {
    constexpr usize kSpan = 0x20000;
    void* host = mmap(nullptr, kSpan, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    const auto addr = static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(host));

    {
        GuestMemory mem;
        ASSERT_TRUE(mem.enable_identity_mapping());
        ASSERT_TRUE(mem.map_region(addr, kSpan, MemoryProtection::None,
                                   /*committed=*/false));
        ASSERT_EQ(munmap(host, 0x10000), 0);
        ASSERT_TRUE(mem.commit_region(addr, 0x10000,
                                      MemoryProtection::ReadWrite));
        Byte* p = mem.host_ptr_mut(addr, 0x10000);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x5A, 0x10000);
        EXPECT_EQ(p[0], 0x5A);
        EXPECT_EQ(p[0xFFFF], 0x5A);
    }
    (void)mach_vm_deallocate(mach_task_self(),
                             static_cast<mach_vm_address_t>(addr), kSpan);
}
