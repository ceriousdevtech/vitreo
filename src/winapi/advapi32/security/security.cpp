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

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <initializer_list>
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

namespace vitreo::winapi {

// =============================================================================
// Static constants
// =============================================================================

static std::wstring to_wide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

constexpr const char* kSecurityDescriptorXattr =
    "com.vitreo.security_descriptor";

// SID and ACE builders shared with security_registration.cpp.
//
// Formerly an anonymous namespace. Its contents have external linkage now
// because the export table calls them; a namespace here would grant
// internal linkage regardless of any `static`. Verified against the rest
// of src/winapi for name collisions before promoting.

// Forward declarations: the grant-DACL builder above was moved up out of the
// anonymous namespace and now precedes the SID builders it calls.
static std::vector<u8> make_builtin_sid(u32 rid);
static std::vector<u8> make_interactive_sid();
static std::vector<u8> make_allowed_ace(const std::vector<u8>& sid,
                                        u32 access_mask, u8 ace_flags);

constexpr u8 kAclRevision = 2;
constexpr u8 kAclRevisionDs = 4;

static bool validate_ace_list(std::span<const u8> ace_list,
                              u8 revision,
                              u16& ace_count) {
    if (ace_list.empty() ||
        (revision != kAclRevision && revision != kAclRevisionDs)) {
        return false;
    }

    u32 offset = 0;
    u32 count = 0;
    while (offset < ace_list.size()) {
        if (ace_list.size() - offset < 4) {
            return false;
        }
        const u8 ace_type = ace_list[offset];
        const u16 ace_size = static_cast<u16>(
            ace_list[offset + 2] |
            (static_cast<u16>(ace_list[offset + 3]) << 8));
        if (ace_size < 4 || (ace_size & 3u) != 0 ||
            offset + ace_size > ace_list.size()) {
            return false;
        }

        // ACCESS_*_OBJECT_ACE and SYSTEM_AUDIT_OBJECT_ACE require
        // ACL_REVISION_DS. The remaining currently-defined ACE layouts are
        // valid in a revision-2 ACL.
        if (revision != kAclRevisionDs && ace_type >= 5 && ace_type <= 8) {
            return false;
        }
        offset += ace_size;
        ++count;
        if (count > std::numeric_limits<u16>::max()) {
            return false;
        }
    }

    if (offset != ace_list.size()) {
        return false;
    }
    ace_count = static_cast<u16>(count);
    return true;
}



bool parse_acl(Marshaler& m, GuestAddr acl, ParsedAcl& parsed) {
    const auto revision = m.read_u8(acl);
    const auto size = m.read_u16(acl + 2);
    const auto count = m.read_u16(acl + 4);
    if (revision.is_err() || size.is_err() || count.is_err() ||
        (revision.value() != kAclRevision &&
         revision.value() != kAclRevisionDs) ||
        size.value() < 8) {
        return false;
    }

    parsed.revision = revision.value();
    parsed.size = size.value();
    parsed.ace_count = count.value();
    parsed.ace_offsets.clear();
    parsed.ace_offsets.reserve(parsed.ace_count);

    u32 offset = 8;
    for (u32 i = 0; i < parsed.ace_count; ++i) {
        const auto ace_size = m.read_u16(acl + offset + 2);
        if (ace_size.is_err() || ace_size.value() < 4 ||
            (ace_size.value() & 3u) != 0 ||
            offset + ace_size.value() > parsed.size) {
            return false;
        }
        parsed.ace_offsets.push_back(offset);
        offset += ace_size.value();
    }
    parsed.bytes_in_use = offset;
    return true;
}

bool sd_has_empty_present_dacl(const std::vector<u8>& descriptor) {
    if (descriptor.size() < 20) return false;
    const u16 control = static_cast<u16>(
        descriptor[2] | (static_cast<u16>(descriptor[3]) << 8));
    constexpr u16 SE_DACL_PRESENT = 0x0004;
    if ((control & SE_DACL_PRESENT) == 0) return false;
    const u32 dacl_off = sd_u32_at(descriptor, 16);
    if (dacl_off == 0 || dacl_off + 8 > descriptor.size()) return false;
    const u16 ace_count = static_cast<u16>(
        descriptor[dacl_off + 4] |
        (static_cast<u16>(descriptor[dacl_off + 5]) << 8));
    return ace_count == 0;
}

std::vector<u8> make_default_grant_dacl_bytes() {
    constexpr u32 FILE_ALL_ACCESS = 0x001F01FFu;
    constexpr u8 OI_CI = 0x03; // OBJECT_INHERIT | CONTAINER_INHERIT
    const auto ace0 =
        make_allowed_ace(make_builtin_sid(544), FILE_ALL_ACCESS, OI_CI);
    const auto ace1 =
        make_allowed_ace(make_builtin_sid(545), FILE_ALL_ACCESS, OI_CI);
    const auto ace2 =
        make_allowed_ace(make_interactive_sid(), FILE_ALL_ACCESS, OI_CI);
    const u16 acl_size = static_cast<u16>(8 + ace0.size() + ace1.size() +
                                          ace2.size());
    std::vector<u8> acl(acl_size, 0);
    acl[0] = 2; // ACL_REVISION
    acl[2] = static_cast<u8>(acl_size);
    acl[3] = static_cast<u8>(acl_size >> 8);
    acl[4] = 3; // AceCount
    usize off = 8;
    for (const auto& ace : {ace0, ace1, ace2}) {
        std::memcpy(acl.data() + off, ace.data(), ace.size());
        off += ace.size();
    }
    return acl;
}

// Declared in security_internal.hpp — these must stay out of the anonymous
// namespace above, which would give them internal linkage and leave the
// export table's references unresolved.

bool add_ace_bytes(Marshaler& m, GuestAddr acl,
                          u32 revision, u32 starting_index,
                          std::span<const u8> ace_list,
                          u32& error) {
    ParsedAcl parsed;
    u16 added_count = 0;
    if (acl == 0 || !parse_acl(m, acl, parsed) ||
        revision != parsed.revision ||
        !validate_ace_list(ace_list, static_cast<u8>(revision),
                           added_count) ||
        static_cast<u32>(parsed.ace_count) + added_count >
            std::numeric_limits<u16>::max()) {
        error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }

    if (parsed.bytes_in_use + ace_list.size() > parsed.size) {
        error = error_codes::ERROR_INSUFFICIENT_BUFFER;
        return false;
    }

    const u32 index =
        starting_index == kAppendAce || starting_index >= parsed.ace_count
            ? parsed.ace_count
            : starting_index;
    const u32 insert_offset =
        index == parsed.ace_count ? parsed.bytes_in_use
                                  : parsed.ace_offsets[index];
    const u32 tail_size = parsed.bytes_in_use - insert_offset;

    std::vector<u8> tail(tail_size);
    if (tail_size != 0 &&
        m.memory().read_bytes(acl + insert_offset, tail.data(),
                              tail.size()).is_err()) {
        error = error_codes::ERROR_NOACCESS;
        return false;
    }
    if (tail_size != 0 &&
        m.memory().write_bytes(acl + insert_offset + ace_list.size(),
                               tail.data(), tail.size()).is_err()) {
        error = error_codes::ERROR_NOACCESS;
        return false;
    }
    if (m.memory().write_bytes(acl + insert_offset, ace_list.data(),
                               ace_list.size()).is_err() ||
        m.write_u16(acl + 4,
                    static_cast<u16>(parsed.ace_count + added_count)).is_err()) {
        error = error_codes::ERROR_NOACCESS;
        return false;
    }

    error = error_codes::ERROR_SUCCESS;
    return true;
}

void apply_generic_mapping(u32& access_mask,
                                  u32 generic_read,
                                  u32 generic_write,
                                  u32 generic_execute,
                                  u32 generic_all) {
    constexpr u32 GENERIC_READ_BIT = 0x80000000u;
    constexpr u32 GENERIC_WRITE_BIT = 0x40000000u;
    constexpr u32 GENERIC_EXECUTE_BIT = 0x20000000u;
    constexpr u32 GENERIC_ALL_BIT = 0x10000000u;
    if ((access_mask & GENERIC_READ_BIT) != 0) {
        access_mask |= generic_read;
    }
    if ((access_mask & GENERIC_WRITE_BIT) != 0) {
        access_mask |= generic_write;
    }
    if ((access_mask & GENERIC_EXECUTE_BIT) != 0) {
        access_mask |= generic_execute;
    }
    if ((access_mask & GENERIC_ALL_BIT) != 0) {
        access_mask |= generic_all;
    }
    access_mask &= ~(GENERIC_READ_BIT | GENERIC_WRITE_BIT | GENERIC_EXECUTE_BIT |
                     GENERIC_ALL_BIT);
}

std::vector<u8> ensure_sd_has_owner_group(std::vector<u8> descriptor) {
    if (descriptor.size() < 20) {
        return make_default_null_dacl_sd(true, true, true);
    }
    const u32 owner_off =
        static_cast<u32>(descriptor[4]) |
        (static_cast<u32>(descriptor[5]) << 8) |
        (static_cast<u32>(descriptor[6]) << 16) |
        (static_cast<u32>(descriptor[7]) << 24);
    const u32 group_off =
        static_cast<u32>(descriptor[8]) |
        (static_cast<u32>(descriptor[9]) << 8) |
        (static_cast<u32>(descriptor[10]) << 16) |
        (static_cast<u32>(descriptor[11]) << 24);
    if (owner_off != 0 && group_off != 0) {
        return descriptor;
    }

    const auto& sid = SecurityStubs::synthetic_sid();
    const u16 control = static_cast<u16>(
        descriptor[2] | (static_cast<u16>(descriptor[3]) << 8));
    const u32 sacl_off =
        static_cast<u32>(descriptor[12]) |
        (static_cast<u32>(descriptor[13]) << 8) |
        (static_cast<u32>(descriptor[14]) << 16) |
        (static_cast<u32>(descriptor[15]) << 24);
    const u32 dacl_off =
        static_cast<u32>(descriptor[16]) |
        (static_cast<u32>(descriptor[17]) << 8) |
        (static_cast<u32>(descriptor[18]) << 16) |
        (static_cast<u32>(descriptor[19]) << 24);

    auto extract = [&](u32 offset) -> std::vector<u8> {
        if (offset == 0 || offset >= descriptor.size()) {
            return {};
        }
        // ACL: size at +2; SID: 8 + 4*SubAuthorityCount
        if (offset + 8 <= descriptor.size() &&
            (offset == sacl_off || offset == dacl_off)) {
            const u16 acl_size = static_cast<u16>(
                descriptor[offset + 2] |
                (static_cast<u16>(descriptor[offset + 3]) << 8));
            if (acl_size >= 8 && offset + acl_size <= descriptor.size()) {
                return {descriptor.begin() + static_cast<std::ptrdiff_t>(offset),
                        descriptor.begin() +
                            static_cast<std::ptrdiff_t>(offset + acl_size)};
            }
        }
        if (offset + 2 <= descriptor.size()) {
            const u8 sub = descriptor[offset + 1];
            const usize sid_size = 8 + 4 * static_cast<usize>(sub);
            if (offset + sid_size <= descriptor.size()) {
                return {descriptor.begin() + static_cast<std::ptrdiff_t>(offset),
                        descriptor.begin() +
                            static_cast<std::ptrdiff_t>(offset + sid_size)};
            }
        }
        return {};
    };

    std::vector<u8> owner =
        owner_off != 0 ? extract(owner_off) : std::vector<u8>(sid);
    std::vector<u8> group =
        group_off != 0 ? extract(group_off) : std::vector<u8>(sid);
    if (owner.empty()) owner = sid;
    if (group.empty()) group = sid;
    std::vector<u8> sacl = extract(sacl_off);
    std::vector<u8> dacl = extract(dacl_off);

    std::vector<u8> out(20, 0);
    out[0] = 1;
    out[2] = static_cast<u8>(control | 0x8000);
    out[3] = static_cast<u8>((control | 0x8000) >> 8);
    auto append = [&](const std::vector<u8>& bytes, usize field) {
        if (bytes.empty()) return;
        while ((out.size() & 3u) != 0) out.push_back(0);
        const u32 offset = static_cast<u32>(out.size());
        for (u32 i = 0; i < 4; ++i) {
            out[field + i] = static_cast<u8>(offset >> (i * 8));
        }
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    append(owner, 4);
    append(group, 8);
    append(sacl, 12);
    append(dacl, 16);
    return out;
}

bool is_registry_object_name(const std::string& path) {
    const std::string upper = normalize_registry_object_name(path);
    return upper.rfind("MACHINE\\", 0) == 0 ||
           upper.rfind("CURRENT_USER\\", 0) == 0 ||
           upper.rfind("CLASSES_ROOT\\", 0) == 0 ||
           upper.rfind("USERS\\", 0) == 0 ||
           upper.rfind("LOCAL_MACHINE\\", 0) == 0 ||
           upper.rfind("HKEY_", 0) == 0;
}

namespace {

/// Well-known accounts LookupAccountSid/LookupAccountName resolve without a
/// directory. Names, domains and SID_NAME_USE values are the ones Windows 10
/// returns (tools/oracle_probe/advapi_token_probe.expected, "las" lines).
struct WellKnownAccount {
    u8 authority;
    std::vector<u32> subs;
    const wchar_t* name;
    const wchar_t* domain;
    u32 use;
};

constexpr u32 kSidTypeUser = 1;
constexpr u32 kSidTypeAlias = 4;
constexpr u32 kSidTypeWellKnownGroup = 5;
constexpr u32 kSidTypeLabel = 10;
constexpr u32 kSidTypeLogonSession = 11;

const std::vector<WellKnownAccount>& well_known_accounts() {
    static const std::vector<WellKnownAccount> table = {
        {0, {0}, L"NULL SID", L"", kSidTypeWellKnownGroup},
        {1, {0}, L"Everyone", L"", kSidTypeWellKnownGroup},
        {2, {0}, L"LOCAL", L"", kSidTypeWellKnownGroup},
        {2, {1}, L"CONSOLE LOGON", L"", kSidTypeWellKnownGroup},
        {3, {0}, L"CREATOR OWNER", L"", kSidTypeWellKnownGroup},
        {3, {1}, L"CREATOR GROUP", L"", kSidTypeWellKnownGroup},
        {3, {4}, L"OWNER RIGHTS", L"", kSidTypeWellKnownGroup},
        {5, {2}, L"NETWORK", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {3}, L"BATCH", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {4}, L"INTERACTIVE", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {6}, L"SERVICE", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {7}, L"ANONYMOUS LOGON", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {11}, L"Authenticated Users", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {14}, L"REMOTE INTERACTIVE LOGON", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {15}, L"This Organization", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {18}, L"SYSTEM", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {19}, L"LOCAL SERVICE", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {20}, L"NETWORK SERVICE", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {113}, L"Local account", L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {114}, L"Local account and member of Administrators group",
         L"NT AUTHORITY", kSidTypeWellKnownGroup},
        {5, {32, 544}, L"Administrators", L"BUILTIN", kSidTypeAlias},
        {5, {32, 545}, L"Users", L"BUILTIN", kSidTypeAlias},
        {5, {32, 546}, L"Guests", L"BUILTIN", kSidTypeAlias},
        {5, {32, 547}, L"Power Users", L"BUILTIN", kSidTypeAlias},
        {5, {32, 555}, L"Remote Desktop Users", L"BUILTIN", kSidTypeAlias},
        {15, {2, 1}, L"ALL APPLICATION PACKAGES",
         L"APPLICATION PACKAGE AUTHORITY", kSidTypeWellKnownGroup},
        {15, {2, 2}, L"ALL RESTRICTED APPLICATION PACKAGES",
         L"APPLICATION PACKAGE AUTHORITY", kSidTypeWellKnownGroup},
        {16, {4096}, L"Low Mandatory Level", L"Mandatory Label", kSidTypeLabel},
        {16, {8192}, L"Medium Mandatory Level", L"Mandatory Label", kSidTypeLabel},
        {16, {12288}, L"High Mandatory Level", L"Mandatory Label", kSidTypeLabel},
        {16, {16384}, L"System Mandatory Level", L"Mandatory Label", kSidTypeLabel},
    };
    return table;
}

std::vector<u8> encode_sid(u8 authority, const std::vector<u32>& subs) {
    std::vector<u8> bytes(8 + subs.size() * 4, 0);
    bytes[0] = 1;
    bytes[1] = static_cast<u8>(subs.size());
    bytes[7] = authority;
    usize index = 0;
    for (u32 sub : subs) {
        std::memcpy(bytes.data() + 8 + index * 4, &sub, 4);
        ++index;
    }
    return bytes;
}

bool ascii_ieq(std::string_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (usize i = 0; i < lhs.size(); ++i) {
        if (rhs[i] > 0x7F ||
            std::tolower(static_cast<unsigned char>(lhs[i])) !=
                std::tolower(static_cast<int>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool sid_from_account_name(std::string_view name, std::vector<u8>& sid_out) {
    // "DOMAIN\account" must name the account's own domain; a bare name is
    // looked up across the well-known table. SDDL aliases ("BA", "SY") are
    // not account names.
    std::string_view account = name;
    std::string_view domain;
    bool has_domain = false;
    if (const usize slash = account.find('\\'); slash != std::string_view::npos) {
        domain = account.substr(0, slash);
        account = account.substr(slash + 1);
        has_domain = true;
    }
    if (account.empty()) return false;
    for (const auto& entry : well_known_accounts()) {
        if (!ascii_ieq(account, entry.name)) continue;
        if (has_domain && !ascii_ieq(domain, entry.domain)) continue;
        sid_out = encode_sid(entry.authority, entry.subs);
        return true;
    }
    // TRUSTEE_W documents CURRENT_USER as a special trustee name; VITREO\
    // VitUser is the synthetic account itself.
    if ((!has_domain && ascii_ieq(account, L"CURRENT_USER")) ||
        (ascii_ieq(account, L"VitUser") &&
         // "." names the local computer, whose account domain is VITREO.
         (!has_domain || ascii_ieq(domain, L"VITREO") || domain == "."))) {
        sid_out = SecurityStubs::synthetic_sid();
        return true;
    }
    return false;
}

bool account_from_sid(std::span<const u8> sid, std::wstring& name_out,
                      std::wstring& domain_out, u32& use_out) {
    auto matches = [&](const std::vector<u8>& other) {
        return sid.size() == other.size() &&
               std::equal(sid.begin(), sid.end(), other.begin());
    };
    if (matches(SecurityStubs::synthetic_sid())) {
        name_out = L"VitUser";
        domain_out = L"VITREO";
        use_out = kSidTypeUser;
        return true;
    }
    for (const auto& entry : well_known_accounts()) {
        if (matches(encode_sid(entry.authority, entry.subs))) {
            name_out = entry.name;
            domain_out = entry.domain;
            use_out = entry.use;
            return true;
        }
    }
    // Logon SIDs S-1-5-5-X-Y: "LogonSessionId_X_Y" (probe S-1-5-5-0-12345).
    if (sid.size() == 20 && sid[1] == 3 && sid[7] == 5 &&
        std::equal(sid.begin(), sid.begin() + 7, encode_sid(5, {5, 0, 0}).begin())) {
        u32 x = 0, y = 0, rid0 = 0;
        std::memcpy(&rid0, sid.data() + 8, 4);
        if (rid0 == 5) {
            std::memcpy(&x, sid.data() + 12, 4);
            std::memcpy(&y, sid.data() + 16, 4);
            name_out = L"LogonSessionId_" + std::to_wstring(x) + L"_" +
                       std::to_wstring(y);
            domain_out = L"NT AUTHORITY";
            use_out = kSidTypeLogonSession;
            return true;
        }
    }
    return false;
}

bool load_security_descriptor(capsule::Vfs& vfs,
                                     const std::string& path,
                                     std::vector<u8>& descriptor,
                                     bool& found,
                                     u32& error) {
    found = false;
    auto host_path = vfs.resolve_host_path(path);
    if (host_path.is_err()) {
        error = error_codes::ERROR_FILE_NOT_FOUND;
        return false;
    }
#ifdef __APPLE__
    errno = 0;
    const ssize_t size = ::getxattr(host_path.value().c_str(),
                                    kSecurityDescriptorXattr,
                                    nullptr, 0, 0, 0);
    if (size < 0) {
        if (errno == ENOATTR) {
            return true;
        }
        error = errno == EACCES ? error_codes::ERROR_ACCESS_DENIED
                                : error_codes::ERROR_NOT_SUPPORTED;
        return false;
    }
    // A SECURITY_DESCRIPTOR is at least the 20-byte header. Shorter xattrs are
    // leftover partial writes; treat them as "no descriptor" so a directory
    // grant is not failed with ERROR_INVALID_DATA.
    if (size < 20) {
        return true;
    }
    // A sandbox grant stores a DACL that can land a few bytes over 64 KiB.
    // Reject only values no real descriptor reaches.
    if (size > (1 << 20)) {
        error = error_codes::ERROR_INVALID_DATA;
        return false;
    }
    descriptor.resize(static_cast<usize>(size));
    const ssize_t read_size = ::getxattr(host_path.value().c_str(),
                                         kSecurityDescriptorXattr,
                                         descriptor.data(), descriptor.size(),
                                         0, 0);
    if (read_size != size) {
        error = error_codes::ERROR_READ_FAULT;
        return false;
    }
    found = true;
    return true;
#else
    (void)descriptor;
    error = error_codes::ERROR_NOT_SUPPORTED;
    return false;
#endif
}

std::vector<u8> make_default_null_dacl_sd(bool include_owner,
                                                 bool include_group,
                                                 bool dacl_present) {
    const auto& sid = SecurityStubs::synthetic_sid();
    const u32 owner_offset = include_owner ? 20u : 0u;
    const u32 group_offset = include_group
        ? 20u + (include_owner ? static_cast<u32>(sid.size()) : 0u)
        : 0u;
    const u32 size = 20u
        + (include_owner ? static_cast<u32>(sid.size()) : 0u)
        + (include_group ? static_cast<u32>(sid.size()) : 0u);
    std::vector<u8> descriptor(size, 0);
    descriptor[0] = 1; // Revision
    u16 control = 0x8000; // SE_SELF_RELATIVE
    if (dacl_present) {
        control |= 0x0004; // SE_DACL_PRESENT, Dacl offset stays 0 → NULL DACL
    }
    descriptor[2] = static_cast<u8>(control);
    descriptor[3] = static_cast<u8>(control >> 8);
    for (u32 i = 0; i < 4; ++i) {
        descriptor[4 + i] = static_cast<u8>(owner_offset >> (i * 8));
        descriptor[8 + i] = static_cast<u8>(group_offset >> (i * 8));
    }
    if (include_owner) {
        std::memcpy(descriptor.data() + owner_offset, sid.data(), sid.size());
    }
    if (include_group) {
        std::memcpy(descriptor.data() + group_offset, sid.data(), sid.size());
    }
    return descriptor;
}

NamedRegistrySdStore& named_registry_sds() {
    static auto* store = new NamedRegistrySdStore();
    return *store;
}

std::string normalize_registry_object_name(std::string path) {
    for (char& c : path) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return path;
}

std::vector<u8> repair_empty_present_dacl(std::vector<u8> descriptor) {
    if (!sd_has_empty_present_dacl(descriptor)) {
        return descriptor;
    }
    const auto& owner_sid = SecurityStubs::synthetic_sid();
    const u32 owner_off = sd_u32_at(descriptor, 4);
    const u32 group_off = sd_u32_at(descriptor, 8);
    const u32 sacl_off = sd_u32_at(descriptor, 12);
    auto extract = [&](u32 offset, bool is_acl) -> std::vector<u8> {
        if (offset == 0 || offset >= descriptor.size()) return {};
        if (is_acl) {
            if (offset + 4 > descriptor.size()) return {};
            const u16 sz = static_cast<u16>(
                descriptor[offset + 2] |
                (static_cast<u16>(descriptor[offset + 3]) << 8));
            if (sz < 8 || offset + sz > descriptor.size()) return {};
            return {descriptor.begin() + static_cast<std::ptrdiff_t>(offset),
                    descriptor.begin() +
                        static_cast<std::ptrdiff_t>(offset + sz)};
        }
        if (offset + 2 > descriptor.size()) return {};
        const u8 sub = descriptor[offset + 1];
        const usize sz = 8 + 4 * static_cast<usize>(sub);
        if (offset + sz > descriptor.size()) return {};
        return {descriptor.begin() + static_cast<std::ptrdiff_t>(offset),
                descriptor.begin() + static_cast<std::ptrdiff_t>(offset + sz)};
    };
    std::vector<u8> owner =
        owner_off != 0 ? extract(owner_off, false) : std::vector<u8>(owner_sid);
    std::vector<u8> group =
        group_off != 0 ? extract(group_off, false) : std::vector<u8>(owner_sid);
    if (owner.empty()) owner = owner_sid;
    if (group.empty()) group = owner_sid;
    std::vector<u8> sacl = extract(sacl_off, true);
    std::vector<u8> dacl = make_default_grant_dacl_bytes();

    const u16 control = static_cast<u16>(
        descriptor[2] | (static_cast<u16>(descriptor[3]) << 8));
    std::vector<u8> out(20, 0);
    out[0] = 1;
    out[2] = static_cast<u8>(control | 0x8000 | 0x0004);
    out[3] = static_cast<u8>((control | 0x8000 | 0x0004) >> 8);
    auto append = [&](const std::vector<u8>& bytes, usize field) {
        if (bytes.empty()) return;
        while ((out.size() & 3u) != 0) out.push_back(0);
        const u32 offset = static_cast<u32>(out.size());
        sd_write_u32_at(out, field, offset);
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    append(owner, 4);
    append(group, 8);
    append(sacl, 12);
    append(dacl, 16);
    return out;
}

u32 sd_u32_at(const std::vector<u8>& descriptor, usize field) {
    if (field + 4 > descriptor.size()) return 0;
    return static_cast<u32>(descriptor[field]) |
           (static_cast<u32>(descriptor[field + 1]) << 8) |
           (static_cast<u32>(descriptor[field + 2]) << 16) |
           (static_cast<u32>(descriptor[field + 3]) << 24);
}

void sd_write_u32_at(std::vector<u8>& descriptor, usize field, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        descriptor[field + i] = static_cast<u8>(value >> (i * 8));
    }
}

bool store_security_descriptor(capsule::Vfs& vfs,
                                      const std::string& path,
                                      const std::vector<u8>& descriptor,
                                      u32& error) {
    auto host_path = vfs.resolve_host_path(path);
    if (host_path.is_err()) {
        error = error_codes::ERROR_FILE_NOT_FOUND;
        return false;
    }
#ifdef __APPLE__
    if (descriptor.size() < 20) {
        error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }
    if (::setxattr(host_path.value().c_str(), kSecurityDescriptorXattr,
                   descriptor.data(), descriptor.size(), 0, 0) == 0) {
        return true;
    }
    error = errno == EACCES ? error_codes::ERROR_ACCESS_DENIED
                            : error_codes::ERROR_NOT_SUPPORTED;
    return false;
#else
    (void)descriptor;
    error = error_codes::ERROR_NOT_SUPPORTED;
    return false;
#endif
}


/// Normalize a registry object name for the named-SD map (case-insensitive).

/// True when `path` looks like a Get/SetNamedSecurityInfo registry object name
/// (MACHINE\…, CURRENT_USER\…, CLASSES_ROOT\…, …).

/// Process-local SD store for named registry keys (no capsule hive SD store).
/// Capsule-default self-relative SD: synthetic owner/group + present NULL DACL
/// (grant-all). AccessCheck requires owner and group SIDs (Microsoft Learn
/// securitybaseapi!AccessCheck → ERROR_INVALID_SECURITY_DESCR otherwise).

/// Well-known SIDs for an install-directory ACL
/// (Administrators / Users / INTERACTIVE, FILE_ALL_ACCESS, OI|CI).
static std::vector<u8> make_builtin_sid(u32 rid) {
    // S-1-5-32-<rid>
    return {
        0x01, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x20, 0x00, 0x00, 0x00,
        static_cast<u8>(rid), static_cast<u8>(rid >> 8),
        static_cast<u8>(rid >> 16), static_cast<u8>(rid >> 24),
    };
}

static std::vector<u8> make_interactive_sid() {
    // S-1-5-4
    return {
        0x01, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x04, 0x00, 0x00, 0x00,
    };
}

static std::vector<u8> make_allowed_ace(const std::vector<u8>& sid,
                                        u32 access_mask,
                                        u8 inheritance) {
    std::vector<u8> ace(8 + sid.size(), 0);
    ace[0] = 0; // ACCESS_ALLOWED_ACE_TYPE
    ace[1] = inheritance;
    const u16 size = static_cast<u16>(ace.size());
    ace[2] = static_cast<u8>(size);
    ace[3] = static_cast<u8>(size >> 8);
    for (u32 i = 0; i < 4; ++i) {
        ace[4 + i] = static_cast<u8>(access_mask >> (i * 8));
    }
    std::memcpy(ace.data() + 8, sid.data(), sid.size());
    return ace;
}

/// Default file ACL matching Agent's successful install-dir descriptor:
/// BA/BU/IU with FILE_ALL_ACCESS and OBJECT_INHERIT|CONTAINER_INHERIT.



/// True when SE_DACL_PRESENT and Dacl points at an AceCount==0 ACL (deny-all).
/// Agent marks builds non-playable (error 2113) when install dirs have this.

/// Rebuild SD replacing an empty present-DACL with Agent-expected grant ACEs.

/// Map GENERIC_* bits using GENERIC_MAPPING (Microsoft Learn MapGenericMask).

/// Ensure a self-relative SD has owner and group SIDs. AccessCheck fails with
/// ERROR_INVALID_SECURITY_DESCR (1338) without both (Microsoft Learn). Stored
/// capsule xattrs from SetNamedSecurityInfoW(DACL-only) historically omitted
/// them; Agent then fails Win32Permissions on those paths.


static std::string hex_encode(std::span<const u8> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

static std::optional<std::vector<u8>> hex_decode(std::string_view encoded) {
    if (encoded.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<u8> decoded;
    decoded.reserve(encoded.size() / 2);
    for (usize index = 0; index < encoded.size(); index += 2) {
        const int high = nibble(encoded[index]);
        const int low = nibble(encoded[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<u8>((high << 4) | low));
    }
    return decoded;
}

const std::string  SecurityStubs::USERNAME_A = "VitUser";
const std::wstring SecurityStubs::USERNAME   = L"VitUser";
const std::wstring SecurityStubs::DOMAIN_NAME = L"VITREO";

// =============================================================================
// Constructor
// =============================================================================

SecurityStubs::SecurityStubs(HandleTable& handles,
                             GlobalLocalManager* local_memory,
                             const GuestElevationState* elevation)
    : m_handles(handles)
    , m_local_memory(local_memory)
    , m_elevation(elevation) {
    m_primary_token_state.elevated = is_elevated();
    // A new process token supplies an ACL, not an empty deny-all ACL.
    // Windows sync_security_probe: owner/system GENERIC_ALL and logon
    // GENERIC_READ|GENERIC_EXECUTE. Capsule token import below may replace it.
    {
        using namespace sync_access;
        const std::vector<u8> admins{1,2,0,0,0,0,0,5,32,0,0,0,32,2,0,0};
        const std::vector<u8> system{1,1,0,0,0,0,0,5,18,0,0,0};
        const std::vector<u8> logon{1,3,0,0,0,0,0,5,5,0,0,0,0,0,0,0,1,0,0,0};
        auto& acl=m_primary_token_state.default_dacl;
        acl.assign(8,0);acl[0]=2;acl[4]=3;
        auto append=[&](const std::vector<u8>& sid,u32 mask){
            const usize start=acl.size();const usize size=8+sid.size();acl.resize(start+size);
            acl[start]=static_cast<u8>(ACCESS_ALLOWED_ACE_TYPE);
            acl[start+2]=static_cast<u8>(size);
            std::memcpy(acl.data()+start+4,&mask,4);
            std::memcpy(acl.data()+start+8,sid.data(),sid.size());
        };
        append(is_elevated()?admins:synthetic_sid(),GENERIC_ALL);
        append(system,GENERIC_ALL);append(logon,GENERIC_READ|GENERIC_EXECUTE);
        acl[2]=static_cast<u8>(acl.size());acl[3]=static_cast<u8>(acl.size()>>8);
    }
    if (const char* value = std::getenv("VITREO_TOKEN_RESTRICTED")) {
        m_primary_token_state.has_restrictions =
            std::strcmp(value, "1") == 0;
    }
    if (const char* value = std::getenv("VITREO_TOKEN_SANDBOX_INERT")) {
        m_primary_token_state.sandbox_inert =
            std::strcmp(value, "1") == 0;
    }
    if (const char* value = std::getenv("VITREO_TOKEN_INTEGRITY_RID");
        value != nullptr && value[0] != '\0') {
        char* end = nullptr;
        const unsigned long rid = std::strtoul(value, &end, 16);
        if (end != value && end != nullptr && *end == '\0' &&
            rid <= std::numeric_limits<u32>::max()) {
            m_primary_token_state.integrity_rid = static_cast<u32>(rid);
        }
    }
    if (const char* value = std::getenv("VITREO_TOKEN_PRIVILEGES");
        value != nullptr) {
        std::vector<TokenPrivilege> privileges;
        std::stringstream stream(value);
        std::string entry;
        bool valid = true;
        while (std::getline(stream, entry, ',')) {
            if (entry.empty()) continue;
            const usize separator = entry.find('@');
            if (separator == std::string::npos) {
                valid = false;
                break;
            }
            char* luid_end = nullptr;
            char* attributes_end = nullptr;
            const std::string luid_text = entry.substr(0, separator);
            const std::string attributes_text = entry.substr(separator + 1);
            const unsigned long long luid =
                std::strtoull(luid_text.c_str(), &luid_end, 16);
            const unsigned long attributes =
                std::strtoul(attributes_text.c_str(), &attributes_end, 16);
            if (luid_end == nullptr || *luid_end != '\0' ||
                attributes_end == nullptr || *attributes_end != '\0' ||
                attributes > std::numeric_limits<u32>::max()) {
                valid = false;
                break;
            }
            privileges.push_back(
                {static_cast<u64>(luid), static_cast<u32>(attributes)});
        }
        if (valid) m_primary_token_state.privileges = std::move(privileges);
    }
    if (const char* value = std::getenv("VITREO_TOKEN_DEFAULT_DACL");
        value != nullptr && value[0] != '\0') {
        auto decoded = hex_decode(value);
        if (decoded.has_value() && decoded->size() >= 8) {
            m_primary_token_state.default_dacl = std::move(*decoded);
        }
    }
    auto parse_sid_list = [](const char* value,
                             std::vector<RestrictedSid>& out) -> bool {
        std::stringstream stream(value);
        std::string entry;
        std::vector<RestrictedSid> sids;
        while (std::getline(stream, entry, ';')) {
            if (entry.empty()) continue;
            const usize separator = entry.find('@');
            if (separator == std::string::npos) return false;
            char* attributes_end = nullptr;
            const std::string attributes_text = entry.substr(0, separator);
            const unsigned long attributes =
                std::strtoul(attributes_text.c_str(), &attributes_end, 16);
            auto sid = hex_decode(entry.substr(separator + 1));
            if (attributes_end == nullptr || *attributes_end != '\0' ||
                attributes > std::numeric_limits<u32>::max() ||
                !sid.has_value()) {
                return false;
            }
            sids.push_back(
                {std::move(*sid), static_cast<u32>(attributes)});
        }
        out = std::move(sids);
        return true;
    };
    if (const char* value = std::getenv("VITREO_TOKEN_RESTRICTING_SIDS");
        value != nullptr) {
        std::vector<RestrictedSid> sids;
        if (parse_sid_list(value, sids)) {
            m_primary_token_state.restricting_sids = std::move(sids);
        }
    }
    // CreateRestrictedToken(SidsToDisable) — the child primary token must
    // see the same deny-only groups the parent prepared.
    if (const char* value = std::getenv("VITREO_TOKEN_DISABLED_SIDS");
        value != nullptr) {
        std::vector<RestrictedSid> sids;
        if (parse_sid_list(value, sids)) {
            m_primary_token_state.disabled_sids = std::move(sids);
        }
    }
}

// =============================================================================
// GetUserNameW
// =============================================================================

bool SecurityStubs::get_user_name_w(std::wstring& name_out, u32& size_inout) {
    u32 needed = static_cast<u32>(USERNAME.size() + 1); // include null
    if (size_inout < needed) {
        size_inout = needed;
        return false;
    }
    name_out = USERNAME;
    size_inout = needed;
    return true;
}

// =============================================================================
// GetUserNameA
// =============================================================================

bool SecurityStubs::get_user_name_a(std::string& name_out, u32& size_inout) {
    u32 needed = static_cast<u32>(USERNAME_A.size() + 1);
    if (size_inout < needed) {
        size_inout = needed;
        return false;
    }
    name_out = USERNAME_A;
    size_inout = needed;
    return true;
}


// =============================================================================
// DuplicateToken / DuplicateTokenEx
// =============================================================================

bool SecurityStubs::duplicate_token(Handle existing, u32 impersonation_level,
                                    Handle& token_out, u32 token_type,
                                    u32 access) {
    token_out = 0;
    if (!is_token_handle(existing)) return false;
    // SECURITY_IMPERSONATION_LEVEL: SecurityAnonymous(0), SecurityIdentification(1),
    // SecurityImpersonation(2), SecurityDelegation(3). Anything else is invalid.
    if (impersonation_level > 3) return false;
    if (token_type != 1 && token_type != 2) return false;
    TokenState state = token_state(existing);
    state.type = token_type;
    state.impersonation_level = impersonation_level;
    if (access == 0) {
        access = m_handles.get_type(existing) == HandleType::Token
            ? m_handles.get_access(existing)
            : HandleAccess::All;
    }
    token_out = m_handles.allocate(
        HandleType::Token, nullptr, 0, access);
    if (token_out == 0) return false;
    {
        std::lock_guard lock(m_token_mutex);
        state.token_id = m_next_token_luid++;
        state.modified_id = m_next_token_luid++;
        m_token_states[token_out] =
            std::make_shared<TokenState>(std::move(state));
    }
    return true;
}

u32 SecurityStubs::duplicate_token_ex(Handle existing, u32 access, u32 level,
                                      u32 token_type, Handle& token_out) {
    constexpr u32 ERROR_BAD_IMPERSONATION_LEVEL = 1346;
    constexpr u32 ERROR_PRIVILEGE_NOT_HELD = 1314;
    constexpr u32 TOKEN_DUPLICATE = 0x0002;
    constexpr u32 ACCESS_SYSTEM_SECURITY = 0x01000000;
    constexpr u64 kSeSecurityPrivilege = 8;
    token_out = 0;
    // NtDuplicateToken validates its parameters before it references the
    // handle (probe dte_level4 / dte_type0 / dte_primary_level9).
    if (level > 3) return ERROR_BAD_IMPERSONATION_LEVEL;
    if (token_type != 1 && token_type != 2) return error_codes::ERROR_INVALID_PARAMETER;
    // The -4/-5/-6 pseudo handles are not accepted (dte_pseudo_process_token).
    if (m_handles.get_type(existing) != HandleType::Token) {
        return error_codes::ERROR_INVALID_HANDLE;
    }
    if (!token_has_access(existing, TOKEN_DUPLICATE)) return error_codes::ERROR_ACCESS_DENIED;
    TokenState state = token_state(existing);
    if (state.type == 2) {
        // An impersonation token cannot be raised (dte_ident_to_impers,
        // dte_imp_to_deleg), nor become primary below SecurityImpersonation
        // (dte_ident_to_primary).
        if (token_type == 2 && level > state.impersonation_level) {
            return ERROR_BAD_IMPERSONATION_LEVEL;
        }
        if (token_type == 1 && state.impersonation_level < 2) {
            return ERROR_BAD_IMPERSONATION_LEVEL;
        }
    }
    u32 granted = 0;
    if (access == 0) {
        granted = m_handles.get_access(existing);   // dte_zero: the source's access
    } else {
        // Bits a token cannot hold are dropped, not refused (dte_bogus_bits).
        granted = (map_token_access(access) | (access & ACCESS_SYSTEM_SECURITY)) &
                  kTokenValidAccess;
        if ((granted & ACCESS_SYSTEM_SECURITY) != 0) {
            // Needs SeSecurityPrivilege enabled in the *caller's* effective
            // token, not the source's (probe dte_sacl_priv_removed succeeds
            // from a source whose privilege was removed).
            bool impersonating = false;
            const TokenIdentity caller = effective_identity(impersonating);
            bool enabled = false;
            for (const auto& p : caller.privileges) {
                enabled = enabled || (p.luid == kSeSecurityPrivilege && (p.attributes & 0x2u) != 0);
            }
            if (!enabled) return ERROR_PRIVILEGE_NOT_HELD;
        }
    }
    state.type = token_type;
    if (token_type == 2) state.impersonation_level = level;
    token_out = m_handles.allocate(HandleType::Token, nullptr, 0, granted);
    if (token_out == 0) return error_codes::ERROR_NOT_ENOUGH_MEMORY;
    std::lock_guard lock(m_token_mutex);
    state.token_id = m_next_token_luid++;
    state.modified_id = m_next_token_luid++;
    m_token_states[token_out] = std::make_shared<TokenState>(std::move(state));
    return error_codes::ERROR_SUCCESS;
}

SecurityStubs::TokenIdentity SecurityStubs::token_identity(Handle token) {
    TokenIdentity id;
    const TokenState state = token_state(token);
    std::vector<u8> blob;
    u32 length = 0;
    if (get_token_information(token, 1, blob, length, false) && blob.size() >= 8) {
        u32 off = 0;
        std::memcpy(&off, blob.data(), 4);
        if (off + 8 <= blob.size()) {
            const usize size = 8u + 4u * blob[off + 1];
            if (off + size <= blob.size()) id.user.assign(blob.begin() + off, blob.begin() + off + size);
        }
    }
    for (const auto& disabled : state.disabled_sids) {
        if (disabled.bytes == id.user) id.user_deny_only = true;
    }
    blob.clear();
    if (get_token_information(token, 2, blob, length, false) && blob.size() >= 4) {
        u32 count = 0;
        std::memcpy(&count, blob.data(), 4);
        for (u32 i = 0; i < count && 4 + i * 8 + 8 <= blob.size(); ++i) {
            u32 off = 0, attrs = 0;
            std::memcpy(&off, blob.data() + 4 + i * 8, 4);
            std::memcpy(&attrs, blob.data() + 4 + i * 8 + 4, 4);
            if (off + 8 > blob.size()) continue;
            const usize size = 8u + 4u * blob[off + 1];
            if (off + size > blob.size()) continue;
            id.groups.push_back({{blob.begin() + off, blob.begin() + off + size}, attrs});
        }
    }
    for (const auto& r : state.restricting_sids) id.restricting.push_back(r.bytes);
    id.privileges = state.privileges;
    id.integrity_rid = state.integrity_rid.value_or(state.elevated ? 0x3000u : 0x2000u);
    id.type = state.type;
    id.impersonation_level = state.impersonation_level;
    return id;
}

SecurityStubs::TokenIdentity SecurityStubs::effective_identity(bool& impersonating) {
    {
        std::lock_guard lock(m_token_mutex);
        impersonating = m_thread_token_states.contains(thread_token_key(0xFFFFFFFEu));
    }
    if (!impersonating) {
        TokenIdentity id = token_identity(0xFFFFFFFCu);
        id.type = 2;                 // "duplicates the thread's primary token"
        id.impersonation_level = 2;  // (Learn CheckTokenMembership)
        return id;
    }
    // token_state() resolves -6 to the process token; take the thread's own.
    std::shared_ptr<TokenState> object;
    {
        std::lock_guard lock(m_token_mutex);
        object = m_thread_token_states[thread_token_key(0xFFFFFFFEu)];
    }
    // A scratch handle lets token_identity reuse the GetTokenInformation
    // layout; it never escapes to the guest.
    const Handle scratch = m_handles.allocate(HandleType::Token, nullptr);
    {
        std::lock_guard lock(m_token_mutex);
        m_token_states[scratch] = object;
    }
    TokenIdentity id = token_identity(scratch);
    {
        std::lock_guard lock(m_token_mutex);
        m_token_states.erase(scratch);
    }
    (void)m_handles.close(scratch);
    return id;
}

u32 SecurityStubs::check_token_membership(Handle token, std::span<const u8> sid,
                                          bool& member) {
    constexpr u32 ERROR_NO_IMPERSONATION_TOKEN = 1309;
    constexpr u32 ERROR_BAD_IMPERSONATION_LEVEL = 1346;
    constexpr u32 TOKEN_QUERY = 0x0008;
    constexpr Handle kProcessPseudo = 0xFFFFFFFCu, kThreadPseudo = 0xFFFFFFFBu,
                     kEffectivePseudo = 0xFFFFFFFAu;
    member = false;
    TokenIdentity id;
    if (token == 0 || token == kProcessPseudo || token == kEffectivePseudo ||
        token == kThreadPseudo) {
        bool impersonating = false;
        id = effective_identity(impersonating);
        // -5 names the thread's own token (probe ctm_pseudo_thread).
        if (token == kThreadPseudo && !impersonating) return error_codes::ERROR_NO_TOKEN;
    } else {
        if (m_handles.get_type(token) != HandleType::Token) return error_codes::ERROR_INVALID_HANDLE;
        if (!token_has_access(token, TOKEN_QUERY)) return error_codes::ERROR_ACCESS_DENIED;
        id = token_identity(token);
        if (id.type != 2) return ERROR_NO_IMPERSONATION_TOKEN;
    }
    if (id.impersonation_level == 0) return ERROR_BAD_IMPERSONATION_LEVEL;

    constexpr u32 SE_GROUP_ENABLED = 0x4, SE_GROUP_USE_FOR_DENY_ONLY = 0x10;
    const std::vector<u8> wanted(sid.begin(), sid.end());
    bool present = !id.user_deny_only && id.user == wanted;
    for (const auto& g : id.groups) {
        if (g.sid == wanted && (g.attributes & SE_GROUP_ENABLED) != 0 &&
            (g.attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) {
            present = true;
        }
    }
    if (present && !id.restricting.empty()) {
        present = std::find(id.restricting.begin(), id.restricting.end(), wanted) !=
                  id.restricting.end();
    }
    member = present;
    return error_codes::ERROR_SUCCESS;
}

bool SecurityStubs::duplicate_token_handle(
    Handle existing, u32 access, u32 flags, Handle& token_out) {
    token_out = 0;
    if (!is_token_handle(existing)) return false;

    std::shared_ptr<TokenState> state;
    {
        std::lock_guard lock(m_token_mutex);
        const auto found = m_token_states.find(existing);
        if (found == m_token_states.end()) return false;
        state = found->second;
    }

    const u32 source_access = m_handles.get_access(existing);
    if (source_access != HandleAccess::All &&
        (access & ~source_access) != 0) {
        return false;
    }
    token_out = m_handles.allocate(
        HandleType::Token, nullptr, flags, access);
    if (token_out == 0) return false;
    {
        std::lock_guard lock(m_token_mutex);
        m_token_states[token_out] = std::move(state);
    }
    return true;
}

bool SecurityStubs::create_restricted_token(
    Handle existing, u32 flags,
    std::vector<RestrictedSid> disabled_sids,
    std::vector<u64> deleted_privileges,
    std::vector<RestrictedSid> restricting_sids,
    Handle& token_out) {
    constexpr u32 kKnownFlags = 0x0Fu;
    token_out = 0;
    if (!is_token_handle(existing) || (flags & ~kKnownFlags) != 0) {
        return false;
    }

    TokenState state = token_state(existing);
    state.has_restrictions =
        state.has_restrictions || !disabled_sids.empty() ||
        !deleted_privileges.empty() || !restricting_sids.empty();
    state.sandbox_inert = state.sandbox_inert || (flags & 0x02u) != 0;
    if ((flags & 0x01u) != 0) { // DISABLE_MAX_PRIVILEGE
        std::erase_if(state.privileges, [](const TokenPrivilege& privilege) {
            return privilege.luid != 23u; // SeChangeNotifyPrivilege survives
        });
    } else if (!deleted_privileges.empty()) {
        std::erase_if(
            state.privileges,
            [&deleted_privileges](const TokenPrivilege& privilege) {
                return std::find(
                    deleted_privileges.begin(), deleted_privileges.end(),
                    privilege.luid) != deleted_privileges.end();
            });
    }
    if (!disabled_sids.empty()) {
        state.disabled_sids.insert(
            state.disabled_sids.end(),
            std::make_move_iterator(disabled_sids.begin()),
            std::make_move_iterator(disabled_sids.end()));
    }
    if (!restricting_sids.empty()) {
        state.restricting_sids.insert(
            state.restricting_sids.end(),
            std::make_move_iterator(restricting_sids.begin()),
            std::make_move_iterator(restricting_sids.end()));
    }

    token_out = m_handles.allocate(HandleType::Token, nullptr);
    if (token_out == 0) return false;
    {
        std::lock_guard lock(m_token_mutex);
        state.token_id = m_next_token_luid++;
        state.modified_id = m_next_token_luid++;
        m_token_states[token_out] =
            std::make_shared<TokenState>(std::move(state));
    }
    return true;
}

bool SecurityStubs::set_token_default_dacl(
    Handle token, std::vector<u8> acl) {
    if (!is_token_handle(token) || acl.size() < 8) return false;
    std::lock_guard lock(m_token_mutex);
    const auto found = m_token_states.find(token);
    if (found == m_token_states.end()) return false;
    found->second->default_dacl = std::move(acl);
    return true;
}

bool SecurityStubs::set_token_integrity_level(
    Handle token, u32 integrity_rid) {
    if (!is_token_handle(token)) return false;
    std::lock_guard lock(m_token_mutex);
    const auto found = m_token_states.find(token);
    if (found == m_token_states.end()) return false;
    found->second->integrity_rid = integrity_rid;
    return true;
}

u32 SecurityStubs::token_type(Handle token) const {
    return is_token_handle(token) ? token_state(token).type : 0;
}

bool SecurityStubs::token_has_restrictions(Handle token) const {
    return is_token_handle(token) && token_state(token).has_restrictions;
}

bool SecurityStubs::token_is_restricted(Handle token) const {
    // Learn IsTokenRestricted: only restricting SIDs count — disabled SIDs or
    // deleted privileges alone leave the token unrestricted.
    return is_token_handle(token) &&
           !token_state(token).restricting_sids.empty();
}

bool SecurityStubs::set_token_security_attributes(
    Handle token, std::vector<SecurityAttribute> attributes,
    bool replace_all) {
    if (!is_token_handle(token)) return false;
    std::lock_guard lock(m_token_mutex);
    TokenState* state = nullptr;
    if (m_handles.get_type(token) == HandleType::Token) {
        const auto found = m_token_states.find(token);
        if (found == m_token_states.end()) return false;
        state = found->second.get();
    } else {
        state = primary_token_object_locked().get();
    }
    if (replace_all) {
        state->security_attributes = std::move(attributes);
        return true;
    }
    for (auto& incoming : attributes) {
        bool replaced = false;
        for (auto& existing : state->security_attributes) {
            if (existing.name == incoming.name) {
                existing = std::move(incoming);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            state->security_attributes.push_back(std::move(incoming));
        }
    }
    return true;
}

named_sd::Principal SecurityStubs::principal() {
    named_sd::Principal who;
    who.user = synthetic_sid();
    const std::vector<u8> users{1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 0x02, 0, 0};
    const std::vector<u8> admins{1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 0x02, 0, 0};
    // SAM aliases the account belongs to (TokenGroups' BUILTIN entries).
    who.alias_groups.push_back(users);
    if (is_elevated()) {
        who.alias_groups.push_back(admins);
        who.owner_groups.push_back(admins); // SE_GROUP_OWNER in TokenGroups
    }
    std::lock_guard lock(m_token_mutex);
    std::shared_ptr<TokenState> state;
    if (const auto found = m_thread_token_states.find(thread_token_key(0xFFFFFFFEu));
        found != m_thread_token_states.end()) {
        state = found->second;
    } else {
        state = primary_token_object_locked();
    }
    for (const auto& privilege : state->privileges) {
        if (privilege.luid == 8u) who.has_security_privilege = true;  // SeSecurityPrivilege
        if (privilege.luid == 18u) who.has_restore_privilege = true;  // SeRestorePrivilege
    }
    return who;
}

bool SecurityStubs::privilege_enabled(Handle token, u64 luid) const {
    const TokenState state = token_state(token);
    for (const auto& privilege : state.privileges) {
        if (privilege.luid == luid) return (privilege.attributes & 0x2u) != 0;
    }
    return false;
}

bool SecurityStubs::is_real_token_handle(Handle token) const {
    return m_handles.get_type(token) == HandleType::Token;
}

bool SecurityStubs::open_linked_token(Handle token, Handle& linked_out,
                                      u32& error) {
    constexpr u32 TOKEN_ALL_ACCESS = 0x000F01FFu;
    if (!duplicate_token(token, 1 /*SecurityIdentification*/, linked_out, 2,
                         TOKEN_ALL_ACCESS)) {
        error = error_codes::ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    std::lock_guard lock(m_token_mutex);
    if (auto found = m_token_states.find(linked_out); found != m_token_states.end()) {
        found->second->elevated = false; // the limited half of the split token
    }
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool SecurityStubs::token_has_access(Handle token, u32 access) const {
    if (m_handles.get_type(token) != HandleType::Token) return false;
    const u32 granted = m_handles.get_access(token);
    return granted == HandleAccess::All || (access & ~granted) == 0;
}

std::vector<std::string>
SecurityStubs::token_launch_environment(Handle token) const {
    if (!is_token_handle(token)) return {};
    const TokenState state = token_state(token);
    std::vector<std::string> environment;
    environment.emplace_back(
        state.has_restrictions ? "VITREO_TOKEN_RESTRICTED=1"
                               : "VITREO_TOKEN_RESTRICTED=0");
    environment.emplace_back(
        state.sandbox_inert ? "VITREO_TOKEN_SANDBOX_INERT=1"
                            : "VITREO_TOKEN_SANDBOX_INERT=0");
    if (state.integrity_rid.has_value()) {
        char value[16]{};
        std::snprintf(value, sizeof(value), "%x", *state.integrity_rid);
        environment.emplace_back(
            std::string("VITREO_TOKEN_INTEGRITY_RID=") + value);
    } else {
        environment.emplace_back("VITREO_TOKEN_INTEGRITY_RID=");
    }

    std::ostringstream privileges;
    bool first = true;
    for (const auto& privilege : state.privileges) {
        if (!first) privileges << ',';
        first = false;
        privileges << std::hex << privilege.luid << '@'
                   << privilege.attributes;
    }
    environment.emplace_back(
        "VITREO_TOKEN_PRIVILEGES=" + privileges.str());
    environment.emplace_back(
        "VITREO_TOKEN_DEFAULT_DACL=" +
        hex_encode(state.default_dacl));

    auto encode_sid_list = [](const std::vector<RestrictedSid>& sids) {
        std::ostringstream encoded;
        bool first_sid = true;
        for (const auto& sid : sids) {
            if (!first_sid) encoded << ';';
            first_sid = false;
            encoded << std::hex << sid.attributes << '@'
                    << hex_encode(sid.bytes);
        }
        return encoded.str();
    };
    environment.emplace_back(
        "VITREO_TOKEN_RESTRICTING_SIDS=" +
        encode_sid_list(state.restricting_sids));
    environment.emplace_back(
        "VITREO_TOKEN_DISABLED_SIDS=" +
        encode_sid_list(state.disabled_sids));
    return environment;
}



// =============================================================================
// LookupPrivilegeValueW
// =============================================================================

namespace {
// Every SE_*_NAME in winnt.h 10.0.26100.0 that LookupPrivilegeValue resolves,
// with its LUID (low part; high part 0), transcribed from
// tools/oracle_probe/advapi_wksid_probe.expected "lpv" lines.
// SeUnsolicitedInputPrivilege is defined in winnt.h but not known to LSA
// (ERROR_NO_SUCH_PRIVILEGE), so it is absent.
constexpr std::pair<const char*, u32> kPrivilegeLuids[] = {
    {"SeCreateTokenPrivilege", 2}, {"SeAssignPrimaryTokenPrivilege", 3},
    {"SeLockMemoryPrivilege", 4}, {"SeIncreaseQuotaPrivilege", 5},
    {"SeMachineAccountPrivilege", 6}, {"SeTcbPrivilege", 7}, {"SeSecurityPrivilege", 8},
    {"SeTakeOwnershipPrivilege", 9}, {"SeLoadDriverPrivilege", 10},
    {"SeSystemProfilePrivilege", 11}, {"SeSystemtimePrivilege", 12},
    {"SeProfileSingleProcessPrivilege", 13}, {"SeIncreaseBasePriorityPrivilege", 14},
    {"SeCreatePagefilePrivilege", 15}, {"SeCreatePermanentPrivilege", 16},
    {"SeBackupPrivilege", 17}, {"SeRestorePrivilege", 18}, {"SeShutdownPrivilege", 19},
    {"SeDebugPrivilege", 20}, {"SeAuditPrivilege", 21},
    {"SeSystemEnvironmentPrivilege", 22}, {"SeChangeNotifyPrivilege", 23},
    {"SeRemoteShutdownPrivilege", 24}, {"SeUndockPrivilege", 25},
    {"SeSyncAgentPrivilege", 26}, {"SeEnableDelegationPrivilege", 27},
    {"SeManageVolumePrivilege", 28}, {"SeImpersonatePrivilege", 29},
    {"SeCreateGlobalPrivilege", 30}, {"SeTrustedCredManAccessPrivilege", 31},
    {"SeRelabelPrivilege", 32}, {"SeIncreaseWorkingSetPrivilege", 33},
    {"SeTimeZonePrivilege", 34}, {"SeCreateSymbolicLinkPrivilege", 35},
    {"SeDelegateSessionUserImpersonatePrivilege", 36},
};
} // namespace

bool SecurityStubs::lookup_privilege_value_w(const std::wstring& name,
                                             u64& luid_out) {
    // Case-insensitive, exact otherwise (probe: "sedebugprivilege" resolves,
    // "SeDebugPrivilege " does not).
    for (const auto& [text, luid] : kPrivilegeLuids) {
        const std::string_view want(text);
        if (want.size() != name.size()) continue;
        bool same = true;
        for (usize i = 0; i < want.size() && same; ++i) {
            const wchar_t c = name[i];
            same = c < 0x80 && std::tolower(static_cast<unsigned char>(c)) ==
                                   std::tolower(static_cast<unsigned char>(want[i]));
        }
        if (same) {
            luid_out = luid;
            return true;
        }
    }
    return false;
}

std::string SecurityStubs::privilege_name(u64 luid) {
    for (const auto& [text, value] : kPrivilegeLuids) {
        if (value == luid) return text;
    }
    return {};
}


// =============================================================================
// Synthetic SID
// =============================================================================

const std::vector<u8>& SecurityStubs::synthetic_sid() {
    // S-1-5-21-1000000000-2000000000-3000000000-1001
    // SID: revision(1) + sub_auth_count(1) + authority(6) + sub_auths(4*count)
    static const std::vector<u8> sid = {
        0x01,                                       // Revision
        0x05,                                       // SubAuthorityCount = 5
        0x00, 0x00, 0x00, 0x00, 0x00, 0x05,        // SECURITY_NT_AUTHORITY
        // SubAuthority[0] = 21 (SECURITY_NT_NON_UNIQUE)
        0x15, 0x00, 0x00, 0x00,
        // SubAuthority[1] = 1000000000
        0x00, 0xCA, 0x9A, 0x3B,
        // SubAuthority[2] = 2000000000
        0x00, 0x94, 0x35, 0x77,
        // SubAuthority[3] = 3000000000
        0x00, 0x5E, 0xD0, 0xB2,
        // SubAuthority[4] = 1001
        0xE9, 0x03, 0x00, 0x00,
    };
    return sid;
}



} // namespace vitreo::winapi
