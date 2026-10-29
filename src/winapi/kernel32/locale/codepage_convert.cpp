// SPDX-License-Identifier: LGPL-2.1-only

/// @file codepage_convert.cpp
/// @brief Measured MultiByteToWideChar / WideCharToMultiByte. See codepage_convert.hpp.

#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_set>

namespace vitreo::winapi::codepage {
namespace {

constexpr u32 ERROR_INVALID_PARAMETER = 87;
constexpr u32 ERROR_INSUFFICIENT_BUFFER = 122;
constexpr u32 ERROR_INVALID_FLAGS = 1004;
constexpr u32 ERROR_NO_UNICODE_TRANSLATION = 1113;

constexpr u32 MB_PRECOMPOSED = 0x1;
constexpr u32 MB_COMPOSITE = 0x2;
constexpr u32 MB_USEGLYPHCHARS = 0x4;
constexpr u32 MB_ERR_INVALID_CHARS = 0x8;

constexpr u32 WC_DISCARDNS = 0x10;
constexpr u32 WC_SEPCHARS = 0x20;
constexpr u32 WC_DEFAULTCHAR = 0x40;
constexpr u32 WC_ERR_INVALID_CHARS = 0x80;
constexpr u32 WC_COMPOSITECHECK = 0x200;
constexpr u32 WC_NO_BEST_FIT_CHARS = 0x400;

struct EncodeException {
    u16 unit;
    u16 value;  ///< low byte = output byte, bit 8 = lpUsedDefaultChar
};

struct SingleBytePage {
    u32 code_page;
    u8 default_char;
    std::array<u16, 256> decode;
    std::vector<EncodeException> best_fit;     ///< sorted by unit
    std::vector<EncodeException> no_best_fit;  ///< sorted by unit
    std::vector<std::pair<u16, u8>> inverse;   ///< plain inverse, sorted
};

// The generated data: each page is its decode table plus the encodes that
// differ from "exact byte, else the default character with used set".
#define CODEPAGE_BEGIN(cp, dc) { SingleBytePage p{cp, dc, {}, {}, {}, {}};
#define DECODE(...) p.decode = {__VA_ARGS__};
#define BESTFIT(...) p.best_fit = {__VA_ARGS__};
#define NOBESTFIT(...) p.no_best_fit = {__VA_ARGS__};
#define CODEPAGE_END(cp) pages.push_back(std::move(p)); }

std::vector<SingleBytePage> build_pages() {
    std::vector<SingleBytePage> pages;
#include "winapi/kernel32/locale/codepage_tables.inc"
    for (auto& page : pages) {
        std::array<i32, 65536> first{};
        first.fill(-1);
        for (i32 b = 255; b >= 0; --b) first[page.decode[static_cast<usize>(b)]] = b;
        for (u32 u = 0; u < 65536; ++u) {
            if (first[u] >= 0) {
                page.inverse.emplace_back(static_cast<u16>(u),
                                          static_cast<u8>(first[u]));
            }
        }
    }
    return pages;
}

#undef CODEPAGE_BEGIN
#undef DECODE
#undef BESTFIT
#undef NOBESTFIT
#undef CODEPAGE_END

const std::vector<SingleBytePage>& pages() {
    static const std::vector<SingleBytePage> all = build_pages();
    return all;
}

const SingleBytePage* find_page(u32 cp) {
    for (const auto& page : pages()) {
        if (page.code_page == cp) return &page;
    }
    return nullptr;
}

struct DecodeOverride {
    u32 code_page;
    u8 byte;
    u32 flag;
    u32 count;
    u16 units[3];
};
constexpr DecodeOverride kDecodeOverrides[] = {
#include "winapi/kernel32/locale/codepage_decode_flags.inc"
};

const DecodeOverride* find_override(u32 cp, u8 byte, u32 flag) {
    for (const auto& o : kDecodeOverrides) {
        if (o.code_page == cp && o.byte == byte && o.flag == flag) return &o;
    }
    return nullptr;
}

/// Encode one unit on a single-byte page: {byte, used_default}.
std::pair<u8, bool> encode_unit(const SingleBytePage& page, u16 unit,
                                bool best_fit) {
    const auto& ex = best_fit ? page.best_fit : page.no_best_fit;
    auto it = std::lower_bound(ex.begin(), ex.end(), unit,
        [](const EncodeException& e, u16 u) { return e.unit < u; });
    if (it != ex.end() && it->unit == unit) {
        return {static_cast<u8>(it->value & 0xFF), (it->value & 0x100) != 0};
    }
    auto inv = std::lower_bound(page.inverse.begin(), page.inverse.end(), unit,
        [](const std::pair<u16, u8>& e, u16 u) { return e.first < u; });
    if (inv != page.inverse.end() && inv->first == unit) return {inv->second, false};
    return {page.default_char, true};
}

void log_unimplemented_once(u32 cp, const char* what) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::lock_guard<std::mutex> lock(mutex);
    const u64 key = (static_cast<u64>(cp) << 8) ^ reinterpret_cast<uintptr_t>(what);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!%s: code page %u\n",
                 what, cp);
}

enum class Kind { SingleByte, Utf8, Symbol, Unimplemented, Invalid };

/// Code pages Windows installs that Vitreo does not convert yet (measured list
/// from EnumSystemCodePages on Windows, minus the single-byte ones).
bool installed_but_unimplemented(u32 cp) {
    switch (cp) {
        case kCpUtf7:
        case 932: case 936: case 949: case 950: case 1361:
        case 10001: case 10002: case 10003: case 10008:
        case 20000: case 20001: case 20002: case 20003: case 20004: case 20005:
        case 20261: case 20932: case 20936: case 20949:
        case 50220: case 50221: case 50222: case 50225: case 50227: case 50229:
        case 51949: case 52936: case 54936:
        case 55000: case 55001: case 55002: case 55003: case 55004:
        case 57002: case 57003: case 57004: case 57005: case 57006:
        case 57007: case 57008: case 57009: case 57010: case 57011:
            return true;
        default:
            return false;
    }
}

Kind classify(u32 cp, const SingleBytePage** page) {
    *page = nullptr;
    if (cp == kCpUtf8) return Kind::Utf8;
    if (cp == kCpSymbol) return Kind::Symbol;
    if (const auto* p = find_page(cp)) {
        *page = p;
        return Kind::SingleByte;
    }
    return installed_but_unimplemented(cp) ? Kind::Unimplemented : Kind::Invalid;
}

Result fail(u32 error) { return Result{0, error, false, 0}; }

/// Measured (string_conversion_probe: utf8-err-invalid, utf8-lonehi-err): a
/// strict UTF-8 failure leaves the units converted before the invalid sequence
/// in the output buffer.
Result strict_fail(u32 count, usize cap) {
    return Result{0, ERROR_NO_UNICODE_TRANSLATION, false, cap != 0 ? count : 0u};
}

/// Append one output unit, honouring capacity. Returns false once full.
template <typename T>
bool emit(T* dst, usize capacity, u32& count, T value, bool& overflow) {
    if (capacity != 0) {
        if (count >= capacity) { overflow = true; return false; }
        dst[count] = value;
    }
    ++count;
    return true;
}

// -- UTF-8 -----------------------------------------------------------------
// Matches all 40,000 cases of tools/oracle_probe/utf8_corpus_probe.c. An
// invalid sequence becomes one U+FFFD covering the lead and every byte already
// accepted -- including a second byte that is a continuation byte but outside
// the lead's permitted range (E0 80, ED A0, F0 80, F4 90), which is consumed.
Result utf8_decode(bool strict, const u8* src, usize n, u16* dst, usize cap) {
    u32 count = 0;
    bool overflow = false;
    usize i = 0;
    auto put = [&](u16 v) { return emit(dst, cap, count, v, overflow); };
    while (i < n) {
        const u8 c = src[i];
        if (c < 0x80) {
            if (!put(c)) break;
            ++i;
            continue;
        }
        u32 need = 0, cp = 0;
        u8 lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1; cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2; cp = c & 0x0F;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3; cp = c & 0x07;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            if (strict) return strict_fail(count, cap);
            if (!put(0xFFFD)) break;
            ++i;
            continue;
        }
        usize j = i + 1;
        bool ok = true;
        for (u32 k = 0; k < need; ++k) {
            if (j >= n || src[j] < 0x80 || src[j] > 0xBF) { ok = false; break; }
            if (k == 0 && (src[j] < lo || src[j] > hi)) { ++j; ok = false; break; }
            cp = (cp << 6) | (src[j] & 0x3F);
            ++j;
        }
        if (!ok) {
            if (strict) return strict_fail(count, cap);
            if (!put(0xFFFD)) break;
            i = j;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            // A pair is written whole or not at all.
            if (cap != 0 && count + 2 > cap) { overflow = true; break; }
            put(static_cast<u16>(0xD800 | (cp >> 10)));
            put(static_cast<u16>(0xDC00 | (cp & 0x3FF)));
        } else if (!put(static_cast<u16>(cp))) {
            break;
        }
        i = j;
    }
    if (overflow) return Result{0, ERROR_INSUFFICIENT_BUFFER, false, count};
    return Result{count, 0, false, 0};
}

Result utf8_encode(bool strict, const u16* src, usize n, u8* dst, usize cap) {
    u32 count = 0;
    bool overflow = false;
    usize i = 0;
    while (i < n) {
        u32 cp = src[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n &&
            src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (src[i + 1] - 0xDC00u);
            i += 2;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            if (strict) return strict_fail(count, cap);
            cp = 0xFFFD;
            ++i;
        } else {
            ++i;
        }
        u8 bytes[4];
        u32 len = 0;
        if (cp < 0x80) {
            bytes[len++] = static_cast<u8>(cp);
        } else if (cp < 0x800) {
            bytes[len++] = static_cast<u8>(0xC0 | (cp >> 6));
            bytes[len++] = static_cast<u8>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            bytes[len++] = static_cast<u8>(0xE0 | (cp >> 12));
            bytes[len++] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
            bytes[len++] = static_cast<u8>(0x80 | (cp & 0x3F));
        } else {
            bytes[len++] = static_cast<u8>(0xF0 | (cp >> 18));
            bytes[len++] = static_cast<u8>(0x80 | ((cp >> 12) & 0x3F));
            bytes[len++] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
            bytes[len++] = static_cast<u8>(0x80 | (cp & 0x3F));
        }
        if (cap != 0 && count + len > cap) { overflow = true; break; }
        for (u32 k = 0; k < len; ++k) {
            if (cap != 0) dst[count] = bytes[k];
            ++count;
        }
    }
    if (overflow) return Result{0, ERROR_INSUFFICIENT_BUFFER, false, count};
    return Result{count, 0, false, 0};
}

/// Units that are the second element of some MB_COMPOSITE decomposition on
/// this page -- the combining marks WC_COMPOSITECHECK would try to compose.
bool is_composable_mark(u32 cp, u16 unit) {
    for (const auto& o : kDecodeOverrides) {
        if (o.code_page != cp || o.flag != MB_COMPOSITE) continue;
        for (u32 k = 1; k < o.count; ++k) {
            if (o.units[k] == unit) return true;
        }
    }
    return false;
}

} // namespace

std::atomic<u32> g_ansi_code_page{kAnsiCodePage};

u32 ansi_code_page() noexcept {
    return g_ansi_code_page.load(std::memory_order_relaxed);
}

void set_ansi_code_page(u32 code_page) noexcept {
    // Only 1252 (legacy ACP) and 65001 (manifest UTF-8) are process ACP values
    // Vitreo models. Other values fall back to the en-US default.
    if (code_page != kAnsiCodePage && code_page != kCpUtf8) {
        code_page = kAnsiCodePage;
    }
    g_ansi_code_page.store(code_page, std::memory_order_relaxed);
}

u32 resolve_pseudo(u32 code_page) noexcept {
    switch (code_page) {
        case kCpAcp: return ansi_code_page();
        case kCpOemcp: return kOemCodePage;
        case kCpMaccp: return kMacCodePage;
        case kCpThreadAcp: return ansi_code_page();
        default: return code_page;
    }
}

Result multi_byte_to_wide(u32 code_page, u32 flags, const u8* src,
                          usize src_len, u16* dst, usize cap) {
    const u32 cp = resolve_pseudo(code_page);
    const SingleBytePage* page = nullptr;
    const Kind kind = classify(cp, &page);
    switch (kind) {
        case Kind::Invalid:
            return fail(ERROR_INVALID_PARAMETER);
        case Kind::Unimplemented:
            log_unimplemented_once(cp, "MultiByteToWideChar");
            return fail(ERROR_INVALID_PARAMETER);
        case Kind::Symbol: {
            if (flags != 0) return fail(ERROR_INVALID_FLAGS);
            u32 count = 0;
            bool overflow = false;
            for (usize i = 0; i < src_len; ++i) {
                const u16 v = src[i] == 0 ? 0 : static_cast<u16>(0xF000 | src[i]);
                if (!emit(dst, cap, count, v, overflow)) break;
            }
            if (overflow) return Result{0, ERROR_INSUFFICIENT_BUFFER, false, count};
            return Result{count, 0, false, 0};
        }
        case Kind::Utf8:
            // Measured: every combination of the four MB_* bits is accepted.
            if ((flags & ~0xFu) != 0) return fail(ERROR_INVALID_FLAGS);
            return utf8_decode((flags & MB_ERR_INVALID_CHARS) != 0, src,
                               src_len, dst, cap);
        case Kind::SingleByte:
            break;
    }
    // Single-byte: MB_PRECOMPOSED and MB_COMPOSITE are mutually exclusive.
    if ((flags & ~0xFu) != 0 ||
        (flags & (MB_PRECOMPOSED | MB_COMPOSITE)) ==
            (MB_PRECOMPOSED | MB_COMPOSITE)) {
        return fail(ERROR_INVALID_FLAGS);
    }
    u32 count = 0;
    bool overflow = false;
    for (usize i = 0; i < src_len && !overflow; ++i) {
        const u8 b = src[i];
        const DecodeOverride* o = nullptr;
        if ((flags & MB_USEGLYPHCHARS) != 0) o = find_override(cp, b, MB_USEGLYPHCHARS);
        if (o == nullptr && (flags & MB_COMPOSITE) != 0) o = find_override(cp, b, MB_COMPOSITE);
        if (o != nullptr) {
            if (cap != 0 && count + o->count > cap) { overflow = true; break; }
            for (u32 k = 0; k < o->count; ++k) emit(dst, cap, count, o->units[k], overflow);
        } else {
            emit(dst, cap, count, page->decode[b], overflow);
        }
    }
    if (overflow) return Result{0, ERROR_INSUFFICIENT_BUFFER, false, count};
    return Result{count, 0, false, 0};
}

Result wide_to_multi_byte(u32 code_page, u32 flags, const u16* src,
                          usize src_len, u8* dst, usize cap,
                          const u8* default_char, bool want_used_default) {
    const u32 cp = resolve_pseudo(code_page);
    const SingleBytePage* page = nullptr;
    const Kind kind = classify(cp, &page);
    switch (kind) {
        case Kind::Invalid:
            return fail(ERROR_INVALID_PARAMETER);
        case Kind::Unimplemented:
            log_unimplemented_once(cp, "WideCharToMultiByte");
            return fail(ERROR_INVALID_PARAMETER);
        case Kind::Symbol: {
            if (flags != 0) return fail(ERROR_INVALID_FLAGS);
            if (default_char != nullptr || want_used_default) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            u32 count = 0;
            bool overflow = false;
            for (usize i = 0; i < src_len; ++i) {
                const u16 u = src[i];
                if (u != 0 && (u < 0xF000 || u > 0xF0FF)) {
                    return fail(ERROR_NO_UNICODE_TRANSLATION);
                }
                if (!emit(dst, cap, count, static_cast<u8>(u & 0xFF), overflow)) break;
            }
            if (overflow) return Result{0, ERROR_INSUFFICIENT_BUFFER, false, count};
            return Result{count, 0, false, 0};
        }
        case Kind::Utf8: {
            // Measured: DISCARDNS, SEPCHARS, DEFAULTCHAR, ERR_INVALID_CHARS,
            // COMPOSITECHECK and NO_BEST_FIT_CHARS are all accepted; the
            // default-character arguments are accepted and ignored, and
            // lpUsedDefaultChar reports FALSE.
            constexpr u32 kUtf8Flags = WC_DISCARDNS | WC_SEPCHARS | WC_DEFAULTCHAR |
                                       WC_ERR_INVALID_CHARS | WC_COMPOSITECHECK |
                                       WC_NO_BEST_FIT_CHARS;
            if ((flags & ~kUtf8Flags) != 0) return fail(ERROR_INVALID_FLAGS);
            return utf8_encode((flags & WC_ERR_INVALID_CHARS) != 0, src, src_len,
                               dst, cap);
        }
        case Kind::SingleByte:
            break;
    }
    // Single-byte: WC_ERR_INVALID_CHARS is UTF-8 only; the three sub-flags
    // are accepted only alongside WC_COMPOSITECHECK.
    constexpr u32 kSubFlags = WC_DISCARDNS | WC_SEPCHARS | WC_DEFAULTCHAR;
    if ((flags & ~(kSubFlags | WC_COMPOSITECHECK | WC_NO_BEST_FIT_CHARS)) != 0 ||
        ((flags & kSubFlags) != 0 && (flags & WC_COMPOSITECHECK) == 0)) {
        return fail(ERROR_INVALID_FLAGS);
    }
    if ((flags & WC_COMPOSITECHECK) != 0) {
        for (usize i = 0; i < src_len; ++i) {
            if (is_composable_mark(cp, src[i])) {
                // Composition is only a no-op when nothing can compose; how
                // Windows composes and what the sub-flags then do is not
                // measured yet.
                log_unimplemented_once(cp, "WideCharToMultiByte(WC_COMPOSITECHECK)");
                return fail(ERROR_INVALID_PARAMETER);
            }
        }
    }
    const bool best_fit = (flags & WC_NO_BEST_FIT_CHARS) == 0;
    u32 count = 0;
    bool overflow = false;
    bool used = false;
    for (usize i = 0; i < src_len; ++i) {
        auto [byte, used_default] = encode_unit(*page, src[i], best_fit);
        if (used_default) {
            used = true;
            if (default_char != nullptr) byte = *default_char;
        }
        if (!emit(dst, cap, count, byte, overflow)) break;
    }
    if (overflow) return Result{0, ERROR_INSUFFICIENT_BUFFER, used, count};
    return Result{count, 0, used, 0};
}

bool is_valid_code_page(u32 code_page) {
    // The pseudo code pages are not code pages (measured: FALSE for 0-3, 42).
    if (code_page == kCpUtf8) return true;
    return find_page(code_page) != nullptr;
}

bool code_page_info(u32 code_page, CpInfo& out) {
    const u32 cp = resolve_pseudo(code_page);
    out = CpInfo{};
    out.code_page = cp;
    if (cp == kCpUtf8) {
        out.max_char_size = 4;
        out.default_char[0] = '?';
        out.unicode_default_char = 0xFFFD;
        return true;
    }
    const auto* page = find_page(cp);
    if (page == nullptr) return false;
    out.max_char_size = 1;
    out.default_char[0] = page->default_char;
    out.unicode_default_char = page->decode[page->default_char];
    return true;
}

} // namespace vitreo::winapi::codepage
