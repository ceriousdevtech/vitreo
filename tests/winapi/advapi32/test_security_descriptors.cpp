// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_descriptors.cpp
/// @brief Unit tests for advapi32 SID, ACL and security-descriptor
///        primitives (WIN32-062).

#include "security_stubs_fixture.hpp"

TEST_F(SecurityStubsTest, GetSidLengthRequiredUsesSidLayoutAndUcharCount) {
    constexpr GuestAddr stack = 0x20000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "GetSidLengthRequired");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 4u);

    for (const auto [count, expected] :
         std::array<std::pair<u32, u32>, 4>{{
             {0u, 8u}, {1u, 12u}, {15u, 68u}, {255u, 1028u}}}) {
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, count).is_ok());
        entry->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), expected) << count;
    }
}

TEST_F(SecurityStubsTest, GetAclInformationReportsRevisionAndExactUsage) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr acl = data;
    constexpr GuestAddr info = data + 0x100;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // A 64-byte revision-2 ACL containing 20- and 24-byte ACEs uses 52 bytes.
    ASSERT_TRUE(memory.write_u8(acl, 2).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 2, 64).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 4, 2).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 8 + 2, 20).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 28 + 2, 24).is_ok());

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "GetAclInformation");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 16u);
    auto arg = [&](u32 index, u32 value) {
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + index * 4, value).is_ok());
    };
    arg(0, static_cast<u32>(acl));
    arg(1, static_cast<u32>(info));

    arg(2, 4);
    arg(3, 1); // AclRevisionInformation
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(info).value_or(0), 2u);

    arg(2, 12);
    arg(3, 2); // AclSizeInformation
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(info).value_or(0), 2u);
    EXPECT_EQ(memory.read_u32(info + 4).value_or(0), 52u);
    EXPECT_EQ(memory.read_u32(info + 8).value_or(0), 12u);

    arg(2, 11);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(SecurityStubsTest, GetSecurityDescriptorLengthIncludesComponents) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr descriptor = data;
    constexpr GuestAddr owner = data + 0x100;
    constexpr GuestAddr group = data + 0x200;
    constexpr GuestAddr dacl = data + 0x300;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u8(descriptor, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(descriptor + 2, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, owner).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 8, group).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 16, dacl).is_ok());
    ASSERT_TRUE(memory.write_u8(owner + 1, 2).is_ok()); // 16-byte SID
    ASSERT_TRUE(memory.write_u8(group + 1, 1).is_ok()); // 12-byte SID
    ASSERT_TRUE(memory.write_u16(dacl + 2, 64).is_ok());

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "GetSecurityDescriptorLength");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 4u);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, descriptor).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 112u);

    // Relative components can appear in any order; the greatest end offset is
    // the size of the contiguous descriptor.
    constexpr GuestAddr relative = data + 0x500;
    ASSERT_TRUE(memory.write_u8(relative, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(relative + 2, 0x8004).is_ok());
    ASSERT_TRUE(memory.write_u32(relative + 4, 20).is_ok());
    ASSERT_TRUE(memory.write_u32(relative + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(relative + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(relative + 16, 32).is_ok());
    ASSERT_TRUE(memory.write_u8(relative + 21, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(relative + 34, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, relative).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 48u);
}

TEST_F(SecurityStubsTest, GetSecurityDescriptorControlReportsStoredFormat) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr descriptor = data;
    constexpr GuestAddr control_out = data + 0x100;
    constexpr GuestAddr revision_out = data + 0x104;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "GetSecurityDescriptorControl");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12u);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, descriptor).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, control_out).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, revision_out).is_ok());

    ASSERT_TRUE(memory.write_u8(descriptor, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(descriptor + 2, 0x8004).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(control_out).value_or(0), 0x8004u);
    EXPECT_EQ(memory.read_u32(revision_out).value_or(0), 1u);

    ASSERT_TRUE(memory.write_u16(descriptor + 2, 0x0004).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(control_out).value_or(0), 0x0004u);
    EXPECT_EQ(memory.read_u32(revision_out).value_or(0), 1u);
}

TEST_F(SecurityStubsTest, SetSecurityDescriptorOwnerAndGroupMutateAbsoluteSd) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr descriptor = data;
    constexpr GuestAddr owner = data + 0x100;
    constexpr GuestAddr group = data + 0x200;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u8(descriptor, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(descriptor + 2, 0).is_ok());
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* set_owner = find_export(exports, "SetSecurityDescriptorOwner");
    auto* set_group = find_export(exports, "SetSecurityDescriptorGroup");
    ASSERT_NE(set_owner, nullptr);
    ASSERT_NE(set_group, nullptr);
    auto arg = [&](u32 index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * 4, value).is_ok());
    };

    arg(0, descriptor);
    arg(1, owner);
    arg(2, 1);
    set_owner->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(descriptor + 4).value_or(0), owner);
    EXPECT_EQ(memory.read_u16(descriptor + 2).value_or(0), 0x0001u);

    arg(1, group);
    arg(2, 1);
    set_group->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(descriptor + 8).value_or(0), group);
    EXPECT_EQ(memory.read_u16(descriptor + 2).value_or(0), 0x0003u);

    // Clearing the owner-defaulted flag must preserve the group flag.
    arg(1, 0);
    arg(2, 0);
    set_owner->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(descriptor + 4).value_or(1), 0u);
    EXPECT_EQ(memory.read_u16(descriptor + 2).value_or(0), 0x0002u);

    ASSERT_TRUE(memory.write_u16(descriptor + 2, 0x8000).is_ok());
    set_group->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_SECURITY_DESCR);
}

TEST_F(SecurityStubsTest, MakeSelfRelativeSdQueriesAndCopiesComponents) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr absolute = data;
    constexpr GuestAddr owner = data + 0x100;
    constexpr GuestAddr dacl = data + 0x200;
    constexpr GuestAddr relative = data + 0x300;
    constexpr GuestAddr size_ptr = data + 0x3f0;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u8(absolute, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(absolute + 2, 0x0004).is_ok());
    ASSERT_TRUE(memory.write_u32(absolute + 4, owner).is_ok());
    ASSERT_TRUE(memory.write_u32(absolute + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(absolute + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(absolute + 16, dacl).is_ok());
    ASSERT_TRUE(memory.write_u8(owner, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(owner + 1, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(owner + 8, 544).is_ok());
    ASSERT_TRUE(memory.write_u8(dacl, 2).is_ok());
    ASSERT_TRUE(memory.write_u16(dacl + 2, 16).is_ok());

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "MakeSelfRelativeSD");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12u);
    auto arg = [&](u32 index, u32 value) {
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + index * 4, value).is_ok());
    };
    arg(0, static_cast<u32>(absolute));
    arg(1, 0);
    arg(2, static_cast<u32>(size_ptr));
    ASSERT_TRUE(memory.write_u32(size_ptr, 0).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(size_ptr).value_or(0), 48u);

    arg(1, static_cast<u32>(relative));
    ASSERT_TRUE(memory.write_u32(size_ptr, 48).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(relative + 2).value_or(0), 0x8004u);
    EXPECT_EQ(memory.read_u32(relative + 4).value_or(0), 20u);
    EXPECT_EQ(memory.read_u32(relative + 8).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(relative + 12).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(relative + 16).value_or(0), 32u);
    EXPECT_EQ(memory.read_u32(relative + 28).value_or(0), 544u);
    EXPECT_EQ(memory.read_u16(relative + 34).value_or(0), 16u);
}
