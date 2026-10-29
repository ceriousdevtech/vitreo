#include "shlwapi.hpp"
#include "shlwapi_path_extras.hpp"
#include "shlwapi_url.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/com_core.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "winapi/kernel32/locale/codepage_convert.hpp"

#include "winapi/kernel32/filesystem/file.hpp"

#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/locale_string.hpp"

#include <atomic>

namespace vitreo::winapi {
namespace {

static constexpr u32 S_OK = 0;
#include "directory_empty_contract.inc"


static bool is_os(u32 feature) {
    // Vitreo currently exposes a modern NT workstation environment. Keep the
    // feature checks aligned with that model rather than the macOS host.
    switch (feature) {
        case 1:  // OS_NT
        case 7:  // OS_WIN2000ORGREATER
        case 20: // OS_PROFESSIONAL
        case 25: // OS_PERSONALTERMINALSERVER
            return true;
        default:
            return false;
    }
}

static u16 tolower_ascii_u16(u16 ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<u16>(ch - 'A' + 'a');
    }
    return ch;
}

static std::vector<u16> read_utf16_z(const Marshaler& m, GuestAddr ptr, usize max_chars = 0) {
    std::vector<u16> out;
    if (ptr == 0) {
        return out;
    }

    usize limit = max_chars == 0 ? 65536 : max_chars;
    out.reserve(std::min<usize>(limit, 256));
    for (usize i = 0; i < limit; i++) {
        auto ch = m.read_u16(ptr + i * 2);
        if (ch.is_err()) {
            break;
        }
        if (ch.value() == 0) {
            break;
        }
        out.push_back(ch.value());
    }
    return out;
}

static GuestAddr str_chr_w(const Marshaler& m, GuestAddr str_ptr, u16 needle) {
    if (str_ptr == 0) {
        return 0;
    }

    for (usize i = 0; i < 65536; i++) {
        auto ch = m.read_u16(str_ptr + i * 2);
        if (ch.is_err()) {
            return 0;
        }
        if (ch.value() == 0) {
            return 0;
        }
        if (ch.value() == needle) {
            return str_ptr + i * 2;
        }
    }
    return 0;
}

std::atomic<LocaleStringManager*> g_locale_strings{nullptr};

/// Characters of the string at @p p before its terminator, at most @p limit
/// (Learn StrCmpN*: nChar characters "from the beginning of each string").
i32 bounded_length(Marshaler& m, GuestAddr p, i32 limit, bool wide) {
    i32 n = 0;
    while (n < limit) {
        const bool end = wide ? m.read_u16(p + static_cast<GuestAddr>(n) * 2).value_or(0) == 0
                              : m.read_u8(p + static_cast<GuestAddr>(n)).value_or(0) == 0;
        if (end) break;
        ++n;
    }
    return n;
}

/// StrCmp{I,N,NI}{A,W}: CompareString in the user's locale, NORM_IGNORECASE
/// for the I forms; the CSTR_* result less CSTR_EQUAL is the documented
/// sign (Learn: zero when identical, negative when psz1 sorts first).
i32 shlwapi_compare(Marshaler& m, bool wide, bool ignore_case, GuestAddr a, GuestAddr b, std::optional<i32> chars) {
    constexpr u32 kLocaleUserDefault = 0x0400;  // LOCALE_USER_DEFAULT
    constexpr u32 kNormIgnoreCase = 0x00000001;  // NORM_IGNORECASE
    constexpr i32 kCstrEqual = 2;               // CSTR_EQUAL
    LocaleStringManager* strings = g_locale_strings.load();
    if (strings == nullptr) return -kCstrEqual;
    i32 la = -1, lb = -1;
    if (chars) {
        if (*chars <= 0) return 0;  // no characters to compare
        if (a == 0 || b == 0) return -kCstrEqual;
        la = bounded_length(m, a, *chars, wide);
        lb = bounded_length(m, b, *chars, wide);
    }
    const u32 flags = ignore_case ? kNormIgnoreCase : 0u;
    const i32 r = wide ? strings->compare_string_w(m, kLocaleUserDefault, flags, a, la, b, lb)
                       : strings->compare_string_a(m, kLocaleUserDefault, flags, a, la, b, lb);
    return r - kCstrEqual;
}


static bool is_path_sep(char c) {
    return c == '\\' || c == '/';
}

static std::string normalize_seps(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

static std::string strip_trailing_seps(std::string s) {
    while (!s.empty() && is_path_sep(s.back())) {
        s.pop_back();
    }
    return s;
}

static bool is_relative_ascii_path(std::string_view p) {
    if (p.empty()) {
        return true;
    }
    // Drive absolute: C:\...
    if (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && is_path_sep(p[2])) {
        return false;
    }
    // UNC: \\server\share\...
    if (p.size() >= 2 && is_path_sep(p[0]) && is_path_sep(p[1])) {
        return false;
    }
    // Rooted: \...
    if (is_path_sep(p[0])) {
        return false;
    }
    return true;
}


static std::string path_combine_ascii(std::string a, std::string b) {
    a = normalize_seps(std::move(a));
    b = normalize_seps(std::move(b));

    if (b.empty()) {
        return a;
    }
    if (!is_relative_ascii_path(b)) {
        return b;
    }
    if (a.empty()) {
        return b;
    }

    a = strip_trailing_seps(std::move(a));
    if (!a.empty()) {
        a.push_back('\\');
    }
    a += b;
    return a;
}

/// PathCanonicalizeW over UTF-16 units, following the rules measured on
/// Windows 11 (tools/oracle_probe/s7_shlwapi_probe.expected):
///  - only '\\' separates; '/' and doubled separators are kept verbatim;
///  - "." segments vanish; ".." removes the previous segment but never the
///    root ("C:\\", "\\\\", "\\"); a leading "." / ".." of a relative
///    path is dropped with the separator after it ("..\\x" -> "x"), while a
///    "\\.." with nothing left to remove is dropped ("a\\..\\..\\b" ->
///    "\\b");
///  - one trailing '.' on the last segment is removed ("z." -> "z");
///  - "C:" becomes "C:\\" and an empty result becomes "\\".
/// Returns false when the input is MAX_PATH (260) units or longer.
bool path_canonicalize_units(const std::vector<u16>& src, std::vector<u16>& out) {
    constexpr usize kMaxPath = 260;
    out.clear();
    if (src.size() >= kMaxPath) return false;
    auto is_alpha = [](u16 c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; };
    usize root = 0;
    if (src.size() >= 2 && is_alpha(src[0]) && src[1] == ':') {
        root = (src.size() >= 3 && src[2] == '\\') ? 3 : 2;
    } else if (src.size() >= 2 && src[0] == '\\' && src[1] == '\\') {
        root = 2;
    } else if (!src.empty() && src[0] == '\\') {
        root = 1;
    }
    out.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(root));
    const usize base = out.size();
    std::vector<usize> starts;  // where each emitted segment begins in `out`
    usize pos = root;
    bool first = true;
    bool at_start = true;       // relative path: next segment has no '\\'
    while (pos <= src.size()) {
        usize end = pos;
        while (end < src.size() && src[end] != '\\') ++end;
        const bool dot = end - pos == 1 && src[pos] == '.';
        const bool dotdot = end - pos == 2 && src[pos] == '.' && src[pos + 1] == '.';
        const bool has_sep = end < src.size();
        if (pos == src.size() && !first) break;
        if (dot || dotdot) {
            if (dotdot && !starts.empty()) {
                out.resize(starts.back());
                starts.pop_back();
            }
            if (first && root == 0 && has_sep) {
                // A leading "." / ".." of a relative path goes with its '\\'.
                pos = end + 1;
                first = false;
                continue;
            }
            if (dotdot) at_start = false;
        } else {
            const bool sep = root > 0 ? out.size() > base : !at_start;
            starts.push_back(out.size());
            if (sep) out.push_back('\\');
            out.insert(out.end(), src.begin() + static_cast<std::ptrdiff_t>(pos),
                       src.begin() + static_cast<std::ptrdiff_t>(end));
            at_start = false;
        }
        first = false;
        if (!has_sep) break;
        pos = end + 1;
        if (pos == src.size()) {
            // A trailing separator is kept as an empty final segment.
            starts.push_back(out.size());
            out.push_back('\\');
            break;
        }
    }
    if (!out.empty() && out.back() == '.' && out.size() > base &&
        !(out.size() >= 2 && out[out.size() - 2] == '\\')) {
        out.pop_back();
    }
    if (root == 2 && src[1] == ':' && out.size() == 2) out.push_back('\\');
    if (out.empty()) out.push_back('\\');
    return true;
}

/// PathAppend (= PathCombine(path, path, more)), measured: a leading '\\' of
/// pszMore is dropped; a drive-absolute or UNC pszMore replaces the path;
/// the join is canonicalized. A result of MAX_PATH or more fails with the
/// path buffer emptied.
bool path_append_units(const std::vector<u16>& path, std::vector<u16> more,
                       std::vector<u16>& out) {
    auto is_alpha = [](u16 c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; };
    const bool unc = more.size() >= 2 && more[0] == '\\' && more[1] == '\\';
    const bool drive = more.size() >= 3 && is_alpha(more[0]) && more[1] == ':' && more[2] == '\\';
    std::vector<u16> joined;
    if (unc || drive) {
        joined = std::move(more);
    } else {
        if (!more.empty() && more[0] == '\\') more.erase(more.begin());
        joined = path;
        if (!more.empty()) {
            if (!joined.empty() && joined.back() != '\\') joined.push_back('\\');
            joined.insert(joined.end(), more.begin(), more.end());
        }
    }
    if (joined.empty()) {
        out.clear();
        return true;
    }
    return path_canonicalize_units(joined, out);
}

bool path_match_spec_one(const std::vector<u16>& path,
                         const std::vector<u16>& spec);

std::vector<u16> read_units(Marshaler& m, GuestAddr at, usize limit = 32768) {
    std::vector<u16> units;
    for (usize i = 0; at != 0 && i < limit; ++i) {
        const u16 c = m.read_u16(at + i * 2).value_or(0);
        if (c == 0) break;
        units.push_back(c);
    }
    return units;
}

std::vector<u16> read_ansi_units(Marshaler& m, GuestAddr at, usize* byte_len = nullptr) {
    std::string bytes = at != 0 ? m.read_ansi_string(at, 32768).value_or("") : std::string{};
    if (byte_len) *byte_len = bytes.size();
    std::vector<u16> wide(bytes.size() + 1, 0);
    const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0,
        reinterpret_cast<const u8*>(bytes.data()), bytes.size(), wide.data(), wide.size());
    wide.resize(r.count);
    return wide;
}

bool write_units(Marshaler& m, GuestAddr at, const std::vector<u16>& units) {
    for (usize i = 0; i < units.size(); ++i) {
        if (m.write_u16(at + i * 2, units[i]).is_err()) return false;
    }
    return m.write_u16(at + units.size() * 2, 0).is_ok();
}

bool write_ansi_units(Marshaler& m, GuestAddr at, const std::vector<u16>& units) {
    std::vector<u8> bytes(units.size() * 2 + 1, 0);
    const auto r = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, units.data(), units.size(),
                                                bytes.data(), bytes.size(), nullptr, false);
    for (u32 i = 0; i < r.count; ++i) {
        if (m.write_u8(at + i, bytes[i]).is_err()) return false;
    }
    return m.write_u8(at + r.count, 0).is_ok();
}

/// PathMatchSpecW: a ';'-separated list, each entry trimmed of leading
/// spaces; "*.*" matches every name and an empty entry matches (measured).
bool path_match_spec_list(const std::vector<u16>& path,
                          const std::vector<u16>& spec) {
    usize pos = 0;
    while (true) {
        usize end = pos;
        while (end < spec.size() && spec[end] != ';') ++end;
        usize start = pos;
        while (start < end && spec[start] == ' ') ++start;
        std::vector<u16> one(spec.begin() + static_cast<std::ptrdiff_t>(start),
                             spec.begin() + static_cast<std::ptrdiff_t>(end));
        const bool star_dot_star = one.size() == 3 && one[0] == '*' && one[1] == '.' && one[2] == '*';
        if (one.empty() || star_dot_star || path_match_spec_one(path, one)) return true;
        if (end >= spec.size()) return false;
        pos = end + 1;
    }
}

bool path_match_spec_one(const std::vector<u16>& path,
                         const std::vector<u16>& spec) {
    // PathMatchSpec uses DOS-style '*' and '?' wildcards and compares path
    // text case-insensitively. Keep the matching iterative so a guest can
    // supply a long pattern without consuming the host stack.
    usize path_pos = 0;
    usize spec_pos = 0;
    usize star_pos = std::string::npos;
    usize retry_path_pos = 0;

    while (path_pos < path.size()) {
        if (spec_pos < spec.size() &&
            (spec[spec_pos] == static_cast<u16>('?') ||
             tolower_ascii_u16(spec[spec_pos]) ==
                 tolower_ascii_u16(path[path_pos]))) {
            ++path_pos;
            ++spec_pos;
            continue;
        }

        if (spec_pos < spec.size() &&
            spec[spec_pos] == static_cast<u16>('*')) {
            star_pos = spec_pos++;
            retry_path_pos = path_pos;
            continue;
        }

        if (star_pos != std::string::npos) {
            spec_pos = star_pos + 1;
            path_pos = ++retry_path_pos;
            continue;
        }

        return false;
    }

    while (spec_pos < spec.size() &&
           spec[spec_pos] == static_cast<u16>('*')) {
        ++spec_pos;
    }
    return spec_pos == spec.size();
}

} // namespace

void set_shlwapi_locale_strings(LocaleStringManager* strings) { g_locale_strings.store(strings); }

DllExports create_shlwapi_thunks(capsule::Vfs* vfs, ComCoreManager* com,
                                 RegistryThunks* registry, GlobalLocalManager* local_heap) {
    DllExports exports;
    exports.entries.reserve(20);

    // shlwapi.h: void ColorRGBToHLS(COLORREF, WORD*, WORD*, WORD*).
    // Rounding and achromatic hue measured by color_rgb_hls_probe.c.
    exports.entries.push_back(ExportEntry{
        "ColorRGBToHLS", 0,
        [](Marshaler& m) {
            const u32 color = m.arg32(0);
            const i32 r = color & 255u, g = (color >> 8) & 255u,
                      b = (color >> 16) & 255u;
            const i32 hi = std::max({r, g, b}), lo = std::min({r, g, b});
            const i32 sum = hi + lo, delta = hi - lo;
            const i32 luminance = (sum * 240 + 255) / 510;
            i32 hue = 160, saturation = 0;
            if (delta != 0) {
                const i32 divisor = luminance <= 120 ? sum : 510 - sum;
                saturation = (delta * 240 + divisor / 2) / divisor;
                const i32 rd = ((hi - r) * 40 + delta / 2) / delta;
                const i32 gd = ((hi - g) * 40 + delta / 2) / delta;
                const i32 bd = ((hi - b) * 40 + delta / 2) / delta;
                hue = hi == r ? bd - gd : hi == g ? 80 + rd - bd : 160 + gd - rd;
                if (hue < 0) hue += 240;
                if (hue > 240) hue -= 240;
            }
            (void)m.memory().write_u16(m.arg_ptr(1), static_cast<u16>(hue));
            (void)m.memory().write_u16(m.arg_ptr(2), static_cast<u16>(luminance));
            (void)m.memory().write_u16(m.arg_ptr(3), static_cast<u16>(saturation));
        },
        16
    });


    exports.entries.push_back(ExportEntry{
        "ColorHLSToRGB", 0,
        [](Marshaler& m) {
            const i32 h = static_cast<u16>(m.arg32(0));
            const i32 l = static_cast<u16>(m.arg32(1));
            const i32 s = static_cast<u16>(m.arg32(2));
            if (s == 0) {
                // Windows requires the achromatic hue sentinel (oracle).
                const u32 gray = h == 160 ? static_cast<u32>(l * 255 / 240) : 0;
                m.set_return(gray | (gray << 8) | (gray << 16));
                return;
            }
            const i32 upper = l <= 120 ? (l * (240 + s) + 120) / 240
                                       : l + s - (l * s + 120) / 240;
            const i32 lower = 2 * l - upper;
            const auto channel = [&](i32 hue) -> u32 {
                if (hue < 0) hue += 240;
                if (hue > 240) hue -= 240;
                const i32 value = hue < 40 ? lower + ((upper - lower) * hue + 20) / 40
                    : hue < 120 ? upper
                    : hue < 160 ? lower + ((upper - lower) * (160 - hue) + 20) / 40
                    : lower;
                return static_cast<u32>((value * 255 + 120) / 240);
            };
            m.set_return(channel(h + 80) | (channel(h) << 8) | (channel(h - 80) << 16));
        },
        12
    });

    // BOOL IsOS(DWORD dwOS). Windows Vista and later export this by name;
    // older callers resolve the same contract through SHLWAPI ordinal 437.
    exports.entries.push_back(ExportEntry{
        "IsOS", 437,
        [](Marshaler& m) {
            // OS_WOW6432 (30): a 32-bit process on 64-bit Windows, the view
            // IsWow64Process gives (measured: 1 for x86, 0 for x64).
            constexpr u32 OS_WOW6432 = 30;
            const u32 feature = m.arg32(0);
            if (feature == OS_WOW6432) {
                m.set_return(m.cpu_is_64bit() ? 0u : 1u);
                return;
            }
            m.set_return(is_os(feature) ? 1u : 0u);
        },
        4
    });

    // IStream* SHCreateMemStream(const BYTE* pInit, UINT cbInit)
    //
    // This API is exported by ordinal 12 on every supported Windows version
    // (and by name on Vista and later). The returned pointer is a real
    // guest-visible COM IStream owned by the process COM manager.
    exports.entries.push_back(ExportEntry{
        "SHCreateMemStream", 12,
        [com](Marshaler& m) {
            if (com == nullptr) {
                m.set_return(0);
                return;
            }
            const GuestAddr initial = m.arg_ptr(0);
            const u32 size = static_cast<u32>(m.arg(1));
            m.set_return(com->create_memory_stream(m, initial, size));
        },
        8
    });

    // LPWSTR StrChrW(LPCWSTR pszStart, WCHAR wMatch)
    exports.entries.push_back(ExportEntry{
        "StrChrW", 0,
        [](Marshaler& m) {
            GuestAddr start = m.arg_ptr(0);
            u16 match = static_cast<u16>(m.arg(1) & 0xFFFF);
            GuestAddr found = str_chr_w(m, start, match);
            m.set_return(static_cast<u64>(found));
        },
        8
    });

    // int StrCmpW(LPCWSTR psz1, LPCWSTR psz2)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-strcmpw
    // Compared over raw UTF-16 code units: narrowing to the host string type
    // first would fold distinct characters together and report equality for
    // strings the guest expects to differ.
    exports.entries.push_back(ExportEntry{
        "StrCmpW", 0,
        [](Marshaler& m) {
            const GuestAddr a = m.arg_ptr(0);
            const GuestAddr b = m.arg_ptr(1);
            if (a == 0 || b == 0) {
                // Learn does not define NULL inputs; order them consistently
                // rather than dereferencing.
                m.set_return(static_cast<u64>(static_cast<i64>(
                    (a == b) ? 0 : (a == 0 ? -1 : 1))));
                return;
            }
            i32 result = 0;
            for (usize i = 0; i < 0x8000; ++i) {
                const auto ca = m.read_u16(a + static_cast<GuestAddr>(i * 2));
                const auto cb = m.read_u16(b + static_cast<GuestAddr>(i * 2));
                if (ca.is_err() || cb.is_err()) break;
                const u16 va = ca.value();
                const u16 vb = cb.value();
                if (va != vb) {
                    result = (va < vb) ? -1 : 1;
                    break;
                }
                if (va == 0) break;
            }
            m.set_return(static_cast<u64>(static_cast<i64>(result)));
        },
        8
    });

    // int StrCmpIW(PCWSTR psz1, PCWSTR psz2); int StrCmpN{,I}{A,W}(psz1, psz2,
    // int nChar). https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-strcmpiw,
    // -strcmpnw, -strcmpniw: zero when equal, negative when psz1 is less,
    // positive when greater; the I forms ignore case; the N forms compare at
    // most nChar characters from the beginning.
    exports.entries.push_back(ExportEntry{"StrCmpIW", 0, [](Marshaler& m) {
        m.set_return(static_cast<u64>(static_cast<i64>(
            shlwapi_compare(m, true, true, m.arg_ptr(0), m.arg_ptr(1), std::nullopt))));
    }, 8});
    for (const bool wide : {true, false}) {
        for (const bool ignore_case : {false, true}) {
            const std::string name = std::string(ignore_case ? "StrCmpNI" : "StrCmpN") + (wide ? "W" : "A");
            exports.entries.push_back(ExportEntry{name, 0, [wide, ignore_case](Marshaler& m) {
                m.set_return(static_cast<u64>(static_cast<i64>(shlwapi_compare(
                    m, wide, ignore_case, m.arg_ptr(0), m.arg_ptr(1), static_cast<i32>(m.arg32(2))))));
            }, 12});
        }
    }

    // BOOL PathCanonicalizeW(LPWSTR pszDst, LPCWSTR pszSrc)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathcanonicalizew
    // BOOL PathCanonicalize{W,A}(LPTSTR pszBuf, LPCTSTR pszPath)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathcanonicalizew
    // Measured (s7_shlwapi_probe.expected): a NULL argument is FALSE /
    // ERROR_INVALID_PARAMETER; a source of MAX_PATH or more (units for W,
    // bytes for A) is FALSE / ERROR_FILENAME_EXCED_RANGE with pszBuf emptied;
    // success leaves the last error alone.
    const auto path_canonicalize = [](Marshaler& m, bool wide) {
        const GuestAddr dst = m.arg_ptr(0);
        const GuestAddr src = m.arg_ptr(1);
        if (dst == 0 || src == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        usize byte_len = 0;
        const std::vector<u16> in = wide ? read_units(m, src) : read_ansi_units(m, src, &byte_len);
        std::vector<u16> out;
        const bool too_long = !wide && byte_len >= 260;
        if (too_long || !path_canonicalize_units(in, out)) {
            if (wide) (void)m.write_u16(dst, 0);
            else (void)m.write_u8(dst, 0);
            set_last_error(206u);  // ERROR_FILENAME_EXCED_RANGE
            m.set_return(0);
            return;
        }
        m.set_return((wide ? write_units(m, dst, out) : write_ansi_units(m, dst, out)) ? 1 : 0);
    };
    exports.entries.push_back(ExportEntry{
        "PathCanonicalizeW", 0,
        [path_canonicalize](Marshaler& m) { path_canonicalize(m, true); }, 8});
    exports.entries.push_back(ExportEntry{
        "PathCanonicalizeA", 0,
        [path_canonicalize](Marshaler& m) { path_canonicalize(m, false); }, 8});


    // BOOL PathIsRelativeW(LPCWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathIsRelativeW", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            auto s = m.read_wide_string(path_ptr, 4096);
            if (s.is_err()) {
                m.set_return(1);
                return;
            }
            bool rel = is_relative_ascii_path(s.value());
            m.set_return(rel ? 1 : 0);
        },
        4
    });

    // BOOL PathIsRelativeA(LPCSTR pszPath)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathisrelativea
    exports.entries.push_back(ExportEntry{
        "PathIsRelativeA", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(1);
                return;
            }
            auto s = m.read_ansi_string(path_ptr, 4096);
            if (s.is_err()) {
                m.set_return(1);
                return;
            }
            m.set_return(is_relative_ascii_path(s.value()) ? 1 : 0);
        },
        4
    });

    // LPWSTR PathCombineW(LPWSTR pszDest, LPCWSTR pszDir, LPCWSTR pszFile)
    exports.entries.push_back(ExportEntry{
        "PathCombineW", 0,
        [](Marshaler& m) {
            GuestAddr dest_ptr = m.arg_ptr(0);
            GuestAddr dir_ptr = m.arg_ptr(1);
            GuestAddr file_ptr = m.arg_ptr(2);

            auto dir = m.read_wide_string(dir_ptr, 4096);
            auto file = m.read_wide_string(file_ptr, 4096);
            if (dir.is_err() || file.is_err() || dest_ptr == 0) {
                m.set_return(0);
                return;
            }
            std::string combined = path_combine_ascii(dir.value(), file.value());
            (void)m.write_wide_string(dest_ptr, combined);
            m.set_return(static_cast<u64>(dest_ptr));
        },
        12
    });

    // LPSTR PathCombineA(LPSTR pszDest, LPCSTR pszDir, LPCSTR pszFile)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathcombinea
    exports.entries.push_back(ExportEntry{
        "PathCombineA", 0,
        [](Marshaler& m) {
            GuestAddr dest_ptr = m.arg_ptr(0);
            GuestAddr dir_ptr = m.arg_ptr(1);
            GuestAddr file_ptr = m.arg_ptr(2);
            if (dest_ptr == 0 || (dir_ptr == 0 && file_ptr == 0)) {
                m.set_return(0);
                return;
            }
            std::string dir;
            std::string file;
            if (dir_ptr != 0) {
                auto r = m.read_ansi_string(dir_ptr, 4096);
                if (r.is_err()) {
                    m.set_return(0);
                    return;
                }
                dir = std::move(r.value());
            }
            if (file_ptr != 0) {
                auto r = m.read_ansi_string(file_ptr, 4096);
                if (r.is_err()) {
                    m.set_return(0);
                    return;
                }
                file = std::move(r.value());
            }
            std::string combined = path_combine_ascii(dir, file);
            if (m.write_ansi_string(dest_ptr, combined).is_err()) {
                m.set_return(0);
                return;
            }
            m.set_return(static_cast<u64>(dest_ptr));
        },
        12
    });

    // BOOL PathAppend{W,A}(LPTSTR pszPath, LPCTSTR pszMore)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathappendw
    // Measured rules in path_append_units; on overflow pszPath is emptied
    // and FALSE returned with the last error untouched.
    const auto path_append = [](Marshaler& m, bool wide) {
        const GuestAddr path_ptr = m.arg_ptr(0);
        const GuestAddr more_ptr = m.arg_ptr(1);
        if (path_ptr == 0 || more_ptr == 0) {
            m.set_return(0);
            return;
        }
        const std::vector<u16> path = wide ? read_units(m, path_ptr) : read_ansi_units(m, path_ptr);
        const std::vector<u16> more = wide ? read_units(m, more_ptr) : read_ansi_units(m, more_ptr);
        std::vector<u16> out;
        if (!path_append_units(path, more, out)) {
            if (wide) (void)m.write_u16(path_ptr, 0);
            else (void)m.write_u8(path_ptr, 0);
            m.set_return(0);
            return;
        }
        m.set_return((wide ? write_units(m, path_ptr, out) : write_ansi_units(m, path_ptr, out)) ? 1 : 0);
    };
    exports.entries.push_back(ExportEntry{
        "PathAppendW", 0,
        [path_append](Marshaler& m) { path_append(m, true); }, 8});
    exports.entries.push_back(ExportEntry{
        "PathAppendA", 0,
        [path_append](Marshaler& m) { path_append(m, false); }, 8});

    // BOOL PathRemoveFileSpecW(LPWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathRemoveFileSpecW", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto path = m.read_wide_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string s = normalize_seps(path.value());
            s = strip_trailing_seps(std::move(s));
            auto pos = s.find_last_of('\\');
            if (pos == std::string::npos) {
                m.set_return(0);
                return;
            }
            s.resize(pos);
            (void)m.write_wide_string(path_ptr, s);
            m.set_return(1);
        },
        4
    });

    // BOOL PathRemoveFileSpecA(LPSTR pszPath)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathremovefilespeca
    exports.entries.push_back(ExportEntry{
        "PathRemoveFileSpecA", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto path = m.read_ansi_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string s = normalize_seps(path.value());
            s = strip_trailing_seps(std::move(s));
            auto pos = s.find_last_of('\\');
            if (pos == std::string::npos) {
                m.set_return(0);
                return;
            }
            s.resize(pos);
            if (m.write_ansi_string(path_ptr, s).is_err()) {
                m.set_return(0);
                return;
            }
            m.set_return(1);
        },
        4
    });

    // LPWSTR PathAddBackslashW(LPWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathAddBackslashW", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto path = m.read_wide_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string s = normalize_seps(path.value());
            if (s.empty() || !is_path_sep(s.back())) {
                s.push_back('\\');
                (void)m.write_wide_string(path_ptr, s);
            }
            // Return pointer to the null terminator AFTER the (possibly appended) backslash.
            // String length in UTF-16 code units = s.size(), so null terminator is at s.size()*2.
            m.set_return(static_cast<u64>(path_ptr + s.size() * 2));
        },
        4
    });

    // HRESULT DllGetVersion(DLLVERSIONINFO* pdvi)
    exports.entries.push_back(ExportEntry{
        "DllGetVersion", 0,
        [](Marshaler& m) {
            GuestAddr pdvi = m.arg_ptr(0);
            if (pdvi != 0) {
                // DLLVERSIONINFO:
                // DWORD cbSize;
                // DWORD dwMajorVersion;
                // DWORD dwMinorVersion;
                // DWORD dwBuildNumber;
                // DWORD dwPlatformID;
                // We'll write a plausible version.
                (void)m.write_u32(pdvi + 4, 6);  // major
                (void)m.write_u32(pdvi + 8, 0);  // minor
                (void)m.write_u32(pdvi + 12, 0); // build
                (void)m.write_u32(pdvi + 16, 2); // VER_PLATFORM_WIN32_NT
            }
            m.set_return(S_OK);
        },
        4
    });

    // HRESULT SHAutoComplete(HWND hwndEdit, DWORD dwFlags)
    // Used to enable shell-style autocomplete in edit controls. Safe to stub.
    exports.entries.push_back(ExportEntry{
        "SHAutoComplete", 0,
        [](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
            (void)m;
            m.set_return(S_OK);
        },
        8
    });

    // BOOL PathFileExistsW(LPCWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathFileExistsW", 0,
        [vfs](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0 || vfs == nullptr) {
                m.set_return(0);
                return;
            }
            auto path = m.read_wide_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            const std::string& s = path.value();
            if (s.empty()) {
                m.set_return(0);
                return;
            }
            // Normalize forward slashes to backslashes for VFS
            std::string win_path = s;
            std::replace(win_path.begin(), win_path.end(), '/', '\\');
            auto info = vfs->stat(win_path);
            const bool exists = info.is_ok();
            VITREO_TRACE("[shlwapi] PathFileExistsW('%s') -> %s\n",
                         s.c_str(), exists ? "TRUE" : "FALSE");
            // Learn: "If the file does not exist, GetLastError will return
            // ERROR_FILE_NOT_FOUND"; measured: ERROR_PATH_NOT_FOUND when a
            // directory on the way is missing.
            if (!exists) set_last_error(vfs_error_to_win32(info.error()));
            m.set_return(exists ? 1 : 0);
        },
        4
    });

    // Learn PathIsDirectoryEmptyA/W; Windows oracle captures define BOOL,
    // LastError, search-buffer bounds and bare-drive behavior.
    for (bool wide : {false, true}) exports.entries.push_back(ExportEntry{
        wide ? directory_empty_sdk::name_W : directory_empty_sdk::name_A, 0,
        [vfs, wide](Marshaler& m) {
            const GuestAddr pointer = m.arg_ptr(0);
            if (pointer == 0) { m.set_return(0); return; }
            std::vector<u16> units;
            std::vector<u8> bytes;
            bool terminated = false;
            for (usize i = 0; i < directory_empty_sdk::MAX_PATH; ++i) {
                const GuestAddr address = pointer + i * (wide ? sizeof(u16) : sizeof(u8));
                u16 value;
                if (wide) {
                    auto read = m.read_u16(address);
                    if (read.is_err()) { (void)m.raise_access_violation(false, address); m.set_return(0); return; }
                    value = read.value();
                } else {
                    auto read = m.read_u8(address);
                    if (read.is_err()) { (void)m.raise_access_violation(false, address); m.set_return(0); return; }
                    value = read.value();
                }
                if (value == 0) { terminated = true; break; }
                if (wide) units.push_back(value); else bytes.push_back(static_cast<u8>(value));
            }
            if (!terminated) { m.set_return(0); return; }
            if (!wide && !bytes.empty()) {
                units.resize(bytes.size());
                const auto decoded = codepage::multi_byte_to_wide(codepage::kCpAcp, 0,
                    bytes.data(), bytes.size(), units.data(), units.size());
                if (!decoded.ok()) { set_last_error(decoded.error); m.set_return(0); return; }
                units.resize(decoded.count);
            }
            if (units.empty()) { m.set_return(0); return; }
            const bool separator = units.back() == '\\' || units.back() == '/';
            // The measured search appends "\\*.*" (or "*.*" after a separator).
            const usize search_length = wide ? units.size() : bytes.size();
            if (search_length + (separator ? 3u : 4u) >= directory_empty_sdk::MAX_PATH) {
                m.set_return(0); return;
            }
            const bool extended = units.size() >= 4 && units[0] == '\\' &&
                units[1] == '\\' && units[2] == '?' && units[3] == '\\';
            const auto name_begin = units.begin() + (extended ? 4 : 0);
            if (std::find(name_begin, units.end(), static_cast<u16>('*')) != units.end() ||
                std::find(name_begin, units.end(), static_cast<u16>('?')) != units.end()) {
                set_last_error(directory_empty_sdk::ERROR_INVALID_NAME); m.set_return(0); return;
            }
            std::vector<u8> utf8(units.size() * 3);
            const auto encoded = codepage::wide_to_multi_byte(codepage::kCpUtf8, 0,
                units.data(), units.size(), utf8.data(), utf8.size(), nullptr, false);
            if (!encoded.ok()) { set_last_error(encoded.error); m.set_return(0); return; }
            std::string path(reinterpret_cast<const char*>(utf8.data()), encoded.count);
            std::replace(path.begin(), path.end(), '/', '\\');
            // The VFS has drive mounts, not network-share/device mounts. Do
            // not reinterpret these namespaces as a default-drive directory.
            if (path.starts_with("\\\\")) {
                const bool drive_namespace = path.size() >= 7 &&
                    (path[2] == '?' || path[2] == '.') && path[3] == '\\' &&
                    ((path[4] >= 'A' && path[4] <= 'Z') || (path[4] >= 'a' && path[4] <= 'z')) &&
                    path[5] == ':' && path[6] == '\\';
                if (!drive_namespace) {
                    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
                    if (!reported.test_and_set()) std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED shlwapi.dll!%s: UNC or device namespace has no capsule mount\n",
                        wide ? directory_empty_sdk::name_W : directory_empty_sdk::name_A);
                    set_last_error(directory_empty_sdk::ERROR_CALL_NOT_IMPLEMENTED);
                    m.set_return(0); return;
                }
            }
            // Unlike C:., this API searches the root for the bare C: form.
            if (path.size() == 2 && path[1] == ':') path.push_back('\\');
            // Search treats trailing spaces as part of the directory component;
            // keep them interior so VFS's final-component trimming cannot erase them.
            if (path.back() == ' ') path.push_back('\\');
            if (vfs == nullptr) {
                static std::atomic_flag reported = ATOMIC_FLAG_INIT;
                if (!reported.test_and_set()) std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED shlwapi.dll!%s: capsule VFS unavailable\n",
                    wide ? directory_empty_sdk::name_W : directory_empty_sdk::name_A);
                set_last_error(directory_empty_sdk::ERROR_CALL_NOT_IMPLEMENTED); m.set_return(0); return;
            }
            const auto entries = vfs->list_dir(path);
            if (entries.is_err()) {
                const auto error = entries.error();
                set_last_error(error == capsule::VfsError::FileNotFound || error == capsule::VfsError::PathNotFound ||
                    error == capsule::VfsError::DriveNotMounted
                    ? directory_empty_sdk::ERROR_PATH_NOT_FOUND
                    : error == capsule::VfsError::NotADirectory ? directory_empty_sdk::ERROR_DIRECTORY
                    : vfs_error_to_win32(error));
                m.set_return(0); return;
            }
            const bool empty = std::none_of(entries.value().begin(), entries.value().end(),
                [](const capsule::DirectoryEntry& entry) { return entry.name != "." && entry.name != ".."; });
            if (empty) set_last_error(directory_empty_sdk::ERROR_NO_MORE_FILES);
            m.set_return(empty ? 1 : 0);
        }, directory_empty_sdk::argument_bytes
    });

    // BOOL PathIsDirectoryW(LPCWSTR pszPath)
    // Learn: returns (BOOL)FILE_ATTRIBUTE_DIRECTORY (0x10) when the path is a
    // directory; otherwise FALSE.
    exports.entries.push_back(ExportEntry{
        "PathIsDirectoryW", 0,
        [vfs](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0 || vfs == nullptr) {
                m.set_return(0);
                return;
            }
            auto path = m.read_wide_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string win_path = path.value();
            if (win_path.empty()) {
                m.set_return(0);
                return;
            }
            std::replace(win_path.begin(), win_path.end(), '/', '\\');
            constexpr u32 FILE_ATTRIBUTE_DIRECTORY = 0x10;
            u32 result = 0;
            if (auto st = vfs->stat(win_path); st.is_ok() &&
                capsule::has_attribute(st.value().attributes,
                                       capsule::FileAttributes::Directory)) {
                result = FILE_ATTRIBUTE_DIRECTORY;
            }
            VITREO_TRACE("[shlwapi] PathIsDirectoryW('%s') -> %s\n",
                         win_path.c_str(), result ? "TRUE" : "FALSE");
            m.set_return(result);
        },
        4
    });

    // BOOL PathFileExistsA(LPCSTR pszPath)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathfileexistsa
    exports.entries.push_back(ExportEntry{
        "PathFileExistsA", 0,
        [vfs](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0 || vfs == nullptr) {
                m.set_return(0);
                return;
            }
            auto path = m.read_ansi_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string win_path = path.value();
            std::replace(win_path.begin(), win_path.end(), '/', '\\');
            auto info = vfs->stat(win_path);
            const bool exists = info.is_ok();
            if (!exists) set_last_error(vfs_error_to_win32(info.error()));
            VITREO_TRACE("[shlwapi] PathFileExistsA('%s') -> %s\n",
                         win_path.c_str(), exists ? "TRUE" : "FALSE");
            m.set_return(exists ? 1 : 0);
        },
        4
    });

    // BOOL PathMatchSpecW(LPCWSTR pszFile, LPCWSTR pszSpec)
    exports.entries.push_back(ExportEntry{
        "PathMatchSpecW", 0,
        [](Marshaler& m) {
            const GuestAddr path_ptr = m.arg_ptr(0);
            const GuestAddr spec_ptr = m.arg_ptr(1);
            if (path_ptr == 0 || spec_ptr == 0) {
                m.set_return(0);
                return;
            }

            const auto path = read_utf16_z(m, path_ptr, 4096);
            const auto spec = read_utf16_z(m, spec_ptr, 4096);
            const bool matches = path_match_spec_list(path, spec);
            VITREO_TRACE("[shlwapi] PathMatchSpecW(path_len=%zu, spec_len=%zu) -> %s\n",
                         path.size(), spec.size(), matches ? "TRUE" : "FALSE");
            m.set_return(matches ? 1u : 0u);
        },
        8
    });

    // StrPBrk, PathFindFileName, PathStripPath, PathGetDriveNumber,
    // PathCompactPathEx and PathRelativePathTo (A and W).
    add_shlwapi_path_extras(exports);

    // The URL family (UrlCanonicalize ... ParseURL, PathIsURL, HashData,
    // GetAcceptLanguages), A and W.
    add_shlwapi_url_exports(exports, registry, local_heap);

    // WCHAR SHStripMneumonicW(LPWSTR pszMenu) — ordinal 225
    // CHAR  SHStripMneumonicA(LPSTR  pszMenu) — ordinal 203
    //
    // Removes the mnemonic marker from a menu string in place and returns the
    // mnemonic character, or 0 if there was none. Documented behavior:
    //
    //   "Str&ing"     -> "String"  mnemonic 'i'
    //   "String (&S)" -> "String"  mnemonic 'S'   (Unicode only, XP or later)
    //
    // Imported by ordinal through the Visual C++ delay-load helper. A missing
    // export makes GetProcAddress return NULL, the helper raise 0xC06D007F
    // (ERROR_PROC_NOT_FOUND), and the caller then call the address the helper
    // never filled in.
    //
    // "&&" is the established escape for a literal ampersand, so it collapses
    // to one '&' and is never treated as the mnemonic. Only the first mnemonic
    // marker is consumed; any later lone '&' is left as written, matching the
    // singular "the mnemonic marker" the documentation describes.
    const auto strip_mneumonic = [](Marshaler& m, bool wide) {
        const GuestAddr text = m.arg_ptr(0);
        if (text == 0) {
            m.set_return(0);
            return;
        }
        constexpr u32 MAX_UNITS = 4096;
        const u32 unit = wide ? 2u : 1u;

        std::vector<u32> in;
        in.reserve(64);
        for (u32 i = 0; i < MAX_UNITS; ++i) {
            const GuestAddr at = text + i * unit;
            u32 value = 0;
            if (wide) {
                auto r = m.read_u16(at);
                if (r.is_err()) { m.set_return(0); return; }
                value = r.value();
            } else {
                auto r = m.read_u8(at);
                if (r.is_err()) { m.set_return(0); return; }
                value = r.value();
            }
            if (value == 0) break;
            in.push_back(value);
        }

        std::vector<u32> out;
        out.reserve(in.size());
        u32 mneumonic = 0;
        for (usize i = 0; i < in.size();) {
            // "(&X)" is the parenthesized form; it takes a preceding space too.
            if (mneumonic == 0 && wide && in[i] == '(' && i + 3 < in.size() &&
                in[i + 1] == '&' && in[i + 2] != ')' && in[i + 3] == ')') {
                mneumonic = in[i + 2];
                if (!out.empty() && out.back() == ' ') out.pop_back();
                i += 4;
                continue;
            }
            if (in[i] == '&' && i + 1 < in.size() && in[i + 1] == '&') {
                out.push_back('&');   // escaped literal ampersand
                i += 2;
                continue;
            }
            if (mneumonic == 0 && in[i] == '&' && i + 1 < in.size()) {
                mneumonic = in[i + 1];
                ++i;                  // drop the marker, keep the character
                continue;
            }
            out.push_back(in[i]);
            ++i;
        }

        for (usize i = 0; i < out.size(); ++i) {
            const GuestAddr at = text + static_cast<u32>(i) * unit;
            if (wide) (void)m.write_u16(at, static_cast<u16>(out[i]));
            else (void)m.write_u8(at, static_cast<u8>(out[i]));
        }
        const GuestAddr terminator =
            text + static_cast<u32>(out.size()) * unit;
        if (wide) (void)m.write_u16(terminator, 0);
        else (void)m.write_u8(terminator, 0);

        m.set_return(mneumonic);
    };

    // HRESULT QISearch(void* that, LPCQITAB pqit, REFIID riid, void** ppv)
    // shlwapi ordinal 219 (dumpbin shlwapi.dll). Learn:
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-qisearch
    // QITAB is { const IID* piid; int dwOffset; } with the pointer's
    // alignment, so the stride is 8 on x86 and 16 on x64. The table ends
    // at piid == NULL. IUnknown selects the first entry. A match writes
    // (BYTE*)that + dwOffset and AddRefs that interface.
    exports.entries.push_back(ExportEntry{
        "QISearch", 219,
        [com](Marshaler& m) {
            constexpr u32 E_NOINTERFACE = 0x80004002u;
            constexpr u32 E_POINTER = 0x80004003u;
            constexpr u32 E_INVALIDARG = 0x80070057u;
            constexpr u8 kIUnknown[16] = {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
            const GuestAddr that = m.arg_ptr(0);
            const GuestAddr pqit = m.arg_ptr(1);
            const GuestAddr riid = m.arg_ptr(2);
            const GuestAddr ppv = m.arg_ptr(3);
            if (ppv == 0) {
                m.set_return(E_POINTER);
                return;
            }
            const auto clear_out = [&] { (void)m.write_ptr(ppv, 0); };
            if (that == 0 || pqit == 0 || riid == 0) {
                clear_out();
                m.set_return(E_INVALIDARG);
                return;
            }
            std::array<u8, 16> want{};
            if (m.memory().read_bytes(riid, reinterpret_cast<Byte*>(want.data()), 16)
                    .is_err()) {
                clear_out();
                m.set_return(E_INVALIDARG);
                return;
            }
            const bool want_unknown =
                std::equal(std::begin(kIUnknown), std::end(kIUnknown), want.begin());
            const u32 stride = m.cpu_is_64bit() ? 16u : 8u;
            const u32 offset_at = m.cpu_is_64bit() ? 8u : 4u;
            GuestAddr first_iface = 0;
            bool have_first = false;
            for (u32 i = 0; i < 64; ++i) {
                const GuestAddr entry = pqit + i * stride;
                const auto piid_r = m.read_ptr(entry);
                const auto off_r = m.read_u32(entry + offset_at);
                if (piid_r.is_err() || off_r.is_err()) {
                    clear_out();
                    m.set_return(E_INVALIDARG);
                    return;
                }
                const GuestAddr piid = static_cast<GuestAddr>(piid_r.value());
                if (piid == 0) break;
                const i32 dw_offset = static_cast<i32>(off_r.value());
                const GuestAddr iface = static_cast<GuestAddr>(
                    static_cast<i64>(static_cast<u64>(that)) + dw_offset);
                if (!have_first) {
                    first_iface = iface;
                    have_first = true;
                }
                std::array<u8, 16> got{};
                if (m.memory().read_bytes(piid, reinterpret_cast<Byte*>(got.data()), 16)
                        .is_err()) {
                    clear_out();
                    m.set_return(E_INVALIDARG);
                    return;
                }
                if (want == got || (want_unknown && i == 0)) {
                    (void)m.write_ptr(ppv, iface);
                    if (com != nullptr) (void)com->guest_iunknown_addref(m, iface);
                    m.set_return(S_OK);
                    return;
                }
            }
            if (want_unknown && have_first) {
                (void)m.write_ptr(ppv, first_iface);
                if (com != nullptr) (void)com->guest_iunknown_addref(m, first_iface);
                m.set_return(S_OK);
                return;
            }
            clear_out();
            m.set_return(E_NOINTERFACE);
        },
        16
    });

    exports.entries.push_back(ExportEntry{
        "SHStripMneumonicW", 225,
        [strip_mneumonic](Marshaler& m) { strip_mneumonic(m, true); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SHStripMneumonicA", 203,
        [strip_mneumonic](Marshaler& m) { strip_mneumonic(m, false); },
        4
    });

    // LPWSTR PathFindExtensionW(LPCWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathFindExtensionW", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            GuestAddr last_dot = 0;
            GuestAddr null_term = path_ptr;
            for (usize i = 0; i < 65536; i++) {
                auto ch = m.read_u16(path_ptr + i * 2);
                if (ch.is_err() || ch.value() == 0) {
                    null_term = path_ptr + i * 2;
                    break;
                }
                if (ch.value() == static_cast<u16>('.')) {
                    last_dot = path_ptr + i * 2;
                }
                // Measured: '\\' and ' ' end an extension ("a.b c" has
                // none); '/' does not ("a.b/c" -> ".b/c").
                if (ch.value() == static_cast<u16>('\\') || ch.value() == static_cast<u16>(' ')) {
                    last_dot = 0;
                }
            }
            m.set_return(static_cast<u64>(last_dot ? last_dot : null_term));
        },
        4
    });

    // LPCSTR PathFindExtensionA(LPCSTR pszPath)
    // Returns address of '.' before the extension, or of the terminating NUL.
    exports.entries.push_back(ExportEntry{
        "PathFindExtensionA", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            GuestAddr last_dot = 0;
            GuestAddr null_term = path_ptr;
            for (usize i = 0; i < 65536; i++) {
                auto ch = m.read_u8(path_ptr + i);
                if (ch.is_err() || ch.value() == 0) {
                    null_term = path_ptr + i;
                    break;
                }
                if (ch.value() == static_cast<u8>('.')) {
                    last_dot = path_ptr + i;
                }
                // Same rule as PathFindExtensionW (measured there).
                if (ch.value() == static_cast<u8>('\\') ||
                    ch.value() == static_cast<u8>(' ')) {
                    last_dot = 0;
                }
            }
            m.set_return(static_cast<u64>(last_dot ? last_dot : null_term));
        },
        4
    });

    // BOOL PathRenameExtensionA/W(LPTSTR pszPath, LPCTSTR pszExt)
    // https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-pathrenameextensiona
    const auto rename_extension = [](bool wide) {
        return [wide](Marshaler& m) {
            constexpr usize kMaxPath = 260;
            const GuestAddr path_ptr = m.arg_ptr(0);
            const GuestAddr ext_ptr = m.arg_ptr(1);
            if (path_ptr == 0 || ext_ptr == 0) {
                m.set_return(0);
                return;
            }
            const auto path = wide
                ? m.read_wide_string(path_ptr, kMaxPath)
                : m.read_ansi_string(path_ptr, kMaxPath);
            const auto ext = wide
                ? m.read_wide_string(ext_ptr, kMaxPath)
                : m.read_ansi_string(ext_ptr, kMaxPath);
            if (path.is_err() || ext.is_err()) {
                m.set_return(0);
                return;
            }
            std::string result = path.value();
            usize cut = result.size();
            for (usize i = 0; i < result.size(); ++i) {
                const char c = result[i];
                if (c == '.') cut = i;
                if (c == '\\' || c == '/') cut = result.size();
            }
            result.resize(cut);
            result += ext.value();
            if (result.size() >= kMaxPath) {
                m.set_return(0);
                return;
            }
            const auto written = wide
                ? m.write_wide_string(path_ptr, result)
                : m.write_ansi_string(path_ptr, result);
            m.set_return(written.is_ok() ? 1u : 0u);
        };
    };
    exports.entries.push_back(
        {"PathRenameExtensionA", 0, rename_extension(false), 8});
    exports.entries.push_back(
        {"PathRenameExtensionW", 0, rename_extension(true), 8});

    // BOOL PathStripToRootW(LPWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathStripToRootW", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto path = m.read_wide_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string s = path.value();
            if (s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) &&
                s[1] == ':' && (s[2] == '\\' || s[2] == '/')) {
                std::string root = s.substr(0, 3);
                root[2] = '\\';
                (void)m.write_wide_string(path_ptr, root);
                m.set_return(1);
            } else {
                m.set_return(0);
            }
        },
        4
    });

    // BOOL PathStripToRootA(LPSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathStripToRootA", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto path = m.read_ansi_string(path_ptr, 4096);
            if (path.is_err()) {
                m.set_return(0);
                return;
            }
            std::string s = path.value();
            if (s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) &&
                s[1] == ':' && (s[2] == '\\' || s[2] == '/')) {
                std::string root = s.substr(0, 3);
                root[2] = '\\';
                (void)m.write_ansi_string(path_ptr, root);
                m.set_return(1);
            } else {
                m.set_return(0);
            }
        },
        4
    });

    // BOOL PathIsUNCA(LPCSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathIsUNCA", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto ch0 = m.read_u8(path_ptr);
            auto ch1 = m.read_u8(path_ptr + 1);
            bool is_unc = ch0.is_ok() && ch1.is_ok() &&
                          ch0.value() == static_cast<u8>('\\') &&
                          ch1.value() == static_cast<u8>('\\');
            m.set_return(is_unc ? 1 : 0);
        },
        4
    });

    // BOOL PathIsUNCW(LPCWSTR pszPath)
    exports.entries.push_back(ExportEntry{
        "PathIsUNCW", 0,
        [](Marshaler& m) {
            GuestAddr path_ptr = m.arg_ptr(0);
            if (path_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto ch0 = m.read_u16(path_ptr);
            auto ch1 = m.read_u16(path_ptr + 2);
            bool is_unc = ch0.is_ok() && ch1.is_ok() &&
                          ch0.value() == static_cast<u16>('\\') &&
                          ch1.value() == static_cast<u16>('\\');
            m.set_return(is_unc ? 1 : 0);
        },
        4
    });

    // BOOL PathQuoteSpaces{A,W}(LPSTR/LPWSTR lpsz)       (MAX_PATH buffer)
    // BOOL PathUnquoteSpaces{A,W}(LPSTR/LPWSTR lpsz)
    //
    // Learn PathQuoteSpacesW / PathUnquoteSpacesW; the behaviour is
    // tools/oracle_probe/shlwapi_quote_spaces_probe{,.x86}.expected
    // (shlwapi.dll and kernelbase.dll agree, both bitnesses, A and W alike,
    // the code units compared raw: DBCS trail bytes are never ' ' or '"'):
    //  - Quote: TRUE when the string contains a space (U+0020 only: not a
    //    tab, not U+00A0) and its length is at most MAX_PATH-3, so the quoted
    //    string and its NUL fit MAX_PATH; it is wrapped in quotes even when
    //    already quoted. Otherwise FALSE and the buffer is untouched.
    //  - Unquote: TRUE when the first and the last unit are '"' (a lone '"'
    //    counts; no length limit). The last unit becomes NUL and the string
    //    and both NULs move down one unit: the units at [len-2] and [len-1]
    //    end NUL, the original NUL at [len] stays. Otherwise FALSE, untouched.
    //  - NULL is FALSE. Neither function changes the last error.
    auto quote_spaces = [](bool wide, bool quote) {
        return [wide, quote](Marshaler& m) {
            const GuestAddr at = m.arg_ptr(0);
            const u32 unit = wide ? 2u : 1u;
            if (at == 0) {
                m.set_return(0);
                return;
            }
            auto read = [&](usize i) -> std::optional<u16> {
                const GuestAddr a = at + static_cast<GuestAddr>(i * unit);
                if (wide) {
                    auto v = m.read_u16(a);
                    return v.is_ok() ? std::optional<u16>(v.value()) : std::nullopt;
                }
                auto v = m.read_u8(a);
                return v.is_ok() ? std::optional<u16>(v.value()) : std::nullopt;
            };
            auto write = [&](usize i, u16 v) {
                const GuestAddr a = at + static_cast<GuestAddr>(i * unit);
                return wide ? m.write_u16(a, v).is_ok() : m.write_u8(a, static_cast<u8>(v)).is_ok();
            };
            std::vector<u16> text;
            for (;;) {
                const auto c = read(text.size());
                if (!c) {
                    m.set_return(0);
                    return;
                }
                if (*c == 0) break;
                text.push_back(*c);
            }
            const usize len = text.size();
            constexpr usize kMaxPath = 260;
            bool ok = true;
            if (quote) {
                if (std::find(text.begin(), text.end(), u16{' '}) == text.end() || len + 3 > kMaxPath) {
                    m.set_return(0);
                    return;
                }
                ok = write(0, '"');
                for (usize i = 0; ok && i < len; ++i) ok = write(i + 1, text[i]);
                ok = ok && write(len + 1, '"') && write(len + 2, 0);
            } else {
                if (len == 0 || text.front() != '"' || text.back() != '"') {
                    m.set_return(0);
                    return;
                }
                for (usize i = 0; ok && i + 2 < len; ++i) ok = write(i, text[i + 1]);
                if (ok && len >= 2) ok = write(len - 2, 0);
                ok = ok && write(len - 1, 0);
            }
            m.set_return(ok ? 1 : 0);
        };
    };
    exports.entries.push_back(ExportEntry{"PathQuoteSpacesA", 0, quote_spaces(false, true), 4});
    exports.entries.push_back(ExportEntry{"PathQuoteSpacesW", 0, quote_spaces(true, true), 4});
    exports.entries.push_back(ExportEntry{"PathUnquoteSpacesA", 0, quote_spaces(false, false), 4});
    exports.entries.push_back(ExportEntry{"PathUnquoteSpacesW", 0, quote_spaces(true, false), 4});

    // LSTATUS SHDeleteKeyW(HKEY hkey, LPCWSTR pszSubKey)
    // LSTATUS SHDeleteKeyA(HKEY hkey, LPCSTR  pszSubKey)
    //
    // Learn: "Deletes a subkey and all its descendants." That recursion is the
    // whole reason the wrapper exists — RegDeleteKey on NT refuses a key that
    // still has subkeys — so this forwards to the registry's delete-tree core,
    // which removes every descendant and then the named subkey itself. A NULL
    // or empty pszSubKey clears hkey in place (its values and subkeys); the key
    // the caller holds open cannot delete itself.
    //
    // Returns a system error code (ERROR_SUCCESS on success), not a BOOL.
    // Deleting a key that is not there is ERROR_FILE_NOT_FOUND, which is what
    // the registry core already reports; without a registry the honest answer
    // is ERROR_INVALID_HANDLE rather than a fabricated success or a blanket
    // "not found" that hides an unwired runtime.
    auto delete_key_tree = [registry](Marshaler& m, bool wide) {
        const u64 hkey = m.arg(0);
        const GuestAddr subkey_ptr = m.arg_ptr(1);
        if (registry == nullptr) {
            m.set_return(6u); // ERROR_INVALID_HANDLE
            return;
        }
        std::wstring subkey;
        if (subkey_ptr != 0) {
            if (wide) {
                for (u16 ch : read_utf16_z(m, subkey_ptr, 4096)) {
                    subkey.push_back(static_cast<wchar_t>(ch));
                }
            } else {
                auto ansi = m.read_ansi_string(subkey_ptr, 4096);
                if (ansi.is_ok()) {
                    for (char c : ansi.value()) {
                        subkey.push_back(
                            static_cast<wchar_t>(static_cast<unsigned char>(c)));
                    }
                }
            }
        }
        m.set_return(registry->reg_delete_tree(hkey, subkey));
    };

    exports.entries.push_back(ExportEntry{
        "SHDeleteKeyW", 0,
        [delete_key_tree](Marshaler& m) { delete_key_tree(m, true); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SHDeleteKeyA", 0,
        [delete_key_tree](Marshaler& m) { delete_key_tree(m, false); },
        8
    });

    // ======================================================================
    // SHGetValue / SHSetValue / SHDeleteValue / SHDeleteEmptyKey (A and W).
    //
    // Learn (shlwapi.h) gives only the signatures and "returns a Win32 error
    // code"; every size and output rule below was measured on Windows 11 by
    // tools/oracle_probe/shlwapi_regsec_shell_probe.c (identical on x86 and x64):
    //  * SHGetValue reports REG_EXPAND_SZ as REG_SZ with the environment
    //    expanded; its size is max(raw, expanded) whether the call succeeds or
    //    not (get_expand*, get_grow*). The ANSI form takes the expanded size
    //    from ExpandEnvironmentStringsA, one byte larger, for the required
    //    size (geta_grow_sizeonly 14 for a 13-byte result).
    //  * A size query or a too-small buffer reports one character of slack
    //    for REG_SZ and two for REG_MULTI_SZ (get_sz_sizeonly 10 for an 8-byte
    //    value, get_multi_sizeonly 14 for 10); a successful read reports the
    //    exact size. An unterminated string is returned terminated
    //    (get_raw_noterm, get_raw_odd, get_raw_empty).
    //  * ERROR_MORE_DATA leaves pvData untouched; a missing value stores
    //    REG_NONE in *pdwType, a missing subkey leaves it alone; pvData without
    //    pcbData is ERROR_INVALID_PARAMETER.
    //  * SHDeleteEmptyKey refuses a key holding any value (the default one
    //    included) or subkey with ERROR_DIR_NOT_EMPTY; a NULL pszSubKey is
    //    ERROR_INVALID_PARAMETER, while "" deletes hkey's own key.
    // ======================================================================
    struct ShRegName {
        bool present{false};
        std::wstring text;
    };
    auto read_name = [](Marshaler& m, GuestAddr ptr, bool wide) -> ShRegName {
        ShRegName out;
        if (ptr == 0) return out;
        out.present = true;
        if (wide) {
            for (u16 ch : read_utf16_z(m, ptr, 32768)) {
                out.text.push_back(static_cast<wchar_t>(ch));
            }
            return out;
        }
        auto ansi = m.read_ansi_string(ptr, 32768);
        if (ansi.is_err() || ansi.value().empty()) return out;
        const std::string& bytes = ansi.value();
        std::vector<u16> units(bytes.size() * 2 + 1);
        const auto r = codepage::multi_byte_to_wide(
            codepage::ansi_code_page(), 0,
            reinterpret_cast<const u8*>(bytes.data()), bytes.size(),
            units.data(), units.size());
        for (u32 i = 0; r.ok() && i < r.count; ++i) {
            out.text.push_back(static_cast<wchar_t>(units[i]));
        }
        return out;
    };
    // Open hkey\pszSubKey (NULL or "" is hkey itself). Returns the key to use
    // and whether it must be closed.
    auto open_sub = [registry](u64 hkey, const ShRegName& sub, bool create,
                               u64& key, bool& opened) -> u32 {
        key = hkey;
        opened = false;
        if (!registry->is_valid_key(hkey)) return 6u;  // ERROR_INVALID_HANDLE
        if (sub.text.empty()) return 0u;
        u32 err = create ? registry->reg_create_key_ex_w(hkey, sub.text, &key, nullptr)
                         : registry->reg_open_key_ex_w(hkey, sub.text, 0, 0, &key);
        if (err != 0u) return err;
        opened = true;
        return 0u;
    };
    auto to_ansi = [](const u8* bytes, u32 size) {
        std::vector<u8> out;
        const u32 units = size / 2;
        if (units == 0) return out;
        std::vector<u16> wide(units);
        std::memcpy(wide.data(), bytes, static_cast<usize>(units) * 2);
        out.resize(static_cast<usize>(units) * 4);
        const auto r = codepage::wide_to_multi_byte(
            codepage::ansi_code_page(), 0, wide.data(), wide.size(),
            out.data(), out.size(), nullptr, false);
        out.resize(r.ok() ? r.count : 0);
        return out;
    };

    auto get_value = [registry, read_name, open_sub, to_ansi](Marshaler& m, bool wide) {
        constexpr u32 kRegSz = 1, kRegExpandSz = 2, kRegMultiSz = 7;
        constexpr u32 kRrfAny = 0x0000FFFFu, kRrfNoExpand = 0x10000000u;
        const u64 hkey = m.arg(0);
        const ShRegName sub = read_name(m, m.arg_ptr(1), wide);
        const ShRegName name = read_name(m, m.arg_ptr(2), wide);
        const GuestAddr type_ptr = m.arg_ptr(3);
        const GuestAddr data_ptr = m.arg_ptr(4);
        const GuestAddr size_ptr = m.arg_ptr(5);
        if (registry == nullptr) {
            m.set_return(6u);
            return;
        }
        u64 key = 0;
        bool opened = false;
        u32 err = open_sub(hkey, sub, false, key, opened);
        if (err != 0u) {
            m.set_return(err);
            return;
        }
        auto close = [&] { if (opened) (void)registry->reg_close_key(key); };

        // The stored bytes, unexpanded.
        u32 type = 0, raw_size = 0;
        err = registry->reg_get_value_w(key, L"", name.text, kRrfAny | kRrfNoExpand,
                                        &type, nullptr, &raw_size);
        if (err != 0u) {
            close();
            if (type_ptr != 0) (void)m.write_u32(type_ptr, 0u);  // REG_NONE
            m.set_return(err);
            return;
        }
        std::vector<u8> raw(raw_size);
        err = registry->reg_get_value_w(key, L"", name.text, kRrfAny | kRrfNoExpand,
                                        &type, raw.data(), &raw_size);
        std::vector<u8> expanded;
        if (err == 0u && type == kRegExpandSz) {
            u32 exp_size = 0, exp_type = 0;
            err = registry->reg_get_value_w(key, L"", name.text, kRrfAny, &exp_type,
                                            nullptr, &exp_size);
            if (err == 0u) {
                expanded.resize(exp_size);
                err = registry->reg_get_value_w(key, L"", name.text, kRrfAny, &exp_type,
                                                expanded.data(), &exp_size);
            }
        }
        close();
        if (err != 0u) {
            m.set_return(err);
            return;
        }

        // What a successful read stores (`content`, reported as `success`)
        // and what a size query or short buffer reports (`required`).
        std::vector<u8> content;
        std::vector<u8> underlay;  // REG_EXPAND_SZ: the stored form, read first
        u32 success = 0, required = 0, need = 0;
        const u32 unit = wide ? 2u : 1u;
        auto terminate = [unit](std::vector<u8> bytes) {
            // Drop a trailing partial unit; append a terminator unless the
            // last whole unit already is one (get_raw_odd / get_raw_noterm).
            bytes.resize(bytes.size() / unit * unit);
            bool terminated = bytes.size() >= unit;
            for (u32 i = 0; terminated && i < unit; ++i) {
                terminated = bytes[bytes.size() - unit + i] == 0;
            }
            if (!terminated) bytes.insert(bytes.end(), unit, 0);
            return bytes;
        };
        if (type == kRegExpandSz) {
            std::vector<u8> raw_form = wide ? raw : to_ansi(raw.data(), raw_size);
            std::vector<u8> exp_form = wide ? expanded
                                            : to_ansi(expanded.data(),
                                                      static_cast<u32>(expanded.size()));
            exp_form = terminate(std::move(exp_form));
            const u32 raw_len = static_cast<u32>(raw_form.size());
            const u32 exp_len = static_cast<u32>(exp_form.size());
            success = std::max(raw_len, exp_len);
            required = std::max(raw_len, wide ? exp_len : exp_len + 1u);
            // The ANSI expansion needs room for ExpandEnvironmentStringsA's
            // extra byte before it succeeds.
            need = wide ? success : required;
            content = std::move(exp_form);
            underlay = std::move(raw_form);
            type = kRegSz;
        } else if (type == kRegSz || type == kRegMultiSz) {
            std::vector<u8> form = wide ? raw : to_ansi(raw.data(), raw_size);
            const u32 stored = static_cast<u32>(form.size());
            form = terminate(std::move(form));
            success = static_cast<u32>(form.size());
            if (type == kRegMultiSz) {
                required = success + 2u * unit;  // get_multi*_sizeonly
            } else {
                // One unit of slack, plus the terminator a whole-unit
                // unterminated string lacks: 8 -> 10 terminated, 8 -> 12
                // unterminated, odd 7 -> 9, empty 0 -> 2 (get_*_sizeonly).
                const bool appended = success != stored;
                const bool whole_units = stored != 0 && stored % unit == 0;
                required = stored + unit + (appended && whole_units ? unit : 0u);
            }
            content = std::move(form);
            need = success;
        } else {
            content = std::move(raw);
            success = required = need = static_cast<u32>(content.size());
        }

        if (data_ptr != 0 && size_ptr == 0) {
            m.set_return(87u);  // ERROR_INVALID_PARAMETER (get_data_nocb)
            return;
        }
        if (type_ptr != 0) (void)m.write_u32(type_ptr, type);
        if (size_ptr == 0) {
            m.set_return(0u);
            return;
        }
        if (data_ptr == 0) {
            (void)m.write_u32(size_ptr, required);
            m.set_return(0u);
            return;
        }
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        // A REG_EXPAND_SZ whose stored form fits but whose expansion does
        // not: Windows leaves a partial expansion in pvData; Vitreo leaves the
        // buffer untouched (recorded in .docs/contracts/shlwapi.md).
        if (capacity < need) {
            (void)m.write_u32(size_ptr, required);
            m.set_return(234u);  // ERROR_MORE_DATA
            return;
        }
        // The expansion lands over the stored string, so bytes past the
        // expanded terminator still hold the unexpanded tail (get_expand).
        for (const std::vector<u8>* bytes : {&underlay, &content}) {
            if (!bytes->empty() &&
                m.memory().write_bytes(data_ptr,
                                       reinterpret_cast<const Byte*>(bytes->data()),
                                       bytes->size()).is_err()) {
                m.set_return(998u);  // ERROR_NOACCESS
                return;
            }
        }
        (void)m.write_u32(size_ptr, success);
        m.set_return(0u);
    };
    exports.entries.push_back(ExportEntry{
        "SHGetValueW", 0, [get_value](Marshaler& m) { get_value(m, true); }, 24});
    exports.entries.push_back(ExportEntry{
        "SHGetValueA", 0, [get_value](Marshaler& m) { get_value(m, false); }, 24});

    // LSTATUS SHSetValue(HKEY, LPCTSTR pszSubKey, LPCTSTR pszValue, DWORD
    // dwType, LPCVOID pvData, DWORD cbData): RegCreateKeyEx on the subkey,
    // then RegSetValueEx. Any dwType is stored despite Learn's "must be
    // REG_SZ" (set_badtype, set_dword); NULL pvData with a nonzero cbData is
    // RegSetValueEx's ERROR_NOACCESS (set_null_data_cb).
    auto set_value = [registry, read_name, open_sub](Marshaler& m, bool wide) {
        const u64 hkey = m.arg(0);
        const ShRegName sub = read_name(m, m.arg_ptr(1), wide);
        const ShRegName name = read_name(m, m.arg_ptr(2), wide);
        const u32 type = m.arg32(3);
        const GuestAddr data_ptr = m.arg_ptr(4);
        const u32 size = m.arg32(5);
        if (registry == nullptr) {
            m.set_return(6u);
            return;
        }
        std::vector<u8> data(size);
        if (size != 0) {
            if (data_ptr == 0 ||
                m.memory().read_bytes(data_ptr, reinterpret_cast<Byte*>(data.data()),
                                      size).is_err()) {
                m.set_return(998u);  // ERROR_NOACCESS
                return;
            }
        }
        u64 key = 0;
        bool opened = false;
        u32 err = open_sub(hkey, sub, true, key, opened);
        if (err != 0u) {
            m.set_return(err);
            return;
        }
        if (wide) {
            err = registry->reg_set_value_ex_w(key, name.text, type, data.data(), size);
        } else {
            // RegSetValueExA's own conversion of string data; the name is
            // already wide (CP_ACP).
            constexpr u32 kRegSz = 1, kRegExpandSz = 2, kRegMultiSz = 7;
            if (type == kRegSz || type == kRegExpandSz || type == kRegMultiSz) {
                std::vector<u16> units(static_cast<usize>(size) + 1);
                const auto r = codepage::multi_byte_to_wide(
                    codepage::ansi_code_page(), 0, data.data(), data.size(),
                    units.data(), units.size());
                std::vector<u8> bytes(static_cast<usize>(r.ok() ? r.count : 0) * 2);
                if (!bytes.empty()) std::memcpy(bytes.data(), units.data(), bytes.size());
                err = registry->reg_set_value_ex_w(key, name.text, type, bytes.data(),
                                                   static_cast<u32>(bytes.size()));
            } else {
                err = registry->reg_set_value_ex_w(key, name.text, type, data.data(), size);
            }
        }
        if (opened) (void)registry->reg_close_key(key);
        m.set_return(err);
    };
    exports.entries.push_back(ExportEntry{
        "SHSetValueW", 0, [set_value](Marshaler& m) { set_value(m, true); }, 24});
    exports.entries.push_back(ExportEntry{
        "SHSetValueA", 0, [set_value](Marshaler& m) { set_value(m, false); }, 24});

    // LSTATUS SHDeleteValue(HKEY, LPCTSTR pszSubKey, LPCTSTR pszValue). A
    // NULL pszValue names the default value (delval_default).
    auto delete_value = [registry, read_name, open_sub](Marshaler& m, bool wide) {
        const u64 hkey = m.arg(0);
        const ShRegName sub = read_name(m, m.arg_ptr(1), wide);
        const ShRegName name = read_name(m, m.arg_ptr(2), wide);
        if (registry == nullptr) {
            m.set_return(6u);
            return;
        }
        u64 key = 0;
        bool opened = false;
        u32 err = open_sub(hkey, sub, false, key, opened);
        if (err == 0u) {
            err = registry->reg_delete_value_w(key, name.text);
            if (opened) (void)registry->reg_close_key(key);
        }
        m.set_return(err);
    };
    exports.entries.push_back(ExportEntry{
        "SHDeleteValueW", 0, [delete_value](Marshaler& m) { delete_value(m, true); }, 12});
    exports.entries.push_back(ExportEntry{
        "SHDeleteValueA", 0, [delete_value](Marshaler& m) { delete_value(m, false); }, 12});

    // LSTATUS SHDeleteEmptyKey(HKEY, LPCTSTR pszSubKey): delete the key only
    // when it holds no value and no subkey.
    auto delete_empty_key = [registry, read_name](Marshaler& m, bool wide) {
        const u64 hkey = m.arg(0);
        const ShRegName sub = read_name(m, m.arg_ptr(1), wide);
        if (registry == nullptr) {
            m.set_return(6u);
            return;
        }
        if (!registry->is_valid_key(hkey)) {
            m.set_return(6u);
            return;
        }
        if (!sub.present) {
            m.set_return(87u);  // delempty_null_sub
            return;
        }
        u64 key = hkey;
        bool opened = false;
        if (!sub.text.empty()) {
            const u32 err = registry->reg_open_key_ex_w(hkey, sub.text, 0, 0, &key);
            if (err != 0u) {
                m.set_return(err);
                return;
            }
            opened = true;
        }
        u32 subkeys = 0, values = 0, max_sub = 0, max_name = 0, max_data = 0;
        u32 err = registry->reg_query_info_key_a(key, &subkeys, &max_sub, &values,
                                                 &max_name, &max_data);
        if (opened) (void)registry->reg_close_key(key);
        if (err == 0u && (subkeys != 0 || values != 0)) err = 145u;  // ERROR_DIR_NOT_EMPTY
        if (err == 0u) err = registry->reg_delete_key(hkey, sub.text);
        m.set_return(err);
    };
    exports.entries.push_back(ExportEntry{
        "SHDeleteEmptyKeyW", 0,
        [delete_empty_key](Marshaler& m) { delete_empty_key(m, true); }, 8});
    exports.entries.push_back(ExportEntry{
        "SHDeleteEmptyKeyA", 0,
        [delete_empty_key](Marshaler& m) { delete_empty_key(m, false); }, 8});


    return exports;
}

} // namespace vitreo::winapi
