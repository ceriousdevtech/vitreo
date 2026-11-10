// SPDX-License-Identifier: LGPL-2.1-only
/// @file crt_nls_helpers.cpp
/// @brief msvcrt's __crtCompareStringA / __crtLCMapStringA.
///
/// These are the CRT's internal ANSI wrappers over CompareStringW and
/// LCMapStringW that the C++ runtime (MSVCP60) imports from msvcrt.dll for
/// std::collate and std::ctype. They are undocumented, so their behaviour is
/// measured (tools/oracle_probe/crt_legacy_exports_probe{32,64}.expected,
/// "cmp" and "map" rows; x86 and x64 agree):
///   - a positive count is clipped at the first NUL; LCMap then keeps that
///     NUL as part of the source ("ab\0cd",5 maps to "AB\0", 3);
///   - Compare: two empty strings compare equal; otherwise both sides are
///     converted with MB_PRECOMPOSED | MB_ERR_INVALID_CHARS from the code
///     page (0 = the locale's, the "C" locale's being CP_ACP) and compared
///     with CompareStringW;
///   - LCMap: the source is converted with MB_PRECOMPOSED (plus
///     MB_ERR_INVALID_CHARS when bError is set), mapped with LCMapStringW,
///     and a string result converted back to the same code page; a sort key
///     is returned as bytes; a zero count fails with ERROR_INVALID_PARAMETER;
///     a short destination fails with ERROR_INSUFFICIENT_BUFFER after the
///     bytes that fit;
///   - an LCID Windows does not know fails with ERROR_INVALID_PARAMETER;
///     LCID 0 means the user default.
///
/// Signatures: the VC6 runtime libraries call
///   int __crtCompareStringA(LCID, DWORD, LPCSTR, int, LPCSTR, int, int cp)
///   int __crtLCMapStringA(LCID, DWORD, LPCSTR, int, LPSTR, int, int cp, BOOL)
/// (MSVCP60 6.0's call sites push exactly these). Vista's msvcrt.dll put a
/// _locale_t in front of both (its export forwards arguments 2..n to the same
/// worker); only the operating system's own rebuilt msvcp60.dll imports that
/// form, and Vitreo does not ship one, so the x86 export keeps the VC6
/// signature every importable caller uses. x64 has no VC6 runtime and keeps
/// the measured _locale_t form.

#include "winapi/msvcrt/text/crt_nls_helpers.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/lcmap.hpp"
#include "winapi/kernel32/locale/locale_string.hpp"

#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 MB_PRECOMPOSED = 0x00000001;
constexpr u32 MB_ERR_INVALID_CHARS = 0x00000008;
constexpr u32 LCMAP_SORTKEY = 0x00000400;
constexpr u32 LCID_SUPPORTED = 0x00000002;
constexpr u32 ERROR_INVALID_PARAMETER_VALUE = 87;
constexpr u32 ERROR_INSUFFICIENT_BUFFER_VALUE = 122;

bool known_lcid(const LocaleStringManager& locales, u32 lcid) {
    // 0 (neutral -> user default), LOCALE_INVARIANT, LOCALE_USER_DEFAULT and
    // LOCALE_SYSTEM_DEFAULT, or a locale Windows supports.
    if (lcid == 0 || lcid == 0x007F || lcid == 0x0400 || lcid == 0x0800) return true;
    return locales.is_valid_locale(lcid, LCID_SUPPORTED) != 0;
}

u32 code_page_or_locale(i32 code_page) {
    return code_page == 0 ? codepage::kCpAcp : static_cast<u32>(code_page);
}

/// The source bytes for a count: -1 reads to the terminator (excluded);
/// a positive count stops at the first NUL (strncnt). @p hit_nul reports
/// whether the count was clipped by a NUL.
bool read_counted(Marshaler& m, GuestAddr s, i32 count, std::vector<u8>& out,
                  bool& hit_nul) {
    out.clear();
    hit_nul = false;
    for (i64 i = 0; count < 0 || i < count; ++i) {
        const auto c = m.read_u8(s + static_cast<GuestAddr>(i));
        if (c.is_err()) {
            (void)m.raise_access_violation(false, s + static_cast<GuestAddr>(i));
            return false;
        }
        if (c.value() == 0) {
            hit_nul = count >= 0;
            break;
        }
        out.push_back(c.value());
    }
    return true;
}

bool widen(u32 code_page, u32 flags, const std::vector<u8>& bytes,
           std::vector<u16>& wide) {
    wide.assign(bytes.size() * 2 + 2, 0);
    const auto r = codepage::multi_byte_to_wide(code_page, flags, bytes.data(),
                                                bytes.size(), wide.data(), wide.size());
    if (!r.ok()) {
        set_last_error(r.error);
        return false;
    }
    wide.resize(r.count);
    return true;
}

/// Arguments after the optional leading _locale_t.
struct Args {
    Marshaler& m;
    usize first;
    u64 operator[](usize i) const { return m.arg(first + i); }
    GuestAddr ptr(usize i) const { return m.arg_ptr(first + i); }
};

Args args_of(Marshaler& m) { return Args{m, m.cpu_is_64bit() ? 1u : 0u}; }

void compare_string(Marshaler& m, const LocaleStringManager& locales) {
    const Args a = args_of(m);
    const auto lcid = static_cast<u32>(a[0]);
    const auto flags = static_cast<u32>(a[1]);
    const GuestAddr s1 = a.ptr(2);
    const auto n1 = static_cast<i32>(a[3]);
    const GuestAddr s2 = a.ptr(4);
    const auto n2 = static_cast<i32>(a[5]);
    const auto cp = static_cast<i32>(a[6]);
    if (!known_lcid(locales, lcid)) {
        set_last_error(ERROR_INVALID_PARAMETER_VALUE);
        m.set_return(0);
        return;
    }
    std::vector<u8> b1, b2;
    bool nul1 = false, nul2 = false;
    if (!read_counted(m, s1, n1, b1, nul1) || !read_counted(m, s2, n2, b2, nul2)) return;
    if (b1.empty() || b2.empty()) {
        // Empty strings never reach MultiByteToWideChar: equal, or the empty
        // one sorts first.
        m.set_return(b1.empty() && b2.empty() ? 2u : (b1.empty() ? 1u : 3u));
        return;
    }
    const u32 code_page = code_page_or_locale(cp);
    std::vector<u16> w1, w2;
    if (!widen(code_page, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, b1, w1) ||
        !widen(code_page, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, b2, w2)) {
        m.set_return(0);
        return;
    }
    m.set_return(static_cast<u32>(LocaleStringManager::compare_units(w1, w2, flags)));
}

void map_string(Marshaler& m, const LocaleStringManager& locales) {
    const Args a = args_of(m);
    const auto lcid = static_cast<u32>(a[0]);
    const auto flags = static_cast<u32>(a[1]);
    const GuestAddr src = a.ptr(2);
    const auto cch_src = static_cast<i32>(a[3]);
    const GuestAddr dst = a.ptr(4);
    const auto cch_dest = static_cast<i32>(a[5]);
    const auto cp = static_cast<i32>(a[6]);
    const bool error_on_invalid = static_cast<u32>(a[7]) != 0;
    if (!known_lcid(locales, lcid) || cch_src == 0 || cch_dest < 0 ||
        (cch_dest > 0 && dst == 0)) {
        set_last_error(ERROR_INVALID_PARAMETER_VALUE);
        m.set_return(0);
        return;
    }
    std::vector<u8> bytes;
    bool clipped = false;
    if (!read_counted(m, src, cch_src, bytes, clipped)) return;
    const u32 code_page = code_page_or_locale(cp);
    std::vector<u16> wide;
    if (!bytes.empty() &&
        !widen(code_page,
               MB_PRECOMPOSED | (error_on_invalid ? MB_ERR_INVALID_CHARS : 0u),
               bytes, wide)) {
        m.set_return(0);
        return;
    }
    // A count clipped by a NUL keeps that NUL as the last source character.
    if (clipped) wide.push_back(0);
    const bool minus_one = cch_src < 0;
    std::vector<u16> mapped;
    std::vector<u8> key;
    const bool sort_key = (flags & LCMAP_SORTKEY) != 0;
    const i32 produced = lcmap_units(flags, wide.data(), wide.size(), minus_one,
                                     sort_key ? nullptr : &mapped,
                                     sort_key ? &key : nullptr);
    if (produced <= 0) {
        m.set_return(0);
        return;
    }
    if (sort_key) {
        if (cch_dest == 0) {
            m.set_return(static_cast<u32>(key.size()));
            return;
        }
        if (static_cast<usize>(cch_dest) < key.size()) {
            set_last_error(ERROR_INSUFFICIENT_BUFFER_VALUE);
            m.set_return(0);
            return;
        }
        for (usize i = 0; i < key.size(); ++i) (void)m.write_u8(dst + i, key[i]);
        m.set_return(static_cast<u32>(key.size()));
        return;
    }
    std::vector<u8> out(cch_dest == 0 ? mapped.size() * 4 + 4
                                      : static_cast<usize>(cch_dest));
    const auto r = codepage::wide_to_multi_byte(code_page, 0, mapped.data(),
                                                mapped.size(), out.data(),
                                                cch_dest == 0 ? 0 : out.size(),
                                                nullptr, false);
    if (cch_dest != 0) {
        const usize stored = r.ok() ? r.count : r.written;
        for (usize i = 0; i < stored; ++i) (void)m.write_u8(dst + i, out[i]);
    }
    if (!r.ok()) {
        set_last_error(r.error);
        m.set_return(0);
        return;
    }
    m.set_return(r.count);
}

} // namespace

DllExports create_crt_nls_helper_thunks(const LocaleStringManager& locales) {
    DllExports exports;
    exports.entries.push_back({"__crtCompareStringA", 0,
        [&locales](Marshaler& m) { compare_string(m, locales); }});
    exports.entries.push_back({"__crtLCMapStringA", 0,
        [&locales](Marshaler& m) { map_string(m, locales); }});
    return exports;
}

} // namespace vitreo::winapi
