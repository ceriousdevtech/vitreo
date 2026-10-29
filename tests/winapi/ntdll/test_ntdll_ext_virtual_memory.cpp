// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_virtual_memory.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): NtAllocate/Free/Protect/QueryVirtualMemory

#include "ntdll_ext_test_fixture.hpp"

TEST_F(NtdllExtTest, NtFreeVirtualMemoryDecommitsRoundedCurrentProcessRange) {
    GuestMemory memory;
    VirtualMemoryManager vmm(memory);
    mgr.set_virtual_memory_manager(&vmm);
    const u64 allocation = vmm.virtual_alloc(
        0, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(allocation, 0u);

    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtFreeVirtualMemory";
        });
    ASSERT_NE(entry, exports.entries.end());

    constexpr GuestAddr kArgs = 0x50000;
    ASSERT_TRUE(memory.map_region(
        kArgs, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(kArgs, allocation + 1).is_ok());
    ASSERT_TRUE(memory.write_u64(kArgs + 8, 1).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = ~u64{0};
    cpu.gpr.rdx = kArgs;
    cpu.gpr.r8 = kArgs + 8;
    cpu.gpr.r9 = MEM_DECOMMIT;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u64(kArgs).value(), allocation);
    EXPECT_EQ(memory.read_u64(kArgs + 8).value(), 0x1000u);
    EXPECT_TRUE(memory.read_u8(allocation).is_err());
}

TEST_F(NtdllExtTest, NtAllocateVirtualMemoryWritesSelectedBaseAndRoundedSize) {
    GuestMemory memory;
    VirtualMemoryManager vmm(memory);
    mgr.set_virtual_memory_manager(&vmm);
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtAllocateVirtualMemory";
        });
    ASSERT_NE(entry, exports.entries.end());

    constexpr GuestAddr kArgs = 0x50000;
    constexpr GuestAddr kStack = 0x51000;
    ASSERT_TRUE(memory.map_region(
        kArgs, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(kArgs, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(kArgs + 8, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(kStack + 0x28,
                                 MEM_RESERVE | MEM_COMMIT).is_ok());
    ASSERT_TRUE(memory.write_u64(kStack + 0x30, PAGE_READWRITE).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = ~u64{0};
    cpu.gpr.rdx = kArgs;
    cpu.gpr.r8 = 0; // ZeroBits
    cpu.gpr.r9 = kArgs + 8;
    cpu.gpr.rsp = kStack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    entry->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    const u64 allocated = memory.read_u64(kArgs).value();
    EXPECT_NE(allocated, 0u);
    EXPECT_EQ(memory.read_u64(kArgs + 8).value(), 0x1000u);
    EXPECT_TRUE(memory.write_u8(allocated, 0x5Au).is_ok());
}

TEST_F(NtdllExtTest, NtProtectVirtualMemoryProtectsAndReturnsRoundedRange) {
    GuestMemory memory;
    VirtualMemoryManager vmm(memory);
    mgr.set_virtual_memory_manager(&vmm);
    const u64 allocation = vmm.virtual_alloc(
        0, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(allocation, 0u);

    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtProtectVirtualMemory";
        });
    ASSERT_NE(entry, exports.entries.end());

    constexpr GuestAddr kArgs = 0x50000;
    constexpr GuestAddr kStack = 0x51000;
    ASSERT_TRUE(memory.map_region(
        kArgs, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(kArgs, allocation + 1).is_ok());
    ASSERT_TRUE(memory.write_u64(kArgs + 8, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(kStack + 0x28, kArgs + 16).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = ~u64{0};
    cpu.gpr.rdx = kArgs;
    cpu.gpr.r8 = kArgs + 8;
    cpu.gpr.r9 = PAGE_EXECUTE_READ;
    cpu.gpr.rsp = kStack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u64(kArgs).value(), allocation);
    EXPECT_EQ(memory.read_u64(kArgs + 8).value(), 0x1000u);
    EXPECT_EQ(memory.read_u32(kArgs + 16).value(), PAGE_READWRITE);
    EXPECT_TRUE(memory.read_u8(allocation).is_ok());
    EXPECT_TRUE(memory.write_u8(allocation, 0x5Au).is_err());
}

// =============================================================================
// NtQueryVirtualMemory Tests
// =============================================================================

TEST_F(NtdllExtTest, QueryVirtualMemory_CommittedRegion) {
    NtdllExtManager::MemoryBasicInfo info{};
    u32 status = mgr.query_virtual_memory(0x00400000, info);

    EXPECT_EQ(status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(info.state, 0x1000u);    // MEM_COMMIT
    EXPECT_EQ(info.protect, 0x04u);    // PAGE_READWRITE
    EXPECT_EQ(info.type, 0x20000u);    // MEM_PRIVATE
}

TEST_F(NtdllExtTest, QueryVirtualMemory_FreeRegion) {
    NtdllExtManager::MemoryBasicInfo info{};
    u32 status = mgr.query_virtual_memory(0xFFFF0000ULL, info);

    EXPECT_EQ(status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(info.state, 0x10000u);   // MEM_FREE
    EXPECT_EQ(info.type, 0u);
}

TEST_F(NtdllExtTest, QueryVirtualMemory_PageAligned) {
    NtdllExtManager::MemoryBasicInfo info{};
    u32 status = mgr.query_virtual_memory(0x00401234, info);

    EXPECT_EQ(status, nt_status::STATUS_SUCCESS);
    // Should be page-aligned down
    EXPECT_EQ(info.base_address, 0x00401000u);
}
