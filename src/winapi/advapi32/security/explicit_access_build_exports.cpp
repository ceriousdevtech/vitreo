// SPDX-License-Identifier: LGPL-2.1-only

/// @file explicit_access_build_exports.cpp
/// @brief advapi32 explicit-access construction: BuildTrusteeWithSidW,
/// SetEntriesInAclA/W and BuildSecurityDescriptorA/W.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "ipc/ipc_gate.hpp"
#include "winapi/advapi32/security/sddl.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace vitreo::winapi {

/// Registers BuildTrusteeWithSidW, SetEntriesInAclA/W,
/// BuildSecurityDescriptorA/W.
void add_explicit_access_build_exports(DllExports& exports,
                                       SecurityStubs& stubs) {
    // VOID BuildTrusteeWithSidW(PTRUSTEE_W, PSID) — 2 args, 8 bytes
    exports.entries.push_back({"BuildTrusteeWithSidW", 0,
        [](Marshaler& m) {
            const GuestAddr trustee = m.arg_ptr(0);
            const GuestAddr sid = m.arg_ptr(1);
            if (trustee == 0) {
                return;
            }
            const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
            const u32 name_offset = m.cpu_is_64bit() ? 24u : 16u;
            (void)m.write_ptr(trustee, 0);
            (void)m.write_u32(trustee + pointer_size, 0); // NO_MULTIPLE_TRUSTEE
            (void)m.write_u32(trustee + pointer_size + 4, 0); // TRUSTEE_IS_SID
            (void)m.write_u32(trustee + pointer_size + 8, 0); // TRUSTEE_IS_UNKNOWN
            (void)m.write_ptr(trustee + name_offset, sid);
            m.set_return(0);
        }, 8});

    // DWORD SetEntriesInAclA/W(ULONG,PEXPLICIT_ACCESS_A/W,PACL,PACL*)
    // Learn: a trustee may be identified by SID or by name. 4 args, 16 bytes.
    // The SetEntriesInAcl merge, shared with BuildSecurityDescriptor: builds
    // the new ACL's bytes from `count` guest EXPLICIT_ACCESS entries and an
    // optional guest ACL. Returns ERROR_SUCCESS or the error to report.
    auto set_entries_core = [](Marshaler& m, bool wide, u32 count, GuestAddr entries,
                               GuestAddr old_acl, std::vector<u8>& acl_bytes) -> u32 {
            constexpr u32 ERROR_INVALID_ACL = 1336;
            constexpr u32 ERROR_INVALID_SID = 1337;
            constexpr u32 GRANT_ACCESS = 1;
            constexpr u32 SET_ACCESS = 2;
            constexpr u32 DENY_ACCESS = 3;
            constexpr u32 REVOKE_ACCESS = 4;
            constexpr u32 SET_AUDIT_SUCCESS = 5;
            constexpr u32 SET_AUDIT_FAILURE = 6;
            constexpr u32 SET_AUDIT_BOTH = SET_AUDIT_SUCCESS | SET_AUDIT_FAILURE;
            constexpr u32 TRUSTEE_IS_SID = 0;
            constexpr u32 TRUSTEE_IS_NAME = 1;

            auto is_supported_mode = [](u32 mode) {
                return mode == GRANT_ACCESS || mode == SET_ACCESS ||
                       mode == DENY_ACCESS || mode == REVOKE_ACCESS ||
                       mode == SET_AUDIT_SUCCESS || mode == SET_AUDIT_FAILURE ||
                       mode == SET_AUDIT_BOTH;
            };
            auto is_audit_mode = [](u32 mode) {
                return mode == SET_AUDIT_SUCCESS || mode == SET_AUDIT_FAILURE ||
                       mode == SET_AUDIT_BOTH;
            };

            if (count > 1024 || (count != 0 && entries == 0)) {
                ipc::ipc_log(
                    "SetEntriesInAclW count=%u -> ERROR_INVALID_PARAMETER",
                    count);
                return error_codes::ERROR_INVALID_PARAMETER;
            }

            struct AceSpec {
                u32 access_mask = 0;
                u32 access_mode = 0;
                u32 inheritance = 0;
                std::vector<u8> sid;
                bool object_ace = false;   // TRUSTEE_IS_OBJECTS_AND_*
                u32 objects_present = 0;   // ACE_OBJECT_TYPE_PRESENT | ...
                std::array<u8, 16> object_type{};
                std::array<u8, 16> inherited_object_type{};
            };
            constexpr u32 TRUSTEE_IS_OBJECTS_AND_SID = 3;
            constexpr u32 TRUSTEE_IS_OBJECTS_AND_NAME = 4;
            std::vector<AceSpec> specs;
            specs.reserve(count);
            const u32 entry_size = m.cpu_is_64bit() ? 48u : 32u;
            const u32 trustee_form_offset = m.cpu_is_64bit() ? 28u : 20u;
            const u32 trustee_name_offset = m.cpu_is_64bit() ? 40u : 28u;
            for (u32 index = 0; index < count; ++index) {
                const GuestAddr entry = entries + index * entry_size;
                AceSpec spec;
                spec.access_mask = m.memory().read_u32(entry).value_or(0);
                spec.access_mode = m.memory().read_u32(entry + 4).value_or(0);
                spec.inheritance = m.memory().read_u32(entry + 8).value_or(0);
                const u32 form =
                    m.memory().read_u32(entry + trustee_form_offset).value_or(0);
                const GuestAddr trustee = m.cpu_is_64bit()
                    ? m.memory().read_u64(entry + trustee_name_offset).value_or(0)
                    : m.memory().read_u32(entry + trustee_name_offset).value_or(0);
                if (!is_supported_mode(spec.access_mode)) {
                    ipc::ipc_log(
                        "SetEntriesInAclW count=%u entry=%u mode=%u "
                        "-> ERROR_INVALID_PARAMETER",
                        count, index, spec.access_mode);
                    return error_codes::ERROR_INVALID_PARAMETER;
                }
                if (form == TRUSTEE_IS_NAME) {
                    if (trustee == 0) {
                        ipc::ipc_log(
                            "SetEntriesInAclW count=%u entry=%u -> "
                            "ERROR_INVALID_PARAMETER (null name)",
                            count, index);
                        return error_codes::ERROR_INVALID_PARAMETER;
                    }
                    auto name = wide ? m.read_wide_string(trustee)
                                     : m.read_ansi_string(trustee);
                    if (name.is_err() ||
                        !sid_from_account_name(name.value(), spec.sid)) {
                        ipc::ipc_log(
                            "SetEntriesInAclW count=%u entry=%u -> "
                            "ERROR_NONE_MAPPED",
                            count, index);
                        return error_codes::ERROR_NONE_MAPPED;
                    }
                } else if (form == TRUSTEE_IS_SID) {
                    if (SecurityStubs::is_valid_sid(m, trustee) == 0) {
                        ipc::ipc_log(
                            "SetEntriesInAclW count=%u entry=%u mode=%u "
                            "-> ERROR_INVALID_SID",
                            count, index, spec.access_mode);
                        return ERROR_INVALID_SID;
                    }
                    const u32 sid_size =
                        SecurityStubs::get_length_sid(m, trustee);
                    spec.sid.resize(sid_size);
                    if (m.memory().read_bytes(
                            trustee, spec.sid.data(), sid_size).is_err()) {
                        ipc::ipc_log(
                            "SetEntriesInAclW count=%u entry=%u mode=%u "
                            "-> ERROR_INVALID_SID (unreadable)",
                            count, index, spec.access_mode);
                        return ERROR_INVALID_SID;
                    }
                } else if (form == TRUSTEE_IS_OBJECTS_AND_SID) {
                    // OBJECTS_AND_SID { DWORD ObjectsPresent; GUID ObjectTypeGuid;
                    // GUID InheritedObjectTypeGuid; SID* pSid; } (accctrl.h)
                    spec.object_ace = true;
                    spec.objects_present = m.read_u32(trustee).value_or(0) & 3u;
                    (void)m.memory().read_bytes(trustee + 4, spec.object_type.data(), 16);
                    (void)m.memory().read_bytes(trustee + 20,
                                                spec.inherited_object_type.data(), 16);
                    const GuestAddr psid = static_cast<GuestAddr>(
                        m.read_ptr(trustee + (m.cpu_is_64bit() ? 40u : 36u)).value_or(0));
                    if (SecurityStubs::is_valid_sid(m, psid) == 0) {
                        return ERROR_INVALID_SID;
                    }
                    spec.sid.resize(SecurityStubs::get_length_sid(m, psid));
                    (void)m.memory().read_bytes(psid, spec.sid.data(), spec.sid.size());
                } else if (form == TRUSTEE_IS_OBJECTS_AND_NAME) {
                    // OBJECTS_AND_NAME { DWORD ObjectsPresent; SE_OBJECT_TYPE;
                    // LPTSTR ObjectTypeName; LPTSTR InheritedObjectTypeName;
                    // LPTSTR ptstrName; }. Object-type NAMES are directory-
                    // schema class names; with no directory service only the
                    // trustee itself can be resolved.
                    spec.object_ace = true;
                    const u32 present = m.read_u32(trustee).value_or(0) & 3u;
                    const u32 ptr = m.cpu_is_64bit() ? 8u : 4u;
                    const GuestAddr name_ptr = static_cast<GuestAddr>(
                        m.read_ptr(trustee + 8 + 2 * ptr).value_or(0));
                    if (present != 0) {
                        std::fprintf(stderr,
                            "[vitreo] UNIMPLEMENTED advapi32!SetEntriesInAcl: "
                            "object-type names need a directory schema\n");
                        return error_codes::ERROR_NONE_MAPPED;
                    }
                    auto name = wide ? m.read_wide_string(name_ptr)
                                     : m.read_ansi_string(name_ptr);
                    if (name_ptr == 0 || name.is_err() ||
                        !sid_from_account_name(name.value(), spec.sid)) {
                        return error_codes::ERROR_NONE_MAPPED;
                    }
                } else {
                    ipc::ipc_log(
                        "SetEntriesInAclW count=%u entry=%u form=%u "
                        "-> ERROR_INVALID_PARAMETER",
                        count, index, form);
                    return error_codes::ERROR_INVALID_PARAMETER;
                }
                specs.push_back(std::move(spec));
            }

            std::vector<std::vector<u8>> old_aces;
            u16 old_ace_count = 0;
            u8 acl_revision = 2;
            if (old_acl != 0) {
                auto revision = m.memory().read_u8(old_acl);
                auto acl_size = m.memory().read_u16(old_acl + 2);
                auto ace_count = m.memory().read_u16(old_acl + 4);
                if (revision.is_err() || acl_size.is_err() || ace_count.is_err() ||
                    acl_size.value() < 8) {
                    ipc::ipc_log(
                        "SetEntriesInAclW count=%u -> ERROR_INVALID_ACL "
                        "(header)",
                        count);
                    return ERROR_INVALID_ACL;
                }
                acl_revision = revision.value();
                old_ace_count = ace_count.value();
                u32 offset = 8;
                for (u16 index = 0; index < old_ace_count; ++index) {
                    auto ace_size = m.memory().read_u16(old_acl + offset + 2);
                    if (ace_size.is_err() || ace_size.value() < 4 ||
                        offset + ace_size.value() > acl_size.value()) {
                        ipc::ipc_log(
                            "SetEntriesInAclW count=%u -> ERROR_INVALID_ACL "
                            "(ace=%u)",
                            count, index);
                        return ERROR_INVALID_ACL;
                    }
                    old_aces.emplace_back(ace_size.value());
                    if (m.memory().read_bytes(old_acl + offset,
                                              old_aces.back().data(),
                                              old_aces.back().size()).is_err()) {
                        ipc::ipc_log(
                            "SetEntriesInAclW count=%u -> ERROR_INVALID_ACL "
                            "(unreadable ace=%u)",
                            count, index);
                        return ERROR_INVALID_ACL;
                    }
                    offset += ace_size.value();
                }
            }

            // Merge rules measured on Windows (advapi_acl_probe sea_*):
            //  REVOKE removes the trustee's explicit ACCESS_ALLOWED (and
            //    SYSTEM_AUDIT) ACEs, keeping its denies;
            //  SET removes every explicit access ACE of the trustee, then
            //    adds the allow;
            //  GRANT / DENY / SET_AUDIT OR the rights into an existing
            //    explicit ACE of the same kind and inheritance, else add one;
            //  new denies go first, new allows go just before the existing
            //    explicit allows, and inherited ACEs stay at the end.
            auto ace_sid_matches = [](const std::vector<u8>& ace,
                                      const std::vector<u8>& sid) {
                usize off = 8;
                if (ace[0] >= 5 && ace[0] <= 8) {
                    u32 flags = 0;
                    std::memcpy(&flags, ace.data() + 8, 4);
                    off = 12 + ((flags & 1) ? 16 : 0) + ((flags & 2) ? 16 : 0);
                }
                return ace.size() >= off + sid.size() &&
                       std::equal(sid.begin(), sid.end(), ace.begin() + off);
            };
            constexpr u8 INHERITED_ACE = 0x10;
            auto is_inherited = [](const std::vector<u8>& ace) {
                return (ace[1] & 0x10) != 0;
            };
            auto kind_of = [](u8 type) -> int { // 0 allow, 1 deny, 2 audit
                switch (type) {
                case 0: case 5: return 0;
                case 1: case 6: return 1;
                case 2: case 7: return 2;
                default: return -1;
                }
            };
            auto make_ace = [&](const AceSpec& spec) {
                u8 type = 0;
                u8 extra_flags = 0;
                if (spec.access_mode == DENY_ACCESS) {
                    type = 1; // ACCESS_DENIED_ACE_TYPE
                } else if (is_audit_mode(spec.access_mode)) {
                    type = 2; // SYSTEM_AUDIT_ACE_TYPE
                    if (spec.access_mode == SET_AUDIT_SUCCESS ||
                        spec.access_mode == SET_AUDIT_BOTH) {
                        extra_flags |= 0x40; // SUCCESSFUL_ACCESS_ACE_FLAG
                    }
                    if (spec.access_mode == SET_AUDIT_FAILURE ||
                        spec.access_mode == SET_AUDIT_BOTH) {
                        extra_flags |= 0x80; // FAILED_ACCESS_ACE_FLAG
                    }
                }
                std::vector<u8> ace(8, 0);
                if (spec.object_ace) {
                    type = static_cast<u8>(type + 5); // *_OBJECT_ACE_TYPE
                    ace.resize(12, 0);
                    std::memcpy(ace.data() + 8, &spec.objects_present, 4);
                    if (spec.objects_present & 1u) {
                        ace.insert(ace.end(), spec.object_type.begin(),
                                   spec.object_type.end());
                    }
                    if (spec.objects_present & 2u) {
                        ace.insert(ace.end(), spec.inherited_object_type.begin(),
                                   spec.inherited_object_type.end());
                    }
                }
                ace.insert(ace.end(), spec.sid.begin(), spec.sid.end());
                ace[0] = type;
                ace[1] = static_cast<u8>((spec.inheritance & 0x1Fu) | extra_flags);
                const u16 size = static_cast<u16>(ace.size());
                ace[2] = static_cast<u8>(size);
                ace[3] = static_cast<u8>(size >> 8);
                std::memcpy(ace.data() + 4, &spec.access_mask, 4);
                return ace;
            };

            std::vector<std::vector<u8>> result_aces = old_aces;
            for (const auto& spec : specs) {
                const bool revoke = spec.access_mode == REVOKE_ACCESS;
                const bool set = spec.access_mode == SET_ACCESS;
                if (revoke || set) {
                    result_aces.erase(
                        std::remove_if(result_aces.begin(), result_aces.end(),
                            [&](const std::vector<u8>& ace) {
                                if (is_inherited(ace) || !ace_sid_matches(ace, spec.sid)) {
                                    return false;
                                }
                                const int k = kind_of(ace[0]);
                                return revoke ? (k == 0 || k == 2) : (k == 0 || k == 1);
                            }),
                        result_aces.end());
                    if (revoke) continue;
                }
                std::vector<u8> fresh = make_ace(spec);
                const int kind = kind_of(fresh[0]);
                bool merged = false;
                if (!set) {
                    for (auto& ace : result_aces) {
                        if (!is_inherited(ace) && ace[0] == fresh[0] &&
                            ace[1] == fresh[1] && ace.size() == fresh.size() &&
                            ace_sid_matches(ace, spec.sid) &&
                            std::equal(ace.begin() + 8,
                                       ace.begin() + static_cast<std::ptrdiff_t>(
                                           ace.size() - spec.sid.size()),
                                       fresh.begin() + 8)) {
                            u32 mask = 0;
                            std::memcpy(&mask, ace.data() + 4, 4);
                            mask |= spec.access_mask;
                            std::memcpy(ace.data() + 4, &mask, 4);
                            merged = true;
                            break;
                        }
                    }
                }
                if (merged) continue;
                usize pos = 0;
                if (kind == 0) {
                    // Before the first existing explicit allow, else before
                    // the inherited ACEs.
                    pos = result_aces.size();
                    for (usize i = 0; i < result_aces.size(); ++i) {
                        if (is_inherited(result_aces[i]) ||
                            kind_of(result_aces[i][0]) == 0) {
                            pos = i;
                            break;
                        }
                    }
                }
                result_aces.insert(result_aces.begin() + static_cast<std::ptrdiff_t>(pos),
                                   std::move(fresh));
            }
            (void)INHERITED_ACE;

            u32 acl_size = 8;
            for (const auto& ace : result_aces) {
                acl_size += static_cast<u32>(ace.size());
            }
            if (acl_size > 0xFFFFu || result_aces.size() > 0xFFFFu) {
                ipc::ipc_log(
                    "SetEntriesInAclW count=%u -> ERROR_INVALID_ACL "
                    "(result too large)",
                    count);
                return ERROR_INVALID_ACL;
            }
            acl_bytes.assign(8, 0);
            acl_bytes[0] = acl_revision;
            acl_bytes[2] = static_cast<u8>(acl_size);
            acl_bytes[3] = static_cast<u8>(acl_size >> 8);
            acl_bytes[4] = static_cast<u8>(result_aces.size());
            acl_bytes[5] = static_cast<u8>(result_aces.size() >> 8);
            for (const auto& ace : result_aces) {
                acl_bytes.insert(acl_bytes.end(), ace.begin(), ace.end());
            }
            std::ostringstream modes;
            for (usize index = 0; index < specs.size(); ++index) {
                if (index != 0) modes << ',';
                modes << specs[index].access_mode;
            }
            ipc::ipc_log(
                "SetEntriesInAclW count=%u modes=[%s] old_aces=%u "
                "-> ERROR_SUCCESS aces=%zu size=%u",
                count, modes.str().c_str(), old_ace_count,
                result_aces.size(), acl_size);
            return error_codes::ERROR_SUCCESS;
    };
    auto make_set_entries_in_acl = [&stubs, set_entries_core](bool wide) -> ApiHandler {
        return [&stubs, set_entries_core, wide](Marshaler& m) {
            const u32 count = static_cast<u32>(m.arg(0));
            const GuestAddr entries = m.arg_ptr(1);
            const GuestAddr old_acl = m.arg_ptr(2);
            const GuestAddr acl_out = m.arg_ptr(3);
            if (count > 1024 || (count != 0 && entries == 0) || acl_out == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            (void)m.write_ptr(acl_out, 0);
            std::vector<u8> acl_bytes;
            const u32 err = set_entries_core(m, wide, count, entries, old_acl, acl_bytes);
            if (err != error_codes::ERROR_SUCCESS) {
                m.set_return(err);
                return;
            }
            const GuestAddr new_acl = stubs.alloc_sid_memory(static_cast<u32>(acl_bytes.size()));
            if (new_acl == 0) {
                m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return;
            }
            (void)m.memory().write_bytes(new_acl, acl_bytes.data(), acl_bytes.size());
            (void)m.write_ptr(acl_out, new_acl);
            VITREO_TRACE("[advapi32] SetEntriesInAcl%c(count=%u) -> ERROR_SUCCESS size=%zu\n",
                         wide ? 'W' : 'A', count, acl_bytes.size());
            m.set_return(error_codes::ERROR_SUCCESS);
        };
    };
    exports.entries.push_back(
        {"SetEntriesInAclA", 0, make_set_entries_in_acl(false), 16});
    exports.entries.push_back(
        {"SetEntriesInAclW", 0, make_set_entries_in_acl(true), 16});

    // DWORD BuildSecurityDescriptor{A,W}(PTRUSTEE pOwner, PTRUSTEE pGroup,
    //   ULONG cCountOfAccessEntries, PEXPLICIT_ACCESS pListOfAccessEntries,
    //   ULONG cCountOfAuditEntries, PEXPLICIT_ACCESS pListOfAuditEntries,
    //   PSECURITY_DESCRIPTOR pOldSD, PULONG pSizeNewSD,
    //   PSECURITY_DESCRIPTOR* pNewSD) — stdcall 36.
    // https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-buildsecuritydescriptorw
    // Owner/group from the trustees, else pOldSD; each ACL is pOldSD's merged
    // with the entries by the SetEntriesInAcl rules, and with no list it is
    // pOldSD's or a present NULL ACL. Measured (advapi_sdapi_probe bsd_*): the
    // result is self-relative (SACL, DACL, owner, group) from LocalAlloc; an
    // unresolvable trustee returns ERROR_NONE_MAPPED (also the last error)
    // with the outputs untouched; success returns ERROR_SUCCESS and leaves
    // ERROR_INSUFFICIENT_BUFFER as the last error.
    auto build_security_descriptor = [&stubs, set_entries_core](Marshaler& m, bool wide) {
        const GuestAddr owner_trustee = m.arg_ptr(0);
        const GuestAddr group_trustee = m.arg_ptr(1);
        const u32 access_count = m.arg32(2);
        const GuestAddr access_list = m.arg_ptr(3);
        const u32 audit_count = m.arg32(4);
        const GuestAddr audit_list = m.arg_ptr(5);
        const GuestAddr old_sd = m.arg_ptr(6);
        const GuestAddr size_out = m.arg_ptr(7);
        const GuestAddr sd_out = m.arg_ptr(8);
        auto fail = [&m](u32 err) {
            set_last_error(err);
            m.set_return(err);
        };
        if (size_out == 0 || sd_out == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        sddl::SdView view;
        GuestAddr old_dacl = 0, old_sacl = 0;
        if (old_sd != 0) {
            if (const u32 err = sddl::capture_guest_sd(m, old_sd, view); err != 0) {
                return fail(error_codes::ERROR_INVALID_SECURITY_DESCR);
            }
            const u16 control = m.read_u16(old_sd + 2).value_or(0);
            if ((control & 0x8000u) != 0) {
                const u32 d = m.read_u32(old_sd + 16).value_or(0);
                const u32 a = m.read_u32(old_sd + 12).value_or(0);
                old_dacl = ((control & 0x4u) != 0 && d != 0) ? old_sd + d : 0;
                old_sacl = ((control & 0x10u) != 0 && a != 0) ? old_sd + a : 0;
            } else {
                const u32 w = m.cpu_is_64bit() ? 8u : 4u;
                old_dacl = (control & 0x4u) != 0
                    ? static_cast<GuestAddr>(m.read_ptr(old_sd + w + 3 * w).value_or(0)) : 0;
                old_sacl = (control & 0x10u) != 0
                    ? static_cast<GuestAddr>(m.read_ptr(old_sd + w + 2 * w).value_or(0)) : 0;
            }
        }
        // TRUSTEE { pMultipleTrustee; MultipleTrusteeOperation; TrusteeForm;
        // TrusteeType; ptstrName } (accctrl.h).
        auto trustee_sid = [&m, wide](GuestAddr trustee, std::vector<u8>& sid) -> u32 {
            constexpr u32 TRUSTEE_IS_SID = 0, TRUSTEE_IS_NAME = 1;
            const bool is64 = m.cpu_is_64bit();
            const u32 form = m.read_u32(trustee + (is64 ? 12u : 8u)).value_or(~0u);
            const GuestAddr name = static_cast<GuestAddr>(
                m.read_ptr(trustee + (is64 ? 24u : 16u)).value_or(0));
            if (form == TRUSTEE_IS_SID) {
                if (name == 0 || SecurityStubs::is_valid_sid(m, name) == 0) {
                    return error_codes::ERROR_INVALID_SID;
                }
                sid.resize(SecurityStubs::get_length_sid(m, name));
                (void)m.memory().read_bytes(name, sid.data(), sid.size());
                return error_codes::ERROR_SUCCESS;
            }
            if (form == TRUSTEE_IS_NAME) {
                auto text = wide ? m.read_wide_string(name) : m.read_ansi_string(name);
                if (name == 0 || text.is_err() || !sid_from_account_name(text.value(), sid)) {
                    return error_codes::ERROR_NONE_MAPPED;
                }
                return error_codes::ERROR_SUCCESS;
            }
            return error_codes::ERROR_INVALID_PARAMETER;
        };
        if (owner_trustee != 0) {
            if (const u32 err = trustee_sid(owner_trustee, view.owner); err != 0) return fail(err);
        }
        if (group_trustee != 0) {
            if (const u32 err = trustee_sid(group_trustee, view.group); err != 0) return fail(err);
        }
        auto build_acl = [&](GuestAddr list, u32 count, GuestAddr old_acl, bool& present,
                             bool& is_null, std::vector<u8>& acl) -> u32 {
            if (list == 0) {
                if (old_acl == 0) {
                    present = true;
                    is_null = true;
                    acl.clear();
                }
                return error_codes::ERROR_SUCCESS;   // otherwise keep pOldSD's
            }
            std::vector<u8> merged;
            if (const u32 err = set_entries_core(m, wide, count, list, old_acl, merged); err != 0) {
                return err;
            }
            present = true;
            is_null = false;
            acl = std::move(merged);
            return error_codes::ERROR_SUCCESS;
        };
        if (const u32 err = build_acl(access_list, access_count, old_dacl, view.dacl_present,
                                      view.dacl_null, view.dacl); err != 0) {
            return fail(err);
        }
        if (const u32 err = build_acl(audit_list, audit_count, old_sacl, view.sacl_present,
                                      view.sacl_null, view.sacl); err != 0) {
            return fail(err);
        }
        const std::vector<u8> bytes = sddl::build_self_relative(view);
        const GuestAddr block = stubs.alloc_local_memory(static_cast<u32>(bytes.size()));
        if (block == 0) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        (void)m.memory().write_bytes(block, bytes.data(), bytes.size());
        (void)m.write_u32(size_out, static_cast<u32>(bytes.size()));
        (void)m.write_ptr(sd_out, block);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(error_codes::ERROR_SUCCESS);
    };
    exports.entries.push_back({"BuildSecurityDescriptorA", 0,
        [build_security_descriptor](Marshaler& m) { build_security_descriptor(m, false); }, 36});
    exports.entries.push_back({"BuildSecurityDescriptorW", 0,
        [build_security_descriptor](Marshaler& m) { build_security_descriptor(m, true); }, 36});
}

} // namespace vitreo::winapi
