// SPDX-License-Identifier: LGPL-2.1-only

/// @file locale_string.cpp
/// @brief Locale, string comparison, and SRW lock stubs — implementation

#include "winapi/kernel32/locale/locale_string.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/locale/locale_string_internal.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

namespace {

void log_compare_flag_once(const char* which) {
    static std::mutex mu;
    static bool logged = false;
    std::lock_guard lock(mu);
    if (logged) return;
    logged = true;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED kernel32!CompareStringW: %s\n", which);
}

[[nodiscard]] bool is_word_soft(u16 cu) noexcept {
    // Word sort: hyphen and apostrophe are soft (Learn CompareString).
    return cu == 0x002Du || cu == 0x0027u;
}

[[nodiscard]] bool is_ignore_symbol(u16 cu) noexcept {
    CFCharacterSetRef punct =
        CFCharacterSetGetPredefined(kCFCharacterSetPunctuation);
    CFCharacterSetRef symbol =
        CFCharacterSetGetPredefined(kCFCharacterSetSymbol);
    return CFCharacterSetIsCharacterMember(punct, cu) ||
           CFCharacterSetIsCharacterMember(symbol, cu);
}

[[nodiscard]] std::vector<u16> read_guest_wide(Marshaler& m, GuestAddr ptr,
                                               i32 len) {
    std::vector<u16> out;
    if (ptr == 0) return out;
    auto& mem = m.memory();
    if (len < 0) {
        for (i32 i = 0; i < 65536; ++i) {
            auto r = mem.read_u16(ptr + static_cast<u64>(i) * 2);
            if (!r) break;
            const u16 cu = static_cast<u16>(r.value());
            if (cu == 0) break;
            out.push_back(cu);
        }
        return out;
    }
    out.resize(static_cast<usize>(len));
    for (i32 i = 0; i < len; ++i) {
        auto r = mem.read_u16(ptr + static_cast<u64>(i) * 2);
        out[static_cast<usize>(i)] =
            r.is_ok() ? static_cast<u16>(r.value()) : 0;
    }
    return out;
}

[[nodiscard]] std::vector<u16> read_guest_ansi_as_wide(Marshaler& m,
                                                       GuestAddr ptr,
                                                       i32 len) {
    std::vector<u8> bytes;
    if (ptr == 0) return {};
    auto& mem = m.memory();
    if (len < 0) {
        for (i32 i = 0; i < 65536; ++i) {
            auto r = mem.read_u8(ptr + static_cast<u64>(i));
            if (!r) break;
            const u8 b = static_cast<u8>(r.value());
            if (b == 0) break;
            bytes.push_back(b);
        }
    } else {
        bytes.resize(static_cast<usize>(len));
        for (i32 i = 0; i < len; ++i) {
            auto r = mem.read_u8(ptr + static_cast<u64>(i));
            bytes[static_cast<usize>(i)] =
                r.is_ok() ? static_cast<u8>(r.value()) : 0;
        }
    }
    std::vector<u16> wide(bytes.size() + 1);
    auto conv = codepage::multi_byte_to_wide(
        codepage::kCpAcp, 0, bytes.data(), bytes.size(), wide.data(),
        wide.size());
    if (!conv.ok()) return {};
    wide.resize(conv.count);
    return wide;
}

[[nodiscard]] std::vector<u16> filter_units(const std::vector<u16>& in,
                                            bool drop_soft, bool drop_symbols) {
    if (!drop_soft && !drop_symbols) return in;
    std::vector<u16> out;
    out.reserve(in.size());
    for (u16 cu : in) {
        if (drop_soft && is_word_soft(cu)) continue;
        if (drop_symbols && is_ignore_symbol(cu)) continue;
        out.push_back(cu);
    }
    return out;
}

[[nodiscard]] bool has_word_soft(const std::vector<u16>& in) noexcept {
    for (u16 cu : in) {
        if (is_word_soft(cu)) return true;
    }
    return false;
}

[[nodiscard]] i32 cstr_from_cf(CFComparisonResult r) noexcept {
    if (r < 0) return CSTR_LESS_THAN;
    if (r > 0) return CSTR_GREATER_THAN;
    return CSTR_EQUAL;
}

[[nodiscard]] i32 cf_compare_units(const std::vector<u16>& a,
                                   const std::vector<u16>& b,
                                   CFOptionFlags opts) {
    CFStringRef sa = CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(a.data()),
        static_cast<CFIndex>(a.size()));
    CFStringRef sb = CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(b.data()),
        static_cast<CFIndex>(b.size()));
    if (!sa || !sb) {
        if (sa) CFRelease(sa);
        if (sb) CFRelease(sb);
        set_last_error(error_codes::ERROR_OUTOFMEMORY);
        return 0;
    }
    const CFComparisonResult r =
        CFStringCompare(sa, sb, opts | kCFCompareLocalized);
    CFRelease(sa);
    CFRelease(sb);
    return cstr_from_cf(r);
}

[[nodiscard]] CFOptionFlags cf_options_from_flags(u32 flags) {
    CFOptionFlags opts = 0;
    if (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) {
        opts |= kCFCompareCaseInsensitive;
    }
    if (flags & (NORM_IGNORENONSPACE | LINGUISTIC_IGNOREDIACRITIC)) {
        opts |= kCFCompareDiacriticInsensitive;
    }
    if (flags & NORM_IGNOREWIDTH) {
        opts |= kCFCompareWidthInsensitive;
    }
    if (flags & SORT_DIGITSASNUMBERS) {
        opts |= kCFCompareNumerically;
    }
    return opts;
}

[[nodiscard]] i32 linguistic_compare_units(const std::vector<u16>& s1,
                                           const std::vector<u16>& s2,
                                           u32 flags) {
    if (flags & ~kCompareStringValidFlags) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }
    if (flags & NORM_IGNOREKANATYPE) {
        log_compare_flag_once("NORM_IGNOREKANATYPE (no kana folding)");
    }

    const CFOptionFlags opts = cf_options_from_flags(flags);
    const bool ignore_symbols = (flags & NORM_IGNORESYMBOLS) != 0;
    const bool string_sort = (flags & SORT_STRINGSORT) != 0;

    // Word sort (default): hyphen/apostrophe are soft — ignored for the
    // primary key; when keys tie, the form without soft chars sorts first
    // (Windows CompareStringW: coop < co-op, bills < bill's).
    if (!string_sort) {
        auto p1 = filter_units(s1, /*drop_soft=*/true, ignore_symbols);
        auto p2 = filter_units(s2, /*drop_soft=*/true, ignore_symbols);
        const i32 primary = cf_compare_units(p1, p2, opts);
        if (primary == 0) return 0;
        if (primary != CSTR_EQUAL) return primary;
        const bool soft1 = has_word_soft(s1);
        const bool soft2 = has_word_soft(s2);
        if (soft1 != soft2) {
            return soft1 ? CSTR_GREATER_THAN : CSTR_LESS_THAN;
        }
    }

    auto a = filter_units(s1, /*drop_soft=*/false, ignore_symbols);
    auto b = filter_units(s2, /*drop_soft=*/false, ignore_symbols);
    return cf_compare_units(a, b, opts);
}

} // namespace

// ---------------------------------------------------------------------------
// LocaleStringManager method bodies
// ---------------------------------------------------------------------------

u32 LocaleStringManager::get_acp() const {
    return codepage::ansi_code_page();
}

i32 LocaleStringManager::lstrlen_w(Marshaler& m, GuestAddr ptr) const {
    if (ptr == 0) return 0;

    i32 len = 0;
    auto& mem = m.memory();
    for (;;) {
        auto r = mem.read_u16(ptr + static_cast<u64>(len) * 2);
        if (!r || r.value() == 0) break;
        ++len;
        if (len > 65535) break; // safety cap
    }
    return len;
}

i32 LocaleStringManager::compare_string_a(Marshaler& m, u32 /*locale*/, u32 flags,
                                           GuestAddr str1, i32 len1,
                                           GuestAddr str2, i32 len2) const {
    if (str1 == 0 || str2 == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    auto s1 = read_guest_ansi_as_wide(m, str1, len1);
    auto s2 = read_guest_ansi_as_wide(m, str2, len2);
    return linguistic_compare_units(s1, s2, flags);
}

i32 LocaleStringManager::compare_units(const std::vector<u16>& s1, const std::vector<u16>& s2, u32 flags) {
    return linguistic_compare_units(s1, s2, flags);
}

i32 LocaleStringManager::compare_string_w(Marshaler& m, u32 /*locale*/, u32 flags,
                                           GuestAddr str1, i32 len1,
                                           GuestAddr str2, i32 len2) const {
    if (str1 == 0 || str2 == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    auto s1 = read_guest_wide(m, str1, len1);
    auto s2 = read_guest_wide(m, str2, len2);
    return linguistic_compare_units(s1, s2, flags);
}

namespace {

constexpr u32 TIME_NOMINUTESORSECONDS = 0x00000001;
constexpr u32 TIME_NOSECONDS          = 0x00000002;
constexpr u32 TIME_NOTIMEMARKER       = 0x00000004;
constexpr u32 TIME_FORCE24HOURFORMAT  = 0x00000008;

void append_padded(std::string& out, u32 value, int min_digits) {
    std::string digits = std::to_string(value);
    while (static_cast<int>(digits.size()) < min_digits) digits.insert(0, "0");
    out += digits;
}

/// Expand a Windows time picture string. Supports the documented h/H/m/s/t
/// groups and single-quoted literals; anything else is copied verbatim.
std::string format_time(const std::string& picture, u32 hour, u32 minute,
                        u32 second) {
    const bool is_pm = hour >= 12;
    u32 hour12 = hour % 12;
    if (hour12 == 0) hour12 = 12;

    std::string out;
    usize i = 0;
    while (i < picture.size()) {
        const char c = picture[i];
        if (c == '\'') {
            ++i;
            while (i < picture.size()) {
                if (picture[i] == '\'') {
                    if (i + 1 < picture.size() && picture[i + 1] == '\'') {
                        out += '\'';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                out += picture[i++];
            }
            continue;
        }
        if (c != 'h' && c != 'H' && c != 'm' && c != 's' && c != 't') {
            out += c;
            ++i;
            continue;
        }
        usize run = 0;
        while (i + run < picture.size() && picture[i + run] == c) ++run;
        // Learn: picture runs longer than two default to two digits.
        const int digits = (c == 't') ? static_cast<int>(run)
                                      : static_cast<int>(std::min(run, usize{2}));
        switch (c) {
        case 'h': append_padded(out, hour12, digits); break;
        case 'H': append_padded(out, hour, digits); break;
        case 'm': append_padded(out, minute, digits); break;
        case 's': append_padded(out, second, digits); break;
        case 't':
            // 't' is the first letter of the marker, 'tt' the whole marker.
            out += (run == 1) ? (is_pm ? "P" : "A") : (is_pm ? "PM" : "AM");
            break;
        default:
            break;
        }
        i += run;
    }
    return out;
}

} // namespace

i32 LocaleStringManager::get_time_format(Marshaler& m, bool wide,
                                          u32 /*locale*/, u32 flags,
                                          GuestAddr time_ptr,
                                          GuestAddr fmt_ptr,
                                          GuestAddr buf, i32 buf_size) const {
    if (buf_size < 0 || (buf_size > 0 && buf == 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // SYSTEMTIME: wHour at +8, wMinute at +10, wSecond at +12.
    // A null lpTime means "use the current local time".
    u32 hour = 0, minute = 0, second = 0;
    if (time_ptr != 0) {
        auto h  = m.memory().read_u16(time_ptr + 8);
        auto mn = m.memory().read_u16(time_ptr + 10);
        auto s  = m.memory().read_u16(time_ptr + 12);
        if (h.is_err() || mn.is_err() || s.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        hour = h.value();
        minute = mn.value();
        second = s.value();
        if (hour > 23 || minute > 59 || second > 59) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
    } else {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        hour = static_cast<u32>(local.tm_hour);
        minute = static_cast<u32>(local.tm_min);
        second = static_cast<u32>(local.tm_sec);
    }

    // An explicit picture wins; otherwise the flags trim the en-US default.
    std::string picture;
    if (fmt_ptr != 0) {
        auto fmt = wide ? m.read_wide_string(fmt_ptr) : m.read_ansi_string(fmt_ptr);
        if (fmt.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        picture = fmt.value();
    } else {
        const bool force_24 = (flags & TIME_FORCE24HOURFORMAT) != 0;
        picture = force_24 ? "HH" : "h";
        if ((flags & TIME_NOMINUTESORSECONDS) == 0) {
            picture += ":mm";
            if ((flags & TIME_NOSECONDS) == 0) picture += ":ss";
        }
        // Learn: TIME_FORCE24HOURFORMAT still shows the marker unless
        // TIME_NOTIMEMARKER is also set (Windows / MSDN).
        if ((flags & TIME_NOTIMEMARKER) == 0) picture += " tt";
    }

    const std::string formatted = format_time(picture, hour, minute, second);
    const i32 required = wide
        ? static_cast<i32>(utf8_to_utf16(formatted).size() + 1)
        : static_cast<i32>(formatted.size() + 1);

    if (buf_size == 0) {
        return required; // size query
    }
    if (buf_size < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const auto written = wide ? m.write_wide_string(buf, formatted)
                              : m.write_ansi_string(buf, formatted);
    if (written.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    return required;
}

// ---------------------------------------------------------------------------
// GetLocaleInfoW
// ---------------------------------------------------------------------------

namespace {

struct Profile {
    u32 lcid;
    const char16_t* name;
    const char16_t* lang_id;
    const char16_t* lang_name;
    const char16_t* native_lang;
    const char16_t* eng_lang;
    const char16_t* eng_country;
    const char16_t* iso639;
    const char16_t* iso3166;
    const char16_t* parent;
    const char16_t* decimal;
    const char16_t* thousand;
    const char16_t* currency;
};

// Profiles for every LCID EnumSystemLocales / IsValidLocale advertise.
// LOCALE_SDECIMAL / LOCALE_STHOUSAND are Windows 11's
// (getnumberformat_probe64.expected gnfex-lcinfo-*: fr-FR groups with
// U+202F NARROW NO-BREAK SPACE, ru/pl/sv/uk/cs with U+00A0).
static const Profile kProfiles[] = {
    {0x0409, u"en-US", u"0409", u"English (United States)", u"English",
     u"English", u"United States", u"en", u"US", u"en", u".", u",", u"$"},
    {0x0809, u"en-GB", u"0809", u"English (United Kingdom)", u"English",
     u"English", u"United Kingdom", u"en", u"GB", u"en", u".", u",", u"£"},
    {0x0407, u"de-DE", u"0407", u"German (Germany)", u"Deutsch",
     u"German", u"Germany", u"de", u"DE", u"de", u",", u".", u"€"},
    {0x040C, u"fr-FR", u"040C", u"French (France)", u"français",
     u"French", u"France", u"fr", u"FR", u"fr", u",", u"\u202F", u"€"},
    {0x0C0A, u"es-ES", u"0C0A", u"Spanish (Spain)", u"español",
     u"Spanish", u"Spain", u"es", u"ES", u"es", u",", u".", u"€"},
    {0x0410, u"it-IT", u"0410", u"Italian (Italy)", u"italiano",
     u"Italian", u"Italy", u"it", u"IT", u"it", u",", u".", u"€"},
    {0x0416, u"pt-BR", u"0416", u"Portuguese (Brazil)", u"português",
     u"Portuguese", u"Brazil", u"pt", u"BR", u"pt", u",", u".", u"R$"},
    {0x0413, u"nl-NL", u"0413", u"Dutch (Netherlands)", u"Nederlands",
     u"Dutch", u"Netherlands", u"nl", u"NL", u"nl", u",", u".", u"€"},
    {0x0411, u"ja-JP", u"0411", u"Japanese (Japan)", u"日本語",
     u"Japanese", u"Japan", u"ja", u"JP", u"ja", u".", u",", u"¥"},
    {0x0804, u"zh-CN", u"0804", u"Chinese (Simplified, China)", u"中文",
     u"Chinese", u"China", u"zh", u"CN", u"zh", u".", u",", u"¥"},
    {0x0412, u"ko-KR", u"0412", u"Korean (Korea)", u"한국어",
     u"Korean", u"Korea", u"ko", u"KR", u"ko", u".", u",", u"₩"},
    {0x0419, u"ru-RU", u"0419", u"Russian (Russia)", u"русский",
     u"Russian", u"Russia", u"ru", u"RU", u"ru", u",", u"\u00A0", u"₽"},
    {0x0404, u"zh-TW", u"0404", u"Chinese (Traditional, Taiwan)", u"中文",
     u"Chinese", u"Taiwan", u"zh", u"TW", u"zh", u".", u",", u"NT$"},
    {0x0415, u"pl-PL", u"0415", u"Polish (Poland)", u"polski",
     u"Polish", u"Poland", u"pl", u"PL", u"pl", u",", u"\u00A0", u"zł"},
    {0x041D, u"sv-SE", u"041D", u"Swedish (Sweden)", u"svenska",
     u"Swedish", u"Sweden", u"sv", u"SE", u"sv", u",", u"\u00A0", u"kr"},
    {0x041F, u"tr-TR", u"041F", u"Turkish (Turkey)", u"Türkçe",
     u"Turkish", u"Turkey", u"tr", u"TR", u"tr", u",", u".", u"₺"},
    {0x0422, u"uk-UA", u"0422", u"Ukrainian (Ukraine)", u"українська",
     u"Ukrainian", u"Ukraine", u"uk", u"UA", u"uk", u",", u"\u00A0", u"₴"},
    {0x0405, u"cs-CZ", u"0405", u"Czech (Czechia)", u"čeština",
     u"Czech", u"Czechia", u"cs", u"CZ", u"cs", u",", u"\u00A0", u"Kč"},
};
static const Profile kInvariant = {
    0x007F, u"", u"007F", u"Invariant Language (Invariant Country)",
    u"Invariant Language", u"Invariant Language", u"Invariant Country",
    u"", u"", u"", u".", u",", u""};

/// The profile GetLocaleInfoW answers for @p locale: LOCALE_USER_DEFAULT,
/// LOCALE_SYSTEM_DEFAULT and LOCALE_NEUTRAL are en-US; null for an LCID no
/// profile covers (the caller's ERROR_INVALID_PARAMETER).
const Profile* find_locale_profile(u32 locale) {
    if (locale == 0 || locale == 0x0400 || locale == 0x0800) return &kProfiles[0];
    if (locale == 0x007F) return &kInvariant;
    for (const auto& p : kProfiles) {
        if (p.lcid == locale) return &p;
    }
    return nullptr;
}

constexpr char16_t ascii_lower(char16_t c) {
    return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c - u'A' + u'a') : c;
}

/// True when @p a and @p b are the same name, ASCII case folded and '_' read
/// as '-' at every position: between language and region (gnfex-grammar-de_DE,
/// -DE-de, gnfex-name-EN-us, -en_US), before a sort (gnfex-fix1-name-de-DE-phoneb,
/// -es_ES_tradnl, -de_DE_phoneb) and inside LOCALE_NAME_SYSTEM_DEFAULT
/// (gnfex-fix1-name-sys-underscores: SNAME en-US).
bool locale_name_equal(std::u16string_view a, std::u16string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const char16_t x = a[i] == u'_' ? u'-' : ascii_lower(a[i]);
        const char16_t y = b[i] == u'_' ? u'-' : ascii_lower(b[i]);
        if (x != y) return false;
    }
    return true;
}

}  // namespace

std::optional<u32> LocaleStringManager::locale_name_profile_lcid(std::u16string_view name) const {
    // LOCALE_NAME_INVARIANT (gnfex-name-invariant), LOCALE_NAME_SYSTEM_DEFAULT
    // in any case (gnfex-name-system-default, -SYS-upper).
    if (name.empty()) return 0x007Fu;
    if (locale_name_equal(name, u"!x-sys-default-locale")) return 0x0800u;
    // The alternate sorts measured on a profile's locale keep its number
    // format (gnfex-grammar-de-DE_PHONEB, -es-ES_tradnl, -ja-JP_radstr); an
    // unknown sort name is rejected (gnfex-grammar-de-DE_phonebook).
    struct Sort { const char16_t* name; u32 lcid; };
    static const Sort kSorts[] = {
        {u"de-DE_phoneb", 0x0407}, {u"es-ES_tradnl", 0x0C0A}, {u"ja-JP_radstr", 0x0411},
    };
    for (const Sort& s : kSorts) {
        if (locale_name_equal(name, s.name)) return s.lcid;
    }
    // Names whose every number field the oracle shows equal to a profile's:
    // the neutral en / de (gnfex-lcinfo-en, -de, -EN, -De) and zh-Hans-CN,
    // which Windows resolves to SNAME zh-CN (gnfex-lcinfo-zh-Hans-CN,
    // -zh-hans-cn, -zh_Hans_CN). No other alias is measured.
    static const Sort kAliases[] = {
        {u"en", 0x0409}, {u"de", 0x0407}, {u"zh-Hans-CN", 0x0804},
    };
    for (const Sort& s : kAliases) {
        if (locale_name_equal(name, s.name)) return s.lcid;
    }
    for (const Profile& p : kProfiles) {
        if (locale_name_equal(name, p.name)) return p.lcid;
    }
    return std::nullopt;
}

namespace {

bool ascii_alpha(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
bool ascii_digit(char16_t c) { return c >= u'0' && c <= u'9'; }
bool all_of_class(std::u16string_view t, bool (*pred)(char16_t)) {
    for (char16_t c : t) {
        if (!pred(c)) return false;
    }
    return true;
}
bool ascii_alnum(char16_t c) { return ascii_alpha(c) || ascii_digit(c); }

}  // namespace

bool locale_name_well_formed(std::u16string_view name) {
    if (name.empty() || name.size() >= 85) return false;   // LOCALE_NAME_MAX_LENGTH counts the NUL
    std::vector<std::u16string_view> tags;
    usize start = 0;
    for (usize i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == u'-' || name[i] == u'_') {
            const std::u16string_view t = name.substr(start, i - start);
            // Empty subtags (en-, -en, en--US) and subtags past 8 characters
            // (abcdefghi, en-x-aaaaaaaaa, _phonebook) are malformed.
            if (t.empty() || t.size() > 8 || !all_of_class(t, ascii_alnum)) return false;
            tags.push_back(t);
            start = i + 1;
        }
    }
    auto singleton = [](std::u16string_view t) { return t.size() == 1 && ascii_alnum(t[0]); };
    usize i = 0;
    // x-foo, x-aaaaaaaa, i-klingon: a singleton first takes one or more subtags.
    if (!singleton(tags[0])) {
        // Language: 2-3 letters (abc yes, abcd and e no).
        if (tags[0].size() < 2 || tags[0].size() > 3 || !all_of_class(tags[0], ascii_alpha)) return false;
        i = 1;
        // Script: 4 letters (en-abcd, en-Latn).
        if (i < tags.size() && tags[i].size() == 4 && all_of_class(tags[i], ascii_alpha)) ++i;
        // Region: 2 letters or 3 digits (en-123 yes, en-12 no).
        if (i < tags.size() && ((tags[i].size() == 2 && all_of_class(tags[i], ascii_alpha)) ||
                                (tags[i].size() == 3 && all_of_class(tags[i], ascii_digit)))) {
            // After a region any 1-8 character subtags follow: variants
            // (en-US-POSIX, en-GB-oed, en-US-aaaaa-bbbbb), sorts, extensions.
            return true;
        }
        // Without a region only extensions follow (en-u-nu-thai, en-a-b,
        // en-x-aaaaaaaa); a variant or a 3-letter subtag directly after the
        // language is malformed (en-aaaaa, en-1abc, en-abc, xx-YYY).
        if (i == tags.size()) return true;
        if (!singleton(tags[i])) return false;
    }
    // A singleton needs at least one subtag after it (x alone is malformed);
    // what follows is any 1-8 character subtags (en-a-b, en-u-nu-thai).
    return i + 1 < tags.size();
}


i32 LocaleStringManager::get_locale_info_w(Marshaler& m, u32 locale, u32 lc_type,
                                            GuestAddr buf, i32 cch_buf) const {
    // Modifier bits may combine with any LCType (Learn GetLocaleInfoEx).
    constexpr u32 LOCALE_RETURN_NUMBER         = 0x20000000;
    constexpr u32 LOCALE_RETURN_GENITIVE_NAMES = 0x10000000;
    constexpr u32 LOCALE_USE_CP_ACP            = 0x40000000;
    constexpr u32 LOCALE_NOUSEROVERRIDE        = 0x80000000;
    constexpr u32 kLocaleModifiers =
        LOCALE_RETURN_NUMBER | LOCALE_RETURN_GENITIVE_NAMES |
        LOCALE_USE_CP_ACP | LOCALE_NOUSEROVERRIDE;

    bool return_number = (lc_type & LOCALE_RETURN_NUMBER) != 0;
    u32 type = lc_type & ~kLocaleModifiers;

    const Profile* prof = find_locale_profile(locale);
    if (prof == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    static const char16_t kAnsiCp[]    = u"1252";
    static const char16_t kOemCp[]     = u"437";
    static const char16_t kMacCp[]     = u"10000";
    static const char16_t kGrouping[]  = u"3;0";
    static const char16_t kShortDate[] = u"M/d/yyyy";
    static const char16_t kLongDate[]  = u"dddd, MMMM d, yyyy";
    static const char16_t kTimeFmt[]   = u"h:mm:ss tt";
    static const char16_t kAm[]        = u"AM";
    static const char16_t kPm[]        = u"PM";
    static const char16_t kNegSign[]   = u"-";
    static const char16_t kFirstDow[]  = u"6";
    static const char16_t kMeasure[]   = u"1"; // US customary
    static const char16_t kDays[][16] = {
        u"Monday", u"Tuesday", u"Wednesday", u"Thursday",
        u"Friday", u"Saturday", u"Sunday",
    };
    static const char16_t kMonths[][16] = {
        u"January", u"February", u"March", u"April", u"May", u"June",
        u"July", u"August", u"September", u"October", u"November",
        u"December", u"",
    };

    const char16_t* value = nullptr;
    i32 value_len = 0;
    auto set_lit = [&](const char16_t* s) {
        value = s;
        value_len = 1;
        while (s[value_len - 1] != 0) ++value_len;
    };

    switch (type) {
    case 0x01: set_lit(prof->lang_id); break;
    case 0x02: set_lit(prof->lang_name); break;
    case 0x04: set_lit(prof->native_lang); break;
    case 0x0B: set_lit(kOemCp); break;
    case 0x0D: set_lit(kMeasure); break;      // LOCALE_IMEASURE
    case 0x0E: set_lit(prof->decimal); break;
    case 0x0F: set_lit(prof->thousand); break;
    case 0x10: set_lit(kGrouping); break;
    case 0x14: set_lit(prof->currency); break;
    case 0x1F: set_lit(kShortDate); break;
    case 0x20: set_lit(kLongDate); break;
    case 0x28: set_lit(kAm); break;
    case 0x29: set_lit(kPm); break;
    case 0x2A: case 0x2B: case 0x2C: case 0x2D:
    case 0x2E: case 0x2F: case 0x30:
        set_lit(kDays[type - 0x2A]); break;
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43:
        set_lit(kMonths[type - 0x38]); break;
    case 0x51: set_lit(kNegSign); break;
    case 0x59: set_lit(prof->iso639); break;
    case 0x5A: set_lit(prof->iso3166); break;
    case 0x5C: set_lit(prof->name); break;
    case 0x6D: set_lit(prof->parent); break;
    case 0x1001: set_lit(prof->eng_lang); break;
    case 0x1002: set_lit(prof->eng_country); break;
    case 0x1003: set_lit(kTimeFmt); break;
    case 0x1004: set_lit(kAnsiCp); break;
    case 0x100C: set_lit(kFirstDow); break;
    case 0x100E: set_lit(kMonths[12]); break;
    case 0x1011: set_lit(kMacCp); break;
    default:
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }

    if (return_number) {
        // Only integer LCTypes accept LOCALE_RETURN_NUMBER (Windows:
        // SDECIMAL/SNAME → ERROR_INVALID_FLAGS). ILANGUAGE is hex; others
        // that store decimal digit strings parse as decimal.
        const bool int_type =
            type == 0x01 || type == 0x0B || type == 0x0D || type == 0x1004 ||
            type == 0x100C || type == 0x1011;
        if (!int_type) {
            set_last_error(error_codes::ERROR_INVALID_FLAGS);
            return 0;
        }
        if (buf == 0 || cch_buf == 0) return 2;
        if (cch_buf < 2) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        u32 num_val = 0;
        if (type == 0x01) {
            // LOCALE_ILANGUAGE — hex string "0409" → 0x0409.
            for (i32 i = 0; value[i] != 0; ++i) {
                const char16_t c = value[i];
                u32 dig = 0;
                if (c >= u'0' && c <= u'9') dig = static_cast<u32>(c - u'0');
                else if (c >= u'a' && c <= u'f') dig = 10 + static_cast<u32>(c - u'a');
                else if (c >= u'A' && c <= u'F') dig = 10 + static_cast<u32>(c - u'A');
                else break;
                num_val = (num_val << 4) | dig;
            }
        } else {
            for (i32 i = 0; value[i] != 0; ++i) {
                if (value[i] < u'0' || value[i] > u'9') break;
                num_val = num_val * 10 + (static_cast<u32>(value[i]) - u'0');
            }
        }
        (void)m.memory().write_u32(buf, num_val);
        return 2;
    }

    if (buf == 0 || cch_buf == 0) {
        return value_len;
    }

    if (cch_buf < value_len) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i32 i = 0; i < value_len; ++i) {
        (void)m.memory().write_u16(buf + static_cast<u64>(i) * 2,
                                   static_cast<u16>(value[i]));
    }
    return value_len;
}

bool LocaleStringManager::number_format_defaults(u32 locale, NumberFormatSpec& out) const {
    const Profile* prof = find_locale_profile(locale);
    if (prof == nullptr) return false;
    out = NumberFormatSpec{};
    out.decimal = prof->decimal;
    out.thousand = prof->thousand;
    return true;
}

i32 LocaleStringManager::get_locale_info_ex(Marshaler& m,
                                            GuestAddr locale_name,
                                            u32 lc_type,
                                            GuestAddr buf,
                                            i32 cch_buf) const {
    // NULL name → user-default locale (en-US). Named locales resolve by
    // matching LOCALE_SNAME; unknown BCP-47 tags still succeed for SNAME
    // (Windows echoes the tag) and fall back to en-US for other types.
    if (locale_name == 0) {
        return get_locale_info_w(m, 0x0409, lc_type, buf, cch_buf);
    }
    auto name = m.read_wide_string(locale_name).value_or("");
    // Case-insensitive match against known profiles via LCID table.
    auto ascii_ieq = [](char a, char b) {
        auto lo = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return lo(a) == lo(b);
    };
    struct NameMap { const char* tag; u32 lcid; };
    static const NameMap kNames[] = {
        {"en-US", 0x0409}, {"en-GB", 0x0809}, {"de-DE", 0x0407},
        {"fr-FR", 0x040C}, {"es-ES", 0x0C0A}, {"it-IT", 0x0410},
        {"pt-BR", 0x0416}, {"nl-NL", 0x0413}, {"ja-JP", 0x0411},
        {"zh-CN", 0x0804}, {"ko-KR", 0x0412}, {"ru-RU", 0x0419},
        {"", 0x007F},
    };
    for (const auto& nm : kNames) {
        const char* t = nm.tag;
        usize i = 0;
        bool ok = true;
        for (; t[i] && i < name.size(); ++i) {
            if (!ascii_ieq(t[i], name[i])) { ok = false; break; }
        }
        if (ok && t[i] == 0 && i == name.size()) {
            return get_locale_info_w(m, nm.lcid, lc_type, buf, cch_buf);
        }
    }

    constexpr u32 LOCALE_SNAME = 0x5C;
    constexpr u32 kMods = 0x80000000u | 0x40000000u | 0x20000000u | 0x10000000u;
    if ((lc_type & ~kMods) == LOCALE_SNAME) {
        // Echo the requested name (Windows xx-YY → "xx-YY").
        std::u16string wide;
        wide.reserve(name.size() + 1);
        for (char c : name) wide.push_back(static_cast<char16_t>(static_cast<u8>(c)));
        wide.push_back(0);
        const i32 value_len = static_cast<i32>(wide.size());
        if (buf == 0 || cch_buf == 0) return value_len;
        if (cch_buf < value_len) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (i32 i = 0; i < value_len; ++i) {
            (void)m.memory().write_u16(buf + static_cast<u64>(i) * 2,
                                       static_cast<u16>(wide[static_cast<usize>(i)]));
        }
        return value_len;
    }
    return get_locale_info_w(m, 0x0409, lc_type, buf, cch_buf);
}

i32 LocaleStringManager::compare_string_ex(Marshaler& m,
                                            GuestAddr /*locale_name*/, u32 flags,
                                            GuestAddr str1, i32 len1,
                                            GuestAddr str2, i32 len2) const {
    return compare_string_w(m, 0x0409, flags, str1, len1, str2, len2);
}

// ---------------------------------------------------------------------------
// IsValidLocale
// ---------------------------------------------------------------------------

namespace {

// Installed/supported LCIDs (exact match — not primary-language-only).
// Keep in sync with EnumSystemLocales / GetLocaleInfo profiles.
struct KnownLocale {
    u32 lcid;
    const char* tag;
};

// Installed UI languages. Tags are the BCP-47 names GetThreadPreferredUILanguages
// returns for MUI_LANGUAGE_NAME. LOCALE_INVARIANT has no language tag.
constexpr KnownLocale kLocales[] = {
    {0x0409, "en-US"}, {0x0809, "en-GB"}, {0x0407, "de-DE"},
    {0x040C, "fr-FR"}, {0x0C0A, "es-ES"}, {0x0410, "it-IT"},
    {0x0416, "pt-BR"}, {0x0413, "nl-NL"}, {0x0411, "ja-JP"},
    {0x0404, "zh-TW"}, {0x0804, "zh-CN"}, {0x0412, "ko-KR"},
    {0x0419, "ru-RU"}, {0x0415, "pl-PL"}, {0x041D, "sv-SE"},
    {0x041F, "tr-TR"}, {0x0422, "uk-UA"}, {0x0405, "cs-CZ"},
    {0x007F, ""},
};

// winnls.h MUI_* values used by the thread UI-language calls.
constexpr u32 MUI_RESET_FILTERS         = 0x001;
constexpr u32 MUI_LANGUAGE_ID           = 0x04;
constexpr u32 MUI_LANGUAGE_NAME         = 0x08;
constexpr u32 MUI_MERGE_SYSTEM_FALLBACK = 0x10;
constexpr u32 MUI_MERGE_USER_FALLBACK   = 0x20;
constexpr u32 MUI_THREAD_LANGUAGES      = 0x40;
constexpr u32 MUI_CONSOLE_FILTER        = 0x100;
constexpr u32 MUI_COMPLEX_SCRIPT_FILTER = 0x200;
constexpr u32 MUI_FILTER_MASK =
    MUI_RESET_FILTERS | MUI_CONSOLE_FILTER | MUI_COMPLEX_SCRIPT_FILTER;
constexpr u32 MUI_FORMAT_MASK = MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME;
constexpr u32 MUI_LIST_MASK =
    MUI_MERGE_SYSTEM_FALLBACK | MUI_MERGE_USER_FALLBACK | MUI_THREAD_LANGUAGES;
constexpr u32 kUserUiLanguage = 0x0409;

struct ThreadUiPreference {
    std::vector<u32> langids;
    u32 filter = 0;
};

struct ThreadUiKey {
    const void* manager = nullptr;
    u32 tid = 0;
    bool operator==(const ThreadUiKey& other) const {
        return manager == other.manager && tid == other.tid;
    }
};

struct ThreadUiKeyHash {
    usize operator()(const ThreadUiKey& key) const {
        return std::hash<const void*>{}(key.manager) ^
               (std::hash<u32>{}(key.tid) << 1);
    }
};

std::mutex g_thread_ui_mu;
std::unordered_map<ThreadUiKey, ThreadUiPreference, ThreadUiKeyHash> g_thread_ui;

void log_ui_filter_once() {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) return;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED kernel32!GetThreadPreferredUILanguages: "
                 "console fallback for a language the console cannot display\n");
}

constexpr u32 LCID_INSTALLED       = 0x00000001;
constexpr u32 LCID_SUPPORTED       = 0x00000002;
constexpr u32 LCID_ALTERNATE_SORTS = 0x00000004;

bool locale_in_table(u32 locale) {
    for (const auto& known : kLocales) {
        if (known.lcid == locale) return true;
    }
    return false;
}

const char* locale_tag(u32 lcid) {
    for (const auto& known : kLocales) {
        if (known.lcid == lcid && known.tag[0] != '\0') return known.tag;
    }
    return nullptr;
}

bool ascii_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

bool parse_langid(std::string_view text, u32& out) {
    if (text.size() != 4) return false;
    u32 value = 0;
    for (char ch : text) {
        value <<= 4;
        if (ch >= '0' && ch <= '9') value |= static_cast<u32>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= static_cast<u32>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= static_cast<u32>(ch - 'A' + 10);
        else return false;
    }
    out = value;
    return true;
}

bool forbidden_language_id(u32 id) {
    // Learn SetThreadPreferredUILanguages: these locale constants are not
    // language identifiers and must not appear in an MUI_LANGUAGE_ID list.
    return id == 0x0400u || id == 0x0800u || id == 0x0C00u ||
           id == 0x1000u || id == 0x1400u;
}

bool needs_console_fallback(u32 lcid) {
    // CJK and Cyrillic text does not fit the OEM console code page. Latin
    // tags in the installed table do, so a console filter leaves them alone.
    switch (lcid) {
        case 0x0411: case 0x0804: case 0x0404: case 0x0412:
        case 0x0419: case 0x0422:
            return true;
        default:
            return false;
    }
}

std::string language_text(u32 lcid, bool id_format) {
    if (id_format) {
        char text[5];
        constexpr char kHex[] = "0123456789ABCDEF";
        text[0] = kHex[(lcid >> 12) & 0xF];
        text[1] = kHex[(lcid >> 8) & 0xF];
        text[2] = kHex[(lcid >> 4) & 0xF];
        text[3] = kHex[lcid & 0xF];
        text[4] = '\0';
        return text;
    }
    const char* tag = locale_tag(lcid);
    return tag ? tag : "";
}

ThreadUiKey thread_ui_key(const LocaleStringManager& mgr) {
    return ThreadUiKey{&mgr, ThreadManager::get_current_thread_id_static()};
}

ThreadUiPreference thread_ui_preference(const LocaleStringManager& mgr) {
    std::lock_guard lock(g_thread_ui_mu);
    const auto it = g_thread_ui.find(thread_ui_key(mgr));
    return it == g_thread_ui.end() ? ThreadUiPreference{} : it->second;
}

void store_thread_ui(const LocaleStringManager& mgr, ThreadUiPreference pref) {
    std::lock_guard lock(g_thread_ui_mu);
    const ThreadUiKey key = thread_ui_key(mgr);
    if (pref.langids.empty() && pref.filter == 0) g_thread_ui.erase(key);
    else g_thread_ui[key] = std::move(pref);
}

enum class TokenResult { Skip, Fail, Ok };

TokenResult resolve_language_token(std::string_view token, bool id_format, u32& lcid) {
    if (id_format) {
        u32 parsed = 0;
        if (!parse_langid(token, parsed)) return TokenResult::Skip;
        if (forbidden_language_id(parsed)) return TokenResult::Fail;
        if (!locale_in_table(parsed) || locale_tag(parsed) == nullptr) {
            return TokenResult::Skip;
        }
        lcid = parsed;
        return TokenResult::Ok;
    }
    for (const auto& known : kLocales) {
        if (known.tag[0] == '\0') continue;
        if (ascii_ieq(token, known.tag)) {
            lcid = known.lcid;
            return TokenResult::Ok;
        }
    }
    return TokenResult::Skip;
}

// Reads a double-NUL multi-string. An empty list (immediate NUL) is success
// with no tokens. A missing terminator or an unreadable address fails.
bool read_language_list(Marshaler& m, GuestAddr buffer,
                        std::vector<std::string>& tokens) {
    tokens.clear();
    if (buffer == 0) return false;
    std::string current;
    bool saw_nul = false;
    for (u32 i = 0; i < 2048; ++i) {
        const auto unit = m.read_u16(buffer + static_cast<GuestAddr>(i) * 2);
        if (unit.is_err()) return false;
        if (unit.value() == 0) {
            if (current.empty()) return true;
            tokens.push_back(std::move(current));
            current.clear();
            if (saw_nul) return true;
            saw_nul = true;
            continue;
        }
        saw_nul = false;
        // A non-ASCII name cannot match an installed tag. Keep the token so
        // it is skipped as an unknown language rather than failing the call.
        current.push_back(unit.value() > 0x7F ? '?' : static_cast<char>(unit.value()));
    }
    return false;
}

struct UiLanguageList {
    std::vector<std::string> texts;
    bool filter_unsupported = false;
};

UiLanguageList ui_languages_for(u32 flags, const ThreadUiPreference& pref) {
    UiLanguageList list;
    std::vector<u32> ids = pref.langids;
    const bool thread_only = (flags & MUI_THREAD_LANGUAGES) != 0;
    if (!thread_only) {
        bool have_user = false;
        for (u32 id : ids) {
            if (id == kUserUiLanguage) have_user = true;
        }
        if (!have_user) ids.push_back(kUserUiLanguage);
    }
    std::vector<u32> unique;
    for (u32 id : ids) {
        bool seen = false;
        for (u32 prior : unique) {
            if (prior == id) seen = true;
        }
        if (!seen) unique.push_back(id);
    }
    const bool id_format = (flags & MUI_LANGUAGE_ID) != 0;
    if (pref.filter == MUI_CONSOLE_FILTER) {
        for (u32 id : unique) {
            if (needs_console_fallback(id)) list.filter_unsupported = true;
        }
    }
    for (u32 id : unique) {
        const std::string text = language_text(id, id_format);
        if (!text.empty()) list.texts.push_back(text);
    }
    return list;
}

bool flags_ok_for_get(u32 flags) {
    if ((flags & ~(MUI_FORMAT_MASK | MUI_LIST_MASK)) != 0) return false;
    if ((flags & MUI_FORMAT_MASK) == MUI_FORMAT_MASK) return false;
    if ((flags & MUI_THREAD_LANGUAGES) != 0 &&
        (flags & (MUI_MERGE_USER_FALLBACK | MUI_MERGE_SYSTEM_FALLBACK)) != 0) {
        return false;
    }
    if ((flags & MUI_MERGE_SYSTEM_FALLBACK) != 0 &&
        (flags & MUI_MERGE_USER_FALLBACK) == 0) {
        return false;
    }
    return true;
}

u32 effective_get_flags(u32 flags) {
    u32 effective = flags;
    if ((effective & MUI_FORMAT_MASK) == 0) effective |= MUI_LANGUAGE_NAME;
    if ((effective & MUI_LIST_MASK) == 0) effective |= MUI_MERGE_USER_FALLBACK;
    return effective;
}

} // namespace

u32 LocaleStringManager::get_thread_locale() const {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_thread_locale_mu);
    const auto it = m_thread_locales.find(tid);
    // A thread is born on the user default locale (GetUserDefaultLCID).
    return it == m_thread_locales.end() ? 0x0409u : it->second;
}

bool LocaleStringManager::set_thread_locale(u32 locale) {
    // winnt.h: LOCALE_USER_DEFAULT 0x0400, LOCALE_SYSTEM_DEFAULT 0x0800,
    // LOCALE_CUSTOM_DEFAULT 0x0C00, LOCALE_CUSTOM_UNSPECIFIED 0x1000,
    // LOCALE_CUSTOM_UI_DEFAULT 0x1400. winnls.h declares the return as BOOL.
    u32 resolved = locale;
    if (locale == 0x0400u || locale == 0x0800u) {
        resolved = 0x0409u;
    } else if (locale == 0x0C00u || locale == 0x1000u || locale == 0x1400u ||
               !locale_in_table(locale)) {
        return false;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_thread_locale_mu);
    m_thread_locales[tid] = resolved;
    return true;
}

u32 thread_ui_language(const LocaleStringManager& mgr) {
    // Learn GetThreadUILanguage: identical to the first language from
    // GetThreadPreferredUILanguages(MUI_MERGE_SYSTEM_FALLBACK |
    // MUI_MERGE_USER_FALLBACK | MUI_LANGUAGE_ID).
    const u32 flags = effective_get_flags(
        MUI_MERGE_SYSTEM_FALLBACK | MUI_MERGE_USER_FALLBACK | MUI_LANGUAGE_ID);
    const ThreadUiPreference pref = thread_ui_preference(mgr);
    const UiLanguageList list = ui_languages_for(flags, pref);
    if (list.filter_unsupported) log_ui_filter_once();
    if (list.texts.empty()) return kUserUiLanguage;
    u32 langid = kUserUiLanguage;
    (void)parse_langid(list.texts.front(), langid);
    return langid;
}

bool set_thread_preferred_ui_languages(const LocaleStringManager& mgr,
                                       Marshaler& m, u32 flags,
                                       GuestAddr languages,
                                       GuestAddr count_ptr) {
    if ((flags & ~(MUI_FORMAT_MASK | MUI_FILTER_MASK)) != 0 ||
        (flags & MUI_FORMAT_MASK) == MUI_FORMAT_MASK) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 filters = flags & MUI_FILTER_MASK;
    const int filter_bits =
        ((filters & MUI_RESET_FILTERS) != 0 ? 1 : 0) +
        ((filters & MUI_CONSOLE_FILTER) != 0 ? 1 : 0) +
        ((filters & MUI_COMPLEX_SCRIPT_FILTER) != 0 ? 1 : 0);
    if (filter_bits > 1) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    ThreadUiPreference pref = thread_ui_preference(mgr);
    if (filter_bits == 1) {
        // A filtering flag requires every other parameter to be NULL and
        // does not replace the stored language list.
        if (languages != 0 || count_ptr != 0 ||
            (flags & MUI_FORMAT_MASK) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        if ((filters & MUI_RESET_FILTERS) != 0) pref.filter = 0;
        else pref.filter = filters;
        store_thread_ui(mgr, std::move(pref));
        return true;
    }
    if (languages == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    std::vector<std::string> tokens;
    if (!read_language_list(m, languages, tokens)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const bool id_format = (flags & MUI_LANGUAGE_ID) != 0;
    std::vector<u32> stored;
    for (const std::string& token : tokens) {
        u32 lcid = 0;
        const TokenResult resolved = resolve_language_token(token, id_format, lcid);
        if (resolved == TokenResult::Fail) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        if (resolved != TokenResult::Ok) continue;
        bool seen = false;
        for (u32 prior : stored) {
            if (prior == lcid) seen = true;
        }
        if (!seen) stored.push_back(lcid);
        if (stored.size() == 5) break;
    }
    if (count_ptr != 0 &&
        m.write_u32(count_ptr, static_cast<u32>(stored.size())).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    pref.langids = std::move(stored);
    store_thread_ui(mgr, std::move(pref));
    return true;
}

bool get_thread_preferred_ui_languages(const LocaleStringManager& mgr,
                                       Marshaler& m, u32 flags,
                                       GuestAddr count_ptr, GuestAddr buffer,
                                       GuestAddr cch_ptr) {
    if (!flags_ok_for_get(flags) || count_ptr == 0 || cch_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto capacity = m.read_u32(cch_ptr);
    if (capacity.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    const ThreadUiPreference pref = thread_ui_preference(mgr);
    const UiLanguageList list = ui_languages_for(effective_get_flags(flags), pref);
    if (list.filter_unsupported) {
        log_ui_filter_once();
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    u32 required = 1;
    for (const std::string& text : list.texts) {
        required += static_cast<u32>(text.size()) + 1;
    }
    if (m.write_u32(count_ptr, static_cast<u32>(list.texts.size())).is_err() ||
        m.write_u32(cch_ptr, required).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    if (buffer == 0) {
        if (capacity.value() != 0) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            return false;
        }
        return true;
    }
    if (capacity.value() < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    GuestAddr cursor = buffer;
    for (const std::string& text : list.texts) {
        for (char ch : text) {
            if (m.write_u16(cursor, static_cast<u16>(ch)).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            cursor += 2;
        }
        if (m.write_u16(cursor, 0).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        cursor += 2;
    }
    if (m.write_u16(cursor, 0).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    return true;
}

u32 LocaleStringManager::is_valid_locale(u32 locale, u32 flags) const {
    // Windows: only 0, LCID_INSTALLED, or LCID_SUPPORTED alone are accepted.
    // INSTALLED|SUPPORTED, ALTERNATE_SORTS, and other bits → FALSE.
    if (flags != 0 && flags != LCID_INSTALLED && flags != LCID_SUPPORTED) {
        return 0;
    }
    // Special constants (LOCALE_USER_DEFAULT etc.) are not valid LCIDs here.
    return locale_in_table(locale) ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// EnumSystemLocalesA/W / EnumSystemLocalesEx
// ---------------------------------------------------------------------------

namespace {

// LOCALE_ENUMPROC receives each LCID as an 8-digit hex string ("00000409").
constexpr u32 kEnumLcids[] = {
    0x0409, // en-US
    0x0809, // en-GB
    0x0407, // de-DE
    0x040C, // fr-FR
    0x0C0A, // es-ES
    0x0410, // it-IT
    0x0416, // pt-BR
    0x0413, // nl-NL
    0x0411, // ja-JP
    0x0404, // zh-TW
    0x0804, // zh-CN
    0x0412, // ko-KR
    0x0419, // ru-RU
    0x0415, // pl-PL
    0x041D, // sv-SE
    0x041F, // tr-TR
    0x0422, // uk-UA
    0x0405, // cs-CZ
    0x007F, // LOCALE_INVARIANT
};

// Windows enum_locales_alt_probe: EnumSystemLocalesA(LCID_ALTERNATE_SORTS).
constexpr u32 kAlternateSortLcids[] = {
    0x00010407, 0x0001040e, 0x00040411, 0x00010437, 0x0001007f,
    0x00050804, 0x00020804, 0x00040c04, 0x00041404, 0x00021404,
    0x00051004, 0x00021004, 0x00030404, 0x00040404,
};

// ASCII BCP-47 / Windows locale names for EnumSystemLocalesEx.
constexpr const char* kEnumLocaleNames[] = {
    "en-US", "en-GB", "de-DE", "fr-FR", "es-ES", "it-IT", "pt-BR",
    "nl-NL", "ja-JP", "zh-TW", "zh-CN", "ko-KR", "ru-RU",
    "pl-PL", "sv-SE", "tr-TR", "uk-UA", "cs-CZ",
};

bool emit_locale_hex(Marshaler& m, GuestAddr scratch, bool wide, u32 lcid,
                     const LocaleStringManager::GuestCall& guest_call,
                     GuestAddr callback, bool is_64bit) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08X", lcid);

    if (wide) {
        for (usize i = 0; i < 8; ++i) {
            (void)m.memory().write_u16(scratch + static_cast<u64>(i) * 2,
                                       static_cast<u16>(static_cast<u8>(hex[i])));
        }
        (void)m.memory().write_u16(scratch + 16, 0);
    } else {
        for (usize i = 0; i < 8; ++i) {
            (void)m.memory().write_u8(scratch + i, static_cast<u8>(hex[i]));
        }
        (void)m.memory().write_u8(scratch + 8, 0);
    }

    u64 args[1] = {static_cast<u64>(scratch)};
    return guest_call(callback, args, 1, is_64bit) != 0;
}

} // namespace

GuestAddr LocaleStringManager::ensure_enum_scratch(Marshaler& m) {
    // Per-thread scratch so concurrent EnumSystemLocales* callers do not
    // overwrite each other's locale strings (audit L7).
    thread_local GuestAddr tls_scratch = 0;
    // The address is only meaningful in the guest memory that mapped it.
    // Tests build a fresh GuestMemory per case on this same thread.
    if (tls_scratch != 0 && m.memory().is_mapped(tls_scratch, 2)) return tls_scratch;
    tls_scratch = 0;

    constexpr usize kSize = 0x1000;
    // Prefer a high user VA well clear of PE image / heap / oleaut scratch.
    // Offset by a thread-local salt so concurrent first-time maps do not collide.
    static std::atomic<u32> s_tls_slot{0};
    const u32 slot = s_tls_slot.fetch_add(1, std::memory_order_relaxed);
    constexpr GuestAddr kMin = 0x5A100000;
    const GuestAddr prefer = kMin + static_cast<GuestAddr>(slot) * 0x10000;
    GuestAddr base = m.memory().find_free_gap(prefer, kSize);
    if (base == 0) {
        base = m.memory().find_free_gap(kMin, kSize);
    }
    if (base == 0 ||
        m.memory().map_region(base, kSize, MemoryProtection::ReadWrite).is_err()) {
        VITREO_TRACE("[locale] EnumSystemLocales: failed to map scratch page\n");
        return 0;
    }
    tls_scratch = base;
    return base;
}

u32 LocaleStringManager::enum_system_locales(Marshaler& m, bool wide,
                                             GuestAddr callback, u32 flags) {
    // MSDN: NULL callback → ERROR_INVALID_PARAMETER.
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    // LCID_INSTALLED and LCID_SUPPORTED are mutually exclusive.
    if ((flags & LCID_INSTALLED) && (flags & LCID_SUPPORTED)) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }

    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    GuestAddr scratch = ensure_enum_scratch(m);
    if (scratch == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    const bool is_64bit = m.cpu_is_64bit();
    // LCID_ALTERNATE_SORTS alone → only alternate-sort LCIDs (Windows).
    // Combined with INSTALLED/SUPPORTED (or flags==0 / SUPPORTED) → primary
    // list, then alternate when the ALTERNATE_SORTS bit is set.
    const bool alt_only =
        (flags & LCID_ALTERNATE_SORTS) != 0 &&
        (flags & (LCID_INSTALLED | LCID_SUPPORTED)) == 0;
    const bool include_alt = (flags & LCID_ALTERNATE_SORTS) != 0;

    if (!alt_only) {
        for (u32 lcid : kEnumLcids) {
            if (!emit_locale_hex(m, scratch, wide, lcid, m_guest_call, callback,
                                 is_64bit)) {
                return 1;
            }
        }
    }
    if (alt_only || include_alt) {
        for (u32 lcid : kAlternateSortLcids) {
            if (!emit_locale_hex(m, scratch, wide, lcid, m_guest_call, callback,
                                 is_64bit)) {
                return 1;
            }
        }
    }

    return 1;
}

u32 LocaleStringManager::enum_system_locales_ex(Marshaler& m, GuestAddr callback,
                                                u32 flags, u64 lparam) {
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    (void)flags;

    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    GuestAddr scratch = ensure_enum_scratch(m);
    if (scratch == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    const bool is_64bit = m.cpu_is_64bit();
    for (const char* name : kEnumLocaleNames) {
        usize len = std::strlen(name);
        for (usize i = 0; i < len; ++i) {
            (void)m.memory().write_u16(scratch + static_cast<u64>(i) * 2,
                                       static_cast<u16>(static_cast<u8>(name[i])));
        }
        (void)m.memory().write_u16(scratch + static_cast<u64>(len) * 2, 0);

        // BOOL CALLBACK EnumLocalesProcEx(LPWSTR, DWORD dwFlags, LPARAM);
        u64 args[3] = {static_cast<u64>(scratch), 0, lparam};
        u64 cb_ret = m_guest_call(callback, args, 3, is_64bit);
        if (cb_ret == 0) {
            return 1;
        }
    }

    return 1;
}

u32 LocaleStringManager::enum_ui_languages(Marshaler& m, bool wide,
                                           GuestAddr callback, u32 flags,
                                           u64 lparam) {
    // Learn EnumUILanguages: format flags MUI_LANGUAGE_ID / MUI_LANGUAGE_NAME
    // (winnls.h 0x4 / 0x8) are mutually exclusive, ID is the default. The
    // filter flags (MUI_ALL_INSTALLED_LANGUAGES, MUI_LICENSED_LANGUAGES,
    // MUI_GROUP_POLICY) have no value in winnls.h; their bits were measured
    // by enum_ui_languages_probe (x86 and x64 identical):
    //   - NULL proc -> ERROR_INVALID_PARAMETER, checked before the flags;
    //   - any bit outside 0xFC -> ERROR_INVALID_FLAGS;
    //   - ID|NAME -> ERROR_INVALID_FLAGS;
    //   - more than one of 0x10/0x20/0x40 -> ERROR_INVALID_FLAGS
    //     (the mutually exclusive filters); 0x80 combines with any of them;
    //   - success returns TRUE and leaves the last error untouched.
    constexpr u32 kFormatMask = MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME;
    constexpr u32 kExclusiveFilters = 0x10 | 0x20 | 0x40;
    constexpr u32 kValidFlags = kFormatMask | kExclusiveFilters | 0x80;
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    const u32 filters = flags & kExclusiveFilters;
    if ((flags & ~kValidFlags) != 0 || (flags & kFormatMask) == kFormatMask ||
        (filters & (filters - 1)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }
    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    GuestAddr scratch = ensure_enum_scratch(m);
    if (scratch == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    // Vitreo's installed UI languages are the ones its locale state reports:
    // the system default UI language and the user default UI language. Every
    // filter selects the same set because each of them is installed,
    // licensed and allowed by (absent) group policy.
    std::vector<u32> langs{get_system_default_ui_language()};
    if (get_user_default_ui_language() != langs.front()) {
        langs.push_back(get_user_default_ui_language());
    }
    const bool id_format = (flags & MUI_LANGUAGE_NAME) == 0;
    const bool is_64bit = m.cpu_is_64bit();
    for (u32 lang : langs) {
        const std::string text = language_text(lang, id_format);
        if (text.empty()) continue;
        for (usize i = 0; i <= text.size(); ++i) {
            const u8 ch = i < text.size() ? static_cast<u8>(text[i]) : 0;
            if (wide) {
                (void)m.memory().write_u16(scratch + static_cast<u64>(i) * 2, ch);
            } else {
                (void)m.memory().write_u8(scratch + i, ch);
            }
        }
        // BOOL CALLBACK UILANGUAGE_ENUMPROC(LPTSTR, LONG_PTR lParam);
        u64 args[2] = {static_cast<u64>(scratch),
                       is_64bit ? lparam : (lparam & 0xFFFFFFFFull)};
        // BOOL is 32 bits: the callback owns only EAX.
        if (static_cast<u32>(m_guest_call(callback, args, 2, is_64bit)) == 0) {
            return 1;
        }
    }
    return 1;
}

u32 LocaleStringManager::enum_calendar_info(Marshaler& m, bool wide,
                                            GuestAddr callback, u32 locale,
                                            u32 calendar, u32 cal_type) {
    // winnls.h. CAL_* below 0x40 are the mutually exclusive calendar types.
    // The high flags match LOCALE_NOUSEROVERRIDE and its siblings.
    constexpr u32 CAL_NOUSEROVERRIDE = 0x80000000;
    constexpr u32 CAL_USE_CP_ACP = 0x40000000;
    constexpr u32 CAL_RETURN_NUMBER = 0x20000000;
    constexpr u32 CAL_RETURN_GENITIVE_NAMES = 0x10000000;
    constexpr u32 kModifiers = CAL_NOUSEROVERRIDE | CAL_USE_CP_ACP |
                               CAL_RETURN_NUMBER | CAL_RETURN_GENITIVE_NAMES;
    constexpr u32 CAL_GREGORIAN = 1;
    constexpr u32 ENUM_ALL_CALENDARS = 0xFFFFFFFF;
    constexpr u32 CAL_SMONTHNAME13 = 0x21;
    constexpr u32 CAL_SABBREVMONTHNAME13 = 0x2e;

    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((cal_type & ~kModifiers) == 0 || (cal_type & CAL_RETURN_NUMBER) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }
    if ((cal_type & ~(kModifiers | 0x3F)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        return 0;
    }

    u32 resolved = locale;
    if (locale == 0 || locale == 0x0400u || locale == 0x0800u) resolved = 0x0409u;
    if (locale == 0x0C00u || locale == 0x1000u || locale == 0x1400u ||
        !locale_in_table(resolved)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    // Calendar strings below are en-US Gregorian. Another installed locale
    // must not receive those strings under its own LCID.
    if (resolved != 0x0409u && resolved != 0x007Fu) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED kernel32!EnumCalendarInfoW: "
                         "locale 0x%x\n",
                         resolved);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (calendar != CAL_GREGORIAN && calendar != ENUM_ALL_CALENDARS) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED kernel32!EnumCalendarInfoW: "
                         "calendar 0x%x\n",
                         calendar);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    (void)CAL_RETURN_GENITIVE_NAMES;
    (void)CAL_USE_CP_ACP;
    (void)CAL_NOUSEROVERRIDE;

    const u32 base = cal_type & ~kModifiers;
    if (base == CAL_SMONTHNAME13 || base == CAL_SABBREVMONTHNAME13) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    static const char* kDays[] = {
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char* kAbbrevDays[] = {
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char* kShortDays[] = {
        "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    static const char* kMonths[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    static const char* kAbbrevMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const char* values[4] = {};
    u32 count = 0;
    auto add = [&](const char* text) {
        if (count < 4) values[count++] = text;
    };
    if (base >= 0x07 && base <= 0x0d) add(kDays[base - 0x07]);
    else if (base >= 0x0e && base <= 0x14) add(kAbbrevDays[base - 0x0e]);
    else if (base >= 0x15 && base <= 0x20) add(kMonths[base - 0x15]);
    else if (base >= 0x22 && base <= 0x2d) add(kAbbrevMonths[base - 0x22]);
    else if (base >= 0x31 && base <= 0x37) add(kShortDays[base - 0x31]);
    else switch (base) {
        case 0x01: add("1"); break;
        case 0x02: add("Gregorian Calendar"); break;
        case 0x03: add("0"); break;
        case 0x04: add("A.D."); break;
        case 0x05: add("M/d/yyyy"); add("M/d/yy"); break;
        case 0x06: add("dddd, MMMM d, yyyy"); break;
        case 0x2f: add("MMMM yyyy"); break;
        case 0x30: add("2029"); break;
        case 0x38: add("MMMM d"); break;
        case 0x39: add("AD"); break;
        case 0x3b: add("A.D."); break;
        case 0x3c: add("AD"); break;
        default: {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED kernel32!EnumCalendarInfoW: "
                             "caltype 0x%x\n",
                             base);
            }
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
    }

    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    GuestAddr scratch = ensure_enum_scratch(m);
    if (scratch == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const bool is_64bit = m.cpu_is_64bit();
    for (u32 i = 0; i < count; ++i) {
        const char* text = values[i];
        const usize len = std::strlen(text);
        if (wide) {
            for (usize n = 0; n < len; ++n) {
                (void)m.memory().write_u16(scratch + static_cast<u64>(n) * 2,
                                           static_cast<u16>(static_cast<u8>(text[n])));
            }
            (void)m.memory().write_u16(scratch + static_cast<u64>(len) * 2, 0);
        } else {
            for (usize n = 0; n < len; ++n) {
                (void)m.memory().write_u8(scratch + n, static_cast<u8>(text[n]));
            }
            (void)m.memory().write_u8(scratch + len, 0);
        }
        u64 args[1] = {static_cast<u64>(scratch)};
        if (m_guest_call(callback, args, 1, is_64bit) == 0) return 1;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// FindStringOrdinal
// ---------------------------------------------------------------------------

i32 LocaleStringManager::find_string_ordinal(Marshaler& m, u32 flags,
                                              GuestAddr source_ptr, i32 cch_source,
                                              GuestAddr value_ptr, i32 cch_value,
                                              u32 ignore_case) const {
    constexpr u32 FIND_STARTSWITH = 0x00;
    constexpr u32 FIND_ENDSWITH   = 0x00200000;
    constexpr u32 FIND_FROMSTART  = 0x00400000;
    constexpr u32 FIND_FROMEND    = 0x00800000;

    if (source_ptr == 0 || value_ptr == 0) return -1;

    // Read source and value as UTF-16 code units
    auto& mem = m.memory();

    auto read_u16_string = [&](GuestAddr ptr, i32 cch) -> std::vector<u16> {
        std::vector<u16> result;
        if (cch < 0) {
            // null-terminated
            for (i32 i = 0; i < 65536; ++i) {
                auto r = mem.read_u16(ptr + static_cast<u64>(i) * 2);
                if (!r || r.value() == 0) break;
                result.push_back(static_cast<u16>(r.value()));
            }
        } else {
            result.reserve(static_cast<usize>(cch));
            for (i32 i = 0; i < cch; ++i) {
                auto r = mem.read_u16(ptr + static_cast<u64>(i) * 2);
                if (!r) break;
                result.push_back(static_cast<u16>(r.value()));
            }
        }
        return result;
    };

    auto source = read_u16_string(source_ptr, cch_source);
    auto value  = read_u16_string(value_ptr, cch_value);

    if (value.empty()) return -1;
    if (source.size() < value.size()) return -1;

    auto chars_equal = [ignore_case](u16 a, u16 b) -> bool {
        if (a == b) return true;
        if (ignore_case) {
            return static_cast<u16>(towlower(static_cast<wint_t>(a))) ==
                   static_cast<u16>(towlower(static_cast<wint_t>(b)));
        }
        return false;
    };

    auto match_at = [&](usize pos) -> bool {
        for (usize j = 0; j < value.size(); ++j) {
            if (!chars_equal(source[pos + j], value[j])) return false;
        }
        return true;
    };

    if (flags == FIND_STARTSWITH) {
        return match_at(0) ? 0 : -1;
    }
    if (flags == FIND_ENDSWITH) {
        usize pos = source.size() - value.size();
        return match_at(pos) ? static_cast<i32>(pos) : -1;
    }
    if (flags == FIND_FROMEND) {
        for (i32 i = static_cast<i32>(source.size() - value.size()); i >= 0; --i) {
            if (match_at(static_cast<usize>(i))) return i;
        }
        return -1;
    }
    // FIND_FROMSTART or default
    for (usize i = 0; i <= source.size() - value.size(); ++i) {
        if (match_at(i)) return static_cast<i32>(i);
    }
    return -1;
}

void LocaleStringManager::acquire_srw_lock_exclusive(GuestAddr lock) const {
    if (m_sync_manager) m_sync_manager->acquire_srw_lock_exclusive(lock);
}

void LocaleStringManager::release_srw_lock_exclusive(GuestAddr lock) const {
    if (m_sync_manager) m_sync_manager->release_srw_lock_exclusive(lock);
}

bool LocaleStringManager::try_acquire_srw_lock_exclusive(GuestAddr lock) const {
    if (m_sync_manager) return m_sync_manager->try_acquire_srw_lock_exclusive(lock);
    return true;
}

void LocaleStringManager::acquire_srw_lock_shared(GuestAddr lock) const {
    if (m_sync_manager) m_sync_manager->acquire_srw_lock_shared(lock);
}

bool LocaleStringManager::release_srw_lock_shared(GuestAddr lock) const {
    if (m_sync_manager) return m_sync_manager->release_srw_lock_shared(lock);
    return true;
}

bool LocaleStringManager::try_acquire_srw_lock_shared(GuestAddr lock) const {
    if (m_sync_manager) return m_sync_manager->try_acquire_srw_lock_shared(lock);
    return true;
}

void LocaleStringManager::wake_all_condition_variable(GuestAddr cv) const {
    if (m_sync_manager) m_sync_manager->wake_all_condition_variable(cv);
}

u32 LocaleStringManager::sleep_condition_variable_srw(GuestAddr cv, GuestAddr lock,
                                                      u32 ms, u32 flags) const {
    if (m_sync_manager) return m_sync_manager->sleep_condition_variable_srw(cv, lock, ms, flags) ? 1u : 0u;
    return 0;
}


} // namespace vitreo::winapi
