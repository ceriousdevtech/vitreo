// SPDX-License-Identifier: LGPL-2.1-only

/// @file sddl.cpp
/// @brief SDDL parser and formatter. See sddl.hpp for the contract sources;
/// every rule below that Learn does not state is quoted from
/// tools/oracle_probe/advapi_sddl_probe.expected.

#include "winapi/advapi32/security/sddl.hpp"

#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace vitreo::winapi::sddl {

namespace {

constexpr u32 kOk = 0;
constexpr u32 kInvalidParameter = 87;        // ERROR_INVALID_PARAMETER
constexpr u32 kInvalidFlags = 1004;          // ERROR_INVALID_FLAGS
constexpr u32 kUnknownRevision = 1305;       // ERROR_UNKNOWN_REVISION
constexpr u32 kNoneMapped = 1332;            // ERROR_NONE_MAPPED
constexpr u32 kInvalidAcl = 1336;            // ERROR_INVALID_ACL
constexpr u32 kInvalidSid = 1337;            // ERROR_INVALID_SID
constexpr u32 kInvalidSecurityDescr = 1338;  // ERROR_INVALID_SECURITY_DESCR
constexpr u32 kInvalidStringUuid = 1705;     // RPC_S_INVALID_STRING_UUID
constexpr u32 kInvalidDatatype = 1804;       // ERROR_INVALID_DATATYPE
constexpr u32 kNoAccess = 998;               // ERROR_NOACCESS

// SECURITY_DESCRIPTOR_CONTROL (winnt.h).
constexpr u16 kDaclPresent = 0x0004;
constexpr u16 kSaclPresent = 0x0010;
constexpr u16 kDaclAutoInheritReq = 0x0100;
constexpr u16 kSaclAutoInheritReq = 0x0200;
constexpr u16 kDaclAutoInherited = 0x0400;
constexpr u16 kSaclAutoInherited = 0x0800;
constexpr u16 kDaclProtected = 0x1000;
constexpr u16 kSaclProtected = 0x2000;
constexpr u16 kSelfRelative = 0x8000;

// ACE types (winnt.h).
constexpr u8 kAllowed = 0x00;
constexpr u8 kDenied = 0x01;
constexpr u8 kAudit = 0x02;
constexpr u8 kAlarm = 0x03;
constexpr u8 kAllowedObject = 0x05;
constexpr u8 kDeniedObject = 0x06;
constexpr u8 kAuditObject = 0x07;
constexpr u8 kAlarmObject = 0x08;
constexpr u8 kAllowedCallback = 0x09;
constexpr u8 kDeniedCallback = 0x0A;
constexpr u8 kAllowedCallbackObject = 0x0B;
constexpr u8 kAuditCallback = 0x0D;
constexpr u8 kMandatoryLabel = 0x11;
constexpr u8 kResourceAttribute = 0x12;
constexpr u8 kScopedPolicyId = 0x13;
constexpr u8 kProcessTrustLabel = 0x14;
constexpr u8 kAccessFilter = 0x15;

// ACE_OBJECT_TYPE_PRESENT / ACE_INHERITED_OBJECT_TYPE_PRESENT.
constexpr u32 kObjectTypePresent = 0x1;
constexpr u32 kInheritedObjectTypePresent = 0x2;

// ---------------------------------------------------------------------------
// Alias tables
// ---------------------------------------------------------------------------

/// How an SDDL SID alias resolves on a machine that is not domain joined.
enum class AliasKind : u8 {
    Fixed,          ///< a well-known SID
    MachineRid,     ///< <machine domain>-RID (LA, LG)
    DomainOnly,     ///< needs a domain: ERROR_INVALID_SID (probe "sid DA")
};

struct SidAlias {
    const char* alias;
    AliasKind kind;
    const char* sid;   ///< Fixed: standard notation
    u32 rid;           ///< MachineRid
};

// Every SID alias in sddl.h, in header order, with the SID Windows maps it to
// (probe section "sid"). Domain-relative aliases fail with ERROR_INVALID_SID
// on a workgroup machine, which is what a capsule is.
constexpr std::array<SidAlias, 68> kSidAliases{{
    {"DA", AliasKind::DomainOnly, nullptr, 512},
    {"DG", AliasKind::DomainOnly, nullptr, 514},
    {"DU", AliasKind::DomainOnly, nullptr, 513},
    {"ED", AliasKind::Fixed, "S-1-5-9", 0},
    {"DD", AliasKind::DomainOnly, nullptr, 516},
    {"DC", AliasKind::DomainOnly, nullptr, 515},
    {"BA", AliasKind::Fixed, "S-1-5-32-544", 0},
    {"BG", AliasKind::Fixed, "S-1-5-32-546", 0},
    {"BU", AliasKind::Fixed, "S-1-5-32-545", 0},
    {"LA", AliasKind::MachineRid, nullptr, 500},
    {"LG", AliasKind::MachineRid, nullptr, 501},
    {"AO", AliasKind::Fixed, "S-1-5-32-548", 0},
    {"BO", AliasKind::Fixed, "S-1-5-32-551", 0},
    {"PO", AliasKind::Fixed, "S-1-5-32-550", 0},
    {"SO", AliasKind::Fixed, "S-1-5-32-549", 0},
    {"AU", AliasKind::Fixed, "S-1-5-11", 0},
    {"PS", AliasKind::Fixed, "S-1-5-10", 0},
    {"CO", AliasKind::Fixed, "S-1-3-0", 0},
    {"CG", AliasKind::Fixed, "S-1-3-1", 0},
    {"SY", AliasKind::Fixed, "S-1-5-18", 0},
    {"PU", AliasKind::Fixed, "S-1-5-32-547", 0},
    {"WD", AliasKind::Fixed, "S-1-1-0", 0},
    {"RE", AliasKind::Fixed, "S-1-5-32-552", 0},
    {"IU", AliasKind::Fixed, "S-1-5-4", 0},
    {"NU", AliasKind::Fixed, "S-1-5-2", 0},
    {"SU", AliasKind::Fixed, "S-1-5-6", 0},
    {"RC", AliasKind::Fixed, "S-1-5-12", 0},
    {"WR", AliasKind::Fixed, "S-1-5-33", 0},
    {"AN", AliasKind::Fixed, "S-1-5-7", 0},
    {"SA", AliasKind::DomainOnly, nullptr, 518},
    {"CA", AliasKind::DomainOnly, nullptr, 517},
    {"RS", AliasKind::DomainOnly, nullptr, 553},
    {"EA", AliasKind::DomainOnly, nullptr, 519},
    {"PA", AliasKind::DomainOnly, nullptr, 520},
    {"RU", AliasKind::Fixed, "S-1-5-32-554", 0},
    {"LS", AliasKind::Fixed, "S-1-5-19", 0},
    {"NS", AliasKind::Fixed, "S-1-5-20", 0},
    {"RD", AliasKind::Fixed, "S-1-5-32-555", 0},
    {"NO", AliasKind::Fixed, "S-1-5-32-556", 0},
    {"MU", AliasKind::Fixed, "S-1-5-32-558", 0},
    {"LU", AliasKind::Fixed, "S-1-5-32-559", 0},
    {"IS", AliasKind::Fixed, "S-1-5-32-568", 0},
    {"CY", AliasKind::Fixed, "S-1-5-32-569", 0},
    {"OW", AliasKind::Fixed, "S-1-3-4", 0},
    {"ER", AliasKind::Fixed, "S-1-5-32-573", 0},
    {"RO", AliasKind::DomainOnly, nullptr, 498},
    {"CD", AliasKind::Fixed, "S-1-5-32-574", 0},
    {"AC", AliasKind::Fixed, "S-1-15-2-1", 0},
    {"RA", AliasKind::Fixed, "S-1-5-32-575", 0},
    {"ES", AliasKind::Fixed, "S-1-5-32-576", 0},
    {"MS", AliasKind::Fixed, "S-1-5-32-577", 0},
    {"UD", AliasKind::Fixed, "S-1-5-84-0-0-0-0-0", 0},
    {"HA", AliasKind::Fixed, "S-1-5-32-578", 0},
    {"CN", AliasKind::DomainOnly, nullptr, 522},
    {"AA", AliasKind::Fixed, "S-1-5-32-579", 0},
    {"RM", AliasKind::Fixed, "S-1-5-32-580", 0},
    {"AS", AliasKind::Fixed, "S-1-18-1", 0},
    {"SS", AliasKind::Fixed, "S-1-18-2", 0},
    {"AP", AliasKind::DomainOnly, nullptr, 525},
    {"KA", AliasKind::DomainOnly, nullptr, 526},
    {"EK", AliasKind::DomainOnly, nullptr, 527},
    {"HO", AliasKind::Fixed, "S-1-5-32-584", 0},
    {"SH", AliasKind::Fixed, "S-1-5-32-585", 0},
    {"LW", AliasKind::Fixed, "S-1-16-4096", 0},
    {"ME", AliasKind::Fixed, "S-1-16-8192", 0},
    {"MP", AliasKind::Fixed, "S-1-16-8448", 0},
    {"HI", AliasKind::Fixed, "S-1-16-12288", 0},
    {"SI", AliasKind::Fixed, "S-1-16-16384", 0},
}};

struct NamedMask {
    const char* name;
    u32 mask;
};

// Aggregate rights, tried first when formatting (probe "mask"): an exact
// match prints the aggregate. KX equals KR, so KR wins.
constexpr std::array<NamedMask, 8> kAggregateRights{{
    {"FA", 0x001F01FF}, {"FR", 0x00120089}, {"FW", 0x00120116}, {"FX", 0x001200A0},
    {"KA", 0x000F003F}, {"KR", 0x00020019}, {"KW", 0x00020006}, {"KX", 0x00020019},
}};

// Single-bit rights in the order Windows prints them (ascending bit).
constexpr std::array<NamedMask, 17> kBitRights{{
    {"CC", 0x00000001}, {"DC", 0x00000002}, {"LC", 0x00000004}, {"SW", 0x00000008},
    {"RP", 0x00000010}, {"WP", 0x00000020}, {"DT", 0x00000040}, {"LO", 0x00000080},
    {"CR", 0x00000100}, {"SD", 0x00010000}, {"RC", 0x00020000}, {"WD", 0x00040000},
    {"WO", 0x00080000}, {"GA", 0x10000000}, {"GX", 0x20000000}, {"GW", 0x40000000},
    {"GR", 0x80000000},
}};

// Mandatory-label rights (SYSTEM_MANDATORY_LABEL_NO_*), valid only in ML.
constexpr std::array<NamedMask, 3> kLabelRights{{
    {"NW", 0x1}, {"NR", 0x2}, {"NX", 0x4},
}};

// ACE flags. TP and CR are rejected by the parser on the measured build
// (ERROR_INVALID_FLAGS) but CRITICAL_ACE_FLAG still prints as "CR".
constexpr std::array<NamedMask, 8> kAceFlags{{
    {"OI", 0x01}, {"CI", 0x02}, {"NP", 0x04}, {"IO", 0x08},
    {"ID", 0x10}, {"CR", 0x20}, {"SA", 0x40}, {"FA", 0x80},
}};

enum class AceSyntax : u8 {
    Plain,        ///< mask + SID
    Object,       ///< mask + flags + GUIDs + SID
    Conditional,  ///< needs a trailing condition expression
    Attribute,    ///< needs a trailing resource attribute
    Unsupported,  ///< recognised but refused (ERROR_INVALID_PARAMETER)
};

struct AceTypeInfo {
    const char* name;
    u8 type;
    AceSyntax syntax;
    bool audit;   ///< SA / FA flags allowed
};

// ACE types a DACL string may contain, and those a SACL string may contain.
// The type is matched as a prefix of the text, per table, so "AU" in a DACL
// matches "A" and then fails for want of ';' (probe "D:(AU;;GA;;;WD)").
constexpr std::array<AceTypeInfo, 7> kDaclTypes{{
    {"A", kAllowed, AceSyntax::Plain, false},
    {"D", kDenied, AceSyntax::Plain, false},
    {"OA", kAllowedObject, AceSyntax::Object, false},
    {"OD", kDeniedObject, AceSyntax::Object, false},
    {"XA", kAllowedCallback, AceSyntax::Conditional, false},
    {"XD", kDeniedCallback, AceSyntax::Conditional, false},
    {"ZA", kAllowedCallbackObject, AceSyntax::Conditional, false},
}};
constexpr std::array<AceTypeInfo, 10> kSaclTypes{{
    {"AU", kAudit, AceSyntax::Plain, true},
    {"AL", kAlarm, AceSyntax::Unsupported, true},
    {"OU", kAuditObject, AceSyntax::Object, true},
    {"OL", kAlarmObject, AceSyntax::Unsupported, true},
    {"ML", kMandatoryLabel, AceSyntax::Plain, false},
    {"XU", kAuditCallback, AceSyntax::Conditional, true},
    {"RA", kResourceAttribute, AceSyntax::Attribute, false},
    {"SP", kScopedPolicyId, AceSyntax::Plain, false},
    {"TL", kProcessTrustLabel, AceSyntax::Plain, false},
    {"FL", kAccessFilter, AceSyntax::Conditional, false},
}};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[nodiscard]] char upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] bool ieq2(std::string_view s, usize at, const char* two) noexcept {
    return at + 1 < s.size() && upper(s[at]) == two[0] && upper(s[at + 1]) == two[1];
}

[[nodiscard]] bool starts_with_ci(std::string_view s, usize at, std::string_view word) noexcept {
    if (at + word.size() > s.size()) return false;
    for (usize i = 0; i < word.size(); ++i) {
        if (upper(s[at + i]) != word[i]) return false;
    }
    return true;
}

// Whitespace the parser skips: the space character only. Tab, CR and LF are
// ordinary characters (probe "D:\t(A;;GA;;;WD)", "D:(A;;GA;;;WD)\r\n(...)"
// -> ERROR_INVALID_DATATYPE).
[[nodiscard]] bool is_space(char c) noexcept { return c == ' '; }

void skip_space(std::string_view s, usize& p, usize end) noexcept {
    while (p < end && is_space(s[p])) ++p;
}

void put_u32(std::vector<u8>& v, usize at, u32 x) {
    v[at] = static_cast<u8>(x);
    v[at + 1] = static_cast<u8>(x >> 8);
    v[at + 2] = static_cast<u8>(x >> 16);
    v[at + 3] = static_cast<u8>(x >> 24);
}

void push_u16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x));
    v.push_back(static_cast<u8>(x >> 8));
}

void push_u32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (i * 8)));
}

[[nodiscard]] u32 get_u32(std::span<const u8> v, usize at) noexcept {
    return static_cast<u32>(v[at]) | (static_cast<u32>(v[at + 1]) << 8) |
           (static_cast<u32>(v[at + 2]) << 16) | (static_cast<u32>(v[at + 3]) << 24);
}

[[nodiscard]] u16 get_u16(std::span<const u8> v, usize at) noexcept {
    return static_cast<u16>(v[at] | (v[at + 1] << 8));
}

[[nodiscard]] bool sid_is_valid(std::span<const u8> sid) noexcept {
    return sid.size() >= 8 && sid[0] == 1 && sid[1] <= 15 &&
           sid.size() >= 8u + sid[1] * 4u;
}

[[nodiscard]] u64 sid_authority(std::span<const u8> sid) noexcept {
    u64 a = 0;
    for (usize i = 0; i < 6; ++i) a = (a << 8) | sid[2 + i];
    return a;
}

/// The capsule's machine domain (S-1-5-21-x-y-z), from the synthetic user SID.
[[nodiscard]] std::vector<u8> machine_rid_sid(u32 rid) {
    std::vector<u8> sid = SecurityStubs::synthetic_sid();
    // synthetic_sid = S-1-5-21-a-b-c-1001: replace the final RID.
    if (sid.size() >= 8 && sid[1] >= 1) {
        put_u32(sid, 8 + (sid[1] - 1u) * 4u, rid);
    }
    return sid;
}

// ---------------------------------------------------------------------------
// Numeric SID grammar (probe "sid" lines):
//   S-<rev>-<authority>-<sub>[-<sub>...], "S" either case, at least one
//   sub-authority. A number is "0x"/"0X" + hex digits or decimal digits
//   (leading zeros fine, no sign or space). rev <= 255, authority < 2^48,
//   sub-authorities saturate at 0xFFFFFFFF. A hex revision makes the
//   remaining unprefixed numbers hex ("S-0x1-5-18" is S-1-5-24). Neither the
//   revision nor the count is validated here (S-2-5-18 converts).
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_sid_number(std::string_view f, bool default_hex, u64 limit,
                                    bool saturate, u64& value, bool& was_hex) {
    usize i = 0;
    bool hex = default_hex;
    if (f.size() >= 2 && f[0] == '0' && (f[1] == 'x' || f[1] == 'X')) {
        hex = true;
        i = 2;
    }
    was_hex = hex && i == 2;
    if (i >= f.size()) return false;
    u64 v = 0;
    bool overflow = false;
    for (; i < f.size(); ++i) {
        const char c = f[i];
        u32 d;
        if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
        else if (hex && c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
        else return false;
        const u64 base = hex ? 16 : 10;
        if (v > (limit - d) / base) overflow = true;
        else v = v * base + d;
    }
    if (overflow) {
        if (!saturate) return false;
        v = limit;
    }
    value = v;
    return true;
}

[[nodiscard]] bool parse_numeric_sid(std::string_view text, std::vector<u8>& sid,
                                     bool allow_no_subauthority = false) {
    if (text.size() < 2 || upper(text[0]) != 'S' || text[1] != '-') return false;
    std::vector<std::string_view> fields;
    usize start = 2;
    while (true) {
        const usize dash = text.find('-', start);
        fields.push_back(text.substr(start, dash == std::string_view::npos
                                                ? std::string_view::npos
                                                : dash - start));
        if (dash == std::string_view::npos) break;
        start = dash + 1;
    }
    // revision, authority, and at least one sub-authority (the string forms;
    // the well-known table also holds S-1-5 itself).
    if (fields.size() < (allow_no_subauthority ? 2u : 3u) || fields.size() - 2 > 255) return false;
    u64 revision = 0, authority = 0;
    bool rev_hex = false, dummy = false;
    if (!parse_sid_number(fields[0], false, 0xFF, false, revision, rev_hex)) return false;
    if (!parse_sid_number(fields[1], rev_hex, 0xFFFFFFFFFFFFull, false, authority, dummy)) {
        return false;
    }
    const usize count = fields.size() - 2;
    sid.assign(8 + count * 4, 0);
    sid[0] = static_cast<u8>(revision);
    sid[1] = static_cast<u8>(count);
    for (usize i = 0; i < 6; ++i) sid[2 + i] = static_cast<u8>(authority >> ((5 - i) * 8));
    for (usize i = 0; i < count; ++i) {
        u64 sub = 0;
        if (!parse_sid_number(fields[i + 2], rev_hex, 0xFFFFFFFFull, true, sub, dummy)) {
            return false;
        }
        put_u32(sid, 8 + i * 4, static_cast<u32>(sub));
    }
    return true;
}

/// Look up a two-letter alias (case-insensitive). ERROR_SUCCESS or
/// ERROR_INVALID_SID.
[[nodiscard]] u32 resolve_alias(char a, char b, std::vector<u8>& sid) {
    const char want[2] = {upper(a), upper(b)};
    for (const auto& e : kSidAliases) {
        if (e.alias[0] != want[0] || e.alias[1] != want[1]) continue;
        switch (e.kind) {
            case AliasKind::Fixed:
                return parse_numeric_sid(e.sid, sid) ? kOk : kInvalidSid;
            case AliasKind::MachineRid:
                sid = machine_rid_sid(e.rid);
                return kOk;
            case AliasKind::DomainOnly:
                return kInvalidSid;
        }
    }
    return kInvalidSid;
}

// ---------------------------------------------------------------------------
// String -> descriptor
// ---------------------------------------------------------------------------

struct Guid {
    std::array<u8, 16> bytes{};
};

[[nodiscard]] bool parse_guid36(std::string_view t, Guid& g) {
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (UuidToString format)
    if (t.size() != 36 || t[8] != '-' || t[13] != '-' || t[18] != '-' || t[23] != '-') {
        return false;
    }
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto read = [&](usize at, usize digits, u64& out) -> bool {
        out = 0;
        for (usize i = 0; i < digits; ++i) {
            const int d = hexv(t[at + i]);
            if (d < 0) return false;
            out = (out << 4) | static_cast<u64>(d);
        }
        return true;
    };
    u64 d1, d2, d3, d4, d5;
    if (!read(0, 8, d1) || !read(9, 4, d2) || !read(14, 4, d3) ||
        !read(19, 4, d4) || !read(24, 12, d5)) {
        return false;
    }
    for (usize i = 0; i < 4; ++i) g.bytes[i] = static_cast<u8>(d1 >> (i * 8));
    g.bytes[4] = static_cast<u8>(d2);
    g.bytes[5] = static_cast<u8>(d2 >> 8);
    g.bytes[6] = static_cast<u8>(d3);
    g.bytes[7] = static_cast<u8>(d3 >> 8);
    g.bytes[8] = static_cast<u8>(d4 >> 8);
    g.bytes[9] = static_cast<u8>(d4);
    for (usize i = 0; i < 6; ++i) g.bytes[10 + i] = static_cast<u8>(d5 >> ((5 - i) * 8));
    return true;
}

/// Owner / group component over [b, e) (probe "O:..." lines).
[[nodiscard]] u32 parse_principal(std::string_view s, usize b, usize e,
                                  std::vector<u8>& sid) {
    if (e < b + 2) return kNoneMapped;                    // "O:", "O:B", "O: "
    if (upper(s[b]) == 'S' && s[b + 1] == '-') {
        return parse_numeric_sid(s.substr(b, e - b), sid) ? kOk : kInvalidSid;
    }
    if (const u32 err = resolve_alias(s[b], s[b + 1], sid); err != kOk) return err;
    usize q = b + 2;
    skip_space(s, q, e);
    return q < e ? kInvalidParameter : kOk;               // "O:BAx"
}

/// strtoul(base 0) as the rights field uses it: optional sign, 0x/0 prefix,
/// saturating at ULONG_MAX. Returns false when no digit was consumed.
[[nodiscard]] bool parse_rights_number(std::string_view s, usize& p, usize end, u32& out) {
    usize q = p;
    bool negative = false;
    if (q < end && (s[q] == '+' || s[q] == '-')) {
        negative = s[q] == '-';
        ++q;
    }
    u32 base = 10;
    if (q < end && s[q] == '0') {
        if (q + 2 < end && (s[q + 1] == 'x' || s[q + 1] == 'X') &&
            std::isxdigit(static_cast<unsigned char>(s[q + 2])) != 0) {
            base = 16;
            q += 2;
        } else {
            base = 8;
        }
    }
    const usize digits_start = q;
    u64 v = 0;
    bool overflow = false;
    while (q < end) {
        const char c = s[q];
        u32 d;
        if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
        else if (c >= 'a' && c <= 'z') d = static_cast<u32>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'Z') d = static_cast<u32>(c - 'A' + 10);
        else break;
        if (d >= base) break;
        v = v * base + d;
        if (v > 0xFFFFFFFFull) overflow = true, v = 0xFFFFFFFFull;
        ++q;
    }
    if (q == digits_start) {
        // "0" alone is a valid octal zero; a bare sign is not a number.
        if (base == 8) {
            out = 0;
            p = digits_start;  // consumed the '0'
            return true;
        }
        return false;
    }
    u32 r = static_cast<u32>(v);
    if (overflow) r = 0xFFFFFFFFu;
    else if (negative) r = static_cast<u32>(0u - r);
    out = r;
    p = q;
    return true;
}

struct BuiltAce {
    std::vector<u8> bytes;  ///< the ACE as written
    u32 reserved{0};        ///< bytes the ACL reserves for it (>= bytes.size())
    bool object_kind{false};
};

/// One ACE starting at p. `count_semis`/extent handling is the caller's.
[[nodiscard]] u32 parse_ace(std::string_view s, usize& p, usize end, bool dacl,
                            BuiltAce& out) {
    skip_space(s, p, end);
    if (p < end && s[p] == '(') ++p;
    skip_space(s, p, end);

    const AceTypeInfo* info = nullptr;
    auto match = [&](const auto& table) {
        for (const auto& t : table) {
            if (starts_with_ci(s.substr(0, end), p, t.name)) {
                info = &t;
                return;
            }
        }
    };
    if (dacl) match(kDaclTypes);
    else match(kSaclTypes);
    if (info == nullptr) return kInvalidDatatype;
    if (info->syntax == AceSyntax::Unsupported) return kInvalidParameter;
    p += std::strlen(info->name);
    if (p >= end || s[p] != ';') return kInvalidAcl;
    ++p;

    // ace_flags: tokens separated by optional spaces, up to ';'.
    u8 flags = 0;
    while (true) {
        skip_space(s, p, end);
        if (p >= end) return kInvalidAcl;
        if (s[p] == ';') break;
        bool found = false;
        for (const auto& f : kAceFlags) {
            if (ieq2(s.substr(0, end), p, f.name)) {
                // TP/CR are not accepted; SA/FA only on audit ACEs.
                if (f.mask == 0x20) break;
                if ((f.mask == 0x40 || f.mask == 0x80) && !info->audit) break;
                flags = static_cast<u8>(flags | f.mask);
                found = true;
                break;
            }
        }
        if (!found) return kInvalidFlags;
        p += 2;
    }
    ++p;

    // rights: numbers (strtoul base 0) and two-letter names, OR-ed. A space
    // is skipped only in front of a token (probe "D:(A;;1 ;;;WD)").
    const bool label = info->type == kMandatoryLabel;
    u32 mask = 0;
    while (p < end && s[p] != ';') {
        skip_space(s, p, end);
        if (p >= end) return kInvalidAcl;
        const char c = s[p];
        if ((c >= '0' && c <= '9') || c == '+' || c == '-') {
            u32 v = 0;
            if (!parse_rights_number(s, p, end, v)) return kInvalidAcl;
            mask |= v;
            continue;
        }
        bool found = false;
        auto try_table = [&](const auto& table) {
            for (const auto& r : table) {
                if (ieq2(s.substr(0, end), p, r.name)) {
                    mask |= r.mask;
                    found = true;
                    return;
                }
            }
        };
        if (label) {
            try_table(kLabelRights);
        } else {
            try_table(kAggregateRights);
            if (!found) try_table(kBitRights);
        }
        if (!found) return kInvalidAcl;
        p += 2;
    }
    if (p >= end) return kInvalidAcl;
    ++p;

    // object_guid; inherit_object_guid. A GUID on a non-object ACE is an
    // invalid ACL; after a GUID one character is stepped over unexamined
    // (probe: a space after the GUID shifts the SID field -> ERROR_INVALID_SID).
    const bool object = info->syntax == AceSyntax::Object ||
                        info->type == kAllowedCallbackObject;
    std::optional<Guid> guids[2];
    for (int g = 0; g < 2; ++g) {
        skip_space(s, p, end);
        if (p < end && s[p] == ';') {
            ++p;
            continue;
        }
        if (!object) return kInvalidAcl;
        usize q = p;
        while (q < end && s[q] != ';' && s[q] != ')' && !is_space(s[q])) ++q;
        Guid guid;
        if (!parse_guid36(s.substr(p, q - p), guid)) return kInvalidStringUuid;
        guids[g] = guid;
        p = q + 1;
    }

    // account_sid: text up to ')' — a ';' is part of it (probe: a space after
    // a GUID shifts the SID field onto ";WD" -> ERROR_INVALID_SID) — except
    // where a condition or resource attribute follows.
    skip_space(s, p, end);
    const bool trailer = info->syntax == AceSyntax::Conditional ||
                         info->syntax == AceSyntax::Attribute;
    usize sid_end = p;
    while (sid_end < end && s[sid_end] != ')' && !(trailer && s[sid_end] == ';')) ++sid_end;
    std::vector<u8> sid;
    if (sid_end < p + 2) return kNoneMapped;
    if (upper(s[p]) == 'S' && s[p + 1] == '-') {
        if (!parse_numeric_sid(s.substr(p, sid_end - p), sid)) return kInvalidSid;
        p = sid_end;
    } else {
        if (const u32 err = resolve_alias(s[p], s[p + 1], sid); err != kOk) return err;
        p += 2;
        skip_space(s, p, end);
    }
    if (!sid_is_valid(sid)) return kInvalidSid;   // "D:(A;;GA;;;S-2-5-18)"

    if (p < end && s[p] == ';') {
        // A condition or resource attribute follows. Plain types refuse it.
        if (info->syntax == AceSyntax::Conditional || info->syntax == AceSyntax::Attribute) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED advapi32!ConvertStringSecurityDescriptorToSecurityDescriptor: "
                    "%s ACE with a %s\n", info->name,
                    info->syntax == AceSyntax::Conditional ? "condition expression"
                                                           : "resource attribute");
            }
        }
        return kInvalidParameter;
    }
    if (p >= end || s[p] != ')') return kInvalidAcl;
    ++p;
    if (info->syntax == AceSyntax::Conditional || info->syntax == AceSyntax::Attribute) {
        return kInvalidAcl;   // "D:(XA;;FA;;;WD)": the condition is mandatory
    }

    // Type-specific rules (probe "S:(ML;;0x8;;;LW)", "S:(ML;;NW;;;WD)",
    // "S:(TL;;0x200;;;WD)", "S:(SP;;GA;;;S-1-17-1)").
    const u64 authority = sid_authority(sid);
    if (info->type == kMandatoryLabel && ((mask & ~7u) != 0 || authority != 16)) {
        return kInvalidParameter;
    }
    if (info->type == kProcessTrustLabel && authority != 19) return kInvalidParameter;
    if (info->type == kScopedPolicyId && (mask != 0 || authority != 17)) {
        return kInvalidParameter;
    }

    // Serialise. An object ACE with neither GUID becomes the plain type but
    // keeps its object-sized reservation in the ACL (Learn remark + probe).
    out.object_kind = object;
    const u32 sid_len = static_cast<u32>(sid.size());
    u8 type = info->type;
    const bool as_object = object && (guids[0] || guids[1]);
    if (object && !as_object) {
        type = type == kAllowedObject ? kAllowed
             : type == kDeniedObject ? kDenied
             : type == kAuditObject ? kAudit : type;
    }
    std::vector<u8>& b = out.bytes;
    b.clear();
    b.push_back(type);
    b.push_back(flags);
    push_u16(b, 0);  // size, patched below
    push_u32(b, mask);
    if (as_object) {
        u32 of = 0;
        if (guids[0]) of |= kObjectTypePresent;
        if (guids[1]) of |= kInheritedObjectTypePresent;
        push_u32(b, of);
        for (const auto& g : guids) {
            if (g) b.insert(b.end(), g->bytes.begin(), g->bytes.end());
        }
    }
    b.insert(b.end(), sid.begin(), sid.end());
    const u16 size = static_cast<u16>(b.size());
    b[2] = static_cast<u8>(size);
    b[3] = static_cast<u8>(size >> 8);
    out.reserved = (object && !as_object) ? 12u + sid_len : size;
    return kOk;
}

/// Count the ';' that separate ACE fields. Every ';' counts (probe
/// "D:((A;;GA;;;WD)" reaches the ACE parser), except inside a quoted string
/// and the one that introduces a condition or resource attribute, so that
/// such an ACE reaches the parser and fails loudly there.
[[nodiscard]] usize count_field_separators(std::string_view s, usize b, usize e) {
    usize n = 0;
    int depth = 0;
    bool quoted = false;
    for (usize i = b; i < e; ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) --depth;
        } else if (c == ';') {
            // Inside an ACE, a ';' followed by '(' opens the condition /
            // attribute; between ACEs it is an ordinary separator (probe
            // "D:(A;;GA;;;WD)x;;;;;(A;;GA;;;BA)" -> ERROR_INVALID_DATATYPE).
            usize k = i + 1;
            while (k < e && is_space(s[k])) ++k;
            if (!(depth >= 1 && k < e && s[k] == '(')) ++n;
        }
    }
    return n;
}

struct ParsedAclText {
    bool null_acl{false};
    u16 control{0};
    std::vector<u8> acl;
};

/// D: / S: component over [b, e).
[[nodiscard]] u32 parse_acl_component(std::string_view s, usize b, usize e, bool dacl,
                                      ParsedAclText& out) {
    usize p = b;
    // dacl_flags / sacl_flags, case-insensitive, any order, repeats allowed.
    while (true) {
        skip_space(s, p, e);
        if (p >= e) break;
        const std::string_view text = s.substr(0, e);
        if (starts_with_ci(text, p, "NO_ACCESS_CONTROL")) {
            out.null_acl = true;
            p += 17;
            skip_space(s, p, e);
            if (p != e) return kInvalidParameter;   // "D:NO_ACCESS_CONTROLx"
            break;
        }
        if (starts_with_ci(text, p, "P")) {
            out.control |= dacl ? kDaclProtected : kSaclProtected;
            p += 1;
        } else if (starts_with_ci(text, p, "AI")) {
            out.control |= dacl ? kDaclAutoInherited : kSaclAutoInherited;
            p += 2;
        } else if (starts_with_ci(text, p, "AR")) {
            out.control |= dacl ? kDaclAutoInheritReq : kSaclAutoInheritReq;
            p += 2;
        } else {
            break;
        }
    }
    if (out.null_acl) return kOk;

    const usize separators = count_field_separators(s, p, e);
    if (separators % 5 != 0) return kInvalidParameter;
    const usize count = separators / 5;
    if (count == 0) {
        // Only nothing, or a lone '(', may follow the flags ("D:(" converts,
        // "D:( " / "D:()" / "D:x" do not).
        if (p == e || (s[p] == '(' && p + 1 == e)) {
            out.acl = {2, 0, 8, 0, 0, 0, 0, 0};
            return kOk;
        }
        return kInvalidParameter;
    }

    std::vector<BuiltAce> aces(count);
    bool any_object = false;
    for (usize i = 0; i < count; ++i) {
        if (const u32 err = parse_ace(s, p, e, dacl, aces[i]); err != kOk) return err;
        any_object = any_object || aces[i].object_kind;
    }
    u32 reserved = 8;
    for (const auto& a : aces) reserved += a.reserved;
    std::vector<u8> acl;
    acl.reserve(reserved);
    acl.push_back(any_object ? 4 : 2);  // ACL_REVISION_DS when any object ACE
    acl.push_back(0);
    push_u16(acl, static_cast<u16>(reserved));
    push_u16(acl, static_cast<u16>(count));
    push_u16(acl, 0);
    for (const auto& a : aces) acl.insert(acl.end(), a.bytes.begin(), a.bytes.end());
    acl.resize(reserved, 0);
    out.acl = std::move(acl);
    return kOk;
}

// ---------------------------------------------------------------------------
// Descriptor -> string
// ---------------------------------------------------------------------------

void append_hex(std::string& out, u64 v, bool upper_case) {
    char buf[24];
    std::snprintf(buf, sizeof buf, upper_case ? "0x%llX" : "0x%llx",
                  static_cast<unsigned long long>(v));
    out += buf;
}

/// Appends the rights field. Returns how many characters short of the
/// 10-character "0x%08x" budget a hex mask came out (Windows reports the
/// budgeted length; probe "mask m=200 ... len=17").
u32 format_mask(std::string& out, u32 mask, bool label) {
    auto hex_mask = [&out](u32 v) -> u32 {
        const usize before = out.size();
        append_hex(out, v, false);
        return static_cast<u32>(10 - (out.size() - before));
    };
    if (mask == 0) return 0;
    if (label) {
        if ((mask & ~7u) != 0) return hex_mask(mask);
        for (const auto& r : kLabelRights) {
            if ((mask & r.mask) != 0) out += r.name;
        }
        return 0;
    }
    for (const auto& r : kAggregateRights) {
        if (mask == r.mask) {
            out += r.name;
            return 0;
        }
    }
    u32 covered = 0;
    for (const auto& r : kBitRights) covered |= r.mask;
    if ((mask & ~covered) != 0) return hex_mask(mask);
    for (const auto& r : kBitRights) {
        if ((mask & r.mask) != 0) out += r.name;
    }
    return 0;
}

void format_guid(std::string& out, std::span<const u8> g) {
    char buf[40];
    std::snprintf(buf, sizeof buf,
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  get_u32(g, 0), get_u16(g, 4), get_u16(g, 6), g[8], g[9], g[10], g[11],
                  g[12], g[13], g[14], g[15]);
    out += buf;
}

/// SECURITY_INFORMATION bit that selects a SACL ACE of `type`.
[[nodiscard]] u32 sacl_class(u8 type) noexcept {
    switch (type) {
        case kMandatoryLabel: return 0x10;
        case kResourceAttribute: return 0x20;
        case kScopedPolicyId: return 0x40;
        case kProcessTrustLabel: return 0x80;
        default: return 0x08;
    }
}

/// `extra` accumulates what Windows adds to the reported length: one per
/// printed ACE plus the hex-mask budget shortfall.
[[nodiscard]] u32 format_acl(std::string& out, std::span<const u8> acl, bool dacl, u32 si,
                             u32& extra) {
    if (acl.size() < 8) return kInvalidAcl;
    const u16 acl_size = get_u16(acl, 2);
    const u16 count = get_u16(acl, 4);
    usize off = 8;
    for (u16 i = 0; i < count; ++i) {
        if (off + 4 > acl.size() || off + 4 > acl_size) return kInvalidAcl;
        const u8 type = acl[off];
        const u8 flags = acl[off + 1];
        const u16 size = get_u16(acl, off + 2);
        if (size < 8 || off + size > acl.size()) return kInvalidAcl;
        const std::span<const u8> ace = acl.subspan(off, size);
        off += size;

        if (!dacl) {
            // A SACL ACE is printed only when its class is requested.
            switch (type) {
                case kAudit: case kAlarm: case kAuditObject: case kAlarmObject:
                case kAuditCallback: case kMandatoryLabel: case kResourceAttribute:
                case kScopedPolicyId: case kProcessTrustLabel: case kAccessFilter:
                    if ((sacl_class(type) & si) == 0) continue;
                    break;
                default:
                    break;
            }
        }
        const char* name = nullptr;
        bool object = false;
        bool label = false;
        if (dacl) {
            switch (type) {
                case kAllowed: name = "A"; break;
                case kDenied: name = "D"; break;
                case kAllowedObject: name = "OA"; object = true; break;
                case kDeniedObject: name = "OD"; object = true; break;
                default: break;
            }
        } else {
            switch (type) {
                case kAudit: name = "AU"; break;
                case kAlarm: name = "AL"; break;
                case kAuditObject: name = "OU"; object = true; break;
                case kAlarmObject: name = "OL"; object = true; break;
                case kMandatoryLabel: name = "ML"; label = true; break;
                case kScopedPolicyId: name = "SP"; break;
                case kProcessTrustLabel: name = "TL"; break;
                default: break;
            }
        }
        if (name == nullptr) {
            const bool callback = type == kAllowedCallback || type == kDeniedCallback ||
                                  type == kAllowedCallbackObject || type == kAuditCallback ||
                                  type == kResourceAttribute || type == kAccessFilter;
            if (callback) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED advapi32!ConvertSecurityDescriptorToStringSecurityDescriptor: "
                    "ACE type 0x%02x (condition / resource attribute)\n", type);
                return kInvalidParameter;
            }
            return kInvalidAcl;   // probe "unknown_ace", "ml_in_dacl", "a_in_sacl"
        }
        const u32 mask = get_u32(ace, 4);
        usize sid_off = 8;
        std::optional<std::span<const u8>> guid_obj, guid_inh;
        if (object) {
            if (size < 12) return kInvalidAcl;
            const u32 of = get_u32(ace, 8);
            sid_off = 12;
            if ((of & kObjectTypePresent) != 0) {
                if (sid_off + 16 > size) return kInvalidAcl;
                guid_obj = ace.subspan(sid_off, 16);
                sid_off += 16;
            }
            if ((of & kInheritedObjectTypePresent) != 0) {
                if (sid_off + 16 > size) return kInvalidAcl;
                guid_inh = ace.subspan(sid_off, 16);
                sid_off += 16;
            }
            if (!guid_obj && !guid_inh) {
                // Printed as the plain type (probe "oa_noguid").
                name = type == kAllowedObject ? "A" : type == kDeniedObject ? "D"
                     : type == kAuditObject ? "AU" : "AL";
            }
        }
        if (sid_off + 8 > size) return kInvalidAcl;
        const std::span<const u8> sid = ace.subspan(sid_off);
        const std::string sid_text = sid_to_sddl(sid);
        if (sid_text.empty()) return kInvalidSid;

        out += '(';
        out += name;
        out += ';';
        for (const auto& f : kAceFlags) {
            if ((flags & f.mask) != 0) out += f.name;
        }
        out += ';';
        extra += 1 + format_mask(out, mask, label);
        out += ';';
        if (guid_obj) format_guid(out, *guid_obj);
        out += ';';
        if (guid_inh) format_guid(out, *guid_inh);
        out += ';';
        out += sid_text;
        out += ')';
    }
    return kOk;
}

void append_acl_flags(std::string& out, u16 control, bool dacl) {
    if ((control & (dacl ? kDaclProtected : kSaclProtected)) != 0) out += 'P';
    if ((control & (dacl ? kDaclAutoInheritReq : kSaclAutoInheritReq)) != 0) out += "AR";
    if ((control & (dacl ? kDaclAutoInherited : kSaclAutoInherited)) != 0) out += "AI";
}

struct WellKnownSidEntry {
    u32 type;
    bool domain_relative;  ///< `text` is a RID appended to the caller's DomainSid
    const char* text;      ///< the SID, or the RID
};

// Every WELL_KNOWN_SID_TYPE in winnt.h 10.0.26100.0 (0..121) with what
// CreateWellKnownSid returns for it, transcribed from
// tools/oracle_probe/advapi_wksid_probe.expected (the "wks" / "dom" lines).
// SidTypeContractTest replays that file, so a transcription error fails.
// Types missing here fail with ERROR_INVALID_PARAMETER on Windows too.
constexpr WellKnownSidEntry kWellKnownSids[] = {
    {0, false, "S-1-0-0"},  // WinNullSid
    {1, false, "S-1-1-0"},  // WinWorldSid
    {2, false, "S-1-2-0"},  // WinLocalSid
    {3, false, "S-1-3-0"},  // WinCreatorOwnerSid
    {4, false, "S-1-3-1"},  // WinCreatorGroupSid
    {5, false, "S-1-3-2"},  // WinCreatorOwnerServerSid
    {6, false, "S-1-3-3"},  // WinCreatorGroupServerSid
    {7, false, "S-1-5"},  // WinNtAuthoritySid
    {8, false, "S-1-5-1"},  // WinDialupSid
    {9, false, "S-1-5-2"},  // WinNetworkSid
    {10, false, "S-1-5-3"},  // WinBatchSid
    {11, false, "S-1-5-4"},  // WinInteractiveSid
    {12, false, "S-1-5-6"},  // WinServiceSid
    {13, false, "S-1-5-7"},  // WinAnonymousSid
    {14, false, "S-1-5-8"},  // WinProxySid
    {15, false, "S-1-5-9"},  // WinEnterpriseControllersSid
    {16, false, "S-1-5-10"},  // WinSelfSid
    {17, false, "S-1-5-11"},  // WinAuthenticatedUserSid
    {18, false, "S-1-5-12"},  // WinRestrictedCodeSid
    {19, false, "S-1-5-13"},  // WinTerminalServerSid
    {20, false, "S-1-5-14"},  // WinRemoteLogonIdSid
    // 21: WinLogonIdsSid -> ERROR_INVALID_PARAMETER
    {22, false, "S-1-5-18"},  // WinLocalSystemSid
    {23, false, "S-1-5-19"},  // WinLocalServiceSid
    {24, false, "S-1-5-20"},  // WinNetworkServiceSid
    {25, false, "S-1-5-32"},  // WinBuiltinDomainSid
    {26, false, "S-1-5-32-544"},  // WinBuiltinAdministratorsSid
    {27, false, "S-1-5-32-545"},  // WinBuiltinUsersSid
    {28, false, "S-1-5-32-546"},  // WinBuiltinGuestsSid
    {29, false, "S-1-5-32-547"},  // WinBuiltinPowerUsersSid
    {30, false, "S-1-5-32-548"},  // WinBuiltinAccountOperatorsSid
    {31, false, "S-1-5-32-549"},  // WinBuiltinSystemOperatorsSid
    {32, false, "S-1-5-32-550"},  // WinBuiltinPrintOperatorsSid
    {33, false, "S-1-5-32-551"},  // WinBuiltinBackupOperatorsSid
    {34, false, "S-1-5-32-552"},  // WinBuiltinReplicatorSid
    {35, false, "S-1-5-32-554"},  // ?
    {36, false, "S-1-5-32-555"},  // WinBuiltinRemoteDesktopUsersSid
    {37, false, "S-1-5-32-556"},  // WinBuiltinNetworkConfigurationOperatorsSid
    {38, true, "500"},  // WinAccountAdministratorSid (domain RID)
    {39, true, "501"},  // WinAccountGuestSid (domain RID)
    {40, true, "502"},  // WinAccountKrbtgtSid (domain RID)
    {41, true, "512"},  // WinAccountDomainAdminsSid (domain RID)
    {42, true, "513"},  // WinAccountDomainUsersSid (domain RID)
    {43, true, "514"},  // WinAccountDomainGuestsSid (domain RID)
    {44, true, "515"},  // WinAccountComputersSid (domain RID)
    {45, true, "516"},  // WinAccountControllersSid (domain RID)
    {46, true, "517"},  // WinAccountCertAdminsSid (domain RID)
    {47, true, "518"},  // WinAccountSchemaAdminsSid (domain RID)
    {48, true, "519"},  // WinAccountEnterpriseAdminsSid (domain RID)
    {49, true, "520"},  // WinAccountPolicyAdminsSid (domain RID)
    {50, true, "553"},  // WinAccountRasAndIasServersSid (domain RID)
    {51, false, "S-1-5-64-10"},  // WinNTLMAuthenticationSid
    {52, false, "S-1-5-64-21"},  // WinDigestAuthenticationSid
    {53, false, "S-1-5-64-14"},  // WinSChannelAuthenticationSid
    {54, false, "S-1-5-15"},  // WinThisOrganizationSid
    {55, false, "S-1-5-1000"},  // WinOtherOrganizationSid
    {56, false, "S-1-5-32-557"},  // WinBuiltinIncomingForestTrustBuildersSid
    {57, false, "S-1-5-32-558"},  // WinBuiltinPerfMonitoringUsersSid
    {58, false, "S-1-5-32-559"},  // WinBuiltinPerfLoggingUsersSid
    {59, false, "S-1-5-32-560"},  // WinBuiltinAuthorizationAccessSid
    {60, false, "S-1-5-32-561"},  // WinBuiltinTerminalServerLicenseServersSid
    {61, false, "S-1-5-32-562"},  // WinBuiltinDCOMUsersSid
    {62, false, "S-1-5-32-568"},  // WinBuiltinIUsersSid
    {63, false, "S-1-5-17"},  // WinIUserSid
    {64, false, "S-1-5-32-569"},  // WinBuiltinCryptoOperatorsSid
    {65, false, "S-1-16-0"},  // WinUntrustedLabelSid
    {66, false, "S-1-16-4096"},  // WinLowLabelSid
    {67, false, "S-1-16-8192"},  // WinMediumLabelSid
    {68, false, "S-1-16-12288"},  // WinHighLabelSid
    {69, false, "S-1-16-16384"},  // WinSystemLabelSid
    {70, false, "S-1-5-33"},  // WinWriteRestrictedCodeSid
    {71, false, "S-1-3-4"},  // WinCreatorOwnerRightsSid
    {72, true, "571"},  // WinCacheablePrincipalsGroupSid (domain RID)
    {73, true, "572"},  // WinNonCacheablePrincipalsGroupSid (domain RID)
    {74, false, "S-1-5-22"},  // WinEnterpriseReadonlyControllersSid
    {75, true, "521"},  // WinAccountReadonlyControllersSid (domain RID)
    {76, false, "S-1-5-32-573"},  // ?
    {77, true, "498"},  // WinNewEnterpriseReadonlyControllersSid (domain RID)
    {78, false, "S-1-5-32-574"},  // ?
    {79, false, "S-1-16-8448"},  // WinMediumPlusLabelSid
    // 80: WinLocalLogonSid -> ERROR_INVALID_PARAMETER
    {81, false, "S-1-2-1"},  // WinConsoleLogonSid
    {82, false, "S-1-5-65-1"},  // WinThisOrganizationCertificateSid
    // 83: WinApplicationPackageAuthoritySid -> ERROR_INVALID_PARAMETER
    {84, false, "S-1-15-2-1"},  // WinBuiltinAnyPackageSid
    {85, false, "S-1-15-3-1"},  // WinCapabilityInternetClientSid
    {86, false, "S-1-15-3-2"},  // WinCapabilityInternetClientServerSid
    {87, false, "S-1-15-3-3"},  // WinCapabilityPrivateNetworkClientServerSid
    {88, false, "S-1-15-3-4"},  // WinCapabilityPicturesLibrarySid
    {89, false, "S-1-15-3-5"},  // WinCapabilityVideosLibrarySid
    {90, false, "S-1-15-3-6"},  // WinCapabilityMusicLibrarySid
    {91, false, "S-1-15-3-7"},  // WinCapabilityDocumentsLibrarySid
    {92, false, "S-1-15-3-9"},  // WinCapabilitySharedUserCertificatesSid
    {93, false, "S-1-15-3-8"},  // WinCapabilityEnterpriseAuthenticationSid
    {94, false, "S-1-15-3-10"},  // WinCapabilityRemovableStorageSid
    {95, false, "S-1-5-32-575"},  // WinBuiltinRDSRemoteAccessServersSid
    {96, false, "S-1-5-32-576"},  // WinBuiltinRDSEndpointServersSid
    {97, false, "S-1-5-32-577"},  // WinBuiltinRDSManagementServersSid
    {98, false, "S-1-5-84-0-0-0-0-0"},  // WinUserModeDriversSid
    {99, false, "S-1-5-32-578"},  // WinBuiltinHyperVAdminsSid
    {100, true, "522"},  // WinAccountCloneableControllersSid (domain RID)
    {101, false, "S-1-5-32-579"},  // WinBuiltinAccessControlAssistanceOperatorsSid
    {102, false, "S-1-5-32-580"},  // WinBuiltinRemoteManagementUsersSid
    {103, false, "S-1-18-1"},  // WinAuthenticationAuthorityAssertedSid
    {104, false, "S-1-18-2"},  // WinAuthenticationServiceAssertedSid
    {105, false, "S-1-5-113"},  // WinLocalAccountSid
    {106, false, "S-1-5-114"},  // WinLocalAccountAndAdministratorSid
    {107, true, "525"},  // WinAccountProtectedUsersSid (domain RID)
    {108, false, "S-1-15-3-11"},  // WinCapabilityAppointmentsSid
    {109, false, "S-1-15-3-12"},  // WinCapabilityContactsSid
    {110, true, "503"},  // WinAccountDefaultSystemManagedSid (domain RID)
    {111, false, "S-1-5-32-581"},  // WinBuiltinDefaultSystemManagedGroupSid
    {112, false, "S-1-5-32-582"},  // WinBuiltinStorageReplicaAdminsSid
    {113, true, "526"},  // WinAccountKeyAdminsSid (domain RID)
    {114, true, "527"},  // WinAccountEnterpriseKeyAdminsSid (domain RID)
    {115, false, "S-1-18-4"},  // WinAuthenticationKeyTrustSid
    {116, false, "S-1-18-5"},  // WinAuthenticationKeyPropertyMFASid
    {117, false, "S-1-18-6"},  // WinAuthenticationKeyPropertyAttestationSid
    {118, false, "S-1-18-3"},  // WinAuthenticationFreshKeyAuthSid
    {119, false, "S-1-5-32-583"},  // WinBuiltinDeviceOwnersSid
    {120, false, "S-1-5-32-584"},  // WinBuiltinUserModeHardwareOperatorsSid
    {121, false, "S-1-5-32-585"},  // WinBuiltinOpenSSHUsersSid
    // 122: ? -> ERROR_INVALID_PARAMETER
    // 123: ? -> ERROR_INVALID_PARAMETER
    // 124: ? -> ERROR_INVALID_PARAMETER
    // 125: ? -> ERROR_INVALID_PARAMETER
};

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

u32 create_well_known_sid(u32 type, const std::vector<u8>* domain, std::vector<u8>& sid) {
    for (const auto& e : kWellKnownSids) {
        if (e.type != type) continue;
        if (!e.domain_relative) {
            return parse_numeric_sid(e.text, sid, true) ? kOk : kInvalidParameter;
        }
        // DomainSid: a valid SID with room for the RID (1..14 sub-authorities;
        // probe admin_domain "S-1-5" / "...-14").
        if (domain == nullptr || !sid_is_valid(*domain) || (*domain)[1] == 0 ||
            (*domain)[1] >= 15) {
            return kInvalidParameter;
        }
        const usize len = 8u + (*domain)[1] * 4u;
        sid.assign(domain->begin(), domain->begin() + static_cast<std::ptrdiff_t>(len));
        sid[1] = static_cast<u8>(sid[1] + 1);
        sid.resize(len + 4);
        put_u32(sid, len, static_cast<u32>(std::strtoul(e.text, nullptr, 10)));
        return kOk;
    }
    return kInvalidParameter;
}

bool is_well_known_sid(std::span<const u8> sid, u32 type) {
    if (!sid_is_valid(sid)) return false;
    const std::span<const u8> exact = sid.subspan(0, 8u + sid[1] * 4u);
    for (const auto& e : kWellKnownSids) {
        if (e.type != type) continue;
        if (!e.domain_relative) {
            std::vector<u8> want;
            return parse_numeric_sid(e.text, want, true) && want.size() == exact.size() &&
                   std::memcmp(want.data(), exact.data(), exact.size()) == 0;
        }
        // Any account domain (S-1-5-21-...) followed by the RID (probe
        // iswk_admin_any_domain).
        return exact[1] >= 2 && sid_authority(exact) == 5 && get_u32(exact, 8) == 21 &&
               get_u32(exact, 8 + (exact[1] - 1u) * 4u) ==
                   static_cast<u32>(std::strtoul(e.text, nullptr, 10));
    }
    return false;
}

u32 string_to_sid(std::string_view text, std::vector<u8>& sid) {
    if (text.size() >= 2 && upper(text[0]) == 'S' && text[1] == '-') {
        return parse_numeric_sid(text, sid) ? kOk : kInvalidSid;
    }
    // Exactly two characters (probe: "BA " and "BAx" are ERROR_INVALID_SID).
    if (text.size() != 2) return kInvalidSid;
    return resolve_alias(text[0], text[1], sid);
}

u32 string_to_sd(std::string_view s, std::vector<u8>& sd_out) {
    SdView sd;
    bool seen[4] = {false, false, false, false};  // O G D S
    usize p = 0;
    const usize n = s.size();
    while (true) {
        skip_space(s, p, n);
        if (p >= n) break;
        const char tag = s[p];
        const int idx = tag == 'O' ? 0 : tag == 'G' ? 1 : tag == 'D' ? 2 : tag == 'S' ? 3 : -1;
        if (idx < 0 || p + 1 >= n || s[p + 1] != ':') return kInvalidParameter;
        if (seen[idx]) return kInvalidParameter;
        seen[idx] = true;
        p += 2;
        // The component runs to the character before the next ':' — which
        // must itself be a component tag (probe "O:BA:", "...Gx:BA").
        usize end = n;
        const usize colon = s.find(':', p);
        if (colon != std::string_view::npos) {
            if (colon == p) return kInvalidParameter;
            const char next = s[colon - 1];
            if (next != 'O' && next != 'G' && next != 'D' && next != 'S') {
                return kInvalidParameter;
            }
            end = colon - 1;
        }
        if (idx <= 1) {
            std::vector<u8> sid;
            if (const u32 err = parse_principal(s, p, end, sid); err != kOk) return err;
            (idx == 0 ? sd.owner : sd.group) = std::move(sid);
        } else {
            ParsedAclText acl;
            if (const u32 err = parse_acl_component(s, p, end, idx == 2, acl); err != kOk) {
                return err;
            }
            sd.control = static_cast<u16>(sd.control | acl.control);
            if (idx == 2) {
                sd.dacl_present = true;
                sd.dacl_null = acl.null_acl;
                sd.dacl = std::move(acl.acl);
            } else {
                sd.sacl_present = true;
                sd.sacl_null = acl.null_acl;
                sd.sacl = std::move(acl.acl);
            }
        }
        p = end;
    }
    sd_out = build_self_relative(sd);
    return kOk;
}

std::vector<u8> build_self_relative(const SdView& sd) {
    std::vector<u8> out(20, 0);
    out[0] = 1;
    u16 control = static_cast<u16>(sd.control | kSelfRelative);
    if (sd.dacl_present) control |= kDaclPresent;
    if (sd.sacl_present) control |= kSaclPresent;
    auto append = [&out](const std::vector<u8>& bytes, usize field) {
        if (bytes.empty()) return;
        put_u32(out, field, static_cast<u32>(out.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    if (sd.sacl_present && !sd.sacl_null) append(sd.sacl, 12);
    if (sd.dacl_present && !sd.dacl_null) append(sd.dacl, 16);
    append(sd.owner, 4);
    append(sd.group, 8);
    out[2] = static_cast<u8>(control);
    out[3] = static_cast<u8>(control >> 8);
    return out;
}

std::string sid_to_string(std::span<const u8> sid) {
    if (sid.size() < 8) return {};
    std::string text = "S-" + std::to_string(sid[0]) + "-";
    const u64 authority = sid_authority(sid);
    if (authority > 0xFFFFFFFFull) append_hex(text, authority, true);
    else text += std::to_string(authority);
    const usize count = sid[1];
    for (usize i = 0; i < count && 8 + i * 4 + 4 <= sid.size(); ++i) {
        text += '-';
        text += std::to_string(get_u32(sid, 8 + i * 4));
    }
    return text;
}

std::string sid_to_sddl(std::span<const u8> sid) {
    if (!sid_is_valid(sid)) return {};
    const usize len = 8u + sid[1] * 4u;
    const std::span<const u8> exact = sid.subspan(0, len);
    for (const auto& e : kSidAliases) {
        std::vector<u8> candidate;
        if (e.kind == AliasKind::Fixed) {
            if (!parse_numeric_sid(e.sid, candidate)) continue;
        } else if (e.kind == AliasKind::MachineRid) {
            candidate = machine_rid_sid(e.rid);
        } else {
            continue;
        }
        if (candidate.size() == exact.size() &&
            std::memcmp(candidate.data(), exact.data(), exact.size()) == 0) {
            return e.alias;
        }
    }
    return sid_to_string(exact);
}

u32 sd_to_string(const SdView& sd, u32 si, std::string& out, u32* reported_length) {
    out.clear();
    u32 extra = 0;
    if ((si & 0x1) != 0 && !sd.owner.empty()) {
        const std::string t = sid_to_sddl(sd.owner);
        if (t.empty()) return kInvalidSid;
        out += "O:" + t;
    }
    if ((si & 0x2) != 0 && !sd.group.empty()) {
        const std::string t = sid_to_sddl(sd.group);
        if (t.empty()) return kInvalidSid;
        out += "G:" + t;
    }
    if ((si & 0x4) != 0 && sd.dacl_present) {
        out += "D:";
        append_acl_flags(out, sd.control, true);
        if (sd.dacl_null) out += "NO_ACCESS_CONTROL", ++extra;  // probe: len 0x14
        else if (const u32 err = format_acl(out, sd.dacl, true, si, extra); err != kOk) return err;
    }
    if ((si & 0xF8) != 0 && sd.sacl_present) {
        out += "S:";
        append_acl_flags(out, sd.control, false);
        if (sd.sacl_null) out += "NO_ACCESS_CONTROL", ++extra;
        else if (const u32 err = format_acl(out, sd.sacl, false, si, extra); err != kOk) return err;
    }
    if (reported_length != nullptr) *reported_length = static_cast<u32>(out.size()) + extra;
    return kOk;
}

u32 capture_guest_sd(Marshaler& m, u64 sd_addr, SdView& out) {
    const GuestAddr sd = static_cast<GuestAddr>(sd_addr);
    auto rev = m.read_u8(sd);
    auto ctl = m.read_u16(sd + 2);
    if (rev.is_err() || ctl.is_err()) return kNoAccess;
    if (rev.value() != 1) return kUnknownRevision;
    out = SdView{};
    out.revision = rev.value();
    const u16 control = ctl.value();
    out.control = static_cast<u16>(control & ~(kSelfRelative | kDaclPresent | kSaclPresent));
    const bool self_relative = (control & kSelfRelative) != 0;

    auto read_sid = [&](GuestAddr at, std::vector<u8>& bytes) -> u32 {
        if (at == 0) return kOk;
        auto count = m.read_u8(at + 1);
        if (count.is_err()) return kNoAccess;
        const usize len = 8u + count.value() * 4u;
        bytes.resize(len);
        return m.memory().read_bytes(at, bytes.data(), len).is_ok() ? kOk : kNoAccess;
    };
    auto read_acl = [&](GuestAddr at, std::vector<u8>& bytes) -> u32 {
        auto size = m.read_u16(at + 2);
        if (size.is_err()) return kNoAccess;
        if (size.value() < 8) return kInvalidSecurityDescr;
        bytes.resize(size.value());
        return m.memory().read_bytes(at, bytes.data(), bytes.size()).is_ok() ? kOk : kNoAccess;
    };
    GuestAddr owner = 0, group = 0, sacl = 0, dacl = 0;
    if (self_relative) {
        auto rd = [&](u32 field) -> GuestAddr {
            const u32 off = m.read_u32(sd + field).value_or(0);
            return off == 0 ? 0 : sd + off;
        };
        owner = rd(4);
        group = rd(8);
        sacl = rd(12);
        dacl = rd(16);
    } else {
        const bool is64 = m.cpu_is_64bit();
        auto rp = [&](u32 f32, u32 f64) -> GuestAddr {
            return static_cast<GuestAddr>(m.read_ptr(sd + (is64 ? f64 : f32)).value_or(0));
        };
        owner = rp(4, 8);
        group = rp(8, 16);
        sacl = rp(12, 24);
        dacl = rp(16, 32);
    }
    if (const u32 e = read_sid(owner, out.owner); e != kOk) return e;
    if (const u32 e = read_sid(group, out.group); e != kOk) return e;
    out.dacl_present = (control & kDaclPresent) != 0;
    out.sacl_present = (control & kSaclPresent) != 0;
    if (out.dacl_present) {
        if (dacl == 0) out.dacl_null = true;
        else if (const u32 e = read_acl(dacl, out.dacl); e != kOk) return e;
    }
    if (out.sacl_present) {
        if (sacl == 0) out.sacl_null = true;
        else if (const u32 e = read_acl(sacl, out.sacl); e != kOk) return e;
    }
    return kOk;
}

} // namespace vitreo::winapi::sddl
