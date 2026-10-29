// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// enable_linear_reservation() backs every region at linear_base()+guest_addr
// inside one contiguous host reservation, instead of scattered host
// allocations. The existing software bounds checks stay, so this must
// be behaviorally identical to the classic model — same map/protect/unmap
// results, same host contents, same read/write semantics. These tests assert
// that parity, plus the linear-only invariant (host pointer == base + addr).

#include <gtest/gtest.h>

#include <cstring>
#include <sys/mman.h>
#include <mach/mach_vm.h>
#include "core/memory/guest_memory.hpp"

using namespace vitreo;

namespace {

constexpr GuestAddr kA = 0x00400000;   // typical 32-bit image base
constexpr GuestAddr kB = 0x10000000;
constexpr usize     kSz = 0x2000;
constexpr usize     kReservation = usize{4} * 1024 * 1024 * 1024;  // 4 GiB

// Run the same sequence of operations against both models and compare.
class LinearParityTest : public ::testing::Test {
protected:
    GuestMemory scattered;   // classic model
    GuestMemory linear;      // reservation-backed

    void SetUp() override {
        ASSERT_TRUE(linear.enable_linear_reservation(kReservation));
        ASSERT_TRUE(linear.is_linear());
        ASSERT_FALSE(scattered.is_linear());
    }

    // Apply `op` to both and require identical VoidResult success/failure.
    template <class Op>
    void both(Op op, const char* what) {
        const bool s = op(scattered).is_ok();
        const bool l = op(linear).is_ok();
        EXPECT_EQ(s, l) << what << ": scattered ok=" << s << " linear ok=" << l;
    }
};

TEST_F(LinearParityTest, MapReadWriteProtectUnmapParity) {
    both([](GuestMemory& m) { return m.map_region(kA, kSz, MemoryProtection::ReadWrite); },
         "map A");
    both([](GuestMemory& m) { return m.map_region(kB, kSz, MemoryProtection::ReadWrite); },
         "map B");

    // Writes then reads must round-trip identically.
    const u8 pat[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_TRUE(scattered.write_bytes(kA + 0x100, pat, 8));
    EXPECT_TRUE(linear.write_bytes(kA + 0x100, pat, 8));
    u8 sbuf[8], lbuf[8];
    EXPECT_TRUE(scattered.read_bytes(kA + 0x100, sbuf, 8));
    EXPECT_TRUE(linear.read_bytes(kA + 0x100, lbuf, 8));
    EXPECT_EQ(0, std::memcmp(sbuf, lbuf, 8));
    EXPECT_EQ(0, std::memcmp(sbuf, pat, 8));

    // Newly mapped memory is zero in both.
    u8 z_s = 0xFF, z_l = 0xFF;
    EXPECT_TRUE(scattered.read_bytes(kB + 0x40, &z_s, 1));
    EXPECT_TRUE(linear.read_bytes(kB + 0x40, &z_l, 1));
    EXPECT_EQ(z_s, 0u);
    EXPECT_EQ(z_l, 0u);

    // Protect read-only, then a write must fail in BOTH.
    both([](GuestMemory& m) { return m.protect_region(kA, kSz, MemoryProtection::Read); },
         "protect A read-only");
    EXPECT_FALSE(scattered.write_bytes(kA + 0x100, pat, 8));
    EXPECT_FALSE(linear.write_bytes(kA + 0x100, pat, 8));

    // Back to RW: writes succeed again in both.
    both([](GuestMemory& m) { return m.protect_region(kA, kSz, MemoryProtection::ReadWrite); },
         "protect A rw");
    EXPECT_TRUE(scattered.write_bytes(kA + 0x100, pat, 8));
    EXPECT_TRUE(linear.write_bytes(kA + 0x100, pat, 8));

    // Unmap, then access must fail in both.
    both([](GuestMemory& m) { return m.unmap_region(kA, kSz); }, "unmap A");
    EXPECT_FALSE(scattered.read_bytes(kA + 0x100, sbuf, 8));
    EXPECT_FALSE(linear.read_bytes(kA + 0x100, lbuf, 8));

    // The other region is still intact in both.
    EXPECT_TRUE(scattered.read_bytes(kB + 0x40, &z_s, 1));
    EXPECT_TRUE(linear.read_bytes(kB + 0x40, &z_l, 1));
}

// The defining linear invariant: the host pointer for a guest address is
// exactly linear_base() + addr — no region lookup required.
TEST_F(LinearParityTest, HostPointerIsBasePlusAddr) {
    ASSERT_TRUE(linear.map_region(kA, kSz, MemoryProtection::ReadWrite));
    auto r = linear.translate(kA + 0x123);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), linear.linear_base() + kA + 0x123);
}

// A remap after unmap must work (decommit restored the reservation slot).
TEST_F(LinearParityTest, RemapAfterUnmap) {
    ASSERT_TRUE(linear.map_region(kA, kSz, MemoryProtection::ReadWrite));
    const u8 v = 0x5A;
    ASSERT_TRUE(linear.write_bytes(kA, &v, 1));
    ASSERT_TRUE(linear.unmap_region(kA, kSz));
    // Re-map the same guest address; must be fresh and zeroed, not stale.
    ASSERT_TRUE(linear.map_region(kA, kSz, MemoryProtection::ReadWrite));
    u8 got = 0xFF;
    ASSERT_TRUE(linear.read_bytes(kA, &got, 1));
    EXPECT_EQ(got, 0u) << "remapped page retained stale data";
}

// Apple Silicon uses 16 KiB host pages while Windows allocations are 4 KiB
// granular. Freeing one guest allocation must not deprotect or discard a live
// adjacent allocation that shares the same host page.
TEST_F(LinearParityTest, UnmapPreservesAdjacentGuestOwnerOnSharedHostPage) {
    const usize host_page = static_cast<usize>(::getpagesize());
    if (host_page <= GUEST_PAGE_SIZE) {
        GTEST_SKIP() << "host page does not contain multiple guest pages";
    }

    const GuestAddr first = 0x00100000;  // host-page aligned
    const GuestAddr second = first + GUEST_PAGE_SIZE;
    ASSERT_TRUE(linear.map_region(first, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(linear.map_region(second, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));

    const u8 marker = 0xA5;
    ASSERT_TRUE(linear.write_bytes(first + 0x10, &marker, 1));
    ASSERT_TRUE(linear.unmap_region(second, GUEST_PAGE_SIZE));

    u8 observed = 0;
    ASSERT_TRUE(linear.read_bytes(first + 0x10, &observed, 1));
    EXPECT_EQ(observed, marker)
        << "unmapping a neighbour discarded bytes owned by the live region";

    const u8 replacement = 0x5A;
    EXPECT_TRUE(linear.write_bytes(first + 0x10, &replacement, 1));
    ASSERT_TRUE(linear.read_bytes(first + 0x10, &observed, 1));
    EXPECT_EQ(observed, replacement);
}

TEST_F(LinearParityTest, NoAccessPreservesAdjacentGuestOwnerOnSharedHostPage) {
    const usize host_page = static_cast<usize>(::getpagesize());
    if (host_page <= GUEST_PAGE_SIZE) {
        GTEST_SKIP() << "host page does not contain multiple guest pages";
    }

    const GuestAddr first = 0x00100000;  // host-page aligned
    const GuestAddr second = first + GUEST_PAGE_SIZE;
    ASSERT_TRUE(linear.map_region(first, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(linear.map_region(second, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));

    const u8 marker = 0x3C;
    ASSERT_TRUE(linear.write_bytes(first + 0x20, &marker, 1));
    ASSERT_TRUE(linear.protect_region(second, GUEST_PAGE_SIZE,
                                      MemoryProtection::None));

    u8 observed = 0;
    ASSERT_TRUE(linear.read_bytes(first + 0x20, &observed, 1));
    EXPECT_EQ(observed, marker);
    EXPECT_FALSE(linear.read_bytes(second, &observed, 1))
        << "software permissions must still reject the PAGE_NOACCESS region";
}

// PAGE_NOACCESS reserve-only mapping must not be readable/writable, in both
// models (the lazy-commit contract that keeps PartitionAlloc from OOMing).
TEST_F(LinearParityTest, ReserveOnlyIsInaccessible) {
    both([](GuestMemory& m) {
             return m.map_region(
                 kA, kSz, MemoryProtection::None, /*committed=*/false);
         },
         "reserve A");
    u8 b;
    EXPECT_FALSE(scattered.read_bytes(kA, &b, 1));
    EXPECT_FALSE(linear.read_bytes(kA, &b, 1));
    // Commit it RW and it becomes accessible in both.
    both([](GuestMemory& m) {
             return m.commit_region(
                 kA, kSz, MemoryProtection::ReadWrite);
         },
         "commit A");
    EXPECT_TRUE(scattered.read_bytes(kA, &b, 1));
    EXPECT_TRUE(linear.read_bytes(kA, &b, 1));
}

// Gap #3: a backed region (MapViewOfFile / shared memory) must be aliased into
// the reservation so the guest reaches it at linear_base+addr AND writes stay
// mutually visible through the original shared mapping (mach_vm_remap, no copy).
TEST(LinearBackedMemory, RemapAliasesSharedPages) {
    GuestMemory m;
    ASSERT_TRUE(m.enable_linear_reservation(kReservation));

    // A shared host buffer standing in for cross-process shared memory.
    const usize sz = 0x8000;  // 32 KiB, 16 KiB-multiple
    void* host = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    std::memset(host, 0xAB, sz);  // peer already wrote data

    ASSERT_TRUE(m.map_region_backed(kA, sz, host, MemoryProtection::ReadWrite));

    // Guest sees the peer's pre-existing data (not zeroed).
    u8 seen = 0;
    ASSERT_TRUE(m.read_bytes(kA + 0x10, &seen, 1));
    EXPECT_EQ(seen, 0xABu);

    // The guest reaches it at linear_base + addr.
    auto t = m.translate(kA + 0x10);
    ASSERT_TRUE(t.is_ok());
    EXPECT_EQ(t.value(), m.linear_base() + kA + 0x10);

    // A guest write is visible through the ORIGINAL shared mapping — proving a
    // true alias (shared physical pages), not a copy.
    const u8 w = 0xCD;
    ASSERT_TRUE(m.write_bytes(kA + 0x20, &w, 1));
    EXPECT_EQ(static_cast<u8*>(host)[0x20], 0xCDu)
        << "write through the guest alias was not visible in the shared mapping";

    // And a peer write through host is visible to the guest.
    static_cast<u8*>(host)[0x30] = 0xEF;
    ASSERT_TRUE(m.read_bytes(kA + 0x30, &seen, 1));
    EXPECT_EQ(seen, 0xEFu);

    munmap(host, sz);
}

// Windows guarantees freshly-committed memory reads as zero. When a guest
// address is released (unmap) and later re-mapped, the reused pages MUST read
// back zero — never the previous occupant's data. This is the fix for the
// lazy-MADV_FREE stale-data corruption (garbage high bits in a reused pointer
// slot). Exercised in the linear model, where the reservation is reused in place
// rather than a fresh OS mmap.
TEST(LinearReservationReuse, ReleasedRangeReadsZeroOnReserveCommitReuse) {
    GuestMemory m;
    ASSERT_TRUE(m.enable_linear_reservation(kReservation));

    // Commit a page and stamp it with a recognizable non-zero pattern,
    // including the "garbage high bits" shape seen corrupting real pointers.
    ASSERT_TRUE(m.map_region(kA, kSz, MemoryProtection::ReadWrite));
    for (usize off = 0; off < kSz; off += 8) {
        const u64 poison = 0x000065D8024889B4ull;
        ASSERT_TRUE(m.write_bytes(kA + off, reinterpret_cast<const u8*>(&poison), 8));
    }

    // Release it (MEM_RELEASE path → linear reconcile drops the pages).
    ASSERT_TRUE(m.unmap_region(kA, kSz).is_ok());

    // Reuse via the PartitionAlloc lifecycle: RESERVE the address as
    // PROT_NONE (no memset — the whole point of a cheap reservation), then
    // COMMIT it by flipping protection to RW (protect_region also does NOT
    // memset). Only guaranteed zero-on-reuse in unmap makes this read zero;
    // with the old lazy MADV_FREE the stale poison reappears when the page is
    // reused before the kernel reclaims it.
    ASSERT_TRUE(m.map_region(
        kA, kSz, MemoryProtection::None, /*committed=*/false).is_ok());
    ASSERT_TRUE(m.commit_region(
        kA, kSz, MemoryProtection::ReadWrite).is_ok());

    for (usize off = 0; off < kSz; off += 8) {
        u64 v = 0xdead;
        ASSERT_TRUE(m.read_bytes(kA + off, reinterpret_cast<u8*>(&v), 8));
        EXPECT_EQ(v, 0u) << "stale data at kA+0x" << std::hex << off
                         << " after release+reserve+commit reuse (0x" << v << ")";
    }
}

// enable_linear_reservation must reject being turned on after regions exist.
TEST(LinearReservationLifecycle, RejectsEnableAfterMapping) {
    GuestMemory m;
    ASSERT_TRUE(m.map_region(kA, kSz, MemoryProtection::ReadWrite));
    EXPECT_FALSE(m.enable_linear_reservation(kReservation));
    EXPECT_FALSE(m.is_linear());
}

}  // namespace

// ---------------------------------------------------------------------------
// kLinearCheckMixedRead — the bit a LOAD gates on
//
// `kLinearCheckMixedProt` means the 4 KiB guest pages inside one 16 KiB host
// page disagree, so no single hardware protection is exact. Loads used to gate
// on it, which was far broader than reads need: the usual disagreement is about
// WRITABILITY (a PE's 4 KiB SectionAlignment puts .rdata beside .data in one
// host page), and such a page is uniformly READABLE. Measured on sc_blocksize
// rung 1, that sent 686,080,048 reads to the helper — 100% of the workload's
// helper reads — and removing it took the rung from 13.85s to 8.83s.
//
// The bit must therefore be SET whenever hardware could admit a read the guest
// denies, and only then. Under-reporting it is a silent wrong-execution bug:
// a load would bypass the software check and read a page the guest forbids.
// Every test here is written to catch that direction.
// ---------------------------------------------------------------------------
namespace {

// The check-map byte for `addr`, or 0 when the map is absent.
u8 linear_check_byte(const GuestMemory& m, GuestAddr addr) {
    const u8* map = m.linear_check_page_map();
    if (map == nullptr) return 0;
    const usize i = static_cast<usize>(addr) >> vitreo::kLinearHostPageShift;
    if (i >= m.linear_check_page_count()) return 0;
    return __atomic_load_n(&map[i], __ATOMIC_ACQUIRE);
}

}  // namespace

/// The read bit only exists when hardware enforces protection inside the
/// reservation; LinearParityTest deliberately does not turn that on.
class LinearReadGateTest : public ::testing::Test {
protected:
    GuestMemory linear;
    void SetUp() override {
        ASSERT_TRUE(linear.enable_linear_reservation(kReservation));
        if (!linear.enable_linear_hardware_protection()) {
            GTEST_SKIP() << "hardware protection unavailable on this host";
        }
        ASSERT_TRUE(linear.linear_hardware_protection());
        if (static_cast<usize>(::getpagesize()) <= GUEST_PAGE_SIZE) {
            GTEST_SKIP() << "host page does not contain multiple guest pages";
        }
    }
};

TEST_F(LinearReadGateTest, MixedWritabilityDoesNotMakeAPageReadUnsafe) {
    // .rdata beside .data: read-only next to read-write in one host page.
    //
    // The WHOLE host page is mapped on purpose. Leaving part of it unmapped
    // would make the page genuinely read-unsafe (the hardware page grants READ
    // across all 16 KiB, so a read of the gap would wrongly succeed), and an
    // earlier draft of this test did exactly that and "failed" on correct
    // conservative behaviour.
    const usize hp = static_cast<usize>(::getpagesize());
    const GuestAddr ro = 0x00200000;
    const GuestAddr rw = ro + GUEST_PAGE_SIZE;
    ASSERT_TRUE(linear.map_region(ro, GUEST_PAGE_SIZE, MemoryProtection::Read));
    ASSERT_TRUE(linear.map_region(rw, hp - GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));

    const u8 byte = linear_check_byte(linear, ro);
    EXPECT_NE(byte & kLinearCheckMixedProt, 0)
        << "a page mixing R and RW is still mixed for WRITES";
    EXPECT_EQ(byte & kLinearCheckMixedRead, 0)
        << "a page whose guest pages are all READABLE must not gate loads: "
           "this is the case that costs 686 M helper reads when it does";
}

TEST_F(LinearReadGateTest, AnUnreadablePageMakesItsHostPageReadUnsafe) {
    // Readable page beside a PAGE_NOACCESS one. Hardware must grant READ for
    // the first, which would otherwise admit a read of the second.
    const usize hp = static_cast<usize>(::getpagesize());
    const GuestAddr readable = 0x00300000;
    const GuestAddr noaccess = readable + GUEST_PAGE_SIZE;
    ASSERT_TRUE(linear.map_region(readable, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));
    // Rest of the host page is NOACCESS, so the page is fully mapped and the
    // ONLY read hazard is the protection -- not an unmapped gap.
    ASSERT_TRUE(linear.map_region(noaccess, hp - GUEST_PAGE_SIZE,
                                  MemoryProtection::None));

    EXPECT_NE(linear_check_byte(linear, readable) & kLinearCheckMixedRead, 0)
        << "a host page holding a NOACCESS guest page must gate loads, or a "
           "load reads memory the guest forbids";
    // And the guest-visible answer must still be a refusal.
    u8 sink = 0;
    EXPECT_FALSE(linear.read_bytes(noaccess, &sink, 1).is_ok())
        << "reading a PAGE_NOACCESS page succeeded";
}

TEST_F(LinearReadGateTest, AnUncommittedNeighbourMakesItsHostPageReadUnsafe) {
    // Only the first 4 KiB of the host page is mapped; the rest is a gap.
    const GuestAddr mapped = 0x00400000;
    ASSERT_TRUE(linear.map_region(mapped, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));

    EXPECT_NE(linear_check_byte(linear, mapped) & kLinearCheckMixedRead, 0)
        << "a host page with an unmapped remainder must gate loads: the "
           "hardware page grants READ across all 16 KiB";
    u8 sink = 0;
    EXPECT_FALSE(linear.read_bytes(mapped + GUEST_PAGE_SIZE, &sink, 1).is_ok())
        << "reading the unmapped remainder succeeded";
}

TEST_F(LinearReadGateTest, AUniformPageGatesNeitherReadsNorWrites) {
    const GuestAddr base = 0x00500000;
    const usize hp = static_cast<usize>(::getpagesize());
    ASSERT_TRUE(linear.map_region(base, hp, MemoryProtection::ReadWrite));

    const u8 byte = linear_check_byte(linear, base);
    EXPECT_EQ(byte & kLinearCheckMixedProt, 0);
    EXPECT_EQ(byte & kLinearCheckMixedRead, 0)
        << "a uniformly mapped host page needs no software check at all";
}

TEST_F(LinearReadGateTest, ReadBitClearsWhenAPageStopsBeingReadUnsafe) {
    // Start read-unsafe (NOACCESS neighbour), then make the neighbour
    // readable. The read bit must CLEAR -- a stale set bit would merely be
    // slow, but a stale CLEAR bit after the reverse transition is the unsafe
    // direction, and the publisher writes both edges through the same path.
    const usize hp = static_cast<usize>(::getpagesize());
    const GuestAddr a = 0x00600000;
    const GuestAddr b = a + GUEST_PAGE_SIZE;
    // Whole host page mapped, so the only read hazard is b's protection.
    ASSERT_TRUE(linear.map_region(a, GUEST_PAGE_SIZE,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(linear.map_region(b, hp - GUEST_PAGE_SIZE,
                                  MemoryProtection::None));
    ASSERT_NE(linear_check_byte(linear, a) & kLinearCheckMixedRead, 0);

    ASSERT_TRUE(linear.protect_region(b, hp - GUEST_PAGE_SIZE,
                                      MemoryProtection::Read));
    EXPECT_EQ(linear_check_byte(linear, a) & kLinearCheckMixedRead, 0)
        << "the read bit stayed set after every guest page became readable";
}

// The x64 reservation extension (enable_linear_reservation(core, true)): the
// Windows x64 user space is 47 bits but the host cannot hold a second 2^46
// window, so the reservation grows from the power-of-two core up to just
// below a check map ending at the VA limit (less the host's headroom).
// Every guest address below linear_size() is then backed at base + address,
// exactly like the core, and a fixed reservation there is honoured in place.
TEST(LinearExtensionTest, ExtendsTheReservationToTheCheckMapAtTheVaLimit) {
    constexpr usize kCore = usize{1} << 40;  // too big for the low host VA, so placed like production
    GuestMemory m;
    ASSERT_TRUE(m.enable_linear_reservation(kCore, /*extend_to_va_limit=*/true));
    if (!m.linear_check_map_ends_at_va_limit()) {
        GTEST_SKIP() << "the VA limit is held by another reservation";
    }
    EXPECT_EQ(m.linear_power_of_two_size(), kCore);
    ASSERT_GT(m.linear_size(), usize{1} << 46)
        << "the extension must reach into the 47-bit high half";
    // The map covers exactly the reservation and ends at the VA limit, so
    // an index past it faults rather than reading host memory.
    EXPECT_EQ(m.linear_check_page_count(),
              m.linear_size() >> kLinearHostPageShift);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(m.linear_check_page_map()) +
                  m.linear_check_page_count(),
              static_cast<uintptr_t>(MACH_VM_MAX_ADDRESS));
    // And the reservation itself ends below the map, with host headroom.
    EXPECT_LE(reinterpret_cast<uintptr_t>(m.linear_base()) + m.linear_size() +
                  (usize{1} << 40),
              reinterpret_cast<uintptr_t>(m.linear_check_page_map()));
    EXPECT_EQ(m.linear_size() & ((usize{1} << 28) - 1), 0u);
    // The sub-page map has the same length and indexing, extension included:
    // without it every mixed-page access in the high half takes the helper.
    EXPECT_NE(m.linear_subpage_map(), nullptr);
}

TEST(LinearExtensionTest, HighHalfRegionsAreBackedAtBasePlusAddress) {
    constexpr usize kCore = usize{1} << 40;  // too big for the low host VA, so placed like production
    GuestMemory m;
    ASSERT_TRUE(m.enable_linear_reservation(kCore, true));
    if (!m.linear_check_map_ends_at_va_limit()) {
        GTEST_SKIP() << "the VA limit is held by another reservation";
    }
    // A Chromium-style hinted pool in the high half, above the core.
    constexpr GuestAddr kHigh = 0x00005A1000000000ull;
    ASSERT_LT(kHigh + 0x10000, m.linear_size());
    EXPECT_TRUE(m.linear_backs(kHigh, 0x10000));
    ASSERT_TRUE(m.map_region(kHigh, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m.write_u64(kHigh + 0x2008, 0x1122334455667788ull));
    u64 raw = 0;
    std::memcpy(&raw, m.linear_base() + kHigh + 0x2008, sizeof(raw));
    EXPECT_EQ(raw, 0x1122334455667788ull)
        << "an extension region must live at linear_base + guest_addr";
    // Overlapping fixed placement still fails like any occupied range.
    EXPECT_FALSE(m.map_region(kHigh + 0x4000, 0x4000,
                              MemoryProtection::ReadWrite));
    // Protection changes are carried by the host page, as in the core.
    ASSERT_TRUE(m.protect_region(kHigh, 0x10000, MemoryProtection::Read));
    EXPECT_TRUE(m.write_u64(kHigh + 0x2008, 0).is_err());
    EXPECT_EQ(m.read_u64(kHigh + 0x2008).value(), 0x1122334455667788ull);
    ASSERT_TRUE(m.unmap_region(kHigh, 0x10000));
    EXPECT_TRUE(m.read_u64(kHigh + 0x2008).is_err());

    // A region straddling two 1 GiB extension chunks (kHigh - core is a
    // chunk boundary): both chunks are made committable on first use.
    constexpr GuestAddr kStraddle = kHigh - 0x100000;
    ASSERT_TRUE(m.map_region(kStraddle, 0x200000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m.write_u32(kStraddle, 0x11111111u));
    ASSERT_TRUE(m.write_u32(kStraddle + 0x1FFFFC, 0x22222222u));
    EXPECT_EQ(m.read_u32(kStraddle).value(), 0x11111111u);
    EXPECT_EQ(m.read_u32(kStraddle + 0x1FFFFC).value(), 0x22222222u);
    // Shared (file-backed style) placement in a fresh chunk remaps into it.
    void* host = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(host, MAP_FAILED);
    static_cast<u32*>(host)[4] = 0x5EEDu;
    constexpr GuestAddr kShared = 0x0000700000000000ull;
    ASSERT_LT(kShared + 0x10000, m.linear_size());
    ASSERT_TRUE(m.map_region_backed(kShared, 0x10000, host,
                                    MemoryProtection::ReadWrite));
    EXPECT_EQ(m.read_u32(kShared + 16).value(), 0x5EEDu);
    ASSERT_TRUE(m.write_u32(kShared + 20, 0xBEEFu));
    EXPECT_EQ(static_cast<u32*>(host)[5], 0xBEEFu)
        << "the shared view in the extension must alias the caller's pages";
}

TEST(LinearExtensionTest, AddressesAboveTheExtensionStayScattered) {
    constexpr usize kCore = usize{1} << 40;  // too big for the low host VA, so placed like production
    GuestMemory m;
    ASSERT_TRUE(m.enable_linear_reservation(kCore, true));
    if (!m.linear_check_map_ends_at_va_limit()) {
        GTEST_SKIP() << "the VA limit is held by another reservation";
    }
    // The last user 64 KiB granule of Windows x64 is never covered.
    constexpr GuestAddr kTop = 0x00007FFFFFFE0000ull;
    ASSERT_GE(kTop, m.linear_size());
    EXPECT_FALSE(m.linear_backs(kTop, 0x10000));
    ASSERT_TRUE(m.map_region(kTop, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m.write_u32(kTop + 0x10, 0xCAFEF00Du));
    EXPECT_EQ(m.read_u32(kTop + 0x10).value(), 0xCAFEF00Du);
    // And the host keeps room for a large scattered backing up there.
    constexpr GuestAddr kTopPool = 0x00007FF000000000ull;
    EXPECT_TRUE(m.map_region(kTopPool, usize{16} << 30,
                             MemoryProtection::None));
}

// Without the request (32-bit guests, tests, the JIT view) the reservation is
// exactly the power-of-two size it always was.
TEST(LinearExtensionTest, ReservationWithoutTheRequestIsNotExtended) {
    GuestMemory m;
    ASSERT_TRUE(m.enable_linear_reservation(kReservation));
    EXPECT_EQ(m.linear_size(), kReservation);
    EXPECT_EQ(m.linear_power_of_two_size(), kReservation);
    EXPECT_EQ(m.linear_check_page_count(), kReservation >> kLinearHostPageShift);
}

// Teardown releases the extension and the map with the core, so a later
// reservation (the next guest of a test process) can extend again.
TEST(LinearExtensionTest, TeardownReleasesTheExtension) {
    constexpr usize kCore = usize{1} << 40;  // too big for the low host VA, so placed like production
    usize first = 0;
    {
        GuestMemory m;
        ASSERT_TRUE(m.enable_linear_reservation(kCore, true));
        if (!m.linear_check_map_ends_at_va_limit()) {
            GTEST_SKIP() << "the VA limit is held by another reservation";
        }
        first = m.linear_size();
    }
    GuestMemory again;
    ASSERT_TRUE(again.enable_linear_reservation(kCore, true));
    EXPECT_TRUE(again.linear_check_map_ends_at_va_limit());
    EXPECT_GT(again.linear_size(), usize{1} << 46);
    EXPECT_NEAR(static_cast<double>(again.linear_size()),
                static_cast<double>(first), double(usize{1} << 36));
}
