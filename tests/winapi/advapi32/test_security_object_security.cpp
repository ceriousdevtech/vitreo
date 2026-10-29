// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_object_security.cpp
/// @brief Unit tests for advapi32 kernel-object security and
///        Get/SetSecurityInfo (WIN32-062).

#include "security_stubs_fixture.hpp"

TEST_F(SecurityStubsTest, GetKernelObjectSecuritySizeQueryAndCopy) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    const Handle event = m_handles->allocate(
        HandleType::Event, nullptr, 0, HandleAccess::All);
    ASSERT_NE(event, NULL_HANDLE);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "GetKernelObjectSecurity");
    ASSERT_NE(entry, nullptr);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr needed = 0x1900;
    constexpr GuestAddr sd = 0x1A00;
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, event).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x7).is_ok()); // owner|group|dacl
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, needed).is_ok());
    ASSERT_TRUE(memory.write_u32(needed, 0xFFFFFFFFu).is_ok());
    set_last_error(0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(needed).value_or(0), 20u);

    ASSERT_TRUE(memory.write_u32(stack + 12, sd).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 20).is_ok());
    set_last_error(0xDEAD);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    // Success leaves the last error untouched (measured on Windows 11:
    // tools/oracle_probe/last_error_on_success_advapi32_probe.expected).
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(memory.read_u8(sd).value_or(0), 1u);
    EXPECT_EQ(memory.read_u8(sd + 3).value_or(0), 0x80u);
}

TEST_F(SecurityStubsTest, GetKernelObjectSecurityLabelReturnsMandatorySacl) {
    // CEF gpu-process: GetKernelObjectSecurity(..., LABEL=0x10) then
    // GetSecurityDescriptorSacl and deref SACL+4 — must be non-NULL.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    const Handle event = m_handles->allocate(
        HandleType::Event, nullptr, 0, HandleAccess::All);
    ASSERT_NE(event, NULL_HANDLE);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* gkos = find_export(exports, "GetKernelObjectSecurity");
    auto* gsacl = find_export(exports, "GetSecurityDescriptorSacl");
    ASSERT_NE(gkos, nullptr);
    ASSERT_NE(gsacl, nullptr);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr needed = 0x1900;
    constexpr GuestAddr sd = 0x1A00;
    constexpr GuestAddr present = 0x1C00;
    constexpr GuestAddr sacl_out = 0x1C04;
    constexpr GuestAddr def = 0x1C08;
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, event).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x10).is_ok()); // LABEL
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, needed).is_ok());
    gkos->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    const u32 label_size = memory.read_u32(needed).value_or(0);
    EXPECT_GE(label_size, 48u);

    ASSERT_TRUE(memory.write_u32(stack + 12, sd).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, label_size).is_ok());
    gkos->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(sd + 2).value_or(0) & 0x8010u, 0x8010u);

    ASSERT_TRUE(memory.write_u32(stack + 4, sd).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, present).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, sacl_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, def).is_ok());
    gsacl->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(present).value_or(0), 1u);
    const u32 sacl = memory.read_u32(sacl_out).value_or(0);
    ASSERT_NE(sacl, 0u);
    EXPECT_EQ(memory.read_u8(sacl).value_or(0), 2u); // ACL_REVISION
    EXPECT_EQ(memory.read_u16(sacl + 4).value_or(0), 1u); // AceCount
    // AceType at SACL+8
    EXPECT_EQ(memory.read_u8(sacl + 8).value_or(0), 0x11u);
}

TEST_F(SecurityStubsTest, SetKernelObjectSecurityAcceptsSelfRelative) {
    // CEF gpu-process GetProcAddress's SetKernelObjectSecurity after Get;
    // a miss returns 0xFFFFFFFF and Chromium intentional-UD2's.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    const Handle event = m_handles->allocate(
        HandleType::Event, nullptr, 0, HandleAccess::All);
    ASSERT_NE(event, NULL_HANDLE);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "SetKernelObjectSecurity");
    ASSERT_NE(entry, nullptr);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr sd = 0x1A00;
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // Self-relative null-DACL SD (revision=1, SE_SELF_RELATIVE).
    std::array<u8, 20> descriptor{};
    descriptor[0] = 1;
    descriptor[3] = 0x80;
    ASSERT_TRUE(memory.write_bytes(sd, descriptor.data(), descriptor.size()).is_ok());

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, event).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x4).is_ok()); // DACL
    ASSERT_TRUE(memory.write_u32(stack + 12, sd).is_ok());
    set_last_error(0xDEAD);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    // Success leaves the last error untouched (measured on Windows 11:
    // tools/oracle_probe/last_error_on_success_advapi32_probe.expected).
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // Null SD -> ERROR_INVALID_PARAMETER
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    // Invalid handle
    ASSERT_TRUE(memory.write_u32(stack + 4, 0xDEAD).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, sd).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(SecurityStubsTest, SetSecurityInfoAcceptsRegistryKeyDacl) {
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* set_entry = find_export(exports, "SetSecurityInfo");
    auto* get_entry = find_export(exports, "GetSecurityInfo");
    ASSERT_NE(set_entry, nullptr);
    ASSERT_NE(get_entry, nullptr);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    // Sid / LocalAlloc scratch used by GetSecurityInfo
    ASSERT_TRUE(memory.map_region(0x5D000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x80000 + 0x800);
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    // Minimal ACL (revision 2, size 8, 0 ACEs)
    constexpr GuestAddr kAcl = 0x10000;
    ASSERT_TRUE(memory.write_u8(kAcl + 0, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(kAcl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 2, 8).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 6, 0).is_ok());

    auto push = [&](u32 index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4),
                               value);
    };
    constexpr u32 SE_REGISTRY_KEY = 4;
    constexpr u32 DACL_SECURITY_INFORMATION = 0x4;
    // A real key handle: Windows fails a handle that names no object with
    // ERROR_INVALID_HANDLE (advapi_sdapi_probe ssi_badhandle).
    int key_object = 0;
    const Handle kHandle = m_handles->allocate(HandleType::Registry, &key_object);
    push(0, kHandle);
    push(1, SE_REGISTRY_KEY);
    push(2, DACL_SECURITY_INFORMATION);
    push(3, 0);
    push(4, 0);
    push(5, static_cast<u32>(kAcl));
    push(6, 0);
    set_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u); // ERROR_SUCCESS

    constexpr GuestAddr kOut = 0x11000;
    ASSERT_TRUE(memory.write_u32(kOut + 0, 0).is_ok());  // owner*
    ASSERT_TRUE(memory.write_u32(kOut + 4, 0).is_ok());  // group*
    ASSERT_TRUE(memory.write_u32(kOut + 8, 0).is_ok());  // dacl*
    ASSERT_TRUE(memory.write_u32(kOut + 12, 0).is_ok()); // sacl*
    ASSERT_TRUE(memory.write_u32(kOut + 16, 0).is_ok()); // sd*
    push(0, kHandle);
    push(1, SE_REGISTRY_KEY);
    push(2, DACL_SECURITY_INFORMATION);
    push(3, static_cast<u32>(kOut + 0));
    push(4, static_cast<u32>(kOut + 4));
    push(5, static_cast<u32>(kOut + 8));
    push(6, static_cast<u32>(kOut + 12));
    push(7, static_cast<u32>(kOut + 16));
    get_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const u32 dacl_ptr = memory.read_u32(kOut + 8).value_or(0);
    ASSERT_NE(dacl_ptr, 0u);
    EXPECT_EQ(memory.read_u8(dacl_ptr).value_or(0), 2u); // ACL revision
    // An empty DACL stays empty (deny all), as on Windows (probe
    // gsi_after_empty "D:P" after setting an empty protected DACL).
    EXPECT_EQ(memory.read_u16(dacl_ptr + 4).value_or(0xFFFF), 0u); // AceCount
    EXPECT_EQ(memory.read_u16(dacl_ptr + 2).value_or(0), 8u);
}

TEST_F(SecurityStubsTest, SetSecurityInfoAcceptsWindowObjectMandatoryLabel) {
    // Chromium AlternateDesktop::UpdateDesktopIntegrity → SetObjectIntegrityLabel
    // → SecurityDescriptor::WriteToHandle(SE_WINDOW_OBJECT, LABEL_SECURITY_INFORMATION).
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* set_entry = find_export(exports, "SetSecurityInfo");
    ASSERT_NE(set_entry, nullptr);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x80000 + 0x800);
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    // Minimal SACL with one SYSTEM_MANDATORY_LABEL_ACE for Low (S-1-16-4096).
    // ACE: type=0x11 size=20 mask=1 SID=S-1-16-0x1000
    constexpr GuestAddr kSacl = 0x10000;
    constexpr u16 kAceSize = 20;
    constexpr u16 kAclSize = 8 + kAceSize;
    ASSERT_TRUE(memory.write_u8(kSacl + 0, 2).is_ok()); // ACL_REVISION
    ASSERT_TRUE(memory.write_u8(kSacl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kSacl + 2, kAclSize).is_ok());
    ASSERT_TRUE(memory.write_u16(kSacl + 4, 1).is_ok()); // AceCount
    ASSERT_TRUE(memory.write_u16(kSacl + 6, 0).is_ok());
    const GuestAddr ace = kSacl + 8;
    ASSERT_TRUE(memory.write_u8(ace + 0, 0x11).is_ok()); // SYSTEM_MANDATORY_LABEL_ACE
    ASSERT_TRUE(memory.write_u8(ace + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(ace + 2, kAceSize).is_ok());
    ASSERT_TRUE(memory.write_u32(ace + 4, 0x1).is_ok()); // NO_WRITE_UP
    ASSERT_TRUE(memory.write_u8(ace + 8, 1).is_ok());    // SID revision
    ASSERT_TRUE(memory.write_u8(ace + 9, 1).is_ok());    // SubAuthCount
    ASSERT_TRUE(memory.write_u8(ace + 15, 16).is_ok());  // Mandatory Label authority
    ASSERT_TRUE(memory.write_u32(ace + 16, 0x1000).is_ok()); // Low RID

    auto push = [&](u32 index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4),
                               value);
    };
    constexpr u32 SE_WINDOW_OBJECT = 7;
    constexpr u32 LABEL_SECURITY_INFORMATION = 0x10;
    constexpr Handle kDesktop = 0xABCD1100; // Vitreo alternate desktop handle style
    push(0, kDesktop);
    push(1, SE_WINDOW_OBJECT);
    push(2, LABEL_SECURITY_INFORMATION);
    push(3, 0);
    push(4, 0);
    push(5, 0);
    push(6, static_cast<u32>(kSacl));
    set_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u); // ERROR_SUCCESS
}

TEST_F(SecurityStubsTest, AddMandatoryAceAppendsLabelAce) {
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* init_entry = find_export(exports, "InitializeAcl");
    auto* add_entry = find_export(exports, "AddMandatoryAce");
    ASSERT_NE(init_entry, nullptr);
    ASSERT_NE(add_entry, nullptr);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x80000 + 0x800);
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    auto push = [&](u32 index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4),
                               value);
    };

    // Room for ACL header + one 20-byte mandatory label ACE.
    constexpr GuestAddr kAcl = 0x10000;
    constexpr u32 kAclCapacity = 8 + 20;
    push(0, static_cast<u32>(kAcl));
    push(1, kAclCapacity);
    push(2, 2); // ACL_REVISION
    init_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    // SID S-1-16-4096 (Low)
    constexpr GuestAddr kSid = 0x10100;
    ASSERT_TRUE(memory.write_u8(kSid + 0, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kSid + 1, 1).is_ok());
    for (u32 i = 2; i < 8; ++i) {
        ASSERT_TRUE(memory.write_u8(kSid + i, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u8(kSid + 7, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(kSid + 8, 0x1000).is_ok());

    push(0, static_cast<u32>(kAcl));
    push(1, 2); // ACL_REVISION
    push(2, 0); // AceFlags
    push(3, 1); // SYSTEM_MANDATORY_LABEL_NO_WRITE_UP
    push(4, static_cast<u32>(kSid));
    add_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(kAcl + 4).value_or(0), 1u); // AceCount
    EXPECT_EQ(memory.read_u8(kAcl + 8).value_or(0), 0x11u); // ACE type
    EXPECT_EQ(memory.read_u32(kAcl + 12).value_or(0), 1u); // policy mask
}

TEST_F(SecurityStubsTest, GetNamedSecurityInfoWSupportsRegistryKeys) {
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* get_entry = find_export(exports, "GetNamedSecurityInfoW");
    auto* set_entry = find_export(exports, "SetNamedSecurityInfoW");
    ASSERT_NE(get_entry, nullptr);
    ASSERT_NE(set_entry, nullptr);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x5D000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x80000 + 0x800);
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr kName = 0x10000;
    const char* path = "MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Battle.net";
    for (usize i = 0; path[i] != 0; ++i) {
        ASSERT_TRUE(memory.write_u16(kName + i * 2, static_cast<u8>(path[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(kName + std::strlen(path) * 2, 0).is_ok());

    constexpr GuestAddr kAcl = 0x10800;
    ASSERT_TRUE(memory.write_u8(kAcl + 0, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(kAcl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 2, 8).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 6, 0).is_ok());

    auto push = [&](u32 index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4),
                               value);
    };
    constexpr u32 SE_REGISTRY_KEY = 4;
    constexpr u32 DACL_SECURITY_INFORMATION = 0x4;
    constexpr u32 OWNER_SECURITY_INFORMATION = 0x1;

    push(0, static_cast<u32>(kName));
    push(1, SE_REGISTRY_KEY);
    push(2, DACL_SECURITY_INFORMATION);
    push(3, 0);
    push(4, 0);
    push(5, static_cast<u32>(kAcl));
    push(6, 0);
    set_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    constexpr GuestAddr kOut = 0x11000;
    ASSERT_TRUE(memory.write_u32(kOut + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kOut + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kOut + 16, 0).is_ok());
    push(0, static_cast<u32>(kName));
    push(1, SE_REGISTRY_KEY);
    push(2, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION);
    push(3, static_cast<u32>(kOut + 0));
    push(4, 0);
    push(5, static_cast<u32>(kOut + 8));
    push(6, 0);
    push(7, static_cast<u32>(kOut + 16));
    get_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const u32 sd = memory.read_u32(kOut + 16).value_or(0);
    ASSERT_NE(sd, 0u);
    EXPECT_NE(memory.read_u32(kOut + 0).value_or(0), 0u); // owner
}

TEST_F(SecurityStubsTest, NamedSecurityInfoAcceptsWow64_64RegistryKey) {
    // Learn SE_OBJECT_TYPE: SE_REGISTRY_WOW64_64KEY is a registry object.
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* set_entry = find_export(exports, "SetNamedSecurityInfoW");
    auto* get_entry = find_export(exports, "GetNamedSecurityInfoW");
    ASSERT_NE(set_entry, nullptr);
    ASSERT_NE(get_entry, nullptr);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x5D000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x80000 + 0x800);
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr kName = 0x10000;
    const char* path = "MACHINE\\SOFTWARE\\Valve\\Steam";
    for (usize i = 0; path[i] != 0; ++i) {
        ASSERT_TRUE(memory.write_u16(kName + i * 2, static_cast<u8>(path[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(kName + std::strlen(path) * 2, 0).is_ok());

    constexpr GuestAddr kAcl = 0x10800;
    ASSERT_TRUE(memory.write_u8(kAcl + 0, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(kAcl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 2, 8).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(kAcl + 6, 0).is_ok());

    auto push = [&](u32 index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4),
                               value);
    };
    constexpr u32 SE_REGISTRY_WOW64_64KEY = 13;
    constexpr u32 DACL_SECURITY_INFORMATION = 0x4;

    push(0, static_cast<u32>(kName));
    push(1, SE_REGISTRY_WOW64_64KEY);
    push(2, DACL_SECURITY_INFORMATION);
    push(3, 0);
    push(4, 0);
    push(5, static_cast<u32>(kAcl));
    push(6, 0);
    set_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    constexpr GuestAddr kOut = 0x11000;
    ASSERT_TRUE(memory.write_u32(kOut + 16, 0).is_ok());
    push(0, static_cast<u32>(kName));
    push(1, SE_REGISTRY_WOW64_64KEY);
    push(2, DACL_SECURITY_INFORMATION);
    push(3, 0);
    push(4, 0);
    push(5, 0);
    push(6, 0);
    push(7, static_cast<u32>(kOut + 16));
    get_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_NE(memory.read_u32(kOut + 16).value_or(0), 0u);
}
