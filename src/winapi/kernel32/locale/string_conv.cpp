/// @file string_conv.cpp
/// @brief MultiByteToWideChar / WideCharToMultiByte implementation (WIN32-034)

#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstring>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/// Return true if code_page is one we support (CP_ACP, CP_OEMCP, 1252, CP_UTF8).
static bool is_supported_codepage(u32 code_page) {
    return code_page == CP_ACP || code_page == CP_OEMCP ||
           code_page == CP_1252 || code_page == CP_UTF8;
}

/// True if @p code_page should be decoded/encoded as CP-1252 (single-byte).
static inline bool is_cp1252_like(u32 code_page) {
    return code_page == CP_ACP || code_page == CP_OEMCP || code_page == CP_1252;
}

/// Windows-1252 → Unicode mapping for bytes 0x80–0x9F.
/// Bytes 0xA0–0xFF map directly to U+00A0–U+00FF.
static constexpr u16 cp1252_0x80_0x9F[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, // 88-8F
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, // 98-9F
};

/// Map a CP-1252 byte to a Unicode codepoint.
inline u16 cp1252_to_unicode(u8 b) {
    if (b < 0x80) return b;
    if (b <= 0x9F) return cp1252_0x80_0x9F[b - 0x80];
    return b; // 0xA0–0xFF map to U+00A0–U+00FF directly
}

/// Map a Unicode codepoint back to CP-1252. Returns '?' if unmappable.
inline u8 unicode_to_cp1252(u16 wc) {
    if (wc < 0x80) return static_cast<u8>(wc);
    if (wc >= 0xA0 && wc <= 0xFF) return static_cast<u8>(wc);
    // Search the 0x80-0x9F mapping table
    for (int i = 0; i < 32; ++i) {
        if (cp1252_0x80_0x9F[i] == wc) return static_cast<u8>(0x80 + i);
    }
    return '?';
}

/// Compute the number of UTF-16LE code units required for the UTF-8 input.
/// Returns -1 on invalid input.
int utf8_to_utf16_count(const char* src, int byte_count) {
    int count = 0;
    int i = 0;
    while (i < byte_count) {
        auto b = static_cast<u8>(src[i]);
        u32 cp = 0;
        int seq_len = 0;

        if (b < 0x80) {
            cp = b;
            seq_len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= byte_count) return -1;
            cp = static_cast<u32>(b & 0x1F) << 6;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 1]) & 0x3F);
            seq_len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= byte_count) return -1;
            cp = static_cast<u32>(b & 0x0F) << 12;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 1]) & 0x3F) << 6;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 2]) & 0x3F);
            seq_len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            if (i + 3 >= byte_count) return -1;
            cp = static_cast<u32>(b & 0x07) << 18;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 1]) & 0x3F) << 12;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 2]) & 0x3F) << 6;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 3]) & 0x3F);
            seq_len = 4;
        } else {
            return -1; // invalid leading byte
        }

        // Codepoints >= 0x10000 require a surrogate pair (2 code units)
        count += (cp >= 0x10000) ? 2 : 1;
        i += seq_len;
    }
    return count;
}

/// Convert UTF-8 to UTF-16LE into the provided buffer.
/// Caller must ensure the buffer is large enough.
/// Returns number of u16 code units written.
int utf8_to_utf16_convert(const char* src, int byte_count, u16* dst) {
    int out = 0;
    int i = 0;
    while (i < byte_count) {
        auto b = static_cast<u8>(src[i]);
        u32 cp = 0;
        int seq_len = 0;

        if (b < 0x80) {
            cp = b;
            seq_len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            cp = static_cast<u32>(b & 0x1F) << 6;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 1]) & 0x3F);
            seq_len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = static_cast<u32>(b & 0x0F) << 12;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 1]) & 0x3F) << 6;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 2]) & 0x3F);
            seq_len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = static_cast<u32>(b & 0x07) << 18;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 1]) & 0x3F) << 12;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 2]) & 0x3F) << 6;
            cp |= static_cast<u32>(static_cast<u8>(src[i + 3]) & 0x3F);
            seq_len = 4;
        } else {
            break; // should never happen if count succeeded
        }

        if (cp >= 0x10000) {
            // Surrogate pair
            cp -= 0x10000;
            dst[out++] = static_cast<u16>(0xD800 | ((cp >> 10) & 0x3FF));
            dst[out++] = static_cast<u16>(0xDC00 | (cp & 0x3FF));
        } else {
            dst[out++] = static_cast<u16>(cp);
        }
        i += seq_len;
    }
    return out;
}

/// Compute the number of UTF-8 bytes required for the UTF-16LE input.
/// Returns -1 on invalid input.
int utf16_to_utf8_count(const u16* src, int unit_count) {
    int count = 0;
    int i = 0;
    while (i < unit_count) {
        u32 cp = src[i];

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate – must be followed by a low surrogate
            if (i + 1 >= unit_count) return -1;
            u32 lo = src[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF) return -1;
            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
            i += 2;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return -1; // lone low surrogate
        } else {
            i += 1;
        }

        if (cp < 0x80) {
            count += 1;
        } else if (cp < 0x800) {
            count += 2;
        } else if (cp < 0x10000) {
            count += 3;
        } else {
            count += 4;
        }
    }
    return count;
}

/// Convert UTF-16LE to UTF-8 into the provided buffer.
/// Caller must ensure the buffer is large enough.
/// Returns number of bytes written.
int utf16_to_utf8_convert(const u16* src, int unit_count, char* dst) {
    int out = 0;
    int i = 0;
    while (i < unit_count) {
        u32 cp = src[i];

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            u32 lo = src[i + 1];
            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
            i += 2;
        } else {
            i += 1;
        }

        if (cp < 0x80) {
            dst[out++] = static_cast<char>(cp);
        } else if (cp < 0x800) {
            dst[out++] = static_cast<char>(0xC0 | (cp >> 6));
            dst[out++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            dst[out++] = static_cast<char>(0xE0 | (cp >> 12));
            dst[out++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            dst[out++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            dst[out++] = static_cast<char>(0xF0 | (cp >> 18));
            dst[out++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            dst[out++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            dst[out++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // anonymous namespace

// =============================================================================
// StringConverter
// =============================================================================

int StringConverter::multi_byte_to_wide_char(
    u32 code_page, u32 /*flags*/,
    const char* mb_str, int cb_multibyte,
    u16* wide_str, int cch_wide_char)
{
    if (!is_supported_codepage(code_page)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // Windows mbwc_arg_probe: null source, length 0, null dest with nonzero
    // cch, or overlapping buffers → ERROR_INVALID_PARAMETER.
    if (mb_str == nullptr || cb_multibyte == 0 || cch_wide_char < 0 ||
        (cch_wide_char > 0 && wide_str == nullptr) ||
        (cch_wide_char > 0 &&
         reinterpret_cast<const void*>(wide_str) ==
             static_cast<const void*>(mb_str))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // Any negative length is null-terminated (not only -1).
    int byte_count = cb_multibyte;
    if (byte_count < 0) {
        byte_count = static_cast<int>(std::strlen(mb_str)) + 1; // include null
    }

    // Compute required output size
    int required = utf8_to_utf16_count(mb_str, byte_count);
    if (required < 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // Query mode: cchWideChar == 0 only (NULL dest + nonzero is invalid above).
    if (cch_wide_char == 0) {
        return required;
    }

    // Buffer too small
    if (cch_wide_char < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    return utf8_to_utf16_convert(mb_str, byte_count, wide_str);
}

int StringConverter::wide_char_to_multi_byte(
    u32 code_page, u32 /*flags*/,
    const u16* wide_str, int cch_wide_char,
    char* mb_str, int cb_multibyte,
    const char* /*default_char*/,
    bool* used_default_char)
{
    if (!is_supported_codepage(code_page)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (wide_str == nullptr || cch_wide_char == 0 || cb_multibyte < 0 ||
        (cb_multibyte > 0 && mb_str == nullptr) ||
        (cb_multibyte > 0 &&
         static_cast<const void*>(mb_str) ==
             static_cast<const void*>(wide_str))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // UTF-8 never uses a default character
    if (used_default_char != nullptr) {
        *used_default_char = false;
    }

    // Any negative length is null-terminated (Windows mbwc_arg_probe).
    int unit_count = cch_wide_char;
    if (unit_count < 0) {
        unit_count = 0;
        while (wide_str[unit_count] != 0) {
            ++unit_count;
        }
        ++unit_count; // include null terminator
    }

    // Compute required output size
    int required = utf16_to_utf8_count(wide_str, unit_count);
    if (required < 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // Query mode: cbMultiByte == 0 only.
    if (cb_multibyte == 0) {
        return required;
    }

    // Buffer too small
    if (cb_multibyte < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    return utf16_to_utf8_convert(wide_str, unit_count, mb_str);
}

// =============================================================================
// Host-Level Helpers
// =============================================================================

std::string utf16_to_utf8(const u16* wide, int len) {
    if (wide == nullptr) {
        return {};
    }

    int unit_count = len;
    if (unit_count == -1) {
        unit_count = 0;
        while (wide[unit_count] != 0) {
            ++unit_count;
        }
    }

    int required = utf16_to_utf8_count(wide, unit_count);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<usize>(required), '\0');
    utf16_to_utf8_convert(wide, unit_count, result.data());
    return result;
}

std::vector<u16> utf8_to_utf16(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }

    int byte_count = static_cast<int>(utf8.size());
    int required = utf8_to_utf16_count(utf8.data(), byte_count);
    if (required <= 0) {
        return {};
    }

    std::vector<u16> result(static_cast<usize>(required));
    utf8_to_utf16_convert(utf8.data(), byte_count, result.data());
    return result;
}

std::string utf8_to_acp(std::string_view utf8) {
    // Learn GetCommandLineA / CP_ACP: WideCharToMultiByte with best-fit (e.g.
    // U+0100 → 'A'). Do not use the identity+0x80..0x9F helper, which returns
    // '?' for every best-fit scalar (Windows cp1252_bestfit_probe).
    const std::vector<u16> wide = utf8_to_utf16(utf8);
    if (wide.empty()) {
        return {};
    }
    auto need = codepage::wide_to_multi_byte(
        codepage::kCpAcp, /*flags=*/0, wide.data(), wide.size(),
        /*dst=*/nullptr, /*dst_capacity=*/0, /*default_char=*/nullptr,
        /*want_used_default=*/false);
    if (!need.ok() || need.count == 0) {
        return {};
    }
    std::string acp(static_cast<usize>(need.count), '\0');
    auto wrote = codepage::wide_to_multi_byte(
        codepage::kCpAcp, 0, wide.data(), wide.size(),
        reinterpret_cast<u8*>(acp.data()), acp.size(), nullptr, false);
    if (!wrote.ok()) {
        return {};
    }
    acp.resize(static_cast<usize>(wrote.count));
    return acp;
}

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

namespace {

/// Read a guest string: exactly @p len units, or up to and including the
/// terminator when @p len is -1. False if the memory is unreadable.
template <typename T>
bool read_guest_units(GuestMemory& mem, GuestAddr at, i32 len,
                      std::vector<T>& out) {
    out.clear();
    if (len >= 0) {
        out.resize(static_cast<usize>(len));
        return len == 0 ||
               mem.read_bytes(at, reinterpret_cast<u8*>(out.data()),
                              out.size() * sizeof(T)).is_ok();
    }
    for (GuestAddr p = at;; p += sizeof(T)) {
        T unit{};
        if (mem.read_bytes(p, reinterpret_cast<u8*>(&unit), sizeof(T)).is_err()) {
            return false;
        }
        out.push_back(unit);
        if (unit == 0) return true;
    }
}

constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorNoAccess = 998;

} // namespace

// Learn MultiByteToWideChar, with every behaviour Learn leaves open measured
// on Windows (src/winapi/kernel32/locale/codepage_convert.hpp lists the
// probes). The conversion itself lives in codepage_convert.cpp.
void impl_MultiByteToWideChar(Marshaler& m) {
    const u32 code_page = static_cast<u32>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(1));
    const GuestAddr src_ptr = m.arg_ptr(2);
    const i32 src_len = static_cast<i32>(m.arg(3));
    const GuestAddr dst_ptr = m.arg_ptr(4);
    const i32 dst_len = static_cast<i32>(m.arg(5));
    auto fail = [&](u32 error) {
        set_last_error(error);
        m.set_return(0);
    };
    // Learn / Windows mbwc_arg_probe: cbMultiByte 0, NULL source, and
    // overlapping buffers fail ERROR_INVALID_PARAMETER. Any negative length
    // (not only -1) means null-terminated; Vitreo must not reject -2/-5.
    const i32 effective_src_len = src_len < 0 ? -1 : src_len;
    if (src_ptr == 0 || effective_src_len == 0 || dst_len < 0 ||
        (dst_len > 0 && dst_ptr == 0) || (dst_len > 0 && dst_ptr == src_ptr)) {
        fail(kErrorInvalidParameter);
        return;
    }
    std::vector<u8> src;
    if (!read_guest_units(m.memory(), src_ptr, effective_src_len, src)) {
        fail(kErrorNoAccess);
        return;
    }
    std::vector<u16> dst(static_cast<usize>(dst_len));
    const auto r = codepage::multi_byte_to_wide(code_page, flags, src.data(),
                                                src.size(), dst.data(),
                                                dst.size());
    const u32 stored = r.ok() ? (dst_len > 0 ? r.count : 0) : r.written;
    if (stored != 0) {
        (void)m.memory().write_bytes(dst_ptr, reinterpret_cast<const u8*>(dst.data()),
                                     stored * sizeof(u16));
    }
    if (!r.ok()) {
        fail(r.error);
        return;
    }
    m.set_return(r.count);
}

// Learn WideCharToMultiByte, measured the same way.
void impl_WideCharToMultiByte(Marshaler& m) {
    const u32 code_page = static_cast<u32>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(1));
    const GuestAddr src_ptr = m.arg_ptr(2);
    const i32 src_len = static_cast<i32>(m.arg(3));
    const GuestAddr dst_ptr = m.arg_ptr(4);
    const i32 dst_len = static_cast<i32>(m.arg(5));
    const GuestAddr default_ptr = m.arg_ptr(6);
    const GuestAddr used_ptr = m.arg_ptr(7);
    auto fail = [&](u32 error) {
        set_last_error(error);
        m.set_return(0);
    };
    // Same rules as MultiByteToWideChar (mbwc_arg_probe): any negative
    // cchWideChar is null-terminated; NULL lpMultiByteStr with nonzero
    // cbMultiByte is not a size query.
    const i32 effective_src_len = src_len < 0 ? -1 : src_len;
    if (src_ptr == 0 || effective_src_len == 0 || dst_len < 0 ||
        (dst_len > 0 && dst_ptr == 0) || (dst_len > 0 && dst_ptr == src_ptr)) {
        fail(kErrorInvalidParameter);
        return;
    }
    std::vector<u16> src;
    if (!read_guest_units(m.memory(), src_ptr, effective_src_len, src)) {
        fail(kErrorNoAccess);
        return;
    }
    u8 default_char = 0;
    if (default_ptr != 0) {
        auto d = m.memory().read_u8(default_ptr);
        if (d.is_err()) {
            fail(kErrorNoAccess);
            return;
        }
        default_char = d.value();
    }
    std::vector<u8> dst(static_cast<usize>(dst_len));
    const auto r = codepage::wide_to_multi_byte(
        code_page, flags, src.data(), src.size(), dst.data(), dst.size(),
        default_ptr != 0 ? &default_char : nullptr, used_ptr != 0);
    const u32 stored = r.ok() ? (dst_len > 0 ? r.count : 0) : r.written;
    if (stored != 0) (void)m.memory().write_bytes(dst_ptr, dst.data(), stored);
    // Measured: lpUsedDefaultChar is written on success and when the buffer
    // was too small, not when the arguments were rejected.
    if (used_ptr != 0 && (r.ok() || r.error == 122)) {
        (void)m.memory().write_u32(used_ptr, r.used_default ? 1u : 0u);
    }
    if (!r.ok()) {
        fail(r.error);
        return;
    }
    m.set_return(r.count);
}

// =============================================================================
// DLL Export Table
// =============================================================================

DllExports make_string_conv_exports() {
    DllExports exports;
    exports.entries.reserve(2);

    // MultiByteToWideChar (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "MultiByteToWideChar",
        1,
        impl_MultiByteToWideChar,
        24
    });

    // WideCharToMultiByte (8 args, stdcall_bytes=32)
    exports.entries.push_back(ExportEntry{
        "WideCharToMultiByte",
        2,
        impl_WideCharToMultiByte,
        32
    });

    return exports;
}

} // namespace vitreo::winapi
