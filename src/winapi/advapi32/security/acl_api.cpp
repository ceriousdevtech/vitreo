// SPDX-License-Identifier: LGPL-2.1-only

/// @file acl_api.cpp
/// @brief advapi32 ACL / security-descriptor primitives: InitializeAcl,
/// IsValidAcl, the Add*Ace family, AddAce, InitializeSecurityDescriptor,
/// IsValidSecurityDescriptor, SetSecurityDescriptorControl and
/// Get/SetSecurityDescriptorDacl/Sacl.
///
/// Every rule not stated on the Learn page for the API is measured by
/// tools/oracle_probe/advapi_sdapi_probe.c (.expected / .x86.expected); the
/// probe line names are quoted next to each rule.

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/common/object_access.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kInsufficientBuffer = 122;      // ERROR_INSUFFICIENT_BUFFER
constexpr u32 kInvalidFlags = 1004;           // ERROR_INVALID_FLAGS
constexpr u32 kUnknownRevision = 1305;        // ERROR_UNKNOWN_REVISION
constexpr u32 kRevisionMismatch = 1306;       // ERROR_REVISION_MISMATCH
constexpr u32 kInvalidAcl = 1336;             // ERROR_INVALID_ACL
constexpr u32 kInvalidSid = 1337;             // ERROR_INVALID_SID
constexpr u32 kInvalidSecurityDescr = 1338;   // ERROR_INVALID_SECURITY_DESCR
constexpr u32 kAllottedSpaceExceeded = 1344;  // ERROR_ALLOTTED_SPACE_EXCEEDED

constexpr u16 kSeDaclPresent = 0x0004;
constexpr u16 kSeDaclDefaulted = 0x0008;
constexpr u16 kSeSaclPresent = 0x0010;
constexpr u16 kSeSaclDefaulted = 0x0020;
constexpr u16 kSeSelfRelative = 0x8000;

[[nodiscard]] bool sid_ok(std::span<const u8> bytes) noexcept {
    return bytes.size() >= 8 && bytes[0] == 1 && bytes[1] <= 15 &&
           bytes.size() >= 8u + bytes[1] * 4u;
}

/// RtlValidAcl as measured (advapi_sdapi_probe ivacl_*); shared with the
/// kernel object security capture, which applies the same rules.
[[nodiscard]] bool acl_valid(std::span<const u8> acl, u32* used = nullptr) {
    return object_access::valid_acl(acl, used);
}

/// The whole ACL (AclSize bytes) at `acl`, or nullopt when unreadable.
[[nodiscard]] std::optional<std::vector<u8>> read_acl(Marshaler& m, GuestAddr acl) {
    if (acl == 0) return std::nullopt;
    const auto size = m.read_u16(acl + 2);
    if (size.is_err()) return std::nullopt;
    std::vector<u8> bytes(std::max<u32>(8u, size.value()));
    if (m.memory().read_bytes(acl, bytes.data(), bytes.size()).is_err()) return std::nullopt;
    return bytes;
}

[[nodiscard]] std::optional<std::vector<u8>> read_sid(Marshaler& m, GuestAddr sid) {
    if (sid == 0) return std::nullopt;
    const auto count = m.read_u8(sid + 1);
    if (count.is_err()) return std::nullopt;
    std::vector<u8> bytes(8u + count.value() * 4u);
    if (m.memory().read_bytes(sid, bytes.data(), bytes.size()).is_err()) return std::nullopt;
    return bytes;
}

/// Append one ACE through the Add*Ace family (not AddAce). Measured
/// (aaa_*, ama_*, aaoa_*): an ACL revision outside 2..4 ->
/// ERROR_REVISION_MISMATCH, a structurally bad ACL -> ERROR_INVALID_ACL, no
/// room -> ERROR_ALLOTTED_SPACE_EXCEEDED; a higher ACE revision raises the
/// ACL's revision.
[[nodiscard]] u32 append_ace(Marshaler& m, GuestAddr acl_ptr, u32 ace_revision,
                             const std::vector<u8>& ace) {
    auto acl = read_acl(m, acl_ptr);
    if (!acl) return error_codes::ERROR_NOACCESS;
    const u8 acl_revision = (*acl)[0];
    if (acl_revision < 2 || acl_revision > 4) return kRevisionMismatch;
    u32 used = 0;
    if (!acl_valid(*acl, &used)) return kInvalidAcl;
    const u32 size = static_cast<u32>((*acl)[2] | ((*acl)[3] << 8));
    if (used + ace.size() > size) return kAllottedSpaceExceeded;
    const u16 count = static_cast<u16>(((*acl)[4] | ((*acl)[5] << 8)) + 1);
    if (m.memory().write_bytes(acl_ptr + used, ace.data(), ace.size()).is_err()) {
        return error_codes::ERROR_NOACCESS;
    }
    (void)m.write_u16(acl_ptr + 4, count);
    if (ace_revision > acl_revision) (void)m.write_u8(acl_ptr, static_cast<u8>(ace_revision));
    return error_codes::ERROR_SUCCESS;
}

/// Serialise a plain (Mask + SID) ACE.
[[nodiscard]] std::vector<u8> plain_ace(u8 type, u8 flags, u32 mask, const std::vector<u8>& sid) {
    std::vector<u8> ace(8, 0);
    ace[0] = type;
    ace[1] = flags;
    std::memcpy(ace.data() + 4, &mask, 4);
    ace.insert(ace.end(), sid.begin(), sid.end());
    const u16 size = static_cast<u16>(ace.size());
    ace[2] = static_cast<u8>(size);
    ace[3] = static_cast<u8>(size >> 8);
    return ace;
}

void finish(Marshaler& m, u32 err) {
    if (err != error_codes::ERROR_SUCCESS) {
        set_last_error(err);
        m.set_return(0);
        return;
    }
    m.set_return(1);   // success leaves the last error untouched (probe gle=dead)
}

/// Shared body of AddAccessAllowedAce(Ex) / AddAccessDeniedAce(Ex) /
/// AddAuditAccessAce(Ex). `valid_flags` is the AceFlags the call accepts
/// (probe aaaex_flags / adaex / auditex): allowed 0x3F (with CRITICAL),
/// denied 0x1F, audit 0xDF.
void add_plain_ace(Marshaler& m, u8 type, u32 ace_revision, u32 flags, u32 valid_flags,
                   u32 mask, GuestAddr sid_ptr, u8 forced_flags) {
    if (ace_revision > 4) return finish(m, kRevisionMismatch);   // aaa_rev rev=5
    if ((flags & ~valid_flags) != 0) return finish(m, kInvalidFlags);
    const auto sid = read_sid(m, sid_ptr);
    if (!sid || !sid_ok(*sid)) return finish(m, kInvalidSid);
    const auto ace = plain_ace(type, static_cast<u8>(flags | forced_flags), mask, *sid);
    finish(m, append_ace(m, m.arg_ptr(0), ace_revision, ace));
}

/// Shared body of the Add*ObjectAce family: dwAceRevision must be
/// ACL_REVISION_DS (aaoa_rev2); with neither GUID the ACE is written in its
/// plain form (aaoa_noguid).
void add_object_ace(Marshaler& m, u8 object_type, u8 plain_type, u32 ace_revision, u32 flags,
                    u32 valid_flags, u32 mask, GuestAddr object_guid, GuestAddr inherited_guid,
                    GuestAddr sid_ptr, u8 forced_flags) {
    if (ace_revision != 4) return finish(m, kRevisionMismatch);
    if ((flags & ~valid_flags) != 0) return finish(m, kInvalidFlags);
    const auto sid = read_sid(m, sid_ptr);
    if (!sid || !sid_ok(*sid)) return finish(m, kInvalidSid);
    const u8 ace_flags = static_cast<u8>(flags | forced_flags);
    if (object_guid == 0 && inherited_guid == 0) {
        return finish(m, append_ace(m, m.arg_ptr(0), ace_revision,
                                    plain_ace(plain_type, ace_flags, mask, *sid)));
    }
    std::vector<u8> ace(12, 0);
    ace[0] = object_type;
    ace[1] = ace_flags;
    std::memcpy(ace.data() + 4, &mask, 4);
    u32 present = 0;
    for (const GuestAddr guid : {object_guid, inherited_guid}) {
        if (guid == 0) continue;
        std::array<u8, 16> bytes{};
        if (m.memory().read_bytes(guid, bytes.data(), 16).is_err()) {
            return finish(m, error_codes::ERROR_NOACCESS);
        }
        ace.insert(ace.end(), bytes.begin(), bytes.end());
    }
    if (object_guid != 0) present |= 1u;     // ACE_OBJECT_TYPE_PRESENT
    if (inherited_guid != 0) present |= 2u;  // ACE_INHERITED_OBJECT_TYPE_PRESENT
    std::memcpy(ace.data() + 8, &present, 4);
    ace.insert(ace.end(), sid->begin(), sid->end());
    const u16 size = static_cast<u16>(ace.size());
    ace[2] = static_cast<u8>(size);
    ace[3] = static_cast<u8>(size >> 8);
    finish(m, append_ace(m, m.arg_ptr(0), ace_revision, ace));
}

/// SD component addresses (absolute pointers or self-relative offsets).
struct SdPointers {
    GuestAddr owner{0}, group{0}, sacl{0}, dacl{0};
};

[[nodiscard]] SdPointers sd_pointers(Marshaler& m, GuestAddr sd, u16 control) {
    SdPointers p;
    if ((control & kSeSelfRelative) != 0) {
        auto at = [&](u32 field) -> GuestAddr {
            const u32 off = m.read_u32(sd + field).value_or(0);
            return off == 0 ? 0 : sd + off;
        };
        p.owner = at(4);
        p.group = at(8);
        p.sacl = at(12);
        p.dacl = at(16);
    } else {
        const u32 w = m.cpu_is_64bit() ? 8u : 4u;
        const u32 base = m.cpu_is_64bit() ? 8u : 4u;
        auto at = [&](u32 index) -> GuestAddr {
            return static_cast<GuestAddr>(m.read_ptr(sd + base + index * w).value_or(0));
        };
        p.owner = at(0);
        p.group = at(1);
        p.sacl = at(2);
        p.dacl = at(3);
    }
    return p;
}

} // namespace

void add_acl_api_exports(DllExports& exports) {
    // BOOL InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR, DWORD) — 8.
    // Revision != SECURITY_DESCRIPTOR_REVISION -> ERROR_UNKNOWN_REVISION and
    // the buffer is left alone (probe isd rev=0/2/-1).
    exports.entries.push_back({"InitializeSecurityDescriptor", 19, [](Marshaler& m) {
        const GuestAddr sd = m.arg_ptr(0);
        const u32 revision = m.arg32(1);
        if (revision != 1) return finish(m, kUnknownRevision);
        if (sd == 0) return finish(m, error_codes::ERROR_NOACCESS);
        std::vector<u8> header(m.cpu_is_64bit() ? 40u : 20u, 0);
        header[0] = 1;
        (void)m.memory().write_bytes(sd, header.data(), header.size());
        m.set_return(1);
    }, 8});

    // BOOL IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR) — 4. Revision 1,
    // valid owner/group SIDs, valid present ACLs; failure sets
    // ERROR_INVALID_SECURITY_DESCR (probe ivsd_*). Sbz1 is not examined.
    exports.entries.push_back({"IsValidSecurityDescriptor", 41, [](Marshaler& m) {
        const GuestAddr sd = m.arg_ptr(0);
        auto invalid = [&m] {
            set_last_error(kInvalidSecurityDescr);
            m.set_return(0);
        };
        const auto revision = m.read_u8(sd);
        const auto control = m.read_u16(sd + 2);
        if (sd == 0 || revision.is_err() || control.is_err() || revision.value() != 1) {
            return invalid();
        }
        const SdPointers p = sd_pointers(m, sd, control.value());
        for (const GuestAddr sid : {p.owner, p.group}) {
            if (sid == 0) continue;
            const auto bytes = read_sid(m, sid);
            if (!bytes || !sid_ok(*bytes)) return invalid();
        }
        const std::pair<u16, GuestAddr> acls[] = {{kSeDaclPresent, p.dacl},
                                                  {kSeSaclPresent, p.sacl}};
        for (const auto& [bit, acl] : acls) {
            if ((control.value() & bit) == 0 || acl == 0) continue;
            const auto bytes = read_acl(m, acl);
            if (!bytes || !acl_valid(*bytes)) return invalid();
        }
        m.set_return(1);
    }, 4});

    // BOOL InitializeAcl(PACL, DWORD nAclLength, DWORD dwAclRevision) — 12.
    // Probe iacl*: length < 8 -> ERROR_INSUFFICIENT_BUFFER, length > 0xFFFC or
    // a revision other than 2..4 -> ERROR_INVALID_PARAMETER (buffer untouched);
    // AclSize is the length rounded down to a DWORD multiple.
    exports.entries.push_back({"InitializeAcl", 37, [](Marshaler& m) {
        const GuestAddr acl = m.arg_ptr(0);
        const u32 length = m.arg32(1);
        const u32 revision = m.arg32(2);
        if (length < 8) return finish(m, kInsufficientBuffer);
        if (length > 0xFFFC || revision < 2 || revision > 4) {
            return finish(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        if (acl == 0) return finish(m, error_codes::ERROR_NOACCESS);
        const u16 size = static_cast<u16>(length & ~3u);
        const u8 header[8] = {static_cast<u8>(revision), 0, static_cast<u8>(size),
                              static_cast<u8>(size >> 8), 0, 0, 0, 0};
        (void)m.memory().write_bytes(acl, header, 8);
        m.set_return(1);
    }, 12});

    // BOOL IsValidAcl(PACL) — 4. FALSE sets ERROR_INVALID_ACL (probe ivacl_*).
    exports.entries.push_back({"IsValidAcl", 0, [](Marshaler& m) {
        const auto bytes = read_acl(m, m.arg_ptr(0));
        if (!bytes || !acl_valid(*bytes)) {
            set_last_error(kInvalidAcl);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    }, 4});

    // The Add*Ace family (securitybaseapi.h). x86 stdcall sizes from the
    // prototypes; flags/revision/SID/space rules in add_plain_ace.
    exports.entries.push_back({"AddAccessAllowedAce", 38, [](Marshaler& m) {
        add_plain_ace(m, 0x00, m.arg32(1), 0, 0, m.arg32(2), m.arg_ptr(3), 0);
    }, 16});
    exports.entries.push_back({"AddAccessAllowedAceEx", 0, [](Marshaler& m) {
        add_plain_ace(m, 0x00, m.arg32(1), m.arg32(2), 0x3F, m.arg32(3), m.arg_ptr(4), 0);
    }, 20});
    exports.entries.push_back({"AddAccessDeniedAce", 0, [](Marshaler& m) {
        add_plain_ace(m, 0x01, m.arg32(1), 0, 0, m.arg32(2), m.arg_ptr(3), 0);
    }, 16});
    exports.entries.push_back({"AddAccessDeniedAceEx", 0, [](Marshaler& m) {
        add_plain_ace(m, 0x01, m.arg32(1), m.arg32(2), 0x1F, m.arg32(3), m.arg_ptr(4), 0);
    }, 20});
    // BOOL AddAuditAccessAce(PACL, DWORD, DWORD AccessMask, PSID,
    //   BOOL bAuditSuccess, BOOL bAuditFailure) — 24.
    exports.entries.push_back({"AddAuditAccessAce", 0, [](Marshaler& m) {
        const u8 forced = static_cast<u8>((m.arg32(4) != 0 ? 0x40 : 0) | (m.arg32(5) != 0 ? 0x80 : 0));
        add_plain_ace(m, 0x02, m.arg32(1), 0, 0, m.arg32(2), m.arg_ptr(3), forced);
    }, 24});
    // BOOL AddAuditAccessAceEx(PACL, DWORD, DWORD AceFlags, DWORD AccessMask,
    //   PSID, BOOL bAuditSuccess, BOOL bAuditFailure) — 28.
    exports.entries.push_back({"AddAuditAccessAceEx", 0, [](Marshaler& m) {
        const u8 forced = static_cast<u8>((m.arg32(5) != 0 ? 0x40 : 0) | (m.arg32(6) != 0 ? 0x80 : 0));
        add_plain_ace(m, 0x02, m.arg32(1), m.arg32(2), 0xDF, m.arg32(3), m.arg_ptr(4), forced);
    }, 28});
    // BOOL AddMandatoryAce(PACL, DWORD, DWORD AceFlags, DWORD MandatoryPolicy,
    //   PSID pLabelSid) — 20. Probe ama_*: a policy above 7, flags outside
    //   the inherit set, or a SID that is not a mandatory label ->
    //   ERROR_INVALID_PARAMETER.
    exports.entries.push_back({"AddMandatoryAce", 0, [](Marshaler& m) {
        const u32 revision = m.arg32(1);
        const u32 flags = m.arg32(2);
        const u32 policy = m.arg32(3);
        if (revision > 4) return finish(m, kRevisionMismatch);
        if ((flags & ~0x1Fu) != 0 || (policy & ~7u) != 0) {
            return finish(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        const auto sid = read_sid(m, m.arg_ptr(4));
        if (!sid || !sid_ok(*sid)) return finish(m, kInvalidSid);
        if ((*sid)[2] != 0 || (*sid)[3] != 0 || (*sid)[4] != 0 || (*sid)[5] != 0 ||
            (*sid)[6] != 0 || (*sid)[7] != 16) {
            return finish(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        finish(m, append_ace(m, m.arg_ptr(0), revision,
                             plain_ace(0x11, static_cast<u8>(flags), policy, *sid)));
    }, 20});
    // BOOL AddAccessAllowedObjectAce(PACL, DWORD, DWORD AceFlags, DWORD
    //   AccessMask, GUID*, GUID*, PSID) — 28.
    exports.entries.push_back({"AddAccessAllowedObjectAce", 0, [](Marshaler& m) {
        add_object_ace(m, 0x05, 0x00, m.arg32(1), m.arg32(2), 0x1F, m.arg32(3),
                       m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6), 0);
    }, 28});
    exports.entries.push_back({"AddAccessDeniedObjectAce", 0, [](Marshaler& m) {
        add_object_ace(m, 0x06, 0x01, m.arg32(1), m.arg32(2), 0x1F, m.arg32(3),
                       m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6), 0);
    }, 28});
    // AddAuditAccessObjectAce(..., PSID, BOOL bAuditSuccess, BOOL
    // bAuditFailure) — 36.
    exports.entries.push_back({"AddAuditAccessObjectAce", 0, [](Marshaler& m) {
        const u8 forced = static_cast<u8>((m.arg32(7) != 0 ? 0x40 : 0) | (m.arg32(8) != 0 ? 0x80 : 0));
        add_object_ace(m, 0x07, 0x02, m.arg32(1), m.arg32(2), 0xDF, m.arg32(3),
                       m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6), forced);
    }, 36});

    // BOOL AddAce(PACL, DWORD dwAceRevision, DWORD dwStartingAceIndex,
    //   LPVOID pAceList, DWORD nAceListLength) — 20. Probe addace_*: the list
    //   must be whole ACEs (else ERROR_INVALID_PARAMETER); no room ->
    //   ERROR_INSUFFICIENT_BUFFER; an index past the end appends; a higher
    //   dwAceRevision raises the ACL's revision (even to 5).
    exports.entries.push_back({"AddAce", 0, [](Marshaler& m) {
        const GuestAddr acl_ptr = m.arg_ptr(0);
        const u32 revision = m.arg32(1);
        const u32 index = m.arg32(2);
        const GuestAddr list_ptr = m.arg_ptr(3);
        const u32 length = m.arg32(4);
        if (list_ptr == 0 || length == 0 || length > 0xFFFF) {
            return finish(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        std::vector<u8> list(length);
        if (m.memory().read_bytes(list_ptr, list.data(), list.size()).is_err()) {
            return finish(m, error_codes::ERROR_NOACCESS);
        }
        u32 added = 0;
        for (u32 off = 0; off < length; ++added) {
            if (off + 4 > length) return finish(m, error_codes::ERROR_INVALID_PARAMETER);
            const u32 size = static_cast<u32>(list[off + 2] | (list[off + 3] << 8));
            if (size < 4 || off + size > length) {
                return finish(m, error_codes::ERROR_INVALID_PARAMETER);
            }
            off += size;
        }
        auto acl = read_acl(m, acl_ptr);
        if (!acl) return finish(m, error_codes::ERROR_NOACCESS);
        u32 used = 0;
        if (!acl_valid(*acl, &used)) return finish(m, kInvalidAcl);
        const u32 size = static_cast<u32>((*acl)[2] | ((*acl)[3] << 8));
        if (used + length > size) return finish(m, kInsufficientBuffer);
        const u32 count = static_cast<u32>((*acl)[4] | ((*acl)[5] << 8));
        // Find the insertion offset.
        u32 at = used;
        if (index < count) {
            at = 8;
            for (u32 i = 0; i < index; ++i) {
                at += static_cast<u32>((*acl)[at + 2] | ((*acl)[at + 3] << 8));
            }
        }
        std::vector<u8> rebuilt(acl->begin(), acl->begin() + at);
        rebuilt.insert(rebuilt.end(), list.begin(), list.end());
        rebuilt.insert(rebuilt.end(), acl->begin() + at, acl->begin() + used);
        if (m.memory().write_bytes(acl_ptr + at, rebuilt.data() + at, rebuilt.size() - at).is_err()) {
            return finish(m, error_codes::ERROR_NOACCESS);
        }
        (void)m.write_u16(acl_ptr + 4, static_cast<u16>(count + added));
        if (revision > (*acl)[0]) (void)m.write_u8(acl_ptr, static_cast<u8>(revision));
        m.set_return(1);
    }, 20});

    // BOOL DeleteAce(PACL pAcl, DWORD dwAceIndex) -- 8. Learn: removes the
    // ACE at the index. Measured (advapi_deleteace_probe, both bitnesses):
    // every failure -- index >= AceCount, an ACL revision outside 2..4, an
    // AceCount the ACEs within AclSize cannot cover -- is
    // ERROR_INVALID_PARAMETER with the ACL unchanged; on success the later
    // ACEs move down, AceCount drops by one, AclSize is unchanged and the
    // bytes the removed ACE vacated at the end of the used area are zeroed.
    exports.entries.push_back({"DeleteAce", 0, [](Marshaler& m) {
        const GuestAddr acl_ptr = m.arg_ptr(0);
        const u32 index = m.arg32(1);
        auto acl = read_acl(m, acl_ptr);
        if (!acl) return finish(m, error_codes::ERROR_NOACCESS);
        const u8 revision = (*acl)[0];
        const u32 size = std::min<u32>(static_cast<u32>((*acl)[2] | ((*acl)[3] << 8)),
                                       static_cast<u32>(acl->size()));
        const u32 count = static_cast<u32>((*acl)[4] | ((*acl)[5] << 8));
        if (revision < 2 || revision > 4 || index >= count) {
            return finish(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        // Walk every ACE: the one to remove and the end of the used area.
        u32 at = 8, target = 0, target_size = 0;
        for (u32 i = 0; i < count; ++i) {
            if (at + 4 > size) return finish(m, error_codes::ERROR_INVALID_PARAMETER);
            const u32 ace_size = static_cast<u32>((*acl)[at + 2] | ((*acl)[at + 3] << 8));
            if (ace_size < 4 || at + ace_size > size) {
                return finish(m, error_codes::ERROR_INVALID_PARAMETER);
            }
            if (i == index) {
                target = at;
                target_size = ace_size;
            }
            at += ace_size;
        }
        const u32 used = at;
        std::vector<u8> tail(acl->begin() + target + target_size, acl->begin() + used);
        tail.resize(tail.size() + target_size, 0);
        if (m.memory().write_bytes(acl_ptr + target, tail.data(), tail.size()).is_err()) {
            return finish(m, error_codes::ERROR_NOACCESS);
        }
        (void)m.write_u16(acl_ptr + 4, static_cast<u16>(count - 1));
        m.set_return(1);
    }, 8});

    // BOOL SetSecurityDescriptorControl(PSECURITY_DESCRIPTOR,
    //   SECURITY_DESCRIPTOR_CONTROL BitsOfInterest, SECURITY_DESCRIPTOR_CONTROL
    //   BitsToSet) — 12 (WORDs are pushed as DWORDs). Only the inheritance
    //   bits plus SE_DACL_UNTRUSTED / SE_SERVER_SECURITY may be named, else
    //   ERROR_INVALID_PARAMETER; no revision check; works on self-relative
    //   descriptors too (probe ssdc_*).
    exports.entries.push_back({"SetSecurityDescriptorControl", 0, [](Marshaler& m) {
        constexpr u16 kSettable = 0x3FC0;
        const GuestAddr sd = m.arg_ptr(0);
        const u16 interest = static_cast<u16>(m.arg32(1));
        const u16 set = static_cast<u16>(m.arg32(2));
        if ((interest & ~kSettable) != 0 || (set & ~kSettable) != 0) {
            return finish(m, error_codes::ERROR_INVALID_PARAMETER);
        }
        const auto control = m.read_u16(sd + 2);
        if (sd == 0 || control.is_err()) return finish(m, error_codes::ERROR_NOACCESS);
        const u16 updated = static_cast<u16>((control.value() & ~interest) | (set & interest));
        (void)m.write_u16(sd + 2, updated);
        m.set_return(1);
    }, 12});

    // BOOL SetSecurityDescriptorDacl/Sacl(PSECURITY_DESCRIPTOR, BOOL bPresent,
    //   PACL, BOOL bDefaulted) — 16. Probe ssdd_* / ssds_*: revision != 1 ->
    //   ERROR_UNKNOWN_REVISION, a self-relative descriptor ->
    //   ERROR_INVALID_SECURITY_DESCR; bPresent FALSE clears only the PRESENT
    //   bit (pointer and DEFAULTED stay as they were).
    auto set_acl = [](Marshaler& m, bool dacl) {
        const GuestAddr sd = m.arg_ptr(0);
        const bool present = m.arg32(1) != 0;
        const GuestAddr acl = m.arg_ptr(2);
        const bool defaulted = m.arg32(3) != 0;
        const auto revision = m.read_u8(sd);
        const auto control = m.read_u16(sd + 2);
        if (sd == 0 || revision.is_err() || control.is_err()) {
            return finish(m, error_codes::ERROR_NOACCESS);
        }
        if (revision.value() != 1) return finish(m, kUnknownRevision);
        if ((control.value() & kSeSelfRelative) != 0) return finish(m, kInvalidSecurityDescr);
        const u16 present_bit = dacl ? kSeDaclPresent : kSeSaclPresent;
        const u16 defaulted_bit = dacl ? kSeDaclDefaulted : kSeSaclDefaulted;
        u16 updated = control.value();
        if (present) {
            updated = static_cast<u16>(updated | present_bit);
            updated = defaulted ? static_cast<u16>(updated | defaulted_bit)
                                : static_cast<u16>(updated & ~defaulted_bit);
            const u32 field = m.cpu_is_64bit() ? (dacl ? 32u : 24u) : (dacl ? 16u : 12u);
            (void)m.write_ptr(sd + field, acl);
        } else {
            updated = static_cast<u16>(updated & ~present_bit);
        }
        (void)m.write_u16(sd + 2, updated);
        m.set_return(1);
    };
    exports.entries.push_back({"SetSecurityDescriptorDacl", 20,
        [set_acl](Marshaler& m) { set_acl(m, true); }, 16});
    exports.entries.push_back({"SetSecurityDescriptorSacl", 0,
        [set_acl](Marshaler& m) { set_acl(m, false); }, 16});

    // BOOL GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR, LPBOOL present,
    //   PACL*, LPBOOL defaulted) — 16. Probe gsds_*: revision != 1 ->
    //   ERROR_UNKNOWN_REVISION with outputs untouched; *pSacl is written only
    //   when present (a present NULL SACL reports TRUE + NULL); *defaulted
    //   reflects SE_SACL_DEFAULTED. x64 absolute Sacl is at +24.
    exports.entries.push_back({"GetSecurityDescriptorSacl", 0, [](Marshaler& m) {
        const GuestAddr sd = m.arg_ptr(0);
        const GuestAddr present_out = m.arg_ptr(1);
        const GuestAddr acl_out = m.arg_ptr(2);
        const GuestAddr defaulted_out = m.arg_ptr(3);
        const auto revision = m.read_u8(sd);
        const auto control = m.read_u16(sd + 2);
        if (sd == 0 || revision.is_err() || control.is_err()) {
            return finish(m, error_codes::ERROR_NOACCESS);
        }
        if (revision.value() != 1) return finish(m, kUnknownRevision);
        const bool present = (control.value() & kSeSaclPresent) != 0;
        if (present_out != 0) (void)m.write_u32(present_out, present ? 1u : 0u);
        if (present) {
            const SdPointers p = sd_pointers(m, sd, control.value());
            if (acl_out != 0) (void)m.write_ptr(acl_out, p.sacl);
        }
        if (defaulted_out != 0) {
            (void)m.write_u32(defaulted_out, (control.value() & kSeSaclDefaulted) != 0 ? 1u : 0u);
        }
        m.set_return(1);
    }, 16});
}

} // namespace vitreo::winapi
