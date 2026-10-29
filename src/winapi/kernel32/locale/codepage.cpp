// SPDX-License-Identifier: LGPL-2.1-only

/// @file codepage.cpp
/// @brief Code-page and locale helpers the MSVC CRT probes at startup.
///
/// IsValidCodePage, GetCPInfo, GetStringTypeW, and LCMapStringW —
/// CRT-critical exports kept beside the locale surface.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/lcmap.hpp"

#include <CoreFoundation/CoreFoundation.h>


#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>

// libicucore — bidi / general category for GetStringTypeW CT_CTYPE2/3.
extern "C" {
int8_t u_charDirection(int32_t c);
int8_t u_charType(int32_t c);
int32_t u_getIntPropertyValue(int32_t c, int32_t which);
int8_t u_isUAlphabetic(int32_t c);
}
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// Code Page / Locale (CRT critical)
// =============================================================================

void Kernel32ExtManager::impl_IsValidCodePage(Marshaler& m) {
    // BOOL IsValidCodePage(UINT CodePage). Measured on Windows: FALSE for the
    // pseudo code pages (CP_ACP..CP_THREAD_ACP, CP_SYMBOL). TRUE here for every
    // code page Vitreo converts exactly (codepage_convert.hpp); code pages
    // Windows installs but Vitreo cannot convert report FALSE rather than
    // promise conversions that would fail.
    m.set_return(codepage::is_valid_code_page(static_cast<u32>(m.arg(0))) ? 1u : 0u);
}

void Kernel32ExtManager::impl_GetCPInfo(Marshaler& m) {
    // BOOL GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo). CPINFO: MaxCharSize
    // (UINT), DefaultChar[2], LeadByte[12] -- 20 bytes. Values come from the
    // measured tables, with the pseudo code pages resolved as Windows does.
    const u32 cp = static_cast<u32>(m.arg(0));
    const GuestAddr info_ptr = m.arg_ptr(1);
    codepage::CpInfo info;
    if (!codepage::code_page_info(cp, info)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (info_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    u8 raw[20] = {};
    std::memcpy(raw, &info.max_char_size, 4);
    raw[4] = info.default_char[0];
    raw[5] = info.default_char[1];
    std::memcpy(raw + 6, info.lead_bytes, 12);
    if (m_memory.write_bytes(info_ptr, raw, sizeof(raw)).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

namespace codepage {

/// GetStringTypeW CT_CTYPE1 classification of one UTF-16 unit. Shared with
/// user32 IsCharAlpha/AlphaNumeric/Upper/Lower, which Windows answers from
/// the same table (u32_char_dde_probe ischarw == ctype1).
u16 ctype1_flags(u16 ch) noexcept {
    constexpr u16 C1_UPPER = 0x0001, C1_LOWER = 0x0002, C1_DIGIT = 0x0004;
    constexpr u16 C1_SPACE = 0x0008, C1_PUNCT = 0x0010, C1_CNTRL = 0x0020;
    constexpr u16 C1_BLANK = 0x0040, C1_XDIGIT = 0x0080, C1_ALPHA = 0x0100;
    constexpr u16 C1_DEFINED = 0x0200;
    auto cf_member = [](CFCharacterSetPredefinedSet set, u16 c) -> bool {
        return CFCharacterSetIsCharacterMember(
            CFCharacterSetGetPredefined(set), c);
    };
    u16 f = 0;
    if (ch <= 0xFF) {
        if (ch <= 0x7F) {
            if (ch <= 0x1F || ch == 0x7F) f |= C1_CNTRL;
            if (ch >= '0' && ch <= '9') f |= C1_DIGIT | C1_XDIGIT;
            if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
                f |= C1_XDIGIT;
            if (ch >= 'A' && ch <= 'Z') f |= C1_UPPER | C1_ALPHA;
            if (ch >= 'a' && ch <= 'z') f |= C1_LOWER | C1_ALPHA;
            if (ch == 0x20) f |= C1_SPACE | C1_BLANK;
            if (ch == 0x09) f |= C1_SPACE | C1_BLANK | C1_CNTRL;
            if (ch == 0x0A || ch == 0x0B || ch == 0x0C || ch == 0x0D)
                f |= C1_SPACE;
            if (ch >= 0x21 && ch <= 0x7E &&
                !((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z')))
                f |= C1_PUNCT;
        } else {
            if (ch >= 0x80 && ch <= 0x9F) {
                f |= C1_CNTRL;
                if (ch == 0x85) f |= C1_SPACE;
            } else if (ch == 0xA0) {
                f |= C1_SPACE | C1_BLANK;
            } else if ((ch >= 0xC0 && ch <= 0xD6) ||
                       (ch >= 0xD8 && ch <= 0xDE)) {
                f |= C1_UPPER | C1_ALPHA;
            } else if ((ch >= 0xDF && ch <= 0xF6) ||
                       (ch >= 0xF8 && ch <= 0xFF)) {
                f |= C1_LOWER | C1_ALPHA;
            } else if (ch == 0xAA || ch == 0xBA || ch == 0xB5) {
                f |= C1_LOWER | C1_ALPHA | C1_PUNCT;
            } else if (ch == 0xAD) {
                f |= C1_CNTRL | C1_PUNCT;
            } else if (ch == 0xB2 || ch == 0xB3 || ch == 0xB9) {
                f |= C1_DIGIT | C1_PUNCT;
            } else {
                f |= C1_PUNCT;
            }
        }
    } else {
        // Letters by general category, as Windows classifies them
        // (u32_char_dde_probe ischarw == ctype1): Lu upper, Ll lower, Lt both
        // (U+01C5), Lm / Lo / Nl alpha only (U+02B0, U+4E00, U+2160, U+3007;
        // Other_Uppercase/Lowercase does not count: U+2160, U+1D2C), and
        // combining marks are not alpha (U+0300, U+0345) -- CoreFoundation's
        // letter set includes M*, so it is not used here.
        switch (u_charType(static_cast<int32_t>(ch))) {
        case 1: f |= C1_UPPER | C1_ALPHA; break;              // Lu
        case 2: f |= C1_LOWER | C1_ALPHA; break;              // Ll
        case 3: f |= C1_UPPER | C1_LOWER | C1_ALPHA; break;   // Lt
        case 4: case 5: case 10: f |= C1_ALPHA; break;        // Lm Lo Nl
        default: break;
        }
        if (cf_member(kCFCharacterSetDecimalDigit, ch)) f |= C1_DIGIT;
        if (cf_member(kCFCharacterSetWhitespaceAndNewline, ch) ||
            cf_member(kCFCharacterSetWhitespace, ch)) {
            f |= C1_SPACE;
            if (ch == 0x3000 || ch == 0x205F) f |= C1_BLANK;
        }
        if (cf_member(kCFCharacterSetPunctuation, ch)) f |= C1_PUNCT;
        if (cf_member(kCFCharacterSetControl, ch)) f |= C1_CNTRL;
        if (cf_member(kCFCharacterSetSymbol, ch) && !(f & C1_PUNCT))
            f |= C1_PUNCT;
    }
    if (!(ch >= 0xD800 && ch <= 0xDFFF) && ch != 0xFFFE && ch != 0xFFFF)
        f |= C1_DEFINED;
    return f;
}

} // namespace codepage

void Kernel32ExtManager::impl_GetStringTypeW(Marshaler& m) {
    // BOOL GetStringTypeW(DWORD dwInfoType, LPCWSTR lpSrcStr, int cchSrc,
    //                     LPWORD lpCharType)
    u32 info_type = static_cast<u32>(m.arg(0));
    GuestAddr src_ptr = m.arg_ptr(1);
    i32 cch_src = static_cast<i32>(m.arg(2));
    GuestAddr char_type_ptr = m.arg_ptr(3);

    VITREO_TRACE("[kernel32_ext] GetStringTypeW(type=%u, src=0x%x, cch=%d, out=0x%x)\n",
                 info_type, static_cast<u32>(src_ptr), cch_src,
                 static_cast<u32>(char_type_ptr));

    if (src_ptr == 0 || char_type_ptr == 0 || src_ptr == char_type_ptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (info_type == 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0);
        return;
    }
    // CT_CTYPE1=1, CT_CTYPE2=2, CT_CTYPE3=3. Other non-zero values are
    // accepted on Windows (type 4 behaves like CTYPE3); we treat >3 as CTYPE3.
    const u32 ctype = (info_type >= 1 && info_type <= 3) ? info_type : 3;

    // Any negative cchSrc means null-terminated (Learn; Windows -2).
    u32 count = 0;
    if (cch_src < 0) {
        const u32 max_scan = 65536;
        while (count < max_scan) {
            auto r = m_memory.read_u16(src_ptr + count * 2);
            count++;
            if (r.is_err() || r.value() == 0) break;
        }
    } else {
        count = static_cast<u32>(cch_src);
    }
    if (count > 65536) count = 65536;

    enum {
        U_LEFT_TO_RIGHT = 0,
        U_RIGHT_TO_LEFT = 1,
        U_EUROPEAN_NUMBER = 2,
        U_EUROPEAN_NUMBER_SEPARATOR = 3,
        U_EUROPEAN_NUMBER_TERMINATOR = 4,
        U_ARABIC_NUMBER = 5,
        U_COMMON_NUMBER_SEPARATOR = 6,
        U_BLOCK_SEPARATOR = 7,
        U_SEGMENT_SEPARATOR = 8,
        U_WHITE_SPACE_NEUTRAL = 9,
        U_OTHER_NEUTRAL = 10,
        U_RIGHT_TO_LEFT_ARABIC = 13,
        U_DIR_NON_SPACING_MARK = 17,
        U_BOUNDARY_NEUTRAL = 18,
        U_NON_SPACING_MARK = 6,
        U_ENCLOSING_MARK = 7,
        U_MATH_SYMBOL = 24,
        U_CURRENCY_SYMBOL = 25,
        U_MODIFIER_SYMBOL = 26,
        U_OTHER_SYMBOL = 27,
        UCHAR_EAST_ASIAN_WIDTH = 0x1004,
        U_EA_HALFWIDTH = 2,
        U_EA_FULLWIDTH = 3,
        U_EA_NARROW = 4,
    };

    // CTYPE2 values are mutually exclusive (not a bitfield).
    auto classify_ctype2 = [](u16 ch) -> u16 {
        const int8_t d = u_charDirection(static_cast<int32_t>(ch));
        switch (d) {
        case U_LEFT_TO_RIGHT: return 0x0001;
        case U_RIGHT_TO_LEFT:
        case U_RIGHT_TO_LEFT_ARABIC: return 0x0002;
        case U_EUROPEAN_NUMBER: return 0x0003;
        case U_EUROPEAN_NUMBER_SEPARATOR: return 0x0004;
        case U_EUROPEAN_NUMBER_TERMINATOR: return 0x0005;
        case U_ARABIC_NUMBER: return 0x0006;
        case U_COMMON_NUMBER_SEPARATOR: return 0x0007;
        case U_BLOCK_SEPARATOR: return 0x0008;
        case U_SEGMENT_SEPARATOR: return 0x0009;
        case U_WHITE_SPACE_NEUTRAL: return 0x000A;
        case U_OTHER_NEUTRAL:
        case U_DIR_NON_SPACING_MARK:
        case U_BOUNDARY_NEUTRAL: return 0x000B;
        default: return 0x0000;
        }
    };

    auto classify_ctype3 = [](u16 ch) -> u16 {
        constexpr u16 C3_NONSPACING = 0x0001;
        constexpr u16 C3_DIACRITIC  = 0x0002;
        constexpr u16 C3_SYMBOL     = 0x0008;
        constexpr u16 C3_KATAKANA   = 0x0010;
        constexpr u16 C3_HIRAGANA   = 0x0020;
        constexpr u16 C3_HALFWIDTH  = 0x0040;
        constexpr u16 C3_FULLWIDTH  = 0x0080;
        constexpr u16 C3_IDEOGRAPH  = 0x0100;
        constexpr u16 C3_KASHIDA    = 0x0200;
        constexpr u16 C3_ALPHA      = 0x8000;
        u16 f = 0;
        const int8_t cat = u_charType(static_cast<int32_t>(ch));
        if (cat == U_NON_SPACING_MARK || cat == U_ENCLOSING_MARK) {
            f |= C3_NONSPACING;
            if (ch >= 0x0300 && ch <= 0x036F) f |= C3_DIACRITIC;
        }
        if (cat == U_MATH_SYMBOL || cat == U_CURRENCY_SYMBOL ||
            cat == U_MODIFIER_SYMBOL || cat == U_OTHER_SYMBOL)
            f |= C3_SYMBOL;
        if (ch >= 0x3040 && ch <= 0x309F) f |= C3_HIRAGANA;
        if ((ch >= 0x30A0 && ch <= 0x30FF) || (ch >= 0xFF66 && ch <= 0xFF9D))
            f |= C3_KATAKANA;
        // Windows: U+30A2 is FULLWIDTH|KATAKANA|ALPHA; hiragana has no width bit.
        if (ch >= 0x30A0 && ch <= 0x30FF) f |= C3_FULLWIDTH;
        if ((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF) ||
            (ch >= 0xF900 && ch <= 0xFAFF))
            f |= C3_IDEOGRAPH;
        if (ch == 0x0640) f |= C3_KASHIDA;
        const int32_t ea =
            u_getIntPropertyValue(static_cast<int32_t>(ch), UCHAR_EAST_ASIAN_WIDTH);
        if (ea == U_EA_FULLWIDTH || (ch >= 0xFF01 && ch <= 0xFF60))
            f |= C3_FULLWIDTH;
        else if (ea == U_EA_HALFWIDTH || ea == U_EA_NARROW ||
                 (ch >= 0x20 && ch <= 0x7E) || (ch >= 0xFF61 && ch <= 0xFF9F))
            f |= C3_HALFWIDTH;
        if (u_isUAlphabetic(static_cast<int32_t>(ch)) || (f & C3_IDEOGRAPH) ||
            (f & C3_HIRAGANA) || (f & C3_KATAKANA))
            f |= C3_ALPHA;
        return f;
    };

    for (u32 i = 0; i < count; ++i) {
        auto rc = m_memory.read_u16(src_ptr + i * 2);
        u16 ch = rc.is_err() ? u16(0) : rc.value();
        u16 out = 0;
        if (ctype == 1) out = codepage::ctype1_flags(ch);
        else if (ctype == 2) out = classify_ctype2(ch);
        else out = classify_ctype3(ch);
        (void)m_memory.write_u16(char_type_ptr + i * 2, out);
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_LCMapStringW(Marshaler& m) {
    // int LCMapStringW(LCID Locale, DWORD dwMapFlags, LPCWSTR lpSrcStr,
    //                  int cchSrc, LPWSTR lpDestStr, int cchDest)
    (void)static_cast<u32>(m.arg(0)); // Locale — linguistic maps use en_US ICU/CF
    u32 flags    = static_cast<u32>(m.arg(1));
    GuestAddr src_ptr  = m.arg_ptr(2);
    i32 cch_src  = static_cast<i32>(m.arg(3));
    GuestAddr dst_ptr  = m.arg_ptr(4);
    i32 cch_dest = static_cast<i32>(m.arg(5));

    VITREO_TRACE("[kernel32_ext] LCMapStringW(flags=0x%x, src=0x%x, cchSrc=%d, dst=0x%x, cchDest=%d)\n",
                 flags, static_cast<u32>(src_ptr), cch_src, static_cast<u32>(dst_ptr), cch_dest);

    m.set_return(static_cast<u64>(
        lcmap_string_w(m_memory, flags, src_ptr, cch_src, dst_ptr, cch_dest)));
}

} // namespace vitreo::winapi
