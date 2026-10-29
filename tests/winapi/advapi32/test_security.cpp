// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security.cpp
/// @brief Unit tests for advapi32 security stubs (WIN32-062): identity,
///        account lookup and the exported-thunk table.

#include "security_stubs_fixture.hpp"

TEST_F(SecurityStubsTest, TraceProviderHandlesHaveLifecycle) {
    u64 first = m_stubs->register_trace_provider();
    u64 second = m_stubs->register_trace_provider();

    EXPECT_NE(first, second);
    EXPECT_TRUE(m_stubs->is_trace_provider_registered(first));
    EXPECT_TRUE(m_stubs->unregister_trace_provider(first));
    EXPECT_FALSE(m_stubs->is_trace_provider_registered(first));
    EXPECT_FALSE(m_stubs->unregister_trace_provider(first));
    EXPECT_TRUE(m_stubs->unregister_trace_provider(second));
}

// =============================================================================
// 1. GetUserNameW returns "VitUser"
// =============================================================================

TEST_F(SecurityStubsTest, GetUserNameW_ReturnsVitUser) {
    std::wstring name;
    u32 size = 128;
    EXPECT_TRUE(m_stubs->get_user_name_w(name, size));
    EXPECT_EQ(name, L"VitUser");
    EXPECT_EQ(size, 8u); // strlen("VitUser") + 1
}

// =============================================================================
// 2. GetUserNameA returns "VitUser"
// =============================================================================

TEST_F(SecurityStubsTest, GetUserNameA_ReturnsVitUser) {
    std::string name;
    u32 size = 128;
    EXPECT_TRUE(m_stubs->get_user_name_a(name, size));
    EXPECT_EQ(name, "VitUser");
    EXPECT_EQ(size, 8u);
}

// =============================================================================
// 3. GetUserNameW with small buffer returns false and sets required size
// =============================================================================

TEST_F(SecurityStubsTest, GetUserNameW_SmallBufferReturnsFalse) {
    std::wstring name;
    u32 size = 2; // Too small
    EXPECT_FALSE(m_stubs->get_user_name_w(name, size));
    EXPECT_EQ(size, 8u); // Required size
}

// =============================================================================
// 8. LookupPrivilegeValueW returns non-zero LUID
// =============================================================================

TEST_F(SecurityStubsTest, LookupPrivilegeValueW_ReturnsNonZeroLUID) {
    u64 luid = 0;
    EXPECT_TRUE(m_stubs->lookup_privilege_value_w(L"SeDebugPrivilege", luid));
    EXPECT_NE(luid, 0u);
}

// =============================================================================
// 9. LookupAccountSidW returns "VitUser" and "VITREO"
// =============================================================================

TEST_F(SecurityStubsTest, LookupAccountSidW_ReturnsVitUserAndVitreo) {
    std::wstring name;
    std::wstring domain;
    u32 use = 0;
    EXPECT_TRUE(m_stubs->lookup_account_sid_w(
        SecurityStubs::synthetic_sid(), name, domain, use));
    EXPECT_EQ(name, L"VitUser");
    EXPECT_EQ(domain, L"VITREO");
    EXPECT_EQ(use, 1u); // SidTypeUser
}

TEST_F(SecurityStubsTest, LookupAccountSidWMapsWellKnownSids) {
    auto make_sid = [](u8 authority, std::initializer_list<u32> subs) {
        std::vector<u8> bytes(8 + subs.size() * 4, 0);
        bytes[0] = 1;
        bytes[1] = static_cast<u8>(subs.size());
        bytes[7] = authority;
        usize index = 0;
        for (u32 sub : subs) {
            bytes[8 + index * 4] = static_cast<u8>(sub);
            bytes[9 + index * 4] = static_cast<u8>(sub >> 8);
            bytes[10 + index * 4] = static_cast<u8>(sub >> 16);
            bytes[11 + index * 4] = static_cast<u8>(sub >> 24);
            ++index;
        }
        return bytes;
    };
    std::wstring name;
    std::wstring domain;
    u32 use = 0;
    ASSERT_TRUE(m_stubs->lookup_account_sid_w(
        make_sid(5, {32, 544}), name, domain, use));
    EXPECT_EQ(name, L"Administrators");
    EXPECT_EQ(domain, L"BUILTIN");
    EXPECT_EQ(use, 4u); // SidTypeAlias
    ASSERT_TRUE(m_stubs->lookup_account_sid_w(
        make_sid(5, {4}), name, domain, use));
    EXPECT_EQ(name, L"INTERACTIVE");
    EXPECT_EQ(domain, L"NT AUTHORITY");
    EXPECT_EQ(use, 5u); // SidTypeWellKnownGroup
    EXPECT_FALSE(m_stubs->lookup_account_sid_w(
        make_sid(5, {99}), name, domain, use));
}

TEST_F(SecurityStubsTest, LookupAccountNameWMapsSyntheticLocalIdentity) {
    for (const auto& account : {
             std::wstring{L"VitUser"},
             std::wstring{L"vitreo\\vituser"},
             std::wstring{L".\\VITUSER"}}) {
        std::vector<u8> sid;
        std::wstring domain;
        u32 use = 0;
        ASSERT_TRUE(m_stubs->lookup_account_name_w(
            L"", account, sid, domain, use));
        EXPECT_EQ(sid, SecurityStubs::synthetic_sid());
        EXPECT_EQ(domain, L"VITREO");
        EXPECT_EQ(use, 1u);
    }
}

TEST_F(SecurityStubsTest, LookupAccountNameWRejectsUnknownAndRemoteAccounts) {
    std::vector<u8> sid;
    std::wstring domain;
    u32 use = 0;
    EXPECT_FALSE(m_stubs->lookup_account_name_w(
        L"", L"SomeoneElse", sid, domain, use));
    EXPECT_FALSE(m_stubs->lookup_account_name_w(
        L"REMOTE", L"VitUser", sid, domain, use));
}

TEST_F(SecurityStubsTest, LookupAccountNameWMapsWellKnownAliases) {
    std::vector<u8> sid;
    std::wstring domain;
    u32 use = 0;
    ASSERT_TRUE(m_stubs->lookup_account_name_w(
        L"", L"Administrators", sid, domain, use));
    EXPECT_EQ(domain, L"BUILTIN");
    EXPECT_EQ(use, 4u);
    ASSERT_TRUE(m_stubs->lookup_account_name_w(
        L"", L"BUILTIN\\Users", sid, domain, use));
    EXPECT_EQ(domain, L"BUILTIN");
    EXPECT_EQ(use, 4u);
    ASSERT_TRUE(m_stubs->lookup_account_name_w(
        L"", L"INTERACTIVE", sid, domain, use));
    EXPECT_EQ(domain, L"NT AUTHORITY");
    EXPECT_EQ(use, 5u);
}

TEST_F(SecurityStubsTest, GetCurrentHwProfileWReturnsStableDesktopProfile) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4,
                                 static_cast<u32>(data)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* get_profile = find_export(exports, "GetCurrentHwProfileW");
    ASSERT_NE(get_profile, nullptr);

    get_profile->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(data).value(), 1u);
    EXPECT_EQ(memory.read_u16(data + 4).value(), u'{');
    EXPECT_EQ(memory.read_u16(data + 4 + 38 * 2).value(), 0u);
    const GuestAddr name = data + 4 + 39 * 2;
    EXPECT_EQ(memory.read_u16(name).value(), u'V');
}

// =============================================================================
// 10. synthetic_sid returns well-formed SID
// =============================================================================

TEST_F(SecurityStubsTest, SyntheticSID_IsWellFormed) {
    const auto& sid = SecurityStubs::synthetic_sid();

    // SID header: revision(1) + sub_auth_count(1) + authority(6) + 5 sub_auths(20)
    EXPECT_EQ(sid.size(), 28u); // 1 + 1 + 6 + 5*4
    EXPECT_EQ(sid[0], 0x01);   // Revision 1
    EXPECT_EQ(sid[1], 0x05);   // 5 sub-authorities
    // Last 2 bytes of IdentifierAuthority = 5 (SECURITY_NT_AUTHORITY)
    EXPECT_EQ(sid[7], 0x05);
}

// =============================================================================
// 12. Thunk export creation
// =============================================================================

TEST_F(SecurityStubsTest, ThunkExports_HasExpectedEntries) {
    auto exports = create_advapi32_security_thunks(*m_stubs);
    // Validate required names and uniqueness below, rather than a count
    // copied from the implementation. New exports must not break this test.

    // Duplicate registrations are the real hazard: a later by_name entry
    // silently shadows an earlier one, which is how a fail-stub once masked the
    // real GetTokenInformation. This invariant is what the count was proxying
    // for, and unlike the count it does not churn when an export is added.
    std::set<std::string> seen;
    for (const auto& e : exports.entries) {
        EXPECT_TRUE(seen.insert(e.name).second)
            << "duplicate export registration shadows an earlier one: " << e.name;
    }

    // Presence, not position — these assertions previously used fixed indices
    // and broke whenever an export was added or removed anywhere before them.
    for (const char* required : {
            "GetUserNameW",
            "GetCurrentHwProfileW",
            "LookupAccountNameW",
            "LookupAccountSidW",
            "GetSidIdentifierAuthority",
            "GetLengthSid",
            "GetSidLengthRequired",
            "IsValidSid",
            "CopySid",
            "GetSidSubAuthority",
            "GetSidSubAuthorityCount",
            "InitializeSid",
            "EqualSid",
            "GetAclInformation",
            "GetSecurityDescriptorLength",
            "MakeSelfRelativeSD",
            "LookupPrivilegeNameW",
            "LookupPrivilegeDisplayNameW",
            "SetFileSecurityW",
            // Both previously fell through to the generic missing-export stub,
            // whose 0xFFFFFFFF reads as TRUE for a BOOL while leaving every
            // out-parameter unwritten. Battle.net-Setup.exe calls both.
            "GetFileSecurityW",
            "GetKernelObjectSecurity",
            "SetKernelObjectSecurity",
            "CreateProcessWithTokenW",
            "DuplicateToken",
            "DeregisterEventSource",
            "RegisterEventSourceW",
            "ReportEventW",
            "InitializeSecurityDescriptor",
            "MakeAbsoluteSD",
            "SetSecurityDescriptorDacl",
            "SetSecurityDescriptorSacl",
            "CryptEnumProvidersA",
            "CryptEnumProvidersW",
            "CryptGetUserKey",
            "CryptGetProvParam",
            "CryptSetHashParam",
            "CryptExportKey",
            "CryptSignHashA",
            "CryptSignHashW",
            "SystemFunction036",
            "ConvertSidToStringSidA",
            "ConvertSidToStringSidW",
            "ConvertSecurityDescriptorToStringSecurityDescriptorW",
            "ConvertStringSidToSidA",
            "ConvertStringSidToSidW",
            "DeriveCapabilitySidsFromName",
            "SetNamedSecurityInfoW",
            "GetNamedSecurityInfoW",
            "SetSecurityInfo",
            "AddAce",
        }) {
        EXPECT_TRUE(seen.count(required) != 0)
            << "missing advapi32 security export: " << required;
    }
}

TEST_F(SecurityStubsTest, DeriveCapabilitySidsFromNameWellKnownAndHashed) {
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "DeriveCapabilitySidsFromName");
    ASSERT_NE(entry, nullptr);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x5D000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x80000 + 0x800);
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    auto push = [&](u32 index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4),
                               value);
    };
    auto write_wide = [&](GuestAddr addr, const char* ascii) {
        for (usize i = 0; ascii[i] != 0; ++i) {
            ASSERT_TRUE(
                memory.write_u16(addr + i * 2, static_cast<u8>(ascii[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(addr + std::strlen(ascii) * 2, 0).is_ok());
    };

    constexpr GuestAddr kName = 0x10000;
    constexpr GuestAddr kGroupSidsOut = 0x10200;
    constexpr GuestAddr kGroupCountOut = 0x10210;
    constexpr GuestAddr kCapSidsOut = 0x10220;
    constexpr GuestAddr kCapCountOut = 0x10230;

    write_wide(kName, "internetClient");
    push(0, static_cast<u32>(kName));
    push(1, static_cast<u32>(kGroupSidsOut));
    push(2, static_cast<u32>(kGroupCountOut));
    push(3, static_cast<u32>(kCapSidsOut));
    push(4, static_cast<u32>(kCapCountOut));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(kGroupCountOut).value_or(0), 1u);
    EXPECT_EQ(memory.read_u32(kCapCountOut).value_or(0), 1u);

    const GuestAddr cap_arr = memory.read_u32(kCapSidsOut).value_or(0);
    ASSERT_NE(cap_arr, 0u);
    const GuestAddr cap_sid = memory.read_u32(cap_arr).value_or(0);
    ASSERT_NE(cap_sid, 0u);
    // Well-known: S-1-15-3-1
    EXPECT_EQ(memory.read_u8(cap_sid + 0).value_or(0), 1u);
    EXPECT_EQ(memory.read_u8(cap_sid + 1).value_or(0), 2u);
    EXPECT_EQ(memory.read_u8(cap_sid + 7).value_or(0), 15u);
    EXPECT_EQ(memory.read_u32(cap_sid + 8).value_or(0), 3u);
    EXPECT_EQ(memory.read_u32(cap_sid + 12).value_or(0), 1u);

    const GuestAddr group_arr = memory.read_u32(kGroupSidsOut).value_or(0);
    ASSERT_NE(group_arr, 0u);
    const GuestAddr group_sid = memory.read_u32(group_arr).value_or(0);
    ASSERT_NE(group_sid, 0u);
    // Group always hash-based: S-1-5-32-<8 dwords>
    EXPECT_EQ(memory.read_u8(group_sid + 1).value_or(0), 9u);
    EXPECT_EQ(memory.read_u8(group_sid + 7).value_or(0), 5u);
    EXPECT_EQ(memory.read_u32(group_sid + 8).value_or(0), 32u);

    // Hash-named capability uses S-1-15-3-1024-<hash>
    write_wide(kName, "lpacAppExperience");
    push(0, static_cast<u32>(kName));
    push(1, static_cast<u32>(kGroupSidsOut));
    push(2, static_cast<u32>(kGroupCountOut));
    push(3, static_cast<u32>(kCapSidsOut));
    push(4, static_cast<u32>(kCapCountOut));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr hashed_arr = memory.read_u32(kCapSidsOut).value_or(0);
    const GuestAddr hashed_sid = memory.read_u32(hashed_arr).value_or(0);
    ASSERT_NE(hashed_sid, 0u);
    EXPECT_EQ(memory.read_u8(hashed_sid + 1).value_or(0), 10u);
    EXPECT_EQ(memory.read_u8(hashed_sid + 7).value_or(0), 15u);
    EXPECT_EQ(memory.read_u32(hashed_sid + 8).value_or(0), 3u);
    EXPECT_EQ(memory.read_u32(hashed_sid + 12).value_or(0), 1024u);
}
