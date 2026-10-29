/// @file test_vmem_protect.cpp
/// @brief Unit tests for VirtualProtect, win32_prot_to_native conversion and PAGE_GUARD (WIN32-020)

#include "test_vmem_fixture.hpp"
#include <core/memory/guard_page_traps.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>

// =============================================================================
// VirtualProtect Tests
// =============================================================================

TEST_F(VirtualMemoryTest, ProtectChange) {
    u64 addr = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    u32 old_prot = 0;
    bool ok = vmm.virtual_protect(addr, 0x1000, PAGE_READONLY, &old_prot);
    EXPECT_TRUE(ok);
    EXPECT_EQ(old_prot, PAGE_READWRITE);
}

TEST_F(VirtualMemoryTest, ProtectChangesOnlyRequestedCommittedPages) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    u32 old_prot = 0;
    ASSERT_TRUE(vmm.virtual_protect(
        addr + 0x1000, 0x1000, PAGE_READONLY, &old_prot));
    EXPECT_EQ(old_prot, PAGE_READWRITE);
    EXPECT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0x11).is_ok());
    EXPECT_TRUE(mem.write_u8(
        static_cast<GuestAddr>(addr + 0x1000), 0x22).is_err());
    EXPECT_TRUE(mem.write_u8(
        static_cast<GuestAddr>(addr + 0x2000), 0x33).is_ok());

    VirtualAllocationQuery info{};
    ASSERT_TRUE(vmm.virtual_query(addr + 0x1000, info));
    EXPECT_EQ(info.state, MEM_COMMIT);
    EXPECT_EQ(info.protect, PAGE_READONLY);
    EXPECT_EQ(info.region_size, 0x1000u);
}

TEST_F(VirtualMemoryTest, ProtectRejectsRangeContainingReservedPages) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x2000, MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_EQ(vmm.virtual_alloc(
                  addr, 0x1000, MEM_COMMIT, PAGE_READWRITE),
              addr);

    u32 old_prot = 0;
    EXPECT_FALSE(vmm.virtual_protect(
        addr, 0x2000, PAGE_READONLY, &old_prot));
}

TEST(VirtualProtectThunkTest, FailureLastErrorsMatchOracle) {
    // Audit 01 VirtualProtect #3 — decommitted/bad addr → INVALID_ADDRESS;
    // bad protect → INVALID_PARAMETER; null/RO oldProtect → NOACCESS.
    GuestMemory memory;
    VirtualMemoryManager vmm{memory};
    VirtualQueryManager vqm{memory, &vmm};
    const u64 addr = vmm.virtual_alloc(0, 0x2000, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_TRUE(vmm.virtual_free(addr + 0x1000, 0x1000, MEM_DECOMMIT));

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    const u64 old_slot = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
    ASSERT_NE(old_slot, 0u);

    auto call = [&](u64 a, u64 sz, u32 prot, u64 oldp) {
        cpu.gpr.rcx = a;
        cpu.gpr.rdx = sz;
        cpu.gpr.r8 = prot;
        cpu.gpr.r9 = oldp;
        set_last_error(0xDEADu);
        impl_VirtualProtect(marshaler, vqm);
        return std::pair<u64, u32>{cpu.gpr.rax, get_last_error()};
    };

    {
        const auto [ok, err] =
            call(addr + 0x1000, 0x1000, PAGE_READONLY, old_slot);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_ADDRESS);
    }
    {
        const auto [ok, err] = call(0x12345000ull, 0x1000, PAGE_READONLY, old_slot);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_ADDRESS);
    }
    {
        const auto [ok, err] = call(addr, 0x1000, 0xFFu, old_slot);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_PARAMETER);
    }
    {
        const auto [ok, err] = call(addr, 0x1000, PAGE_READONLY, 0);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_NOACCESS);
    }
    EXPECT_TRUE(vmm.virtual_free(addr, 0, MEM_RELEASE));
    EXPECT_TRUE(vmm.virtual_free(old_slot, 0, MEM_RELEASE));
}

// =============================================================================
// win32_prot_to_native conversion
// =============================================================================

TEST_F(VirtualMemoryTest, ProtConversion) {
    EXPECT_EQ(win32_prot_to_native(PAGE_NOACCESS),          MemoryProtection::None);
    EXPECT_EQ(win32_prot_to_native(PAGE_READONLY),          MemoryProtection::Read);
    EXPECT_EQ(win32_prot_to_native(PAGE_READWRITE),         MemoryProtection::ReadWrite);
    EXPECT_EQ(win32_prot_to_native(PAGE_WRITECOPY),         MemoryProtection::ReadWrite);
    EXPECT_EQ(win32_prot_to_native(PAGE_EXECUTE),           MemoryProtection::Execute);
    EXPECT_EQ(win32_prot_to_native(PAGE_EXECUTE_READ),      MemoryProtection::ReadExecute);
    EXPECT_EQ(win32_prot_to_native(PAGE_EXECUTE_READWRITE), MemoryProtection::ReadWriteExecute);
    EXPECT_EQ(win32_prot_to_native(PAGE_EXECUTE_WRITECOPY), MemoryProtection::ReadWriteExecute);
    // Modifiers are stripped for the host mapping.
    EXPECT_EQ(win32_prot_to_native(PAGE_READWRITE | PAGE_GUARD),
              MemoryProtection::ReadWrite);
    // Unknown value falls back to None.
    EXPECT_EQ(win32_prot_to_native(0xFF),                   MemoryProtection::None);
}

// Audit 01 VirtualAlloc #1 / VirtualProtect #2. PAGE_GUARD and other
// documented modifiers are accepted; PAGE_GUARD is one-shot (armed until
// first access). tools/oracle_probe/virtual_memory_contract_probe.expected:
// valloc rc prot=0x104 ok aprot=0x104 prot=0x104; guard|noaccess fails.
TEST_F(VirtualMemoryTest, PageGuardAllocProtectAndTrip) {
    EXPECT_TRUE(is_supported_win32_protection(PAGE_READWRITE | PAGE_GUARD));
    EXPECT_TRUE(is_supported_win32_protection(PAGE_READWRITE | PAGE_NOCACHE));
    EXPECT_TRUE(is_supported_win32_protection(PAGE_EXECUTE_READ |
                                              PAGE_TARGETS_NO_UPDATE));
    EXPECT_FALSE(is_supported_win32_protection(PAGE_NOACCESS | PAGE_GUARD));
    EXPECT_FALSE(is_supported_win32_protection(PAGE_READWRITE | PAGE_GUARD |
                                               PAGE_NOCACHE));
    EXPECT_FALSE(is_supported_win32_protection(PAGE_WRITECOPY));

    const u64 addr = vmm.virtual_alloc(
        0, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
    ASSERT_NE(addr, 0u);
    VirtualAllocationQuery q{};
    ASSERT_TRUE(vmm.virtual_query(addr, q));
    EXPECT_EQ(q.allocation_protect, PAGE_READWRITE | PAGE_GUARD);
    EXPECT_EQ(q.protect, PAGE_READWRITE | PAGE_GUARD);
    EXPECT_TRUE(guard_page::is_armed(static_cast<GuestAddr>(addr)));

    // Soft trip (same clear as a SIGSEGV would perform), then restore the
    // host mapping the way VMM does after a fault when host_base is unset.
    ASSERT_TRUE(guard_page::handle_fault(static_cast<GuestAddr>(addr + 0x10)));
    ASSERT_TRUE(mem.protect_region(static_cast<GuestAddr>(addr), 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(vmm.virtual_query(addr, q));
    EXPECT_EQ(q.allocation_protect, PAGE_READWRITE | PAGE_GUARD);
    EXPECT_EQ(q.protect, PAGE_READWRITE);
    EXPECT_FALSE(guard_page::is_armed(static_cast<GuestAddr>(addr)));
    // Sibling pages in the same allocation stay armed.
    EXPECT_TRUE(guard_page::is_armed(static_cast<GuestAddr>(addr + 0x1000)));

    u32 old = 0;
    ASSERT_TRUE(vmm.virtual_protect(addr, 0x1000, PAGE_READONLY | PAGE_GUARD,
                                    &old));
    EXPECT_EQ(old, PAGE_READWRITE);
    ASSERT_TRUE(vmm.virtual_query(addr, q));
    EXPECT_EQ(q.protect, PAGE_READONLY | PAGE_GUARD);

    EXPECT_FALSE(vmm.virtual_protect(addr, 0x1000, PAGE_NOACCESS | PAGE_GUARD,
                                     &old));
}

TEST_F(VirtualMemoryTest, UnsupportedProtectionDoesNotBecomeNoAccessSuccess) {
    EXPECT_FALSE(is_supported_win32_protection(0xFF));
    EXPECT_EQ(vmm.virtual_alloc(
                  0, 0x1000, MEM_COMMIT | MEM_RESERVE, 0x80000000u),
              0u);

    const u64 addr = vmm.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    u32 old_protect = 0;
    EXPECT_FALSE(vmm.virtual_protect(addr, 0x1000, 0xFF, &old_protect));
    EXPECT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0x5A).is_ok());
}
