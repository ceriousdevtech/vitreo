/// @file test_vmem_reserve.cpp
/// @brief Unit tests for VirtualAlloc MEM_RESERVE/MEM_COMMIT state, reserved tails and first-fit address selection (WIN32-020)

#include "test_vmem_fixture.hpp"
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <chrono>
#include <vector>

TEST_F(VirtualMemoryTest, ReserveOnlyIsInaccessibleUntilSubrangeCommit) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x3000, MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x3000));
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr)).is_err());
    EXPECT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0x11).is_err());

    VirtualAllocationQuery reserved{};
    ASSERT_TRUE(vmm.virtual_query(addr, reserved));
    EXPECT_EQ(reserved.allocation_base, addr);
    EXPECT_EQ(reserved.allocation_protect, PAGE_READWRITE);
    EXPECT_EQ(reserved.state, MEM_RESERVE);
    EXPECT_EQ(reserved.protect, 0u);
    EXPECT_EQ(reserved.region_size, 0x3000u);

    ASSERT_EQ(vmm.virtual_alloc(
                  addr + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE),
              addr + 0x1000);
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr)).is_err());
    EXPECT_TRUE(mem.write_u8(
        static_cast<GuestAddr>(addr + 0x1000), 0x22).is_ok());
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr + 0x2000)).is_err());

    VirtualAllocationQuery first{};
    VirtualAllocationQuery middle{};
    VirtualAllocationQuery last{};
    ASSERT_TRUE(vmm.virtual_query(addr, first));
    ASSERT_TRUE(vmm.virtual_query(addr + 0x1000, middle));
    ASSERT_TRUE(vmm.virtual_query(addr + 0x2000, last));
    EXPECT_EQ(first.state, MEM_RESERVE);
    EXPECT_EQ(first.region_size, 0x1000u);
    EXPECT_EQ(middle.state, MEM_COMMIT);
    EXPECT_EQ(middle.protect, PAGE_READWRITE);
    EXPECT_EQ(middle.region_size, 0x1000u);
    EXPECT_EQ(last.state, MEM_RESERVE);
    EXPECT_EQ(last.region_size, 0x1000u);
}

// Learn: MEM_COMMIT of a reserved subrange zero-fills and makes that
// subrange accessible. The rest of the reservation stays MEM_RESERVE.
TEST(VirtualMemoryLinearTest, InteriorCommitGrowsReserved64kTail) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 page = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(page, 0u);
    ASSERT_EQ(manager.virtual_alloc(
                  page, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE),
              page);
    EXPECT_TRUE(memory.read_u8(static_cast<GuestAddr>(page + 0x1000)).is_err());

    ASSERT_EQ(manager.virtual_alloc(
                  page + 0x1000, 0xF000, MEM_COMMIT, PAGE_EXECUTE_READWRITE),
              page + 0x1000);
    ASSERT_TRUE(memory.write_u8(
        static_cast<GuestAddr>(page + 0xFC44), 0x5A).is_ok());

    const u64 next = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_EQ(next, page + 0x10000);
    EXPECT_EQ(memory.read_u8(
                  static_cast<GuestAddr>(page + 0xFC44)).value(),
              0x5A);
}

// Learn: reserved pages stay inaccessible until MEM_COMMIT. A later
// NULL reservation does not commit the previous slot's tail.
TEST(VirtualMemoryLinearTest, ReservedTailStaysInaccessibleAfterNextReserve) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 partial = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(partial, 0u);
    ASSERT_EQ(manager.virtual_alloc(
                  partial, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE),
              partial);

    const u64 next = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_EQ(next, partial + 0x10000);
    EXPECT_TRUE(memory.read_u8(
                    static_cast<GuestAddr>(next - 0x3BC)).is_err());

    VirtualAllocationQuery tail{};
    ASSERT_TRUE(manager.virtual_query(partial + 0xF000, tail));
    EXPECT_EQ(tail.state, MEM_RESERVE);
    EXPECT_EQ(tail.protect, 0u);
}

// Learn: VirtualAlloc(NULL, …, MEM_COMMIT, …) still creates a reservation,
// so the address is 64 KiB-aligned like MEM_RESERVE. Only the page-rounded
// request is committed; the rest of the paragraph stays a free hole
// (Software Verify) and must not be readable.
TEST(VirtualMemoryLinearTest, NullCommitOnlyIsAllocationGranularityAligned) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 first = manager.virtual_alloc(
        0, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    const u64 second = manager.virtual_alloc(
        0, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(first & 0xFFFFu, 0u);
    EXPECT_EQ(second, first + 0x10000);

    VirtualAllocationQuery span{};
    ASSERT_TRUE(manager.virtual_query(first, span));
    EXPECT_EQ(span.state, MEM_COMMIT);
    EXPECT_EQ(span.region_size, 0x1000u);
    EXPECT_TRUE(memory.read_u8(
                    static_cast<GuestAddr>(first + 0xFC44)).is_err());
    EXPECT_TRUE(memory.read_u8(
                    static_cast<GuestAddr>(second + 0x44 - 0x400)).is_err());
}

// Learn: NULL lpAddress is bottom-up first-fit. After a later reservation
// is taken, a new NULL alloc must reuse a released low gap rather than
// sit immediately after the most recent allocation.
TEST(VirtualMemoryLinearTest, NullAllocReusesLowestReleasedGap) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 low = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    const u64 high = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(low, 0u);
    ASSERT_EQ(high, low + 0x10000);
    ASSERT_TRUE(manager.virtual_free(low, 0, MEM_RELEASE));

    const u64 reused = manager.virtual_alloc(
        0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_EQ(reused, low);
    EXPECT_NE(reused, high + 0x10000);
}

// Learn (VirtualAlloc, SYSTEM_INFO::dwAllocationGranularity): a NULL
// lpAddress reserves at a 64 KiB-aligned base, bottom-up first-fit, and the
// size is only page-rounded. Every small reservation therefore leaves a free
// but unusable page-granular tail before the next 64 KiB boundary. The search
// must return exactly the lowest 64 KiB-aligned base whose whole range is free
// -- checked here against a brute-force scan -- and must not step through
// those tails one region at a time: UE's thousands of small reservations made
// that walk the largest runtime cost in Satisfactory (9-19% of all CPU).
TEST(VirtualMemoryLinearTest, NullAllocFirstFitAcrossGranularityTails) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x40000000));
    VirtualMemoryManager manager{memory, 0x3FFF'FFFF};
    constexpr u64 kGranularity = 0x10000;
    constexpr u64 kFirstFitStart = 0x00100000;  // VirtualAlloc's search base
    const auto brute = [&](u64 size) -> u64 {
        const u64 sz = (size + 0xFFFu) & ~u64{0xFFFu};
        const auto extents = memory.region_extents();
        u64 cand = kFirstFitStart;
        for (;;) {
            bool moved = false;
            for (const auto& [lo, hi] : extents) {
                if (cand < hi && lo < cand + sz) {
                    cand = (hi + kGranularity - 1) & ~(kGranularity - 1);
                    moved = true;
                }
            }
            if (!moved) return cand + sz <= 0x40000000 ? cand : 0;
        }
    };

    constexpr int kFragments = 6000;
    std::vector<u64> live;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFragments; ++i) {
        const u64 a = manager.virtual_alloc(0, 0x1000, MEM_RESERVE,
                                            PAGE_NOACCESS);
        ASSERT_NE(a, 0u) << "fragment " << i;
        ASSERT_EQ(a % kGranularity, 0u);
        if (!live.empty()) ASSERT_EQ(a, live.back() + kGranularity);
        live.push_back(a);
    }
    const auto t1 = std::chrono::steady_clock::now();

    u64 state = 0x9E3779B97F4A7C15ull;
    const auto next = [&](u64 bound) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return state % bound;
    };
    const u64 sizes[] = {0x1000, 0x3000, 0x10000, 0x11000, 0x40000};
    std::chrono::steady_clock::duration churn{};
    for (int step = 0; step < 1500; ++step) {
        if (next(3) != 0) {
            const u64 size = sizes[next(5)];
            const u64 expected = brute(size);
            const auto s = std::chrono::steady_clock::now();
            const u64 a = manager.virtual_alloc(0, size, MEM_RESERVE,
                                                PAGE_NOACCESS);
            churn += std::chrono::steady_clock::now() - s;
            ASSERT_EQ(a, expected) << "step " << step << " size " << size;
            live.push_back(a);
        } else {
            const usize victim = static_cast<usize>(next(live.size()));
            const auto s = std::chrono::steady_clock::now();
            ASSERT_TRUE(manager.virtual_free(live[victim], 0, MEM_RELEASE));
            churn += std::chrono::steady_clock::now() - s;
            live[victim] = live.back();
            live.pop_back();
        }
    }
    const auto ms = [](auto d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d)
            .count();
    };
    std::printf("[vmem-firstfit] %d fragments: %lld ms; 1500 churn ops: "
                "%lld ms\n",
                kFragments, static_cast<long long>(ms(t1 - t0)),
                static_cast<long long>(ms(churn)));
}

TEST_F(VirtualMemoryTest, CommitOutsideReservationFails) {
    EXPECT_EQ(vmm.virtual_alloc(
                  0x400000, 0x1000, MEM_COMMIT, PAGE_READWRITE),
              0u);
}

TEST_F(VirtualMemoryTest, CommitReserveCannotOverlapExistingReservation) {
    const u64 base = vmm.virtual_alloc(
        0x400000, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_EQ(base, 0x400000u);

    // Microsoft Learn: MEM_COMMIT|MEM_RESERVE simultaneously reserves and
    // commits free pages. MEM_RESERVE cannot reserve an already-reserved page.
    EXPECT_EQ(vmm.virtual_alloc(
                  base, 0x1000, MEM_COMMIT | MEM_RESERVE,
                  PAGE_EXECUTE_READWRITE),
              0u);

    VirtualAllocationQuery query{};
    ASSERT_TRUE(vmm.virtual_query(base, query));
    EXPECT_EQ(query.state, MEM_RESERVE);
    EXPECT_EQ(query.protect, 0u);
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(base)).is_err());
}

TEST(VirtualMemoryAddressLimitTest, AutoAllocReusesReleaseBelowProcessCeiling) {
    GuestMemory limited_memory;
    // Exactly one 64 KiB allocation-granularity slot beginning at 0x100000.
    VirtualMemoryManager limited{limited_memory, 0x0010FFFF};

    const u64 first = limited.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_EQ(first, 0x00100000u);

    // A second live reservation cannot cross the configured process ceiling.
    EXPECT_EQ(limited.virtual_alloc(
                  0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE),
              0u);

    ASSERT_TRUE(limited.virtual_free(first, 0, MEM_RELEASE));

    // MEM_RELEASE made the slot available again. Storm exercises this exact
    // reserve/commit/release pattern every frame.
    const u64 reused = limited.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    EXPECT_EQ(reused, first);
    EXPECT_LE(reused + 0xFFF, 0x0010FFFFu);
}

TEST(VirtualMemoryAddressLimitTest,
     Linear64CeilingFromGuestMemoryConstrainsFixedAllocations) {
    GuestMemory memory;
    constexpr GuestAddr kLinear64Ceiling = 0x0000007F'FFFFFFFFULL;
    memory.set_maximum_user_address(kLinear64Ceiling);

    VirtualMemoryManager manager{memory, memory.maximum_user_address()};
    constexpr GuestAddr kLastGranularitySlot =
        (kLinear64Ceiling + 1) - 0x10000;

    EXPECT_EQ(manager.virtual_alloc(
                  kLastGranularitySlot, 0x10000,
                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
              kLastGranularitySlot);
    EXPECT_TRUE(memory.is_mapped(kLastGranularitySlot, 0x10000));

    // The next allocation-granularity slot begins outside the reserved
    // linear64 guest window and must not be admitted by the runtime owner.
    EXPECT_EQ(manager.virtual_alloc(
                  kLinear64Ceiling + 1, 0x10000,
                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
              0u);
}

// Learn (VirtualAlloc lpAddress): a reservation at a caller-chosen address is
// placed exactly there (rounded down to the 64 KiB granularity) or fails when
// the range is not free. With the x64 reservation extension the high half of
// the 47-bit user space is inside the linear reservation, so such a request
// is still honoured in place -- and lands linearly backed (one host load from
// translated code), not in the scattered model.
TEST(VirtualMemoryLinearTest, FixedHighHalfReservationIsHonouredInTheExtension) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(usize{1} << 40,
                                                 /*extend_to_va_limit=*/true));
    if (!memory.linear_check_map_ends_at_va_limit()) {
        GTEST_SKIP() << "the VA limit is held by another reservation";
    }
    constexpr GuestAddr kUserCeiling = 0x00007FFF'FFFEFFFFull;
    memory.set_maximum_user_address(kUserCeiling);
    VirtualMemoryManager manager{memory, kUserCeiling};
    constexpr u64 kHint = 0x00005A1000000000ull;
    constexpr u64 kPool = u64{16} << 30;
    ASSERT_EQ(manager.virtual_alloc(kHint + 0x1234, kPool, MEM_RESERVE,
                                    PAGE_NOACCESS),
              kHint)
        << "a fixed reservation must be placed at the rounded-down address";
    EXPECT_TRUE(memory.linear_backs(static_cast<GuestAddr>(kHint), kPool));
    // Overlapping fixed request: the range is not free.
    EXPECT_EQ(manager.virtual_alloc(kHint + 0x10000, 0x10000, MEM_RESERVE,
                                    PAGE_NOACCESS),
              0u);
    // Reserved stays inaccessible; a committed subrange is read/write.
    EXPECT_TRUE(memory.read_u8(static_cast<GuestAddr>(kHint)).is_err());
    ASSERT_EQ(manager.virtual_alloc(kHint + 0x40000000, 0x4000, MEM_COMMIT,
                                    PAGE_READWRITE),
              kHint + 0x40000000);
    ASSERT_TRUE(memory.write_u32(static_cast<GuestAddr>(kHint + 0x40000010),
                                 0xA5A5F00Du).is_ok());
    u32 host = 0;
    std::memcpy(&host, memory.linear_base() + kHint + 0x40000010, 4);
    EXPECT_EQ(host, 0xA5A5F00Du);
    VirtualAllocationQuery q{};
    ASSERT_TRUE(manager.virtual_query(kHint + 0x40000000, q));
    EXPECT_EQ(q.allocation_base, kHint);
    EXPECT_EQ(q.state, MEM_COMMIT);
    EXPECT_EQ(q.protect, PAGE_READWRITE);
    // Release frees the whole reservation; the same fixed request succeeds.
    ASSERT_TRUE(manager.virtual_free(kHint, 0, MEM_RELEASE));
    EXPECT_EQ(manager.virtual_alloc(kHint, kPool, MEM_RESERVE, PAGE_NOACCESS),
              kHint);
}
