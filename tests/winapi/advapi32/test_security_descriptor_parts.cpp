// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_descriptor_parts.cpp
/// @brief Unit tests for advapi32 security-descriptor component accessors
///        and absolute/self-relative conversion (WIN32-062).

#include "security_stubs_fixture.hpp"

TEST_F(SecurityStubsTest, SecurityDescriptorDaclHandlesX64Layouts) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    ASSERT_TRUE(memory.map_region(kData, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* initialize = find_export(exports, "InitializeSecurityDescriptor");
    auto* set_dacl = find_export(exports, "SetSecurityDescriptorDacl");
    auto* get_dacl = find_export(exports, "GetSecurityDescriptorDacl");
    ASSERT_NE(initialize, nullptr);
    ASSERT_NE(set_dacl, nullptr);
    ASSERT_NE(get_dacl, nullptr);

    constexpr GuestAddr kAbsolute = kData + 0x100;
    constexpr GuestAddr kAcl = kData + 0x200;
    constexpr GuestAddr kPresent = kData + 0x300;
    constexpr GuestAddr kDaclOut = kData + 0x308;
    constexpr GuestAddr kDefaulted = kData + 0x310;
    std::array<u8, 40> dirty{};
    dirty.fill(0xCC);
    ASSERT_TRUE(memory.write_bytes(kAbsolute, dirty.data(), dirty.size()).is_ok());

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = 1;
    initialize->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kAbsolute + 8).value(), 0u);
    EXPECT_EQ(memory.read_u64(kAbsolute + 32).value(), 0u);

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = kAcl;
    cpu.gpr.r9 = 1;
    set_dacl->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u16(kAbsolute + 2).value(), 0x000Cu);
    EXPECT_EQ(memory.read_u64(kAbsolute + 32).value(), kAcl);

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = kPresent;
    cpu.gpr.r8 = kDaclOut;
    cpu.gpr.r9 = kDefaulted;
    get_dacl->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(kPresent).value(), 1u);
    EXPECT_EQ(memory.read_u64(kDaclOut).value(), kAcl);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 1u);

    constexpr GuestAddr kRelative = kData + 0x400;
    ASSERT_TRUE(memory.write_u8(kRelative, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(kRelative + 2, 0x8004).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 16, 20).is_ok());
    cpu.gpr.rcx = kRelative;
    get_dacl->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kDaclOut).value(), kRelative + 20);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 0u);
}

TEST_F(SecurityStubsTest, GetSecurityDescriptorOwnerAndGroupReturnSids) {
    // Microsoft Learn: GetSecurityDescriptorOwner/Group return a pointer to
    // the SID inside the descriptor. AccessCheck requires those SIDs; a NULL
    // owner/group is ERROR_INVALID_SECURITY_DESCR.
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    ASSERT_TRUE(memory.map_region(kData, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* initialize = find_export(exports, "InitializeSecurityDescriptor");
    auto* set_owner = find_export(exports, "SetSecurityDescriptorOwner");
    auto* set_group = find_export(exports, "SetSecurityDescriptorGroup");
    auto* get_owner = find_export(exports, "GetSecurityDescriptorOwner");
    auto* get_group = find_export(exports, "GetSecurityDescriptorGroup");
    ASSERT_NE(initialize, nullptr);
    ASSERT_NE(set_owner, nullptr);
    ASSERT_NE(set_group, nullptr);
    ASSERT_NE(get_owner, nullptr);
    ASSERT_NE(get_group, nullptr);

    constexpr GuestAddr kAbsolute = kData + 0x100;
    constexpr GuestAddr kOwnerSid = kData + 0x200;
    constexpr GuestAddr kGroupSid = kData + 0x220;
    constexpr GuestAddr kSidOut = kData + 0x300;
    constexpr GuestAddr kDefaulted = kData + 0x308;
    const std::array<u8, 12> owner_sid = {
        1, 1, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0};
    const std::array<u8, 12> group_sid = {
        1, 1, 0, 0, 0, 0, 0, 5, 0x21, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        kOwnerSid, owner_sid.data(), owner_sid.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        kGroupSid, group_sid.data(), group_sid.size()).is_ok());

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = 1;
    initialize->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = kSidOut;
    cpu.gpr.r8 = kDefaulted;
    ASSERT_TRUE(memory.write_u64(kSidOut, 0xDEAD).is_ok());
    ASSERT_TRUE(memory.write_u32(kDefaulted, 0xA5).is_ok());
    get_owner->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kSidOut).value(), 0u);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 0xA5u); // untouched

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = kOwnerSid;
    cpu.gpr.r8 = 1; // defaulted
    set_owner->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    cpu.gpr.rdx = kGroupSid;
    cpu.gpr.r8 = 0;
    set_group->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = kAbsolute;
    cpu.gpr.rdx = kSidOut;
    cpu.gpr.r8 = kDefaulted;
    get_owner->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kSidOut).value(), kOwnerSid);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 1u);
    get_group->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kSidOut).value(), kGroupSid);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 0u);

    constexpr GuestAddr kRelative = kData + 0x400;
    constexpr u32 kOwnerOff = 20;
    constexpr u32 kGroupOff = 32;
    ASSERT_TRUE(memory.write_u8(kRelative, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kRelative + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kRelative + 2, 0x8003).is_ok()); // SR|owner+group defaulted
    ASSERT_TRUE(memory.write_u32(kRelative + 4, kOwnerOff).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 8, kGroupOff).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        kRelative + kOwnerOff, owner_sid.data(), owner_sid.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        kRelative + kGroupOff, group_sid.data(), group_sid.size()).is_ok());

    cpu.gpr.rcx = kRelative;
    cpu.gpr.rdx = kSidOut;
    cpu.gpr.r8 = kDefaulted;
    get_owner->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kSidOut).value(), kRelative + kOwnerOff);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 1u);
    get_group->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(kSidOut).value(), kRelative + kGroupOff);
    EXPECT_EQ(memory.read_u32(kDefaulted).value(), 1u);
}

TEST_F(SecurityStubsTest, MakeAbsoluteSdReportsSizesAndCopiesComponents) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    ASSERT_TRUE(memory.map_region(kData, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr kStack = kData + 0xF000;
    cpu.gpr.rsp = kStack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* make_absolute = find_export(exports, "MakeAbsoluteSD");
    ASSERT_NE(make_absolute, nullptr);

    constexpr GuestAddr kRelative = kData + 0x100;
    constexpr GuestAddr kAbsolute = kData + 0x400;
    constexpr GuestAddr kOwner = kData + 0x500;
    constexpr GuestAddr kGroup = kData + 0x520;
    constexpr GuestAddr kDacl = kData + 0x540;
    constexpr GuestAddr kAbsoluteSize = kData + 0x600;
    constexpr GuestAddr kOwnerSize = kData + 0x604;
    constexpr GuestAddr kGroupSize = kData + 0x608;
    constexpr GuestAddr kSaclSize = kData + 0x60C;
    constexpr GuestAddr kDaclSize = kData + 0x610;

    constexpr u32 kOwnerOffset = 20;
    constexpr u32 kGroupOffset = 32;
    constexpr u32 kDaclOffset = 44;
    ASSERT_TRUE(memory.write_u8(kRelative, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kRelative + 1, 0x5A).is_ok());
    ASSERT_TRUE(memory.write_u16(kRelative + 2, 0x8004).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 4, kOwnerOffset).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 8, kGroupOffset).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kRelative + 16, kDaclOffset).is_ok());

    const std::array<u8, 12> owner_sid = {
        1, 1, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0
    };
    const std::array<u8, 12> group_sid = {
        1, 1, 0, 0, 0, 0, 0, 5, 0x21, 0, 0, 0
    };
    const std::array<u8, 8> dacl = {2, 0, 8, 0, 0, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        kRelative + kOwnerOffset, owner_sid.data(), owner_sid.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        kRelative + kGroupOffset, group_sid.data(), group_sid.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        kRelative + kDaclOffset, dacl.data(), dacl.size()).is_ok());

    auto set_args = [&](GuestAddr absolute, GuestAddr owner,
                        GuestAddr group, GuestAddr dacl_buffer) {
        const std::array<u32, 11> args = {
            static_cast<u32>(kRelative),
            static_cast<u32>(absolute),
            static_cast<u32>(kAbsoluteSize),
            static_cast<u32>(dacl_buffer),
            static_cast<u32>(kDaclSize),
            0,
            static_cast<u32>(kSaclSize),
            static_cast<u32>(owner),
            static_cast<u32>(kOwnerSize),
            static_cast<u32>(group),
            static_cast<u32>(kGroupSize),
        };
        for (usize index = 0; index < args.size(); ++index) {
            ASSERT_TRUE(memory.write_u32(
                kStack + 4 + index * sizeof(u32), args[index]).is_ok());
        }
    };

    ASSERT_TRUE(memory.write_u32(kAbsoluteSize, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kOwnerSize, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kGroupSize, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kSaclSize, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kDaclSize, 0).is_ok());
    set_args(0, 0, 0, 0);
    make_absolute->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(kAbsoluteSize).value(), 20u);
    EXPECT_EQ(memory.read_u32(kOwnerSize).value(), owner_sid.size());
    EXPECT_EQ(memory.read_u32(kGroupSize).value(), group_sid.size());
    EXPECT_EQ(memory.read_u32(kSaclSize).value(), 0u);
    EXPECT_EQ(memory.read_u32(kDaclSize).value(), dacl.size());

    set_args(kAbsolute, kOwner, kGroup, kDacl);
    make_absolute->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u8(kAbsolute).value(), 1u);
    EXPECT_EQ(memory.read_u8(kAbsolute + 1).value(), 0x5Au);
    EXPECT_EQ(memory.read_u16(kAbsolute + 2).value(), 0x0004u);
    EXPECT_EQ(memory.read_u32(kAbsolute + 4).value(), kOwner);
    EXPECT_EQ(memory.read_u32(kAbsolute + 8).value(), kGroup);
    EXPECT_EQ(memory.read_u32(kAbsolute + 12).value(), 0u);
    EXPECT_EQ(memory.read_u32(kAbsolute + 16).value(), kDacl);

    std::array<u8, 12> copied_sid{};
    ASSERT_TRUE(memory.read_bytes(
        kOwner, copied_sid.data(), copied_sid.size()).is_ok());
    EXPECT_EQ(copied_sid, owner_sid);
    ASSERT_TRUE(memory.read_bytes(
        kGroup, copied_sid.data(), copied_sid.size()).is_ok());
    EXPECT_EQ(copied_sid, group_sid);
    std::array<u8, 8> copied_dacl{};
    ASSERT_TRUE(memory.read_bytes(
        kDacl, copied_dacl.data(), copied_dacl.size()).is_ok());
    EXPECT_EQ(copied_dacl, dacl);
}
