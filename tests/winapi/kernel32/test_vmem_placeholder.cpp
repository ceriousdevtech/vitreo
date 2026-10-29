/// @file test_vmem_placeholder.cpp
/// @brief Unit tests for the MEM_RESERVE_PLACEHOLDER split, coalesce and replace contract (WIN32-020)

#include "test_vmem_fixture.hpp"
#include <winapi/kernel32/memory/virtual_query.hpp>

TEST_F(VirtualMemoryTest, PlaceholderCanSplitAndCoalesce) {
    const u64 placeholder = vmm.virtual_alloc2(
        0, 0x20000,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS);
    ASSERT_NE(placeholder, 0u);

    VirtualAllocationQuery original{};
    ASSERT_TRUE(vmm.virtual_query(placeholder, original));
    EXPECT_EQ(original.state, MEM_RESERVE);
    EXPECT_EQ(original.allocation_protect, PAGE_NOACCESS);
    EXPECT_EQ(original.region_size, 0x20000u);

    ASSERT_TRUE(vmm.virtual_free(
        placeholder, 0x10000,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    VirtualAllocationQuery first{};
    VirtualAllocationQuery second{};
    ASSERT_TRUE(vmm.virtual_query(placeholder, first));
    ASSERT_TRUE(vmm.virtual_query(placeholder + 0x10000, second));
    EXPECT_EQ(first.allocation_base, placeholder);
    EXPECT_EQ(first.region_size, 0x10000u);
    EXPECT_EQ(second.allocation_base, placeholder + 0x10000);
    EXPECT_EQ(second.region_size, 0x10000u);

    ASSERT_TRUE(vmm.virtual_free(
        placeholder, 0x20000,
        MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
    VirtualAllocationQuery combined_base{};
    VirtualAllocationQuery combined_tail{};
    ASSERT_TRUE(vmm.virtual_query(placeholder, combined_base));
    ASSERT_TRUE(vmm.virtual_query(placeholder + 0x10000, combined_tail));
    EXPECT_EQ(combined_base.allocation_base, placeholder);
    EXPECT_EQ(combined_base.region_size, 0x20000u);
    EXPECT_EQ(combined_tail.allocation_base, placeholder);
}

TEST_F(VirtualMemoryTest, PrivateReplacementCanReturnToPlaceholder) {
    const u64 placeholder = vmm.virtual_alloc2(
        0, 0x10000,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS);
    ASSERT_NE(placeholder, 0u);

    ASSERT_EQ(vmm.virtual_alloc2(
                  placeholder, 0x10000,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE),
              placeholder);
    ASSERT_TRUE(mem.write_u32(
        static_cast<GuestAddr>(placeholder), 0xA1B2C3D4u).is_ok());

    EXPECT_FALSE(vmm.virtual_free(
        placeholder, 0,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    EXPECT_FALSE(vmm.virtual_free(
        placeholder, 0x8000,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    ASSERT_TRUE(vmm.virtual_free(
        placeholder, 0x10000,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    EXPECT_TRUE(mem.read_u32(static_cast<GuestAddr>(placeholder)).is_err());
    VirtualAllocationQuery restored{};
    ASSERT_TRUE(vmm.virtual_query(placeholder, restored));
    EXPECT_EQ(restored.state, MEM_RESERVE);
    EXPECT_EQ(restored.allocation_protect, PAGE_NOACCESS);
}

TEST_F(VirtualMemoryTest, PlaceholderCanBecomeReservedPrivateAllocation) {
    const u64 placeholder = vmm.virtual_alloc2(
        0, 0x10000,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS);
    ASSERT_NE(placeholder, 0u);

    ASSERT_EQ(vmm.virtual_alloc2(
                  placeholder, 0x10000,
                  MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                  PAGE_NOACCESS),
              placeholder);
    EXPECT_TRUE(mem.write_u32(
        static_cast<GuestAddr>(placeholder), 0xA1B2C3D4u).is_err());

    ASSERT_EQ(vmm.virtual_alloc2(
                  placeholder, 0x10000, MEM_COMMIT, PAGE_READWRITE),
              placeholder);
    EXPECT_TRUE(mem.write_u32(
        static_cast<GuestAddr>(placeholder), 0xA1B2C3D4u).is_ok());
}

TEST_F(VirtualMemoryTest, ReplacementRequiresExactPlaceholderRange) {
    const u64 placeholder = vmm.virtual_alloc2(
        0, 0x20000,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS);
    ASSERT_NE(placeholder, 0u);
    EXPECT_EQ(vmm.virtual_alloc2(
                  placeholder, 0x10000,
                  MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE),
              0u);
    VirtualAllocationQuery intact{};
    ASSERT_TRUE(vmm.virtual_query(placeholder, intact));
    EXPECT_EQ(intact.region_size, 0x20000u);
    EXPECT_EQ(intact.state, MEM_RESERVE);
}
