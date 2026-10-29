/// @file test_vmem_discard.cpp
/// @brief Unit tests for DiscardVirtualMemory and VirtualAlloc MEM_RESET / MEM_RESET_UNDO (WIN32-020)

#include "test_vmem_fixture.hpp"
#include <capsule/vfs/vfs.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/memory/file_mapping.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <algorithm>
#include <cstring>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

TEST_F(VirtualMemoryTest, DiscardPrimitiveRejectsReadOnlyMemory) {
    constexpr GuestAddr kMapped = 0x70000000;
    ASSERT_TRUE(mem.map_region(
                    kMapped, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(mem.write_u8(kMapped, 0xA5).is_ok());
    ASSERT_TRUE(mem.protect_region(
                    kMapped, 0x1000, MemoryProtection::Read)
                    .is_ok());

    EXPECT_TRUE(mem.discard_region_contents(kMapped, 0x1000).is_err());
    EXPECT_EQ(mem.read_u8(kMapped).value_or(0), 0xA5u);
}

TEST_F(VirtualMemoryTest, DiscardRequiresAlignedCommittedReadWritePages) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0xA5).is_ok());
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr + 0x1000),
                             0x5A).is_ok());

    EXPECT_FALSE(vmm.discard_virtual_memory(addr + 1, 0x1000));
    EXPECT_FALSE(vmm.discard_virtual_memory(addr, 0x1001));
    EXPECT_FALSE(vmm.discard_virtual_memory(addr, 0));
    EXPECT_EQ(mem.read_u8(static_cast<GuestAddr>(addr)).value_or(0), 0xA5u);

    u32 old_protect = 0;
    ASSERT_TRUE(vmm.virtual_protect(
        addr, 0x1000, PAGE_READONLY, &old_protect));
    EXPECT_FALSE(vmm.discard_virtual_memory(addr, 0x1000));
    EXPECT_EQ(mem.read_u8(static_cast<GuestAddr>(addr)).value_or(0), 0xA5u);

    ASSERT_TRUE(vmm.virtual_free(addr + 0x1000, 0x1000, MEM_DECOMMIT));
    EXPECT_FALSE(vmm.discard_virtual_memory(addr + 0x1000, 0x1000));
    EXPECT_FALSE(vmm.discard_virtual_memory(0x70000000, 0x1000));
}

TEST_F(VirtualMemoryTest, DiscardKeepsPagesCommittedReadWrite) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0xA5).is_ok());
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr + 0x1000),
                             0x5A).is_ok());

    ASSERT_TRUE(vmm.discard_virtual_memory(addr, 0x2000));
    VirtualAllocationQuery info{};
    ASSERT_TRUE(vmm.virtual_query(addr, info));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.protect, PAGE_READWRITE);
    // Learn: discarded contents are undefined (old bytes or zero are both
    // valid), but the pages stay committed, readable, and writable.
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr)).is_ok());
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr + 0x1000)).is_ok());
    EXPECT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0x11).is_ok());
    EXPECT_EQ(mem.read_u8(static_cast<GuestAddr>(addr)).value_or(0), 0x11u);
}

TEST_F(VirtualMemoryTest, DiscardDoesNotCreateWriteWatchDirtyPages) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x2000, MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
        PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_EQ(vmm.reset_write_watch(addr, 0x2000), 0u);

    ASSERT_TRUE(vmm.discard_virtual_memory(addr, 0x2000));

    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(addr, 0x2000, count, granularity, pages),
              0u);
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(pages.empty());
}

TEST_F(VirtualMemoryTest, DiscardPreservesExistingWriteWatchDirtyPages) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x2000, MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
        PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_EQ(vmm.reset_write_watch(addr, 0x2000), 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr + 0x1000), 0x5A)
                    .is_ok());

    ASSERT_TRUE(vmm.discard_virtual_memory(addr, 0x2000));

    u64 count = 8;
    u32 granularity = 0;
    std::vector<u64> pages;
    EXPECT_EQ(vmm.get_write_watch(addr, 0x2000, count, granularity, pages),
              0u);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0], addr + 0x1000);
}

TEST(DiscardVirtualMemoryThunkTest, ReturnsSystemErrorWithoutChangingLastError) {
    GuestMemory memory;
    capsule::Vfs vfs;
    VirtualMemoryManager manager{memory};
    Kernel32ExtManager kernel32{memory, vfs};
    kernel32.set_virtual_memory_manager(&manager);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    const u64 address = manager.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(address, 0u);
    set_last_error(0x1234u);
    cpu.gpr.rcx = address;
    cpu.gpr.rdx = 0x1000;
    kernel32.impl_DiscardVirtualMemory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(get_last_error(), 0x1234u);

    cpu.gpr.rcx = address + 1;
    cpu.gpr.rdx = 0x1000;
    kernel32.impl_DiscardVirtualMemory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST(VirtualMemoryLinearTest,
     DiscardPreservesHardwareProtectionAndNeighbourBytes) {
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x01000000));
    ASSERT_TRUE(memory.enable_linear_hardware_protection());
    VirtualMemoryManager manager{memory, 0x00FF'FFFF};
    const u64 allocation = manager.virtual_alloc(
        0, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(allocation, 0u);
    ASSERT_TRUE(memory.write_u8(
        static_cast<GuestAddr>(allocation), 0x11).is_ok());
    ASSERT_TRUE(memory.write_u8(
        static_cast<GuestAddr>(allocation + 0x1000), 0x22).is_ok());

    ASSERT_TRUE(manager.discard_virtual_memory(
        allocation + 0x1000, 0x1000));
    EXPECT_EQ(memory.read_u8(
        static_cast<GuestAddr>(allocation)).value_or(0), 0x11u);
    EXPECT_TRUE(memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x1000)).is_ok());
    ASSERT_TRUE(memory.write_u8(
        static_cast<GuestAddr>(allocation + 0x1000), 0x33).is_ok());
    EXPECT_EQ(memory.read_u8(
        static_cast<GuestAddr>(allocation + 0x1000)).value_or(0), 0x33u);
}

// =============================================================================
// VirtualAlloc(MEM_RESET) / VirtualAlloc(MEM_RESET_UNDO)
// =============================================================================
//
// Chromium's PartitionAlloc discards unused pages with
// VirtualAlloc(addr, len, MEM_RESET, PAGE_READWRITE) and PA_CHECKs the result,
// so a NULL return is a fatal int3 rather than a degraded allocator.

TEST_F(VirtualMemoryTest, MemResetOnCommittedRangeReturnsBaseAndKeepsContents) {
    const u64 base = vmm.virtual_alloc(0, 0x40000, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(mem.write_u32(static_cast<GuestAddr>(base + 0x100), 0xA5A5A5A5u)
                    .is_ok());

    // MEM_RESET neither reserves nor commits; it returns the range's base.
    EXPECT_EQ(vmm.virtual_alloc(base, 0x40000, MEM_RESET, PAGE_READWRITE),
              base);
    // The block stays committed and readable for later reuse.
    auto after_reset = mem.read_u32(static_cast<GuestAddr>(base + 0x100));
    ASSERT_TRUE(after_reset.is_ok());

    // MEM_RESET_UNDO takes the hint back; the contents were never paged out.
    EXPECT_EQ(vmm.virtual_alloc(base, 0x40000, MEM_RESET_UNDO, PAGE_READWRITE),
              base);
    auto after_undo = mem.read_u32(static_cast<GuestAddr>(base + 0x100));
    ASSERT_TRUE(after_undo.is_ok());
    EXPECT_EQ(after_undo.value(), 0xA5A5A5A5u);
}

namespace {

u64 host_phys_footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.phys_footprint;
}

usize host_dirty_pages(const void* host, usize size) {
    const usize page = static_cast<usize>(::getpagesize());
    std::vector<char> state(size / page);
    if (::mincore(const_cast<void*>(host), size, state.data()) != 0) return ~usize{0};
    return static_cast<usize>(std::count_if(state.begin(), state.end(),
        [](char s) { return (s & MINCORE_MODIFIED) != 0; }));
}

constexpr u64 kDiscardSpan = u64{64} << 20;
constexpr u8 kDiscardFill = 0xA7;

// Learn (VirtualAlloc MEM_RESET, DiscardVirtualMemory): the data is no longer
// of interest and should not be written to the paging file; the block stays
// committed with its protection, and later writes must work. So the host must
// not dirty discarded pages, and should give them back.
void expect_discard_releases_host_pages(GuestMemory& mem,
                                        VirtualMemoryManager& vmm,
                                        bool via_mem_reset) {
    const u64 base = vmm.virtual_alloc(0, kDiscardSpan,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    const auto region = mem.get_region_info(static_cast<GuestAddr>(base));
    ASSERT_NE(region.host_ptr, nullptr);
    std::memset(region.host_ptr, kDiscardFill, kDiscardSpan);

    const usize dirty_before = host_dirty_pages(region.host_ptr, kDiscardSpan);
    const u64 footprint_before = host_phys_footprint();
    if (via_mem_reset) {
        EXPECT_EQ(vmm.virtual_alloc(base, kDiscardSpan, MEM_RESET,
                                    PAGE_NOACCESS),
                  base);
    } else {
        EXPECT_TRUE(vmm.discard_virtual_memory(base, kDiscardSpan));
    }
    const u64 footprint_after = host_phys_footprint();
    const usize dirty_after = host_dirty_pages(region.host_ptr, kDiscardSpan);

    const usize host_pages = kDiscardSpan / static_cast<usize>(::getpagesize());
    EXPECT_GE(dirty_before, host_pages * 3 / 4);
    EXPECT_LE(dirty_after, host_pages / 16);
    ASSERT_GT(footprint_before, footprint_after);
    EXPECT_GE(footprint_before - footprint_after, kDiscardSpan * 3 / 4);

    VirtualAllocationQuery info{};
    ASSERT_TRUE(vmm.virtual_query(base, info));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.protect, PAGE_READWRITE);
    EXPECT_GE(info.region_size, kDiscardSpan);

    for (u64 off = 0; off < kDiscardSpan; off += 0x100000) {
        ASSERT_TRUE(mem.write_u32(static_cast<GuestAddr>(base + off),
                                  0x5EED0000u | static_cast<u32>(off >> 20))
                        .is_ok());
    }
    for (u64 off = 0; off < kDiscardSpan; off += 0x100000) {
        EXPECT_EQ(mem.read_u32(static_cast<GuestAddr>(base + off)).value_or(0),
                  0x5EED0000u | static_cast<u32>(off >> 20));
    }
    EXPECT_TRUE(vmm.virtual_free(base, 0, MEM_RELEASE));
}

}  // namespace

TEST_F(VirtualMemoryTest, MemResetReleasesHostPagesWithoutDirtyingThem) {
    expect_discard_releases_host_pages(mem, vmm, true);
}

TEST_F(VirtualMemoryTest, DiscardReleasesHostPagesWithoutDirtyingThem) {
    expect_discard_releases_host_pages(mem, vmm, false);
}

TEST_F(VirtualMemoryTest, MemResetUndoSuccessMeansEveryByteSurvived) {
    const u64 base = vmm.virtual_alloc(0, kDiscardSpan,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    const auto region = mem.get_region_info(static_cast<GuestAddr>(base));
    ASSERT_NE(region.host_ptr, nullptr);
    // Leave the top half untouched: never-written pages read as zero either
    // way and must not make MEM_RESET_UNDO report lost data.
    std::memset(region.host_ptr, kDiscardFill, kDiscardSpan / 2);

    ASSERT_EQ(vmm.virtual_alloc(base, kDiscardSpan, MEM_RESET, PAGE_READWRITE),
              base);
    // Learn: success means all data is intact; failure means some was zeroed.
    // Without host memory pressure nothing is reclaimed, so success is the
    // expected outcome, and either way the claim must be true.
    const u64 undo = vmm.virtual_alloc(base, kDiscardSpan, MEM_RESET_UNDO,
                                       PAGE_READWRITE);
    const u8* bytes = region.host_ptr;
    const bool intact =
        std::all_of(bytes, bytes + kDiscardSpan / 2,
                    [](u8 b) { return b == kDiscardFill; }) &&
        std::all_of(bytes + kDiscardSpan / 2, bytes + kDiscardSpan,
                    [](u8 b) { return b == 0; });
    if (undo != 0) {
        EXPECT_EQ(undo, base);
        EXPECT_TRUE(intact);
    } else {
        EXPECT_FALSE(intact);
    }
    EXPECT_EQ(undo, base) << "host reclaimed reset pages without pressure";
    EXPECT_TRUE(vmm.virtual_free(base, 0, MEM_RELEASE));
}

// tools/oracle_probe/mem_reset_probe{32,64}.expected: Windows does not round
// an unaligned MEM_RESET base down; it fails ERROR_INVALID_ADDRESS.
TEST_F(VirtualMemoryTest, MemResetRejectsUnalignedBaseWithInvalidAddress) {
    const u64 base = vmm.virtual_alloc(0, 0x40000, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    EXPECT_EQ(vmm.virtual_alloc(base + 0x10, 0x10, MEM_RESET, PAGE_READWRITE),
              0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_INVALID_ADDRESS);
    // Page aligned but not allocation-granularity aligned is fine.
    EXPECT_EQ(vmm.virtual_alloc(base + 0x1000, 0x1000, MEM_RESET,
                                PAGE_READWRITE),
              base + 0x1000);
}

// The PartitionAlloc pool shape: a placeholder replaced by a NOACCESS
// reservation with only its first pages committed. MEM_RESET over the
// reserved remainder succeeds on Windows and leaves state and protection as
// they were (mem_reset_probe "reserved range rw" / "after reset").
TEST_F(VirtualMemoryTest, MemResetOnReservedPlaceholderPoolSucceeds) {
    constexpr u64 kPool = 0x10000000;   // 256 MiB, as the x86 probe
    constexpr u64 kOff = 0x100000;
    const u64 base = vmm.virtual_alloc2(0, kPool,
                                        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                        PAGE_NOACCESS);
    ASSERT_NE(base, 0u);
    ASSERT_EQ(vmm.virtual_alloc2(base, kPool,
                                 MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                                 PAGE_NOACCESS),
              base);
    ASSERT_EQ(vmm.virtual_alloc(base, 0x10000, MEM_COMMIT, PAGE_READWRITE),
              base);
    ASSERT_TRUE(mem.write_u32(base, 0xC0FFEEu).is_ok());

    EXPECT_EQ(vmm.virtual_alloc(base + kOff, kPool - kOff, MEM_RESET,
                                PAGE_READWRITE),
              base + kOff);
    EXPECT_EQ(vmm.virtual_alloc(base + kOff, kPool - kOff, MEM_RESET,
                                PAGE_NOACCESS),
              base + kOff);
    EXPECT_EQ(vmm.virtual_alloc(base, kPool, MEM_RESET, PAGE_READWRITE), base);
    VirtualAllocationQuery reserved{};
    ASSERT_TRUE(vmm.virtual_query(base + kOff, reserved));
    EXPECT_EQ(reserved.state, MEM_RESERVE);
    VirtualAllocationQuery committed{};
    ASSERT_TRUE(vmm.virtual_query(base, committed));
    EXPECT_EQ(committed.state, MEM_COMMIT);
    EXPECT_EQ(committed.protect, PAGE_READWRITE);

    // Past the allocation's end, and after release: ERROR_INVALID_ADDRESS.
    EXPECT_EQ(vmm.virtual_alloc(base + kPool - 0x1000, 0x2000, MEM_RESET,
                                PAGE_READWRITE),
              0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_INVALID_ADDRESS);
    ASSERT_TRUE(vmm.virtual_free(base, 0, MEM_RELEASE));
    EXPECT_EQ(vmm.virtual_alloc(base + kOff, 0x1000, MEM_RESET,
                                PAGE_READWRITE),
              0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_INVALID_ADDRESS);
}

TEST_F(VirtualMemoryTest, MemResetProtectionAndBaseErrorsMatchWindows) {
    const u64 base = vmm.virtual_alloc(0, 0x10000, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    EXPECT_EQ(vmm.virtual_alloc(base, 0x1000, MEM_RESET, 0), 0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(vmm.virtual_alloc(base, 0x1000, MEM_RESET, 0x1234), 0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_NOT_SUPPORTED);
    EXPECT_EQ(vmm.virtual_alloc(0, 0x1000, MEM_RESET, PAGE_READWRITE), 0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_INVALID_ADDRESS);
}

namespace {

/// Calls the registered VirtualAlloc2 export with x64 arguments.
struct VirtualAlloc2Call {
    GuestMemory& memory;
    VirtualMemoryManager& vmm;
    VirtualQueryManager vqm{memory, &vmm};
    DllExports exports = make_virtual_protect_exports(vmm, vqm);
    cpu::CpuState cpu;
    static constexpr u64 kStack = 0x7F000000;

    VirtualAlloc2Call(GuestMemory& m, VirtualMemoryManager& v)
        : memory(m), vmm(v) {
        EXPECT_TRUE(memory.map_region(kStack, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    u64 operator()(u64 base, u64 size, u32 type, u32 protect, bool x64 = true) {
        if (x64) {
            cpu.init_long_mode();
            cpu.gpr.rsp = kStack + 0x8000;
            cpu.gpr.rcx = ~u64{0};   // GetCurrentProcess()
            cpu.gpr.rdx = base;
            cpu.gpr.r8 = size;
            cpu.gpr.r9 = type;
            EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, protect).is_ok());
            EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());
            EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 0).is_ok());
        } else {
            cpu.init_protected_mode();
            cpu.gpr.rsp = kStack + 0x8000;
            const u32 args[] = {0xFFFFFFFFu, static_cast<u32>(base),
                                static_cast<u32>(size), type, protect, 0, 0};
            for (u32 i = 0; i < 7; ++i)
                EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, args[i]).is_ok());
        }
        Marshaler marshaler{memory};
        marshaler.attach_cpu(cpu);
        for (const auto& entry : exports.entries) {
            if (entry.name == "VirtualAlloc2") entry.handler(marshaler);
        }
        return x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFu);
    }
};

}  // namespace

// VS Code (Electron) DiscardSystemPages: DiscardVirtualMemory refuses the
// NOACCESS pool, then VirtualAlloc2(MEM_RESET) on it must succeed or Chromium
// CHECK-fails. Expected values: tools/oracle_probe/mem_reset_probe64.expected.
TEST_F(VirtualMemoryTest, VirtualAlloc2ResetMatchesWindows) {
    VirtualAlloc2Call va2{mem, vmm};
    constexpr u64 kPool = 0x10000000;
    const u64 base = va2(0, kPool, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                         PAGE_NOACCESS);
    ASSERT_NE(base, 0u);
    ASSERT_EQ(va2(base, kPool, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                  PAGE_NOACCESS), base);
    // MEM_COMMIT inside a reservation at a page-, not 64K-, aligned base.
    EXPECT_EQ(va2(base + 0x11000, 0x1000, MEM_COMMIT, PAGE_READWRITE),
              base + 0x11000);

    EXPECT_EQ(va2(base + 0x100000, kPool - 0x100000, MEM_RESET,
                  PAGE_READWRITE), base + 0x100000);
    EXPECT_EQ(va2(base + 0x11000, 0x1000, MEM_RESET_UNDO, PAGE_READWRITE),
              base + 0x11000);
    const struct { u64 at; u64 size; u32 type; u32 prot; u32 error; } kFail[] = {
        {base + 0x10, 0x10, MEM_RESET, PAGE_READWRITE, error_codes::ERROR_INVALID_ADDRESS},
        {base + kPool - 0x1000, 0x2000, MEM_RESET, PAGE_READWRITE, error_codes::ERROR_INVALID_ADDRESS},
        {0, 0x1000, MEM_RESET, PAGE_READWRITE, error_codes::ERROR_INVALID_ADDRESS},
        {base, 0x1000, MEM_RESET, 0, error_codes::ERROR_INVALID_PARAMETER},
        {base, 0x1000, MEM_RESET, 0x1234, error_codes::ERROR_NOT_SUPPORTED},
        {base, 0x1000, MEM_RESET | MEM_COMMIT, PAGE_READWRITE, error_codes::ERROR_INVALID_PARAMETER},
        {base, 0x1000, MEM_RESET | MEM_RESET_UNDO, PAGE_READWRITE, error_codes::ERROR_INVALID_PARAMETER},
        {base, 0, MEM_RESET, PAGE_READWRITE, error_codes::ERROR_INVALID_PARAMETER},
    };
    for (const auto& f : kFail) {
        SCOPED_TRACE(testing::Message() << std::hex << "type=0x" << f.type
                                        << " at=0x" << f.at);
        set_last_error(0xDEAD);
        EXPECT_EQ(va2(f.at, f.size, f.type, f.prot), 0u);
        EXPECT_EQ(get_last_error(), f.error);
    }
    ASSERT_TRUE(vmm.virtual_free(base, 0, MEM_RELEASE));
    set_last_error(0xDEAD);
    EXPECT_EQ(va2(base + 0x100000, 0x1000, MEM_RESET, PAGE_READWRITE), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_ADDRESS);
}

// mem_reset_probe "VirtualAlloc2 types": accepted combinations and their
// errors, x64 and WOW64 (they differ only for a NULL-base commit|write_watch).
TEST_F(VirtualMemoryTest, VirtualAlloc2AllocationTypesMatchWindows) {
    VirtualAlloc2Call va2{mem, vmm};
    const struct { const char* name; u32 type; u32 prot; bool ok64; bool ok32; } kTypes[] = {
        {"reserve|top_down", MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS, true, true},
        {"reserve|commit|top_down", MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE, true, true},
        {"reserve|commit|write_watch", MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, true, true},
        {"reserve|write_watch", MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE, true, true},
        {"commit|write_watch", MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, false, true},
        {"reserve|placeholder rw", MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_READWRITE, false, false},
        {"reserve|placeholder|top_down", MEM_RESERVE | MEM_RESERVE_PLACEHOLDER | MEM_TOP_DOWN, PAGE_NOACCESS, true, true},
        {"commit|placeholder", MEM_COMMIT | MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, false, false},
        {"low bits 0x3f", MEM_RESERVE | 0x3Fu, PAGE_NOACCESS, false, false},
        {"unknown 0x10000", MEM_RESERVE | 0x00010000u, PAGE_NOACCESS, false, false},
        {"none", 0, PAGE_NOACCESS, false, false},
        {"reserve|physical rw", MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE, true, true},
        {"reserve|commit|large", MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE, false, false},
    };
    for (bool x64 : {true, false}) {
        for (const auto& t : kTypes) {
            SCOPED_TRACE(testing::Message() << t.name << (x64 ? " x64" : " x86"));
            set_last_error(0xDEAD);
            const u64 r = va2(0, 0x10000, t.type, t.prot, x64);
            const bool expect_ok = x64 ? t.ok64 : t.ok32;
            EXPECT_EQ(r != 0, expect_ok);
            if (r != 0) {
                EXPECT_TRUE(vmm.virtual_free(r, 0, MEM_RELEASE));
            } else {
                EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            }
        }
        // A size that is not a page multiple is rounded up, not refused.
        const u64 r = va2(0, 0x1001, MEM_RESERVE, PAGE_NOACCESS, x64);
        EXPECT_NE(r, 0u);
        if (r != 0) EXPECT_TRUE(vmm.virtual_free(r, 0, MEM_RELEASE));
    }
}

TEST_F(VirtualMemoryTest, MemResetRejectsRangeOutsideAnyAllocation) {
    const u64 base = vmm.virtual_alloc(0, 0x2000, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    // Past the end of the owning allocation.
    EXPECT_EQ(vmm.virtual_alloc(base, 0x10000, MEM_RESET, PAGE_READWRITE), 0u);
    // Never allocated at all.
    EXPECT_EQ(vmm.virtual_alloc(0x60000000ull, 0x1000, MEM_RESET,
                                PAGE_READWRITE),
              0u);
}

TEST_F(VirtualMemoryTest, MemResetRejectsCombinationWithOtherAllocationTypes) {
    const u64 base = vmm.virtual_alloc(0, 0x2000, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    // Learn: MEM_RESET "cannot be used with any other value".
    EXPECT_EQ(vmm.virtual_alloc(base, 0x2000, MEM_RESET | MEM_COMMIT,
                                PAGE_READWRITE),
              0u);
    EXPECT_EQ(vmm.virtual_alloc(base, 0x2000, MEM_RESET | MEM_RESET_UNDO,
                                PAGE_READWRITE),
              0u);
}

TEST_F(VirtualMemoryTest, MemResetAllowsPagefileViewAndRejectsFileView) {
    // Audit 01 VirtualAlloc #5 / Learn MEM_RESET: pagefile-backed shared views
    // succeed; file-backed views fail ERROR_USER_MAPPED_FILE (Windows
    // tools/oracle_probe/virtual_alloc_reset_map_probe.expected).
    HandleTable handles;
    FileMappingManager mappings{handles, mem};
    vmm.set_file_mapping_manager(&mappings);

    const Handle anon = mappings.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 0x10000, "");
    ASSERT_NE(anon, NULL_HANDLE);
    const GuestAddr anon_view =
        mappings.map_view_of_file(anon, 0x0002 /*FILE_MAP_WRITE*/, 0, 0);
    ASSERT_NE(anon_view, 0u);
    EXPECT_EQ(mappings.mem_reset_view_disposition(anon_view, 0x1000), ~u32{0});
    EXPECT_EQ(vmm.virtual_alloc(anon_view, 0x1000, MEM_RESET, PAGE_READWRITE),
              static_cast<u64>(anon_view & ~u64{0xFFFu}));
}
