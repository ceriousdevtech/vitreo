/// @file shlwapi_path_extras.cpp
/// @brief SHLWAPI string/path helpers whose observable behaviour Learn leaves
///        open: StrPBrk, PathFindFileName, PathStripPath, PathGetDriveNumber,
///        PathCompactPathEx and PathRelativePathTo (A and W).
///
/// Contract sources:
///   https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-strpbrkw
///   https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathfindfilenamew
///   https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathstrippathw
///   https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathgetdrivenumberw
///   https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathcompactpathexw
///   https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathrelativepathtow
/// Every rule below that Learn does not state was measured on Windows 11 by
/// tools/oracle_probe/shlwapi_path_extras_probe.c (both bitnesses; the output
/// is tests/oracle_data/shlwapi_path_extras_probe{,.x86}.expected). None of
/// these functions touches the thread's last error.
///
/// The ANSI forms work on bytes. Vitreo's ANSI code page is 1252 or UTF-8
/// (codepage::ansi_code_page; DBCS pages are not implemented), so no ANSI
/// string has a lead byte and an ASCII separator byte is always a character.

#include "winapi/system/shlwapi/shlwapi_path_extras.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/user32/messages/user32_case.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include "winapi/kernel32/error.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

using Units = std::vector<u16>;
#include "path_is_root_contract.inc"

constexpr usize kMaxPath = 260;
constexpr u32 kFileAttributeDirectory = 0x10;
constexpr usize kReadLimit = 32768;

bool is_slash(u16 c) { return c == '\\' || c == '/'; }

Units read_str(Marshaler& m, GuestAddr at, bool wide, usize limit = kReadLimit) {
    Units s;
    for (usize i = 0; i < limit; ++i) {
        const u16 c = wide ? m.read_u16(at + i * 2).value_or(0)
                           : m.read_u8(at + i).value_or(0);
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

bool write_unit(Marshaler& m, GuestAddr at, usize index, u16 c, bool wide) {
    return wide ? m.write_u16(at + index * 2, c).is_ok()
                : m.write_u8(at + index, static_cast<u8>(c)).is_ok();
}

/// Writes @p s and its terminator.
bool write_str(Marshaler& m, GuestAddr at, const Units& s, bool wide) {
    for (usize i = 0; i < s.size(); ++i)
        if (!write_unit(m, at, i, s[i], wide)) return false;
    return write_unit(m, at, s.size(), 0, wide);
}

void append(Units& s, const char* text) {
    for (; *text; ++text) s.push_back(static_cast<u8>(*text));
}

/// Case-insensitive unit compare (shlwapi compares path components without
/// regard to case): USER's measured mapping for UTF-16; for ANSI, code page
/// 1252's when that is the ANSI page, otherwise ASCII.
u16 fold(u16 c, bool wide) {
    if (wide) return user32_case::upper(c);
    if (codepage::ansi_code_page() == 1252) return user32_case::upper_ansi(static_cast<u8>(c));
    return (c >= 'a' && c <= 'z') ? static_cast<u16>(c - 32) : c;
}

/// PathFindFileName's index (measured): the unit after the last '\\' or '/'
/// that is followed by something other than NUL, '\\' or '/'; then, inside
/// that component, after its ':' when it has exactly one and the ':' is
/// followed by a character ("C:file" -> "file", "a:b:c" -> "a:b:c",
/// "C:\\dir\\" -> "dir\\").
usize find_file_name(const Units& s) {
    usize base = 0;
    for (usize i = 0; i < s.size(); ++i)
        if (is_slash(s[i]) && i + 1 < s.size() && !is_slash(s[i + 1])) base = i + 1;
    usize end = base;
    usize colons = 0, colon = 0;
    for (; end < s.size() && !is_slash(s[end]); ++end)
        if (s[end] == ':') { ++colons; colon = end; }
    if (colons == 1 && colon + 1 < s.size() && !is_slash(s[colon + 1])) base = colon + 1;
    return base;
}

/// The resulting string of PathRemoveFileSpec (measured; the buffer residue
/// past the new terminator is not modelled because PathRelativePathTo only
/// works on its own copies). The root is "X:\\" / "X:", "\\\\" for UNC, "\\"
/// for a rooted path, and for the Unicode form also "\\\\?\\X:\\". The path is
/// cut at the start of its last run of backslashes, or after the root.
/// Returns whether anything was removed.
bool remove_file_spec(Units& s, bool wide) {
    const usize n = s.size();
    if (n == 0) return false;
    auto at = [&](usize i) -> u16 { return i < n ? s[i] : 0; };
    usize root_end = 0;
    usize p = 0;
    const bool long_prefix = wide && at(0) == '\\' && at(1) == '\\' && at(2) == '?' && at(3) == '\\';
    if (long_prefix) { p = 4; root_end = 4; }
    if (at(p) != 0 && at(p + 1) == ':') {
        root_end = p + 2;
        if (at(p + 2) == '\\') root_end = p + 3;
    } else if (!long_prefix && at(0) == '\\') {
        root_end = at(1) == '\\' ? 2 : 1;
    }
    usize last_run = n;
    for (usize i = 0; i < n; ++i)
        if (s[i] == '\\' && (i == 0 || s[i - 1] != '\\')) last_run = i;
    const usize cut = (last_run != n && last_run >= root_end) ? last_run : root_end;
    if (cut >= n) return false;
    s.resize(cut);
    return true;
}

/// PathCommonPrefix's length (measured): components separated by '\\' are
/// compared case-insensitively; "\\\\" never matches on its own and both or
/// neither path must be UNC; a two-unit result ("c:") reports 3 (its root).
/// The scan stops when either path runs out.
usize common_prefix(const Units& a, const Units& b, bool wide) {
    auto unc = [](const Units& s) { return s.size() >= 2 && s[0] == '\\' && s[1] == '\\'; };
    usize ia = 0, ib = 0;
    if (unc(a) != unc(b)) return 0;
    if (unc(a)) ia = ib = 2;
    bool any = false;
    usize common = 0;
    for (;;) {
        usize na = ia, nb = ib;
        while (na < a.size() && a[na] != '\\') ++na;
        while (nb < b.size() && b[nb] != '\\') ++nb;
        if (na - ia != nb - ib) break;
        bool same = true;
        for (usize k = 0; k < na - ia && same; ++k)
            same = fold(a[ia + k], wide) == fold(b[ib + k], wide);
        if (!same) break;
        any = true;
        common = na;
        if (na >= a.size() || nb >= b.size()) break;
        ia = na + 1;
        ib = nb + 1;
    }
    if (!any) return 0;
    return common == 2 ? 3 : common;
}

// ---------------------------------------------------------------------------

/// LPWSTR StrPBrkW(PCWSTR psz, PCWSTR pszSet) / PSTR StrPBrkA(PCSTR, PCSTR):
/// the first character of @p psz that occurs in @p pszSet, or NULL (also for
/// a NULL argument).
void str_pbrk(Marshaler& m, bool wide) {
    const GuestAddr str = m.arg_ptr(0);
    const GuestAddr set_ptr = m.arg_ptr(1);
    if (str == 0 || set_ptr == 0) { m.set_return(0); return; }
    const Units set = read_str(m, set_ptr, wide);
    const usize unit = wide ? 2 : 1;
    for (usize i = 0; i < kReadLimit; ++i) {
        const u16 c = wide ? m.read_u16(str + i * unit).value_or(0) : m.read_u8(str + i).value_or(0);
        if (c == 0) break;
        if (std::find(set.begin(), set.end(), c) != set.end()) {
            m.set_return(static_cast<u64>(str + i * unit));
            return;
        }
    }
    m.set_return(0);
}

/// PathFindFileName{A,W}: NULL -> NULL, else a pointer into the argument.
void path_find_file_name(Marshaler& m, bool wide) {
    const GuestAddr path = m.arg_ptr(0);
    if (path == 0) { m.set_return(0); return; }
    const Units s = read_str(m, path, wide);
    m.set_return(static_cast<u64>(path + find_file_name(s) * (wide ? 2 : 1)));
}

/// void PathStripPath{A,W}(LPTSTR): moves the PathFindFileName part and its
/// terminator to the start of the buffer; the old tail past it stays.
void path_strip_path(Marshaler& m, bool wide) {
    const GuestAddr path = m.arg_ptr(0);
    if (path == 0) return;
    const Units s = read_str(m, path, wide);
    const usize base = find_file_name(s);
    if (base == 0) return;
    (void)write_str(m, path, Units(s.begin() + static_cast<std::ptrdiff_t>(base), s.end()), wide);
}

// PathIsRoot is lexical. Its A/W drive classes and extended-W grammar
// differ; Windows captures override the inaccurate examples in Learn Remarks.
bool root_drive_character(u16 unit, bool wide) {
    if (wide) {
        for (auto range : root_contract::drive_W) if (unit >= range.first && unit <= range.last) return true;
    } else {
        for (auto range : root_contract::drive_A) if (unit >= range.first && unit <= range.last) return true;
    }
    return false;
}

bool root_grammar(std::basic_string_view<u16> path, bool wide) {
    const auto drive = [wide](std::basic_string_view<u16> value) {
        return value.size() == 3 && root_drive_character(value[0], wide) && value[1] == ':' && value[2] == '\\';
    };
    const auto unc = [](std::basic_string_view<u16> tail) {
        const auto first = tail.find('\\');
        return first == tail.npos || (first + 1 < tail.size() && tail.find('\\', first + 1) == tail.npos);
    };
    const auto ascii_lower = [](u16 value) { return value >= 'A' && value <= 'Z' ? static_cast<u16>(value + ('a' - 'A')) : value; };
    if (path.empty()) return false;
    if (path.size() >= 4 && path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\') {
        if (!wide) return false;
        const auto tail = path.substr(4);
        if (tail.size() >= 4 && ascii_lower(tail[0]) == 'u' && ascii_lower(tail[1]) == 'n' &&
            ascii_lower(tail[2]) == 'c' && tail[3] == '\\') return unc(tail.substr(4));
        if (drive(tail)) return true;
        // Canonical volume GUID root spelling, measured with valid/malformed
        // GUIDs rather than accepting every name followed by a backslash.
        constexpr std::u16string_view volume = u"Volume{00000000-0000-0000-0000-000000000000}\\";
        if (tail.size() != volume.size()) return false;
        for (usize i = 0; i < tail.size(); ++i) {
            const u16 expected = volume[i], actual = ascii_lower(tail[i]);
            if (expected == '0') {
                if (!((actual >= '0' && actual <= '9') || (actual >= 'a' && actual <= 'f'))) return false;
            } else if (ascii_lower(expected) != actual) return false;
        }
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        if (path.size() >= 3 && path[2] == '?') return false;
        return unc(path.substr(2));
    }
    return path.size() == 1 && path[0] == '\\' ? true : drive(path);
}

void path_is_root(Marshaler& m, bool wide) {
    const GuestAddr pointer = m.arg_ptr(0);
    if (pointer == 0) { m.set_return(0); return; }
    Units units;
    for (usize i = 0;; ++i) {
        const GuestAddr address = pointer + i * (wide ? sizeof(u16) : sizeof(u8));
        u16 unit;
        if (wide) {
            const auto read = m.read_u16(address);
            if (read.is_err()) { (void)m.raise_access_violation(false, address); m.set_return(0); return; }
            unit = read.value();
        } else {
            const auto read = m.read_u8(address);
            if (read.is_err()) { (void)m.raise_access_violation(false, address); m.set_return(0); return; }
            unit = read.value();
        }
        if (unit == 0) break;
        units.push_back(unit);
    }
    if (!wide && !units.empty()) {
        if (!codepage::is_valid_code_page(codepage::ansi_code_page())) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED shlwapi.dll!PathIsRootA: process ANSI code page conversion\n");
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED); m.set_return(0); return;
        }
        std::vector<u8> bytes; bytes.reserve(units.size());
        for (u16 unit : units) bytes.push_back(static_cast<u8>(unit));
        const auto decoded = codepage::multi_byte_to_wide(codepage::kCpAcp, 0,
            bytes.data(), bytes.size(), units.data(), units.size());
        if (!decoded.ok()) { set_last_error(decoded.error); m.set_return(0); return; }
        units.resize(decoded.count);
    }
    m.set_return(root_grammar({units.data(), units.size()}, wide) ? 1u : 0u);
}

/// int PathGetDriveNumber{A,W}: 0..25 for "a:".."z:" (either case), else -1.
/// The Unicode form also accepts the "\\\\?\\" prefix; the ANSI form does not.
void path_get_drive_number(Marshaler& m, bool wide) {
    const GuestAddr path = m.arg_ptr(0);
    i32 drive = -1;
    if (path != 0) {
        const Units s = read_str(m, path, wide, 8);
        usize p = 0;
        if (wide && s.size() >= 4 && s[0] == '\\' && s[1] == '\\' && s[2] == '?' && s[3] == '\\') p = 4;
        if (s.size() >= p + 2 && s[p + 1] == ':') {
            const u16 c = s[p];
            if (c >= 'a' && c <= 'z') drive = c - 'a';
            else if (c >= 'A' && c <= 'Z') drive = c - 'A';
        }
    }
    m.set_return(static_cast<u32>(drive));
}

/// BOOL PathCompactPathEx{A,W}(out, src, cchMax, dwFlags). dwFlags is
/// reserved and ignored. FALSE without writing for a NULL buffer, a NULL
/// source or cchMax 0. A source shorter than cchMax - 1 is copied and the
/// rest of the cchMax-unit buffer zero-filled. Otherwise exactly cchMax - 1
/// units are produced: "<head>...<sep><name>" when the file name and "...\\"
/// fit (head = the leading units that still fit, sep = '/' when the name
/// follows a '/'), else "...<sep><name start>...", and for a source with no
/// directory part "<name start>...", each cut to cchMax - 1.
void path_compact_path_ex(Marshaler& m, bool wide) {
    const GuestAddr out = m.arg_ptr(0);
    const GuestAddr src = m.arg_ptr(1);
    const usize max = m.arg32(2);
    if (out == 0 || src == 0 || max == 0) { m.set_return(0); return; }
    const Units s = read_str(m, src, wide);
    const usize len = s.size();
    if (len + 1 < max) {
        if (!write_str(m, out, s, wide)) { m.set_return(0); return; }
        for (usize i = len + 1; i < max; ++i)
            if (!write_unit(m, out, i, 0, wide)) break;
        m.set_return(1);
        return;
    }
    const usize avail = max - 1;
    const usize base = find_file_name(s);
    const Units name(s.begin() + static_cast<std::ptrdiff_t>(base), s.end());
    Units r;
    if (base == 0) {
        r.assign(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(std::min(name.size(), max >= 4 ? max - 4 : 0)));
        append(r, "...");
    } else {
        const u16 sep = s[base - 1] == '/' ? '/' : '\\';
        // Measured: a one-unit name is kept whole only from cchMax 7 on
        // ("C:\\a\\b" at 6 gives "...\\."), as if it were two units long.
        if (4 + std::max<usize>(name.size(), 2) <= avail) {
            const usize head = avail - 4 - name.size();
            r.assign(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(head));
            append(r, "...");
            r.push_back(sep);
            r.insert(r.end(), name.begin(), name.end());
        } else {
            append(r, "...");
            r.push_back(sep);
            r.insert(r.end(), name.begin(),
                     name.begin() + static_cast<std::ptrdiff_t>(std::min(name.size(), max >= 8 ? max - 8 : 0)));
            append(r, "...");
        }
    }
    if (r.size() > avail) r.resize(avail);
    m.set_return(write_str(m, out, r, wide) ? 1u : 0u);
}

/// BOOL PathRelativePathTo{A,W}(out, from, attrFrom, to, attrTo), measured:
///  * NULL out: FALSE, nothing written. NULL from/to: out[0] = 0, FALSE.
///  * from and to are compared up to MAX_PATH - 1 units. A path whose attribute
///    lacks FILE_ATTRIBUTE_DIRECTORY loses its file spec (PathRemoveFileSpec)
///    for the comparison; no other attribute bit matters.
///  * No common prefix (common_prefix == 0): out[0] = 0, FALSE.
///  * The rest of from after the prefix (one leading '\\' skipped) gives one
///    ".." per component (a run of '\\' separates once); none gives ".".
///    The rest of the caller's whole to after the prefix is appended, behind a
///    '\\' unless it starts with one.
///  * A result over MAX_PATH - 1 units is stored cut to MAX_PATH - 1 units,
///    then out[0] = 0 and FALSE.
/// Where Windows reads past a terminator (a prefix longer than from or to,
/// e.g. from "c:" against "c:\\x"), the results differ between runs and
/// bitnesses; Vitreo stops at the terminator.
void path_relative_path_to(Marshaler& m, bool wide) {
    const GuestAddr out = m.arg_ptr(0);
    const GuestAddr from_ptr = m.arg_ptr(1);
    const u32 attr_from = m.arg32(2);
    const GuestAddr to_ptr = m.arg_ptr(3);
    const u32 attr_to = m.arg32(4);
    if (out == 0) { m.set_return(0); return; }
    auto fail = [&] {
        (void)write_unit(m, out, 0, 0, wide);
        m.set_return(0);
    };
    if (from_ptr == 0 || to_ptr == 0) { fail(); return; }
    // The comparison works on MAX_PATH - 1 unit copies; the appended tail is
    // read from the caller's whole pszTo (measured: a 257-unit tail fails).
    const Units to = read_str(m, to_ptr, wide);
    Units from_dir = read_str(m, from_ptr, wide, kMaxPath - 1);
    Units to_dir(to.begin(), to.begin() + static_cast<std::ptrdiff_t>(std::min(to.size(), kMaxPath - 1)));
    if (!(attr_from & kFileAttributeDirectory)) (void)remove_file_spec(from_dir, wide);
    if (!(attr_to & kFileAttributeDirectory)) (void)remove_file_spec(to_dir, wide);
    const usize prefix = common_prefix(from_dir, to_dir, wide);
    if (prefix == 0) { fail(); return; }

    usize p = std::min(prefix, from_dir.size());
    if (p < from_dir.size() && from_dir[p] == '\\') ++p;
    usize ups = 0;
    while (p < from_dir.size()) {
        while (p < from_dir.size() && from_dir[p] != '\\') ++p;
        while (p < from_dir.size() && from_dir[p] == '\\') ++p;
        ++ups;
    }
    Units r;
    if (ups == 0) append(r, ".");
    for (usize i = 0; i < ups; ++i) append(r, i == 0 ? ".." : "\\..");
    const usize q = std::min(prefix, to.size());
    if (q < to.size()) {
        if (to[q] != '\\') r.push_back('\\');
        r.insert(r.end(), to.begin() + static_cast<std::ptrdiff_t>(q), to.end());
    }
    if (r.size() > kMaxPath - 1) {
        r.resize(kMaxPath - 1);
        (void)write_str(m, out, r, wide);
        fail();
        return;
    }
    m.set_return(write_str(m, out, r, wide) ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// StrToInt, StrRChr, StrStr, PathRemoveBackslash, PathRemoveExtension,
// PathSkipRoot. Behaviour Learn leaves open was measured by
// tools/oracle_probe/shlwapi_str_extras_probe.c (both bitnesses). The ANSI
// forms behave like the Unicode ones on the same text.

/// The value StrToIntEx / StrToInt64Ex parse, or nullopt for FALSE
/// (measured): leading ' ', '\t' and '\n' are skipped (not '\v', '\f',
/// '\r'); one '+' or '-'; with STIF_SUPPORT_HEX a "0x"/"0X" prefix and at
/// least one hex digit (the sign is then ignored), otherwise at least one
/// decimal digit; parsing stops at the first other character. The digits
/// accumulate modulo 2^64 and the caller truncates.
std::optional<u64> parse_int(const Units& s, bool hex_allowed) {
    usize i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    auto digit = [](u16 c, u32 base) -> int {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        return d < static_cast<int>(base) ? d : -1;
    };
    if (hex_allowed && i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        if (i >= s.size() || digit(s[i], 16) < 0) return std::nullopt;
        u64 v = 0;
        for (; i < s.size() && digit(s[i], 16) >= 0; ++i) v = v * 16 + static_cast<u64>(digit(s[i], 16));
        return v;
    }
    if (i >= s.size() || digit(s[i], 10) < 0) return std::nullopt;
    u64 v = 0;
    for (; i < s.size() && digit(s[i], 10) >= 0; ++i) v = v * 10 + static_cast<u64>(digit(s[i], 10));
    return negative ? (~v + 1) : v;
}

/// int StrToInt{A,W}(PCTSTR): an optional '-' and decimal digits only (no
/// whitespace, no '+'); 0 otherwise or for NULL. Wraps modulo 2^32.
void str_to_int(Marshaler& m, bool wide) {
    const GuestAddr p = m.arg_ptr(0);
    if (p == 0) { m.set_return(0); return; }
    const Units s = read_str(m, p, wide);
    usize i = 0;
    const bool negative = !s.empty() && s[0] == '-';
    if (negative) ++i;
    u32 v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
    m.set_return(negative ? (~v + 1) : v);
}

/// BOOL StrToIntEx{A,W}(PCTSTR, STIF_FLAGS, int*) and StrToInt64Ex{A,W}(...,
/// LONGLONG*): *pRet is 0 when parsing fails; for a NULL string StrToIntEx
/// stores 0 and StrToInt64Ex leaves *pllRet alone (measured). Only
/// STIF_SUPPORT_HEX changes anything.
void str_to_int_ex(Marshaler& m, bool wide, bool is64) {
    const GuestAddr p = m.arg_ptr(0);
    const u32 flags = m.arg32(1);
    const GuestAddr out = m.arg_ptr(2);
    auto store = [&](u64 v) {
        if (is64) (void)m.write_u64(out, v);
        else (void)m.write_u32(out, static_cast<u32>(v));
    };
    if (p == 0) {
        if (!is64) store(0);
        m.set_return(0);
        return;
    }
    const auto v = parse_int(read_str(m, p, wide), (flags & 1u) != 0);
    store(v ? *v : 0);
    m.set_return(v ? 1u : 0u);
}

/// PCTSTR StrRChr{,I}{A,W}(PCTSTR start, PCTSTR end, WORD match): the last
/// occurrence before @p end (NULL: the terminator). The ANSI forms compare
/// the low byte of wMatch (code pages 1252/UTF-8 have no lead bytes).
void str_rchr(Marshaler& m, bool wide, bool fold_case) {
    const GuestAddr start = m.arg_ptr(0), end = m.arg_ptr(1);
    u16 match = static_cast<u16>(m.arg32(2));
    if (!wide) match &= 0xFF;
    if (start == 0 || match == 0) { m.set_return(0); return; }
    const usize unit = wide ? 2 : 1;
    const usize limit = end == 0 ? kReadLimit : (end > start ? (end - start) / unit : 0);
    const u16 want = fold_case ? fold(match, wide) : match;
    GuestAddr found = 0;
    for (usize i = 0; i < limit; ++i) {
        const u16 c = wide ? m.read_u16(start + i * unit).value_or(0) : m.read_u8(start + i).value_or(0);
        if (c == 0) break;
        if ((fold_case ? fold(c, wide) : c) == want) found = start + i * unit;
    }
    m.set_return(static_cast<u64>(found));
}

/// PCTSTR StrStr{,I}{A,W}(PCTSTR, PCTSTR) and StrStrN{,I}W(..., UINT cchMax):
/// the first occurrence of a non-empty needle (an empty needle or a NULL
/// argument gives NULL); the N forms only match inside the first cchMax
/// characters (the match has to end within them -- Learn's reading, and what
/// StrStrNW does; StrStrNIW was measured to also accept a match that starts
/// inside them but ends past them in one case, "HELLO"/"llo"/3 -> +2, which
/// is not modelled).
void str_str(Marshaler& m, bool wide, bool fold_case, bool bounded) {
    const GuestAddr hay = m.arg_ptr(0), needle_ptr = m.arg_ptr(1);
    const usize max = bounded ? m.arg32(2) : kReadLimit;
    if (hay == 0 || needle_ptr == 0) { m.set_return(0); return; }
    const Units needle = read_str(m, needle_ptr, wide);
    if (needle.empty()) { m.set_return(0); return; }
    const Units h = read_str(m, hay, wide, std::min<usize>(max, kReadLimit));
    for (usize i = 0; i + needle.size() <= h.size(); ++i) {
        bool same = true;
        for (usize k = 0; k < needle.size() && same; ++k)
            same = fold_case ? fold(h[i + k], wide) == fold(needle[k], wide) : h[i + k] == needle[k];
        if (same) { m.set_return(static_cast<u64>(hay + i * (wide ? 2 : 1))); return; }
    }
    m.set_return(0);
}

/// A root PathRemoveBackslash keeps its backslash: "X:\\", "\\", "\\\\", and
/// for the Unicode form also "\\\\?\\X:\\" (measured).
bool is_backslash_root(const Units& s, bool wide) {
    if (s.size() == 1 && s[0] == '\\') return true;
    if (s.size() == 2 && s[0] == '\\' && s[1] == '\\') return true;
    auto drive_root = [&](usize p) {
        return s.size() == p + 3 && ((s[p] | 0x20) >= 'a' && (s[p] | 0x20) <= 'z') && s[p + 1] == ':' && s[p + 2] == '\\';
    };
    if (drive_root(0)) return true;
    return wide && s.size() >= 4 && s[0] == '\\' && s[1] == '\\' && s[2] == '?' && s[3] == '\\' && drive_root(4);
}

/// LPTSTR PathRemoveBackslash{A,W}(LPTSTR): removes one trailing '\\'
/// unless the path is a root; returns the address of the new terminator, or
/// of the last character when nothing was removed (the string itself when
/// empty); NULL for NULL.
void path_remove_backslash(Marshaler& m, bool wide) {
    const GuestAddr p = m.arg_ptr(0);
    if (p == 0) { m.set_return(0); return; }
    const Units s = read_str(m, p, wide);
    const usize unit = wide ? 2 : 1;
    if (s.empty()) { m.set_return(static_cast<u64>(p)); return; }
    const usize last = s.size() - 1;
    if (s[last] == '\\' && !is_backslash_root(s, wide)) {
        (void)write_unit(m, p, last, 0, wide);
    }
    m.set_return(static_cast<u64>(p + last * unit));
}

/// void PathRemoveExtension{A,W}(LPTSTR): cuts at PathFindExtension's '.':
/// the last '.' with no '\\' or ' ' after it ('/' does not end the search).
void path_remove_extension(Marshaler& m, bool wide) {
    const GuestAddr p = m.arg_ptr(0);
    if (p == 0) return;
    const Units s = read_str(m, p, wide);
    usize dot = s.size();
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '.') dot = i;
        else if (s[i] == '\\' || s[i] == ' ') dot = s.size();
    }
    if (dot < s.size()) (void)write_unit(m, p, dot, 0, wide);
}

/// LPCTSTR PathSkipRoot{A,W}(LPCTSTR): past "X:\\", a leading '\\', or
/// "\\\\server\\share\\"; the Unicode form also skips "\\\\?\\X:\\" and
/// "\\\\?\\UNC\\server\\share\\" (the ANSI form returns NULL for any "\\\\?\\"
/// path). NULL when there is no root ("C:", "C:x", "C:/x", "/x", "dir").
void path_skip_root(Marshaler& m, bool wide) {
    const GuestAddr p = m.arg_ptr(0);
    if (p == 0) { m.set_return(0); return; }
    const Units s = read_str(m, p, wide);
    auto at = [&](usize i) -> u16 { return i < s.size() ? s[i] : 0; };
    auto drive = [&](usize i) {
        const u16 c = static_cast<u16>(at(i) | 0x20);
        return c >= 'a' && c <= 'z' && at(i + 1) == ':' && at(i + 2) == '\\';
    };
    auto unc = [&](usize i) {
        while (at(i) && at(i) != '\\') ++i;  // server
        if (at(i) == '\\') ++i;
        if (at(i) && at(i) != '\\') {
            while (at(i) && at(i) != '\\') ++i;  // share
            if (at(i) == '\\') ++i;
        }
        return i;
    };
    std::optional<usize> skip;
    const bool long_prefix = at(0) == '\\' && at(1) == '\\' && at(2) == '?' && at(3) == '\\';
    if (long_prefix) {
        if (wide) {
            if (drive(4)) skip = 7;
            else if (at(4) == 'U' && at(5) == 'N' && at(6) == 'C' && at(7) == '\\') skip = unc(8);
        }
    } else if (at(0) == '\\' && at(1) == '\\') {
        skip = unc(2);
    } else if (at(0) == '\\') {
        skip = 1;
    } else if (drive(0)) {
        skip = 3;
    }
    m.set_return(skip ? static_cast<u64>(p + *skip * (wide ? 2 : 1)) : 0u);
}

} // namespace

void add_shlwapi_path_extras(DllExports& exports) {
    auto add = [&](const char* name, void (*fn)(Marshaler&, bool), bool wide, u16 bytes) {
        exports.entries.push_back(ExportEntry{name, 0, [fn, wide](Marshaler& m) { fn(m, wide); }, bytes});
    };
    add("StrPBrkW", str_pbrk, true, 8);
    add("StrPBrkA", str_pbrk, false, 8);
    add("PathFindFileNameW", path_find_file_name, true, 4);
    add("PathFindFileNameA", path_find_file_name, false, 4);
    add("PathStripPathW", path_strip_path, true, 4);
    add("PathStripPathA", path_strip_path, false, 4);
    add(root_contract::name_A, path_is_root, false, root_contract::argument_bytes);
    add(root_contract::name_W, path_is_root, true, root_contract::argument_bytes);
    add("PathGetDriveNumberW", path_get_drive_number, true, 4);
    add("PathGetDriveNumberA", path_get_drive_number, false, 4);
    add("PathCompactPathExW", path_compact_path_ex, true, 16);
    add("PathCompactPathExA", path_compact_path_ex, false, 16);
    add("PathRelativePathToW", path_relative_path_to, true, 20);
    add("PathRelativePathToA", path_relative_path_to, false, 20);
    auto add_fn = [&](const char* name, std::function<void(Marshaler&)> fn, u16 bytes) {
        exports.entries.push_back(ExportEntry{name, 0, std::move(fn), bytes});
    };
    for (const bool w : {true, false}) {
        const char* sfx = w ? "W" : "A";
        auto name = [sfx](const char* base) { return std::string(base) + sfx; };
        add_fn(name("StrToInt").c_str(), [w](Marshaler& m) { str_to_int(m, w); }, 4);
        add_fn(name("StrToIntEx").c_str(), [w](Marshaler& m) { str_to_int_ex(m, w, false); }, 12);
        add_fn(name("StrToInt64Ex").c_str(), [w](Marshaler& m) { str_to_int_ex(m, w, true); }, 12);
        add_fn(name("StrRChr").c_str(), [w](Marshaler& m) { str_rchr(m, w, false); }, 12);
        add_fn(name("StrRChrI").c_str(), [w](Marshaler& m) { str_rchr(m, w, true); }, 12);
        add_fn(name("StrStr").c_str(), [w](Marshaler& m) { str_str(m, w, false, false); }, 8);
        add_fn(name("StrStrI").c_str(), [w](Marshaler& m) { str_str(m, w, true, false); }, 8);
        add_fn(name("PathRemoveBackslash").c_str(), [w](Marshaler& m) { path_remove_backslash(m, w); }, 4);
        add_fn(name("PathRemoveExtension").c_str(), [w](Marshaler& m) { path_remove_extension(m, w); }, 4);
        add_fn(name("PathSkipRoot").c_str(), [w](Marshaler& m) { path_skip_root(m, w); }, 4);
    }
    add_fn("StrStrNW", [](Marshaler& m) { str_str(m, true, false, true); }, 12);
    add_fn("StrStrNIW", [](Marshaler& m) { str_str(m, true, true, true); }, 12);
}

} // namespace vitreo::winapi
