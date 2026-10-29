// SPDX-License-Identifier: LGPL-2.1-only

/// @file locale_string_registration.cpp
/// @brief Export table for kernel32's locale and string-comparison surface.
///
/// Composition only: maps export names to LocaleStringManager handlers and
/// records each one's stdcall byte count.
///
/// Split out of locale_string.cpp, which was 1,824 lines and two-thirds this
/// table.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export, each
/// naming a handler and its stdcall byte count. Plan section 4.1 exempts these
/// because length here tracks how many locale exports kernel32 implements, not
/// how much logic one file carries.

#include "winapi/kernel32/locale/locale_string.hpp"
#include "winapi/kernel32/locale/locale_string_internal.hpp"
#include "winapi/kernel32/locale/lcmap.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/srw_word.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

// ---------------------------------------------------------------------------
// GetNumberFormatW / GetNumberFormatA
// ---------------------------------------------------------------------------
//
// int GetNumberFormat{W,A}(LCID Locale, DWORD dwFlags, LPCTSTR lpValue,
//                          const NUMBERFMT* lpFormat, LPTSTR lpNumberStr,
//                          int cchNumber)
// https://learn.microsoft.com/windows/win32/api/winnls/nf-winnls-getnumberformata
// Measured: tools/oracle_probe/getnumberformat_probe{32,64}.expected.
// The A form is the W form around CP_ACP: lpValue and the NUMBERFMTA
// separators are converted to UTF-16 first, the result back to CP_ACP after,
// and cchNumber / the return value count bytes of that conversion
// (gnfa-a-locale-fr-FR-size-query: 14).

constexpr u32 kLocaleNoUserOverride = 0x80000000u;
constexpr u32 kLocaleUseCpAcp = 0x40000000u;
/// lpValue is read to its NUL however long it is (Windows formats the whole
/// number); only unreadable memory ends it.
constexpr usize kWholeString = ~usize{0};

/// A NUL-terminated guest string as UTF-16 units: wide, or CP_ACP bytes
/// converted. Nullopt when unreadable, or when no NUL comes within @p limit
/// units (a separator longer than that is invalid anyway).
std::optional<std::u16string> read_tstring(Marshaler& m, GuestAddr p, bool wide, usize limit) {
    std::u16string out;
    if (wide) {
        for (usize i = 0; i < limit; ++i) {
            auto u = m.memory().read_u16(p + i * 2);
            if (u.is_err()) return std::nullopt;
            if (u.value() == 0) return out;
            out.push_back(static_cast<char16_t>(u.value()));
        }
        return std::nullopt;
    }
    std::vector<u8> bytes;
    for (usize i = 0;; ++i) {
        if (i == limit) return std::nullopt;
        auto b = m.memory().read_u8(p + i);
        if (b.is_err()) return std::nullopt;
        if (b.value() == 0) break;
        bytes.push_back(static_cast<u8>(b.value()));
    }
    if (bytes.empty()) return out;
    std::vector<u16> units(bytes.size() * 2 + 2);
    const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, bytes.data(), bytes.size(),
                                                units.data(), units.size());
    if (!r.ok()) return std::nullopt;
    out.assign(units.begin(), units.begin() + r.count);
    return out;
}

/// NUMBERFMT{W,A}: three UINTs, two string pointers, a UINT. x86: the
/// pointers at +12 / +16, NegativeOrder at +20; x64: +16 / +24, +32.
bool read_number_format(Marshaler& m, GuestAddr p, bool wide, NumberFormatSpec& out) {
    const bool x64 = m.cpu_is_64bit();
    auto nd = m.memory().read_u32(p);
    auto lz = m.memory().read_u32(p + 4);
    auto gr = m.memory().read_u32(p + 8);
    auto dec = m.read_ptr(p + (x64 ? 16 : 12));
    auto th = m.read_ptr(p + (x64 ? 24 : 16));
    auto no = m.memory().read_u32(p + (x64 ? 32 : 20));
    if (nd.is_err() || lz.is_err() || gr.is_err() || dec.is_err() || th.is_err() || no.is_err()) return false;
    if (dec.value() == 0 || th.value() == 0) return false;   // gnfw-fmt-null-decimal / -null-thousand
    // A separator longer than three characters is invalid, so a few more
    // units than that are enough to decide.
    auto d = read_tstring(m, static_cast<GuestAddr>(dec.value()), wide, 16);
    auto t = read_tstring(m, static_cast<GuestAddr>(th.value()), wide, 16);
    if (!d || !t) return false;
    out.num_digits = nd.value();
    out.leading_zero = lz.value();
    out.grouping = gr.value();
    out.decimal = *d;
    out.thousand = *t;
    out.negative_order = no.value();
    return true;
}

void fail_number_format(Marshaler& m, u32 error) {
    set_last_error(error);
    m.set_return(0);
}

/// dwFlags of every GetNumberFormat form: with lpFormat only LOCALE_USE_CP_ACP
/// (gnfw-fmt-flags-nouseroverride, -fmt-flags-1: ERROR_INVALID_FLAGS;
/// gnfex-fmt-flags-use-cp-acp, gnfex-crosscheck-w-fmt-flags-use-cp-acp,
/// gnfa-a-fmt-flags-use-cp-acp: accepted), without it LOCALE_NOUSEROVERRIDE and LOCALE_USE_CP_ACP
/// (gnfw-flags-use-cp-acp, gnfex-flags-use-cp-acp-nouseroverride; gnfw-flags-1,
/// gnfex-flags-0x20000000: ERROR_INVALID_FLAGS).
bool number_format_flags_valid(u32 flags, bool has_format) {
    const u32 allowed = has_format ? kLocaleUseCpAcp : (kLocaleNoUserOverride | kLocaleUseCpAcp);
    return (flags & ~allowed) == 0;
}

/// The checks and formatting every GetNumberFormat form shares once its
/// locale and flags are accepted: @p spec holds the locale's defaults, which
/// a NUMBERFMT at lpFormat replaces. Arguments 2-5 are lpValue, lpFormat,
/// lpNumberStr and cchNumber in each form.
void format_number_to_guest(Marshaler& m, bool wide, NumberFormatSpec spec) {
    const GuestAddr value_p = m.arg_ptr(2);
    const GuestAddr format_p = m.arg_ptr(3);
    const GuestAddr buf = m.arg_ptr(4);
    const i32 cch = static_cast<i32>(m.arg32(5));
    auto fail = [&m](u32 error) { fail_number_format(m, error); };
    if (value_p == 0 || cch < 0 || (buf == 0 && cch > 0)) {   // -null-value, -negative-cch, -null-buffer-cch5
        fail(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (format_p != 0 && (!read_number_format(m, format_p, wide, spec) || !number_format_spec_valid(spec))) {
        fail(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    auto value = read_tstring(m, value_p, wide, kWholeString);
    std::u16string formatted;
    if (!value || !format_number_string(*value, spec, formatted)) {
        fail(error_codes::ERROR_INVALID_PARAMETER);       // gnfw-us-null-fmt value="abc", -size-query-bad-value
        return;
    }

    if (wide) {
        const i32 need = static_cast<i32>(formatted.size()) + 1;
        if (cch == 0) {                                      // gnfw-size-query
            m.set_return(static_cast<u32>(need));
            return;
        }
        if (cch < need) {                                    // gnfw-short-buffer-13: nothing written
            fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
            return;
        }
        for (usize i = 0; i <= formatted.size(); ++i) {
            const u16 unit = i < formatted.size() ? static_cast<u16>(formatted[i]) : 0;
            (void)m.memory().write_u16(buf + i * 2, unit);
        }
        m.set_return(static_cast<u32>(need));
        return;
    }

    std::vector<u16> units(formatted.begin(), formatted.end());
    units.push_back(0);
    const auto query = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, units.data(), units.size(),
                                                    nullptr, 0, nullptr, false);
    if (!query.ok()) {
        fail(query.error);
        return;
    }
    const i32 need = static_cast<i32>(query.count);
    if (cch == 0) {                                          // gnfa-a-size-query
        m.set_return(static_cast<u32>(need));
        return;
    }
    std::vector<u8> bytes(query.count);
    const auto conv = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, units.data(), units.size(),
                                                   bytes.data(), bytes.size(), nullptr, false);
    if (!conv.ok()) {
        fail(conv.error);
        return;
    }
    // gnfa-a-short-buffer-13: the first cchNumber bytes are written, without
    // a NUL, and the call fails.
    const usize n = cch < need ? static_cast<usize>(cch) : static_cast<usize>(need);
    (void)m.memory().write_bytes(buf, reinterpret_cast<const Byte*>(bytes.data()), n);
    if (cch < need) {
        fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    m.set_return(static_cast<u32>(need));
}

void get_number_format(LocaleStringManager& mgr, Marshaler& m, bool wide) {
    const u32 locale = m.arg32(0);
    const u32 flags = m.arg32(1);
    // Flags first (gnfw-flags-1-bad-value: ERROR_INVALID_FLAGS), then the
    // locale (gnfw-locale-bad-0x1234: ERROR_INVALID_PARAMETER).
    if (!number_format_flags_valid(flags, m.arg_ptr(3) != 0)) {
        fail_number_format(m, error_codes::ERROR_INVALID_FLAGS);
        return;
    }
    NumberFormatSpec spec;
    if (!mgr.number_format_defaults(locale, spec)) {
        fail_number_format(m, error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    format_number_to_guest(m, wide, std::move(spec));
}

// ---------------------------------------------------------------------------
// GetNumberFormatEx
// ---------------------------------------------------------------------------
//
// int GetNumberFormatEx(LPCWSTR lpLocaleName, DWORD dwFlags, LPCWSTR lpValue,
//                       const NUMBERFMTW* lpFormat, LPWSTR lpNumberStr,
//                       int cchNumber)
// https://learn.microsoft.com/windows/win32/api/winnls/nf-winnls-getnumberformatex
// winnls.h: six parameters (no lpReserved), stdcall_bytes 24 on x86; on x64
// lpLocaleName/dwFlags/lpValue/lpFormat in RCX/RDX/R8/R9, lpNumberStr and
// cchNumber on the stack. It is the W form keyed by name: the same value
// grammar, NUMBERFMTW checks, size query and buffer rules (gnfex-* in
// tools/oracle_probe/getnumberformat_probe{32,64}.expected, identical for both
// bitnesses). Unlike the W form the name is checked before the flags
// (gnfex-invalid-name-flags-1: ERROR_INVALID_PARAMETER) and before every other
// argument (-invalid-name-size-query, -short-buffer, -fmt).

/// LOCALE_NAME_MAX_LENGTH (winnls.h), the NUL included.
constexpr usize kLocaleNameMaxLength = 85;

/// Says why an *Ex export failed a locale name with ERROR_INVALID_PARAMETER.
/// A malformed name fails on Windows too, so it gets only a trace line. A
/// well-formed one is answered on Windows (gnfex-name-xx-YY, -en-IN,
/// gnfex-grammar-es-419), but has no profile in Vitreo. That divergence is
/// printed once per name, up to kMaxReported distinct names.
void report_unanswered_locale_name(const char* api, const std::optional<std::u16string>& name) {
    std::string printable;
    if (name) {
        for (char16_t c : *name) printable.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    } else {
        printable = "<unreadable or longer than LOCALE_NAME_MAX_LENGTH>";
    }
    if (!name || !locale_name_well_formed(*name)) {
        VITREO_TRACE("[locale] %s: malformed locale name \"%s\" -> ERROR_INVALID_PARAMETER\n", api,
                     printable.c_str());
        return;
    }
    constexpr usize kMaxReported = 256;
    static std::mutex mu;
    static std::set<std::u16string> reported;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (reported.size() >= kMaxReported || !reported.insert(*name).second) return;
    }
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED kernel32!%s: locale \"%s\" is well formed but outside Vitreo's locale "
                 "profile table (not malformed); failing with ERROR_INVALID_PARAMETER where Windows answers it\n",
                 api, printable.c_str());
}

void get_number_format_ex(LocaleStringManager& mgr, Marshaler& m) {
    const GuestAddr name_p = m.arg_ptr(0);
    const u32 flags = m.arg32(1);
    // LOCALE_NAME_USER_DEFAULT is NULL (gnfex-name-null: en-US's format).
    std::optional<u32> lcid = 0x0400u;
    std::optional<std::u16string> name;
    if (name_p != 0) {
        // No locale name is longer (gnfex-grammar-len85-subtags).
        name = read_tstring(m, name_p, /*wide=*/true, kLocaleNameMaxLength);
        lcid = name ? mgr.locale_name_profile_lcid(*name) : std::nullopt;
    }
    NumberFormatSpec spec;
    if (!lcid || !mgr.number_format_defaults(*lcid, spec)) {        // gnfex-name-not-a-locale
        report_unanswered_locale_name("GetNumberFormatEx", name);
        fail_number_format(m, error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (!number_format_flags_valid(flags, m.arg_ptr(3) != 0)) {     // gnfex-flags-1, -fmt-flags-nouseroverride
        fail_number_format(m, error_codes::ERROR_INVALID_FLAGS);
        return;
    }
    format_number_to_guest(m, /*wide=*/true, std::move(spec));
}

}  // namespace

// ---------------------------------------------------------------------------
// Thunk factory
// ---------------------------------------------------------------------------

DllExports create_locale_string_thunks(LocaleStringManager& mgr) {
    DllExports exports;
    exports.entries.reserve(19);

    // GetConsoleOutputCP (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetConsoleOutputCP", 0,
        [&mgr](Marshaler& m) {
            // process_contract_probe.expected: no console → 0 err=6
            if (!mgr.console_is_attached()) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0u);
                return;
            }
            m.set_return(static_cast<u64>(mgr.get_console_output_cp()));
        },
        0
    });

    // BOOL SetConsoleOutputCP(UINT wCodePageID)
    //
    // Windows rejects a code page it cannot install and leaves the current one
    // in place; a console app that asked for UTF-8 and got a silent success
    // while the console stayed at OEM-US would mis-encode every byte it wrote.
    const auto set_code_page = [](Marshaler& m, u32& out) {
        const u32 cp = static_cast<u32>(m.arg(0));
        // The code pages Vitreo can actually translate through.
        const bool supported = cp == 437u || cp == 850u || cp == 1252u ||
                               cp == 65000u || cp == 65001u || cp == 1200u ||
                               cp == 1201u;
        if (!supported) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        out = cp;
        m.set_return(1);
    };
    exports.entries.push_back(ExportEntry{
        "SetConsoleOutputCP", 0,
        [&mgr, set_code_page](Marshaler& m) {
            u32 cp = mgr.get_console_output_cp();
            set_code_page(m, cp);
            mgr.set_console_output_cp(cp);
        },
        4
    });
    // GetThreadLocale (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetThreadLocale", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_thread_locale()));
        },
        0
    });

    // BOOL SetThreadLocale(LCID Locale) — 1 arg, stdcall_bytes=4.
    // winnls.h: WINBOOL WINAPI. A missing-export stub pops nothing, so the
    // LCID (often LOCALE_USER_DEFAULT, 0x0400) is consumed as the return
    // address on x86.
    exports.entries.push_back(ExportEntry{
        "SetThreadLocale", 0,
        [&mgr](Marshaler& m) {
            const u32 locale = static_cast<u32>(m.arg(0));
            if (!mgr.set_thread_locale(locale)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        },
        4
    });

    // lstrlenW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "lstrlenW", 0,
        [&mgr](Marshaler& m) {
            auto ptr = m.arg_ptr(0);
            auto len = mgr.lstrlen_w(m, ptr);
            m.set_return(static_cast<u64>(len));
        },
        4
    });

    // CompareStringA (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "CompareStringA", 0,
        [&mgr](Marshaler& m) {
            auto str1   = m.arg_ptr(2);
            auto str2   = m.arg_ptr(4);
            if (str1 == 0 || str2 == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            auto locale = static_cast<u32>(m.arg(0));
            auto flags  = static_cast<u32>(m.arg(1));
            auto len1   = static_cast<i32>(m.arg(3));
            auto len2   = static_cast<i32>(m.arg(5));
            auto result = mgr.compare_string_a(m, locale, flags, str1, len1, str2, len2);
            m.set_return(static_cast<u64>(result));
        },
        24
    });

    // CompareStringW (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "CompareStringW", 0,
        [&mgr](Marshaler& m) {
            auto str1   = m.arg_ptr(2);
            auto str2   = m.arg_ptr(4);
            if (str1 == 0 || str2 == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            auto locale = static_cast<u32>(m.arg(0));
            auto flags  = static_cast<u32>(m.arg(1));
            auto len1   = static_cast<i32>(m.arg(3));
            auto len2   = static_cast<i32>(m.arg(5));
            auto result = mgr.compare_string_w(m, locale, flags, str1, len1, str2, len2);
            m.set_return(static_cast<u64>(result));
        },
        24
    });

    // GetTimeFormatW (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "GetTimeFormatW", 0,
        [&mgr](Marshaler& m) {
            auto locale   = static_cast<u32>(m.arg(0));
            auto flags    = static_cast<u32>(m.arg(1));
            auto time_ptr = m.arg_ptr(2);
            auto fmt_ptr  = m.arg_ptr(3);
            auto buf      = m.arg_ptr(4);
            auto buf_size = static_cast<i32>(m.arg(5));
            auto result   = mgr.get_time_format(m, /*wide=*/true, locale, flags,
                                                time_ptr, fmt_ptr,
                                                buf, buf_size);
            m.set_return(static_cast<u64>(result));
        },
        24
    });

    // GetTimeFormatA (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "GetTimeFormatA", 0,
        [&mgr](Marshaler& m) {
            auto locale   = static_cast<u32>(m.arg(0));
            auto flags    = static_cast<u32>(m.arg(1));
            auto time_ptr = m.arg_ptr(2);
            auto fmt_ptr  = m.arg_ptr(3);
            auto buf      = m.arg_ptr(4);
            auto buf_size = static_cast<i32>(m.arg(5));
            auto result   = mgr.get_time_format(m, /*wide=*/false, locale, flags,
                                                time_ptr, fmt_ptr,
                                                buf, buf_size);
            m.set_return(static_cast<u64>(result));
        },
        24
    });

    // GetUserDefaultUILanguage (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetUserDefaultUILanguage", 0,
        [&mgr](Marshaler& m) {
            auto lang = mgr.get_user_default_ui_language();
            VITREO_TRACE( "[locale] GetUserDefaultUILanguage -> 0x%x\n", lang);
            m.set_return(static_cast<u64>(lang));
        },
        0
    });

    // GetUserDefaultLangID (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetUserDefaultLangID", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_user_default_lang_id()));
        },
        0
    });

    // GetUserDefaultLCID (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetUserDefaultLCID", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_user_default_lcid()));
        },
        0
    });

    // GetSystemDefaultLangID (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetSystemDefaultLangID", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_system_default_lang_id()));
        },
        0
    });

    // GetSystemDefaultUILanguage (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetSystemDefaultUILanguage", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_system_default_ui_language()));
        },
        0
    });

    // GetLocaleInfoW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetLocaleInfoW", 0,
        [&mgr](Marshaler& m) {
            auto locale  = static_cast<u32>(m.arg(0));
            auto lc_type = static_cast<u32>(m.arg(1));
            auto buf     = m.arg_ptr(2);
            auto cch_buf = static_cast<i32>(m.arg(3));
            // Windows: LOCALE_NEUTRAL (0) succeeds as user default; NULL
            // buffer with cchData>0 or negative cchData → ERROR_INVALID_PARAMETER.
            if (cch_buf < 0 || (buf == 0 && cch_buf > 0)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto result  = mgr.get_locale_info_w(m, locale, lc_type, buf, cch_buf);
            m.set_return(static_cast<u64>(result));
        },
        16
    });

    // EnumSystemLocalesA (2 args, stdcall_bytes=8)
    // BOOL EnumSystemLocalesA(LOCALE_ENUMPROCA lpLocaleEnumProc, DWORD dwFlags);
    exports.entries.push_back(ExportEntry{
        "EnumSystemLocalesA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr callback = m.arg_ptr(0);
            u32 flags = static_cast<u32>(m.arg(1));
            VITREO_TRACE("[locale] EnumSystemLocalesA(callback=0x%x, flags=0x%x)\n",
                         static_cast<u32>(callback), flags);
            m.set_return(mgr.enum_system_locales(m, /*wide=*/false, callback, flags));
        },
        8
    });

    // EnumSystemLocalesW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "EnumSystemLocalesW", 0,
        [&mgr](Marshaler& m) {
            GuestAddr callback = m.arg_ptr(0);
            u32 flags = static_cast<u32>(m.arg(1));
            VITREO_TRACE("[locale] EnumSystemLocalesW(callback=0x%x, flags=0x%x)\n",
                         static_cast<u32>(callback), flags);
            m.set_return(mgr.enum_system_locales(m, /*wide=*/true, callback, flags));
        },
        8
    });

    // BOOL EnumUILanguagesA/W(UILANGUAGE_ENUMPROC proc, DWORD dwFlags,
    //                         LONG_PTR lParam) — stdcall_bytes=12.
    auto enum_ui_languages = [&mgr](Marshaler& m, bool wide) {
        const GuestAddr callback = m.arg_ptr(0);
        const u32 flags = static_cast<u32>(m.arg(1));
        const u64 lparam = m.arg(2);
        VITREO_TRACE("[locale] EnumUILanguages%c(callback=0x%llx, flags=0x%x)\n",
                     wide ? 'W' : 'A', static_cast<unsigned long long>(callback),
                     flags);
        m.set_return(mgr.enum_ui_languages(m, wide, callback, flags, lparam));
    };
    exports.entries.push_back(ExportEntry{
        "EnumUILanguagesA", 0,
        [enum_ui_languages](Marshaler& m) { enum_ui_languages(m, false); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "EnumUILanguagesW", 0,
        [enum_ui_languages](Marshaler& m) { enum_ui_languages(m, true); },
        12
    });

    // BOOL EnumCalendarInfoA/W(proc, LCID, CALID, CALTYPE) — stdcall_bytes=16.
    auto enum_calendar = [&mgr](Marshaler& m, bool wide) {
        const GuestAddr callback = m.arg_ptr(0);
        const u32 locale = static_cast<u32>(m.arg(1));
        const u32 calendar = static_cast<u32>(m.arg(2));
        const u32 cal_type = static_cast<u32>(m.arg(3));
        m.set_return(mgr.enum_calendar_info(m, wide, callback, locale, calendar,
                                            cal_type));
    };
    exports.entries.push_back(ExportEntry{
        "EnumCalendarInfoA", 0,
        [enum_calendar](Marshaler& m) { enum_calendar(m, false); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "EnumCalendarInfoW", 0,
        [enum_calendar](Marshaler& m) { enum_calendar(m, true); },
        16
    });

    // GetLocaleInfoEx (4 args, stdcall_bytes=16)
    //   int GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType,
    //                       LPWSTR lpLCData, int cchData);
    exports.entries.push_back(ExportEntry{
        "GetLocaleInfoEx", 0,
        [&mgr](Marshaler& m) {
            auto locale_name = m.arg_ptr(0);
            auto lc_type     = static_cast<u32>(m.arg(1));
            // NULL locale name + LCType=0 -> ERROR_INVALID_FLAGS (0x3EC) on real Win.
            if (locale_name == 0 && lc_type == 0) {
                set_last_error(error_codes::ERROR_INVALID_FLAGS);
                m.set_return(0);
                return;
            }
            auto buf         = m.arg_ptr(2);
            auto cch_buf     = static_cast<i32>(m.arg(3));
            auto result = mgr.get_locale_info_ex(m, locale_name, lc_type, buf, cch_buf);
            VITREO_TRACE("[locale] GetLocaleInfoEx(name=0x%x, lctype=0x%x, buf=0x%x, cch=%d) -> %d\n",
                         static_cast<u32>(locale_name), lc_type,
                         static_cast<u32>(buf), cch_buf, result);
            m.set_return(static_cast<u64>(result));
        },
        16
    });

    // IsValidLocale (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "IsValidLocale", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            auto locale = static_cast<u32>(m.arg(0));
            auto flags  = static_cast<u32>(m.arg(1));
            m.set_return(static_cast<u64>(mgr.is_valid_locale(locale, flags)));
        },
        8
    });

    // GetACP (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetACP", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_acp()));
        },
        0
    });

    // GetOEMCP (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetOEMCP", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_oem_cp()));
        },
        0
    });

    // FindStringOrdinal (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "FindStringOrdinal", 0,
        [&mgr](Marshaler& m) {
            auto flags      = static_cast<u32>(m.arg(0));
            auto source_ptr = m.arg_ptr(1);
            auto cch_source = static_cast<i32>(m.arg(2));
            auto value_ptr  = m.arg_ptr(3);
            auto cch_value  = static_cast<i32>(m.arg(4));
            auto ignore_case = static_cast<u32>(m.arg(5));
            auto result = mgr.find_string_ordinal(m, flags, source_ptr, cch_source,
                                                   value_ptr, cch_value, ignore_case);
            m.set_return(static_cast<u64>(static_cast<u32>(result)));
        },
        24
    });

    // CompareStringEx (9 args, stdcall_bytes=36)
    exports.entries.push_back(ExportEntry{
        "CompareStringEx", 0,
        [&mgr](Marshaler& m) {
            auto str1        = m.arg_ptr(2);
            auto str2        = m.arg_ptr(4);
            auto locale_name = m.arg_ptr(0);
            auto flags       = static_cast<u32>(m.arg(1));
            auto len1        = static_cast<i32>(m.arg(3));
            auto len2        = static_cast<i32>(m.arg(5));
            if (str1 == 0 || str2 == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            (void)m.arg(6);
            (void)m.arg(7);
            (void)m.arg(8);
            auto result = mgr.compare_string_ex(m, locale_name, flags,
                                                str1, len1, str2, len2);
            m.set_return(static_cast<u64>(result));
        },
        36
    });

    // SleepConditionVariableSRW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "SleepConditionVariableSRW", 0,
        [&mgr](Marshaler& m) {
            auto cv    = m.arg_ptr(0);
            auto lock  = m.arg_ptr(1);
            auto ms    = static_cast<u32>(m.arg(2));
            auto flags = static_cast<u32>(m.arg(3));
            {
                srw_word::Word cvw, lw;
                const bool wide = m.cpu_is_64bit();
                if (srw_word::resolve(m.memory(), cv, wide, cvw) &&
                    srw_word::resolve(m.memory(), lock, wide, lw)) {
                    // CONDITION_VARIABLE_LOCKMODE_SHARED
                    const bool shared = (flags & 1u) != 0;
                    const u32 tid = ThreadManager::get_current_thread_id_static();
                    if (!shared && !srw_word::owned_exclusively_by(lw, tid)) {
                        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                        m.set_return(0);
                        return;
                    }
                    const bool woken = srw_word::cv_sleep_srw(
                        cvw, lw, shared, ms, tid, nullptr);
                    if (!woken) set_last_error(error_codes::ERROR_TIMEOUT);
                    m.set_return(woken ? 1u : 0u);
                    return;
                }
            }
            auto ok    = mgr.sleep_condition_variable_srw(cv, lock, ms, flags);
            // Lock is reacquired before returning — update guest SRWLOCK value.
            (void)m.memory().write_u32(lock, 1);
            m.set_return(static_cast<u64>(ok));
        },
        16
    });

    // AcquireSRWLockExclusive (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "AcquireSRWLockExclusive", 0,
        [&mgr](Marshaler& m) {
            auto lock = m.arg_ptr(0);
            {
                srw_word::Word w;
                if (srw_word::resolve(m.memory(), lock, m.cpu_is_64bit(), w)) {
                    srw_word::acquire_exclusive(w, ThreadManager::get_current_thread_id_static(), nullptr);
                    return;
                }
            }
            mgr.acquire_srw_lock_exclusive(lock);
            // Update guest SRWLOCK value to match real Windows (bit 0 = locked).
            // Without this, guest code that reads the value inline sees 0 (free)
            // and may re-enter AcquireSRWLockExclusive, causing a self-deadlock.
            (void)m.memory().write_u32(lock, 1);
        },
        4
    });

    // ReleaseSRWLockExclusive (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "ReleaseSRWLockExclusive", 0,
        [&mgr](Marshaler& m) {
            auto lock = m.arg_ptr(0);
            {
                srw_word::Word w;
                if (srw_word::resolve(m.memory(), lock, m.cpu_is_64bit(), w)) {
                    srw_word::release_exclusive(w, ThreadManager::get_current_thread_id_static());
                    return;
                }
            }
            mgr.release_srw_lock_exclusive(lock);
            (void)m.memory().write_u32(lock, 0);
        },
        4
    });

    // AcquireSRWLockShared (1 arg, stdcall_bytes=4)
    // A REAL shared acquisition. Taking this exclusively is semantically sound
    // but serialises every reader against every other reader, which is what
    // froze the UI for seconds at a time — see the SrwLock comment in
    // sync.hpp.
    exports.entries.push_back(ExportEntry{
        "AcquireSRWLockShared", 0,
        [&mgr](Marshaler& m) {
            auto lock = m.arg_ptr(0);
            {
                srw_word::Word w;
                if (srw_word::resolve(m.memory(), lock, m.cpu_is_64bit(), w)) {
                    srw_word::acquire_shared(w, ThreadManager::get_current_thread_id_static(), nullptr);
                    return;
                }
            }
            mgr.acquire_srw_lock_shared(lock);
            (void)m.memory().write_u32(lock, 1);
        },
        4
    });

    // ReleaseSRWLockShared (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "ReleaseSRWLockShared", 0,
        [&mgr](Marshaler& m) {
            auto lock = m.arg_ptr(0);
            {
                srw_word::Word w;
                if (srw_word::resolve(m.memory(), lock, m.cpu_is_64bit(), w)) {
                    (void)srw_word::release_shared(w, ThreadManager::get_current_thread_id_static());
                    return;
                }
            }
            // Only clear the guest-visible word when the LAST reader leaves;
            // other readers still hold it.
            if (mgr.release_srw_lock_shared(lock)) {
                (void)m.memory().write_u32(lock, 0);
            }
        },
        4
    });

    // TryAcquireSRWLockShared (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "TryAcquireSRWLockShared", 0,
        [&mgr](Marshaler& m) {
            auto lock = m.arg_ptr(0);
            {
                srw_word::Word w;
                if (srw_word::resolve(m.memory(), lock, m.cpu_is_64bit(), w)) {
                    m.set_return(srw_word::try_acquire_shared(w, ThreadManager::get_current_thread_id_static()) ? 1u : 0u);
                    return;
                }
            }
            bool ok = mgr.try_acquire_srw_lock_shared(lock);
            if (ok) (void)m.memory().write_u32(lock, 1);
            m.set_return(static_cast<u64>(ok ? 1 : 0));
        },
        4
    });

    // WakeAllConditionVariable (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "WakeAllConditionVariable", 0,
        [&mgr](Marshaler& m) {
            auto cv = m.arg_ptr(0);
            {
                srw_word::Word cvw;
                if (srw_word::resolve(m.memory(), cv, m.cpu_is_64bit(), cvw)) {
                    srw_word::cv_wake(cvw, /*all=*/true);
                    m.set_return(0);
                    return;
                }
            }
            mgr.wake_all_condition_variable(cv);
            // The native API is VOID.  Do not leak the host-call dispatch
            // address that RAX contains on thunk entry back into guest code.
            m.set_return(0);
            VITREO_TRACE("[sync] WakeAllConditionVariable(0x%llx)\n",
                         static_cast<unsigned long long>(cv));
        },
        4
    });

    // LCMapStringA (6 args, stdcall_bytes=24): ACP widen → lcmap_units → narrow
    exports.entries.push_back(ExportEntry{
        "LCMapStringA", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            u32 flags     = static_cast<u32>(m.arg(1));
            GuestAddr src = m.arg_ptr(2);
            i32 src_len   = static_cast<i32>(m.arg(3));
            GuestAddr dst = m.arg_ptr(4);
            i32 dst_len   = static_cast<i32>(m.arg(5));
            if (src == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            constexpr u32 LCMAP_SORTKEY = 0x00000400;
            std::vector<u8> bytes;
            const bool minus_one = src_len < 0;
            if (minus_one) {
                for (i32 i = 0; i < 65536; ++i) {
                    auto r = m.memory().read_u8(src + static_cast<u64>(i));
                    if (!r) break;
                    if (r.value() == 0) break;
                    bytes.push_back(static_cast<u8>(r.value()));
                }
            } else {
                bytes.resize(static_cast<usize>(src_len));
                for (i32 i = 0; i < src_len; ++i) {
                    auto r = m.memory().read_u8(src + static_cast<u64>(i));
                    bytes[static_cast<usize>(i)] =
                        r.is_ok() ? static_cast<u8>(r.value()) : 0;
                }
            }
            std::vector<u16> wide(bytes.size() + 4);
            auto wr = codepage::multi_byte_to_wide(
                codepage::kCpAcp, 0, bytes.data(), bytes.size(), wide.data(),
                wide.size());
            if (!wr.ok()) {
                set_last_error(wr.error);
                m.set_return(0);
                return;
            }
            wide.resize(wr.count);

            std::vector<u16> mapped;
            std::vector<u8> key;
            const i32 needed = lcmap_units(
                flags, wide.data(), wide.size(), minus_one,
                (flags & LCMAP_SORTKEY) ? nullptr : &mapped,
                (flags & LCMAP_SORTKEY) ? &key : nullptr);
            if (needed <= 0) {
                m.set_return(0);
                return;
            }
            if (dst == 0 || dst_len == 0) {
                m.set_return(static_cast<u64>(needed));
                return;
            }
            if (flags & LCMAP_SORTKEY) {
                if (dst_len < needed) {
                    set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                    m.set_return(0);
                    return;
                }
                for (i32 i = 0; i < needed; ++i) {
                    (void)m.memory().write_u8(dst + static_cast<u64>(i),
                                              key[static_cast<usize>(i)]);
                }
                m.set_return(static_cast<u64>(needed));
                return;
            }
            const bool had_nul =
                minus_one && !mapped.empty() && mapped.back() == 0;
            const usize map_units =
                had_nul ? mapped.size() - 1 : mapped.size();
            std::vector<u8> out(map_units * 2 + 2);
            auto nr = codepage::wide_to_multi_byte(
                codepage::kCpAcp, 0, mapped.data(), map_units, out.data(),
                out.size(), nullptr, false);
            if (!nr.ok()) {
                set_last_error(nr.error);
                m.set_return(0);
                return;
            }
            out.resize(nr.count);
            if (had_nul) out.push_back(0);
            if (static_cast<i32>(out.size()) > dst_len) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            for (usize i = 0; i < out.size(); ++i) {
                (void)m.memory().write_u8(dst + i, out[i]);
            }
            m.set_return(static_cast<u64>(out.size()));
        },
        24
    });

    // LCMapStringEx (9 args, stdcall_bytes=36)
    exports.entries.push_back(ExportEntry{
        "LCMapStringEx", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            u32 flags     = static_cast<u32>(m.arg(1));
            GuestAddr src = m.arg_ptr(2);
            i32 src_len   = static_cast<i32>(m.arg(3));
            GuestAddr dst = m.arg_ptr(4);
            i32 dst_len   = static_cast<i32>(m.arg(5));
            (void)m.arg(6);
            (void)m.arg(7);
            (void)m.arg(8);
            m.set_return(static_cast<u64>(
                lcmap_string_w(m.memory(), flags, src, src_len, dst, dst_len)));
        },
        36
    });

    // LocaleNameToLCID (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "LocaleNameToLCID", 0,
        [](Marshaler& m) {
            // LPCWSTR lpName, DWORD dwFlags → LCID
            (void)m.arg(0); // locale name pointer
            (void)m.arg(1); // flags
            // Return en-US LCID (0x0409) for any input
            m.set_return(static_cast<u32>(0x0409));
        },
        8
    });

    // GetStringTypeA (5 args, stdcall_bytes=20): character type info
    exports.entries.push_back(ExportEntry{
        "GetStringTypeA", 0,
        [](Marshaler& m) {
            // LCID locale, DWORD dwInfoType, LPCSTR lpSrcStr, int cchSrc, LPWORD lpCharType
            (void)m.arg(0); // locale
            u32 info_type = static_cast<u32>(m.arg(1));
            GuestAddr src = m.arg_ptr(2);
            i32 src_len   = static_cast<i32>(m.arg(3));
            GuestAddr out = m.arg_ptr(4);

            (void)info_type; // CT_CTYPE1=1, CT_CTYPE2=2, CT_CTYPE3=4

            if (src == 0 || out == 0) { m.set_return(0u); return; }

            // Read source
            std::vector<u8> src_buf;
            if (src_len == -1) {
                for (u32 i = 0; i < 4096u; ++i) {
                    auto r = m.memory().read_u8(src + i);
                    if (r.is_err()) break;
                    src_buf.push_back(static_cast<u8>(r.value()));
                    if (r.value() == 0) break;
                }
                if (!src_buf.empty() && src_buf.back() == 0) src_buf.pop_back();
                src_len = static_cast<i32>(src_buf.size());
            } else if (src_len > 0) {
                src_buf.resize(static_cast<size_t>(src_len));
                (void)m.memory().read_bytes(src, reinterpret_cast<Byte*>(src_buf.data()),
                                            static_cast<usize>(src_len));
            }

            // Write WORD character type for each character (CT_CTYPE1)
            // C1_UPPER=1, C1_LOWER=2, C1_DIGIT=4, C1_SPACE=8, C1_ALPHA=0x100
            for (i32 i = 0; i < src_len; ++i) {
                u16 ct = 0;
                u8 ch = src_buf[static_cast<size_t>(i)];
                if (ch >= 'A' && ch <= 'Z') ct = 0x0001 | 0x0100; // C1_UPPER | C1_ALPHA
                else if (ch >= 'a' && ch <= 'z') ct = 0x0002 | 0x0100; // C1_LOWER | C1_ALPHA
                else if (ch >= '0' && ch <= '9') ct = 0x0004; // C1_DIGIT
                else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ct = 0x0008; // C1_SPACE
                else if (ch >= 0x20 && ch <= 0x7E) ct = 0x0010; // C1_PUNCT (printable)
                else ct = 0x0020; // C1_CNTRL
                (void)m.memory().write_u16(out + static_cast<u32>(i) * 2, ct);
            }

            m.set_return(1u); // TRUE
        },
        20
    });

    // GetLocaleInfoA (4 args, stdcall_bytes=16): return locale info for en-US
    exports.entries.push_back(ExportEntry{
        "GetLocaleInfoA", 0,
        [](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_FLAGS); m.set_return(0); return; }
            // LCID locale, LCTYPE LCType, LPSTR lpLCData, int cchData
            (void)m.arg(0); // locale
            u32 lc_type  = static_cast<u32>(m.arg(1)) & 0xFFFF; // mask out flags
            GuestAddr buf = m.arg_ptr(2);
            i32 cch      = static_cast<i32>(m.arg(3));

            // Common LCTYPE values for en-US
            const char* result = nullptr;
            switch (lc_type) {
                case 0x0001: result = "0409"; break; // LOCALE_ILANGUAGE
                case 0x0002: result = "English (United States)"; break; // LOCALE_SLANGUAGE
                case 0x0003: result = "eng"; break;  // LOCALE_SABBREVLANGNAME
                case 0x000E: result = "."; break;    // LOCALE_SDECIMAL
                case 0x000F: result = ","; break;    // LOCALE_STHOUSAND
                case 0x0014: result = "/"; break;    // LOCALE_SDATE
                case 0x0015: result = ":"; break;    // LOCALE_STIME
                case 0x001D: result = "M/d/yyyy"; break;  // LOCALE_SSHORTDATE
                case 0x001E: result = "dddd, MMMM d, yyyy"; break; // LOCALE_SLONGDATE
                case 0x001F: result = "$"; break;    // LOCALE_SCURRENCY
                case 0x0020: result = "AM"; break;   // LOCALE_S1159
                case 0x0021: result = "PM"; break;   // LOCALE_S2359
                case 0x0022: result = "2"; break;    // LOCALE_IDIGITS
                case 0x002A: result = "h:mm:ss tt"; break; // LOCALE_STIMEFORMAT
                case 0x1004: result = "en"; break;   // LOCALE_SISO639LANGNAME
                case 0x100E: result = "."; break;    // LOCALE_SMONDECKSEP
                default:     result = ""; break;
            }

            i32 result_len = static_cast<i32>(strlen(result)) + 1; // including NUL

            // If cchData is 0, return required buffer size
            if (cch == 0 || buf == 0) {
                m.set_return(static_cast<u32>(result_len));
                return;
            }

            // Write result
            i32 copy_len = std::min(result_len, cch);
            (void)m.memory().write_bytes(buf, reinterpret_cast<const Byte*>(result),
                                         static_cast<usize>(copy_len));
            // Ensure NUL termination
            if (copy_len > 0) {
                (void)m.memory().write_u8(buf + static_cast<u32>(copy_len - 1), 0);
            }
            m.set_return(static_cast<u32>(copy_len));
        },
        16
    });

    // ------------------------------------------------------------------
    // Phase-1 string/locale extras
    // ------------------------------------------------------------------

    // CompareStringOrdinal(LPCWCH s1, INT c1, LPCWCH s2, INT c2, BOOL bIgnoreCase)
    // 5 args, stdcall_bytes=20
    // Returns: CSTR_LESS_THAN=1, CSTR_EQUAL=2, CSTR_GREATER_THAN=3, 0=fail
    exports.entries.push_back(ExportEntry{
        "CompareStringOrdinal", 0,
        [](Marshaler& m) {
            GuestAddr s1 = m.arg_ptr(0);
            i32 c1 = static_cast<i32>(m.arg(1));
            GuestAddr s2 = m.arg_ptr(2);
            i32 c2 = static_cast<i32>(m.arg(3));
            bool ignore_case = m.arg(4) != 0;
            if (s1 == 0 || s2 == 0) { m.set_return(0); return; }

            auto read_len = [&](GuestAddr p, i32 c) -> usize {
                if (c >= 0) return static_cast<usize>(c);
                for (usize i = 0; i < 65536; ++i) {
                    if (m.read_u16(p + i * 2).value_or(0) == 0) return i;
                }
                return 65536;
            };
            usize n1 = read_len(s1, c1);
            usize n2 = read_len(s2, c2);
            usize n  = std::min(n1, n2);
            for (usize i = 0; i < n; ++i) {
                u16 a = m.read_u16(s1 + i * 2).value_or(0);
                u16 b = m.read_u16(s2 + i * 2).value_or(0);
                if (ignore_case) {
                    if (a >= 'a' && a <= 'z') a = static_cast<u16>(a - 32);
                    if (b >= 'a' && b <= 'z') b = static_cast<u16>(b - 32);
                }
                if (a != b) { m.set_return(a < b ? 1u : 3u); return; }
            }
            if (n1 < n2) { m.set_return(1u); return; }
            if (n1 > n2) { m.set_return(3u); return; }
            m.set_return(2u);
        },
        20
    });

    // GetStringTypeExA(LCID, DWORD, LPCSTR, int, LPWORD) - delegate to GetStringTypeA
    // 5 args, stdcall_bytes=20
    exports.entries.push_back(ExportEntry{
        "GetStringTypeExA", 0,
        [](Marshaler& m) {
            (void)m.arg(0); // locale
            u32 info_type = static_cast<u32>(m.arg(1));
            GuestAddr src = m.arg_ptr(2);
            i32 src_len   = static_cast<i32>(m.arg(3));
            GuestAddr out = m.arg_ptr(4);
            (void)info_type;
            if (src == 0 || out == 0) { m.set_return(0u); return; }

            std::vector<u8> buf;
            if (src_len == -1) {
                for (u32 i = 0; i < 4096u; ++i) {
                    u8 c = m.read_u8(src + i).value_or(0);
                    if (c == 0) break;
                    buf.push_back(c);
                }
                src_len = static_cast<i32>(buf.size());
            } else if (src_len > 0) {
                buf.resize(static_cast<usize>(src_len));
                (void)m.memory().read_bytes(src, reinterpret_cast<Byte*>(buf.data()),
                                            static_cast<usize>(src_len));
            }
            for (i32 i = 0; i < src_len; ++i) {
                u16 ct = 0;
                u8 ch = buf[static_cast<usize>(i)];
                if (ch >= 'A' && ch <= 'Z') ct = 0x0001 | 0x0100;
                else if (ch >= 'a' && ch <= 'z') ct = 0x0002 | 0x0100;
                else if (ch >= '0' && ch <= '9') ct = 0x0004;
                else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ct = 0x0008;
                else if (ch < 0x20) ct = 0x0020; // C1_CNTRL
                else ct = 0x0010; // C1_PUNCT
                (void)m.write_u16(out + static_cast<u32>(i) * 2, ct);
            }
            m.set_return(1u);
        },
        20
    });

    // GetStringTypeExW(LCID, DWORD, LPCWSTR, int, LPWORD) - 5 args, stdcall=20
    exports.entries.push_back(ExportEntry{
        "GetStringTypeExW", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1); // info type
            GuestAddr src = m.arg_ptr(2);
            i32 src_len   = static_cast<i32>(m.arg(3));
            GuestAddr out = m.arg_ptr(4);
            if (src == 0 || out == 0) { m.set_return(0u); return; }

            usize n;
            if (src_len < 0) {
                n = 0;
                for (usize i = 0; i < 65536; ++i) {
                    if (m.read_u16(src + i * 2).value_or(0) == 0) { n = i; break; }
                }
            } else {
                n = static_cast<usize>(src_len);
            }
            for (usize i = 0; i < n; ++i) {
                u16 ch = m.read_u16(src + i * 2).value_or(0);
                u16 ct = 0;
                if (ch >= 'A' && ch <= 'Z') ct = 0x0001 | 0x0100;
                else if (ch >= 'a' && ch <= 'z') ct = 0x0002 | 0x0100;
                else if (ch >= '0' && ch <= '9') ct = 0x0004;
                else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ct = 0x0008;
                else if (ch < 0x20) ct = 0x0020;
                else if (ch < 0x80) ct = 0x0010;
                else ct = 0x0100; // best effort: alpha
                (void)m.write_u16(out + i * 2, ct);
            }
            m.set_return(1u);
        },
        20
    });

    // GetCPInfoExA/W — measured tables via codepage::code_page_info.
    // CodePageName strings from Windows cpinfoex_name_probe.expected.
    auto fill_cpinfoex = [](Marshaler& m, GuestAddr p, u32 cp, u32 flags,
                            bool wide) -> bool {
        if (flags != 0) {
            set_last_error(error_codes::ERROR_INVALID_FLAGS);
            return false;
        }
        if (p == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        codepage::CpInfo info;
        if (!codepage::code_page_info(cp, info)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        (void)m.write_u32(p, info.max_char_size);
        (void)m.write_u8(p + 4, info.default_char[0]);
        (void)m.write_u8(p + 5, info.default_char[1]);
        for (u32 i = 0; i < 12; ++i)
            (void)m.write_u8(p + 6 + i, info.lead_bytes[i]);
        (void)m.write_u16(p + 18, info.unicode_default_char);
        // Windows: CP_ACP writes the resolved ACP (1252 or 65001), not 0.
        (void)m.write_u32(p + 20, info.code_page);
        const char* name = "Codepage";
        switch (info.code_page) {
            // Spacing matches Windows NLS strings exactly (two/three/one spaces).
            case 1252: name = "1252  (ANSI - Latin I)"; break;
            case 437:  name = "437   (OEM - United States)"; break;
            case 10000: name = "10000 (MAC - Roman)"; break;
            case 65001: name = "65001 (UTF-8)"; break;
            default: break;
        }
        u32 i = 0;
        if (wide) {
            for (; name[i] && i < 259; ++i)
                (void)m.write_u16(p + 24 + i * 2, static_cast<u16>(name[i]));
            (void)m.write_u16(p + 24 + i * 2, 0);
        } else {
            for (; name[i] && i < 259; ++i)
                (void)m.write_u8(p + 24 + i, static_cast<u8>(name[i]));
            (void)m.write_u8(p + 24 + i, 0);
        }
        return true;
    };
    exports.entries.push_back(ExportEntry{
        "GetCPInfoExA", 0,
        [fill_cpinfoex](Marshaler& m) {
            u32 cp = static_cast<u32>(m.arg(0));
            u32 flags = static_cast<u32>(m.arg(1));
            GuestAddr p = m.arg_ptr(2);
            m.set_return(fill_cpinfoex(m, p, cp, flags, false) ? 1u : 0u);
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetCPInfoExW", 0,
        [fill_cpinfoex](Marshaler& m) {
            u32 cp = static_cast<u32>(m.arg(0));
            u32 flags = static_cast<u32>(m.arg(1));
            GuestAddr p = m.arg_ptr(2);
            m.set_return(fill_cpinfoex(m, p, cp, flags, true) ? 1u : 0u);
        },
        12
    });

    // FoldStringW(DWORD flags, LPCWSTR src, int cchSrc, LPWSTR dst, int cchDst)
    // 5 args, stdcall_bytes=20.  Best-effort copy-through (no real folding).
    exports.entries.push_back(ExportEntry{
        "FoldStringW", 0,
        [](Marshaler& m) {
            (void)m.arg(0); // flags
            GuestAddr src = m.arg_ptr(1);
            i32 cch_src = static_cast<i32>(m.arg(2));
            GuestAddr dst = m.arg_ptr(3);
            i32 cch_dst = static_cast<i32>(m.arg(4));
            if (src == 0) { m.set_return(0); return; }
            usize n;
            if (cch_src < 0) {
                n = 0;
                for (usize i = 0; i < 65536; ++i) {
                    if (m.read_u16(src + i * 2).value_or(0) == 0) { n = i + 1; break; }
                }
            } else {
                n = static_cast<usize>(cch_src);
            }
            if (dst == 0 || cch_dst == 0) {
                m.set_return(static_cast<u32>(n));
                return;
            }
            if (cch_dst > 0 && static_cast<usize>(cch_dst) < n) {
                m.set_return(0);
                return;
            }
            for (usize i = 0; i < n; ++i) {
                (void)m.write_u16(dst + i * 2, m.read_u16(src + i * 2).value_or(0));
            }
            m.set_return(static_cast<u32>(n));
        },
        20
    });

    // FoldStringA — same shape, byte-copy
    exports.entries.push_back(ExportEntry{
        "FoldStringA", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr src = m.arg_ptr(1);
            i32 cch_src = static_cast<i32>(m.arg(2));
            GuestAddr dst = m.arg_ptr(3);
            i32 cch_dst = static_cast<i32>(m.arg(4));
            if (src == 0) { m.set_return(0); return; }
            usize n;
            if (cch_src < 0) {
                n = 0;
                for (usize i = 0; i < 65536; ++i) {
                    if (m.read_u8(src + i).value_or(0) == 0) { n = i + 1; break; }
                }
            } else {
                n = static_cast<usize>(cch_src);
            }
            if (dst == 0 || cch_dst == 0) {
                m.set_return(static_cast<u32>(n));
                return;
            }
            if (cch_dst > 0 && static_cast<usize>(cch_dst) < n) {
                m.set_return(0);
                return;
            }
            for (usize i = 0; i < n; ++i) {
                (void)m.write_u8(dst + i, m.read_u8(src + i).value_or(0));
            }
            m.set_return(static_cast<u32>(n));
        },
        20
    });

    // ------------------------------------------------------------------------
    // Wave 2: Locale "Ex" variants & geo/timezone APIs
    // ------------------------------------------------------------------------
    auto write_wide_lit = [](Marshaler& m, GuestAddr dst, const char* s, i32 cch_buf) -> i32 {
        i32 len = 0; while (s[len]) ++len; i32 need = len + 1;
        if (dst == 0 || cch_buf == 0) return need;
        i32 n = (cch_buf < need) ? cch_buf : need;
        for (i32 i = 0; i < n; ++i) {
            (void)m.memory().write_u16(dst + static_cast<u64>(i) * 2,
                static_cast<u16>(static_cast<unsigned char>(s[i])));
        }
        return n;
    };

    // GetDateFormatEx(lpLocaleName, dwFlags, lpDate, lpFormat, lpDateStr, cchDate, lpCalendar) -> int
    exports.entries.push_back(ExportEntry{
        "GetDateFormatEx", 0,
        [write_wide_lit](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            GuestAddr buf = m.arg_ptr(4);
            i32 cch = static_cast<i32>(m.arg(5));
            (void)m.arg(6);
            i32 n = write_wide_lit(m, buf, "01/01/2025", cch);
            m.set_return(static_cast<u32>(n));
        }, 28 });

    // GetTimeFormatEx(lpLocaleName, dwFlags, lpTime, lpFormat, lpTimeStr, cchTime) -> int
    exports.entries.push_back(ExportEntry{
        "GetTimeFormatEx", 0,
        [write_wide_lit](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            GuestAddr buf = m.arg_ptr(4);
            i32 cch = static_cast<i32>(m.arg(5));
            i32 n = write_wide_lit(m, buf, "12:00:00", cch);
            m.set_return(static_cast<u32>(n));
        }, 24 });

    auto format_currency_w = [](Marshaler& m, GuestAddr value_p, GuestAddr buf,
                                i32 cch) {
        // Learn: GetCurrencyFormat{Ex,W} write a locale currency string, or
        // return the required cch (including NUL) when cchCurrency is 0.
        std::string val = "0";
        if (value_p) {
            val = m.read_wide_string(value_p).value_or(
                m.read_ansi_string(value_p).value_or("0"));
        }
        std::string out = "$" + val;
        i32 need = static_cast<i32>(out.size()) + 1;
        if (buf == 0 || cch == 0) {
            m.set_return(static_cast<u32>(need));
            return;
        }
        i32 n = (cch < need) ? cch : need;
        for (i32 i = 0; i < n; ++i) {
            (void)m.memory().write_u16(
                buf + static_cast<u64>(i) * 2,
                static_cast<u16>(static_cast<unsigned char>(
                    i < static_cast<i32>(out.size()) ? out[i] : 0)));
        }
        m.set_return(static_cast<u32>(n));
    };

    // GetCurrencyFormatEx(lpLocaleName, dwFlags, lpValue, lpFormat, lpCurrencyStr, cchCurrency) -> int
    exports.entries.push_back(ExportEntry{
        "GetCurrencyFormatEx", 0,
        [format_currency_w](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            GuestAddr value_p = m.arg_ptr(2);
            (void)m.arg(3);
            format_currency_w(m, value_p, m.arg_ptr(4),
                              static_cast<i32>(m.arg(5)));
        }, 24 });

    // GetCurrencyFormatW(Locale, dwFlags, lpValue, lpFormat, lpCurrencyStr, cchCurrency) -> int
    // LCID sibling of GetCurrencyFormatEx. Qt5Core IAT-imports this; a
    // missing-export thunk returns 0xFFFFFFFF as a character count and pops
    // 0 stdcall bytes.
    exports.entries.push_back(ExportEntry{
        "GetCurrencyFormatW", 0,
        [format_currency_w](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            GuestAddr value_p = m.arg_ptr(2);
            (void)m.arg(3);
            format_currency_w(m, value_p, m.arg_ptr(4),
                              static_cast<i32>(m.arg(5)));
        }, 24 });

    // GetNumberFormatW / GetNumberFormatA (6 args, stdcall_bytes=24).
    exports.entries.push_back(ExportEntry{
        "GetNumberFormatW", 0, [&mgr](Marshaler& m) { get_number_format(mgr, m, /*wide=*/true); }, 24});
    exports.entries.push_back(ExportEntry{
        "GetNumberFormatA", 0, [&mgr](Marshaler& m) { get_number_format(mgr, m, /*wide=*/false); }, 24});

    // GetNumberFormatEx (6 args, stdcall_bytes=24).
    exports.entries.push_back(ExportEntry{
        "GetNumberFormatEx", 0, [&mgr](Marshaler& m) { get_number_format_ex(mgr, m); }, 24});

    // GetUserDefaultLocaleName(lpLocaleName, cchLocaleName) -> int
    exports.entries.push_back(ExportEntry{
        "GetUserDefaultLocaleName", 0,
        [write_wide_lit](Marshaler& m) {
            GuestAddr buf = m.arg_ptr(0);
            i32 cch = static_cast<i32>(m.arg(1));
            i32 n = write_wide_lit(m, buf, "en-US", cch);
            m.set_return(static_cast<u32>(n));
        }, 8 });

    // GetSystemDefaultLocaleName(lpLocaleName, cchLocaleName) -> int
    exports.entries.push_back(ExportEntry{
        "GetSystemDefaultLocaleName", 0,
        [write_wide_lit](Marshaler& m) {
            GuestAddr buf = m.arg_ptr(0);
            i32 cch = static_cast<i32>(m.arg(1));
            i32 n = write_wide_lit(m, buf, "en-US", cch);
            m.set_return(static_cast<u32>(n));
        }, 8 });

    // LCIDToLocaleName(LCID, lpName, cchName, dwFlags) -> int
    exports.entries.push_back(ExportEntry{
        "LCIDToLocaleName", 0,
        [write_wide_lit](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr buf = m.arg_ptr(1);
            i32 cch = static_cast<i32>(m.arg(2));
            (void)m.arg(3);
            i32 n = write_wide_lit(m, buf, "en-US", cch);
            m.set_return(static_cast<u32>(n));
        }, 16 });

    // EnumSystemLocalesEx(lpLocaleEnumProcEx, dwFlags, lParam, lpReserved) -> BOOL
    exports.entries.push_back(ExportEntry{
        "EnumSystemLocalesEx", 0,
        [&mgr](Marshaler& m) {
            GuestAddr callback = m.arg_ptr(0);
            u32 flags = static_cast<u32>(m.arg(1));
            u64 lparam = m.arg(2);
            (void)m.arg(3); // lpReserved must be NULL on Windows; ignored here
            VITREO_TRACE("[locale] EnumSystemLocalesEx(callback=0x%x, flags=0x%x, lParam=0x%llx)\n",
                         static_cast<u32>(callback), flags,
                         static_cast<unsigned long long>(lparam));
            m.set_return(mgr.enum_system_locales_ex(m, callback, flags, lparam));
        },
        16 });

    // ResolveLocaleName(lpNameToResolve, lpLocaleName, cchLocaleName) -> int
    exports.entries.push_back(ExportEntry{
        "ResolveLocaleName", 0,
        [write_wide_lit](Marshaler& m) {
            GuestAddr in_p = m.arg_ptr(0);
            GuestAddr out_p = m.arg_ptr(1);
            i32 cch = static_cast<i32>(m.arg(2));
            std::string in = in_p ? m.read_wide_string(in_p).value_or("en-US") : "en-US";
            if (in.empty()) in = "en-US";
            // Just echo back input (or default).
            i32 need = static_cast<i32>(in.size()) + 1;
            if (out_p == 0 || cch == 0) { m.set_return(static_cast<u32>(need)); return; }
            i32 n = (cch < need) ? cch : need;
            for (i32 i = 0; i < n; ++i) {
                (void)m.memory().write_u16(out_p + static_cast<u64>(i) * 2,
                    static_cast<u16>(static_cast<unsigned char>(i < static_cast<i32>(in.size()) ? in[i] : 0)));
            }
            (void)write_wide_lit; // suppress unused capture warning if any
            m.set_return(static_cast<u32>(n));
        }, 12 });

    // GetGeoInfoW(Location, GeoType, lpGeoData, cchData, LangId) -> int
    exports.entries.push_back(ExportEntry{
        "GetGeoInfoW", 0,
        [write_wide_lit](Marshaler& m) {
            (void)m.arg(0);
            u32 type = static_cast<u32>(m.arg(1));
            GuestAddr buf = m.arg_ptr(2);
            i32 cch = static_cast<i32>(m.arg(3));
            (void)m.arg(4);
            const char* s = "US";
            switch (type) {
                case 4: s = "US"; break;       // GEO_ISO2
                case 5: s = "USA"; break;      // GEO_ISO3
                case 8: s = "United States"; break; // GEO_FRIENDLYNAME
                default: s = "US"; break;
            }
            i32 n = write_wide_lit(m, buf, s, cch);
            m.set_return(static_cast<u32>(n));
        }, 20 });

    // GetGeoInfoA(Location, GeoType, lpGeoData, cchData, LangId) -> int
    //
    // ANSI twin of GetGeoInfoW above: same values, narrow output. Returns the
    // required size (including the terminator) when the buffer is null/small,
    // per the GetGeoInfo contract.
    // https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getgeoinfoa
    exports.entries.push_back(ExportEntry{
        "GetGeoInfoA", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            u32 type = static_cast<u32>(m.arg(1));
            GuestAddr buf = m.arg_ptr(2);
            i32 cch = static_cast<i32>(m.arg(3));
            (void)m.arg(4);
            const char* s = "US";
            switch (type) {
                case 4: s = "US"; break;            // GEO_ISO2
                case 5: s = "USA"; break;           // GEO_ISO3
                case 8: s = "United States"; break; // GEO_FRIENDLYNAME
                default: s = "US"; break;
            }
            const i32 need = static_cast<i32>(std::strlen(s)) + 1;
            if (buf == 0 || cch == 0) {
                m.set_return(static_cast<u32>(need));
                return;
            }
            if (cch < need) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            for (i32 i = 0; i < need; ++i) {
                (void)m.write_u8(buf + static_cast<u64>(i),
                                 static_cast<u8>(s[i]));
            }
            m.set_return(static_cast<u32>(need));
        }, 20 });

    // GetUserGeoID(GeoClass) -> GEOID
    exports.entries.push_back(ExportEntry{
        "GetUserGeoID", 0,
        [](Marshaler& m) { (void)m.arg(0); m.set_return(244u); /* US */ }, 4 });

    // GetSystemDefaultLCID() -> LCID
    exports.entries.push_back(ExportEntry{
        "GetSystemDefaultLCID", 0,
        [](Marshaler& m) { m.set_return(0x0409u); /* en-US */ }, 0 });

    // GetDynamicTimeZoneInformation: kernel32 system/timezone.cpp (Windows
    // time-zone database), registered with GetTimeZoneInformation.

    // TzSpecificLocalTimeToSystemTime(lpTimeZoneInformation, lpLocalTime, lpUniversalTime) -> BOOL
    exports.entries.push_back(ExportEntry{
        "TzSpecificLocalTimeToSystemTime", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr local = m.arg_ptr(1);
            GuestAddr utc = m.arg_ptr(2);
            if (local && utc) {
                for (u32 i = 0; i < 16; i += 2) {
                    auto r = m.read_u16(local + i);
                    (void)m.write_u16(utc + i, r.is_ok() ? r.value() : 0);
                }
            }
            m.set_return(1);
        }, 12 });

    // LANGID GetThreadUILanguage(void)
    exports.entries.push_back(ExportEntry{
        "GetThreadUILanguage", 0,
        [&mgr](Marshaler& m) {
            m.set_return(thread_ui_language(mgr));
        },
        0 });

    // BOOL SetThreadPreferredUILanguages(dwFlags, pwszLanguagesBuffer, pulNumLanguages)
    exports.entries.push_back(ExportEntry{
        "SetThreadPreferredUILanguages", 0,
        [&mgr](Marshaler& m) {
            const u32 flags = static_cast<u32>(m.arg(0));
            const GuestAddr languages = m.arg_ptr(1);
            const GuestAddr count = m.arg_ptr(2);
            m.set_return(set_thread_preferred_ui_languages(
                             mgr, m, flags, languages, count)
                             ? 1u
                             : 0u);
        },
        12 });

    // GetThreadPreferredUILanguages(dwFlags, pulNumLanguages, pwszLanguagesBuffer, pcchLanguagesBuffer) -> BOOL
    exports.entries.push_back(ExportEntry{
        "GetThreadPreferredUILanguages", 0,
        [&mgr](Marshaler& m) {
            const u32 flags = static_cast<u32>(m.arg(0));
            const GuestAddr count = m.arg_ptr(1);
            const GuestAddr buffer = m.arg_ptr(2);
            const GuestAddr cch = m.arg_ptr(3);
            m.set_return(get_thread_preferred_ui_languages(
                             mgr, m, flags, count, buffer, cch)
                             ? 1u
                             : 0u);
        },
        16 });

    // GetUserPreferredUILanguages(dwFlags, pulNumLanguages,
    //   pwszLanguagesBuffer, pcchLanguagesBuffer) -> BOOL
    // The user locale exposed by Vitreo is en-US. Return it using the
    // double-NUL multistring and size-query rules documented by Microsoft.
    exports.entries.push_back(ExportEntry{
        "GetUserPreferredUILanguages", 0,
        [](Marshaler& m) {
            constexpr u32 MUI_LANGUAGE_ID = 0x4;
            constexpr u32 MUI_LANGUAGE_NAME = 0x8;
            const u32 flags = static_cast<u32>(m.arg(0));
            const GuestAddr nlangs = m.arg_ptr(1);
            const GuestAddr buf = m.arg_ptr(2);
            const GuestAddr cch_ptr = m.arg_ptr(3);
            if (nlangs == 0 || cch_ptr == 0 ||
                (flags & (MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME)) ==
                    (MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const char* language =
                (flags & MUI_LANGUAGE_ID) != 0 ? "0409" : "en-US";
            const u32 length =
                (flags & MUI_LANGUAGE_ID) != 0 ? 4u : 5u;
            const u32 required = length + 2; // entry NUL + list NUL
            const auto capacity = m.read_u32(cch_ptr);
            if (capacity.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            (void)m.write_u32(nlangs, 1);
            (void)m.write_u32(cch_ptr, required);
            if (buf == 0) {
                if (capacity.value() != 0) {
                    set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                    m.set_return(0);
                    return;
                }
                m.set_return(1);
                return;
            }
            if (capacity.value() < required) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            for (u32 i = 0; i < length; ++i) {
                (void)m.write_u16(buf + i * 2,
                                  static_cast<u16>(language[i]));
            }
            (void)m.write_u16(buf + length * 2, 0);
            (void)m.write_u16(buf + (length + 1) * 2, 0);
            VITREO_TRACE(
                "[locale] GetUserPreferredUILanguages(flags=0x%x) "
                "-> TRUE count=1 cch=%u\n",
                flags, required);
            m.set_return(1);
        }, 16 });

    return exports;
}

} // namespace vitreo::winapi
