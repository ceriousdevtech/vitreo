// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_product.cpp
/// @brief Windows Installer engine area: product operations (install, configure, reinstall,
/// advertise, patch) and the file/descriptor/transaction utility functions.
///
/// Contract sources: the Microsoft Learn page of each export, Msi.h (SDK
/// 10.0.26100), [MS-SHLLINK], the OpenType `name` table, and
/// tools/oracle_probe/msi_productops_probe.c (msi_productops32/64.expected),
/// which measured on Windows 11 every parameter-validation order and error
/// code below that Learn leaves open. Gaps: .docs/contracts/msi/productops.md.

#include "winapi/system/msi/msi_product.hpp"
#include "winapi/system/msi/msi_service.hpp"

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/system/msi/msi_register.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/system/version/version.hpp"
#include "winapi/system/crypt32/authenticode.hpp"
#include "capsule/vfs/vfs.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace vitreo::winapi::msi {

namespace {

// ---- Win32 / HRESULT values (winerror.h) ---------------------------------------------
constexpr u32 kOk = 0;
constexpr u32 kErrFileNotFound = 2;
constexpr u32 kErrPathNotFound = 3;
constexpr u32 kErrAccessDenied = 5;
constexpr u32 kErrInvalidHandle = 6;
constexpr u32 kErrInvalidData = 13;
constexpr u32 kErrInvalidParameter = 87;
constexpr u32 kErrCallNotImplemented = 120;
constexpr u32 kErrMoreData = 234;
constexpr u32 kErrFileInvalid = 1006;
constexpr u32 kErrInstallFailure = 1603;
constexpr u32 kErrUnknownProduct = 1605;
constexpr u32 kErrUnknownFeature = 1606;
constexpr u32 kErrUnknownComponent = 1607;
constexpr u32 kErrAlreadyRunning = 1618;
constexpr u32 kErrPackageOpenFailed = 1619;
constexpr u32 kErrPackageInvalid = 1620;
constexpr u32 kErrFunctionFailed = 1627;
constexpr u32 kErrPatchPackageInvalid = 1636;
constexpr u32 kErrPatchTargetNotFound = 1642;
constexpr u32 kErrOpenFailed = 110;

constexpr u32 kHrOk = 0;
constexpr u32 kHrFalse = 1;
constexpr u32 kHrFail = 0x80004005u;
constexpr u32 kHrInvalidArg = 0x80070057u;
constexpr u32 kHrNoSignature = 0x800B0100u;        // TRUST_E_NOSIGNATURE
constexpr u32 kHrCryptFileError = 0x80092003u;     // CRYPT_E_FILE_ERROR
constexpr u32 kHrClassNotAvailable = 0x80040111u;  // CLASS_E_CLASSNOTAVAILABLE
constexpr u32 kHrSelfRegClass = 0x80040201u;       // SELFREG_E_CLASS
constexpr u32 hresult_from_win32(u32 e) { return e == 0 ? 0 : (e & 0xFFFFu) | 0x80070000u; }

// ---- Msi.h enumerations --------------------------------------------------------------------
constexpr i32 kStateAdvertised = 1, kStateAbsent = 2, kStateLocal = 3, kStateSource = 4, kStateDefault = 5;
constexpr u32 kInstallLevelMaximum = 0xFFFF;
constexpr u32 kReinstallModeMask = 0x7FF;  // REINSTALLMODE_REPAIR .. REINSTALLMODE_PACKAGE
constexpr u32 kInstallTypeDefault = 0, kInstallTypeNetworkImage = 1, kInstallTypeSingleInstance = 2;
constexpr u32 kTransactionAttributesMask = 0x3;  // MSITRANSACTION_CHAIN_EMBEDDEDUI | _JOIN_EXISTING_EMBEDDEDUI
constexpr u32 kTransactionRollback = 0, kTransactionCommit = 1;
constexpr u32 kPatchDataPatchFile = 0, kPatchDataXmlPath = 1, kPatchDataXmlBlob = 2;
constexpr u32 kMsiInvalidHashIsFatal = 0x1;

// ---- Strings ---------------------------------------------------------------------------------

std::u16string to_u16(const std::string& utf8) {
    const std::vector<u16> units = utf8_to_utf16(utf8);
    return std::u16string(units.begin(), units.end());
}
std::string to_utf8(std::u16string_view s) {
    if (s.empty()) return {};
    return utf16_to_utf8(reinterpret_cast<const u16*>(s.data()), static_cast<int>(s.size()));
}

/// An input string argument: nullopt for a NULL pointer. ANSI text is in
/// the process code page (CP_ACP).
std::optional<std::u16string> read_in(Marshaler& m, GuestAddr p, bool wide) {
    if (p == 0) return std::nullopt;
    std::u16string out;
    if (wide) {
        for (usize i = 0; i < 0x100000; ++i) {
            auto u = m.read_u16(p + i * 2);
            if (u.is_err() || u.value() == 0) break;
            out.push_back(static_cast<char16_t>(u.value()));
        }
        return out;
    }
    std::string bytes;
    for (usize i = 0; i < 0x100000; ++i) {
        auto b = m.read_u8(p + i);
        if (b.is_err() || b.value() == 0) break;
        bytes.push_back(static_cast<char>(b.value()));
    }
    if (bytes.empty()) return out;
    std::vector<u16> units(bytes.size() * 2 + 2);
    const auto r = codepage::multi_byte_to_wide(codepage::ansi_code_page(), 0,
                                                reinterpret_cast<const u8*>(bytes.data()), bytes.size(),
                                                units.data(), units.size());
    out.assign(units.begin(), units.begin() + r.count);
    return out;
}
std::optional<std::string> read_in8(Marshaler& m, GuestAddr p, bool wide) {
    auto s = read_in(m, p, wide);
    if (!s) return std::nullopt;
    return to_utf8(*s);
}

/// The text in the caller's character set: UTF-16 units, or CP_ACP bytes.
std::vector<u16> units_for(const std::u16string& text, bool wide) {
    if (wide) return std::vector<u16>(text.begin(), text.end());
    std::vector<u8> bytes(text.size() * 4 + 4);
    const auto r = codepage::wide_to_multi_byte(codepage::ansi_code_page(), 0,
                                                reinterpret_cast<const u16*>(text.data()), text.size(),
                                                bytes.data(), bytes.size(), nullptr, false);
    return std::vector<u16>(bytes.begin(), bytes.begin() + r.count);
}

void write_units(Marshaler& m, GuestAddr buf, bool wide, const std::vector<u16>& units, usize count) {
    for (usize i = 0; i < count; ++i) {
        if (wide) (void)m.write_u16(buf + i * 2, units[i]);
        else (void)m.write_u8(buf + i, static_cast<u8>(units[i]));
    }
    if (wide) (void)m.write_u16(buf + count * 2, 0);
    else (void)m.write_u8(buf + count, 0);
}

/// The installer's buffer + count convention (Learn "MsiGetFileVersion";
/// probe version lines): *pcch is the capacity including the terminator on
/// entry and the length without it on exit; a NULL buffer is a size query;
/// a buffer that is too small receives capacity-1 characters and a
/// terminator, and the call returns ERROR_MORE_DATA. The caller has
/// rejected a buffer without a count.
u32 put_sized(Marshaler& m, bool wide, GuestAddr buf, GuestAddr pcch, const std::u16string& text) {
    if (pcch == 0) return kOk;
    const std::vector<u16> units = units_for(text, wide);
    const u32 required = static_cast<u32>(units.size());
    if (buf == 0) {
        (void)m.write_u32(pcch, required);
        return kOk;
    }
    const u32 capacity = m.read_u32(pcch).is_ok() ? m.read_u32(pcch).value() : 0;
    (void)m.write_u32(pcch, required);
    if (required + 1 > capacity) {
        if (capacity > 0) write_units(m, buf, wide, units, capacity - 1);
        return kErrMoreData;
    }
    write_units(m, buf, wide, units, required);
    return kOk;
}

/// A fixed-size output buffer (product/feature/component codes): the text
/// and a terminator.
void put_fixed(Marshaler& m, bool wide, GuestAddr buf, const std::u16string& text) {
    if (buf == 0) return;
    const std::vector<u16> units = units_for(text, wide);
    write_units(m, buf, wide, units, units.size());
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ---- Files through the capsule VFS -------------------------------------------------------------

enum class FileProblem { None, Empty, NotFound, PathNotFound, Directory, AccessDenied, Other };

FileProblem read_file(capsule::Vfs* vfs, const std::string& path, std::vector<u8>& out) {
    out.clear();
    if (path.empty()) return FileProblem::Empty;
    if (vfs == nullptr) return FileProblem::Other;
    if (auto st = vfs->stat(path); st.is_ok()) {
        if ((st.value().attributes & capsule::FileAttributes::Directory) != capsule::FileAttributes::None)
            return FileProblem::Directory;
    }
    auto opened = vfs->open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                            capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) {
        switch (opened.error()) {
            case capsule::VfsError::FileNotFound: return FileProblem::NotFound;
            case capsule::VfsError::PathNotFound: return FileProblem::PathNotFound;
            case capsule::VfsError::AccessDenied:
            case capsule::VfsError::SharingViolation: return FileProblem::AccessDenied;
            case capsule::VfsError::NotAFile: return FileProblem::Directory;
            default: return FileProblem::NotFound;
        }
    }
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto r = vfs->read(opened.value(), chunk);
        if (r.is_err() || r.value() == 0) break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(r.value()));
    }
    (void)vfs->close(opened.value());
    return FileProblem::None;
}

// ---- little-endian readers over a byte span ----------------------------------------------------

struct Bytes {
    std::span<const u8> b;
    [[nodiscard]] bool has(usize off, usize n) const { return off <= b.size() && n <= b.size() - off; }
    [[nodiscard]] u16 u16le(usize o) const { return has(o, 2) ? static_cast<u16>(b[o] | (b[o + 1] << 8)) : 0; }
    [[nodiscard]] u32 u32le(usize o) const {
        return has(o, 4) ? static_cast<u32>(b[o]) | (static_cast<u32>(b[o + 1]) << 8) |
                               (static_cast<u32>(b[o + 2]) << 16) | (static_cast<u32>(b[o + 3]) << 24)
                         : 0;
    }
    [[nodiscard]] u16 u16be(usize o) const { return has(o, 2) ? static_cast<u16>((b[o] << 8) | b[o + 1]) : 0; }
    [[nodiscard]] u32 u32be(usize o) const {
        return has(o, 4) ? (static_cast<u32>(b[o]) << 24) | (static_cast<u32>(b[o + 1]) << 16) |
                               (static_cast<u32>(b[o + 2]) << 8) | static_cast<u32>(b[o + 3])
                         : 0;
    }
};

}  // namespace

// ---- GUIDs ---------------------------------------------------------------------------------------

namespace {

constexpr char kBase85[] =
    "!$%&'()*+,-.0123456789=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{}~";

int base85_digit(char16_t c) {
    for (int i = 0; i < 85; ++i)
        if (static_cast<char16_t>(kBase85[i]) == c) return i;
    return -1;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// The GUID's 16 bytes in memory order (Data1..3 little-endian, Data4 as is).
std::optional<std::array<u8, 16>> guid_bytes(std::string_view g) {
    if (!is_guid(g)) return std::nullopt;
    auto field = [&](usize at, usize digits) {
        u64 v = 0;
        for (usize i = 0; i < digits; ++i) v = (v << 4) | static_cast<u64>(hex_value(g[at + i]));
        return v;
    };
    std::array<u8, 16> b{};
    const u32 d1 = static_cast<u32>(field(1, 8));
    const u16 d2 = static_cast<u16>(field(10, 4));
    const u16 d3 = static_cast<u16>(field(15, 4));
    for (int i = 0; i < 4; ++i) b[i] = static_cast<u8>(d1 >> (8 * i));
    b[4] = static_cast<u8>(d2); b[5] = static_cast<u8>(d2 >> 8);
    b[6] = static_cast<u8>(d3); b[7] = static_cast<u8>(d3 >> 8);
    const usize d4[8] = {20, 22, 25, 27, 29, 31, 33, 35};
    for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<u8>(field(d4[i], 2));
    return b;
}

std::string guid_string(const std::array<u8, 16>& b) {
    char out[40];
    std::snprintf(out, sizeof out, "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}", b[3], b[2],
                  b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

}  // namespace

bool is_guid(std::string_view g) {
    if (g.size() != 38 || g.front() != '{' || g.back() != '}') return false;
    for (usize i = 1; i < 37; ++i) {
        const bool dash = (i == 9 || i == 14 || i == 19 || i == 24);
        if (dash ? g[i] != '-' : hex_value(g[i]) < 0) return false;
    }
    return true;
}

std::string compress_guid(std::string_view guid) {
    const auto b = guid_bytes(guid);
    if (!b) return {};
    std::string out;
    for (int d = 0; d < 4; ++d) {
        u32 v = static_cast<u32>((*b)[d * 4]) | (static_cast<u32>((*b)[d * 4 + 1]) << 8) |
                (static_cast<u32>((*b)[d * 4 + 2]) << 16) | (static_cast<u32>((*b)[d * 4 + 3]) << 24);
        for (int k = 0; k < 5; ++k) { out.push_back(kBase85[v % 85]); v /= 85; }
    }
    return out;
}

std::optional<std::string> decompress_guid(std::u16string_view c) {
    if (c.size() != 20) return std::nullopt;
    std::array<u8, 16> b{};
    for (int d = 0; d < 4; ++d) {
        u32 v = 0, scale = 1;
        for (int k = 0; k < 5; ++k) {
            const int digit = base85_digit(c[d * 5 + k]);
            if (digit < 0) return std::nullopt;
            v += static_cast<u32>(digit) * scale;  // wraps modulo 2^32, as Windows does
            scale *= 85;
        }
        for (int i = 0; i < 4; ++i) b[d * 4 + i] = static_cast<u8>(v >> (8 * i));
    }
    return guid_string(b);
}

// ---- Darwin descriptors -------------------------------------------------------------------------

u32 decompose_descriptor(std::u16string_view text, Descriptor& out, DescriptorProgress* progress) {
    out = Descriptor{};
    DescriptorProgress local;
    DescriptorProgress& p = progress ? *progress : local;
    p = DescriptorProgress{};
    if (text.size() < 20) return kErrInvalidParameter;
    const auto product = decompress_guid(text.substr(0, 20));
    if (!product) return kErrInvalidParameter;
    out.product = *product;
    p.product = true;
    usize i = 20;
    std::u16string feature;
    for (;;) {
        if (i >= text.size()) return out.feature = to_utf8(feature), kErrInvalidParameter;
        const char16_t c = text[i];
        if (c == u'<' || c == u'>') break;
        if (feature.size() == kMaxFeatureChars) return out.feature = to_utf8(feature), kErrInvalidParameter;
        feature.push_back(c);
        p.feature_chars = feature.size();
        ++i;
    }
    out.feature = to_utf8(feature);
    // A descriptor names a feature (probe: "<" or ">" right after the product
    // is ERROR_INVALID_PARAMETER with only the product written).
    if (feature.empty()) return kErrInvalidParameter;
    p.feature_terminated = true;
    if (text[i] == u'<') {
        out.consumed = i + 1;
        p.component = true;
        return kOk;
    }
    ++i;
    if (text.size() - i < 20) return kErrInvalidParameter;
    const auto component = decompress_guid(text.substr(i, 20));
    if (!component) return kErrInvalidParameter;
    out.component = *component;
    p.component = true;
    out.consumed = i + 20;
    return kOk;
}

std::string compose_descriptor(std::string_view product, std::string_view feature, std::string_view component) {
    const std::string p = compress_guid(product);
    if (p.empty() || feature.empty() || to_u16(std::string(feature)).size() > kMaxFeatureChars) return {};
    if (component.empty()) return p + std::string(feature) + "<";
    const std::string c = compress_guid(component);
    if (c.empty()) return {};
    return p + std::string(feature) + ">" + c;
}

// ---- File facts -----------------------------------------------------------------------------------

namespace {

/// VS_FIXEDFILEINFO (verrsrc.h) inside a version block, plus the
/// VarFileInfo\Translation languages. @p wide: a PE (UTF-16 keys, wType)
/// block, else an NE (ANSI keys, no wType) block.
struct ParsedVersion {
    bool fixed{false};
    u32 ms{0}, ls{0};
    std::set<u32> languages;
    bool translation{false};
};

usize align4(usize v) { return (v + 3) & ~usize{3}; }

/// Walk one version block node: returns its length (0 on malformed input).
usize parse_version_node(Bytes in, usize at, usize end, bool wide, int depth, std::string_view parent,
                         ParsedVersion& out) {
    if (!in.has(at, wide ? 6 : 4) || depth > 4) return 0;
    const usize length = in.u16le(at);
    const usize value_length = in.u16le(at + 2);
    if (length < (wide ? 6u : 4u) || at + length > end) return 0;
    usize key_at = at + (wide ? 6 : 4);
    std::string key;
    if (wide) {
        while (in.has(key_at, 2) && in.u16le(key_at) != 0 && key_at < at + length) {
            key.push_back(static_cast<char>(in.u16le(key_at) & 0xFF));
            key_at += 2;
        }
        key_at += 2;
    } else {
        while (in.has(key_at, 1) && in.b[key_at] != 0 && key_at < at + length) key.push_back(static_cast<char>(in.b[key_at++]));
        key_at += 1;
    }
    usize value_at = align4(key_at);
    const usize value_bytes = (wide && in.u16le(at + 4) == 1) ? value_length * 2 : value_length;
    if (depth == 0) {
        if (key != "VS_VERSION_INFO") return 0;
        if (value_bytes >= 52 && in.has(value_at, 52) && in.u32le(value_at) == 0xFEEF04BDu) {
            out.fixed = true;
            out.ms = in.u32le(value_at + 8);
            out.ls = in.u32le(value_at + 12);
        }
    } else if (parent == "VarFileInfo" && key == "Translation") {
        out.translation = true;
        for (usize k = 0; k + 4 <= value_bytes && in.has(value_at + k, 4); k += 4) out.languages.insert(in.u16le(value_at + k));
    }
    usize child = align4(value_at + value_bytes);
    while (child + (wide ? 6 : 4) <= at + length) {
        const usize n = parse_version_node(in, child, at + length, wide, depth + 1, key, out);
        if (n == 0) break;
        child = align4(child + n);
    }
    return length;
}

/// The RT_VERSION resource of an NE image (the 16-bit resource table;
/// .fon font files are NE images). Empty when it has none.
std::vector<u8> ne_version_resource(Bytes in) {
    const u32 ne = in.u32le(0x3C);
    if (!in.has(ne, 0x40) || in.u16le(ne) != 0x454E) return {};  // "NE"
    const usize table = ne + in.u16le(ne + 0x24);
    if (!in.has(table, 2)) return {};
    const u32 shift = in.u16le(table);
    usize at = table + 2;
    while (in.has(at, 8)) {
        const u16 type = in.u16le(at);
        if (type == 0) break;
        const u16 count = in.u16le(at + 2);
        at += 8;
        for (u16 k = 0; k < count; ++k, at += 12) {
            if (type != 0x8010) continue;  // RT_VERSION | 0x8000 (integer type id)
            const usize off = static_cast<usize>(in.u16le(at)) << shift;
            const usize len = static_cast<usize>(in.u16le(at + 2)) << shift;
            if (!in.has(off, len)) return {};
            return std::vector<u8>(in.b.begin() + off, in.b.begin() + off + len);
        }
    }
    return {};
}

/// A TrueType/OpenType font's `name` table version string (name ID 5,
/// Windows platform preferred) parsed as "major.minor.0.0" (probe font
/// lines: "Version 2102.025" -> 2102.25.0.0, "Version 2.07;..." -> 2.7.0.0).
std::optional<std::string> font_version(Bytes in) {
    usize font = 0;
    const u32 tag = in.u32be(0);
    if (tag == 0x74746366u) font = in.u32be(12);  // "ttcf": the first font
    else if (tag != 0x00010000u && tag != 0x4F54544Fu && tag != 0x74727565u) return std::nullopt;  // OTTO, true
    const u32 sfnt = in.u32be(font);
    if (sfnt != 0x00010000u && sfnt != 0x4F54544Fu && sfnt != 0x74727565u) return std::nullopt;
    const u16 tables = in.u16be(font + 4);
    for (u16 t = 0; t < tables; ++t) {
        const usize rec = font + 12 + 16 * static_cast<usize>(t);
        if (!in.has(rec, 16) || in.u32be(rec) != 0x6E616D65u) continue;  // "name"
        const usize name = in.u32be(rec + 8);
        const u16 count = in.u16be(name + 2);
        const usize strings = name + in.u16be(name + 4);
        std::u16string best;
        bool have = false;
        for (int pass = 0; pass < 2 && !have; ++pass) {
            for (u16 k = 0; k < count; ++k) {
                const usize r = name + 6 + 12 * static_cast<usize>(k);
                if (!in.has(r, 12) || in.u16be(r + 6) != 5) continue;
                const u16 platform = in.u16be(r);
                const usize len = in.u16be(r + 8), off = strings + in.u16be(r + 10);
                if (!in.has(off, len)) continue;
                if (pass == 0 && (platform == 3 || platform == 0)) {
                    for (usize i = 0; i + 1 < len; i += 2) best.push_back(static_cast<char16_t>(in.u16be(off + i)));
                    have = true;
                    break;
                }
                if (pass == 1 && platform == 1) {
                    for (usize i = 0; i < len; ++i) best.push_back(static_cast<char16_t>(in.b[off + i]));
                    have = true;
                    break;
                }
            }
        }
        if (!have) return std::nullopt;
        usize i = 0;
        while (i < best.size() && !(best[i] >= u'0' && best[i] <= u'9')) ++i;
        if (i == best.size()) return std::nullopt;
        u32 major = 0, minor = 0;
        while (i < best.size() && best[i] >= u'0' && best[i] <= u'9') major = major * 10 + static_cast<u32>(best[i++] - u'0');
        if (i < best.size() && best[i] == u'.') {
            ++i;
            while (i < best.size() && best[i] >= u'0' && best[i] <= u'9') minor = minor * 10 + static_cast<u32>(best[i++] - u'0');
        }
        return std::to_string(major) + "." + std::to_string(minor) + ".0.0";
    }
    return std::nullopt;
}

}  // namespace

u32 file_version_of_image(std::span<const u8> image, FileVersion& out) {
    out = FileVersion{};
    const Bytes in{image};
    std::vector<u8> block;
    bool wide = true;
    if (in.has(0, 0x40) && in.u16le(0) == 0x5A4D) {
        const u32 header = in.u32le(0x3C);
        if (in.has(header, 4) && in.u32le(header) == 0x00004550u) {
            u32 error = 0;
            block = version_resource_from_image(std::vector<u8>(image.begin(), image.end()), error);
        } else if (in.has(header, 2) && in.u16le(header) == 0x454E) {
            block = ne_version_resource(in);
            wide = false;
        }
        if (block.empty()) return kErrFileInvalid;
        ParsedVersion parsed;
        const Bytes vb{block};
        (void)parse_version_node(vb, 0, block.size(), wide, 0, {}, parsed);
        if (!parsed.fixed) return kErrInvalidData;
        out.version = std::to_string(parsed.ms >> 16) + "." + std::to_string(parsed.ms & 0xFFFF) + "." +
                      std::to_string(parsed.ls >> 16) + "." + std::to_string(parsed.ls & 0xFFFF);
        for (u32 lang : parsed.languages) {
            if (!out.language.empty()) out.language += ",";
            out.language += std::to_string(lang);
        }
        return kOk;
    }
    if (auto v = font_version(in)) {
        out.version = *v;
        return kOk;
    }
    return kErrFileInvalid;
}

// MD5 is the MsiFileHash table's contract (Learn "MsiFileHash Table"), not a
// security use.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::array<u32, 4> msi_file_hash(std::span<const u8> bytes) {
    std::array<u32, 4> parts{};
    if (bytes.empty()) return parts;
    u8 digest[CC_MD5_DIGEST_LENGTH];
    CC_MD5_CTX ctx;
    CC_MD5_Init(&ctx);
    usize done = 0;
    while (done < bytes.size()) {
        const usize n = std::min<usize>(bytes.size() - done, 1u << 30);
        CC_MD5_Update(&ctx, bytes.data() + done, static_cast<CC_LONG>(n));
        done += n;
    }
    CC_MD5_Final(digest, &ctx);
    for (int i = 0; i < 4; ++i)
        parts[i] = static_cast<u32>(digest[i * 4]) | (static_cast<u32>(digest[i * 4 + 1]) << 8) |
                   (static_cast<u32>(digest[i * 4 + 2]) << 16) | (static_cast<u32>(digest[i * 4 + 3]) << 24);
    return parts;
}
#pragma clang diagnostic pop

std::optional<std::u16string> darwin_descriptor_of_shortcut(std::span<const u8> lnk) {
    // [MS-SHLLINK] 2.1 ShellLinkHeader: HeaderSize 0x4C, LinkCLSID
    // 00021401-0000-0000-C000-000000000046, LinkFlags at 0x14.
    static constexpr u8 kClsid[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
    const Bytes in{lnk};
    if (!in.has(0, 0x4C) || in.u32le(0) != 0x4C || std::memcmp(lnk.data() + 4, kClsid, 16) != 0) return std::nullopt;
    const u32 flags = in.u32le(0x14);
    usize at = 0x4C;
    if (flags & 0x1) {  // HasLinkTargetIDList
        at += 2 + in.u16le(at);
    }
    if (flags & 0x2) {  // HasLinkInfo
        if (!in.has(at, 4)) return std::nullopt;
        at += in.u32le(at);
    }
    const bool unicode = (flags & 0x80) != 0;  // IsUnicode
    for (u32 bit : {0x4u, 0x8u, 0x10u, 0x20u, 0x40u}) {  // Name, RelativePath, WorkingDir, Arguments, IconLocation
        if (!(flags & bit)) continue;
        if (!in.has(at, 2)) return std::nullopt;
        at += 2 + static_cast<usize>(in.u16le(at)) * (unicode ? 2 : 1);
    }
    // ExtraData: blocks until a terminal block smaller than 4 bytes.
    while (in.has(at, 8)) {
        const u32 size = in.u32le(at);
        if (size < 8 || !in.has(at, size)) break;
        if (in.u32le(at + 4) == 0xA0000006u && size >= 0x314) {  // EXP_DARWIN_LINK
            std::u16string wide_id;
            for (usize i = 0; i < 260; ++i) {
                const u16 c = in.u16le(at + 8 + 260 + i * 2);
                if (c == 0) break;
                wide_id.push_back(static_cast<char16_t>(c));
            }
            if (!wide_id.empty()) return wide_id;
            std::string ansi;
            for (usize i = 0; i < 260 && lnk[at + 8 + i] != 0; ++i) ansi.push_back(static_cast<char>(lnk[at + 8 + i]));
            std::vector<u16> units(ansi.size() + 1);
            const auto r = codepage::multi_byte_to_wide(codepage::ansi_code_page(), 0,
                                                        reinterpret_cast<const u8*>(ansi.data()), ansi.size(),
                                                        units.data(), units.size());
            return std::u16string(units.begin(), units.begin() + r.count);
        }
        at += size;
    }
    return std::nullopt;
}

// ---- Per-manager state ----------------------------------------------------------------------------

namespace {

struct TransactionObject {
    std::string name;
};

struct ProductOpsState {
    std::mutex mutex;
    ProductOpsPlatform platform;
    TransactionInfo transaction;
    std::shared_ptr<TransactionObject> transaction_object;
};

ProductOpsState& state_of(MsiManager& mgr) {
    static std::mutex mutex;
    static std::unordered_map<MsiManager*, std::unique_ptr<ProductOpsState>> states;
    std::lock_guard lock(mutex);
    auto& slot = states[&mgr];
    if (!slot) slot = std::make_unique<ProductOpsState>();
    return *slot;
}

}  // namespace

void set_product_ops_platform(MsiManager& mgr, ProductOpsPlatform platform) {
    ProductOpsState& s = state_of(mgr);
    std::lock_guard lock(s.mutex);
    s.platform = std::move(platform);
}

TransactionInfo current_transaction(MsiManager& mgr) {
    ProductOpsState& s = state_of(mgr);
    std::lock_guard lock(s.mutex);
    return s.transaction;
}


// ---- Product operations: the installed product, and the engine run ---------------------------

namespace {

/// Installed-product facts product operations need, from the installed-
/// products database (msi_register.hpp): the instance the calling user sees
/// (managed, unmanaged, then machine; advertised or installed).
struct ProductFacts {
    bool known{false};
    std::string local_package;  ///< the cached package (InstallProperties\LocalPackage)
    std::vector<std::string> features;  ///< published features
};

ProductFacts product_facts(MsiManager& mgr, const std::string& product) {
    ProductFacts f;
    auto db = product_database(mgr);
    if (!db) return f;
    const auto instance = db->visible(upper(product));
    if (!instance) return f;
    f.known = true;
    f.local_package = db->local_package(*instance);
    for (const auto& [feature, parent] : db->features(*instance)) f.features.push_back(feature);
    return f;
}

std::string quoted(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    return out + "\"";
}

std::string prop(const std::string& name, const std::string& value) { return " " + name + "=" + quoted(value); }

/// Run an installation as its client, the path msiexec /i takes (Learn: the
/// Msi*Product functions "run an installation" with the current
/// MsiSetInternalUI / MsiSetExternalUI / MsiEnableLog settings).
u32 run_engine(MsiManager& mgr, const std::string& arguments) {
    // The request is written as msiexec arguments (package, PROPERTY=value
    // pairs, /j target, /t transforms, /g language) and parsed by the engine's
    // own command-line parser; the session runs with MsiSetInternalUI's level.
    constexpr u32 kInvalidCommandLine = 1639;  // ERROR_INVALID_COMMAND_LINE
    const auto cmd = parse_msiexec_command_line("msiexec.exe " + arguments);
    if (!cmd || !cmd->invalid.empty() || cmd->package.empty()) return kInvalidCommandLine;
    std::map<std::string, std::string> properties = cmd->properties;
    std::string action = "INSTALL";
    if (cmd->mode == MsiexecCommand::Mode::Advertise) {
        action = "ADVERTISE";
        // ADVERTISEFLAGS_MACHINEASSIGN / _USERASSIGN: a per-machine or a
        // per-user advertisement (Learn "ALLUSERS").
        if (cmd->advertise_target == 'm') properties["ALLUSERS"] = "1";
        else properties.erase("ALLUSERS");
    } else if (cmd->mode != MsiexecCommand::Mode::Install) {
        return kInvalidCommandLine;
    }
    if (!cmd->transforms.empty()) properties["TRANSFORMS"] = cmd->transforms;
    if (!cmd->language.empty()) properties["ProductLanguage"] = cmd->language;
    InstallInvocation inv;
    inv.package_path = absolute_package_path(mgr, cmd->package);
    {
        std::vector<u8> bytes;
        if (read_file(mgr.vfs(), inv.package_path, bytes) != FileProblem::None) return kErrPackageOpenFailed;
        auto opened = Package::open(std::span<const u8>(bytes.data(), bytes.size()));
        if (!opened) return kErrPackageInvalid;
        inv.package = std::make_shared<Package>(std::move(*opened));
    }
    inv.action = action;
    inv.properties = properties;
    inv.ui_level = mgr.engine_ui_level();
    // MsiEnableLog: "all subsequent installations initiated in the calling
    // process" log there.
    apply_enable_log(mgr, inv);
    // The client/service split (msi_service.hpp): the install runs in the
    // Windows Installer service, the UI and external handlers here.
    return install_package(mgr, inv);
}

/// Applying or removing a patch package needs the engine's patch support
/// (the .msp's transforms applied to the product database, the PATCH /
/// MSIPATCHREMOVE properties processed), which the engine does not have.
u32 patch_engine_unavailable(const char* api) {
    log_unimplemented(api, "applying/removing patch packages (engine patch support)");
    return kErrInstallFailure;
}

/// Open the package at @p path for the product operations that validate it
/// before running (probe: missing file 2, "" 3, not a package / no
/// ProductCode 1620).
u32 open_install_package(MsiManager& mgr, const std::string& path, std::shared_ptr<Package>* out = nullptr) {
    std::vector<u8> bytes;
    switch (read_file(mgr.vfs(), path, bytes)) {
        case FileProblem::None: break;
        case FileProblem::Empty: return kErrPathNotFound;
        case FileProblem::NotFound: return kErrFileNotFound;
        case FileProblem::PathNotFound: return kErrPathNotFound;
        case FileProblem::AccessDenied: return kErrAccessDenied;
        case FileProblem::Directory: return kErrAccessDenied;
        case FileProblem::Other: return kErrPackageOpenFailed;
    }
    auto package = Package::open(std::span<const u8>(bytes.data(), bytes.size()));
    if (!package) return kErrPackageInvalid;
    auto shared = std::make_shared<Package>(std::move(*package));
    const Table* property = shared->table("Property");
    bool has_code = false;
    if (property != nullptr) {
        const auto key = property->column_index("Property");
        const auto value = property->column_index("Value");
        if (key && value)
            for (const auto& row : property->rows)
                if (row[*key] == "ProductCode" && !row[*value].empty()) has_code = true;
    }
    if (!has_code) return kErrPackageInvalid;
    if (out) *out = std::move(shared);
    return kOk;
}

/// The product's cached package, opened (for feature / component / file
/// lookups that precede the engine run).
std::shared_ptr<Package> product_package(MsiManager& mgr, const ProductFacts& facts) {
    if (facts.local_package.empty()) return {};
    std::shared_ptr<Package> package;
    if (open_install_package(mgr, facts.local_package, &package) != kOk) return {};
    return package;
}

std::optional<usize> col(const Table* t, std::string_view name) { return t ? t->column_index(name) : std::nullopt; }

bool package_has_feature(Package& package, const std::string& feature);

/// A feature of the product: published (Features key) or in its package.
bool product_has_feature(const ProductFacts& facts, Package& package, const std::string& feature) {
    if (std::find(facts.features.begin(), facts.features.end(), feature) != facts.features.end()) return true;
    return package_has_feature(package, feature);
}

bool package_has_feature(Package& package, const std::string& feature) {
    const Table* t = package.table("Feature");
    const auto c = col(t, "Feature");
    if (!c) return false;
    for (const auto& row : t->rows)
        if (row[*c] == feature) return true;
    return false;
}

/// Features that contain component key @p component (FeatureComponents),
/// in Feature table order.
std::vector<std::string> features_of_component(Package& package, const std::string& component_key) {
    std::vector<std::string> out;
    const Table* fc = package.table("FeatureComponents");
    const auto cf = col(fc, "Feature_"), cc = col(fc, "Component_");
    if (!cf || !cc) return out;
    std::set<std::string> owners;
    for (const auto& row : fc->rows)
        if (row[*cc] == component_key) owners.insert(row[*cf]);
    const Table* features = package.table("Feature");
    const auto ff = col(features, "Feature");
    if (ff)
        for (const auto& row : features->rows)
            if (owners.count(row[*ff])) out.push_back(row[*ff]);
    return out;
}

/// REINSTALLMODE letters for a REINSTALLMODE_* mask (Learn "REINSTALLMODE
/// property" / "MsiReinstallProduct"); REINSTALLMODE_REPAIR has no letter.
std::string reinstall_letters(u32 mode) {
    static constexpr std::pair<u32, char> kLetters[] = {
        {0x002, 'p'}, {0x004, 'o'}, {0x008, 'e'}, {0x010, 'd'}, {0x020, 'c'}, {0x040, 'a'},
        {0x080, 'm'}, {0x100, 'u'}, {0x200, 's'}, {0x400, 'v'},
    };
    std::string out;
    for (auto [bit, letter] : kLetters)
        if (mode & bit) out.push_back(letter);
    return out;
}

bool valid_reinstall_mode(u32 mode) { return mode != 0 && (mode & ~kReinstallModeMask) == 0; }
bool valid_product_state(i32 state) { return state >= kStateAdvertised && state <= kStateDefault; }

/// The feature-option property for a feature-level state.
const char* feature_option(i32 state) {
    switch (state) {
        case kStateLocal: return "ADDLOCAL";
        case kStateSource: return "ADDSOURCE";
        case kStateDefault: return "ADDDEFAULT";
        case kStateAbsent: return "REMOVE";
        case kStateAdvertised: return "ADVERTISE";
        default: return nullptr;
    }
}

u32 configure_product(MsiManager& mgr, const std::optional<std::string>& product, i32 level, i32 state,
                      const std::optional<std::string>& command_line) {
    if (!product || !is_guid(*product)) return kErrInvalidParameter;
    if (!valid_product_state(state)) return kErrInvalidParameter;
    if (level < 0 || static_cast<u32>(level) > kInstallLevelMaximum) return kErrInvalidParameter;
    const ProductFacts facts = product_facts(mgr, *product);
    if (!facts.known) return kErrUnknownProduct;
    if (facts.local_package.empty()) {
        log_unimplemented("MsiConfigureProductEx", "product with no cached package (source resolution)");
        return 1610;  // ERROR_BAD_CONFIGURATION
    }
    std::string args = "/i " + quoted(facts.local_package);
    if (state == kStateDefault) {
        if (level != 0) args += prop("INSTALLLEVEL", std::to_string(level));
    } else {
        args += prop(feature_option(state), "ALL");
    }
    if (command_line && !command_line->empty()) args += " " + *command_line;
    return run_engine(mgr, args);
}

u32 configure_feature(MsiManager& mgr, const std::optional<std::string>& product,
                      const std::optional<std::string>& feature, i32 state) {
    if (!product || !is_guid(*product)) return kErrInvalidParameter;
    if (!feature || feature->empty() || to_u16(*feature).size() > kMaxFeatureChars) return kErrInvalidParameter;
    if (!valid_product_state(state)) return kErrInvalidParameter;
    const ProductFacts facts = product_facts(mgr, *product);
    if (!facts.known) return kErrUnknownProduct;
    auto package = product_package(mgr, facts);
    if (!package) {
        log_unimplemented("MsiConfigureFeature", "product with no cached package (source resolution)");
        return 1610;
    }
    if (!product_has_feature(facts, *package, *feature)) return kErrUnknownFeature;
    return run_engine(mgr, "/i " + quoted(facts.local_package) + prop(feature_option(state), *feature));
}

u32 reinstall(MsiManager& mgr, const std::optional<std::string>& product, const std::string* feature, u32 mode) {
    if (!product || !is_guid(*product)) return kErrInvalidParameter;
    if (!valid_reinstall_mode(mode)) return kErrInvalidParameter;
    const ProductFacts facts = product_facts(mgr, *product);
    if (!facts.known) return kErrUnknownProduct;
    auto package = product_package(mgr, facts);
    if (!package) {
        log_unimplemented("MsiReinstallProduct", "product with no cached package (source resolution)");
        return 1610;
    }
    if (feature && !product_has_feature(facts, *package, *feature)) return kErrUnknownFeature;
    std::string args = "/i " + quoted(facts.local_package) + prop("REINSTALL", feature ? *feature : "ALL");
    const std::string letters = reinstall_letters(mode);
    if (!letters.empty()) args += prop("REINSTALLMODE", letters);
    return run_engine(mgr, args);
}

/// Install the feature that holds component key @p key (MsiInstallMissing*:
/// "the product feature that requires the least additional disk space is
/// installed"), and repair what is missing in it.
u32 install_component_feature(MsiManager& mgr, const ProductFacts& facts, Package& package,
                              const std::string& key, i32 state) {
    const std::vector<std::string> features = features_of_component(package, key);
    if (features.empty()) return 1610;  // ERROR_BAD_CONFIGURATION: a component with no feature
    if (features.size() > 1)
        log_unimplemented("MsiInstallMissingComponent", "least-disk-cost choice among several owning features (first used)");
    const std::string& feature = features.front();
    std::string args = "/i " + quoted(facts.local_package) + prop(feature_option(state), feature) +
                       prop("REINSTALL", feature) + prop("REINSTALLMODE", "p");
    return run_engine(mgr, args);
}

// ---- Patch packages ---------------------------------------------------------------------------

constexpr std::array<u8, 16> kPatchClsid = {0x86, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

/// The string properties of an OLE property set stream (Learn "Summary
/// Information Stream Property Set": VT_LPSTR values in the stream's code
/// page). Only the PIDs patches carry are read.
std::map<u32, std::string> summary_strings(const std::vector<u8>& stream) {
    std::map<u32, std::string> out;
    const Bytes in{stream};
    if (!in.has(0, 48) || in.u16le(0) != 0xFFFE) return out;
    const usize section = in.u32le(44);
    if (!in.has(section, 8)) return out;
    const u32 count = in.u32le(section + 4);
    for (u32 i = 0; i < count && i < 64; ++i) {
        const u32 pid = in.u32le(section + 8 + i * 8);
        const usize at = section + in.u32le(section + 12 + i * 8);
        if (!in.has(at, 8) || in.u32le(at) != 30) continue;  // VT_LPSTR
        const u32 len = in.u32le(at + 4);
        if (!in.has(at + 8, len)) continue;
        std::string s(reinterpret_cast<const char*>(stream.data() + at + 8), len);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        out[pid] = s;
    }
    return out;
}

struct PatchPackage {
    std::shared_ptr<Package> package;
    std::vector<std::string> targets;  ///< PID_TEMPLATE: target ProductCodes
    std::string patch_code;            ///< first GUID of PID_REVNUMBER
    bool sequenced{false};             ///< carries MsiPatchSequence rows (Windows Installer 3.0 sequencing)
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

/// Open @p path as a patch package (root storage CLSID of a patch, Learn
/// "Patch Packages"). The probe: a missing file, a text file and a database
/// are all ERROR_PATCH_PACKAGE_INVALID for MsiApplyPatch.
std::optional<PatchPackage> open_patch(MsiManager& mgr, const std::string& path, FileProblem* problem = nullptr) {
    std::vector<u8> bytes;
    const FileProblem p = read_file(mgr.vfs(), path, bytes);
    if (problem) *problem = p;
    if (p != FileProblem::None) return std::nullopt;
    auto package = Package::open(std::span<const u8>(bytes.data(), bytes.size()));
    if (!package || package->clsid() != kPatchClsid) return std::nullopt;
    PatchPackage out;
    out.package = std::make_shared<Package>(std::move(*package));
    if (const auto* si = out.package->stream("\x05SummaryInformation")) {
        const auto props = summary_strings(*si);
        if (auto it = props.find(7); it != props.end())
            for (const auto& t : split(it->second, ';'))
                if (is_guid(t)) out.targets.push_back(upper(t));
        if (auto it = props.find(9); it != props.end() && it->second.size() >= 38) out.patch_code = upper(it->second.substr(0, 38));
    }
    if (const Table* seq = out.package->table("MsiPatchSequence")) out.sequenced = !seq->rows.empty();
    return out;
}

std::string package_product_code(Package& package) {
    const Table* t = package.table("Property");
    const auto k = col(t, "Property"), v = col(t, "Value");
    if (!k || !v) return {};
    for (const auto& row : t->rows)
        if (row[*k] == "ProductCode") return upper(row[*v]);
    return {};
}

}  // namespace

// ---- Exports ---------------------------------------------------------------------------------------

namespace {

GuestAddr P(Marshaler& m, usize i) { return m.arg_ptr(i); }
i32 I(Marshaler& m, usize i) { return static_cast<i32>(m.arg32(i)); }
u32 U(Marshaler& m, usize i) { return m.arg32(i); }

/// Register an A/W pair implemented once.
void pair(HandlerMap& map, const std::string& base, const std::function<void(Marshaler&, bool wide)>& fn) {
    map[base + "A"] = [fn](Marshaler& m) { fn(m, false); };
    map[base + "W"] = [fn](Marshaler& m) { fn(m, true); };
}

// ---- MsiLoadString ----------------------------------------------------------------------------

/// A guest module's string-table entry (RT_STRING), read from its mapped
/// image: the language chosen and the text. Language order (probe
/// loadstring lines): the requested language, the user default language,
/// language-neutral, then any.
std::optional<std::pair<u16, std::u16string>> load_module_string(Marshaler& m, GuestAddr base, u32 id, u16 wanted) {
    auto r16 = [&](GuestAddr a) -> std::optional<u16> {
        auto v = m.read_u16(a);
        if (v.is_err()) return std::nullopt;
        return v.value();
    };
    auto r32 = [&](GuestAddr a) -> std::optional<u32> {
        auto v = m.read_u32(a);
        if (v.is_err()) return std::nullopt;
        return v.value();
    };
    if (base == 0 || r16(base) != 0x5A4D) return std::nullopt;
    const auto lfanew = r32(base + 0x3C);
    if (!lfanew || r32(base + *lfanew) != 0x00004550u) return std::nullopt;
    const GuestAddr opt = base + *lfanew + 24;
    const auto magic = r16(opt);
    if (!magic) return std::nullopt;
    const GuestAddr dirs = opt + (*magic == 0x20B ? 112 : 96);
    const auto rsrc = r32(dirs + 2 * 8);
    if (!rsrc || *rsrc == 0) return std::nullopt;
    const GuestAddr root = base + *rsrc;
    // One level of IMAGE_RESOURCE_DIRECTORY: the entry for @p want_id (or
    // every id entry when want_id is nullopt).
    auto entries = [&](GuestAddr dir) {
        std::vector<std::pair<u32, u32>> out;
        const auto named = r16(dir + 12), ids = r16(dir + 14);
        if (!named || !ids) return out;
        for (u32 i = 0; i < static_cast<u32>(*named) + *ids && i < 4096; ++i) {
            const auto name = r32(dir + 16 + i * 8), off = r32(dir + 20 + i * 8);
            if (!name || !off) break;
            if (*name & 0x80000000u) continue;  // named entries never match an integer id
            out.emplace_back(*name, *off);
        }
        return out;
    };
    auto find = [&](GuestAddr dir, u32 want, bool subdir) -> std::optional<GuestAddr> {
        for (auto [name, off] : entries(dir))
            if (name == want && (((off & 0x80000000u) != 0) == subdir)) return root + (off & 0x7FFFFFFFu);
        return std::nullopt;
    };
    const auto types = find(root, 6, true);  // RT_STRING
    if (!types) return std::nullopt;
    const auto block = find(*types, (id >> 4) + 1, true);
    if (!block) return std::nullopt;
    const auto langs = entries(*block);
    if (langs.empty()) return std::nullopt;
    // A string table block is per language; a string missing from the
    // preferred language's block falls back to the next language (probe:
    // id 1 asked in German, whose block holds only id 2, loads English).
    std::vector<u16> order;
    if (wanted != 0) order.push_back(wanted);
    order.push_back(0x0409);  // user default language (kernel32 GetUserDefaultLangID)
    order.push_back(0x0000);
    for (const auto& e : langs) order.push_back(static_cast<u16>(e.first));
    std::set<u16> tried;
    for (u16 want : order) {
        if (!tried.insert(want).second) continue;
        const auto it = std::find_if(langs.begin(), langs.end(), [&](const auto& e) {
            return e.first == want && !(e.second & 0x80000000u);
        });
        if (it == langs.end()) continue;
        const GuestAddr data = root + (it->second & 0x7FFFFFFFu);
        const auto rva = r32(data), size = r32(data + 4);
        if (!rva || !size) continue;
        GuestAddr at = base + *rva;
        const GuestAddr end = at + *size;
        bool ok = true;
        for (u32 k = 0; k < (id & 15) && ok; ++k) {
            const auto len = r16(at);
            if (!len || at >= end) ok = false;
            else at += 2 + static_cast<GuestAddr>(*len) * 2;
        }
        const auto len = ok ? r16(at) : std::nullopt;
        if (!len || *len == 0) continue;
        std::u16string text;
        for (u16 i = 0; i < *len && ok; ++i) {
            const auto c = r16(at + 2 + i * 2);
            if (!c) ok = false;
            else text.push_back(static_cast<char16_t>(*c));
        }
        if (ok) return std::make_pair(want, text);
    }
    return std::nullopt;
}

}  // namespace

void register_product_ops_api(MsiManager& mgr, HandlerMap& map) {
    MsiManager* const pm = &mgr;

    // ---- Install / configure / reinstall -----------------------------------------------------
    // UINT MsiInstallProduct(LPCTSTR szPackagePath, LPCTSTR szCommandLine)
    pair(map, "MsiInstallProduct", [pm](Marshaler& m, bool w) {
        const auto package = read_in8(m, P(m, 0), w);
        const auto command = read_in8(m, P(m, 1), w);
        if (!package) return m.set_return(kErrInvalidParameter);
        if (const u32 e = open_install_package(*pm, *package); e != kOk) return m.set_return(e);
        std::string args = "/i " + quoted(*package);
        if (command && !command->empty()) args += " " + *command;
        m.set_return(run_engine(*pm, args));
    });
    // UINT MsiConfigureProduct(LPCTSTR szProduct, int iInstallLevel, INSTALLSTATE eInstallState)
    pair(map, "MsiConfigureProduct", [pm](Marshaler& m, bool w) {
        m.set_return(configure_product(*pm, read_in8(m, P(m, 0), w), I(m, 1), I(m, 2), std::nullopt));
    });
    // UINT MsiConfigureProductEx(LPCTSTR szProduct, int iInstallLevel, INSTALLSTATE eInstallState, LPCTSTR szCommandLine)
    pair(map, "MsiConfigureProductEx", [pm](Marshaler& m, bool w) {
        m.set_return(configure_product(*pm, read_in8(m, P(m, 0), w), I(m, 1), I(m, 2), read_in8(m, P(m, 3), w)));
    });
    // UINT MsiConfigureFeature(LPCTSTR szProduct, LPCTSTR szFeature, INSTALLSTATE eInstallState)
    pair(map, "MsiConfigureFeature", [pm](Marshaler& m, bool w) {
        m.set_return(configure_feature(*pm, read_in8(m, P(m, 0), w), read_in8(m, P(m, 1), w), I(m, 2)));
    });
    // UINT MsiReinstallProduct(LPCTSTR szProduct, DWORD szReinstallMode)
    pair(map, "MsiReinstallProduct", [pm](Marshaler& m, bool w) {
        m.set_return(reinstall(*pm, read_in8(m, P(m, 0), w), nullptr, U(m, 1)));
    });
    // UINT MsiReinstallFeature(LPCTSTR szProduct, LPCTSTR szFeature, DWORD dwReinstallMode)
    // (probe: a NULL feature is ERROR_INVALID_PARAMETER).
    pair(map, "MsiReinstallFeature", [pm](Marshaler& m, bool w) {
        const auto product = read_in8(m, P(m, 0), w);
        const auto feature = read_in8(m, P(m, 1), w);
        if (!product || !is_guid(*product) || !feature || feature->empty() ||
            to_u16(*feature).size() > kMaxFeatureChars)
            return m.set_return(kErrInvalidParameter);
        m.set_return(reinstall(*pm, product, &*feature, U(m, 2)));
    });
    // UINT Msi{Configure,Reinstall}FeatureFromDescriptor(LPCTSTR szDescriptor, INSTALLSTATE|DWORD)
    // (undocumented; probe cffd/rffd lines: a descriptor that does not
    // decompose is ERROR_INVALID_PARAMETER, then the feature operation's own rules).
    pair(map, "MsiConfigureFeatureFromDescriptor", [pm](Marshaler& m, bool w) {
        const auto text = read_in(m, P(m, 0), w);
        Descriptor d;
        if (!text || decompose_descriptor(*text, d) != kOk) return m.set_return(kErrInvalidParameter);
        m.set_return(configure_feature(*pm, d.product, d.feature, I(m, 1)));
    });
    pair(map, "MsiReinstallFeatureFromDescriptor", [pm](Marshaler& m, bool w) {
        const auto text = read_in(m, P(m, 0), w);
        Descriptor d;
        if (!text || decompose_descriptor(*text, d) != kOk) return m.set_return(kErrInvalidParameter);
        m.set_return(reinstall(*pm, d.product, &d.feature, U(m, 1)));
    });
    // UINT MsiInstallMissingComponent(LPCTSTR szProduct, LPCTSTR szComponent, INSTALLSTATE eInstallState)
    pair(map, "MsiInstallMissingComponent", [pm](Marshaler& m, bool w) {
        const auto product = read_in8(m, P(m, 0), w);
        const auto component = read_in8(m, P(m, 1), w);
        const i32 state = I(m, 2);
        if (!product || !is_guid(*product) || !component || !is_guid(*component)) return m.set_return(kErrInvalidParameter);
        if (state != kStateLocal && state != kStateSource && state != kStateDefault) return m.set_return(kErrInvalidParameter);
        const ProductFacts facts = product_facts(*pm, *product);
        if (!facts.known) return m.set_return(kErrUnknownProduct);
        auto package = product_package(*pm, facts);
        if (!package) {
            log_unimplemented("MsiInstallMissingComponent", "product with no cached package (source resolution)");
            return m.set_return(1610u);
        }
        const Table* t = package->table("Component");
        const auto ck = col(t, "Component"), cid = col(t, "ComponentId");
        std::optional<std::string> key;
        if (ck && cid)
            for (const auto& row : t->rows)
                if (ieq(row[*cid], *component)) key = row[*ck];
        if (!key) return m.set_return(kErrUnknownComponent);
        m.set_return(install_component_feature(*pm, facts, *package, *key, state));
    });
    // UINT MsiInstallMissingFile(LPCTSTR szProduct, LPCTSTR szFile): the file's
    // component from the File table, then as MsiInstallMissingComponent.
    pair(map, "MsiInstallMissingFile", [pm](Marshaler& m, bool w) {
        const auto product = read_in8(m, P(m, 0), w);
        const auto file = read_in8(m, P(m, 1), w);
        if (!product || !is_guid(*product) || !file) return m.set_return(kErrInvalidParameter);
        const ProductFacts facts = product_facts(*pm, *product);
        if (!facts.known) return m.set_return(kErrUnknownProduct);
        auto package = product_package(*pm, facts);
        if (!package) {
            log_unimplemented("MsiInstallMissingFile", "product with no cached package (source resolution)");
            return m.set_return(1610u);
        }
        const Table* t = package->table("File");
        const auto fc = col(t, "Component_"), fn = col(t, "FileName");
        std::optional<std::string> key;
        if (fc && fn)
            for (const auto& row : t->rows) {
                // FileName is "short|long" or one name (Learn "Filename").
                const std::string& cell = row[*fn];
                const usize bar = cell.find('|');
                const std::string long_name = bar == std::string::npos ? cell : cell.substr(bar + 1);
                const std::string short_name = bar == std::string::npos ? cell : cell.substr(0, bar);
                if (ieq(long_name, *file) || ieq(short_name, *file)) { key = row[*fc]; break; }
            }
        if (!key) return m.set_return(kErrFileNotFound);
        m.set_return(install_component_feature(*pm, facts, *package, *key, kStateDefault));
    });
    // UINT MsiCollectUserInfo(LPCTSTR szProduct): the package's FirstRun dialog.
    pair(map, "MsiCollectUserInfo", [pm](Marshaler& m, bool w) {
        const auto product = read_in8(m, P(m, 0), w);
        if (!product || !is_guid(*product)) return m.set_return(kErrInvalidParameter);
        if (!product_facts(*pm, *product).known) return m.set_return(kErrUnknownProduct);
        log_unimplemented(w ? "MsiCollectUserInfoW" : "MsiCollectUserInfoA", "FirstRun dialog sequence (needs an engine entry that runs one dialog)");
        m.set_return(kErrInstallFailure);
    });

    // ---- Advertising ---------------------------------------------------------------------------
    // UINT MsiAdvertiseProductEx(LPCTSTR szPackagePath, LPCTSTR szScriptfilePath, LPCTSTR szTransforms,
    //                            LANGID lgidLanguage, DWORD dwPlatform, DWORD dwOptions)
    // szScriptfilePath is ADVERTISEFLAGS_MACHINEASSIGN (0) / _USERASSIGN (1)
    // for local advertisement, else the script to write. Probe: a platform
    // other than 0 for local advertisement, more than one platform bit, an
    // unknown bit, an unknown option, or MSIADVERTISEOPTIONFLAGS_INSTANCE
    // without transforms are ERROR_INVALID_PARAMETER before the package is read.
    auto advertise = [pm](Marshaler& m, bool w, bool ex) {
        const auto package = read_in8(m, P(m, 0), w);
        const GuestAddr script_ptr = P(m, 1);
        const auto transforms = read_in8(m, P(m, 2), w);
        const u32 language = U(m, 3) & 0xFFFF;
        const u32 platform = ex ? U(m, 4) : 0;
        const u32 options = ex ? U(m, 5) : 0;
        if (!package) return m.set_return(kErrInvalidParameter);
        const bool local = script_ptr <= 1;
        if (platform != 0 && (local || (platform & (platform - 1)) != 0 || (platform & ~0xFu) != 0))
            return m.set_return(kErrInvalidParameter);
        if ((options & ~1u) != 0) return m.set_return(kErrInvalidParameter);
        const bool instance = (options & 1u) != 0;
        if (instance && (!transforms || transforms->empty())) return m.set_return(kErrInvalidParameter);
        if (const u32 e = open_install_package(*pm, *package); e != kOk) return m.set_return(e);
        if (!local) {
            log_unimplemented(w ? (ex ? "MsiAdvertiseProductExW" : "MsiAdvertiseProductW")
                                : (ex ? "MsiAdvertiseProductExA" : "MsiAdvertiseProductA"),
                              "advertise script generation");
            return m.set_return(kErrCallNotImplemented);
        }
        std::string args = std::string(script_ptr == 1 ? "/ju " : "/jm ") + quoted(*package);
        if (transforms && !transforms->empty()) args += " /t " + quoted(*transforms);
        if (language != 0) args += " /g " + std::to_string(language);
        if (instance) args += prop("MSINEWINSTANCE", "1");
        m.set_return(run_engine(*pm, args));
    };
    pair(map, "MsiAdvertiseProduct", [advertise](Marshaler& m, bool w) { advertise(m, w, false); });
    pair(map, "MsiAdvertiseProductEx", [advertise](Marshaler& m, bool w) { advertise(m, w, true); });
    // UINT MsiAdvertiseScript(LPCTSTR szScriptFile, DWORD dwFlags, PHKEY phRegData, BOOL fRemoveItems)
    // Learn: callable only from the LocalSystem account; the probe (an
    // elevated administrator) gets ERROR_ACCESS_DENIED for every input. A
    // capsule guest never runs as LocalSystem.
    pair(map, "MsiAdvertiseScript", [](Marshaler& m, bool) { m.set_return(kErrAccessDenied); });
    // UINT MsiProcessAdvertiseScript(LPCTSTR szScriptFile, LPCTSTR szIconFolder, HKEY hRegData,
    //                                BOOL fShortcuts, BOOL fRemoveItems): same rule; a NULL script is 87.
    pair(map, "MsiProcessAdvertiseScript", [](Marshaler& m, bool) {
        m.set_return(P(m, 0) == 0 ? kErrInvalidParameter : kErrAccessDenied);
    });

    // ---- Patches -----------------------------------------------------------------------------------
    // UINT MsiApplyPatch(LPCTSTR szPatchPackage, LPCTSTR szInstallPackage, INSTALLTYPE eInstallType,
    //                    LPCTSTR szCommandLine)
    pair(map, "MsiApplyPatch", [pm](Marshaler& m, bool w) {
        const auto patch_path = read_in8(m, P(m, 0), w);
        const auto install = read_in8(m, P(m, 1), w);
        const u32 type = U(m, 2);
        const auto command = read_in8(m, P(m, 3), w);
        if (!patch_path || type > kInstallTypeSingleInstance) return m.set_return(kErrInvalidParameter);
        if (type != kInstallTypeDefault && !install) return m.set_return(kErrInvalidParameter);
        const auto patch = open_patch(*pm, *patch_path);
        if (!patch) return m.set_return(kErrPatchPackageInvalid);
        const std::string extra = (command && !command->empty()) ? " " + *command : std::string();
        if (type == kInstallTypeNetworkImage)
            return m.set_return(patch_engine_unavailable(w ? "MsiApplyPatchW" : "MsiApplyPatchA"));
        std::vector<std::string> targets;
        if (type == kInstallTypeSingleInstance) {
            if (!is_guid(*install)) return m.set_return(kErrInvalidParameter);
            targets.push_back(upper(*install));
        } else {
            targets = patch->targets;
        }
        u32 result = kErrPatchTargetNotFound;
        for (const std::string& target : targets) {
            const ProductFacts facts = product_facts(*pm, target);
            if (!facts.known) continue;
            if (std::find(patch->targets.begin(), patch->targets.end(), target) == patch->targets.end()) continue;
            (void)extra;
            result = patch_engine_unavailable(w ? "MsiApplyPatchW" : "MsiApplyPatchA");
            break;
        }
        m.set_return(result);
    });
    // UINT MsiApplyMultiplePatches(LPCTSTR szPatchPackages, LPCTSTR szProductCode, LPCTSTR szPropertiesList)
    pair(map, "MsiApplyMultiplePatches", [pm](Marshaler& m, bool w) {
        const auto list = read_in8(m, P(m, 0), w);
        const auto product = read_in8(m, P(m, 1), w);
        const auto props = read_in8(m, P(m, 2), w);
        if (!list || list->empty()) return m.set_return(kErrInvalidParameter);
        if (product && !is_guid(*product)) return m.set_return(kErrInvalidParameter);
        if (product && !product_facts(*pm, *product).known) return m.set_return(kErrUnknownProduct);
        std::set<std::string> targets;
        for (const std::string& path : split(*list, ';')) {
            if (path.empty()) continue;
            FileProblem problem = FileProblem::None;
            const auto patch = open_patch(*pm, path, &problem);
            if (problem == FileProblem::NotFound) return m.set_return(kErrFileNotFound);
            if (problem == FileProblem::PathNotFound || problem == FileProblem::Empty) return m.set_return(kErrPathNotFound);
            if (!patch) return m.set_return(kErrPatchPackageInvalid);
            for (const auto& t : patch->targets)
                if (!product || upper(*product) == t) targets.insert(t);
        }
        u32 result = kErrPatchTargetNotFound;
        for (const std::string& target : targets) {
            const ProductFacts facts = product_facts(*pm, target);
            if (!facts.known) continue;
            (void)props;
            result = patch_engine_unavailable(w ? "MsiApplyMultiplePatchesW" : "MsiApplyMultiplePatchesA");
            break;
        }
        m.set_return(result);
    });
    // UINT MsiRemovePatches(LPCTSTR szPatchList, LPCTSTR szProductCode, INSTALLTYPE eUninstallType,
    //                       LPCTSTR szPropertyList): eUninstallType must be INSTALLTYPE_SINGLE_INSTANCE.
    pair(map, "MsiRemovePatches", [pm](Marshaler& m, bool w) {
        const auto list = read_in8(m, P(m, 0), w);
        const auto product = read_in8(m, P(m, 1), w);
        const auto props = read_in8(m, P(m, 3), w);
        if (!list || list->empty() || !product || !is_guid(*product) || U(m, 2) != kInstallTypeSingleInstance)
            return m.set_return(kErrInvalidParameter);
        const ProductFacts facts = product_facts(*pm, *product);
        if (!facts.known) return m.set_return(kErrUnknownProduct);
        (void)props;
        m.set_return(patch_engine_unavailable(w ? "MsiRemovePatchesW" : "MsiRemovePatchesA"));
    });
    // UINT MsiExtractPatchXMLData(LPCTSTR szPatchPath, DWORD dwReserved, LPTSTR szXMLData, LPDWORD pcchXMLData)
    pair(map, "MsiExtractPatchXMLData", [pm](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        if (!path || U(m, 1) != 0 || (P(m, 2) != 0 && P(m, 3) == 0)) return m.set_return(kErrInvalidParameter);
        FileProblem problem = FileProblem::None;
        const auto patch = open_patch(*pm, *path, &problem);
        if (problem == FileProblem::NotFound || problem == FileProblem::Empty) return m.set_return(kErrFileNotFound);
        if (problem == FileProblem::PathNotFound) return m.set_return(kErrPathNotFound);
        if (!patch) return m.set_return(kErrPatchPackageInvalid);
        log_unimplemented(w ? "MsiExtractPatchXMLDataW" : "MsiExtractPatchXMLDataA", "patch applicability XML generation");
        m.set_return(kErrFunctionFailed);
    });
    // UINT MsiDetermineApplicablePatches(LPCTSTR szProductPackagePath, DWORD cPatchInfo,
    //                                    PMSIPATCHSEQUENCEINFO pPatchInfo)
    // UINT MsiDeterminePatchSequence(LPCTSTR szProductCode, LPCTSTR szUserSid, MSIINSTALLCONTEXT dwContext,
    //                                DWORD cPatchInfo, PMSIPATCHSEQUENCEINFO pPatchInfo)
    // MSIPATCHSEQUENCEINFO { LPCTSTR szPatchData; MSIPATCHDATATYPE ePatchDataType; DWORD dwOrder; UINT uStatus; }
    auto sequence_patches = [pm](Marshaler& m, bool w, const char* api, const std::string& product_code,
                                 u32 count, GuestAddr info) -> u32 {
        const usize ptr = m.cpu_is_64bit() ? 8 : 4;
        const usize stride = m.cpu_is_64bit() ? 24 : 16;
        std::vector<u32> status(count, kOk);
        std::vector<i32> order(count, -1);
        auto flush = [&] {
            for (u32 i = 0; i < count; ++i) {
                (void)m.write_u32(info + i * stride + ptr + 4, static_cast<u32>(order[i]));
                (void)m.write_u32(info + i * stride + ptr + 8, status[i]);
            }
        };
        u32 result = kOk;
        i32 next = 0;
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr entry = info + i * stride;
            const GuestAddr data = m.read_ptr(entry).is_ok() ? static_cast<GuestAddr>(m.read_ptr(entry).value()) : 0;
            const u32 type = m.read_u32(entry + ptr).is_ok() ? m.read_u32(entry + ptr).value() : ~0u;
            const auto text = read_in8(m, data, w);
            if (!text || type > kPatchDataXmlBlob) { result = kErrInvalidParameter; break; }
            if (type != kPatchDataPatchFile) {
                log_unimplemented(api, "patch XML data (MSIPATCH_DATATYPE_XMLPATH / XMLBLOB)");
                status[i] = kErrFunctionFailed;
                result = kErrFunctionFailed;
                break;
            }
            const auto patch = open_patch(*pm, *text);
            if (!patch) {
                status[i] = kErrPackageOpenFailed;
                result = kErrPackageOpenFailed;
                break;
            }
            if (patch->sequenced) {
                log_unimplemented(api, "MsiPatchSequence sequencing / supersedence");
                status[i] = kErrFunctionFailed;
                result = kErrFunctionFailed;
                break;
            }
            if (std::find(patch->targets.begin(), patch->targets.end(), product_code) == patch->targets.end()) {
                status[i] = kErrPatchTargetNotFound;
                continue;
            }
            order[i] = next++;
        }
        if (result != kOk) std::fill(order.begin(), order.end(), -1);
        flush();
        return result;
    };
    pair(map, "MsiDetermineApplicablePatches", [pm, sequence_patches](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        const u32 count = U(m, 1);
        const GuestAddr info = P(m, 2);
        if (!path || count == 0 || info == 0) return m.set_return(kErrInvalidParameter);
        const usize ptr = m.cpu_is_64bit() ? 8 : 4, stride = m.cpu_is_64bit() ? 24 : 16;
        for (u32 i = 0; i < count; ++i) {  // probe: every entry reads dwOrder -1, uStatus 0 even on failure
            (void)m.write_u32(info + i * stride + ptr + 4, 0xFFFFFFFFu);
            (void)m.write_u32(info + i * stride + ptr + 8, 0);
        }
        std::vector<u8> bytes;
        const FileProblem problem = read_file(pm->vfs(), *path, bytes);
        if (problem != FileProblem::None) return m.set_return(kErrOpenFailed);
        auto package = Package::open(std::span<const u8>(bytes.data(), bytes.size()));
        if (!package) return m.set_return(kErrOpenFailed);
        const std::string code = package_product_code(*package);
        if (code.empty()) return m.set_return(kErrPackageInvalid);
        m.set_return(sequence_patches(m, w, w ? "MsiDetermineApplicablePatchesW" : "MsiDetermineApplicablePatchesA",
                                      code, count, info));
    });
    pair(map, "MsiDeterminePatchSequence", [pm, sequence_patches](Marshaler& m, bool w) {
        const auto product = read_in8(m, P(m, 0), w);
        const GuestAddr sid = P(m, 1);
        const u32 context = U(m, 2);
        const u32 count = U(m, 3);
        const GuestAddr info = P(m, 4);
        if (!product || !is_guid(*product)) return m.set_return(kErrInvalidParameter);
        if (context != kContextUserManaged && context != kContextUserUnmanaged && context != kContextMachine)
            return m.set_return(kErrInvalidParameter);
        if (context == kContextMachine && sid != 0) return m.set_return(kErrInvalidParameter);
        if (count == 0 || info == 0) return m.set_return(kErrInvalidParameter);
        const usize ptr = m.cpu_is_64bit() ? 8 : 4, stride = m.cpu_is_64bit() ? 24 : 16;
        for (u32 i = 0; i < count; ++i) {
            (void)m.write_u32(info + i * stride + ptr + 4, 0xFFFFFFFFu);
            (void)m.write_u32(info + i * stride + ptr + 8, 0);
        }
        const ProductFacts facts = product_facts(*pm, *product);
        if (!facts.known) return m.set_return(kErrUnknownProduct);
        m.set_return(sequence_patches(m, w, w ? "MsiDeterminePatchSequenceW" : "MsiDeterminePatchSequenceA",
                                      upper(*product), count, info));
    });
    // UINT MsiGetPatchFileList(LPCTSTR szProductCode, LPCTSTR szPatchPackages, LPDWORD pcFiles,
    //                          MSIHANDLE** pphFileRecords). Probe: no applicable patch (any
    // input, NULL outputs included) is ERROR_SUCCESS with *pcFiles 0 and *pphFileRecords NULL.
    pair(map, "MsiGetPatchFileList", [pm](Marshaler& m, bool w) {
        const auto product = read_in8(m, P(m, 0), w);
        const auto list = read_in8(m, P(m, 1), w);
        const GuestAddr count_out = P(m, 2), records_out = P(m, 3);
        if (count_out) (void)m.write_u32(count_out, 0);
        if (records_out) (void)m.write_ptr(records_out, 0);
        if (product && list && is_guid(*product) && product_facts(*pm, *product).known) {
            for (const std::string& path : split(*list, ';')) {
                const auto patch = open_patch(*pm, path);
                if (patch && std::find(patch->targets.begin(), patch->targets.end(), upper(*product)) != patch->targets.end()) {
                    log_unimplemented(w ? "MsiGetPatchFileListW" : "MsiGetPatchFileListA", "file records of an applicable patch");
                    return m.set_return(kErrFunctionFailed);
                }
            }
        }
        m.set_return(kOk);
    });

    // ---- Verification and file facts -----------------------------------------------------------------
    // UINT MsiVerifyPackage(LPCTSTR szPackagePath). Probe: NULL 87; "", a missing
    // file, a directory, a text file 1619; a database 0.
    pair(map, "MsiVerifyPackage", [pm](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        if (!path) return m.set_return(kErrInvalidParameter);
        std::vector<u8> bytes;
        if (read_file(pm->vfs(), *path, bytes) != FileProblem::None) return m.set_return(kErrPackageOpenFailed);
        m.set_return(Package::open(std::span<const u8>(bytes.data(), bytes.size())) ? kOk : kErrPackageOpenFailed);
    });
    // UINT MsiGetFileVersion(LPCTSTR szFilePath, LPTSTR lpVersionBuf, LPDWORD pcchVersionBuf,
    //                        LPTSTR lpLangBuf, LPDWORD pcchLangBuf)
    pair(map, "MsiGetFileVersion", [pm](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        const GuestAddr vbuf = P(m, 1), vcch = P(m, 2), lbuf = P(m, 3), lcch = P(m, 4);
        if (!path || (vbuf && !vcch) || (lbuf && !lcch)) return m.set_return(kErrInvalidParameter);
        std::vector<u8> bytes;
        const FileProblem problem = read_file(pm->vfs(), *path, bytes);
        if (problem == FileProblem::AccessDenied) return m.set_return(kErrAccessDenied);
        if (problem != FileProblem::None) return m.set_return(kErrFileNotFound);  // probe: "", missing dir, directory
        FileVersion version;
        if (const u32 e = file_version_of_image(bytes, version); e != kOk) return m.set_return(e);
        const u32 a = put_sized(m, w, vbuf, vcch, to_u16(version.version));
        const u32 b = put_sized(m, w, lbuf, lcch, to_u16(version.language));
        m.set_return(a != kOk ? a : b);
    });
    // UINT MsiGetFileHash(LPCTSTR szFilePath, DWORD dwOptions, PMSIFILEHASHINFO pHash)
    // MSIFILEHASHINFO { ULONG dwFileHashInfoSize (must be 20); ULONG dwData[4]; }
    pair(map, "MsiGetFileHash", [pm](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        const GuestAddr info = P(m, 2);
        if (!path || U(m, 1) != 0 || info == 0) return m.set_return(kErrInvalidParameter);
        const auto size = m.read_u32(info);
        if (size.is_err() || size.value() != 20) return m.set_return(kErrInvalidParameter);
        std::vector<u8> bytes;
        switch (read_file(pm->vfs(), *path, bytes)) {
            case FileProblem::None: break;
            case FileProblem::NotFound: return m.set_return(kErrFileNotFound);
            case FileProblem::Empty:
            case FileProblem::PathNotFound: return m.set_return(kErrPathNotFound);
            case FileProblem::Directory:
            case FileProblem::AccessDenied: return m.set_return(kErrAccessDenied);
            case FileProblem::Other: return m.set_return(kHrFail);
        }
        const auto parts = msi_file_hash(bytes);
        for (int i = 0; i < 4; ++i) (void)m.write_u32(info + 4 + i * 4, parts[i]);
        m.set_return(kOk);
    });
    // UINT MsiDecomposeDescriptor(LPCTSTR szDescriptor, LPTSTR szProductCode, LPTSTR szFeatureId,
    //                             LPTSTR szComponentCode, LPDWORD pcchArgsOffset) (undocumented;
    // measured: outputs optional, written as decoded, the offset only on success).
    pair(map, "MsiDecomposeDescriptor", [](Marshaler& m, bool w) {
        const auto text = read_in(m, P(m, 0), w);
        const GuestAddr pbuf = P(m, 1), fbuf = P(m, 2), cbuf = P(m, 3), off = P(m, 4);
        if (!text) return m.set_return(kErrInvalidParameter);
        Descriptor d;
        DescriptorProgress progress;
        const u32 e = decompose_descriptor(*text, d, &progress);
        if (progress.product) put_fixed(m, w, pbuf, to_u16(d.product));
        if (fbuf && progress.feature_chars > 0) {
            const std::vector<u16> units = units_for(to_u16(d.feature), w);
            const usize n = std::min(units.size(), progress.feature_chars);
            if (progress.feature_terminated) write_units(m, fbuf, w, units, n);
            else
                for (usize i = 0; i < n; ++i) {
                    if (w) (void)m.write_u16(fbuf + i * 2, units[i]);
                    else (void)m.write_u8(fbuf + i, static_cast<u8>(units[i]));
                }
        } else if (fbuf && progress.feature_terminated) {
            put_fixed(m, w, fbuf, u"");
        }
        if (progress.component) put_fixed(m, w, cbuf, to_u16(d.component));
        if (e == kOk && off) {
            // The offset counts characters of the caller's string.
            const usize consumed = w ? d.consumed : units_for(text->substr(0, d.consumed), false).size();
            (void)m.write_u32(off, static_cast<u32>(consumed));
        }
        m.set_return(e);
    });
    // UINT MsiGetShortcutTarget(LPCTSTR szShortcutPath, LPTSTR szProductCode, LPTSTR szFeatureId,
    //                           LPTSTR szComponentCode). Probe: anything but a Darwin shortcut whose
    // descriptor decomposes (a plain .lnk, a text file, a missing file, NULL) is ERROR_FUNCTION_FAILED.
    pair(map, "MsiGetShortcutTarget", [pm](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        if (!path) return m.set_return(kErrFunctionFailed);
        std::vector<u8> bytes;
        if (read_file(pm->vfs(), *path, bytes) != FileProblem::None) return m.set_return(kErrFunctionFailed);
        const auto descriptor = darwin_descriptor_of_shortcut(bytes);
        if (!descriptor) return m.set_return(kErrFunctionFailed);
        Descriptor d;
        if (decompose_descriptor(*descriptor, d) != kOk) return m.set_return(kErrFunctionFailed);
        put_fixed(m, w, P(m, 1), to_u16(d.product));
        put_fixed(m, w, P(m, 2), to_u16(d.feature));
        put_fixed(m, w, P(m, 3), to_u16(d.component));
        m.set_return(kOk);
    });
    // HRESULT MsiGetFileSignatureInformation(LPCTSTR szSignedObjectPath, DWORD dwFlags,
    //     PCCERT_CONTEXT* ppcCertContext, LPBYTE pbHashData, LPDWORD pcbHashData)
    pair(map, "MsiGetFileSignatureInformation", [pm](Marshaler& m, bool w) {
        const auto path = read_in8(m, P(m, 0), w);
        const u32 flags = U(m, 1);
        const GuestAddr cert_out = P(m, 2), hash = P(m, 3), hash_size = P(m, 4);
        if (!path || cert_out == 0 || (flags & ~kMsiInvalidHashIsFatal) != 0 || (hash != 0 && hash_size == 0))
            return m.set_return(kHrInvalidArg);
        (void)m.write_ptr(cert_out, 0);
        std::vector<u8> bytes;
        if (read_file(pm->vfs(), *path, bytes) != FileProblem::None) return m.set_return(kHrCryptFileError);
        const bool pe = authenticode::is_pe_image(bytes), cab = authenticode::is_cab_image(bytes);
        if (!pe && !cab) return m.set_return(hresult_from_win32(kErrFunctionFailed));  // probe: a text file
        const bool signed_image = pe ? authenticode::locate_pe_signature(bytes).is_ok()
                                     : authenticode::locate_cab_signature(bytes).is_ok();
        if (!signed_image) return m.set_return(kHrNoSignature);
        ProductOpsPlatform platform;
        {
            ProductOpsState& s = state_of(*pm);
            std::lock_guard lock(s.mutex);
            platform = s.platform;
        }
        if (!platform.verify_signature || !platform.create_cert_context) {
            log_unimplemented(w ? "MsiGetFileSignatureInformationW" : "MsiGetFileSignatureInformationA",
                              "WinVerifyTrust is not wired to msi.dll");
            return m.set_return(hresult_from_win32(kErrFunctionFailed));  // Learn: WinVerifyTrust not available
        }
        std::vector<u8> signer, signature;
        const u32 hr = platform.verify_signature(bytes, (flags & kMsiInvalidHashIsFatal) != 0, signer, signature);
        if (hr != kHrOk) return m.set_return(hr);
        if (hash != 0) {
            const u32 capacity = m.read_u32(hash_size).is_ok() ? m.read_u32(hash_size).value() : 0;
            (void)m.write_u32(hash_size, static_cast<u32>(signature.size()));
            if (capacity < signature.size()) return m.set_return(hresult_from_win32(kErrMoreData));
            for (usize i = 0; i < signature.size(); ++i) (void)m.write_u8(hash + i, signature[i]);
        }
        const u64 context = platform.create_cert_context(m, signer);
        if (context == 0) return m.set_return(hresult_from_win32(8));  // ERROR_NOT_ENOUGH_MEMORY
        (void)m.write_ptr(cert_out, context);
        m.set_return(kHrOk);
    });

    // ---- Strings and message boxes -----------------------------------------------------------------------
    // int MsiLoadString(HINSTANCE hInstance, UINT uID, LPTSTR lpBuffer, int nBufferMax, LANGID wLanguage)
    // (undocumented; probe loadstring lines): returns the ANSI code page of the
    // string's language (0 when the string is missing or empty, or the buffer
    // has no room); a short buffer receives a truncated, terminated string
    // (W) or an empty one (A).
    pair(map, "MsiLoadString", [](Marshaler& m, bool w) {
        const GuestAddr module = P(m, 0), buf = P(m, 2);
        const u32 id = U(m, 1);
        const i32 capacity = I(m, 3);
        const u16 language = static_cast<u16>(U(m, 4));
        if (buf == 0 || capacity <= 0) return m.set_return(0);
        const auto found = load_module_string(m, module, id, language);
        if (!found) return m.set_return(0);
        const u32 code_page = codepage::ansi_code_page();
        const std::vector<u16> units = units_for(found->second, w);
        if (units.size() + 1 <= static_cast<usize>(capacity)) write_units(m, buf, w, units, units.size());
        else if (w) write_units(m, buf, w, units, static_cast<usize>(capacity) - 1);
        else write_units(m, buf, w, units, 0);
        m.set_return(code_page);
    });
    // int MsiMessageBox(HWND, LPCTSTR, LPCTSTR, UINT, LANGID, ...) / MsiMessageBoxEx: undocumented,
    // no Learn page or header prototype, not measurable without an interactive desktop.
    for (const char* name : {"MsiMessageBoxA", "MsiMessageBoxW", "MsiMessageBoxExA", "MsiMessageBoxExW"}) {
        const std::string api = name;
        map[api] = [api](Marshaler& m) {
            log_unimplemented(api, "undocumented message box export");
            m.set_return(0);
        };
    }

    // ---- Transactions ----------------------------------------------------------------------------------
    // UINT MsiBeginTransaction(LPCTSTR szName, DWORD dwTransactionAttributes,
    //                          MSIHANDLE* phTransactionHandle, HANDLE* phChangeOfOwnerEvent)
    // Probe: NULL name/outputs or an unknown attribute 87; a second transaction
    // while one is open 1618.
    pair(map, "MsiBeginTransaction", [pm](Marshaler& m, bool w) {
        const auto name = read_in8(m, P(m, 0), w);
        const u32 attributes = U(m, 1);
        const GuestAddr handle_out = P(m, 2), event_out = P(m, 3);
        if (!name || handle_out == 0 || event_out == 0 || (attributes & ~kTransactionAttributesMask) != 0)
            return m.set_return(kErrInvalidParameter);
        ProductOpsState& s = state_of(*pm);
        std::lock_guard lock(s.mutex);
        if (s.transaction.active) return m.set_return(kErrAlreadyRunning);
        if (!s.platform.create_event) {
            log_unimplemented(w ? "MsiBeginTransactionW" : "MsiBeginTransactionA", "kernel event objects are not wired to msi.dll");
            return m.set_return(1601u);  // ERROR_INSTALL_SERVICE_FAILURE
        }
        const u64 event = s.platform.create_event(true, false);
        if (event == 0) return m.set_return(kErrFunctionFailed);
        auto object = std::make_shared<TransactionObject>(TransactionObject{*name});
        const u32 handle = pm->handles().add(object);
        s.transaction = TransactionInfo{true, *name, attributes, handle};
        s.transaction_object = object;
        (void)m.write_u32(handle_out, handle);
        (void)m.write_ptr(event_out, event);
        m.set_return(kOk);
    });
    // UINT MsiJoinTransaction(MSIHANDLE hTransactionHandle, DWORD dwTransactionAttributes,
    //                         HANDLE* phChangeOfOwnerEvent)
    map["MsiJoinTransaction"] = [pm](Marshaler& m) {
        const u32 handle = U(m, 0), attributes = U(m, 1);
        const GuestAddr event_out = P(m, 2);
        ProductOpsState& s = state_of(*pm);
        std::lock_guard lock(s.mutex);
        if (!pm->handles().get<TransactionObject>(handle) || !s.transaction.active || s.transaction.handle != handle)
            return m.set_return(kErrInvalidHandle);
        if (event_out == 0 || (attributes & ~kTransactionAttributesMask) != 0) return m.set_return(kErrInvalidParameter);
        const u64 event = s.platform.create_event ? s.platform.create_event(true, false) : 0;
        if (event == 0) return m.set_return(kErrFunctionFailed);
        (void)m.write_ptr(event_out, event);
        m.set_return(kOk);
    };
    // UINT MsiEndTransaction(DWORD dwTransactionState). Probe: an unknown state 87
    // (checked first); no open transaction ERROR_ACCESS_DENIED; ending closes
    // the transaction's MSIHANDLE.
    map["MsiEndTransaction"] = [pm](Marshaler& m) {
        const u32 state = U(m, 0);
        if (state != kTransactionRollback && state != kTransactionCommit) return m.set_return(kErrInvalidParameter);
        ProductOpsState& s = state_of(*pm);
        std::lock_guard lock(s.mutex);
        if (!s.transaction.active) return m.set_return(kErrAccessDenied);
        (void)pm->handles().close(s.transaction.handle);
        s.transaction = TransactionInfo{};
        s.transaction_object.reset();
        m.set_return(kOk);
    };

    // ---- Installer housekeeping --------------------------------------------------------------------------
    // UINT MsiCreateAndVerifyInstallerDirectory(DWORD dwReserved): %windir%\Installer.
    map["MsiCreateAndVerifyInstallerDirectory"] = [pm](Marshaler& m) {
        if (U(m, 0) != 0) return m.set_return(kErrInvalidParameter);
        capsule::Vfs* vfs = pm->vfs();
        if (vfs == nullptr) return m.set_return(kErrFunctionFailed);
        if (auto st = vfs->stat("C:\\Windows\\Installer"); st.is_ok()) {
            const bool dir = (st.value().attributes & capsule::FileAttributes::Directory) != capsule::FileAttributes::None;
            return m.set_return(dir ? kOk : kErrFunctionFailed);
        }
        m.set_return(vfs->create_dir("C:\\Windows\\Installer").is_ok() ? kOk : kErrFunctionFailed);
    };
    // void MsiInvalidateFeatureCache(void): Vitreo answers feature queries from the
    // registration database every time; there is no cache to drop.
    map["MsiInvalidateFeatureCache"] = [](Marshaler& m) { m.set_return(0); };
    // UINT MsiNotifySidChange(LPCTSTR pOldSid, LPCTSTR pNewSid)
    pair(map, "MsiNotifySidChange", [](Marshaler& m, bool w) {
        const auto old_sid = read_in8(m, P(m, 0), w);
        const auto new_sid = read_in8(m, P(m, 1), w);
        if (!old_sid || !new_sid) return m.set_return(kErrInvalidParameter);
        log_unimplemented(w ? "MsiNotifySidChangeW" : "MsiNotifySidChangeA", "moving per-user installer data to a new SID");
        m.set_return(kErrFunctionFailed);
    });
    // UINT Migrate10CachedPackages(LPCTSTR szProductCode, LPCTSTR szUser, LPCTSTR szAlternativePackage,
    //                              const MIGRATIONOPTIONS eMigOptions): migrates Windows Installer 1.0
    // cached packages, which a capsule never has; the probe returns ERROR_SUCCESS
    // for every input when there is nothing to migrate.
    pair(map, "Migrate10CachedPackages", [](Marshaler& m, bool) { m.set_return(kOk); });
    for (const char* name : {"MsiDeleteUserDataA", "MsiDeleteUserDataW", "MsiSetOfflineContextW"}) {
        const std::string api = name;
        map[api] = [api](Marshaler& m) {
            log_unimplemented(api, "undocumented export");
            m.set_return(kErrCallNotImplemented);
        };
    }

    // ---- DLL entry points --------------------------------------------------------------------------
    // HRESULT DllGetVersion(DLLVERSIONINFO* pdvi). Probe: cbSize < 20 is E_FAIL;
    // otherwise the DLLVERSIONINFO fields (5.0.10011, DLLVER_PLATFORM_NT) and
    // nothing of DLLVERSIONINFO2.
    map["DllGetVersion"] = [](Marshaler& m) {
        const GuestAddr info = P(m, 0);
        if (info == 0) return m.set_return(kHrInvalidArg);
        const auto size = m.read_u32(info);
        if (size.is_err() || size.value() < 20) return m.set_return(kHrFail);
        (void)m.write_u32(info + 4, 5);
        (void)m.write_u32(info + 8, 0);
        (void)m.write_u32(info + 12, 10011);
        (void)m.write_u32(info + 16, 2);
        m.set_return(kHrOk);
    };
    // HRESULT DllCanUnloadNow(void): msi.dll stays loaded (probe: S_FALSE).
    map["DllCanUnloadNow"] = [](Marshaler& m) { m.set_return(kHrFalse); };
    // HRESULT DllGetClassObject(REFCLSID, REFIID, LPVOID*): the Windows Installer
    // automation classes are not implemented; an unknown CLSID is
    // CLASS_E_CLASSNOTAVAILABLE with *ppv NULL (probe).
    map["DllGetClassObject"] = [](Marshaler& m) {
        const GuestAddr out = P(m, 2);
        if (out != 0) (void)m.write_ptr(out, 0);
        const GuestAddr clsid = P(m, 0);
        const u32 d1 = (clsid != 0 && m.read_u32(clsid).is_ok()) ? m.read_u32(clsid).value() : 0;
        if ((d1 & 0xFFFFF000u) == 0x000C1000u)
            log_unimplemented("DllGetClassObject", "Windows Installer COM classes (WindowsInstaller.Installer automation)");
        m.set_return(kHrClassNotAvailable);
    };
    for (const char* name : {"DllRegisterServer", "DllUnregisterServer"}) {
        const std::string api = name;
        map[api] = [api](Marshaler& m) {
            log_unimplemented(api, "msi.dll COM class registration");
            m.set_return(kHrSelfRegClass);
        };
    }
}

}  // namespace vitreo::winapi::msi
