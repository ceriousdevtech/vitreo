// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_token_restricted.cpp
/// @brief Unit tests for advapi32 token classes, restricted tokens and
///        elevation (WIN32-062).

#include "security_stubs_fixture.hpp"

// =============================================================================
// 11. GetTokenInformation for TokenElevationType returns data
// =============================================================================

TEST_F(SecurityStubsTest, GetTokenInformation_ElevationType_ReturnsData) {
    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));

    std::vector<u8> data;
    u32 return_length = 0;
    EXPECT_TRUE(m_stubs->get_token_information(token, 18, data, return_length));
    EXPECT_EQ(return_length, 4u);
    EXPECT_EQ(data.size(), 4u);

    u32 elev_type = 0;
    std::memcpy(&elev_type, data.data(), 4);
    // TokenElevationTypeDefault: no UAC split token. Deliberate — reporting
    // TokenElevationTypeFull makes installers (e.g. Spotify) refuse to run
    // with an "Administrator account" warning.
    EXPECT_EQ(elev_type, 1u);
}

TEST_F(SecurityStubsTest, GetTokenInformation_VirtualizationClasses_ReturnZero) {
    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(token, 23, data, return_length));
    EXPECT_EQ(return_length, 4u);
    u32 allowed = 0xFFFFFFFFu;
    std::memcpy(&allowed, data.data(), 4);
    EXPECT_EQ(allowed, 0u);

    ASSERT_TRUE(m_stubs->get_token_information(token, 24, data, return_length));
    EXPECT_EQ(return_length, 4u);
    u32 enabled = 0xFFFFFFFFu;
    std::memcpy(&enabled, data.data(), 4);
    EXPECT_EQ(enabled, 0u);
}

TEST_F(SecurityStubsTest, RestrictedTokenDisablesSidsInTokenGroups) {
    Handle source = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, source));

    // BUILTIN\Users S-1-5-32-545 — present in the default TokenGroups list.
    SecurityStubs::RestrictedSid users{
        {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
         0x20, 0x00, 0x00, 0x00, 0x21, 0x02, 0x00, 0x00},
        0};
    Handle restricted = INVALID_HANDLE;
    ASSERT_TRUE(m_stubs->create_restricted_token(
        source, 0, {users}, {}, {}, restricted));

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 2, data, return_length, true));
    ASSERT_GE(data.size(), 8u);
    u32 count = 0;
    std::memcpy(&count, data.data(), sizeof(count));
    ASSERT_GE(count, 1u);
    constexpr u32 SE_GROUP_USE_FOR_DENY_ONLY = 0x10u;
    bool found_deny_only = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 entry = 8u + i * 16u;
        u32 attrs = 0;
        std::memcpy(&attrs, data.data() + entry + 8u, sizeof(attrs));
        if ((attrs & SE_GROUP_USE_FOR_DENY_ONLY) != 0) {
            found_deny_only = true;
            break;
        }
    }
    EXPECT_TRUE(found_deny_only);
}

TEST_F(SecurityStubsTest, RestrictedTokenDisabledSidsRoundTripThroughLaunchEnv) {
    Handle source = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, source));
    SecurityStubs::RestrictedSid users{
        {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
         0x20, 0x00, 0x00, 0x00, 0x21, 0x02, 0x00, 0x00},
        0};
    Handle restricted = INVALID_HANDLE;
    ASSERT_TRUE(m_stubs->create_restricted_token(
        source, 0, {users}, {}, {}, restricted));

    const auto environment = m_stubs->token_launch_environment(restricted);
    std::string disabled_env;
    for (const auto& entry : environment) {
        constexpr const char* kPrefix = "VITREO_TOKEN_DISABLED_SIDS=";
        if (entry.rfind(kPrefix, 0) == 0) {
            disabled_env = entry.substr(std::strlen(kPrefix));
            break;
        }
    }
    ASSERT_FALSE(disabled_env.empty());
    ASSERT_EQ(::setenv("VITREO_TOKEN_DISABLED_SIDS", disabled_env.c_str(), 1),
              0);
    ASSERT_EQ(::setenv("VITREO_TOKEN_RESTRICTED", "1", 1), 0);

    auto child_handles = std::make_unique<HandleTable>();
    auto child_stubs = std::make_unique<SecurityStubs>(*child_handles);
    ::unsetenv("VITREO_TOKEN_DISABLED_SIDS");
    ::unsetenv("VITREO_TOKEN_RESTRICTED");

    Handle child_primary = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*child_stubs, child_primary));
    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(child_stubs->get_token_information(
        child_primary, 2, data, return_length, true));
    ASSERT_GE(data.size(), 8u);
    u32 count = 0;
    std::memcpy(&count, data.data(), sizeof(count));
    constexpr u32 SE_GROUP_USE_FOR_DENY_ONLY = 0x10u;
    bool found_deny_only = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 entry = 8u + i * 16u;
        u32 attrs = 0;
        std::memcpy(&attrs, data.data() + entry + 8u, sizeof(attrs));
        if ((attrs & SE_GROUP_USE_FOR_DENY_ONLY) != 0) {
            found_deny_only = true;
            break;
        }
    }
    EXPECT_TRUE(found_deny_only);
}

TEST_F(SecurityStubsTest, RestrictedTokenPreservesObservableRestrictionState) {
    Handle source = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, source));

    SecurityStubs::RestrictedSid restricting_sid{
        SecurityStubs::synthetic_sid(), 0};
    Handle restricted = INVALID_HANDLE;
    ASSERT_TRUE(m_stubs->create_restricted_token(
        source, 0x02u, {}, {23u, 24u}, {restricting_sid}, restricted));
    ASSERT_NE(restricted, INVALID_HANDLE);

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 15, data, return_length));
    ASSERT_EQ(return_length, 4u);
    u32 sandbox_inert = 0;
    std::memcpy(&sandbox_inert, data.data(), sizeof(sandbox_inert));
    EXPECT_EQ(sandbox_inert, 1u);

    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 21, data, return_length));
    u32 has_restrictions = 0;
    std::memcpy(&has_restrictions, data.data(), sizeof(has_restrictions));
    EXPECT_EQ(has_restrictions, 1u);

    // On Windows, WOW64 rejects TokenIsRestricted (40); the native x64
    // query returns the restricting-SID state.
    EXPECT_FALSE(m_stubs->get_token_information(
        restricted, 40, data, return_length, /*is64=*/false));
    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 40, data, return_length, /*is64=*/true));
    ASSERT_EQ(return_length, 4u);
    u32 is_restricted = 0;
    std::memcpy(&is_restricted, data.data(), sizeof(is_restricted));
    EXPECT_EQ(is_restricted, 1u);
    EXPECT_TRUE(m_stubs->token_is_restricted(restricted));

    // TokenSecurityAttributes (39): empty V1 header when none are set.
    // x64 header is 16 bytes; pass is64 so the layout matches Chrome PE.
    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 39, data, return_length, /*is64=*/true));
    ASSERT_GE(data.size(), 16u);
    u16 version = 0;
    u32 attr_count = 0xFFFFFFFFu;
    std::memcpy(&version, data.data(), sizeof(version));
    std::memcpy(&attr_count, data.data() + 4, sizeof(attr_count));
    EXPECT_EQ(version, 1u);
    EXPECT_EQ(attr_count, 0u);

    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 11, data, return_length, true));
    ASSERT_GE(data.size(), 8u + 16u + restricting_sid.bytes.size());
    u32 restricted_sid_count = 0;
    std::memcpy(&restricted_sid_count, data.data(),
                sizeof(restricted_sid_count));
    EXPECT_EQ(restricted_sid_count, 1u);
    EXPECT_EQ(data[8u + 16u], 1u);  // SID revision

    Handle duplicate = INVALID_HANDLE;
    ASSERT_TRUE(m_stubs->duplicate_token(
        restricted, 2, duplicate, 1));
    ASSERT_TRUE(m_stubs->get_token_information(
        duplicate, 21, data, return_length));
    std::memcpy(&has_restrictions, data.data(), sizeof(has_restrictions));
    EXPECT_EQ(has_restrictions, 1u);
    ASSERT_TRUE(m_stubs->get_token_information(
        duplicate, 8, data, return_length));
    u32 token_type = 0;
    std::memcpy(&token_type, data.data(), sizeof(token_type));
    EXPECT_EQ(token_type, 1u);
}

TEST_F(SecurityStubsTest, TokenSecurityAttributesRoundTrip) {
    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(
        token, 40, data, return_length, /*is64=*/true));
    u32 is_restricted = 1;
    std::memcpy(&is_restricted, data.data(), sizeof(is_restricted));
    EXPECT_EQ(is_restricted, 0u);
    EXPECT_FALSE(m_stubs->token_is_restricted(token));

    SecurityStubs::SecurityAttribute attr;
    attr.name = L"WIN://TESTATTR";
    attr.value = L"vitreo";
    attr.flags = 0x20;  // TOKEN_SECURITY_ATTRIBUTE_MANDATORY
    attr.value_type = 3;
    ASSERT_TRUE(m_stubs->set_token_security_attributes(
        token, {attr}, false));

    ASSERT_TRUE(m_stubs->get_token_information(
        token, 39, data, return_length, true));
    ASSERT_GE(data.size(), 16u);
    u16 version = 0;
    u32 attr_count = 0;
    std::memcpy(&version, data.data(), sizeof(version));
    std::memcpy(&attr_count, data.data() + 4, sizeof(attr_count));
    EXPECT_EQ(version, 1u);
    EXPECT_EQ(attr_count, 1u);
    u32 attrs_rel = 0;
    std::memcpy(&attrs_rel, data.data() + 8, sizeof(attrs_rel));
    // attrs_rel is relative; name UTF-16 follows inside the buffer.
    EXPECT_NE(attrs_rel, 0u);
    EXPECT_LT(attrs_rel, data.size());
}

TEST_F(SecurityStubsTest, TokenLogonSidUsesTokenGroupsX64Layout) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x20000;
    constexpr GuestAddr kStack = kData + 0x8000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x10000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* get_info = find_export(exports, "GetTokenInformation");
    ASSERT_NE(get_info, nullptr);

    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));
    constexpr GuestAddr kOut = kData + 0x100;
    constexpr GuestAddr kReturnLength = kData + 0x200;
    cpu.gpr.rcx = token;
    cpu.gpr.rdx = 28; // TokenLogonSid
    cpu.gpr.r8 = kOut;
    cpu.gpr.r9 = 64;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, kReturnLength).is_ok());
    get_info->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    ASSERT_EQ(memory.read_u32(kOut).value_or(0), 1u);
    const GuestAddr sid = memory.read_u64(kOut + 8).value_or(0);
    EXPECT_EQ(sid, kOut + 24);
    EXPECT_EQ(memory.read_u32(kOut + 16).value_or(0), 0xC0000007u);
    EXPECT_EQ(memory.read_u8(sid).value_or(0), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value_or(0), 3u);
    EXPECT_EQ(memory.read_u32(sid + 8).value_or(0), 5u);
}

TEST_F(SecurityStubsTest, TokenDefaultDaclAndIntegrityRoundTripThroughX64Thunks) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x20000;
    constexpr GuestAddr kStack = kData + 0x8000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x10000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* set_info = find_export(exports, "SetTokenInformation");
    auto* get_info = find_export(exports, "GetTokenInformation");
    ASSERT_NE(set_info, nullptr);
    ASSERT_NE(get_info, nullptr);

    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));

    constexpr GuestAddr kAcl = kData + 0x100;
    constexpr GuestAddr kDefaultDacl = kData + 0x180;
    const std::array<u8, 8> acl{2, 0, 8, 0, 0, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(kAcl, acl.data(), acl.size()).is_ok());
    ASSERT_TRUE(memory.write_u64(kDefaultDacl, kAcl).is_ok());
    cpu.gpr.rcx = token;
    cpu.gpr.rdx = 6; // TokenDefaultDacl
    cpu.gpr.r8 = kDefaultDacl;
    cpu.gpr.r9 = 8;
    set_info->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    constexpr GuestAddr kOut = kData + 0x200;
    constexpr GuestAddr kReturnLength = kData + 0x280;
    cpu.gpr.rcx = token;
    cpu.gpr.rdx = 6;
    cpu.gpr.r8 = kOut;
    cpu.gpr.r9 = 64;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, kReturnLength).is_ok());
    get_info->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const GuestAddr returned_acl =
        memory.read_u64(kOut).value_or(0);
    EXPECT_EQ(returned_acl, kOut + 8);
    EXPECT_EQ(memory.read_u16(returned_acl + 2).value_or(0), 8u);

    constexpr GuestAddr kIntegrity = kData + 0x300;
    constexpr GuestAddr kIntegritySid = kData + 0x380;
    const std::array<u8, 12> low_integrity_sid{
        1, 1, 0, 0, 0, 0, 0, 16, 0, 16, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        kIntegritySid, low_integrity_sid.data(),
        low_integrity_sid.size()).is_ok());
    ASSERT_TRUE(memory.write_u64(kIntegrity, kIntegritySid).is_ok());
    ASSERT_TRUE(memory.write_u32(kIntegrity + 8, 0x20).is_ok());
    cpu.gpr.rcx = token;
    cpu.gpr.rdx = 25; // TokenIntegrityLevel
    cpu.gpr.r8 = kIntegrity;
    cpu.gpr.r9 = 16;
    set_info->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(
        token, 25, data, return_length, true));
    ASSERT_GE(data.size(), 20u);
    u32 integrity_rid = 0;
    std::memcpy(&integrity_rid, data.data() + 24,
                sizeof(integrity_rid));
    EXPECT_EQ(integrity_rid, 0x1000u);
}

TEST(SecurityStubsElevationTest, ElevatedStateIsReportedByTokenInformation) {
    HandleTable handles;
    GuestElevationState elevation{true};
    SecurityStubs stubs{handles, nullptr, &elevation};
    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(stubs, token));

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(stubs.get_token_information(
        token, 20, data, return_length));
    ASSERT_EQ(data.size(), 4u);
    u32 elevated = 0;
    std::memcpy(&elevated, data.data(), sizeof(elevated));
    EXPECT_EQ(elevated, 1u);

    ASSERT_TRUE(stubs.get_token_information(
        token, 25, data, return_length));
    ASSERT_GE(data.size(), 20u);
    u32 integrity_rid = 0;
    std::memcpy(&integrity_rid, data.data() + 16, sizeof(integrity_rid));
    EXPECT_EQ(integrity_rid, 0x3000u);
}

TEST(SecurityStubsElevationTest, ElevatedTokenGroupsIncludeAdministrators) {
    HandleTable handles;
    GuestElevationState elevation{true};
    SecurityStubs stubs{handles, nullptr, &elevation};
    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(stubs, token));

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(stubs.get_token_information(
        token, 2, data, return_length, /*is64=*/false));
    ASSERT_GE(data.size(), 4u);
    u32 count = 0;
    std::memcpy(&count, data.data(), sizeof(count));
    ASSERT_GE(count, 1u);

    // x86 SID_AND_ATTRIBUTES: Sid offset @0, Attributes @4, SID bytes follow.
    constexpr u32 kSaaSize = 8;
    constexpr u32 kEntriesOffset = 4;
    constexpr u8 kAdminSid[] = {
        1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 32, 2, 0, 0}; // S-1-5-32-544
    bool found_admin = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 entry = kEntriesOffset + i * kSaaSize;
        u32 sid_off = 0;
        u32 attrs = 0;
        std::memcpy(&sid_off, data.data() + entry, 4);
        std::memcpy(&attrs, data.data() + entry + 4, 4);
        ASSERT_LT(sid_off + sizeof(kAdminSid), data.size() + 1);
        if (std::memcmp(data.data() + sid_off, kAdminSid,
                        sizeof(kAdminSid)) == 0) {
            found_admin = true;
            EXPECT_NE(attrs & 0x4u, 0u); // SE_GROUP_ENABLED
            break;
        }
    }
    EXPECT_TRUE(found_admin);

    GuestElevationState unelevated{false};
    SecurityStubs plain{handles, nullptr, &unelevated};
    Handle plain_token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(plain, plain_token));
    ASSERT_TRUE(plain.get_token_information(
        plain_token, 2, data, return_length, /*is64=*/false));
    std::memcpy(&count, data.data(), sizeof(count));
    found_admin = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 entry = kEntriesOffset + i * kSaaSize;
        u32 sid_off = 0;
        std::memcpy(&sid_off, data.data() + entry, 4);
        if (sid_off + sizeof(kAdminSid) <= data.size() &&
            std::memcmp(data.data() + sid_off, kAdminSid,
                        sizeof(kAdminSid)) == 0) {
            found_admin = true;
            break;
        }
    }
    EXPECT_FALSE(found_admin);
}
