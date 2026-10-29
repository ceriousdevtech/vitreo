/// @file normalization.cpp
/// @brief NormalizeString, IsNormalizedString, IdnToAscii, IdnToUnicode and
/// IdnToNameprepUnicode (winnls.h), on the host's ICU.
///
/// Learn documents the parameters and the error names. Everything below that
/// Learn leaves open was measured on winoracle, x86 and x64 identical
/// (tools/oracle_probe/normaliz_*_probe.c):
///
/// NormalizeString / IsNormalizedString
///   - NORM_FORM 1, 2, 5, 6 are accepted. Form 13 is also accepted by Windows
///     (an undocumented IDNA mapping whose tables are not published); it fails
///     here with the documented ERROR_INVALID_PARAMETER and a one-time
///     UNIMPLEMENTED line. Every other value is ERROR_INVALID_PARAMETER.
///   - The normalization data is Unicode 11.0: a code point assigned later is
///     copied unchanged (normalize_v11).
///   - Invalid input is an unpaired surrogate or a noncharacter (U+FDD0..FDEF
///     and U+xxFFFE/xxFFFF on every plane). Unassigned and private-use code
///     points are valid. The reported index is the UTF-16 unit at which
///     decoding found the error (find_invalid).
///   - Success sets ERROR_SUCCESS, including the size query and an empty
///     input. IsNormalizedString never sets it: TRUE and a plain FALSE leave
///     GetLastError as it was.
///   - The size query does not look at the text: n units give n*3 (forms C/D)
///     or n*18 (KC/KD) when that is at most 64, else max(64, n + n/8).
///   - A short buffer receives the first cwDstLength units of the result and
///     the call fails with ERROR_INSUFFICIENT_BUFFER. Windows returns the
///     negative of an internal re-estimate, always at least the size needed in
///     the measured cases; this returns the negative of the exact size needed,
///     so the documented retry loop succeeds on its next call.
///   - When the valid prefix fits, invalid input fails with
///     ERROR_NO_UNICODE_TRANSLATION, the normalized valid prefix written, and
///     the negative of the error index returned.
///
/// IdnToAscii / IdnToNameprepUnicode / IdnToUnicode
///   - The IDNA2003 structure of RFC 3490: labels split on U+002E, U+3002,
///     U+FF0E and U+FF61; an all-ASCII label is copied without mapping (case
///     and "xn--" labels included). A non-ASCII label is mapped with UTS #46
///     transitional mapping (case folding, NFKC, ß -> ss, ZWJ/ZWNJ removed),
///     not RFC 3491's Unicode 3.2 tables, and no bidi rule applies.
///   - The UTS #46 data is version 11.0.0. A sweep of every code point found
///     Windows rejecting exactly the code points 11.0.0 marks disallowed
///     (idna_v11_disallowed.inc, generated from IdnaMappingTable-11.0.0.txt)
///     and those unassigned in Unicode 11.0, and mapping the rest as the
///     host's newer UTS #46 data does. IDN_ALLOW_UNASSIGNED passes unassigned
///     and private-use code points through unmapped; noncharacters still fail.
///   - Control characters (U+0001..U+001F, U+007F) always fail; space, '_',
///     '/' and '@' are valid unless IDN_USE_STD3_ASCII_RULES restricts labels
///     to letters, digits and interior hyphens.
///   - Output labels hold 1-63 characters and the output name at most 255
///     (a trailing dot counts; the NUL does not). A single trailing dot is
///     kept; any other empty label fails.
///   - Flags: IdnToAscii and IdnToNameprepUnicode accept IDN_ALLOW_UNASSIGNED,
///     IDN_USE_STD3_ASCII_RULES and IDN_EMAIL_ADDRESS; IDN_RAW_PUNYCODE and
///     any other bit fail with ERROR_INVALID_FLAGS. IdnToUnicode also accepts
///     IDN_RAW_PUNYCODE, which skips the round-trip verification.
///   - IDN_EMAIL_ADDRESS splits at the last '@'. The domain converts as a
///     name; a non-ASCII local part becomes "xl--" + Punycode of its code
///     points, unmapped; an ASCII local part is copied. No '@' makes the whole
///     string the local part.
///   - Every failure in the text is ERROR_INVALID_NAME. Success leaves
///     GetLastError as it was. The output is NUL-terminated only when the
///     counted input ends with one.

#include "winapi/kernel32/locale/normalization.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

// libicucore. Declarations and constants from ICU's unorm2.h, uidna.h,
// uchar.h and utypes.h.
extern "C" {
typedef struct UNormalizer2 UNormalizer2;
typedef struct UIDNA UIDNA;
typedef struct UIDNAInfo {
    int16_t size;
    int8_t isTransitionalDifferent;
    int8_t reservedB3;
    uint32_t errors;
    int32_t reservedI2;
    int32_t reservedI3;
} UIDNAInfo;
const UNormalizer2* unorm2_getNFCInstance(int* status);
const UNormalizer2* unorm2_getNFDInstance(int* status);
const UNormalizer2* unorm2_getNFKCInstance(int* status);
const UNormalizer2* unorm2_getNFKDInstance(int* status);
int32_t unorm2_normalize(const UNormalizer2* norm2, const char16_t* src, int32_t length,
                         char16_t* dest, int32_t capacity, int* status);
int8_t unorm2_isNormalized(const UNormalizer2* norm2, const char16_t* s, int32_t length,
                           int* status);
UIDNA* uidna_openUTS46(uint32_t options, int* status);
int32_t uidna_labelToUnicode(const UIDNA* idna, const char16_t* label, int32_t length,
                             char16_t* dest, int32_t capacity, UIDNAInfo* info,
                             int* status);
int8_t u_charType(int32_t c);
void u_charAge(int32_t c, uint8_t version[4]);
}

namespace vitreo::winapi::nls {
namespace {

constexpr int U_ZERO_ERROR = 0;
constexpr int U_BUFFER_OVERFLOW_ERROR = 15;
constexpr int8_t U_UNASSIGNED = 0;
constexpr uint32_t UIDNA_USE_STD3_RULES = 0x2;
constexpr uint32_t UIDNA_ERROR_DISALLOWED = 0x80;
constexpr uint32_t UIDNA_ERROR_PUNYCODE = 0x100;
constexpr uint32_t UIDNA_ERROR_LABEL_HAS_DOT = 0x200;
constexpr uint32_t UIDNA_ERROR_INVALID_ACE_LABEL = 0x400;

// winnls.h NORM_FORM.
constexpr i32 NormalizationC = 1;
constexpr i32 NormalizationD = 2;
constexpr i32 NormalizationKC = 5;
constexpr i32 NormalizationKD = 6;
constexpr i32 kWindowsIdnaForm = 13;  // Accepted by Windows; not in NORM_FORM.

// winnls.h IDN_* flags.
constexpr u32 IDN_ALLOW_UNASSIGNED = 0x01;
constexpr u32 IDN_USE_STD3_ASCII_RULES = 0x02;
constexpr u32 IDN_EMAIL_ADDRESS = 0x04;
constexpr u32 IDN_RAW_PUNYCODE = 0x08;

// winerror.h; not in error_codes (codepage_convert.cpp defines it the same way).
constexpr u32 ERROR_NO_UNICODE_TRANSLATION = 1113;

constexpr usize kMaxLabel = 63;
constexpr usize kMaxName = 255;

void log_unimplemented_form_13(const char* api) {
    static std::mutex mu;
    static bool normalize_logged = false;
    static bool is_normalized_logged = false;
    std::lock_guard lock(mu);
    bool& logged = api[0] == 'N' ? normalize_logged : is_normalized_logged;
    if (logged) return;
    logged = true;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED kernelbase!%s: NORM_FORM 13 (Windows IDNA "
                 "mapping, undocumented tables)\n",
                 api);
}

[[nodiscard]] const UNormalizer2* normalizer_for(i32 form) {
    int status = U_ZERO_ERROR;
    const UNormalizer2* n = nullptr;
    switch (form) {
    case NormalizationC: n = unorm2_getNFCInstance(&status); break;
    case NormalizationD: n = unorm2_getNFDInstance(&status); break;
    case NormalizationKC: n = unorm2_getNFKCInstance(&status); break;
    case NormalizationKD: n = unorm2_getNFKDInstance(&status); break;
    default: return nullptr;
    }
    return status > U_ZERO_ERROR ? nullptr : n;
}

[[nodiscard]] bool is_noncharacter(u32 cp) noexcept {
    return (cp & 0xFFFEu) == 0xFFFEu || (cp >= 0xFDD0u && cp <= 0xFDEFu);
}
[[nodiscard]] bool is_high(char16_t u) noexcept { return u >= 0xD800 && u <= 0xDBFF; }
[[nodiscard]] bool is_low(char16_t u) noexcept { return u >= 0xDC00 && u <= 0xDFFF; }

/// Unassigned as of Unicode 11.0, the version of both Windows' normalization
/// data and its UTS #46 data. Noncharacters are Cn but never unassigned.
[[nodiscard]] bool unassigned_in_v11(u32 cp) {
    if (is_noncharacter(cp)) return false;
    if (u_charType(static_cast<int32_t>(cp)) == U_UNASSIGNED) return true;
    uint8_t age[4]{};
    u_charAge(static_cast<int32_t>(cp), age);
    return age[0] > 11 || (age[0] == 11 && age[1] > 0);
}

/// Where NormalizeString finds the input invalid: the valid prefix ends at
/// @c prefix_end, and the call reports @c index (the unit at which decoding
/// failed: the unit after an unpaired high surrogate, the low unit of a
/// supplementary noncharacter, else the offending unit itself).
struct Invalid {
    usize prefix_end;
    usize index;
};

[[nodiscard]] std::optional<Invalid> find_invalid(std::u16string_view s) {
    for (usize i = 0; i < s.size(); ++i) {
        const char16_t u = s[i];
        if (is_high(u)) {
            if (i + 1 < s.size() && is_low(s[i + 1])) {
                const u32 cp = 0x10000u + ((static_cast<u32>(u) - 0xD800u) << 10) +
                               (static_cast<u32>(s[i + 1]) - 0xDC00u);
                if (is_noncharacter(cp)) return Invalid{i, i + 1};
                ++i;
                continue;
            }
            return Invalid{i, i + 1 < s.size() ? i + 1 : i};
        }
        if (is_low(u) || is_noncharacter(u)) return Invalid{i, i};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::u16string> icu_normalize_run(const UNormalizer2* norm,
                                                              std::u16string_view s) {
    if (s.empty()) return std::u16string{};
    std::u16string out(s.size() * 4 + 16, u'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        int status = U_ZERO_ERROR;
        const int32_t n = unorm2_normalize(norm, s.data(), static_cast<int32_t>(s.size()),
                                           out.data(), static_cast<int32_t>(out.size()),
                                           &status);
        if (status == U_BUFFER_OVERFLOW_ERROR) {
            out.assign(static_cast<usize>(n), u'\0');
            continue;
        }
        if (status > U_ZERO_ERROR) return std::nullopt;
        out.resize(static_cast<usize>(n));
        return out;
    }
    return std::nullopt;
}

/// Length in units of the code point starting at s[i] in valid UTF-16.
[[nodiscard]] usize unit_count(std::u16string_view s, usize i) {
    return is_high(s[i]) && i + 1 < s.size() && is_low(s[i + 1]) ? 2 : 1;
}

[[nodiscard]] u32 code_point_at(std::u16string_view s, usize i) {
    if (unit_count(s, i) == 1) return s[i];
    return 0x10000u + ((static_cast<u32>(s[i]) - 0xD800u) << 10) +
           (static_cast<u32>(s[i + 1]) - 0xDC00u);
}

/// Normalize valid UTF-16 with Unicode 11.0 data. A code point assigned
/// later is unassigned to Windows: a starter with no decomposition that
/// composes with nothing. The host's newer data is the 11.0 data for every
/// other code point (Unicode normalization stability policy), so the runs
/// between such code points normalize independently and the code points
/// themselves are copied. Measured: per-code-point sweeps of all four forms
/// (tools/oracle_probe/normaliz_form_sweep_probe.c) differ from the host
/// only in code points assigned after 11.0.
[[nodiscard]] std::optional<std::u16string> normalize_v11(const UNormalizer2* norm,
                                                          std::u16string_view s) {
    std::u16string out;
    usize run = 0;
    for (usize i = 0; i < s.size();) {
        const usize n = unit_count(s, i);
        if (unassigned_in_v11(code_point_at(s, i))) {
            const auto mapped = icu_normalize_run(norm, s.substr(run, i - run));
            if (!mapped) return std::nullopt;
            out += *mapped;
            out += s.substr(i, n);
            run = i + n;
        }
        i += n;
    }
    const auto mapped = icu_normalize_run(norm, s.substr(run));
    if (!mapped) return std::nullopt;
    out += *mapped;
    return out;
}

/// IsNormalized with the same Unicode 11.0 view: every run between code
/// points assigned after 11.0 must be normalized on its own.
[[nodiscard]] std::optional<bool> is_normalized_v11(const UNormalizer2* norm,
                                                    std::u16string_view s) {
    const auto check = [norm](std::u16string_view run) -> std::optional<bool> {
        int status = U_ZERO_ERROR;
        const bool ok =
            unorm2_isNormalized(norm, run.data(), static_cast<int32_t>(run.size()), &status) != 0;
        if (status > U_ZERO_ERROR) return std::nullopt;
        return ok;
    };
    usize run = 0;
    for (usize i = 0; i < s.size();) {
        const usize n = unit_count(s, i);
        if (unassigned_in_v11(code_point_at(s, i))) {
            const auto ok = check(s.substr(run, i - run));
            if (!ok || !*ok) return ok;
            run = i + n;
        }
        i += n;
    }
    return check(s.substr(run));
}

[[nodiscard]] i32 size_estimate(usize n, i32 form) {
    const i64 factor = (form == NormalizationC || form == NormalizationD) ? 3 : 18;
    const i64 units = static_cast<i64>(n);
    i64 estimate = units * factor;
    if (estimate > 64) estimate = std::max<i64>(64, units + units / 8);
    return static_cast<i32>(std::min<i64>(estimate, std::numeric_limits<i32>::max()));
}

// ---------------------------------------------------------------------------
// Punycode, RFC 3492 sections 5 and 6.
// ---------------------------------------------------------------------------

constexpr u32 kBase = 36;
constexpr u32 kTmin = 1;
constexpr u32 kTmax = 26;
constexpr u32 kSkew = 38;
constexpr u32 kDamp = 700;
constexpr u32 kInitialBias = 72;
constexpr u32 kInitialN = 0x80;
constexpr u64 kMaxInt = 0xFFFFFFFFu;

[[nodiscard]] u32 adapt(u64 delta, u64 numpoints, bool firsttime) {
    delta = firsttime ? delta / kDamp : delta / 2;
    delta += delta / numpoints;
    u32 k = 0;
    while (delta > ((kBase - kTmin) * kTmax) / 2) {
        delta /= kBase - kTmin;
        k += kBase;
    }
    return static_cast<u32>(k + (((kBase - kTmin + 1) * delta) / (delta + kSkew)));
}

[[nodiscard]] u32 threshold(u32 k, u32 bias) {
    if (k <= bias) return kTmin;
    if (k >= bias + kTmax) return kTmax;
    return k - bias;
}

[[nodiscard]] char16_t encode_digit(u32 d) {
    return static_cast<char16_t>(d < 26 ? u'a' + d : u'0' + (d - 26));
}

[[nodiscard]] std::optional<u32> decode_digit(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0' + 26;
    if (c >= u'A' && c <= u'Z') return c - u'A';
    if (c >= u'a' && c <= u'z') return c - u'a';
    return std::nullopt;
}

[[nodiscard]] std::optional<std::u16string> punycode_encode(const std::vector<u32>& input) {
    std::u16string output;
    for (u32 c : input) {
        if (c < 0x80) output.push_back(static_cast<char16_t>(c));
    }
    const u64 b = output.size();
    u64 h = b;
    if (b > 0) output.push_back(u'-');
    u64 n = kInitialN;
    u64 delta = 0;
    u32 bias = kInitialBias;
    while (h < input.size()) {
        u64 m = kMaxInt;
        for (u32 c : input) {
            if (c >= n && c < m) m = c;
        }
        if ((m - n) > (kMaxInt - delta) / (h + 1)) return std::nullopt;
        delta += (m - n) * (h + 1);
        n = m;
        for (u32 c : input) {
            if (c < n && ++delta > kMaxInt) return std::nullopt;
            if (c != n) continue;
            u64 q = delta;
            for (u32 k = kBase;; k += kBase) {
                const u32 t = threshold(k, bias);
                if (q < t) break;
                output.push_back(encode_digit(static_cast<u32>(t + (q - t) % (kBase - t))));
                q = (q - t) / (kBase - t);
            }
            output.push_back(encode_digit(static_cast<u32>(q)));
            bias = adapt(delta, h + 1, h == b);
            delta = 0;
            ++h;
        }
        ++delta;
        ++n;
    }
    return output;
}

[[nodiscard]] std::optional<std::vector<u32>> punycode_decode(std::u16string_view input) {
    std::vector<u32> output;
    usize in = 0;
    if (const usize delim = input.rfind(u'-'); delim != std::u16string_view::npos) {
        for (usize j = 0; j < delim; ++j) {
            if (input[j] >= 0x80) return std::nullopt;
            output.push_back(input[j]);
        }
        in = delim + 1;
    }
    u64 n = kInitialN;
    u64 i = 0;
    u32 bias = kInitialBias;
    while (in < input.size()) {
        const u64 oldi = i;
        u64 w = 1;
        for (u32 k = kBase;; k += kBase) {
            if (in >= input.size()) return std::nullopt;
            const auto digit = decode_digit(input[in++]);
            if (!digit) return std::nullopt;
            if (*digit > (kMaxInt - i) / w) return std::nullopt;
            i += *digit * w;
            const u32 t = threshold(k, bias);
            if (*digit < t) break;
            if (w > kMaxInt / (kBase - t)) return std::nullopt;
            w *= kBase - t;
        }
        const u64 count = output.size() + 1;
        bias = adapt(i - oldi, count, oldi == 0);
        if (i / count > kMaxInt - n) return std::nullopt;
        n += i / count;
        i %= count;
        // A decoded basic code point, a surrogate or a value past U+10FFFF is
        // not something an encoder produces.
        if (n < 0x80 || (n >= 0xD800 && n <= 0xDFFF) || n > 0x10FFFF) return std::nullopt;
        output.insert(output.begin() + static_cast<std::ptrdiff_t>(i), static_cast<u32>(n));
        ++i;
    }
    return output;
}

// ---------------------------------------------------------------------------
// IDN helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] bool all_ascii(std::u16string_view s) {
    return std::all_of(s.begin(), s.end(), [](char16_t c) { return c < 0x80; });
}

[[nodiscard]] bool has_control(std::u16string_view s) {
    return std::any_of(s.begin(), s.end(), [](char16_t c) { return c < 0x20 || c == 0x7F; });
}

[[nodiscard]] char16_t ascii_lower(char16_t c) {
    return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + 32) : c;
}

[[nodiscard]] bool starts_with_ci(std::u16string_view s, std::u16string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i) {
        if (ascii_lower(s[i]) != ascii_lower(prefix[i])) return false;
    }
    return true;
}

[[nodiscard]] bool equals_ci(std::u16string_view a, std::u16string_view b) {
    return a.size() == b.size() && starts_with_ci(a, b);
}

/// STD3 ASCII rules: letters, digits and hyphens, no hyphen at either end.
[[nodiscard]] bool std3_ok(std::u16string_view label) {
    if (label.empty() || label.front() == u'-' || label.back() == u'-') return false;
    return std::all_of(label.begin(), label.end(), [](char16_t c) {
        return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
               (c >= u'0' && c <= u'9') || c == u'-';
    });
}

/// Code points of well-formed UTF-16; nullopt for an unpaired surrogate.
[[nodiscard]] std::optional<std::vector<u32>> code_points(std::u16string_view s) {
    std::vector<u32> out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        const char16_t u = s[i];
        if (is_high(u)) {
            if (i + 1 >= s.size() || !is_low(s[i + 1])) return std::nullopt;
            out.push_back(0x10000u + ((static_cast<u32>(u) - 0xD800u) << 10) +
                          (static_cast<u32>(s[i + 1]) - 0xDC00u));
            ++i;
        } else if (is_low(u)) {
            return std::nullopt;
        } else {
            out.push_back(u);
        }
    }
    return out;
}

[[nodiscard]] std::u16string to_utf16(const std::vector<u32>& cps) {
    std::u16string out;
    for (u32 cp : cps) {
        if (cp >= 0x10000) {
            out.push_back(static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

[[nodiscard]] const UIDNA* uts46(bool std3) {
    static UIDNA* plain = [] {
        int status = U_ZERO_ERROR;
        return uidna_openUTS46(0, &status);
    }();
    static UIDNA* strict = [] {
        int status = U_ZERO_ERROR;
        return uidna_openUTS46(UIDNA_USE_STD3_RULES, &status);
    }();
    return std3 ? strict : plain;
}

/// UTS #46 transitional mapping of a run of assigned code points.
///
/// ICU's labelToUnicode decodes a label that maps to an "xn--" form; Windows
/// maps such a label and then rejects the ACE prefix. A leading '0' keeps
/// ICU from treating the mapped text as an ACE label and is removed again:
/// U+0030 is a valid starter with no canonical composition, so it changes
/// neither the mapping nor the normalization of what follows.
[[nodiscard]] std::optional<std::u16string> uts46_map_run(std::u16string_view run, bool std3) {
    if (run.empty()) return std::u16string{};
    const UIDNA* idna = uts46(std3);
    if (!idna) return std::nullopt;
    std::u16string in;
    in.reserve(run.size() + 1);
    in.push_back(u'0');
    in.append(run);
    std::u16string out(in.size() * 4 + 16, u'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        UIDNAInfo info{};
        info.size = sizeof(UIDNAInfo);
        int status = U_ZERO_ERROR;
        const int32_t n = uidna_labelToUnicode(idna, in.data(), static_cast<int32_t>(in.size()),
                                               out.data(), static_cast<int32_t>(out.size()),
                                               &info, &status);
        if (status == U_BUFFER_OVERFLOW_ERROR) {
            out.assign(static_cast<usize>(n), u'\0');
            continue;
        }
        if (status > U_ZERO_ERROR) return std::nullopt;
        if ((info.errors & (UIDNA_ERROR_DISALLOWED | UIDNA_ERROR_LABEL_HAS_DOT |
                            UIDNA_ERROR_INVALID_ACE_LABEL | UIDNA_ERROR_PUNYCODE)) != 0) {
            return std::nullopt;
        }
        out.resize(static_cast<usize>(n));
        if (out.empty() || out.front() != u'0') return std::nullopt;
        out.erase(0, 1);
        return out;
    }
    return std::nullopt;
}

struct CodePointRange {
    u32 first;
    u32 last;
};

constexpr CodePointRange kIdnaV11Disallowed[] = {
#include "winapi/kernel32/locale/idna_v11_disallowed.inc"
};

/// The Unicode Standard's private-use code points (section 23.5; fixed by the
/// stability policy). The host's general category is not used: Apple's ICU
/// reports parts of U+E000..U+F8FF as assigned.
[[nodiscard]] bool is_private_use(u32 cp) {
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
           (cp >= 0x100000 && cp <= 0x10FFFD);
}

/// U+115F HANGUL CHOSEONG FILLER and U+1160 HANGUL JUNGSEONG FILLER pass
/// through under IDN_ALLOW_UNASSIGNED although UTS #46 11.0.0 marks them
/// disallowed; the other Hangul fillers (U+3164, U+FFA0) do not. Measured by
/// the all-code-point sweep (tools/oracle_probe/normaliz_sweep_probe.c);
/// no published table explains it.
[[nodiscard]] bool windows_treats_as_unassigned(u32 cp) {
    return cp == 0x115F || cp == 0x1160;
}

[[nodiscard]] bool disallowed_in_v11(u32 cp) {
    const auto it = std::upper_bound(std::begin(kIdnaV11Disallowed), std::end(kIdnaV11Disallowed),
                                     cp, [](u32 v, const CodePointRange& r) { return v < r.first; });
    return it != std::begin(kIdnaV11Disallowed) && cp <= std::prev(it)->last;
}

/// Map one non-ASCII label. Each code point is first judged against the
/// UTS #46 11.0.0 data; the rest map as runs. IDN_ALLOW_UNASSIGNED passes
/// unassigned and private-use code points through unchanged. Either is a
/// starter with no decomposition, so the runs on each side map independently.
[[nodiscard]] std::optional<std::u16string> map_label(std::u16string_view label, bool std3,
                                                      bool allow_unassigned) {
    const auto cps = code_points(label);
    if (!cps) return std::nullopt;
    std::u16string out;
    std::vector<u32> run;
    for (u32 cp : *cps) {
        const bool unassigned = unassigned_in_v11(cp);
        const bool pass_through =
            allow_unassigned &&
            (unassigned || is_private_use(cp) || windows_treats_as_unassigned(cp));
        if (!pass_through && (unassigned || disallowed_in_v11(cp))) return std::nullopt;
        if (!pass_through) {
            run.push_back(cp);
            continue;
        }
        const auto mapped = uts46_map_run(to_utf16(run), std3);
        if (!mapped) return std::nullopt;
        out += *mapped;
        out += to_utf16({cp});
        run.clear();
    }
    const auto mapped = uts46_map_run(to_utf16(run), std3);
    if (!mapped) return std::nullopt;
    out += *mapped;
    return out;
}

[[nodiscard]] std::optional<std::u16string> convert_label(IdnApi api, u32 flags,
                                                          std::u16string_view label);

[[nodiscard]] std::optional<std::u16string> to_unicode_label(u32 flags,
                                                             std::u16string_view label) {
    if (label.size() > kMaxLabel || has_control(label)) return std::nullopt;
    if (!starts_with_ci(label, u"xn--")) {
        if ((flags & IDN_USE_STD3_ASCII_RULES) && !std3_ok(label)) return std::nullopt;
        return std::u16string(label);
    }
    std::u16string payload;
    for (char16_t c : label.substr(4)) payload.push_back(ascii_lower(c));
    const auto decoded = punycode_decode(payload);
    if (!decoded || decoded->empty()) return std::nullopt;
    if (std::all_of(decoded->begin(), decoded->end(), [](u32 c) { return c < 0x80; })) {
        return std::nullopt;
    }
    std::u16string text = to_utf16(*decoded);
    if (!(flags & IDN_RAW_PUNYCODE)) {
        // RFC 3490 ToUnicode step 7: ToASCII of the result must give back the
        // label, compared case-insensitively.
        const auto again = convert_label(
            IdnApi::ToAscii, flags & (IDN_ALLOW_UNASSIGNED | IDN_USE_STD3_ASCII_RULES), text);
        if (!again || !equals_ci(*again, u"xn--" + payload)) return std::nullopt;
    }
    return text;
}

[[nodiscard]] std::optional<std::u16string> convert_label(IdnApi api, u32 flags,
                                                          std::u16string_view label) {
    if (api == IdnApi::ToUnicode) return to_unicode_label(flags, label);
    const bool std3 = (flags & IDN_USE_STD3_ASCII_RULES) != 0;
    if (has_control(label)) return std::nullopt;
    std::u16string mapped;
    if (all_ascii(label)) {
        mapped = label;
    } else {
        auto m = map_label(label, std3, (flags & IDN_ALLOW_UNASSIGNED) != 0);
        if (!m || m->empty()) return std::nullopt;
        mapped = std::move(*m);
    }
    if (all_ascii(mapped)) {
        if (std3 && !std3_ok(mapped)) return std::nullopt;
        if (mapped.size() > kMaxLabel) return std::nullopt;
        return mapped;
    }
    if (std3 && (mapped.front() == u'-' || mapped.back() == u'-')) return std::nullopt;
    if (api == IdnApi::ToNameprepUnicode) {
        if (mapped.size() > kMaxLabel) return std::nullopt;
        return mapped;
    }
    if (starts_with_ci(mapped, u"xn--")) return std::nullopt;
    const auto cps = code_points(mapped);
    if (!cps) return std::nullopt;
    const auto encoded = punycode_encode(*cps);
    if (!encoded) return std::nullopt;
    std::u16string out = u"xn--" + *encoded;
    if (out.size() > kMaxLabel) return std::nullopt;
    return out;
}

[[nodiscard]] bool is_separator(IdnApi api, char16_t c) {
    if (c == u'.') return true;
    return api != IdnApi::ToUnicode && (c == 0x3002 || c == 0xFF0E || c == 0xFF61);
}

[[nodiscard]] std::optional<std::u16string> convert_name(IdnApi api, u32 flags,
                                                         std::u16string_view name) {
    if (api == IdnApi::ToUnicode && name.size() > kMaxName) return std::nullopt;
    std::vector<std::u16string_view> labels;
    usize start = 0;
    for (usize i = 0; i <= name.size(); ++i) {
        if (i == name.size() || is_separator(api, name[i])) {
            labels.push_back(name.substr(start, i - start));
            start = i + 1;
        }
    }
    const bool trailing_dot = labels.size() > 1 && labels.back().empty();
    if (trailing_dot) labels.pop_back();
    std::u16string out;
    for (usize i = 0; i < labels.size(); ++i) {
        if (labels[i].empty()) return std::nullopt;
        const auto converted = convert_label(api, flags, labels[i]);
        if (!converted) return std::nullopt;
        if (i) out.push_back(u'.');
        out += *converted;
    }
    if (trailing_dot) out.push_back(u'.');
    if (api != IdnApi::ToUnicode && out.size() > kMaxName) return std::nullopt;
    return out;
}

[[nodiscard]] std::optional<std::u16string> convert_local_part(IdnApi api,
                                                               std::u16string_view local) {
    switch (api) {
    case IdnApi::ToNameprepUnicode:
        return std::u16string(local);
    case IdnApi::ToAscii: {
        const auto cps = code_points(local);
        if (!cps) return std::nullopt;
        if (std::any_of(cps->begin(), cps->end(), is_noncharacter)) return std::nullopt;
        if (all_ascii(local)) return std::u16string(local);
        const auto encoded = punycode_encode(*cps);
        if (!encoded) return std::nullopt;
        return u"xl--" + *encoded;
    }
    case IdnApi::ToUnicode: {
        if (!starts_with_ci(local, u"xl--")) return std::u16string(local);
        const auto decoded = punycode_decode(local.substr(4));
        if (!decoded || decoded->empty()) return std::nullopt;
        if (std::all_of(decoded->begin(), decoded->end(), [](u32 c) { return c < 0x80; })) {
            return std::nullopt;
        }
        return to_utf16(*decoded);
    }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::u16string> convert_text(IdnApi api, u32 flags,
                                                         std::u16string_view text) {
    if (api == IdnApi::ToUnicode && !all_ascii(text)) return std::nullopt;
    if (!(flags & IDN_EMAIL_ADDRESS)) return convert_name(api, flags, text);
    const usize at = text.rfind(u'@');
    if (at == std::u16string_view::npos) return convert_local_part(api, text);
    const auto local = text.substr(0, at);
    const auto domain = text.substr(at + 1);
    if (local.empty() || domain.empty()) return std::nullopt;
    auto converted_local = convert_local_part(api, local);
    const auto converted_domain = convert_name(api, flags, domain);
    if (!converted_local || !converted_domain) return std::nullopt;
    *converted_local += u'@';
    *converted_local += *converted_domain;
    return converted_local;
}

[[nodiscard]] StringResult fail(u32 error) {
    StringResult r;
    r.last_error = error;
    return r;
}

} // namespace

StringResult normalize_string(i32 form, const StringArgs& args) {
    if (form == kWindowsIdnaForm) log_unimplemented_form_13("NormalizeString");
    const UNormalizer2* norm = normalizer_for(form);
    if (!norm || args.src_null || args.src_len < -1 || args.dst_len < 0) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    StringResult r;
    r.last_error = error_codes::ERROR_SUCCESS;
    if (args.src.empty()) return r;
    if (args.dst_null || args.dst_len == 0) {
        r.ret = size_estimate(args.src.size(), form);
        return r;
    }
    const std::u16string_view src(args.src);
    const auto invalid = find_invalid(src);
    const auto normalized = normalize_v11(norm, src.substr(0, invalid ? invalid->prefix_end
                                                                      : src.size()));
    if (!normalized) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    const usize capacity = static_cast<usize>(args.dst_len);
    if (normalized->size() > capacity) {
        r.written = normalized->substr(0, capacity);
        r.ret = -static_cast<i32>(normalized->size());
        r.last_error = error_codes::ERROR_INSUFFICIENT_BUFFER;
        return r;
    }
    r.written = *normalized;
    if (invalid) {
        r.ret = -static_cast<i32>(invalid->index);
        r.last_error = ERROR_NO_UNICODE_TRANSLATION;
        return r;
    }
    r.ret = static_cast<i32>(normalized->size());
    return r;
}

StringResult is_normalized_string(i32 form, const StringArgs& args) {
    if (form == kWindowsIdnaForm) log_unimplemented_form_13("IsNormalizedString");
    const UNormalizer2* norm = normalizer_for(form);
    if (!norm || args.src_null || args.src_len < -1) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    StringResult r;
    const std::u16string_view src(args.src);
    const auto invalid = find_invalid(src);
    const auto prefix = src.substr(0, invalid ? invalid->prefix_end : src.size());
    const auto checked = is_normalized_v11(norm, prefix);
    if (!checked) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    const bool normalized = *checked;
    // Whichever comes first decides: an unnormalized sequence before the
    // invalid unit is a plain FALSE.
    if (!normalized) return r;
    if (invalid) return fail(ERROR_NO_UNICODE_TRANSLATION);
    r.ret = 1;
    return r;
}

StringResult idn_convert(IdnApi api, u32 flags, const StringArgs& args) {
    const u32 accepted = IDN_ALLOW_UNASSIGNED | IDN_USE_STD3_ASCII_RULES | IDN_EMAIL_ADDRESS |
                         (api == IdnApi::ToUnicode ? IDN_RAW_PUNYCODE : 0u);
    if ((flags & ~accepted) != 0) return fail(error_codes::ERROR_INVALID_FLAGS);
    if (args.src_null || args.src_len < -1 || args.dst_len < 0 ||
        (args.dst_null && args.dst_len > 0)) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    std::u16string_view text(args.src);
    const bool terminated = !text.empty() && text.back() == u'\0';
    if (terminated) text.remove_suffix(1);
    if (text.empty() || text.find(u'\0') != std::u16string_view::npos) {
        return fail(error_codes::ERROR_INVALID_NAME);
    }
    auto converted = convert_text(api, flags, text);
    if (!converted) return fail(error_codes::ERROR_INVALID_NAME);
    if (terminated) converted->push_back(u'\0');
    StringResult r;
    r.ret = static_cast<i32>(converted->size());
    if (args.dst_len == 0) return r;
    if (static_cast<usize>(args.dst_len) < converted->size()) {
        return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
    }
    r.written = std::move(*converted);
    return r;
}

// ---------------------------------------------------------------------------
// Guest thunks.
// ---------------------------------------------------------------------------

namespace {

/// Longest NUL-terminated input read for a -1 length; beyond this the string
/// is taken as ending where the read stopped.
constexpr i32 kMaxTerminatedUnits = 1 << 24;

[[nodiscard]] StringArgs read_args(Marshaler& m, GuestAddr src, i32 src_len, GuestAddr dst,
                                   i32 dst_len) {
    StringArgs a;
    a.src_null = src == 0;
    a.src_len = src_len;
    a.dst_null = dst == 0;
    a.dst_len = dst_len;
    if (a.src_null) return a;
    const i32 limit = src_len == -1 ? kMaxTerminatedUnits : src_len;
    for (i32 i = 0; i < limit; ++i) {
        const auto unit = m.read_u16(src + static_cast<u64>(i) * 2);
        if (!unit.is_ok()) break;
        a.src.push_back(static_cast<char16_t>(unit.value()));
        if (src_len == -1 && unit.value() == 0) break;
    }
    return a;
}

void apply(Marshaler& m, GuestAddr dst, const StringResult& r) {
    for (usize i = 0; i < r.written.size(); ++i) {
        (void)m.write_u16(dst + static_cast<u64>(i) * 2, static_cast<u16>(r.written[i]));
    }
    if (r.last_error) set_last_error(*r.last_error);
    m.set_return(static_cast<u32>(r.ret));
}

ExportEntry idn_entry(const char* name, IdnApi api) {
    return ExportEntry{
        name, 0,
        [api](Marshaler& m) {
            const u32 flags = static_cast<u32>(m.arg(0));
            const GuestAddr src = m.arg_ptr(1);
            const i32 src_len = static_cast<i32>(m.arg(2));
            const GuestAddr dst = m.arg_ptr(3);
            const i32 dst_len = static_cast<i32>(m.arg(4));
            apply(m, dst, idn_convert(api, flags, read_args(m, src, src_len, dst, dst_len)));
        },
        20};
}

} // namespace

DllExports create_normalization_thunks() {
    DllExports exports;
    // int IdnToAscii(DWORD, LPCWSTR, int, LPWSTR, int) and its two siblings.
    exports.entries.push_back(idn_entry("IdnToAscii", IdnApi::ToAscii));
    exports.entries.push_back(idn_entry("IdnToNameprepUnicode", IdnApi::ToNameprepUnicode));
    exports.entries.push_back(idn_entry("IdnToUnicode", IdnApi::ToUnicode));
    // BOOL IsNormalizedString(NORM_FORM, LPCWSTR, int)
    exports.entries.push_back(ExportEntry{
        "IsNormalizedString", 0,
        [](Marshaler& m) {
            const i32 form = static_cast<i32>(m.arg(0));
            const GuestAddr src = m.arg_ptr(1);
            const i32 src_len = static_cast<i32>(m.arg(2));
            apply(m, 0, is_normalized_string(form, read_args(m, src, src_len, 0, 0)));
        },
        12});
    // int NormalizeString(NORM_FORM, LPCWSTR, int, LPWSTR, int)
    exports.entries.push_back(ExportEntry{
        "NormalizeString", 0,
        [](Marshaler& m) {
            const i32 form = static_cast<i32>(m.arg(0));
            const GuestAddr src = m.arg_ptr(1);
            const i32 src_len = static_cast<i32>(m.arg(2));
            const GuestAddr dst = m.arg_ptr(3);
            const i32 dst_len = static_cast<i32>(m.arg(4));
            apply(m, dst, normalize_string(form, read_args(m, src, src_len, dst, dst_len)));
        },
        20});
    return exports;
}

u16 normaliz_ordinal(std::string_view name) noexcept {
    // Normaliz.dll export table (Windows 11, x86 and x64): ordinal base 1.
    if (name == "IdnToAscii") return 1;
    if (name == "IdnToNameprepUnicode") return 2;
    if (name == "IdnToUnicode") return 3;
    if (name == "IsNormalizedString") return 4;
    if (name == "NormalizeString") return 5;
    return 0;
}

} // namespace vitreo::winapi::nls
