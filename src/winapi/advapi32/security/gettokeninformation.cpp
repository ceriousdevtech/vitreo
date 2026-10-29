// SPDX-License-Identifier: LGPL-2.1-only

/// @file security.cpp
/// @brief advapi32 security stubs implementation (WIN32-062)

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"

#include <cstdio>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <stdlib.h> // arc4random_buf
#include <unistd.h>
#include <pwd.h>
#include <cerrno>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <set>
#include <variant>
#include <CommonCrypto/CommonDigest.h>

#ifdef __APPLE__
#include <os/log.h>
#include <sys/xattr.h>
#endif

/// @file gettokeninformation.cpp
/// @brief GetTokenInformation
///
/// Closed cut from security.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// GetTokenInformation
// =============================================================================

bool SecurityStubs::is_token_handle(Handle token) const {
    if (m_handles.get_type(token) == HandleType::Token) return true;
    // NT pseudo-token handles (kernel32 GetCurrentProcessToken/
    // GetCurrentThreadToken/GetCurrentThreadEffectiveToken return these
    // constants without any advapi32 Open* call): -4, -5, -6.
    return token == 0xFFFFFFFCu || token == 0xFFFFFFFBu || token == 0xFFFFFFFAu;
}

SecurityStubs::TokenState SecurityStubs::token_state(Handle token) const {
    if (m_handles.get_type(token) == HandleType::Token) {
        std::lock_guard lock(m_token_mutex);
        if (const auto found = m_token_states.find(token);
            found != m_token_states.end()) {
            return *found->second;
        }
    }
    // The NT current-token pseudo handles describe the process primary token
    // (the shared object once any OpenProcessToken handle exists).
    std::lock_guard lock(m_token_mutex);
    return m_primary_token_object ? *m_primary_token_object
                                  : m_primary_token_state;
}

bool SecurityStubs::get_token_information(Handle token, u32 info_class,
                                          std::vector<u8>& data_out,
                                          u32& return_length, bool is64) {
    if (!is_token_handle(token)) return false;

    // SID_AND_ATTRIBUTES layout differs by bitness: PSID is 4 bytes on x86
    // (attrs @4, SID data @8) and 8 bytes on x64 (attrs @8, pad, SID @16).
    // The Sid pointer is stored as a relative offset; the thunk converts it
    // to an absolute guest pointer at the correct width.
    const u32 saa_size = is64 ? 16u : 8u;
    const TokenState state = token_state(token);

    // ANONYMOUS LOGON (S-1-5-7): ImpersonateAnonymousToken's identity.
    static const std::vector<u8> kAnonymousSid{1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0};

    if (info_class == 1) { // TokenUser
        // TOKEN_USER: { SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; } }
        const auto& sid = state.anonymous ? kAnonymousSid : synthetic_sid();
        u32 total = saa_size + static_cast<u32>(sid.size());
        return_length = total;
        data_out.assign(total, 0);
        u32 sid_offset = saa_size;
        std::memcpy(data_out.data(), &sid_offset, 4);   // Sid (relative offset)
        // Attributes = 0 (already zeroed)
        std::memcpy(data_out.data() + saa_size, sid.data(), sid.size());
        return true;
    }
    if (info_class == 2) { // TokenGroups
        struct Group {
            std::vector<u8> sid;
            u32 attributes;
        };
        auto make_sid = [](std::array<u8, 6> authority,
                           std::initializer_list<u32> subauthorities) {
            std::vector<u8> sid(
                8 + subauthorities.size() * sizeof(u32), 0);
            sid[0] = 1;
            sid[1] = static_cast<u8>(subauthorities.size());
            std::memcpy(sid.data() + 2, authority.data(), authority.size());
            usize offset = 8;
            for (const u32 subauthority : subauthorities) {
                std::memcpy(sid.data() + offset, &subauthority, sizeof(u32));
                offset += sizeof(u32);
            }
            return sid;
        };
        constexpr u32 SE_GROUP_MANDATORY = 0x00000001;
        constexpr u32 SE_GROUP_ENABLED_BY_DEFAULT = 0x00000002;
        constexpr u32 SE_GROUP_ENABLED = 0x00000004;
        constexpr u32 SE_GROUP_OWNER = 0x00000008;
        constexpr u32 SE_GROUP_USE_FOR_DENY_ONLY = 0x00000010;
        constexpr u32 SE_GROUP_LOGON_ID = 0xC0000000;
        constexpr u32 enabled = SE_GROUP_MANDATORY |
                                SE_GROUP_ENABLED_BY_DEFAULT |
                                SE_GROUP_ENABLED;
        const std::array<u8, 6> nt_authority{0, 0, 0, 0, 0, 5};
        const std::array<u8, 6> world_authority{0, 0, 0, 0, 0, 1};
        // Elevated primary tokens include an enabled BUILTIN\Administrators
        // membership (S-1-5-32-544). Installers walk TokenGroups + EqualSid
        // rather than IsUserAnAdmin; omitting this SID makes an elevated
        // guest look like a standard user.
        std::vector<Group> groups;
        if (state.anonymous) {
            // One group: the untrusted mandatory label (advapi_token2_probe
            // anon_user "groups=1 S-1-16-0/60").
            groups.push_back({make_sid({0, 0, 0, 0, 0, 16}, {0}), 0x60});
        } else if (state.elevated) {
            groups.push_back({make_sid(nt_authority, {32, 544}),
                              enabled | SE_GROUP_OWNER});
        }
        if (!state.anonymous) {
        groups.push_back({make_sid(nt_authority, {32, 545}), enabled}); // Users
        groups.push_back({make_sid(world_authority, {0}), enabled});    // Everyone
        groups.push_back({make_sid(nt_authority, {4}), enabled});       // Interactive
        groups.push_back({make_sid(nt_authority, {11}), enabled}); // Authenticated
        groups.push_back({make_sid(nt_authority, {5, 0, 1}),
                          enabled | SE_GROUP_LOGON_ID}); // Logon SID
        }

        // CreateRestrictedToken(SidsToDisable): Learn marks matching groups
        // SE_GROUP_USE_FOR_DENY_ONLY and clears the enabled bits.
        for (auto& group : groups) {
            for (const auto& disabled : state.disabled_sids) {
                if (disabled.bytes == group.sid) {
                    group.attributes = SE_GROUP_USE_FOR_DENY_ONLY;
                    break;
                }
            }
        }
        for (const auto& disabled : state.disabled_sids) {
            bool present = false;
            for (const auto& group : groups) {
                if (group.sid == disabled.bytes) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                groups.push_back({disabled.bytes, SE_GROUP_USE_FOR_DENY_ONLY});
            }
        }

        const u32 count = static_cast<u32>(groups.size());
        const u32 entries_offset = is64 ? 8u : 4u;
        const u32 sid_data_offset = entries_offset + count * saa_size;
        u32 sid_bytes = 0;
        for (const auto& group : groups) {
            sid_bytes += static_cast<u32>(group.sid.size());
        }
        return_length = sid_data_offset + sid_bytes;
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data(), &count, 4);
        u32 next_sid = sid_data_offset;
        for (u32 index = 0; index < count; ++index) {
            const u32 entry = entries_offset + index * saa_size;
            std::memcpy(data_out.data() + entry, &next_sid, 4);
            std::memcpy(data_out.data() + entry + (is64 ? 8u : 4u),
                        &groups[index].attributes, 4);
            std::memcpy(data_out.data() + next_sid,
                        groups[index].sid.data(), groups[index].sid.size());
            next_sid += static_cast<u32>(groups[index].sid.size());
        }
        return true;
    }
    if (info_class == 3) { // TokenPrivileges
        constexpr u32 header_size = 4;
        constexpr u32 entry_size = 12; // LUID_AND_ATTRIBUTES
        const u32 count = static_cast<u32>(state.privileges.size());
        return_length = header_size + count * entry_size;
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data(), &count, sizeof(count));
        for (u32 index = 0; index < count; ++index) {
            const u32 offset = header_size + index * entry_size;
            std::memcpy(data_out.data() + offset,
                        &state.privileges[index].luid, sizeof(u64));
            std::memcpy(data_out.data() + offset + 8,
                        &state.privileges[index].attributes, sizeof(u32));
        }
        return true;
    }
    if (info_class == 4) { // TokenOwner
        // TOKEN_OWNER contains a PSID. The default owner is the synthetic
        // token user, which satisfies Windows' requirement that Owner be a
        // user or owner-capable group SID already present in the token.
        const auto& sid = state.anonymous ? kAnonymousSid : synthetic_sid();
        const u32 sid_offset = is64 ? 8u : 4u;
        return_length = sid_offset + static_cast<u32>(sid.size());
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data(), &sid_offset, sizeof(sid_offset));
        std::memcpy(data_out.data() + sid_offset, sid.data(), sid.size());
        return true;
    }
    if (info_class == 5) { // TokenPrimaryGroup
        // TOKEN_PRIMARY_GROUP contains a PSID to a group already present in
        // the token. Use BUILTIN\Users (S-1-5-32-545), which TokenGroups
        // exposes for every synthetic primary token.
        const std::array<u8, 16> users_sid{
            1, 2, 0, 0, 0, 0, 0, 5,
            32, 0, 0, 0,
            0x21, 0x02, 0, 0,
        };
        const u32 sid_offset = is64 ? 8u : 4u;
        return_length = sid_offset + static_cast<u32>(users_sid.size());
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data(), &sid_offset, sizeof(sid_offset));
        std::memcpy(data_out.data() + sid_offset,
                    users_sid.data(), users_sid.size());
        return true;
    }
    if (info_class == 6 && state.default_dacl.empty()) {
        // Learn (TOKEN_DEFAULT_DACL): no default DACL -> DefaultDacl is NULL
        // and ReturnLength is sizeof(TOKEN_DEFAULT_DACL).
        return_length = is64 ? 8u : 4u;
        data_out.assign(return_length, 0);
        return true;
    }
    if (info_class == 6) { // TokenDefaultDacl
        const u32 acl_offset = is64 ? 8u : 4u;
        return_length =
            acl_offset + static_cast<u32>(state.default_dacl.size());
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data(), &acl_offset, 4);
        std::memcpy(data_out.data() + acl_offset,
                    state.default_dacl.data(), state.default_dacl.size());
        return true;
    }
    if (info_class == 18) { // TokenElevationType
        return_length = 4;
        data_out.resize(4);
        // TokenElevationTypeDefault (1) when unelevated — no UAC split token,
        // which is the normal case. TokenElevationTypeFull (2) only when the
        // user asked for elevation. Reporting Full unconditionally is read
        // as an Administrator account and installers abort. Must agree with
        // IsUserAnAdmin.
        u32 elev_type = is_elevated() ? 2u : 1u;
        std::memcpy(data_out.data(), &elev_type, 4);
        return true;
    }
    if (info_class == 8) { // TokenType
        return_length = 4;
        data_out.resize(4);
        std::memcpy(data_out.data(), &state.type, 4);
        return true;
    }
    if (info_class == 9) { // TokenImpersonationLevel
        if (state.type != 2) {
            return_length = 0;
            return false;
        }
        return_length = 4;
        data_out.resize(4);
        std::memcpy(data_out.data(), &state.impersonation_level, 4);
        return true;
    }
    if (info_class == 10) { // TokenStatistics
        // TOKEN_STATISTICS contains only fixed-width fields and therefore has
        // the same 56-byte layout for x86 and x64 guests.
        constexpr u32 TOKEN_STATISTICS_SIZE = 56;
        // Must agree with TokenGroups (advapi_token_probe
        // stat_groupcount_matches).
        std::vector<u8> groups_blob;
        u32 groups_len = 0;
        (void)get_token_information(token, 2, groups_blob, groups_len, is64);
        u32 SYNTHETIC_GROUP_COUNT = 0;
        if (groups_blob.size() >= 4) {
            std::memcpy(&SYNTHETIC_GROUP_COUNT, groups_blob.data(), 4);
        }
        const i64 expiration_time = std::numeric_limits<i64>::max();
        const u32 dynamic_charged =
            static_cast<u32>(state.default_dacl.size());
        const u32 dynamic_available = 0;
        const u32 privilege_count =
            static_cast<u32>(state.privileges.size());

        return_length = TOKEN_STATISTICS_SIZE;
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data() + 0, &state.token_id, sizeof(u64));
        std::memcpy(data_out.data() + 8, &state.authentication_id,
                    sizeof(u64));
        std::memcpy(data_out.data() + 16, &expiration_time, sizeof(i64));
        std::memcpy(data_out.data() + 24, &state.type, sizeof(u32));
        std::memcpy(data_out.data() + 28, &state.impersonation_level,
                    sizeof(u32));
        std::memcpy(data_out.data() + 32, &dynamic_charged, sizeof(u32));
        std::memcpy(data_out.data() + 36, &dynamic_available, sizeof(u32));
        std::memcpy(data_out.data() + 40, &SYNTHETIC_GROUP_COUNT,
                    sizeof(u32));
        std::memcpy(data_out.data() + 44, &privilege_count, sizeof(u32));
        std::memcpy(data_out.data() + 48, &state.modified_id, sizeof(u64));
        return true;
    }
    if (info_class == 11) { // TokenRestrictedSids
        const u32 count =
            static_cast<u32>(state.restricting_sids.size());
        const u32 entries_offset = is64 ? 8u : 4u;
        const u32 sid_data_offset = entries_offset + count * saa_size;
        u32 sid_bytes = 0;
        for (const auto& sid : state.restricting_sids) {
            sid_bytes += static_cast<u32>(sid.bytes.size());
        }
        return_length = sid_data_offset + sid_bytes;
        data_out.assign(return_length, 0);
        std::memcpy(data_out.data(), &count, 4);
        u32 next_sid = sid_data_offset;
        for (u32 index = 0; index < count; ++index) {
            const auto& sid = state.restricting_sids[index];
            const u32 entry = entries_offset + index * saa_size;
            std::memcpy(data_out.data() + entry, &next_sid, 4);
            std::memcpy(data_out.data() + entry + (is64 ? 8u : 4u),
                        &sid.attributes, 4);
            std::memcpy(data_out.data() + next_sid, sid.bytes.data(),
                        sid.bytes.size());
            next_sid += static_cast<u32>(sid.bytes.size());
        }
        return true;
    }
    if (info_class == 15) { // TokenSandBoxInert
        return_length = 4;
        data_out.resize(4);
        const u32 inert = state.sandbox_inert ? 1u : 0u;
        std::memcpy(data_out.data(), &inert, 4);
        return true;
    }
    if (info_class == 21) {
        // TokenHasRestrictions: Windows reports ReturnLength 1
        // (advapi_token_probe class 0x15, both bitnesses).
        return_length = 1;
        data_out.assign(1, state.has_restrictions ? 1 : 0);
        return true;
    }
    if (info_class == 20) { // TokenElevation
        return_length = 4;
        data_out.resize(4);
        u32 elevated = is_elevated() ? 1u : 0u;
        std::memcpy(data_out.data(), &elevated, 4);
        return true;
    }
    if (info_class == 23) { // TokenVirtualizationAllowed
        // DWORD nonzero if UAC file/registry virtualization is allowed.
        // Elevated tokens and modern non-legacy processes report 0.
        // https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-token_information_class
        return_length = 4;
        data_out.assign(4, 0);
        return true;
    }
    if (info_class == 24) { // TokenVirtualizationEnabled
        // DWORD, nonzero when virtualization is currently enabled for the
        // token. ERROR_NOT_SUPPORTED leaves the caller without a DWORD to
        // branch on, so this class returns 0.
        return_length = 4;
        data_out.assign(4, 0);
        return true;
    }
    if (info_class == 25) { // TokenIntegrityLevel
        // TOKEN_MANDATORY_LABEL { SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; } }
        // SID: S-1-16-8192 (medium) or S-1-16-12288 (high).
        u8 integrity_sid[] = {
            0x01,                                       // Revision = 1
            0x01,                                       // SubAuthorityCount = 1
            0x00, 0x00, 0x00, 0x00, 0x00, 0x10,         // SECURITY_MANDATORY_LABEL_AUTHORITY
            0x00, 0x20, 0x00, 0x00,                     // SubAuthority[0] = 8192 (Medium)
        };
        // The token's own elevation, not the process's: a linked (filtered)
        // or restricted duplicate carries its own.
        const u32 integrity_rid = state.integrity_rid.value_or(
            state.elevated ? 0x3000u : 0x2000u);
        std::memcpy(integrity_sid + 8, &integrity_rid,
                    sizeof(integrity_rid));
        const u32 sid_offset = saa_size; // SID starts after SID_AND_ATTRIBUTES
        u32 total = sid_offset + static_cast<u32>(sizeof(integrity_sid));
        return_length = total;
        data_out.assign(total, 0);
        u32 rel_ptr = sid_offset;  // relative offset, fixed up by thunk
        std::memcpy(data_out.data() + 0, &rel_ptr, 4);
        // SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED
        // (advapi_token_probe integrity_attrs: 0x60).
        u32 attrs = 0x60;
        std::memcpy(data_out.data() + (is64 ? 8 : 4), &attrs, 4);
        std::memcpy(
            data_out.data() + sid_offset,
            integrity_sid,
            sizeof(integrity_sid));
        return true;
    }
    if (info_class == 28) { // TokenLogonSid
        constexpr u32 SE_GROUP_MANDATORY = 0x00000001;
        constexpr u32 SE_GROUP_ENABLED_BY_DEFAULT = 0x00000002;
        constexpr u32 SE_GROUP_ENABLED = 0x00000004;
        constexpr u32 SE_GROUP_LOGON_ID = 0xC0000000;
        constexpr u32 attributes = SE_GROUP_MANDATORY |
                                   SE_GROUP_ENABLED_BY_DEFAULT |
                                   SE_GROUP_ENABLED |
                                   SE_GROUP_LOGON_ID;
        // TOKEN_GROUPS containing the synthetic logon-session SID
        // S-1-5-5-0-1. Callers use this SID to revoke the logon session's
        // explicit access from a lockdown token's default DACL.
        const std::array<u8, 20> logon_sid{
            1, 3, 0, 0, 0, 0, 0, 5,
            5, 0, 0, 0,
            0, 0, 0, 0,
            1, 0, 0, 0,
        };
        const u32 entries_offset = is64 ? 8u : 4u;
        const u32 sid_offset = entries_offset + saa_size;
        return_length = sid_offset + static_cast<u32>(logon_sid.size());
        data_out.assign(return_length, 0);
        const u32 count = 1;
        std::memcpy(data_out.data(), &count, sizeof(count));
        std::memcpy(data_out.data() + entries_offset, &sid_offset,
                    sizeof(sid_offset));
        std::memcpy(data_out.data() + entries_offset +
                        (is64 ? 8u : 4u),
                    &attributes, sizeof(attributes));
        std::memcpy(data_out.data() + sid_offset, logon_sid.data(),
                    logon_sid.size());
        return true;
    }
    if (info_class == 39) { // TokenSecurityAttributes
        // TOKEN_SECURITY_ATTRIBUTES_INFORMATION (V1). Pointer fields are
        // stored as buffer-relative offsets; the GetTokenInformation thunk
        // rewrites them to absolute guest addresses after the write.
        // https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-token_information_class
        constexpr u16 kVersionV1 = 1;
        constexpr u16 kTypeString = 3;
        const u32 pointer_size = is64 ? 8u : 4u;
        // Version(2)+Reserved(2)+AttributeCount(4)+pAttributeV1(pointer).
        const u32 header_size = 8u + pointer_size;
        // UNICODE_STRING: Length/MaxLength + (pad on x64) + Buffer.
        const u32 unicode_string_size = is64 ? 16u : 8u;
        const u32 buffer_field = is64 ? 8u : 4u;
        // TOKEN_SECURITY_ATTRIBUTE_V1: Name + ValueType/Reserved + Flags +
        // ValueCount + Values pointer (Values aligned to pointer_size).
        const u32 attr_size =
            unicode_string_size + 2u + 2u + 4u + 4u +
            (pointer_size == 8u ? 4u : 0u) + pointer_size;
        const u32 count =
            static_cast<u32>(state.security_attributes.size());

        u32 string_bytes = 0;
        for (const auto& attr : state.security_attributes) {
            string_bytes +=
                static_cast<u32>(attr.name.size() * sizeof(u16));
            string_bytes +=
                static_cast<u32>(attr.value.size() * sizeof(u16));
        }
        // One UNICODE_STRING for the value (string type) per attribute.
        const u32 value_ustr_bytes = count * unicode_string_size;
        const u32 attrs_offset = header_size;
        const u32 value_ustr_offset =
            attrs_offset + count * attr_size;
        const u32 string_offset = value_ustr_offset + value_ustr_bytes;
        return_length = string_offset + string_bytes;
        data_out.assign(return_length, 0);

        std::memcpy(data_out.data(), &kVersionV1, sizeof(kVersionV1));
        std::memcpy(data_out.data() + 4, &count, sizeof(count));
        if (count != 0) {
            std::memcpy(data_out.data() + 8, &attrs_offset,
                        sizeof(u32));
        }

        u32 next_string = string_offset;
        for (u32 index = 0; index < count; ++index) {
            const auto& attr = state.security_attributes[index];
            const u32 attr_base = attrs_offset + index * attr_size;
            const u16 name_bytes =
                static_cast<u16>(attr.name.size() * sizeof(u16));
            const u16 value_bytes =
                static_cast<u16>(attr.value.size() * sizeof(u16));

            std::memcpy(data_out.data() + attr_base, &name_bytes, 2);
            std::memcpy(data_out.data() + attr_base + 2, &name_bytes, 2);
            std::memcpy(data_out.data() + attr_base + buffer_field,
                        &next_string, sizeof(u32));
            for (usize i = 0; i < attr.name.size(); ++i) {
                const u16 ch = static_cast<u16>(attr.name[i]);
                std::memcpy(data_out.data() + next_string + i * sizeof(u16),
                            &ch, sizeof(ch));
            }
            next_string += name_bytes;

            const u32 value_type_off = attr_base + unicode_string_size;
            std::memcpy(data_out.data() + value_type_off, &kTypeString,
                        sizeof(kTypeString));
            std::memcpy(data_out.data() + value_type_off + 4, &attr.flags,
                        sizeof(attr.flags));
            const u32 value_count = 1;
            std::memcpy(data_out.data() + value_type_off + 8,
                        &value_count, sizeof(value_count));
            const u32 values_ptr_off =
                value_type_off + 12 + (pointer_size == 8u ? 4u : 0u);
            const u32 value_ustr = value_ustr_offset +
                                   index * unicode_string_size;
            std::memcpy(data_out.data() + values_ptr_off, &value_ustr,
                        sizeof(u32));

            std::memcpy(data_out.data() + value_ustr, &value_bytes, 2);
            std::memcpy(data_out.data() + value_ustr + 2, &value_bytes, 2);
            std::memcpy(data_out.data() + value_ustr + buffer_field,
                        &next_string, sizeof(u32));
            for (usize i = 0; i < attr.value.size(); ++i) {
                const u16 ch = static_cast<u16>(attr.value[i]);
                std::memcpy(data_out.data() + next_string + i * sizeof(u16),
                            &ch, sizeof(ch));
            }
            next_string += value_bytes;
        }
        return true;
    }
    if (info_class == 40 && !is64) {
        // WOW64 refuses TokenIsRestricted (advapi_token_probe.x86.expected:
        // class 0x28 -> ERROR_INVALID_PARAMETER).
        return_length = 0;
        return false;
    }
    if (info_class == 40) { // TokenIsRestricted
        // NtDoc: ULONG nonzero when TOKEN_IS_RESTRICTED /
        // TOKEN_WRITE_RESTRICTED. Matches IsTokenRestricted (restricting SIDs).
        return_length = 4;
        data_out.resize(4);
        const u32 restricted =
            state.restricting_sids.empty() ? 0u : 1u;
        std::memcpy(data_out.data(), &restricted, 4);
        return true;
    }
    // ---------------------------------------------------------------------
    // Remaining documented classes (winnt.h TOKEN_INFORMATION_CLASS). Sizes
    // and values measured on Windows 10 by tools/oracle_probe/
    // advapi_token_probe.c (x64 .expected and WOW64 .x86.expected).
    // ---------------------------------------------------------------------
    auto dword_class = [&](u32 value) {
        return_length = 4;
        data_out.assign(4, 0);
        std::memcpy(data_out.data(), &value, 4);
        return true;
    };
    const u32 ptr = is64 ? 8u : 4u;
    if (info_class == 7) { // TokenSource: CHAR SourceName[8]; LUID
        return_length = 16;
        data_out.assign(16, 0);
        static constexpr char kSource[8] = {'U', 's', 'e', 'r', '3', '2', ' ', ' '};
        std::memcpy(data_out.data(), kSource, 8);
        std::memcpy(data_out.data() + 8, &state.authentication_id, 8);
        return true;
    }
    if (info_class == 12) {
        // TokenSessionId: the same session ProcessIdToSessionId reports
        // (kernel32 process_apis.cpp): the console session, 1.
        return dword_class(1);
    }
    if (info_class == 13) { // TokenGroupsAndPrivileges
        // { DWORD SidCount, SidLength; PSID_AND_ATTRIBUTES Sids;
        //   DWORD RestrictedSidCount, RestrictedSidLength;
        //   PSID_AND_ATTRIBUTES RestrictedSids; DWORD PrivilegeCount,
        //   PrivilegeLength; PLUID_AND_ATTRIBUTES Privileges;
        //   LUID AuthenticationId; }  56 bytes x64, 44 x86.
        // Sids holds the user SID followed by the groups (probe gap);
        // pointers are buffer-relative here, rebased by the thunk.
        std::vector<u8> user_blob, groups_blob;
        u32 len = 0;
        (void)get_token_information(token, 1, user_blob, len, is64);
        (void)get_token_information(token, 2, groups_blob, len, is64);
        struct Entry { std::vector<u8> sid; u32 attrs; };
        std::vector<Entry> sids;
        auto take = [&](const std::vector<u8>& blob, u32 entry_off) {
            u32 rel = 0, attrs = 0;
            std::memcpy(&rel, blob.data() + entry_off, 4);
            std::memcpy(&attrs, blob.data() + entry_off + ptr, 4);
            const u8 count = blob[rel + 1];
            const usize size = 8u + count * 4u;
            sids.push_back({std::vector<u8>(blob.begin() + rel,
                                            blob.begin() + rel + size), attrs});
        };
        take(user_blob, 0);
        u32 group_count = 0;
        std::memcpy(&group_count, groups_blob.data(), 4);
        for (u32 i = 0; i < group_count; ++i) take(groups_blob, ptr + i * saa_size);
        std::vector<Entry> restricted;
        for (const auto& r : state.restricting_sids) restricted.push_back({r.bytes, r.attributes});

        const u32 header = is64 ? 56u : 44u;
        const u32 sid_array = header;
        u32 cursor = sid_array + static_cast<u32>(sids.size()) * saa_size;
        const u32 restricted_array = cursor;
        cursor += static_cast<u32>(restricted.size()) * saa_size;
        const u32 priv_array = cursor;
        const u32 priv_count = static_cast<u32>(state.privileges.size());
        cursor += priv_count * 12u;
        u32 sid_bytes = 0, rsid_bytes = 0;
        for (const auto& e : sids) sid_bytes += static_cast<u32>(e.sid.size());
        for (const auto& e : restricted) rsid_bytes += static_cast<u32>(e.sid.size());
        return_length = cursor + sid_bytes + rsid_bytes;
        data_out.assign(return_length, 0);
        auto put32 = [&](u32 off, u32 v) { std::memcpy(data_out.data() + off, &v, 4); };
        const u32 sid_count = static_cast<u32>(sids.size());
        const u32 rsid_count = static_cast<u32>(restricted.size());
        put32(0, sid_count);
        put32(4, sid_count * saa_size + sid_bytes);
        put32(8, sid_array);
        put32(8 + ptr, rsid_count);
        put32(12 + ptr, rsid_count * saa_size + rsid_bytes);
        if (rsid_count != 0) put32(16 + ptr, restricted_array);
        put32(16 + 2 * ptr, priv_count);
        put32(20 + 2 * ptr, priv_count * 12u);
        if (priv_count != 0) put32(24 + 2 * ptr, priv_array);
        std::memcpy(data_out.data() + 24 + 3 * ptr, &state.authentication_id, 8);
        u32 data_cursor = cursor;
        auto emit = [&](const std::vector<Entry>& list, u32 array) {
            for (u32 i = 0; i < list.size(); ++i) {
                put32(array + i * saa_size, data_cursor);
                put32(array + i * saa_size + ptr, list[i].attrs);
                std::memcpy(data_out.data() + data_cursor, list[i].sid.data(),
                            list[i].sid.size());
                data_cursor += static_cast<u32>(list[i].sid.size());
            }
        };
        emit(sids, sid_array);
        emit(restricted, restricted_array);
        for (u32 i = 0; i < priv_count; ++i) {
            std::memcpy(data_out.data() + priv_array + i * 12u,
                        &state.privileges[i].luid, 8);
            put32(priv_array + i * 12u + 8, state.privileges[i].attributes);
        }
        return true;
    }
    if (info_class == 16) { // TokenAuditPolicy: TOKEN_AUDIT_POLICY, 31 bytes
        return_length = 31;
        data_out.assign(31, 0);
        return true;
    }
    if (info_class == 17) { // TokenOrigin: LUID OriginatingLogonSession
        return_length = 8;
        data_out.assign(8, 0);
        const u64 system_logon = 0x3E7; // SYSTEM_LUID created the session
        std::memcpy(data_out.data(), &system_logon, 8);
        return true;
    }
    if (info_class == 19) { // TokenLinkedToken: TOKEN_LINKED_TOKEN { HANDLE }
        // Only the size is produced here; the thunk opens the handle or
        // reports ERROR_NO_SUCH_LOGON_SESSION for an unsplit token.
        return_length = ptr;
        data_out.assign(ptr, 0);
        return true;
    }
    if (info_class == 22) { // TokenAccessInformation
        // TOKEN_ACCESS_INFORMATION embeds SID_AND_ATTRIBUTES_HASH tables
        // whose hash function is not documented; refuse loudly rather than
        // hand back fabricated hashes.
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED advapi32!GetTokenInformation: "
                "TokenAccessInformation (undocumented SID hash)\n");
        }
        return_length = 0;
        return false;
    }
    if (info_class == 26) return dword_class(0); // TokenUIAccess
    if (info_class == 27) {
        // TokenMandatoryPolicy: TOKEN_MANDATORY_POLICY_NO_WRITE_UP |
        // TOKEN_MANDATORY_POLICY_NEW_PROCESS_MIN (probe val=3).
        return dword_class(3);
    }
    if (info_class == 29) return dword_class(0); // TokenIsAppContainer
    if (info_class == 30 || info_class == 37) {
        // TokenCapabilities / TokenDeviceGroups: empty TOKEN_GROUPS. Windows
        // sizes the empty device-group list with one SID_AND_ATTRIBUTES slot
        // on x64 (probe: class 0x1e len 8/4, class 0x25 len 0x18/4).
        return_length = (info_class == 37 && is64) ? 8u + saa_size : ptr;
        data_out.assign(return_length, 0);
        return true;
    }
    if (info_class == 31 || info_class == 41) {
        // TokenAppContainerSid (TOKEN_APPCONTAINER_INFORMATION) and
        // TokenProcessTrustLevel (TOKEN_PROCESS_TRUST_LEVEL): a NULL PSID.
        return_length = ptr;
        data_out.assign(ptr, 0);
        return true;
    }
    if (info_class == 32) return dword_class(0); // TokenAppContainerNumber
    if (info_class == 33 || info_class == 34 || info_class == 43) {
        // User/Device claim attributes and singleton attributes: an empty
        // CLAIM_SECURITY_ATTRIBUTES_INFORMATION (Version 1, count 0). WOW64
        // returns the 16-byte native form too (probe .x86.expected).
        return_length = 16;
        data_out.assign(16, 0);
        if (info_class != 43) data_out[0] = 1; // CLAIM_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1
        return true;
    }
    if (info_class == 42) return dword_class(0); // TokenPrivateNameSpace
    if (info_class == 44) {
        // TokenBnoIsolation: { PWSTR IsolationPrefix; BOOLEAN IsolationEnabled; }
        return_length = is64 ? 16u : 8u;
        data_out.assign(return_length, 0);
        return true;
    }
    if (info_class == 47 || info_class == 48) return dword_class(0); // IsSandboxed / IsAppSilo
    ipc::ipc_log(
        "GetTokenInformation h=0x%x class=%u -> ERROR_NOT_SUPPORTED",
        static_cast<unsigned>(token), info_class);
    VITREO_TRACE("[advapi32] GetTokenInformation: unhandled class %u\n", info_class);
    return_length = 0;
    return false;
}

object_access::Token SecurityStubs::effective_object_token() {
    constexpr Handle effective=0xfffffffau;
    object_access::Token result;
    const auto state=token_state(effective);
    result.user=synthetic_sid();
    for(const auto& disabled:state.disabled_sids)
        if(disabled.bytes==result.user)result.user_deny_only=true;
    result.default_dacl=state.default_dacl;
    result.integrity_rid=state.integrity_rid.value_or(state.elevated?0x3000u:0x2000u);
    for(const auto& sid:state.restricting_sids)
        result.restricting.push_back({sid.bytes,sync_access::SE_GROUP_ENABLED});
    for(const auto& privilege:state.privileges) {
        if(privilege.luid==8)result.security_privilege_held=true;
        if(privilege.luid==8 && (privilege.attributes&2))result.security_privilege=true;
        if(privilege.luid==9 && (privilege.attributes&2))result.take_ownership_privilege=true;
    }
    std::vector<u8> groups;u32 length=0;
    if(get_token_information(effective,2,groups,length,false)) {
        const u32 count=object_access::word(groups,0);
        for(u32 i=0;i<count;++i) {
            const usize slot=4+8u*i;
            const u32 offset=object_access::word(groups,slot);
            if(offset+8>groups.size())continue;
            const usize size=8+4u*groups[offset+1];
            if(offset+size>groups.size())continue;
            result.groups.push_back({{groups.begin()+offset,groups.begin()+offset+size},
                object_access::word(groups,slot+4)});
        }
    }
    return result;
}

} // namespace vitreo::winapi
