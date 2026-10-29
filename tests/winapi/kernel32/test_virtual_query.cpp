/// @file test_virtual_query.cpp
/// @brief Tests for VirtualQuery / FlushInstructionCache / VirtualProtect thunks
///        (WIN32-036)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/pe/mapper/section_mapper.hpp>
#include <cstring>
#include <memory>
#include <tuple>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class VirtualQueryTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE = 0x10000;
    static constexpr usize     MEM_SIZE = 0x10000;

    void SetUp() override {
        memory_.map_region(MEM_BASE, MEM_SIZE, MemoryProtection::ReadWrite);
        vmm_ = std::make_unique<VirtualMemoryManager>(memory_);
        vqm_ = std::make_unique<VirtualQueryManager>(memory_, vmm_.get());
    }

    GuestMemory memory_;
    std::unique_ptr<VirtualMemoryManager> vmm_;
    std::unique_ptr<VirtualQueryManager>  vqm_;
};

// =============================================================================
// MemoryBasicInformation Layout
// =============================================================================

TEST(MemoryBasicInformationTest, StructSizeIs48Bytes) {
    static_assert(sizeof(MemoryBasicInformation) == 48);
    EXPECT_EQ(sizeof(MemoryBasicInformation), 48u);
}

TEST(MemoryBasicInformationTest, TriviallyCopyable) {
    EXPECT_TRUE(std::is_trivially_copyable_v<MemoryBasicInformation>);
}

// =============================================================================
// VirtualQuery — Mapped Region
// =============================================================================

TEST_F(VirtualQueryTest, QueryMappedRegionReturnsCommit) {
    MemoryBasicInformation info{};
    usize ret = vqm_->virtual_query(MEM_BASE, info);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.protect, PAGE_READWRITE);
    EXPECT_EQ(info.type, MEM_PRIVATE);
    EXPECT_EQ(info.base_address, MEM_BASE);
    EXPECT_EQ(info.region_size, MEM_SIZE);
}

TEST_F(VirtualQueryTest, QueryInsideMappedRegion) {
    // Query an address in the middle of the mapped region.
    MemoryBasicInformation info{};
    usize ret = vqm_->virtual_query(MEM_BASE + 0x100, info);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_COMMIT);
    // base_address should be page-aligned down.
    EXPECT_EQ(info.base_address, MEM_BASE);
}

// Audit 01 VirtualQuery #1: AllocationBase is the mapping base for every page
// of a GuestMemory region, even when Protect runs differ inside it.
TEST_F(VirtualQueryTest, AllocationBaseIsMappingBaseAcrossProtectionRuns) {
    constexpr GuestAddr kBase = 0x600000;
    ASSERT_TRUE(memory_.map_region(kBase, 0x3000, MemoryProtection::Read).is_ok());
    ASSERT_TRUE(memory_.protect_region(kBase + 0x1000, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory_.protect_region(kBase + 0x2000, 0x1000,
                                       MemoryProtection::ReadExecute).is_ok());
    MemoryBasicInformation a{}, b{}, c{};
    ASSERT_EQ(vqm_->virtual_query(kBase, a), sizeof(a));
    ASSERT_EQ(vqm_->virtual_query(kBase + 0x1000, b), sizeof(b));
    ASSERT_EQ(vqm_->virtual_query(kBase + 0x2000, c), sizeof(c));
    EXPECT_EQ(a.allocation_base, kBase);
    EXPECT_EQ(b.allocation_base, kBase);
    EXPECT_EQ(c.allocation_base, kBase);
    EXPECT_EQ(a.protect, PAGE_READONLY);
    EXPECT_EQ(b.protect, PAGE_READWRITE);
    EXPECT_EQ(c.protect, PAGE_EXECUTE_READ);
}

TEST_F(VirtualQueryTest, QueryPeImageReportsMemImage) {
    // Learn MEMORY_BASIC_INFORMATION.Type: loaded PE pages are MEM_IMAGE
    // (0x1000000) with AllocationBase equal to the image base. Without a
    // ModuleManager the same GuestMemory mapping stays MEM_PRIVATE.
    ThunkRegistry registry(0x70000000);
    ModuleManager modules(registry);
    modules.set_main_module(MEM_BASE, "C:\\app.exe");
    modules.register_main_module_exports(
        MEM_BASE, "C:\\app.exe", MEM_BASE + MEM_SIZE, 32, {});
    vqm_->set_module_manager(&modules);

    MemoryBasicInformation info{};
    usize ret = vqm_->virtual_query(MEM_BASE + 0x100, info);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.type, MEM_IMAGE);
    EXPECT_EQ(info.allocation_base, MEM_BASE);
}

TEST_F(VirtualQueryTest, QueryPreservesExecuteOnlyProtection) {
    constexpr GuestAddr EXECUTE_BASE = 0x40000;
    ASSERT_TRUE(memory_.map_region(
        EXECUTE_BASE, 0x1000, MemoryProtection::Execute).is_ok());
    MemoryBasicInformation info{};

    EXPECT_EQ(vqm_->virtual_query(EXECUTE_BASE, info),
              sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.allocation_protect, PAGE_EXECUTE);
    EXPECT_EQ(info.protect, PAGE_EXECUTE);
}

TEST_F(VirtualQueryTest, QueryPreservesNoAccessProtection) {
    constexpr GuestAddr NOACCESS_BASE = 0x50000;
    ASSERT_TRUE(memory_.map_region(
        NOACCESS_BASE, 0x1000, MemoryProtection::None).is_ok());
    MemoryBasicInformation info{};

    EXPECT_EQ(vqm_->virtual_query(NOACCESS_BASE, info),
              sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.allocation_protect, PAGE_NOACCESS);
    EXPECT_EQ(info.protect, PAGE_NOACCESS);
}

TEST_F(VirtualQueryTest, FragmentedUpdatesPreserveStateProtectionAndMaximalRuns) {
    constexpr usize count = 32;
    constexpr u64 page = 0x4000;
    std::array<u32, count> expected{};
    const u64 base = vmm_->virtual_alloc(0, count * page,
                                        MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    u32 seed = 0x81a93c45;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 16;
    };
    for (usize step = 0; step < 300; ++step) {
        const usize first = next() % count;
        const usize length = 1 + next() % (count - first);
        const u32 operation = next() % 4;
        u32 protection = 0;
        if (operation == 0) {
            ASSERT_TRUE(vmm_->virtual_free(base + first * page,
                                           length * page, MEM_DECOMMIT));
        } else {
            ASSERT_EQ(vmm_->virtual_alloc(base + first * page, length * page,
                MEM_COMMIT, PAGE_READWRITE), base + first * page);
            protection = operation == 1 ? PAGE_READWRITE
                       : operation == 2 ? PAGE_READONLY : PAGE_NOACCESS;
            u32 old = 0;
            ASSERT_TRUE(vmm_->virtual_protect(base + first * page,
                length * page, protection, &old));
            EXPECT_EQ(old, PAGE_READWRITE);
        }
        std::fill(expected.begin() + first,
                  expected.begin() + first + length, protection);
        for (usize i = 0; i < count;) {
            usize end = i + 1;
            while (end < count && expected[end] == expected[i]) ++end;
            MemoryBasicInformation info{};
            ASSERT_EQ(vqm_->virtual_query(base + i * page, info), sizeof(info));
            ASSERT_EQ(info.base_address, base + i * page) << step;
            ASSERT_EQ(info.region_size, (end - i) * page) << step;
            ASSERT_EQ(info.allocation_base, base);
            ASSERT_EQ(info.allocation_protect, PAGE_READWRITE);
            ASSERT_EQ(info.state, expected[i] ? MEM_COMMIT : MEM_RESERVE);
            ASSERT_EQ(info.protect, expected[i]);
            i = end;
        }
    }
    EXPECT_TRUE(vmm_->virtual_free(base, 0, MEM_RELEASE));
}

TEST_F(VirtualQueryTest, QueryDistinguishesReservedAndCommittedRuns) {
    const u64 allocation = vmm_->virtual_alloc(
        0, 0x3000, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ASSERT_NE(allocation, 0u);
    ASSERT_EQ(vmm_->virtual_alloc(
                  allocation + 0x1000, 0x1000,
                  MEM_COMMIT, PAGE_READWRITE),
              allocation + 0x1000);

    MemoryBasicInformation first{};
    MemoryBasicInformation middle{};
    MemoryBasicInformation last{};
    ASSERT_EQ(vqm_->virtual_query(allocation, first),
              sizeof(MemoryBasicInformation));
    ASSERT_EQ(vqm_->virtual_query(allocation + 0x1000, middle),
              sizeof(MemoryBasicInformation));
    ASSERT_EQ(vqm_->virtual_query(allocation + 0x2000, last),
              sizeof(MemoryBasicInformation));

    EXPECT_EQ(first.base_address, allocation);
    EXPECT_EQ(first.allocation_base, allocation);
    EXPECT_EQ(first.allocation_protect, PAGE_EXECUTE_READWRITE);
    EXPECT_EQ(first.region_size, 0x1000u);
    EXPECT_EQ(first.state, MEM_RESERVE);
    EXPECT_EQ(first.protect, 0u);
    EXPECT_EQ(first.type, MEM_PRIVATE);

    EXPECT_EQ(middle.base_address, allocation + 0x1000);
    EXPECT_EQ(middle.allocation_base, allocation);
    EXPECT_EQ(middle.allocation_protect, PAGE_EXECUTE_READWRITE);
    EXPECT_EQ(middle.region_size, 0x1000u);
    EXPECT_EQ(middle.state, MEM_COMMIT);
    EXPECT_EQ(middle.protect, PAGE_READWRITE);
    EXPECT_EQ(middle.type, MEM_PRIVATE);

    EXPECT_EQ(last.base_address, allocation + 0x2000);
    EXPECT_EQ(last.state, MEM_RESERVE);
    EXPECT_EQ(last.region_size, 0x1000u);
}

// =============================================================================
// VirtualQuery — Unmapped Region
// =============================================================================

TEST_F(VirtualQueryTest, QueryUnmappedRegionReturnsFree) {
    MemoryBasicInformation info{};
    usize ret = vqm_->virtual_query(0xDEAD0000, info);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_FREE);
    EXPECT_EQ(info.protect, PAGE_NOACCESS);
    EXPECT_EQ(info.type, 0u);
    EXPECT_EQ(info.allocation_base, 0u);
    EXPECT_EQ(info.allocation_protect, 0u);
}

TEST_F(VirtualQueryTest, QueryNullAddress) {
    MemoryBasicInformation info{};
    usize ret = vqm_->virtual_query(0, info);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_FREE);
    EXPECT_EQ(info.base_address, 0u);
}

TEST_F(VirtualQueryTest, QueryAboveMaximumApplicationAddressFails) {
    MemoryBasicInformation info{};

    EXPECT_EQ(vqm_->virtual_query(0x1'0000'0000ULL, info, 0xFFFF'FFFFULL), 0u);
}

TEST_F(VirtualQueryTest, GuestMemoryCeilingClampsX64QueryBounds) {
    constexpr GuestAddr kLinear64Ceiling = 0x0000007F'FFFFFFFFULL;
    memory_.set_maximum_user_address(kLinear64Ceiling);
    MemoryBasicInformation info{};

    EXPECT_EQ(vqm_->virtual_query(kLinear64Ceiling + 1, info), 0u);
    ASSERT_EQ(vqm_->virtual_query(kLinear64Ceiling - 0xFFFF, info),
              sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_FREE);
    EXPECT_EQ(info.region_size, 0x10000u);
}

TEST_F(VirtualQueryTest, X64ThunkRejectsAddressAboveGuestMemoryCeiling) {
    constexpr GuestAddr kLinear64Ceiling = 0x0000007F'FFFFFFFFULL;
    constexpr GuestAddr kBuffer = 0x30000;
    memory_.set_maximum_user_address(kLinear64Ceiling);
    ASSERT_TRUE(memory_.map_region(
        kBuffer, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = kLinear64Ceiling + 1;
    cpu.gpr.rdx = kBuffer;
    cpu.gpr.r8 = sizeof(MemoryBasicInformation);
    Marshaler marshaler{memory_};
    marshaler.attach_cpu(cpu);
    set_last_error(error_codes::ERROR_SUCCESS);

    impl_VirtualQuery(marshaler, *vqm_);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(VirtualQueryTest, TerminalFreeRegionEndsAtApplicationLimit) {
    MemoryBasicInformation info{};
    constexpr u64 maximum_address = 0xFFFF'FFFFULL;

    usize ret = vqm_->virtual_query(0xFFFF'0000ULL, info, maximum_address);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.region_size, 0x10000u);
}

TEST_F(VirtualQueryTest, OutOfRangeMappingDoesNotExtendFreeRegion) {
    constexpr u64 maximum_address = 0x00007FFF'FFFEFFFFULL;
    ASSERT_TRUE(memory_.map_region(0xFFEE'FFEE'FFEE'0000ULL, 0x10000,
                                   MemoryProtection::None).is_ok());
    MemoryBasicInformation info{};

    usize ret = vqm_->virtual_query(0x00007FFB'00200000ULL, info,
                                    maximum_address);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.region_size,
              maximum_address + 1 - 0x00007FFB'00200000ULL);
}

// =============================================================================
// VirtualQuery — Multiple Regions
// =============================================================================

TEST_F(VirtualQueryTest, QueryMultipleDistinctRegions) {
    // Map a second region at a different address.
    constexpr GuestAddr SECOND_BASE = 0x40000;
    constexpr usize SECOND_SIZE     = 0x8000;
    auto res = memory_.map_region(SECOND_BASE, SECOND_SIZE,
                                  MemoryProtection::Read);
    ASSERT_FALSE(res.is_err());

    // Query each region independently.
    MemoryBasicInformation info1{};
    MemoryBasicInformation info2{};

    usize ret1 = vqm_->virtual_query(MEM_BASE, info1);
    usize ret2 = vqm_->virtual_query(SECOND_BASE, info2);

    EXPECT_EQ(ret1, sizeof(MemoryBasicInformation));
    EXPECT_EQ(ret2, sizeof(MemoryBasicInformation));

    EXPECT_EQ(info1.state, MEM_COMMIT);
    EXPECT_EQ(info2.state, MEM_COMMIT);

    EXPECT_EQ(info1.base_address, MEM_BASE);
    EXPECT_EQ(info2.base_address, SECOND_BASE);
}

TEST_F(VirtualQueryTest, QueryAddressBetweenRegions) {
    // An address between the mapped region and the end of address space
    // should be free.
    MemoryBasicInformation info{};
    usize ret = vqm_->virtual_query(MEM_BASE + MEM_SIZE + 0x10000, info);

    EXPECT_EQ(ret, sizeof(MemoryBasicInformation));
    EXPECT_EQ(info.state, MEM_FREE);
}

// =============================================================================
// FlushInstructionCache
// =============================================================================

TEST_F(VirtualQueryTest, FlushInstructionCacheAlwaysTrue) {
    EXPECT_TRUE(vqm_->flush_instruction_cache(MEM_BASE, 0x1000));
}

TEST_F(VirtualQueryTest, FlushInstructionCacheZeroSize) {
    EXPECT_TRUE(vqm_->flush_instruction_cache(0, 0));
}

TEST_F(VirtualQueryTest, FlushInstructionCacheUnmappedAddress) {
    EXPECT_TRUE(vqm_->flush_instruction_cache(0xDEAD0000, 0x100));
}

TEST_F(VirtualQueryTest, FlushInstructionCacheNotifiesExecutableWriteObserver) {
    GuestAddr seen_addr = ~GuestAddr{0};
    usize seen_size = 0;
    memory_.set_thread_executable_write_observer(
        [&](GuestAddr addr, usize size, u64) {
            seen_addr = addr;
            seen_size = size;
            return true;
        });
    EXPECT_TRUE(vqm_->flush_instruction_cache(MEM_BASE + 0x40, 0x80));
    EXPECT_EQ(seen_addr, MEM_BASE + 0x40);
    EXPECT_EQ(seen_size, 0x80u);
    memory_.clear_thread_executable_write_observer();
}

TEST_F(VirtualQueryTest, FlushInstructionCacheNullBaseDoesNotNotify) {
    bool notified = false;
    memory_.set_thread_executable_write_observer(
        [&](GuestAddr, usize, u64) {
            notified = true;
            return true;
        });
    EXPECT_TRUE(vqm_->flush_instruction_cache(0, 0x1234));
    EXPECT_FALSE(notified);
    memory_.clear_thread_executable_write_observer();
}

TEST_F(VirtualQueryTest, VirtualProtectToExecuteNotifiesObserver) {
    GuestAddr seen_addr = ~GuestAddr{0};
    usize seen_size = 0;
    u32 notify_count = 0;
    memory_.set_thread_executable_write_observer(
        [&](GuestAddr addr, usize size, u64) {
            seen_addr = addr;
            seen_size = size;
            ++notify_count;
            return true;
        });

    u64 addr = vmm_->virtual_alloc(0, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    notify_count = 0;

    u32 old_prot = 0;
    ASSERT_TRUE(vmm_->virtual_protect(addr, 0x1000, PAGE_EXECUTE_READ, &old_prot));
    EXPECT_EQ(notify_count, 1u);
    EXPECT_EQ(seen_addr, static_cast<GuestAddr>(addr));
    EXPECT_GE(seen_size, 0x1000u);

    notify_count = 0;
    ASSERT_TRUE(vmm_->virtual_protect(addr, 0x1000, PAGE_READONLY, &old_prot));
    EXPECT_EQ(notify_count, 0u) << "non-execute protect should not flush";

    memory_.clear_thread_executable_write_observer();
}

// =============================================================================
// VirtualProtect (via VirtualMemoryManager)
// =============================================================================

TEST_F(VirtualQueryTest, VirtualProtectChangesProtection) {
    // Allocate a region through the VMM so it's tracked.
    u64 addr = vmm_->virtual_alloc(0, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    u32 old_prot = 0;
    bool ok = vmm_->virtual_protect(addr, 0x1000, PAGE_READONLY, &old_prot);
    EXPECT_TRUE(ok);
    EXPECT_EQ(old_prot, PAGE_READWRITE);

    // Protect again and verify the old value is now PAGE_READONLY.
    u32 old_prot2 = 0;
    ok = vmm_->virtual_protect(addr, 0x1000, PAGE_EXECUTE_READ, &old_prot2);
    EXPECT_TRUE(ok);
    EXPECT_EQ(old_prot2, PAGE_READONLY);
}

TEST_F(VirtualQueryTest, VirtualProtectNullOldProtect) {
    u64 addr = vmm_->virtual_alloc(0, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    // Passing nullptr for old_protect should not crash.
    bool ok = vmm_->virtual_protect(addr, 0x1000, PAGE_READONLY, nullptr);
    EXPECT_TRUE(ok);
}

TEST_F(VirtualQueryTest, VirtualProtectThunkRejectsNullOldProtect) {
    const u64 addr = vmm_->virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory_};
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = addr;
    cpu.gpr.rdx = 0x1000;
    cpu.gpr.r8 = PAGE_READONLY;
    cpu.gpr.r9 = 0;

    impl_VirtualProtect(marshaler, *vqm_);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    // tests/oracle_data/virtual_protect_errors_probe.expected:
    // "null_old ok=0 e=3e6" -- Windows fails with ERROR_NOACCESS.
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
    VirtualAllocationQuery info{};
    ASSERT_TRUE(vmm_->virtual_query(addr, info));
    EXPECT_EQ(info.protect, PAGE_READWRITE);
}

TEST_F(VirtualQueryTest, VirtualProtectInvalidAddress) {
    bool ok = vmm_->virtual_protect(0xBAD00000, 0x1000,
                                    PAGE_READONLY, nullptr);
    EXPECT_FALSE(ok);
}

// =============================================================================
// DLL Export Table
// =============================================================================

TEST_F(VirtualQueryTest, MakeExportsHasExpectedEntries) {
    DllExports exports = make_virtual_protect_exports(*vmm_, *vqm_);
    // VirtualAlloc, VirtualAlloc2, VirtualFree, VirtualProtect, VirtualQuery,
    // VirtualQueryEx, GetWriteWatch, ResetWriteWatch.
    ASSERT_EQ(exports.entries.size(), 8u);
}

TEST_F(VirtualQueryTest, MakeExportsContainsExpectedNames) {
    DllExports exports = make_virtual_protect_exports(*vmm_, *vqm_);

    bool has_alloc   = false;
    bool has_alloc2  = false;
    bool has_free    = false;
    bool has_protect = false;
    bool has_query   = false;
    bool has_flush   = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "VirtualAlloc")          has_alloc = true;
        if (entry.name == "VirtualAlloc2")         has_alloc2 = true;
        if (entry.name == "VirtualFree")           has_free = true;
        if (entry.name == "VirtualProtect")        has_protect = true;
        if (entry.name == "VirtualQuery")           has_query   = true;
        if (entry.name == "FlushInstructionCache")  has_flush   = true;
    }
    EXPECT_TRUE(has_alloc);
    EXPECT_TRUE(has_alloc2);
    EXPECT_TRUE(has_free);
    EXPECT_TRUE(has_protect);
    EXPECT_TRUE(has_query);
    EXPECT_TRUE(has_flush);
}

// =============================================================================
// Win32 Constants
// =============================================================================

TEST(VirtualQueryConstantsTest, MemoryTypeConstants) {
    EXPECT_EQ(MEM_FREE,    0x10000u);
    EXPECT_EQ(MEM_PRIVATE, 0x20000u);
    EXPECT_EQ(MEM_MAPPED,  0x40000u);
    EXPECT_EQ(MEM_IMAGE,   0x1000000u);
}

TEST(VirtualQueryConstantsTest, ExistingMemoryConstants) {
    // Verify the constants from memory.hpp are accessible.
    EXPECT_EQ(MEM_COMMIT,  0x1000u);
    EXPECT_EQ(MEM_RESERVE, 0x2000u);
}

// Audit 01 VirtualProtect #4. Unwritten image pages report WRITECOPY forms;
// VirtualProtect(PAGE_READWRITE) stores PAGE_WRITECOPY; AllocationProtect is
// PAGE_EXECUTE_WRITECOPY (virtual_memory_contract_probe img_*).
TEST_F(VirtualQueryTest, ImageWritecopyOldProtectAndQuery) {
    constexpr GuestAddr kImage = 0x500000;
    ASSERT_TRUE(memory_.map_region(kImage, 0x4000, MemoryProtection::Read).is_ok());
    ASSERT_TRUE(memory_.protect_region(kImage + 0x1000, 0x1000,
                                       MemoryProtection::ReadExecute).is_ok());
    ASSERT_TRUE(memory_.protect_region(kImage + 0x2000, 0x2000,
                                       MemoryProtection::ReadWrite).is_ok());

    ThunkRegistry registry(0x71000000);
    ModuleManager modules(registry);
    modules.bind_guest_memory(memory_);
    modules.set_main_module(kImage, "C:\\img.exe");
    modules.register_main_module_exports(
        kImage, "C:\\img.exe", kImage + 0x4000, 64, {});

    pe::MappedSection text{};
    text.guest_address = kImage + 0x1000;
    text.size = 0x1000;
    text.protection = MemoryProtection::ReadExecute;
    text.win32_protection = PAGE_EXECUTE_READ;
    std::memcpy(text.name, ".text", 6);
    pe::MappedSection data{};
    data.guest_address = kImage + 0x2000;
    data.size = 0x2000;
    data.protection = MemoryProtection::ReadWrite;
    data.win32_protection = PAGE_WRITECOPY;
    std::memcpy(data.name, ".data", 6);
    modules.set_mapped_image_layout(kImage, {text, data}, 0x1000);
    vqm_->set_module_manager(&modules);

    MemoryBasicInformation info{};
    ASSERT_EQ(vqm_->virtual_query(kImage + 0x2000, info), sizeof(info));
    EXPECT_EQ(info.type, MEM_IMAGE);
    EXPECT_EQ(info.allocation_protect, PAGE_EXECUTE_WRITECOPY);
    EXPECT_EQ(info.protect, PAGE_WRITECOPY);

    u32 old = 0;
    ASSERT_TRUE(vqm_->virtual_protect(kImage + 0x2000, 0x1000,
                                      PAGE_READWRITE, &old));
    EXPECT_EQ(old, PAGE_WRITECOPY);
    ASSERT_EQ(vqm_->virtual_query(kImage + 0x2000, info), sizeof(info));
    EXPECT_EQ(info.protect, PAGE_WRITECOPY);

    ASSERT_TRUE(vqm_->virtual_protect(kImage + 0x2000, 0x1000,
                                      PAGE_EXECUTE_READWRITE, &old));
    EXPECT_EQ(old, PAGE_WRITECOPY);
    ASSERT_EQ(vqm_->virtual_query(kImage + 0x2000, info), sizeof(info));
    EXPECT_EQ(info.protect, PAGE_EXECUTE_WRITECOPY);

    ASSERT_TRUE(vqm_->virtual_protect(kImage, 0x1000, PAGE_READONLY, &old));
    EXPECT_EQ(old, PAGE_READONLY);  // headers
}

// Audit 01 VirtualProtect #1. Learn: all pages must be "within the same
// reserved region" -- for a runtime mapping such as a PE image that is the
// whole mapping, not one protection run. virtual_memory_contract_probe on
// Windows: "img protect whole ok=0x1 old=0x2" (header page READONLY) and
// "img protect cross ok=0x1 old=0x80"; "img protect past end" fails.
TEST_F(VirtualQueryTest, VirtualProtectOnMappingMayCrossProtectionRuns) {
    constexpr GuestAddr kImage = 0x400000;
    ASSERT_TRUE(memory_.map_region(kImage, 0x4000, MemoryProtection::Read).is_ok());
    ASSERT_TRUE(memory_.protect_region(kImage + 0x1000, 0x1000,
                                       MemoryProtection::ReadExecute).is_ok());
    ASSERT_TRUE(memory_.protect_region(kImage + 0x2000, 0x2000,
                                       MemoryProtection::ReadWrite).is_ok());
    u32 old = 0;
    ASSERT_TRUE(vmm_->virtual_protect(kImage, 0x4000, PAGE_READWRITE, &old));
    EXPECT_EQ(old, PAGE_READONLY);
    ASSERT_TRUE(vmm_->virtual_protect(kImage + 0x1800, 0x1000, PAGE_EXECUTE_READ, &old));
    EXPECT_EQ(old, PAGE_READWRITE);
    EXPECT_TRUE(memory_.has_protection(kImage + 0x2000, 0x1000, MemoryProtection::ReadExecute));
    EXPECT_FALSE(vmm_->virtual_protect(kImage + 0x3000, 0x2000, PAGE_READWRITE, &old));
}

namespace vitreo::winapi {
class ModuleManagerTestAccessor {
public:
    /// The post-link template LoadLibrary captures before DllMain.
    static void capture_template(ModuleManager& modules, GuestAddr base) {
        auto& info = modules.modules_by_name_.at(modules.base_to_name_.at(base));
        modules.capture_mapped_image_template(info);
    }
};
}  // namespace vitreo::winapi

namespace {

/// Layout of the writecopy_protect_probe companion DLL: .cowtxt (code),
/// .cowdat (3 writable pages), .cowro (2 read-only pages), then one more
/// read-only page.
class ImageWritecopyProtectTest : public VirtualQueryTest {
protected:
    static constexpr GuestAddr kImage = 0x600000;
    static constexpr GuestAddr kText = kImage + 0x1000;
    static constexpr GuestAddr kData = kImage + 0x2000;
    static constexpr GuestAddr kRo = kImage + 0x5000;
    static constexpr GuestAddr kEnd = kImage + 0x8000;
    static constexpr u64 kPg = 0x1000;

    void SetUp() override {
        VirtualQueryTest::SetUp();
        ASSERT_TRUE(memory_.map_region(kImage, kEnd - kImage,
                                       MemoryProtection::Read).is_ok());
        ASSERT_TRUE(memory_.protect_region(kText, kPg,
                                           MemoryProtection::ReadExecute).is_ok());
        ASSERT_TRUE(memory_.protect_region(kData, 3 * kPg,
                                           MemoryProtection::ReadWrite).is_ok());
        Byte* bytes = memory_.host_ptr_mut(kImage, kEnd - kImage);
        ASSERT_NE(bytes, nullptr);
        for (usize i = 0; i < kEnd - kImage; ++i) bytes[i] = static_cast<Byte>(i * 7 + 1);

        modules_ = std::make_unique<ModuleManager>(registry_);
        modules_->bind_guest_memory(memory_);
        modules_->set_main_module(kImage, "C:\\wcdll.dll");
        modules_->register_main_module_exports(kImage, "C:\\wcdll.dll", kEnd, 64, {});
        modules_->set_mapped_image_layout(
            kImage,
            {section(".cowtxt", kText, kPg, MemoryProtection::ReadExecute,
                     PAGE_EXECUTE_READ),
             section(".cowdat", kData, 3 * kPg, MemoryProtection::ReadWrite,
                     PAGE_WRITECOPY),
             section(".cowro", kRo, 2 * kPg, MemoryProtection::Read,
                     PAGE_READONLY),
             section(".rdata", kRo + 2 * kPg, kPg, MemoryProtection::Read,
                     PAGE_READONLY)},
            0x1000);
        ModuleManagerTestAccessor::capture_template(*modules_, kImage);
        vqm_->set_module_manager(modules_.get());
    }

    static pe::MappedSection section(const char* name, GuestAddr addr, u64 size,
                                     MemoryProtection prot, u32 win32) {
        pe::MappedSection s{};
        s.guest_address = addr;
        s.size = size;
        s.protection = prot;
        s.win32_protection = win32;
        std::memcpy(s.name, name, std::strlen(name) + 1);
        return s;
    }

    /// VirtualProtect through the kernel32 owner; @return {ok, old, error}.
    std::tuple<bool, u32, u32> protect(GuestAddr addr, u64 size, u32 prot) {
        u32 old = 0xBEEF;
        const bool ok = vqm_->virtual_protect(addr, size, prot, &old);
        return {ok, old, ok ? 0u : vqm_->take_last_protect_error()};
    }

    std::pair<u32, u64> query(GuestAddr addr) {
        MemoryBasicInformation info{};
        EXPECT_EQ(vqm_->virtual_query(addr, info), sizeof(info));
        EXPECT_EQ(info.type, MEM_IMAGE);
        EXPECT_EQ(info.allocation_protect, PAGE_EXECUTE_WRITECOPY);
        return {info.protect, info.region_size};
    }

    void poke(GuestAddr addr, u8 value) {
        ASSERT_TRUE(memory_.write_u8(addr, value).is_ok());
    }

    ThunkRegistry registry_{0x72000000};
    std::unique_ptr<ModuleManager> modules_;
};

using Prot = std::pair<u32, u64>;
using Vp = std::tuple<bool, u32, u32>;

}  // namespace

// The sequence dbghelp runs on its own import slot (RV There Yet fast-fail
// 0x19): RW over an untouched WRITECOPY page, write, then restore WRITECOPY.
// Every expected value is a dll:img line of
// tools/oracle_probe/writecopy_protect_probe{,32}.expected (identical on x86
// and x64; the image path has no bitness-dependent marshaling).
TEST_F(ImageWritecopyProtectTest, PatchSequenceRestoresWritecopy) {
    EXPECT_EQ(query(kData), Prot(PAGE_WRITECOPY, 0x3000));
    EXPECT_EQ(query(kData + kPg), Prot(PAGE_WRITECOPY, 0x2000));
    EXPECT_EQ(query(kRo), Prot(PAGE_READONLY, 0x3000));
    EXPECT_EQ(query(kText), Prot(PAGE_EXECUTE_READ, 0x1000));

    EXPECT_EQ(protect(kData, 0x38, PAGE_READWRITE), Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_WRITECOPY, 0x3000));
    poke(kData + 8, 0x55);
    EXPECT_EQ(query(kData), Prot(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(protect(kData, 0x38, PAGE_WRITECOPY), Vp(true, PAGE_READWRITE, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_READWRITE, 0x1000));
    poke(kData + 9, 0x66);
    EXPECT_EQ(protect(kData, 0x38, PAGE_WRITECOPY), Vp(true, PAGE_READWRITE, 0));

    EXPECT_EQ(protect(kData, kPg, PAGE_EXECUTE_WRITECOPY),
              Vp(true, PAGE_READWRITE, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_EXECUTE_READWRITE, 0x1000));
    EXPECT_EQ(protect(kData, kPg, PAGE_EXECUTE_READWRITE),
              Vp(true, PAGE_EXECUTE_READWRITE, 0));
    EXPECT_EQ(protect(kData, kPg, PAGE_READONLY),
              Vp(true, PAGE_EXECUTE_READWRITE, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_READONLY, 0x1000));
    EXPECT_EQ(protect(kData, kPg, PAGE_WRITECOPY), Vp(true, PAGE_READONLY, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_READWRITE, 0x1000));
}

TEST_F(ImageWritecopyProtectTest, UntouchedPagesKeepWritecopyForms) {
    EXPECT_EQ(protect(kData + kPg, kPg, PAGE_READWRITE),
              Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(protect(kData + kPg, kPg, PAGE_WRITECOPY),
              Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(query(kData + kPg), Prot(PAGE_WRITECOPY, 0x2000));

    EXPECT_EQ(protect(kData + 2 * kPg, kPg, PAGE_WRITECOPY),
              Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(protect(kData + 2 * kPg, kPg, PAGE_EXECUTE_WRITECOPY),
              Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(query(kData + 2 * kPg), Prot(PAGE_EXECUTE_WRITECOPY, 0x1000));
    EXPECT_EQ(protect(kData + 2 * kPg, kPg, PAGE_WRITECOPY),
              Vp(true, PAGE_EXECUTE_WRITECOPY, 0));
}

TEST_F(ImageWritecopyProtectTest, MultiPageRangeReportsEachPage) {
    poke(kData + 8, 0x55);  // page 0 private, pages 1-2 untouched
    EXPECT_EQ(protect(kData, 3 * kPg, PAGE_READONLY),
              Vp(true, PAGE_READWRITE, 0));
    EXPECT_EQ(protect(kData, 3 * kPg, PAGE_WRITECOPY),
              Vp(true, PAGE_READONLY, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(query(kData + kPg), Prot(PAGE_WRITECOPY, 0x2000));
    EXPECT_EQ(query(kData + 2 * kPg), Prot(PAGE_WRITECOPY, 0x1000));

    EXPECT_EQ(protect(kData + kPg, 2 * kPg, PAGE_READWRITE),
              Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(query(kData + kPg), Prot(PAGE_WRITECOPY, 0x2000));
    poke(kData + 2 * kPg + 3, 0x77);
    EXPECT_EQ(query(kData + 2 * kPg), Prot(PAGE_READWRITE, 0x1000));

    EXPECT_EQ(protect(kData, 3 * kPg, PAGE_EXECUTE_WRITECOPY),
              Vp(true, PAGE_READWRITE, 0));
    EXPECT_EQ(query(kData), Prot(PAGE_EXECUTE_READWRITE, 0x1000));
    EXPECT_EQ(query(kData + kPg), Prot(PAGE_EXECUTE_WRITECOPY, 0x1000));
    EXPECT_EQ(query(kData + 2 * kPg), Prot(PAGE_EXECUTE_READWRITE, 0x1000));
}

TEST_F(ImageWritecopyProtectTest, ReadOnlyAndCodePagesAcceptWritecopy) {
    // As in the probe at this point, the first data page is private, so a
    // WRITECOPY code page does not coalesce with the data run after it.
    poke(kData + 8, 0x55);
    EXPECT_EQ(protect(kRo, kPg, PAGE_WRITECOPY), Vp(true, PAGE_READONLY, 0));
    EXPECT_EQ(query(kRo), Prot(PAGE_WRITECOPY, 0x1000));
    poke(kRo + 5, 0x11);
    EXPECT_EQ(query(kRo), Prot(PAGE_READWRITE, 0x1000));
    EXPECT_EQ(protect(kRo + kPg, kPg, PAGE_EXECUTE_WRITECOPY),
              Vp(true, PAGE_READONLY, 0));
    EXPECT_EQ(query(kRo + kPg), Prot(PAGE_EXECUTE_WRITECOPY, 0x1000));
    EXPECT_EQ(protect(kRo + kPg, kPg, PAGE_READONLY),
              Vp(true, PAGE_EXECUTE_WRITECOPY, 0));

    EXPECT_EQ(protect(kText, kPg, PAGE_WRITECOPY),
              Vp(true, PAGE_EXECUTE_READ, 0));
    EXPECT_EQ(query(kText), Prot(PAGE_WRITECOPY, 0x1000));
    EXPECT_EQ(protect(kText, kPg, PAGE_EXECUTE_WRITECOPY),
              Vp(true, PAGE_WRITECOPY, 0));
    EXPECT_EQ(query(kText), Prot(PAGE_EXECUTE_WRITECOPY, 0x1000));
    EXPECT_EQ(protect(kText, kPg, PAGE_EXECUTE_READ),
              Vp(true, PAGE_EXECUTE_WRITECOPY, 0));
}

// writecopy_protect_probe priv ...: the WRITECOPY forms fail on MEM_PRIVATE
// with ERROR_INVALID_PARAMETER, store PAGE_NOACCESS as the old protection,
// and leave the page unchanged; a malformed mask
// (virtual_memory_contract_probe vprot prot3) leaves the old value untouched.
TEST_F(VirtualQueryTest, PrivateMemoryRejectsWritecopyWithNoaccessOld) {
    const u64 base = vmm_->virtual_alloc(0, 0x2000, MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
    ASSERT_NE(base, 0u);
    for (const u32 prot : {PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY}) {
        u32 old = 0xBEEF;
        EXPECT_FALSE(vqm_->virtual_protect(base, 0x1000, prot, &old));
        EXPECT_EQ(old, PAGE_NOACCESS);
        EXPECT_EQ(vqm_->take_last_protect_error(),
                  error_codes::ERROR_INVALID_PARAMETER);
    }
    u32 old = 0xBEEF;
    EXPECT_FALSE(vqm_->virtual_protect(base, 0x2000, PAGE_WRITECOPY, &old));
    EXPECT_EQ(old, PAGE_NOACCESS);
    (void)vqm_->take_last_protect_error();
    MemoryBasicInformation info{};
    ASSERT_EQ(vqm_->virtual_query(base, info), sizeof(info));
    EXPECT_EQ(info.protect, PAGE_READWRITE);
    EXPECT_EQ(info.region_size, 0x2000u);

    old = 0xBEEF;
    EXPECT_FALSE(vqm_->virtual_protect(base, 0x1000, 0x03, &old));
    EXPECT_EQ(old, 0xBEEFu);
}
