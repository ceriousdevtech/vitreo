// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_wctype.cpp
/// @brief Wide-character classification, and the wide ctype exports built on it
///        (iswalpha, iswspace, towlower, ...).
///
/// Split out of crt_string_registration.cpp. win_iswxdigit and win_wctype_bits
/// are declared in crt_wctype_internal.hpp because the strtol parser, iswxdigit
/// and iswctype (in sibling files) use them.

#include "winapi/msvcrt/text/crt_wctype_internal.hpp"
#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include <xlocale.h>
#include <locale.h>
#include <cwctype>
#include <cwchar>

namespace vitreo::winapi {

namespace {

// ---------------------------------------------------------------------------
// Wide character classification, measured against real Windows.
//
// Three separate behaviours, and they do NOT come from one locale:
//
//   * CLASSIFICATION is Unicode-aware regardless of the CRT locale. The host's
//     iswalpha in the "C" locale answers 0 for every non-ASCII character, so
//     delegating to it made E-acute, sharp-s and Greek Alpha all non-alpha.
//     A persistent UTF-8 locale reproduces Windows exactly here.
//   * CASE MAPPING is ASCII-only. Windows leaves E-acute, Greek Alpha and
//     fullwidth A unchanged; a UTF-8 locale would map all three, so the host
//     cannot be used for this half.
//   * iswxdigit is its own rule: ASCII hex digits plus their FULLWIDTH forms,
//     but not other scripts' digits. Windows accepts U+FF21 and rejects the
//     Arabic-Indic zero U+0660, which no single locale setting reproduces.
// ---------------------------------------------------------------------------
[[nodiscard]] locale_t unicode_ctype_locale() noexcept {
    static locale_t loc = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
    return loc;
}

} // namespace

[[nodiscard]] bool win_iswxdigit(u32 c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
        return true;
    return (c >= 0xFF10 && c <= 0xFF19) ||   // fullwidth 0-9
           (c >= 0xFF21 && c <= 0xFF26) ||   // fullwidth A-F
           (c >= 0xFF41 && c <= 0xFF46);     // fullwidth a-f
}

namespace {

[[nodiscard]] u32 win_towlower(u32 c) noexcept {
    return (c >= 'A' && c <= 'Z') ? c + 32u : c;
}
[[nodiscard]] u32 win_towupper(u32 c) noexcept {
    return (c >= 'a' && c <= 'z') ? c - 32u : c;
}

} // namespace

/// The MSVC ctype property bits for a wide character.
///
///   _UPPER 0x01  _LOWER 0x02  _DIGIT 0x04  _SPACE 0x08  _PUNCT 0x10
///   _CONTROL 0x20  _BLANK 0x40  _HEX 0x80  _ALPHA 0x100
///
/// iswctype(c, desc) is a bitwise AND against these, which is how mingw
/// implements iswalpha/iswdigit/iswspace/iswxdigit -- they are macros over
/// iswctype, so this one function decides all of them.
[[nodiscard]] u32 win_wctype_bits(u32 c) noexcept {
    const locale_t loc = unicode_ctype_locale();
    u32 b = 0;
    if (loc != nullptr) {
        if (iswupper_l(c, loc)) b |= 0x01u;
        if (iswlower_l(c, loc)) b |= 0x02u;
        if (iswdigit_l(c, loc)) b |= 0x04u;
        if (iswspace_l(c, loc)) b |= 0x08u;
        if (iswpunct_l(c, loc)) b |= 0x10u;
        if (iswcntrl_l(c, loc)) b |= 0x20u;
        if (iswalpha_l(c, loc)) b |= 0x100u;
    } else {
        // No UTF-8 locale available: fall back to ASCII rather than
        // classifying everything as nothing.
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') b |= 0x01u | 0x100u;
            if (c >= 'a' && c <= 'z') b |= 0x02u | 0x100u;
            if (c >= '0' && c <= '9') b |= 0x04u;
            if (c == ' ' || (c >= 0x09 && c <= 0x0D)) b |= 0x08u;
            if (c < 0x20 || c == 0x7F) b |= 0x20u;
        }
    }
    if (c == ' ' || c == '\t') b |= 0x40u;
    if (win_iswxdigit(c)) b |= 0x80u;
    return b;
}

void add_crt_wide_ctype_exports(DllExports& exports) {
    // ---------------------------------------------------------------------
    // Wide ctype helpers.
    // ---------------------------------------------------------------------
    // Windows `wint_t` is `unsigned short`. On x64 the high 16 bits of the
    // argument register are undefined after a 16-bit move, so classify only
    // the low 16 bits (Learn iswctype / wint_t).
    auto arg_wint = [](Marshaler& m) -> wint_t {
        return static_cast<wint_t>(static_cast<u16>(m.arg(0) & 0xFFFFu));
    };

    exports.entries.push_back(ExportEntry{
        "iswspace", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswspace_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswalpha", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswalpha_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    // int iswascii(wint_t c) — Microsoft extension, true for U+0000..U+007F.
    // Purely a range test: it does NOT consult the locale, so it must not go
    // through iswctype.
    exports.entries.push_back(ExportEntry{
        "iswascii", 0,
        [](Marshaler& m) {
            const u16 c = static_cast<u16>(m.arg(0) & 0xFFFFu);
            m.set_return(c <= 0x7Fu ? 1u : 0u);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswdigit", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswdigit_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswalnum", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswalnum_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswlower", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswlower_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswupper", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswupper_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswcntrl", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswcntrl_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswpunct", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswpunct_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    // iswgraph is "printable and not a space"; iswprint is "printable",
    // which includes the space. Both are separate msvcrt exports rather than
    // iswctype macros, so neither is covered by win_wctype_bits.
    exports.entries.push_back(ExportEntry{
        "iswgraph", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswgraph_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "iswprint", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(iswprint_l(arg_wint(m), unicode_ctype_locale()) ? 1 : 0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "towlower", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(win_towlower(static_cast<u32>(arg_wint(m))));
        }
    });
    exports.entries.push_back(ExportEntry{
        "towupper", 0,
        [arg_wint](Marshaler& m) {
            m.set_return(win_towupper(static_cast<u32>(arg_wint(m))));
        }
    });
}

} // namespace vitreo::winapi
