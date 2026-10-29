// SPDX-License-Identifier: LGPL-2.1-only

/// @file access_check.cpp
/// @brief advapi32!AccessCheck: DACL evaluation against an impersonation
/// token, including deny ACEs, deny-only and restricting SIDs, owner rights,
/// privileges, mandatory integrity and MAXIMUM_ALLOWED.
///
/// Contract: Learn AccessCheck and "How DACLs Control Access to an Object";
/// everything else measured by tools/oracle_probe/advapi_token2_probe.c
/// (the ac_* lines of .expected / .x86.expected).

#include "winapi/advapi32/security/sddl.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kMaximumAllowed = 0x02000000u;
constexpr u32 kAccessSystemSecurity = 0x01000000u;
constexpr u32 kGenericBits = 0xF0000000u;
constexpr u32 kReadControl = 0x00020000u;
constexpr u32 kWriteDac = 0x00040000u;
constexpr u32 kWriteOwner = 0x00080000u;
constexpr u32 kSeGroupEnabled = 0x4u;
constexpr u32 kSeGroupUseForDenyOnly = 0x10u;
constexpr u64 kSeSecurityPrivilege = 8;
constexpr u64 kSeTakeOwnershipPrivilege = 9;
constexpr u32 kSePrivilegeEnabled = 0x2u;
constexpr u32 kSePrivilegeUsedForAccess = 0x80000000u;

struct GenericMapping {
    u32 read{0}, write{0}, execute{0}, all{0};
};

[[nodiscard]] std::vector<u8> sid_at(std::span<const u8> bytes, usize at) {
    if (at + 8 > bytes.size()) return {};
    const usize len = 8u + bytes[at + 1] * 4u;
    if (at + len > bytes.size()) return {};
    return {bytes.begin() + static_cast<std::ptrdiff_t>(at),
            bytes.begin() + static_cast<std::ptrdiff_t>(at + len)};
}

[[nodiscard]] bool is_owner_rights(const std::vector<u8>& sid) {
    static const std::vector<u8> kOwnerRights{1, 1, 0, 0, 0, 0, 0, 3, 4, 0, 0, 0};
    return sid == kOwnerRights;   // S-1-3-4
}

struct Ace {
    bool deny{false};
    u32 mask{0};
    std::vector<u8> sid;
};

/// The DACL's effective ACEs: allowed / denied (and object forms without
/// GUIDs), inherit-only ones skipped. ACE masks are used as stored: generic
/// bits in an ACE are not mapped (probe ac_generic_ace).
[[nodiscard]] std::vector<Ace> dacl_aces(const std::vector<u8>& acl) {
    std::vector<Ace> out;
    if (acl.size() < 8) return out;
    const u32 count = static_cast<u32>(acl[4] | (acl[5] << 8));
    usize off = 8;
    for (u32 i = 0; i < count && off + 8 <= acl.size(); ++i) {
        const u8 type = acl[off];
        const u8 flags = acl[off + 1];
        const usize size = static_cast<usize>(acl[off + 2] | (acl[off + 3] << 8));
        if (size < 8 || off + size > acl.size()) break;
        const std::span<const u8> ace(acl.data() + off, size);
        off += size;
        if ((flags & 0x08u) != 0) continue;  // INHERIT_ONLY_ACE
        u32 mask = 0;
        std::memcpy(&mask, ace.data() + 4, 4);
        if (type == 0x00 || type == 0x01) {
            out.push_back({type == 0x01, mask, sid_at(ace, 8)});
        } else if ((type == 0x05 || type == 0x06) && size >= 12) {
            u32 object_flags = 0;
            std::memcpy(&object_flags, ace.data() + 8, 4);
            // AccessCheck has no object-type list: only ACEs that name no
            // object type apply to the object itself.
            if ((object_flags & 1u) != 0) continue;
            const usize sid_off = 12 + ((object_flags & 2u) != 0 ? 16u : 0u);
            out.push_back({type == 0x06, mask, sid_at(ace, sid_off)});
        }
    }
    return out;
}

/// Mandatory label of the object: (integrity RID, policy). No label means
/// Medium with NO_WRITE_UP (probe ac_low_nolabel_write).
void object_label(const sddl::SdView& sd, u32& rid, u32& policy) {
    rid = 0x2000;
    policy = 0x1;
    if (!sd.sacl_present || sd.sacl_null || sd.sacl.size() < 8) return;
    const u32 count = static_cast<u32>(sd.sacl[4] | (sd.sacl[5] << 8));
    usize off = 8;
    for (u32 i = 0; i < count && off + 8 <= sd.sacl.size(); ++i) {
        const u8 type = sd.sacl[off];
        const u8 flags = sd.sacl[off + 1];
        const usize size = static_cast<usize>(sd.sacl[off + 2] | (sd.sacl[off + 3] << 8));
        if (size < 8 || off + size > sd.sacl.size()) return;
        if (type == 0x11 && (flags & 0x08u) == 0) {
            const auto sid = sid_at(std::span<const u8>(sd.sacl).subspan(off, size), 8);
            if (sid.size() >= 12) {
                std::memcpy(&policy, sd.sacl.data() + off + 4, 4);
                std::memcpy(&rid, sid.data() + 8 + (sid[1] - 1u) * 4u, 4);
            }
            return;
        }
        off += size;
    }
}

/// One evaluation pass. `principals` are the SIDs that match allow ACEs,
/// `deny_only` the extra SIDs that match deny ACEs. Returns the granted mask
/// for MAXIMUM_ALLOWED semantics and, for a specific request, whether every
/// requested bit was granted before a deny hit it.
struct PassResult {
    u32 granted{0};
    bool request_granted{false};
};

[[nodiscard]] PassResult evaluate(const std::vector<Ace>& aces,
                                  const std::vector<std::vector<u8>>& principals,
                                  const std::vector<std::vector<u8>>& deny_only,
                                  const std::vector<u8>& owner, u32 request) {
    auto has = [](const std::vector<std::vector<u8>>& set, const std::vector<u8>& sid) {
        return std::find(set.begin(), set.end(), sid) != set.end();
    };
    const bool is_owner = !owner.empty() && has(principals, owner);
    bool owner_rights_ace = false;
    for (const auto& ace : aces) owner_rights_ace = owner_rights_ace || is_owner_rights(ace.sid);
    // The owner gets READ_CONTROL | WRITE_DAC unless an OWNER RIGHTS ACE
    // restates its rights (probe ac_owner_implicit / ac_owner_rights_ace).
    const u32 implicit = (is_owner && !owner_rights_ace) ? (kReadControl | kWriteDac) : 0u;

    PassResult r;
    // MAXIMUM_ALLOWED walk: allows add what no earlier deny took.
    u32 granted = implicit, denied = 0;
    // Specific-request walk: the first deny that hits a still-needed bit ends
    // it (probe ac_deny_first / ac_allow_then_deny / ac_deny_other_bits).
    u32 remaining = request & ~implicit;
    bool request_denied = false;
    for (const auto& ace : aces) {
        const bool owner_ace = is_owner_rights(ace.sid);
        const bool matches = owner_ace ? is_owner
                                       : (has(principals, ace.sid) ||
                                          (ace.deny && has(deny_only, ace.sid)));
        if (!matches) continue;
        if (ace.deny) {
            denied |= ace.mask & ~granted;
            if ((ace.mask & remaining) != 0) request_denied = true;
        } else {
            granted |= ace.mask & ~denied;
            if (!request_denied) remaining &= ~ace.mask;
        }
    }
    r.granted = granted;
    r.request_granted = !request_denied && remaining == 0;
    return r;
}

void write_privilege_set(Marshaler& m, GuestAddr set,
                         const std::vector<SecurityStubs::TokenPrivilege>& used) {
    if (set == 0) return;
    (void)m.write_u32(set, static_cast<u32>(used.size()));
    (void)m.write_u32(set + 4, 0);
    for (usize i = 0; i < used.size(); ++i) {
        (void)m.write_u64(set + 8 + i * 12, used[i].luid);
        (void)m.write_u32(set + 16 + i * 12, used[i].attributes | kSePrivilegeUsedForAccess);
    }
}

} // namespace

void add_access_check_export(DllExports& exports, SecurityStubs& stubs) {
    // BOOL AccessCheck(PSECURITY_DESCRIPTOR, HANDLE ClientToken,
    //   DWORD DesiredAccess, PGENERIC_MAPPING, PPRIVILEGE_SET PrivilegeSet,
    //   LPDWORD PrivilegeSetLength, LPDWORD GrantedAccess,
    //   LPBOOL AccessStatus) — stdcall 32.
    exports.entries.push_back({"AccessCheck", 42, [&stubs](Marshaler& m) {
        constexpr u32 ERROR_NO_IMPERSONATION_TOKEN = 1309;
        constexpr u32 ERROR_BAD_IMPERSONATION_LEVEL = 1346;
        constexpr u32 ERROR_GENERIC_NOT_MAPPED = 1360;
        constexpr u32 ERROR_PRIVILEGE_NOT_HELD = 1314;
        constexpr u32 TOKEN_QUERY = 0x0008;
        constexpr Handle kProcessPseudo = 0xFFFFFFFCu, kThreadPseudo = 0xFFFFFFFBu,
                         kEffectivePseudo = 0xFFFFFFFAu;
        const GuestAddr sd_ptr = m.arg_ptr(0);
        const Handle token = static_cast<Handle>(m.arg(1));
        const u32 desired = m.arg32(2);
        const GuestAddr mapping_ptr = m.arg_ptr(3);
        const GuestAddr privilege_set = m.arg_ptr(4);
        const GuestAddr privilege_len = m.arg_ptr(5);
        const GuestAddr granted_out = m.arg_ptr(6);
        const GuestAddr status_out = m.arg_ptr(7);
        auto fail = [&m](u32 err) {
            // GrantedAccess / AccessStatus are not written on failure.
            set_last_error(err);
            m.set_return(0);
        };

        // PRIVILEGE_SET with one entry is 20 bytes; a shorter buffer reports
        // the size needed before anything else is looked at, and leaves the
        // set alone (probe ac_pslen*). Past that, PrivilegeCount is zeroed
        // even when a later check fails (probe ac_primary privCount=0).
        constexpr u32 kPrivilegeSetSize = 20;
        if (privilege_len == 0) return fail(error_codes::ERROR_NOACCESS);
        if (m.read_u32(privilege_len).value_or(0) < kPrivilegeSetSize) {
            (void)m.write_u32(privilege_len, kPrivilegeSetSize);
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        if (privilege_set != 0) (void)m.write_u32(privilege_set, 0);

        // The client token: an impersonation token the handle may query
        // (probe ac_badtoken / ac_noquery / ac_primary).
        SecurityStubs::TokenIdentity id;
        if (token == kProcessPseudo) {
            return fail(ERROR_NO_IMPERSONATION_TOKEN);
        } else if (token == kThreadPseudo || token == kEffectivePseudo) {
            bool impersonating = false;
            id = stubs.effective_identity(impersonating);
            if (!impersonating) {
                return fail(token == kThreadPseudo ? error_codes::ERROR_NO_TOKEN
                                                   : ERROR_NO_IMPERSONATION_TOKEN);
            }
        } else {
            if (stubs.handle_table().get_type(token) != HandleType::Token) {
                return fail(error_codes::ERROR_INVALID_HANDLE);
            }
            if (!stubs.token_has_access(token, TOKEN_QUERY)) {
                return fail(error_codes::ERROR_ACCESS_DENIED);
            }
            id = stubs.token_identity(token);
            if (id.type != 2) return fail(ERROR_NO_IMPERSONATION_TOKEN);
        }
        if (id.impersonation_level == 0) return fail(ERROR_BAD_IMPERSONATION_LEVEL);

        // The descriptor must name an owner and a group (Learn; probe
        // ac_no_owner / ac_no_group).
        sddl::SdView sd;
        if (sd_ptr == 0 || sddl::capture_guest_sd(m, sd_ptr, sd) != 0 || sd.owner.empty() ||
            sd.group.empty()) {
            return fail(error_codes::ERROR_INVALID_SECURITY_DESCR);
        }
        // DesiredAccess must already be mapped (Learn; probe ac_generic_desired).
        if ((desired & kGenericBits) != 0) return fail(ERROR_GENERIC_NOT_MAPPED);
        if (mapping_ptr == 0 || granted_out == 0 || status_out == 0) {
            return fail(error_codes::ERROR_NOACCESS);
        }
        GenericMapping map;
        map.read = m.read_u32(mapping_ptr).value_or(0);
        map.write = m.read_u32(mapping_ptr + 4).value_or(0);
        map.execute = m.read_u32(mapping_ptr + 8).value_or(0);
        map.all = m.read_u32(mapping_ptr + 12).value_or(0);

        const bool maximum = (desired & kMaximumAllowed) != 0;
        u32 request = desired & ~(kMaximumAllowed | kAccessSystemSecurity);
        std::vector<SecurityStubs::TokenPrivilege> used;
        auto privilege = [&id](u64 luid) -> const SecurityStubs::TokenPrivilege* {
            for (const auto& p : id.privileges) {
                if (p.luid == luid && (p.attributes & kSePrivilegeEnabled) != 0) return &p;
            }
            return nullptr;
        };
        auto deny = [&](u32 err) {
            write_privilege_set(m, privilege_set, used);
            (void)m.write_u32(granted_out, 0);
            (void)m.write_u32(status_out, 0);
            set_last_error(err);
            m.set_return(1);
        };

        // ACCESS_SYSTEM_SECURITY comes only from an enabled
        // SeSecurityPrivilege (probe ac_sacl_priv_*).
        u32 privileged = 0;
        if ((desired & kAccessSystemSecurity) != 0) {
            const auto* p = privilege(kSeSecurityPrivilege);
            if (p == nullptr) return deny(ERROR_PRIVILEGE_NOT_HELD);
            used.push_back(*p);
            privileged |= kAccessSystemSecurity;
        }
        // WRITE_OWNER asked for by name is granted by an enabled
        // SeTakeOwnershipPrivilege (probe ac_writeowner_*); MAXIMUM_ALLOWED
        // alone does not use it.
        if ((request & kWriteOwner) != 0) {
            if (const auto* p = privilege(kSeTakeOwnershipPrivilege); p != nullptr) {
                used.push_back(*p);
                privileged |= kWriteOwner;
                request &= ~kWriteOwner;
            }
        }

        // Mandatory integrity: below the object's label the policy removes
        // the generic write / read / execute rights (probe ac_label_* and
        // ac_low_*).
        u32 label_rid = 0, policy = 0;
        object_label(sd, label_rid, policy);
        u32 mic_allowed = 0xFFFFFFFFu;
        if (id.integrity_rid < label_rid && (policy & 7u) != 0) {
            mic_allowed = 0;
            if ((policy & 0x2u) == 0) mic_allowed |= map.read;      // NO_READ_UP
            if ((policy & 0x1u) == 0) mic_allowed |= map.write;     // NO_WRITE_UP
            if ((policy & 0x4u) == 0) mic_allowed |= map.execute;   // NO_EXECUTE_UP
        }

        // Principals of the normal pass and of the restricting pass.
        std::vector<std::vector<u8>> principals, deny_only;
        if (!id.user.empty()) (id.user_deny_only ? deny_only : principals).push_back(id.user);
        for (const auto& g : id.groups) {
            if ((g.attributes & kSeGroupUseForDenyOnly) != 0) deny_only.push_back(g.sid);
            else if ((g.attributes & kSeGroupEnabled) != 0) principals.push_back(g.sid);
        }

        u32 granted_all = 0;
        bool request_ok = false;
        if (!sd.dacl_present || sd.dacl_null) {
            // No DACL: everything (probe ac_nulldacl / ac_nodacl).
            granted_all = map.all | request;
            request_ok = true;
        } else {
            const auto aces = dacl_aces(sd.dacl);
            PassResult normal = evaluate(aces, principals, deny_only, sd.owner, request);
            granted_all = normal.granted;
            request_ok = normal.request_granted;
            if (!id.restricting.empty()) {
                PassResult restricted = evaluate(aces, id.restricting, {}, sd.owner, request);
                granted_all &= restricted.granted;
                request_ok = request_ok && restricted.request_granted;
            }
        }
        granted_all &= mic_allowed;
        if ((request & ~mic_allowed) != 0) request_ok = false;

        u32 result = 0;
        if (maximum) {
            // Explicitly requested bits must be grantable too (probe
            // ac_max_plus_read); nothing grantable is a denial (ac_max_empty).
            if ((request & ~granted_all) != 0 || (granted_all | privileged) == 0) {
                return deny(error_codes::ERROR_ACCESS_DENIED);
            }
            result = granted_all | privileged;
        } else {
            // Nothing requested grants nothing (probe ac_zero_desired).
            if ((request | privileged) == 0 || !request_ok) {
                return deny(error_codes::ERROR_ACCESS_DENIED);
            }
            result = request | privileged;
        }
        write_privilege_set(m, privilege_set, used);
        (void)m.write_u32(granted_out, result);
        (void)m.write_u32(status_out, 1);
        m.set_return(1);
    }, 32});
}

} // namespace vitreo::winapi
