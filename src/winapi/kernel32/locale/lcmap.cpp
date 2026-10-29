// SPDX-License-Identifier: LGPL-2.1-only

/// @file lcmap.cpp
/// @brief LCMapStringW/Ex: Unicode case/title/width maps and ICU sort keys.
///
/// Case mapping uses CFString (independent of the C locale). Sort keys use
/// libicucore so memcmp order matches linguistic CompareString. Oracle:
/// tools/oracle_probe/lcmap_string_probe.expected.

#include "winapi/kernel32/locale/lcmap.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/guest_memory.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 LCMAP_LOWERCASE           = 0x00000100;
constexpr u32 LCMAP_UPPERCASE           = 0x00000200;
constexpr u32 LCMAP_TITLECASE           = 0x00000300;
constexpr u32 LCMAP_SORTKEY             = 0x00000400;
constexpr u32 LCMAP_BYTEREV             = 0x00000800;
constexpr u32 LCMAP_HIRAGANA            = 0x00100000;
constexpr u32 LCMAP_KATAKANA            = 0x00200000;
constexpr u32 LCMAP_HALFWIDTH           = 0x00400000;
constexpr u32 LCMAP_FULLWIDTH           = 0x00800000;
constexpr u32 LCMAP_LINGUISTIC_CASING   = 0x01000000;
constexpr u32 LCMAP_SIMPLIFIED_CHINESE  = 0x02000000;
constexpr u32 LCMAP_TRADITIONAL_CHINESE = 0x04000000;
constexpr u32 LCMAP_HASH                = 0x00040000;
constexpr u32 LCMAP_SORTHANDLE          = 0x20000000;

constexpr u32 NORM_IGNORECASE     = 0x00000001;
constexpr u32 NORM_IGNORENONSPACE = 0x00000002;
constexpr u32 NORM_IGNORESYMBOLS  = 0x00000004;
constexpr u32 SORT_DIGITSASNUMBERS = 0x00000008;
constexpr u32 SORT_STRINGSORT     = 0x00001000;
constexpr u32 NORM_IGNOREKANATYPE = 0x00010000;
constexpr u32 NORM_IGNOREWIDTH    = 0x00020000;

using UChar = u16;
using UErrorCode = i32;
struct UCollator;
enum {
    UCOL_PRIMARY = 0,
    UCOL_SECONDARY = 1,
    UCOL_TERTIARY = 2,
    UCOL_NUMERIC_COLLATION = 7,
    UCOL_ALTERNATE_HANDLING = 1,
    UCOL_NON_IGNORABLE = 0,
    UCOL_SHIFTED = 1,
    UCOL_ON = 17,
};

extern "C" {
UCollator* ucol_open(const char* loc, UErrorCode* status);
void ucol_close(UCollator* coll);
void ucol_setStrength(UCollator* coll, i32 strength);
void ucol_setAttribute(UCollator* coll, i32 attr, i32 value, UErrorCode* status);
i32 ucol_getSortKey(const UCollator* coll, const UChar* source, i32 sourceLength,
                    u8* result, i32 resultLength);
}

void log_lcmap_once(const char* which) {
    static std::mutex mu;
    static bool logged = false;
    std::lock_guard lock(mu);
    if (logged) return;
    logged = true;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!LCMapStringW: %s\n",
                 which);
}

void apply_byterev(std::vector<u16>& units) {
    for (u16& cu : units) {
        cu = static_cast<u16>((cu << 8) | (cu >> 8));
    }
}

bool apply_cf_map(std::vector<u16>& units, u32 flags) {
    CFMutableStringRef ms = CFStringCreateMutable(kCFAllocatorDefault, 0);
    if (!ms) {
        set_last_error(error_codes::ERROR_OUTOFMEMORY);
        return false;
    }
    if (!units.empty()) {
        CFStringAppendCharacters(ms,
                                 reinterpret_cast<const UniChar*>(units.data()),
                                 static_cast<CFIndex>(units.size()));
    }

    const u32 case_bits = flags & (LCMAP_UPPERCASE | LCMAP_LOWERCASE);
    if (case_bits == LCMAP_TITLECASE) {
        CFStringCapitalize(ms, nullptr);
    } else if (flags & LCMAP_UPPERCASE) {
        CFStringUppercase(ms, nullptr);
    } else if (flags & LCMAP_LOWERCASE) {
        CFStringLowercase(ms, nullptr);
    }

    if (flags & LCMAP_HIRAGANA) {
        (void)CFStringTransform(ms, nullptr, CFSTR("Katakana-Hiragana"), false);
    }
    if (flags & LCMAP_KATAKANA) {
        (void)CFStringTransform(ms, nullptr, CFSTR("Hiragana-Katakana"), false);
    }
    if (flags & LCMAP_HALFWIDTH) {
        (void)CFStringTransform(ms, nullptr, kCFStringTransformFullwidthHalfwidth,
                                false);
    }
    if (flags & LCMAP_FULLWIDTH) {
        (void)CFStringTransform(ms, nullptr, kCFStringTransformFullwidthHalfwidth,
                                true);
    }
    if (flags & LCMAP_SIMPLIFIED_CHINESE) {
        (void)CFStringTransform(ms, nullptr, CFSTR("Traditional-Simplified"),
                                false);
    }
    if (flags & LCMAP_TRADITIONAL_CHINESE) {
        (void)CFStringTransform(ms, nullptr, CFSTR("Simplified-Traditional"),
                                false);
    }
    (void)LCMAP_LINGUISTIC_CASING;

    const CFIndex n = CFStringGetLength(ms);
    units.resize(static_cast<usize>(n));
    if (n > 0) {
        CFStringGetCharacters(ms, CFRangeMake(0, n),
                              reinterpret_cast<UniChar*>(units.data()));
    }
    CFRelease(ms);
    return true;
}

std::vector<u8> make_sort_key(const std::vector<u16>& units, u32 flags) {
    UErrorCode st = 0;
    UCollator* col = ucol_open("en_US", &st);
    if (!col) return {};

    if (flags & NORM_IGNORENONSPACE) {
        ucol_setStrength(col, UCOL_PRIMARY);
    } else if (flags & NORM_IGNORECASE) {
        ucol_setStrength(col, UCOL_SECONDARY);
    } else {
        ucol_setStrength(col, UCOL_TERTIARY);
    }
    st = 0;
    if (flags & SORT_DIGITSASNUMBERS) {
        ucol_setAttribute(col, UCOL_NUMERIC_COLLATION, UCOL_ON, &st);
    }
    st = 0;
    if (flags & SORT_STRINGSORT) {
        ucol_setAttribute(col, UCOL_ALTERNATE_HANDLING, UCOL_NON_IGNORABLE, &st);
    } else {
        ucol_setAttribute(col, UCOL_ALTERNATE_HANDLING, UCOL_SHIFTED, &st);
    }
    (void)NORM_IGNORESYMBOLS;
    (void)NORM_IGNOREKANATYPE;
    (void)NORM_IGNOREWIDTH;

    std::vector<u8> key(units.size() * 8 + 16);
    const i32 n = ucol_getSortKey(col, units.data(),
                                  static_cast<i32>(units.size()), key.data(),
                                  static_cast<i32>(key.size()));
    ucol_close(col);
    if (n <= 0) return {};
    key.resize(static_cast<usize>(n));
    return key;
}

} // namespace

i32 lcmap_units(u32 flags, const u16* src, usize src_len,
                bool cch_src_was_minus_one, std::vector<u16>* mapped,
                std::vector<u8>* sort_key) {
    if (flags & (LCMAP_HASH | LCMAP_SORTHANDLE)) {
        log_lcmap_once("LCMAP_HASH/LCMAP_SORTHANDLE");
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }
    // SORTKEY cannot combine with string-mapping transforms (Windows 0x3ec).
    if ((flags & LCMAP_SORTKEY) &&
        (flags & (LCMAP_UPPERCASE | LCMAP_LOWERCASE | LCMAP_HIRAGANA |
                  LCMAP_KATAKANA | LCMAP_HALFWIDTH | LCMAP_FULLWIDTH |
                  LCMAP_SIMPLIFIED_CHINESE | LCMAP_TRADITIONAL_CHINESE |
                  LCMAP_LINGUISTIC_CASING))) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }
    if ((flags & LCMAP_HIRAGANA) && (flags & LCMAP_KATAKANA)) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }
    if ((flags & LCMAP_HALFWIDTH) && (flags & LCMAP_FULLWIDTH)) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }

    std::vector<u16> units(src, src + src_len);

    if (flags & LCMAP_SORTKEY) {
        auto key = make_sort_key(units, flags);
        if (key.empty()) {
            set_last_error(error_codes::ERROR_OUTOFMEMORY);
            return 0;
        }
        if (flags & LCMAP_BYTEREV) {
            const usize even = key.size() & ~usize{1};
            for (usize i = 0; i + 1 < even; i += 2) {
                std::swap(key[i], key[i + 1]);
            }
        }
        if (sort_key) *sort_key = std::move(key);
        return static_cast<i32>(sort_key ? sort_key->size() : 0);
    }

    const bool needs_cf =
        (flags & (LCMAP_UPPERCASE | LCMAP_LOWERCASE | LCMAP_HIRAGANA |
                  LCMAP_KATAKANA | LCMAP_HALFWIDTH | LCMAP_FULLWIDTH |
                  LCMAP_SIMPLIFIED_CHINESE | LCMAP_TRADITIONAL_CHINESE)) != 0;
    if (needs_cf && !apply_cf_map(units, flags)) return 0;
    if (flags & LCMAP_BYTEREV) apply_byterev(units);

    if (cch_src_was_minus_one) units.push_back(0);
    if (mapped) *mapped = std::move(units);
    return static_cast<i32>(mapped ? mapped->size() : 0);
}

i32 lcmap_string_w(GuestMemory& memory, u32 flags, GuestAddr src, i32 cch_src,
                   GuestAddr dst, i32 cch_dest) {
    if (src == 0 || cch_src == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    std::vector<u16> raw;
    bool minus_one = false;
    if (cch_src < 0) {
        minus_one = true;
        for (i32 i = 0; i < 65536; ++i) {
            auto r = memory.read_u16(src + static_cast<u64>(i) * 2);
            if (!r) break;
            const u16 cu = static_cast<u16>(r.value());
            if (cu == 0) break;
            raw.push_back(cu);
        }
    } else {
        raw.resize(static_cast<usize>(cch_src));
        for (i32 i = 0; i < cch_src; ++i) {
            auto r = memory.read_u16(src + static_cast<u64>(i) * 2);
            raw[static_cast<usize>(i)] =
                r.is_ok() ? static_cast<u16>(r.value()) : 0;
        }
    }

    std::vector<u16> mapped;
    std::vector<u8> key;
    const i32 needed =
        lcmap_units(flags, raw.data(), raw.size(), minus_one,
                    (flags & LCMAP_SORTKEY) ? nullptr : &mapped,
                    (flags & LCMAP_SORTKEY) ? &key : nullptr);
    if (needed <= 0) return 0;

    if (flags & LCMAP_SORTKEY) {
        if (dst == 0 || cch_dest == 0) return needed;
        if (cch_dest < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (i32 i = 0; i < needed; ++i) {
            (void)memory.write_u8(dst + static_cast<u64>(i),
                                  key[static_cast<usize>(i)]);
        }
        return needed;
    }

    if (dst == 0 || cch_dest == 0) return needed;
    if (cch_dest < needed) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i32 i = 0; i < needed; ++i) {
        (void)memory.write_u16(dst + static_cast<u64>(i) * 2,
                               mapped[static_cast<usize>(i)]);
    }
    return needed;
}

} // namespace vitreo::winapi
