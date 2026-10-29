// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_tokens.cpp
/// @brief Unit tests for advapi32 token open, adjust and query (WIN32-062).

#include "security_stubs_fixture.hpp"

TEST_F(SecurityStubsTest,
       ComCallerImpersonationPreservesTransportedElevationIdentity) {
    constexpr Handle CURRENT_THREAD = 0xFFFFFFFEu;
    constexpr u32 TOKEN_QUERY = 0x0008;
    u32 error = error_codes::ERROR_SUCCESS;
    Handle token = NULL_HANDLE;

    ASSERT_TRUE(m_stubs->impersonate_com_caller(true));
    ASSERT_TRUE(m_stubs->open_thread_token(
        CURRENT_THREAD, TOKEN_QUERY, token, error));
    EXPECT_TRUE(m_stubs->token_is_elevated(token));
    EXPECT_TRUE(m_stubs->revert_to_self());

    ASSERT_TRUE(m_stubs->impersonate_com_caller(false));
    ASSERT_TRUE(m_stubs->open_thread_token(
        CURRENT_THREAD, TOKEN_QUERY, token, error));
    EXPECT_FALSE(m_stubs->token_is_elevated(token));
    EXPECT_TRUE(m_stubs->revert_to_self());
}

// =============================================================================
// 4. OpenProcessToken returns valid handle
// =============================================================================

TEST_F(SecurityStubsTest, OpenProcessToken_ReturnsValidHandle) {
    Handle token = INVALID_HANDLE;
    EXPECT_TRUE(open_primary_token(*m_stubs, token));
    EXPECT_NE(token, INVALID_HANDLE);
    EXPECT_TRUE(m_handles->is_valid(token));
}

TEST_F(SecurityStubsTest, OpenProcessToken_ReturnsIndependentClosableHandles) {
    Handle first = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, first));
    ASSERT_TRUE(m_handles->close(first));
    EXPECT_FALSE(m_stubs->is_token_handle(first));

    Handle second = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, second));
    EXPECT_NE(second, first);
    EXPECT_TRUE(m_stubs->is_token_handle(second));
    EXPECT_TRUE(m_handles->close(second));
}

TEST_F(SecurityStubsTest, OpenProcessToken_SuccessPreservesLastError) {
    // InstallShield IsNTAdmin: OpenThreadToken fails with ERROR_NO_TOKEN,
    // OpenProcessToken succeeds, then GetLastError must still be NO_TOKEN
    // so the probe continues into GetTokenInformation.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x4000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr kStack = 0x1800;
    constexpr GuestAddr kOut = 0x1900;
    cpu.set_sp(kStack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* open = find_export(exports, "OpenProcessToken");
    ASSERT_NE(open, nullptr);

    ASSERT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 8, 0x8).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 12, kOut).is_ok());
    set_last_error(error_codes::ERROR_NO_TOKEN);
    open->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NO_TOKEN);
    EXPECT_NE(memory.read_u32(kOut).value_or(0), 0u);
}

TEST_F(SecurityStubsTest, AdjustTokenPrivilegesWritesPreviousStateAndEnablesRestore) {
    // Learn: PreviousState receives the prior LUID_AND_ATTRIBUTES of privileges
    // the call actually modifies. SeRestorePrivilege is present but disabled
    // on a typical Administrators token, so enabling it must succeed.
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr new_state = data;
    constexpr GuestAddr previous = data + 0x40;
    constexpr GuestAddr return_length = data + 0x80;
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

    Handle token = NULL_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));

    u64 restore_luid = 0;
    ASSERT_TRUE(m_stubs->lookup_privilege_value_w(
        L"SeRestorePrivilege", restore_luid));
    ASSERT_TRUE(memory.write_u32(new_state, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(new_state + 4, restore_luid).is_ok());
    ASSERT_TRUE(memory.write_u32(new_state + 12, 0x2u).is_ok()); // ENABLED
    ASSERT_TRUE(memory.write_u32(return_length, 0).is_ok());

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* entry = find_export(exports, "AdjustTokenPrivileges");
    ASSERT_NE(entry, nullptr);
    auto arg = [&](u32 index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * 4, value).is_ok());
    };
    arg(0, static_cast<u32>(token));
    arg(1, 0);
    arg(2, static_cast<u32>(new_state));
    arg(3, 4); // too small for one LUID_AND_ATTRIBUTES
    arg(4, static_cast<u32>(previous));
    arg(5, static_cast<u32>(return_length));
    set_last_error(error_codes::ERROR_SUCCESS);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(return_length).value_or(0), 16u);

    arg(3, 16);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u32(previous).value_or(0xFFu), 1u);
    EXPECT_EQ(memory.read_u64(previous + 4).value_or(0), restore_luid);
    EXPECT_EQ(memory.read_u32(previous + 12).value_or(0xFFu), 0u);
}

TEST_F(SecurityStubsTest,
       ThreadImpersonationTokenCanBeSetOpenedQueriedAndRemoved) {
    constexpr u32 TOKEN_IMPERSONATE = 0x0004;
    constexpr u32 TOKEN_QUERY = 0x0008;
    const Handle thread =
        m_handles->allocate(HandleType::Thread, nullptr);
    ASSERT_NE(thread, NULL_HANDLE);

    Handle primary = NULL_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, primary));
    Handle impersonation = NULL_HANDLE;
    ASSERT_TRUE(m_stubs->duplicate_token(
        primary, 2, impersonation, 2,
        TOKEN_IMPERSONATE | TOKEN_QUERY));

    u32 error = 0;
    ASSERT_TRUE(m_stubs->set_thread_token(thread, impersonation, error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);

    Handle opened = NULL_HANDLE;
    ASSERT_TRUE(
        m_stubs->open_thread_token(thread, TOKEN_QUERY, opened, error));
    ASSERT_NE(opened, NULL_HANDLE);
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(
        opened, 8, data, return_length));
    ASSERT_EQ(return_length, sizeof(u32));
    u32 type = 0;
    std::memcpy(&type, data.data(), sizeof(type));
    EXPECT_EQ(type, 2u);

    ASSERT_TRUE(m_stubs->get_token_information(
        opened, 9, data, return_length));
    u32 level = 0;
    std::memcpy(&level, data.data(), sizeof(level));
    EXPECT_EQ(level, 2u);

    ASSERT_TRUE(m_stubs->get_token_information(
        opened, 10, data, return_length, true));
    ASSERT_EQ(return_length, 56u);
    ASSERT_EQ(data.size(), 56u);
    u64 token_id = 0;
    std::memcpy(&token_id, data.data(), sizeof(token_id));
    EXPECT_NE(token_id, 0u);
    std::memcpy(&type, data.data() + 24, sizeof(type));
    std::memcpy(&level, data.data() + 28, sizeof(level));
    EXPECT_EQ(type, 2u);
    EXPECT_EQ(level, 2u);
    u32 group_count = 0;
    u32 privilege_count = 0;
    std::memcpy(&group_count, data.data() + 40, sizeof(group_count));
    std::memcpy(&privilege_count, data.data() + 44,
                sizeof(privilege_count));
    EXPECT_EQ(group_count, 5u);
    EXPECT_EQ(privilege_count,
              SecurityStubs::default_process_token_privileges().size());

    ASSERT_TRUE(
        m_stubs->set_thread_token(thread, NULL_HANDLE, error));
    EXPECT_FALSE(
        m_stubs->open_thread_token(thread, TOKEN_QUERY, opened, error));
    EXPECT_EQ(error, error_codes::ERROR_NO_TOKEN);
}

TEST_F(SecurityStubsTest,
       SetThreadTokenX64ThunkDereferencesTheThreadHandlePointer) {
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
    auto* set_thread_token = find_export(exports, "SetThreadToken");
    ASSERT_NE(set_thread_token, nullptr);

    const Handle thread =
        m_handles->allocate(HandleType::Thread, nullptr);
    Handle primary = NULL_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, primary));
    Handle impersonation = NULL_HANDLE;
    ASSERT_TRUE(m_stubs->duplicate_token(
        primary, 2, impersonation, 2, 0x000Cu));

    constexpr GuestAddr kThreadPointer = kData + 0x100;
    ASSERT_TRUE(memory.write_u64(kThreadPointer, thread).is_ok());
    cpu.gpr.rcx = kThreadPointer;
    cpu.gpr.rdx = impersonation;
    set_thread_token->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    Handle opened = NULL_HANDLE;
    u32 error = 0;
    EXPECT_TRUE(
        m_stubs->open_thread_token(thread, 0x0008u, opened, error));
    EXPECT_NE(opened, NULL_HANDLE);
}

// =============================================================================
// 5. GetTokenInformation for TokenUser returns SID bytes
// =============================================================================

TEST_F(SecurityStubsTest, GetTokenInformation_TokenUser_ReturnsSID) {
    Handle token = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, token));

    std::vector<u8> data;
    u32 return_length = 0;
    EXPECT_TRUE(m_stubs->get_token_information(token, 1, data, return_length));
    EXPECT_FALSE(data.empty());
    EXPECT_GT(return_length, 0u);

    // TOKEN_USER header is 8 bytes, SID starts at offset 8
    ASSERT_GT(data.size(), 9u);
    // First byte of SID is revision = 1
    EXPECT_EQ(data[8], 0x01);
    // Second byte is SubAuthorityCount = 5
    EXPECT_EQ(data[9], 0x05);
}

// =============================================================================
// 6. GetTokenInformation with invalid token fails
// =============================================================================

TEST_F(SecurityStubsTest, GetTokenInformation_InvalidToken_ReturnsFalse) {
    std::vector<u8> data;
    u32 return_length = 0;
    EXPECT_FALSE(m_stubs->get_token_information(INVALID_HANDLE, 1, data,
                                                return_length));
}

// =============================================================================
// 6b. GetTokenInformation accepts NT pseudo-token handles (no Open* needed)
// =============================================================================

TEST_F(SecurityStubsTest, GetTokenInformation_PseudoHandles_Accepted) {
    // GetCurrentProcessToken()=-4, GetCurrentThreadToken()=-5,
    // GetCurrentThreadEffectiveToken()=-6 — Crashpad/chrome_elf query
    // TokenUser through these without ever calling OpenProcessToken.
    for (Handle pseudo : {0xFFFFFFFCu, 0xFFFFFFFBu, 0xFFFFFFFAu}) {
        std::vector<u8> data;
        u32 return_length = 0;
        EXPECT_TRUE(m_stubs->get_token_information(pseudo, 1, data,
                                                   return_length))
            << "pseudo handle 0x" << std::hex << pseudo;
    }
}

// =============================================================================
// 6c. GetTokenInformation x64 layout: PSID @0 (8B), attrs @8, SID data @16
// =============================================================================

TEST_F(SecurityStubsTest, GetTokenInformation_TokenUser_X64Layout) {
    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(0xFFFFFFFCu, 1, data,
                                               return_length, /*is64=*/true));
    ASSERT_GT(data.size(), 17u);
    // Relative SID offset stored in the pointer field = 16 (start of SID data)
    u32 rel = 0;
    std::memcpy(&rel, data.data(), 4);
    EXPECT_EQ(rel, 16u);
    // SID revision/subauthority-count at offset 16
    EXPECT_EQ(data[16], 0x01);
    EXPECT_EQ(data[17], 0x05);
}

TEST_F(SecurityStubsTest, GetTokenInformation_TokenOwnerReturnsTokenUserSid) {
    for (const bool is64 : {false, true}) {
        std::vector<u8> data;
        u32 return_length = 0;
        ASSERT_TRUE(m_stubs->get_token_information(
            0xFFFFFFFCu, 4, data, return_length, is64));

        const u32 sid_offset = is64 ? 8u : 4u;
        EXPECT_EQ(return_length,
                  sid_offset + SecurityStubs::synthetic_sid().size());
        u32 relative = 0;
        std::memcpy(&relative, data.data(), sizeof(relative));
        EXPECT_EQ(relative, sid_offset);
        ASSERT_GT(data.size(), sid_offset + 1);
        EXPECT_EQ(data[sid_offset], 1u);     // SID revision
        EXPECT_EQ(data[sid_offset + 1], 5u); // subauthority count
    }
}

TEST_F(SecurityStubsTest, GetTokenInformation_TokenPrimaryGroupReturnsUsersSid) {
    for (const bool is64 : {false, true}) {
        std::vector<u8> data;
        u32 return_length = 0;
        ASSERT_TRUE(m_stubs->get_token_information(
            0xFFFFFFFCu, 5, data, return_length, is64));

        const u32 sid_offset = is64 ? 8u : 4u;
        EXPECT_EQ(return_length, sid_offset + 16u);
        u32 relative = 0;
        std::memcpy(&relative, data.data(), sizeof(relative));
        EXPECT_EQ(relative, sid_offset);
        ASSERT_GE(data.size(), sid_offset + 16u);
        EXPECT_EQ(data[sid_offset], 1u);
        EXPECT_EQ(data[sid_offset + 1], 2u);
        u32 builtin = 0;
        u32 users = 0;
        std::memcpy(&builtin, data.data() + sid_offset + 8, 4);
        std::memcpy(&users, data.data() + sid_offset + 12, 4);
        EXPECT_EQ(builtin, 32u);
        EXPECT_EQ(users, 545u);
    }
}

// =============================================================================
// 7. Token privilege state follows Windows token-object aliasing
// =============================================================================

TEST_F(SecurityStubsTest, RestrictedTokenPrivilegeRemovalIsVisibleThroughDuplicateHandle) {
    Handle source = INVALID_HANDLE;
    ASSERT_TRUE(open_primary_token(*m_stubs, source));
    Handle restricted = INVALID_HANDLE;
    ASSERT_TRUE(m_stubs->create_restricted_token(
        source, 0x01u, {}, {}, {}, restricted));

    std::vector<u8> data;
    u32 return_length = 0;
    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 3, data, return_length));
    ASSERT_EQ(return_length, 16u);
    u32 count = 0;
    u64 luid = 0;
    std::memcpy(&count, data.data(), sizeof(count));
    std::memcpy(&luid, data.data() + 4, sizeof(luid));
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(luid, 23u); // SeChangeNotifyPrivilege

    Handle duplicate = INVALID_HANDLE;
    ASSERT_TRUE(m_stubs->duplicate_token_handle(
        restricted, HandleAccess::All, 0, duplicate));
    bool all_assigned = false;
    ASSERT_TRUE(m_stubs->adjust_token_privileges(
        duplicate, false, {{23u, 0x04u}}, all_assigned));
    EXPECT_TRUE(all_assigned);

    ASSERT_TRUE(m_stubs->get_token_information(
        restricted, 3, data, return_length));
    ASSERT_EQ(return_length, 4u);
    std::memcpy(&count, data.data(), sizeof(count));
    EXPECT_EQ(count, 0u);
}
