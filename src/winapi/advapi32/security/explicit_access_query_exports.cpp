// SPDX-License-Identifier: LGPL-2.1-only

/// @file explicit_access_query_exports.cpp
/// @brief advapi32 explicit-access queries: GetExplicitEntriesFromAclA/W and
/// GetEffectiveRightsFromAclW.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "ipc/ipc_gate.hpp"
#include "winapi/advapi32/security/named_security.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <utility>
#include <vector>

namespace vitreo::winapi {

/// Registers GetExplicitEntriesFromAclA/W, GetEffectiveRightsFromAclW.
void add_explicit_access_query_exports(DllExports& exports,
                                       SecurityStubs& stubs) {
    // DWORD GetExplicitEntriesFromAclW(PACL pacl, PULONG pcCountOfExplicitEntries,
    //                                  PEXPLICIT_ACCESS_W *pListOfExplicitEntries)
    // 3 args, stdcall_bytes=12. Inverse of SetEntriesInAclW for standard
    // ACCESS_ALLOWED / ACCESS_DENIED ACEs. The result buffer is LocalAlloc'd
    // (via alloc_local_memory) so the guest can LocalFree it.
    // EXPLICIT_ACCESS_A and _W have one layout for TRUSTEE_IS_SID entries (the
    // name slot holds the PSID), so both exports share this handler
    // (advapi_sdapi_probe geea_w / geea_a print identical entries).
    auto get_explicit_entries = [&stubs](Marshaler& m) {
            const GuestAddr acl = m.arg_ptr(0);
            const GuestAddr count_out = m.arg_ptr(1);
            const GuestAddr list_out = m.arg_ptr(2);
            constexpr u32 ERROR_INVALID_ACL = 1336;
            constexpr u32 GRANT_ACCESS = 1;
            constexpr u32 DENY_ACCESS = 3;
            constexpr u32 SET_AUDIT_SUCCESS = 5;
            constexpr u32 SET_AUDIT_FAILURE = 6;
            constexpr u8 ACCESS_ALLOWED_ACE_TYPE = 0;
            constexpr u8 ACCESS_DENIED_ACE_TYPE = 1;
            constexpr u8 SYSTEM_AUDIT_ACE_TYPE = 2;
            constexpr u8 SUCCESSFUL_ACCESS_ACE_FLAG = 0x40;
            constexpr u8 FAILED_ACCESS_ACE_FLAG = 0x80;
            constexpr u32 TRUSTEE_IS_SID = 0;
            constexpr u32 TRUSTEE_IS_UNKNOWN = 0;
            constexpr u32 NO_MULTIPLE_TRUSTEE = 0;

            if (count_out == 0 || list_out == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            (void)m.write_u32(count_out, 0);
            (void)m.write_ptr(list_out, 0);

            // A NULL ACL has no explicit entries (probe geea_a_null).
            if (acl == 0) {
                m.set_return(error_codes::ERROR_SUCCESS);
                return;
            }

            auto revision = m.memory().read_u8(acl);
            auto acl_size = m.memory().read_u16(acl + 2);
            auto ace_count = m.memory().read_u16(acl + 4);
            if (revision.is_err() || acl_size.is_err() || ace_count.is_err() ||
                acl_size.value() < 8) {
                ipc::ipc_log("GetExplicitEntriesFromAclW -> ERROR_INVALID_ACL (header)");
                m.set_return(ERROR_INVALID_ACL);
                return;
            }

            struct EntrySpec {
                u32 access_mask = 0;
                u32 access_mode = 0;
                u32 inheritance = 0;
                std::vector<u8> sid;
            };
            std::vector<EntrySpec> entries;
            entries.reserve(ace_count.value());

            u32 offset = 8;
            for (u16 index = 0; index < ace_count.value(); ++index) {
                if (offset + 4 > acl_size.value()) {
                    ipc::ipc_log(
                        "GetExplicitEntriesFromAclW -> ERROR_INVALID_ACL (ace=%u eof)",
                        index);
                    m.set_return(ERROR_INVALID_ACL);
                    return;
                }
                auto ace_type = m.memory().read_u8(acl + offset);
                auto ace_flags = m.memory().read_u8(acl + offset + 1);
                auto ace_size = m.memory().read_u16(acl + offset + 2);
                if (ace_type.is_err() || ace_flags.is_err() || ace_size.is_err() ||
                    ace_size.value() < 8 ||
                    offset + ace_size.value() > acl_size.value()) {
                    ipc::ipc_log(
                        "GetExplicitEntriesFromAclW -> ERROR_INVALID_ACL (ace=%u)",
                        index);
                    m.set_return(ERROR_INVALID_ACL);
                    return;
                }

                const u8 type = ace_type.value();
                const u8 flags = ace_flags.value();
                // Only explicit ACEs are returned; INHERITED_ACE ones are
                // skipped (probe geea_w: 2 of 3 ACEs).
                if ((flags & 0x10u) != 0) {
                    offset += ace_size.value();
                    continue;
                }
                // Standard ACE body: Mask + SID. Object ACEs are not converted.
                if (type == ACCESS_ALLOWED_ACE_TYPE ||
                    type == ACCESS_DENIED_ACE_TYPE ||
                    type == SYSTEM_AUDIT_ACE_TYPE) {
                    auto mask = m.memory().read_u32(acl + offset + 4);
                    if (mask.is_err()) {
                        m.set_return(ERROR_INVALID_ACL);
                        return;
                    }
                    const GuestAddr sid_addr = acl + offset + 8;
                    if (SecurityStubs::is_valid_sid(m, sid_addr) == 0) {
                        ipc::ipc_log(
                            "GetExplicitEntriesFromAclW -> ERROR_INVALID_ACL "
                            "(ace=%u bad sid)",
                            index);
                        m.set_return(ERROR_INVALID_ACL);
                        return;
                    }
                    const u32 sid_size = SecurityStubs::get_length_sid(m, sid_addr);
                    if (8 + sid_size > ace_size.value()) {
                        m.set_return(ERROR_INVALID_ACL);
                        return;
                    }
                    EntrySpec spec;
                    spec.access_mask = mask.value();
                    // AceFlags hold inheritance bits; audit success/failure flags
                    // are folded into access_mode for SYSTEM_AUDIT_ACE.
                    spec.inheritance = flags & 0x1Fu;
                    if (type == ACCESS_ALLOWED_ACE_TYPE) {
                        spec.access_mode = GRANT_ACCESS;
                    } else if (type == ACCESS_DENIED_ACE_TYPE) {
                        spec.access_mode = DENY_ACCESS;
                    } else {
                        u32 mode = 0;
                        if ((flags & SUCCESSFUL_ACCESS_ACE_FLAG) != 0)
                            mode |= SET_AUDIT_SUCCESS;
                        if ((flags & FAILED_ACCESS_ACE_FLAG) != 0)
                            mode |= SET_AUDIT_FAILURE;
                        if (mode == 0) mode = SET_AUDIT_SUCCESS;
                        spec.access_mode = mode;
                        spec.inheritance = flags & 0x1Fu;
                    }
                    spec.sid.resize(sid_size);
                    if (m.memory().read_bytes(sid_addr, spec.sid.data(),
                                              sid_size).is_err()) {
                        m.set_return(ERROR_INVALID_ACL);
                        return;
                    }
                    entries.push_back(std::move(spec));
                }
                offset += ace_size.value();
            }

            if (entries.empty()) {
                ipc::ipc_log(
                    "GetExplicitEntriesFromAclW ace_count=%u -> ERROR_SUCCESS "
                    "explicit=0",
                    ace_count.value());
                m.set_return(error_codes::ERROR_SUCCESS);
                return;
            }

            const bool is64 = m.cpu_is_64bit();
            const u32 entry_size = is64 ? 48u : 32u;
            const u32 trustee_name_offset = is64 ? 40u : 28u;
            u32 sid_bytes_total = 0;
            for (const auto& e : entries)
                sid_bytes_total += static_cast<u32>(e.sid.size());
            const u32 total =
                entry_size * static_cast<u32>(entries.size()) + sid_bytes_total;
            const GuestAddr block = stubs.alloc_local_memory(total);
            if (block == 0) {
                m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return;
            }

            GuestAddr sid_cursor =
                block + entry_size * static_cast<u32>(entries.size());
            for (usize i = 0; i < entries.size(); ++i) {
                const GuestAddr ea = block + static_cast<u32>(i) * entry_size;
                const auto& e = entries[i];
                (void)m.write_u32(ea + 0, e.access_mask);
                (void)m.write_u32(ea + 4, e.access_mode);
                (void)m.write_u32(ea + 8, e.inheritance);
                if (is64) {
                    (void)m.write_u64(ea + 16, 0);  // pMultipleTrustee
                    (void)m.write_u32(ea + 24, NO_MULTIPLE_TRUSTEE);
                    (void)m.write_u32(ea + 28, TRUSTEE_IS_SID);
                    (void)m.write_u32(ea + 32, TRUSTEE_IS_UNKNOWN);
                    (void)m.write_u64(ea + trustee_name_offset,
                                      static_cast<u64>(sid_cursor));
                } else {
                    (void)m.write_u32(ea + 12, 0);
                    (void)m.write_u32(ea + 16, NO_MULTIPLE_TRUSTEE);
                    (void)m.write_u32(ea + 20, TRUSTEE_IS_SID);
                    (void)m.write_u32(ea + 24, TRUSTEE_IS_UNKNOWN);
                    (void)m.write_u32(ea + trustee_name_offset,
                                      static_cast<u32>(sid_cursor));
                }
                (void)m.memory().write_bytes(sid_cursor, e.sid.data(), e.sid.size());
                sid_cursor += e.sid.size();
            }

            (void)m.write_u32(count_out, static_cast<u32>(entries.size()));
            (void)m.write_ptr(list_out, block);
            ipc::ipc_log(
                "GetExplicitEntriesFromAclW ace_count=%u -> ERROR_SUCCESS "
                "explicit=%zu",
                ace_count.value(), entries.size());
            VITREO_TRACE("[advapi32] GetExplicitEntriesFromAcl(aces=%u)"
                         " -> ERROR_SUCCESS explicit=%zu\n",
                         ace_count.value(), entries.size());
            m.set_return(error_codes::ERROR_SUCCESS);
    };
    exports.entries.push_back({"GetExplicitEntriesFromAclA", 0, get_explicit_entries, 12});
    exports.entries.push_back({"GetExplicitEntriesFromAclW", 0, get_explicit_entries, 12});

    // DWORD GetEffectiveRightsFromAclW(PACL pacl, PTRUSTEE_W pTrustee,
    //                                  PACCESS_MASK pAccessMask)
    // 3 args, stdcall_bytes=12. Computes the rights granted to a trustee by
    // applying ACCESS_ALLOWED / ACCESS_DENIED ACEs (SID trustees only).
    exports.entries.push_back({"GetEffectiveRightsFromAclW", 0,
        [&stubs](Marshaler& m) {
            const GuestAddr acl = m.arg_ptr(0);
            const GuestAddr trustee = m.arg_ptr(1);
            const GuestAddr mask_out = m.arg_ptr(2);
            constexpr u32 ERROR_INVALID_ACL = 1336;
            constexpr u32 TRUSTEE_IS_SID = 0;
            constexpr u32 TRUSTEE_IS_NAME = 1;

            if (trustee == 0 || mask_out == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            const bool is64 = m.cpu_is_64bit();
            const u32 form_offset = is64 ? 12u : 8u;
            const u32 name_offset = is64 ? 24u : 16u;
            const u32 form = m.memory().read_u32(trustee + form_offset).value_or(~0u);
            const GuestAddr trustee_ptr = static_cast<GuestAddr>(
                m.read_ptr(trustee + name_offset).value_or(0));
            // Learn: "You can use a name or a SID to identify a trustee."
            std::vector<u8> trustee_sid;
            if (form == TRUSTEE_IS_SID) {
                if (SecurityStubs::is_valid_sid(m, trustee_ptr) == 0) {
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                trustee_sid.resize(SecurityStubs::get_length_sid(m, trustee_ptr));
                (void)m.memory().read_bytes(trustee_ptr, trustee_sid.data(),
                                            trustee_sid.size());
            } else if (form == TRUSTEE_IS_NAME) {
                auto name = m.read_wide_string(trustee_ptr);
                if (trustee_ptr == 0 || name.is_err() ||
                    !sid_from_account_name(name.value(), trustee_sid)) {
                    m.set_return(error_codes::ERROR_NONE_MAPPED);
                    return;
                }
            } else {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }

            if (acl == 0) {
                // A NULL DACL grants everything (advapi_acl_probe
                // ger_null_acl: ERROR_SUCCESS, mask 0x03FFFFFF).
                (void)m.write_u32(mask_out, 0x03FFFFFFu);
                m.set_return(error_codes::ERROR_SUCCESS);
                return;
            }
            const u16 acl_size = m.memory().read_u16(acl + 2).value_or(0);
            std::vector<u8> acl_bytes(acl_size);
            u8 revision = 0;
            std::vector<std::vector<u8>> aces;
            if (acl_size < 8 ||
                m.memory().read_bytes(acl, acl_bytes.data(), acl_size).is_err() ||
                !named_sd::parse_acl(acl_bytes, revision, aces)) {
                m.set_return(ERROR_INVALID_ACL);
                return;
            }
            // Group membership comes from the account's SAM aliases, not the
            // token: BUILTIN groups count, INTERACTIVE / Authenticated Users
            // do not (advapi_acl_probe ger_groups / ger_admins_*).
            std::vector<std::vector<u8>> aliases;
            const named_sd::Principal who = stubs.principal();
            if (trustee_sid == who.user) aliases = who.alias_groups;
            const u32 effective =
                named_sd::effective_rights(aces, trustee_sid, aliases);
            (void)m.write_u32(mask_out, effective);
            m.set_return(error_codes::ERROR_SUCCESS);
        }, 12});
}

} // namespace vitreo::winapi
