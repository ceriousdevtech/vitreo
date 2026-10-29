// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_charset_conversion_registration.cpp
/// @brief mbstowcs / mbtowc / wctomb / wcstombs_s / btowc / __strncnt / _isctype /
///        iswctype.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/text/crt_wctype_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include <algorithm>

namespace vitreo::winapi {

void add_crt_charset_conversion_exports(DllExports& exports,
                                        CrtMemoryManager& mem_mgr) {
    // size_t mbstowcs(wchar_t* dst, const char* src, size_t count)
    //
    // Single-byte "C" locale: every byte maps to the code unit of the same
    // value. A null dst is the size query form and must not write anything.
    exports.entries.push_back({"mbstowcs", 0, [](Marshaler& m) {
        const GuestAddr dst = m.arg_ptr(0);
        const GuestAddr src = m.arg_ptr(1);
        const auto count = static_cast<usize>(m.arg(2));
        if (src == 0) {
            m.set_return(static_cast<u64>(~usize{0}));
            return;
        }
        usize written = 0;
        for (usize i = 0; dst == 0 || i < count; ++i) {
            auto byte = m.read_u8(src + i);
            if (byte.is_err()) {
                m.set_return(static_cast<u64>(~usize{0}));
                return;
            }
            if (dst != 0) {
                (void)m.write_u16(dst + i * 2, byte.value());
            }
            if (byte.value() == 0) break;
            ++written;
        }
        // Windows returns the character count excluding the terminator; when
        // the buffer filled exactly, no terminator was written.
        m.set_return(static_cast<u64>(written));
    }});

    // int mbtowc(wchar_t* wchar, const char* mbchar, size_t count)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbtowc-mbtowc-l
    // Single-byte "C" locale (MB_CUR_MAX == 1), matching mbstowcs. A NULL
    // mbchar returns 0. A NUL byte returns 0 and, when wchar is non-NULL,
    // stores L'\0'. count == 0 cannot form a character and returns -1.
    exports.entries.push_back({"mbtowc", 0, [](Marshaler& m) {
        const GuestAddr wchar = m.arg_ptr(0);
        const GuestAddr mbchar = m.arg_ptr(1);
        const auto count = static_cast<usize>(m.arg(2));
        if (mbchar == 0) {
            m.set_return(0);
            return;
        }
        if (count == 0) {
            m.set_return(static_cast<u64>(static_cast<i32>(-1)));
            return;
        }
        auto byte = m.read_u8(mbchar);
        if (byte.is_err()) {
            m.set_return(static_cast<u64>(static_cast<i32>(-1)));
            return;
        }
        if (wchar != 0) {
            (void)m.write_u16(wchar, byte.value());
        }
        m.set_return(byte.value() == 0 ? 0u : 1u);
    }});

    // int wctomb(char* mbchar, wchar_t wchar)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/wctomb-wctomb-l
    // Return-value table: a NULL mbchar returns 0 because the "C" locale is
    // not state-dependent. L'\0' is written and returns 1. A code unit that
    // does not fit in one byte returns -1 and sets errno to EILSEQ.
    exports.entries.push_back({"wctomb", 0, [&mem_mgr](Marshaler& m) {
        constexpr u32 EILSEQ_VALUE = 42;
        const GuestAddr mbchar = m.arg_ptr(0);
        const auto wchar = static_cast<u16>(m.arg(1));
        if (mbchar == 0) {
            m.set_return(0);
            return;
        }
        if (wchar > 0xFF) {
            mem_mgr.set_errno(EILSEQ_VALUE);
            m.set_return(static_cast<u64>(static_cast<i32>(-1)));
            return;
        }
        (void)m.write_u8(mbchar, static_cast<u8>(wchar));
        m.set_return(1);
    }});

    // errno_t wcstombs_s(size_t* converted, char* dst, size_t dst_bytes,
    //                    const wchar_t* src, size_t count)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/wcstombs-s-wcstombs-s-l
    // `*converted` counts the terminating NUL. A NULL dst with dst_bytes 0 is
    // the size query. count == _TRUNCATE truncates instead of failing; any
    // other overflow is ERANGE and leaves dst empty. Single-byte "C" locale,
    // matching wcstombs above.
    exports.entries.push_back({"wcstombs_s", 0, [](Marshaler& m) {
        constexpr u32 EINVAL_VALUE = 22;
        constexpr u32 ERANGE_VALUE = 34;
        constexpr u32 EILSEQ_VALUE = 42;
        constexpr usize TRUNCATE = static_cast<usize>(-1);  // _TRUNCATE

        const GuestAddr converted_ptr = m.arg_ptr(0);
        const GuestAddr dst = m.arg_ptr(1);
        const auto dst_bytes = static_cast<usize>(m.arg(2));
        const GuestAddr src = m.arg_ptr(3);
        const auto count = static_cast<usize>(m.arg(4));

        auto set_converted = [&](usize value) {
            if (converted_ptr == 0) return;
            if (m.cpu_is_64bit()) {
                (void)m.write_u64(converted_ptr, static_cast<u64>(value));
            } else {
                (void)m.write_u32(converted_ptr, static_cast<u32>(value));
            }
        };

        if (src == 0 || (dst == 0 && dst_bytes != 0) ||
            (dst != 0 && dst_bytes == 0)) {
            set_converted(0);
            m.set_return(EINVAL_VALUE);
            return;
        }

        const bool truncate = count == TRUNCATE;
        // With truncation the last byte of the buffer is reserved for the NUL.
        const usize limit = dst == 0 ? ~usize{0}
                          : truncate  ? (dst_bytes - 1)
                                      : std::min(count, dst_bytes);

        usize produced = 0;
        for (;;) {
            auto unit = m.read_u16(src + produced * 2);
            if (unit.is_err()) {
                set_converted(0);
                if (dst != 0) (void)m.write_u8(dst, 0);
                m.set_return(EINVAL_VALUE);
                return;
            }
            const u16 value = unit.value();
            if (value == 0) break;
            if (value > 0xFF) {  // not representable in the single-byte locale
                set_converted(0);
                if (dst != 0) (void)m.write_u8(dst, 0);
                m.set_return(EILSEQ_VALUE);
                return;
            }
            if (produced >= limit) {
                if (truncate) break;
                if (dst == 0 && produced < count) {
                    ++produced;   // size query keeps counting
                    continue;
                }
                set_converted(0);
                if (dst != 0) (void)m.write_u8(dst, 0);
                m.set_return(ERANGE_VALUE);
                return;
            }
            if (dst != 0) {
                (void)m.write_u8(dst + produced, static_cast<u8>(value));
            }
            ++produced;
            if (!truncate && produced == count) break;   // count exhausted
        }

        if (dst != 0) (void)m.write_u8(dst + produced, 0);
        set_converted(produced + 1);
        m.set_return(0);
    }});

    // mbstowcs_s / mbsrtowcs_s / wcsrtombs_s. Learn pages:
    //   https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbstowcs-s-mbstowcs-s-l
    //   https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbsrtowcs-s
    //   https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/wcsrtombs-s
    // Every rule below is the measured UCRT behaviour from
    // tests/winapi/msvcrt/oracle/crt_ucrt_s5_probe (x86 and x64 agree), which
    // departs from the pages in places: mbsrtowcs_s never reports ERANGE (a
    // full buffer is emptied and the call succeeds), wcsrtombs_s treats a full
    // buffer as ERANGE even under _TRUNCATE, and *mbstr == NULL converts as an
    // empty string. Single-byte "C" locale, matching mbstowcs/wcstombs_s: every
    // byte is a valid character, and a code unit above 0xFF cannot be
    // converted. The mbstate object is never touched (no shift states).
    constexpr u32 kEinval = 22;
    constexpr u32 kErange = 34;
    constexpr u32 kEilseq = 42;
    constexpr u32 kStruncate = 80;
    constexpr u64 kTruncate = ~u64{0};
    const auto set_size = [](Marshaler& m, GuestAddr out, u64 value) {
        if (out != 0) (void)m.write_ptr(out, m.cpu_is_64bit() ? value : (value & 0xFFFFFFFFu));
    };
    const auto size_arg = [](Marshaler& m, usize i) -> u64 {
        const u64 v = m.arg(i);
        return m.cpu_is_64bit() ? v : (static_cast<u32>(v) == 0xFFFFFFFFu ? kTruncate : v);
    };

    // errno_t mbstowcs_s(size_t* pReturnValue, wchar_t* wcstr, size_t sizeInWords,
    //                    const char* mbstr, size_t count)
    // Measured: an invalid (wcstr, sizeInWords) pair fails before
    // *pReturnValue is written; otherwise *pReturnValue = 0 and wcstr[0] = 0
    // first. Overflow (count != _TRUNCATE) converts into the buffer until it
    // is full, then empties it: ERANGE. _TRUNCATE keeps sizeInWords-1
    // characters and returns STRUNCATE. The handler runs before errno is set.
    exports.entries.push_back({"mbstowcs_s", 0, [&mem_mgr, set_size, size_arg](Marshaler& m) {
        const GuestAddr ret_ptr = m.arg_ptr(0);
        const GuestAddr dst = m.arg_ptr(1);
        const u64 size = size_arg(m, 2);
        const GuestAddr src = m.arg_ptr(3);
        const u64 count = size_arg(m, 4);
        auto fail = [&](u32 code) {
            if (!mem_mgr.invalid_parameter()) return;
            mem_mgr.set_errno(code);
            m.set_return(code);
        };
        if ((dst == 0) != (size == 0)) { fail(kEinval); return; }
        set_size(m, ret_ptr, 0);
        if (dst != 0) (void)m.write_u16(dst, 0);
        if (src == 0) { fail(kEinval); return; }
        if (dst == 0) {  // size query: count is ignored
            u64 n = 0;
            while (m.read_u8(src + n).value_or(0) != 0) ++n;
            set_size(m, ret_ptr, n + 1);
            m.set_return(0);
            return;
        }
        const bool truncate = count == kTruncate;
        const u64 limit = truncate ? size - 1 : count;
        u64 n = 0;
        for (;; ++n) {
            const u8 byte = m.read_u8(src + n).value_or(0);
            if (byte == 0 || n == limit) break;
            if (n == size) {  // no room for this character
                (void)m.write_u16(dst, 0);
                fail(kErange);
                return;
            }
            (void)m.write_u16(dst + n * 2, byte);
        }
        if (n == size) {  // no room for the terminator
            (void)m.write_u16(dst, 0);
            fail(kErange);
            return;
        }
        (void)m.write_u16(dst + n * 2, 0);
        set_size(m, ret_ptr, n + 1);
        const bool cut = truncate && m.read_u8(src + n).value_or(0) != 0;
        m.set_return(cut ? kStruncate : 0);
    }});

    // Shared body of the restartable forms. n = min(count, size) units are
    // converted (count == _TRUNCATE means size); reaching the source NUL
    // stores the terminator and sets *src to NULL, otherwise *src advances by
    // the units converted and the terminator goes after them. A buffer filled
    // without room for the terminator is emptied: mbsrtowcs_s still succeeds,
    // wcsrtombs_s fails with ERANGE. *pReturnValue is -1 on every failure.
    const auto restartable = [&mem_mgr, set_size, size_arg](Marshaler& m, bool to_wide) {
        const GuestAddr ret_ptr = m.arg_ptr(0);
        const GuestAddr dst = m.arg_ptr(1);
        const u64 size = size_arg(m, 2);
        const GuestAddr src_ptr = m.arg_ptr(3);
        const u64 count = size_arg(m, 4);
        const usize src_unit = to_wide ? 1 : 2;
        const usize dst_unit = to_wide ? 2 : 1;
        auto put = [&](u64 index, u16 value) {
            if (to_wide) (void)m.write_u16(dst + index * dst_unit, value);
            else (void)m.write_u8(dst + index, static_cast<u8>(value));
        };
        auto get = [&](GuestAddr src, u64 index) -> u16 {
            if (to_wide) return m.read_u8(src + index).value_or(0);
            return m.read_u16(src + index * src_unit).value_or(0);
        };
        auto fail = [&](u32 code, bool handler) {
            set_size(m, ret_ptr, kTruncate);
            if (handler && !mem_mgr.invalid_parameter()) return;
            mem_mgr.set_errno(code);
            m.set_return(code);
        };
        if (dst != 0 && size != 0) put(0, 0);
        if ((dst == 0) != (size == 0) || src_ptr == 0) { fail(kEinval, true); return; }
        const GuestAddr src = static_cast<GuestAddr>(m.read_ptr(src_ptr).value_or(0));
        if (src == 0) {  // *src == NULL converts as an empty string (measured)
            set_size(m, ret_ptr, 1);
            m.set_return(0);
            return;
        }
        const u64 limit = dst == 0 ? kTruncate : (count == kTruncate ? size : std::min(count, size));
        u64 n = 0;
        bool reached_nul = false;
        for (; n < limit; ++n) {
            const u16 unit = get(src, n);
            if (unit == 0) { reached_nul = true; break; }
            if (!to_wide && unit > 0xFF) {  // not representable in the "C" locale
                if (dst != 0) {
                    put(0, 0);
                    (void)m.write_ptr(src_ptr, src + n * src_unit);
                }
                fail(kEilseq, false);
                return;
            }
            if (dst != 0) put(n, unit);
        }
        if (dst == 0) {  // size query: *src is left alone
            set_size(m, ret_ptr, n + 1);
            m.set_return(0);
            return;
        }
        (void)m.write_ptr(src_ptr, reached_nul ? 0 : src + n * src_unit);
        if (!reached_nul && n == size) {  // filled with no room for the NUL
            put(0, 0);
            if (!to_wide) { fail(kErange, true); return; }
        } else {
            put(n, 0);
        }
        set_size(m, ret_ptr, n + 1);
        m.set_return(0);
    };
    // errno_t mbsrtowcs_s(size_t*, wchar_t*, size_t, const char**, size_t, mbstate_t*)
    exports.entries.push_back({"mbsrtowcs_s", 0, [restartable](Marshaler& m) {
        restartable(m, true);
    }});
    // errno_t wcsrtombs_s(size_t*, char*, size_t, const wchar_t**, size_t, mbstate_t*)
    exports.entries.push_back({"wcsrtombs_s", 0, [restartable](Marshaler& m) {
        restartable(m, false);
    }});

    // wint_t btowc(int c)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/btowc-btowc-l
    // WEOF unless `c` is a valid single byte in the initial shift state; the
    // "C" locale maps each byte to the code unit of the same value.
    exports.entries.push_back({"btowc", 0, [](Marshaler& m) {
        constexpr u32 WEOF_VALUE = 0xFFFF;
        const auto c = static_cast<i32>(static_cast<u32>(m.arg(0)));
        if (c == -1 || c < 0 || c > 255) {   // EOF or not a single byte
            m.set_return(WEOF_VALUE);
            return;
        }
        m.set_return(static_cast<u32>(c));
    }});

    // size_t __strncnt(const char* s, size_t count)
    //
    // vcruntime's bounded length helper: characters before the terminator,
    // never more than `count`. Identical to strnlen.
    exports.entries.push_back({"__strncnt", 0, [](Marshaler& m) {
        const GuestAddr s = m.arg_ptr(0);
        const auto count = static_cast<usize>(m.arg(1));
        if (s == 0) {
            m.set_return(0);
            return;
        }
        usize length = 0;
        while (length < count) {
            auto byte = m.read_u8(s + length);
            if (byte.is_err() || byte.value() == 0) break;
            ++length;
        }
        m.set_return(static_cast<u64>(length));
    }});

    // int _isctype(int c, int mask)
    //
    // Shares the classification table published as _pctype so that guests
    // mixing the macro form and the function form agree.
    exports.entries.push_back({"_isctype", 0, [](Marshaler& m) {
        const auto c = static_cast<i32>(static_cast<u32>(m.arg(0)));
        const auto mask = static_cast<u16>(m.arg(1) & 0xFFFF);
        if (c < 0 || c > 255) {
            m.set_return(0);
            return;
        }
        const u16 bits = CrtDataBlock::ctype_bits(static_cast<u8>(c));
        m.set_return((bits & mask) != 0 ? 1u : 0u);
    }});

    // int iswctype(wint_t c, wctype_t mask)
    //
    // Wide classification for the single-byte "C" locale: code points above
    // 0xFF classify as nothing, matching what setlocale("C") promises.
    // int iswctype(wint_t c, wctype_t desc)
    //
    // This decides iswalpha/iswdigit/iswspace/iswxdigit and friends, because
    // mingw implements all of them as macros over it -- the individual
    // exports are never imported. It previously consulted the 256-entry
    // NARROW table and returned 0 outright above U+00FF, so every non-ASCII
    // character classified as nothing at all: E-acute was not alphabetic,
    // U+3000 was not space. Windows classifies the full Unicode range here.
    exports.entries.push_back({"iswctype", 0, [](Marshaler& m) {
        const auto c = static_cast<u32>(m.arg(0) & 0xFFFFu);
        const auto mask = static_cast<u32>(m.arg(1) & 0xFFFFu);
        m.set_return((win_wctype_bits(c) & mask) != 0 ? 1u : 0u);
    }});
}

} // namespace vitreo::winapi
