/// @file test_vmem.cpp
/// @brief Unit tests for VirtualAlloc / VirtualFree / VirtualProtect (WIN32-020): allocate/free basics, flag constraints and the write-watch isolation listener

#include "test_vmem_fixture.hpp"
#include <core/memory/guard_page_traps.hpp>
#include <core/memory/write_watch_traps.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <utility>

// -----------------------------------------------------------------------------
// Process-wide write-watch isolation between tests
// -----------------------------------------------------------------------------
//
// write_watch's slot table is a process-global registry that holds RAW pointers
// into the registrant's storage -- the AllocInfo::write_watch_dirty/armed
// vectors owned by a VirtualMemoryManager. Only VirtualFree unregisters a
// region. Every test here that does VirtualAlloc(MEM_WRITE_WATCH) and then lets
// its per-test manager go out of scope therefore leaves a live slot aimed at
// freed heap, and the next record_store() anywhere in that guest range -- from
// any later test in this binary, via mem.write_u8/write_bytes or a JIT helper --
// atomically ORs a bit into a freed malloc block.
//
// That is a silent heap corruption that only becomes a signal thousands of tests
// later, which is exactly how it presented: SIGTRAP/SIGSEGV at an unrelated,
// build- and order-dependent test, and every one of those tests passing when run
// alone. It also left any_active() permanently non-zero for the rest of the run,
// so unrelated tests executed the watched-store path.
//
// Retiring the registry after each test bounds the leak to the test that made
// it. OnTestEnd runs after the fixture (and its manager) is destroyed, which is
// precisely the moment the pointers go stale. The listener is installed for the
// whole binary, not just this fixture, because a leaked region corrupts whatever
// test happens to run next.
namespace {
class WriteWatchIsolationListener : public ::testing::EmptyTestEventListener {
    void OnTestEnd(const ::testing::TestInfo&) override {
        write_watch::reset_all();
        // Same raw-pointer registry shape for PAGE_GUARD regions.
        guard_page::reset_all();
    }
};

const int g_install_write_watch_isolation = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(
        new WriteWatchIsolationListener);
    return 0;
}();
} // namespace

TEST(VirtualMemoryLinearTest, PartialCommitAndDecommitUseSoftwarePageState) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};

    const u64 allocation = manager.virtual_alloc(
        0, 0x4000, MEM_RESERVE, PAGE_READWRITE);
    ASSERT_EQ(allocation, 0x00100000u);
    ASSERT_EQ(manager.virtual_alloc(
                  allocation + 0x1000, 0x1000,
                  MEM_COMMIT, PAGE_READWRITE),
              allocation + 0x1000);

    EXPECT_TRUE(memory.read_u8(
        static_cast<GuestAddr>(allocation)).is_err());
    ASSERT_TRUE(memory.write_u8(
        static_cast<GuestAddr>(allocation + 0x1000), 0x5A).is_ok());
    EXPECT_TRUE(memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x2000)).is_err());

    ASSERT_TRUE(manager.virtual_free(
        allocation + 0x1000, 0x1000, MEM_DECOMMIT));
    EXPECT_TRUE(memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x1000)).is_err());
    ASSERT_EQ(manager.virtual_alloc(
                  allocation + 0x1000, 0x1000,
                  MEM_COMMIT, PAGE_READWRITE),
              allocation + 0x1000);
    auto recommitted = memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x1000));
    ASSERT_TRUE(recommitted.is_ok());
    EXPECT_EQ(recommitted.value(), 0u);

    ASSERT_TRUE(memory.write_u8(
        static_cast<GuestAddr>(allocation + 0x1000), 0x6B).is_ok());
    u32 old_protect = 0;
    ASSERT_TRUE(manager.virtual_protect(
        allocation + 0x1000, 0x1000,
        PAGE_NOACCESS, &old_protect));
    EXPECT_EQ(old_protect, PAGE_READWRITE);
    EXPECT_TRUE(memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x1000)).is_err());
    ASSERT_TRUE(manager.virtual_protect(
        allocation + 0x1000, 0x1000,
        PAGE_READWRITE, &old_protect));
    EXPECT_EQ(old_protect, PAGE_NOACCESS);
    auto restored = memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x1000));
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), 0x6Bu);
}

// Unhinted MEM_TOP_DOWN searches the linear reservation before the rest of
// the process ceiling. Addresses above the reservation are valid mappings,
// but the JIT cannot execute them as one host load.
TEST(VirtualMemoryLinearTest, TopDownPrefersLinearReservation) {
    constexpr u64 kWindow = 0x01000000;
    constexpr u64 kCeiling = 0x03FFFFFF;
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(kWindow));
    memory.set_maximum_user_address(kCeiling);
    VirtualMemoryManager manager{memory, kCeiling};

    const u64 low = manager.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_EQ(low, 0x00100000u);

    const u64 high = manager.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    ASSERT_NE(high, 0u);
    EXPECT_GT(high, low);
    EXPECT_LT(high, kWindow);

    // Larger than the free span inside the reservation: still succeeds, above it.
    const u64 spilled = manager.virtual_alloc(
        0, kWindow, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    ASSERT_NE(spilled, 0u);
    EXPECT_GE(spilled, kWindow);
    EXPECT_LT(spilled, kCeiling);
}

// =============================================================================
// VirtualAlloc Tests
// =============================================================================

TEST_F(VirtualMemoryTest, AllocAutoAddress) {
    u64 addr = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x1000));
}

TEST_F(VirtualMemoryTest, AllocFixedAddress) {
    u64 addr = vmm.virtual_alloc(0x200000, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    EXPECT_EQ(addr, 0x200000u);
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(0x200000), 0x1000));
}

TEST_F(VirtualMemoryTest, DynamicInternalScratchDoesNotConsumeGuestFixedRange) {
    const u64 scratch = vmm.virtual_alloc(
        0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(scratch, 0u);

    constexpr u64 guest_requested = 0x5D700000;
    const u64 fixed = vmm.virtual_alloc(
        guest_requested, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    EXPECT_EQ(fixed, guest_requested);
}

TEST_F(VirtualMemoryTest, AllocHighFixedReadWriteExecute) {
    constexpr u64 requested = 0x00007FFB'00200000ULL;
    u64 addr = vmm.virtual_alloc(requested, 0x1000,
                                 MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);

    EXPECT_EQ(addr, requested);
    EXPECT_TRUE(mem.has_protection(static_cast<GuestAddr>(requested), 0x1000,
                                   MemoryProtection::ReadWriteExecute));
}

TEST_F(VirtualMemoryTest, AllocReservationRoundsAddressDownToGranularity) {
    u64 addr = vmm.virtual_alloc(0x20F000, 0x2000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    EXPECT_EQ(addr, 0x200000u);
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(0x200000), 0x11000));
}

TEST_F(VirtualMemoryTest, AllocCommitReserve) {
    u64 addr = vmm.virtual_alloc(0, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x2000));
}

TEST_F(VirtualMemoryTest, AllocReadWrite) {
    u64 addr = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    auto wr = mem.write_u32(static_cast<GuestAddr>(addr), 0xDEADBEEF);
    EXPECT_FALSE(wr.is_err());

    auto rd = mem.read_u32(static_cast<GuestAddr>(addr));
    EXPECT_FALSE(rd.is_err());
    EXPECT_EQ(rd.value(), 0xDEADBEEFu);
}

// =============================================================================
// VirtualFree Tests
// =============================================================================

TEST_F(VirtualMemoryTest, FreeRelease) {
    u64 addr = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    bool ok = vmm.virtual_free(addr, 0, MEM_RELEASE);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x1000));
}

TEST_F(VirtualMemoryTest, ReleaseRejectsNonzeroSizeWithoutUnmapping) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    EXPECT_FALSE(vmm.virtual_free(addr, 0x1000, MEM_RELEASE));
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x1000));
}

TEST_F(VirtualMemoryTest, FreeRejectsCombinedOrUnknownOperations) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    EXPECT_FALSE(vmm.virtual_free(
        addr, 0, MEM_RELEASE | MEM_DECOMMIT));
    EXPECT_FALSE(vmm.virtual_free(addr, 0, 0x10000));
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x1000));
}

// =============================================================================
// Multiple Allocations
// =============================================================================

TEST_F(VirtualMemoryTest, AllocMultiple) {
    u64 a1 = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    u64 a2 = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    u64 a3 = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    ASSERT_NE(a1, 0u);
    ASSERT_NE(a2, 0u);
    ASSERT_NE(a3, 0u);

    // All three addresses must be distinct.
    EXPECT_NE(a1, a2);
    EXPECT_NE(a2, a3);
    EXPECT_NE(a1, a3);

    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(a1)));
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(a2)));
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(a3)));
}

// =============================================================================
// Alignment
// =============================================================================

TEST_F(VirtualMemoryTest, AllocAligns) {
    u64 addr = vmm.virtual_alloc(0, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    EXPECT_EQ(addr % GUEST_PAGE_SIZE, 0u);
}

// =============================================================================
// Error Cases
// =============================================================================

TEST_F(VirtualMemoryTest, FreeInvalidAddress) {
    bool ok = vmm.virtual_free(0xDEAD0000, 0, MEM_RELEASE);
    EXPECT_FALSE(ok);
}

TEST_F(VirtualMemoryTest, AllocZeroSize) {
    u64 addr = vmm.virtual_alloc(0, 0, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    EXPECT_EQ(addr, 0u);
}

TEST_F(VirtualMemoryTest, AllocAboveMaximumApplicationAddressFails) {
    u64 addr = vmm.virtual_alloc(0xFFEE'FFEE'FFEE'FFEEULL, 1,
                                 MEM_RESERVE, PAGE_NOACCESS);
    EXPECT_EQ(addr, 0u);
}

// Audit 01 VirtualAlloc #3 — flag constraints (Windows
// tools/oracle_probe/virtual_alloc_flags_probe.expected).
TEST_F(VirtualMemoryTest, AllocationTypeFlagConstraints) {
    EXPECT_EQ(virtual_alloc_flag_error(MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE),
              0u);
    EXPECT_EQ(virtual_alloc_flag_error(
                  MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE),
              0u);
    EXPECT_EQ(virtual_alloc_flag_error(
                  MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE),
              0u);
    EXPECT_EQ(virtual_alloc_flag_error(
                  MEM_RESERVE | MEM_COMMIT | MEM_PHYSICAL, PAGE_READWRITE),
              error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(virtual_alloc_flag_error(MEM_RESERVE | MEM_PHYSICAL,
                                       PAGE_READONLY),
              error_codes::ERROR_NO_SYSTEM_RESOURCES);
    EXPECT_EQ(virtual_alloc_flag_error(MEM_COMMIT | MEM_WRITE_WATCH,
                                       PAGE_READWRITE),
              error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(virtual_alloc_flag_error(
                  MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE),
              error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(virtual_alloc_flag_error(
                  MEM_COMMIT | MEM_RESERVE | 0x40u, PAGE_READWRITE),
              error_codes::ERROR_INVALID_PARAMETER);

    EXPECT_EQ(vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_WRITE_WATCH,
                                PAGE_READWRITE),
              0u);
    EXPECT_EQ(vmm.virtual_alloc(0, 0x1000,
                                MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                PAGE_READWRITE),
              0u);
    EXPECT_EQ(vmm.virtual_alloc(0, 0x1000,
                                MEM_RESERVE | MEM_COMMIT | MEM_PHYSICAL,
                                PAGE_READWRITE),
              0u);

    const u64 phys = vmm.virtual_alloc(0, 0x10000, MEM_RESERVE | MEM_PHYSICAL,
                                       PAGE_READWRITE);
    ASSERT_NE(phys, 0u);
    EXPECT_TRUE(vmm.virtual_free(phys, 0, MEM_RELEASE));

    const u64 lo = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
    ASSERT_NE(lo, 0u);
    const u64 hi = vmm.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    ASSERT_NE(hi, 0u);
    EXPECT_GT(hi, lo);
    EXPECT_TRUE(vmm.virtual_free(lo, 0, MEM_RELEASE));
    EXPECT_TRUE(vmm.virtual_free(hi, 0, MEM_RELEASE));
}

TEST(VirtualAllocThunkTest, FlagConstraintsSetDocumentedLastError) {
    GuestMemory memory;
    VirtualMemoryManager manager{memory};
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    auto call = [&](u32 type, u32 prot) {
        cpu.gpr.rcx = 0;
        cpu.gpr.rdx = 0x1000;
        cpu.gpr.r8 = type;
        cpu.gpr.r9 = prot;
        set_last_error(0xDEADu);
        impl_VirtualAlloc(marshaler, manager);
        return std::pair<u64, u32>{cpu.gpr.rax, get_last_error()};
    };

    {
        const auto [addr, err] =
            call(MEM_COMMIT | MEM_RESERVE | 0x40u, PAGE_READWRITE);
        EXPECT_EQ(addr, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_PARAMETER);
    }
    {
        const auto [addr, err] =
            call(MEM_RESERVE | MEM_PHYSICAL, PAGE_READONLY);
        EXPECT_EQ(addr, 0u);
        EXPECT_EQ(err, error_codes::ERROR_NO_SYSTEM_RESOURCES);
    }
    {
        const auto [addr, err] =
            call(MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        EXPECT_EQ(addr, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_PARAMETER);
    }
    {
        const auto [addr, err] =
            call(MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
        EXPECT_NE(addr, 0u);
        EXPECT_EQ(err, 0xDEADu);
        EXPECT_TRUE(manager.virtual_free(addr, 0, MEM_RELEASE));
    }
}
