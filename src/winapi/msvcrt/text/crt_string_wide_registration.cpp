// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_wide_registration.cpp
/// @brief The wide UTF-16 string and memory exports: wmem*, wcscmp / wcsncmp / wcsstr /
///        wcsrchr / wcscat / wcsncat / wcsnlen / strnlen, the restartable conversions
///        (mbrtowc, wcrtomb, mbsrtowcs) and wcsspn / wcscspn.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/mem_watch.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace vitreo::winapi {

void add_crt_wide_string_exports(DllExports& exports) {
    // ------------------------------------------------------------------
    // Missing wide UTF-16 string / memory ops (cdecl)
    //
    // Apple wchar_t is 32-bit.  Guest wchar_t is UTF-16 (16-bit).
    // We MUST use 16-bit accessors, never host wcs*/wmem*.
    // ------------------------------------------------------------------

    // wmemcpy(u16* dst, const u16* src, size_t n) -> u16*
    exports.entries.push_back(ExportEntry{
        "wmemcpy", 0,
        [](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            usize bytes = n * 2;
            if (bytes > 0) {
                Byte* dp = m.memory().host_ptr_mut(dst, bytes);
                const Byte* sp = m.memory().host_ptr(src, bytes);
                if (dp && sp) {
                    std::memcpy(dp, sp, bytes);
                    if (store_ring_enabled()) mem_watch_bytes(dst, dp, bytes, "crt.wmemcpy");
                } else {
                    std::vector<Byte> buf(bytes);
                    (void)m.memory().read_bytes(src, buf.data(), bytes);
                    (void)m.memory().write_bytes(dst, buf.data(), bytes);
                }
            }
            m.set_return(dst);
        }
    });

    // wmemmove(u16* dst, const u16* src, size_t n) -> u16*
    exports.entries.push_back(ExportEntry{
        "wmemmove", 0,
        [](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            usize bytes = n * 2;
            if (bytes > 0) {
                Byte* dp = m.memory().host_ptr_mut(dst, bytes);
                const Byte* sp = m.memory().host_ptr(src, bytes);
                if (dp && sp) {
                    std::memmove(dp, sp, bytes);
                    if (store_ring_enabled()) mem_watch_bytes(dst, dp, bytes, "crt.wmemmove");
                } else {
                    std::vector<Byte> buf(bytes);
                    (void)m.memory().read_bytes(src, buf.data(), bytes);
                    (void)m.memory().write_bytes(dst, buf.data(), bytes);
                }
            }
            m.set_return(dst);
        }
    });

    // wmemset(u16* dst, u16 val, size_t n) -> u16*
    exports.entries.push_back(ExportEntry{
        "wmemset", 0,
        [](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            u16 val = static_cast<u16>(m.arg(1));
            usize n = static_cast<usize>(m.arg(2));
            for (usize i = 0; i < n; ++i) {
                (void)m.write_u16(dst + i * 2, val);
            }
            m.set_return(dst);
        }
    });

    // wmemcmp(const u16* a, const u16* b, size_t n) -> int
    exports.entries.push_back(ExportEntry{
        "wmemcmp", 0,
        [](Marshaler& m) {
            GuestAddr a = m.arg_ptr(0);
            GuestAddr b = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            for (usize i = 0; i < n; ++i) {
                u16 ca = m.read_u16(a + i * 2).value_or(0);
                u16 cb = m.read_u16(b + i * 2).value_or(0);
                if (ca != cb) {
                    m.set_return(static_cast<u64>(static_cast<i64>(
                        ca < cb ? -1 : 1)));
                    return;
                }
            }
            m.set_return(0);
        }
    });

    // wmemchr(const u16* s, u16 c, size_t n) -> u16*
    exports.entries.push_back(ExportEntry{
        "wmemchr", 0,
        [](Marshaler& m) {
            GuestAddr s = m.arg_ptr(0);
            u16 ch = static_cast<u16>(m.arg(1));
            usize n = static_cast<usize>(m.arg(2));
            for (usize i = 0; i < n; ++i) {
                u16 c = m.read_u16(s + i * 2).value_or(0);
                if (c == ch) { m.set_return(s + i * 2); return; }
            }
            m.set_return(0);
        }
    });

    // wcscmp(const u16* a, const u16* b) -> int
    exports.entries.push_back(ExportEntry{
        "wcscmp", 0,
        [](Marshaler& m) {
            GuestAddr a = m.arg_ptr(0);
            GuestAddr b = m.arg_ptr(1);
            for (usize i = 0; i < (1u << 22); ++i) {
                u16 ca = m.read_u16(a + i * 2).value_or(0);
                u16 cb = m.read_u16(b + i * 2).value_or(0);
                if (ca != cb) {
                    m.set_return(static_cast<u64>(static_cast<i64>(
                        ca < cb ? -1 : 1)));
                    return;
                }
                if (ca == 0) break;
            }
            m.set_return(0);
        }
    });

    // wcsncmp(const u16* a, const u16* b, size_t n) -> int
    exports.entries.push_back(ExportEntry{
        "wcsncmp", 0,
        [](Marshaler& m) {
            GuestAddr a = m.arg_ptr(0);
            GuestAddr b = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            for (usize i = 0; i < n; ++i) {
                u16 ca = m.read_u16(a + i * 2).value_or(0);
                u16 cb = m.read_u16(b + i * 2).value_or(0);
                if (ca != cb) {
                    m.set_return(static_cast<u64>(static_cast<i64>(
                        ca < cb ? -1 : 1)));
                    return;
                }
                if (ca == 0) break;
            }
            m.set_return(0);
        }
    });

    // wcsstr(const u16* haystack, const u16* needle) -> u16*
    exports.entries.push_back(ExportEntry{
        "wcsstr", 0,
        [](Marshaler& m) {
            GuestAddr h = m.arg_ptr(0);
            GuestAddr n = m.arg_ptr(1);
            // Empty needle: return haystack
            u16 first = m.read_u16(n).value_or(0);
            if (first == 0) { m.set_return(h); return; }
            for (usize i = 0; i < (1u << 22); ++i) {
                u16 hc = m.read_u16(h + i * 2).value_or(0);
                if (hc == 0) break;
                if (hc != first) continue;
                bool match = true;
                for (usize j = 1; ; ++j) {
                    u16 nc = m.read_u16(n + j * 2).value_or(0);
                    if (nc == 0) break;
                    u16 hh = m.read_u16(h + (i + j) * 2).value_or(0);
                    if (hh != nc) { match = false; break; }
                }
                if (match) { m.set_return(h + i * 2); return; }
            }
            m.set_return(0);
        }
    });

    // wcsrchr(const u16* s, u16 c) -> u16*
    exports.entries.push_back(ExportEntry{
        "wcsrchr", 0,
        [](Marshaler& m) {
            GuestAddr s = m.arg_ptr(0);
            u16 ch = static_cast<u16>(m.arg(1));
            GuestAddr last = 0;
            for (usize i = 0; i < (1u << 22); ++i) {
                u16 c = m.read_u16(s + i * 2).value_or(0);
                if (c == ch) last = s + i * 2;
                if (c == 0) break;
            }
            m.set_return(last);
        }
    });

    // wcsncat(u16* dst, const u16* src, size_t n) -> u16*
    exports.entries.push_back(ExportEntry{
        "wcsncat", 0,
        [](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            usize end = 0;
            for (; end < (1u << 22); ++end) {
                u16 c = m.read_u16(dst + end * 2).value_or(0);
                if (c == 0) break;
            }
            usize i = 0;
            for (; i < n; ++i) {
                u16 c = m.read_u16(src + i * 2).value_or(0);
                if (c == 0) break;
                (void)m.write_u16(dst + (end + i) * 2, c);
            }
            (void)m.write_u16(dst + (end + i) * 2, 0);
            m.set_return(dst);
        }
    });

    // wcscat(u16* dst, const u16* src) -> u16*
    //
    // Was missing entirely, so a guest calling it silently got a no-op: the
    // destination came back unchanged and every wide path built by
    // concatenation stayed truncated at its prefix. Measured against real
    // Windows -- FindFirstFileW(dir + L"pattern") found nothing under Vitreo
    // and one match on Windows, because the pattern was still just the dir.
    exports.entries.push_back(ExportEntry{
        "wcscat", 0,
        [](Marshaler& m) {
            const GuestAddr dst = m.arg_ptr(0);
            const GuestAddr src = m.arg_ptr(1);
            usize end = 0;
            for (; end < (1u << 22); ++end) {
                if (m.read_u16(dst + end * 2).value_or(0) == 0) break;
            }
            usize i = 0;
            for (; i < (1u << 22); ++i) {
                const u16 c = m.read_u16(src + i * 2).value_or(0);
                if (c == 0) break;
                (void)m.write_u16(dst + (end + i) * 2, c);
            }
            (void)m.write_u16(dst + (end + i) * 2, 0);
            m.set_return(dst);   // returns dst, not the end
        }
    });

    // size_t wcsnlen(const u16* s, size_t max) -> length, never scanning past max
    exports.entries.push_back(ExportEntry{
        "wcsnlen", 0,
        [](Marshaler& m) {
            const GuestAddr s2 = m.arg_ptr(0);
            const usize maxn = static_cast<usize>(m.arg(1));
            usize i = 0;
            for (; i < maxn; ++i) {
                if (m.read_u16(s2 + i * 2).value_or(0) == 0) break;
            }
            m.set_return(static_cast<u32>(i));
        }
    });

    // size_t strnlen(const char* s, size_t max) -- length, never scanning
    // past max. Learn: strnlen never inspects s[max], so it is safe on a
    // buffer that is not NUL-terminated.
    exports.entries.push_back(ExportEntry{
        "strnlen", 0,
        [](Marshaler& m) {
            const GuestAddr s2 = m.arg_ptr(0);
            const usize maxn = static_cast<usize>(m.arg(1));
            usize i = 0;
            for (; i < maxn; ++i) {
                if (m.read_u8(s2 + i).value_or(0) == 0) break;
            }
            m.set_return(static_cast<u32>(i));
        }
    });

    // size_t mbrtowc(wchar_t* wc, const char* mb, size_t count,
    //                mbstate_t* state)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbrtowc
    // Single-byte "C" locale, matching wcstombs/mbstowcs above, so one byte is
    // always one whole character and the shift state stays empty.
    //   mb == NULL  -> same as mbrtowc(NULL, "", 1, state), i.e. 0
    //   count == 0  -> (size_t)-2, the "incomplete" answer
    //   *mb == 0    -> 0, and the null wide character is stored
    //   otherwise   -> 1 byte consumed
    exports.entries.push_back(ExportEntry{
        "mbrtowc", 0,
        [](Marshaler& m) {
            const GuestAddr wc = m.arg_ptr(0);
            const GuestAddr mb = m.arg_ptr(1);
            const usize count = static_cast<usize>(m.arg(2));
            if (mb == 0) {
                m.set_return(0u);
                return;
            }
            if (count == 0) {
                // (size_t)-2: the bytes seen so far are a valid prefix.
                m.set_return(std::numeric_limits<u32>::max() - 1);
                return;
            }
            const auto byte = m.read_u8(mb);
            if (byte.is_err()) {
                m.set_return(std::numeric_limits<u32>::max());
                return;
            }
            if (wc != 0) (void)m.write_u16(wc, byte.value());
            m.set_return(byte.value() == 0 ? 0u : 1u);
        }
    });

    // size_t wcrtomb(char* mb, wchar_t wc, mbstate_t* state)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/wcrtomb
    // mb == NULL is defined as wcrtomb(internal, L'\0', state), which writes
    // one byte and so returns 1. A wide value with no single-byte form is
    // EILSEQ -> (size_t)-1, matching wcstombs above.
    //
    // mingw's static printf converts %ls through this function, so a failing
    // stub here prints an empty string rather than the argument.
    exports.entries.push_back(ExportEntry{
        "wcrtomb", 0,
        [](Marshaler& m) {
            const GuestAddr mb = m.arg_ptr(0);
            const auto wc = static_cast<u32>(m.arg(1)) & 0xFFFFu;
            if (mb == 0) {
                m.set_return(1u);
                return;
            }
            if (wc > 0xFF) {
                m.set_return(std::numeric_limits<u32>::max());
                return;
            }
            if (m.write_u8(mb, static_cast<u8>(wc)).is_err()) {
                m.set_return(std::numeric_limits<u32>::max());
                return;
            }
            m.set_return(1u);
        }
    });

    // size_t mbsrtowcs(wchar_t* dst, const char** src, size_t count,
    //                  mbstate_t* state)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbsrtowcs
    // Returns the wide characters written, excluding the terminator.
    //   dst == NULL -> length query; *src is left alone
    //   stopped at the terminator -> write it and set *src = NULL
    //   stopped because count ran out -> *src points at the next byte
    exports.entries.push_back(ExportEntry{
        "mbsrtowcs", 0,
        [](Marshaler& m) {
            const GuestAddr dst = m.arg_ptr(0);
            const GuestAddr src_pp = m.arg_ptr(1);
            const usize count = static_cast<usize>(m.arg(2));
            if (src_pp == 0) {
                m.set_return(std::numeric_limits<u32>::max());
                return;
            }
            const auto src_value = m.read_ptr(src_pp);
            if (src_value.is_err()) {
                m.set_return(std::numeric_limits<u32>::max());
                return;
            }
            const auto src = static_cast<GuestAddr>(src_value.value());
            if (src == 0) {
                m.set_return(std::numeric_limits<u32>::max());
                return;
            }
            usize written = 0;
            while (written < MAX_BULK_SIZE) {
                if (dst != 0 && written == count) {
                    // Ran out of room: leave *src at the unconsumed byte.
                    (void)m.write_ptr(src_pp, src + written);
                    m.set_return(static_cast<u32>(written));
                    return;
                }
                const auto byte = m.read_u8(src + written);
                if (byte.is_err()) {
                    m.set_return(std::numeric_limits<u32>::max());
                    return;
                }
                if (byte.value() == 0) {
                    if (dst != 0) {
                        (void)m.write_u16(dst + written * 2, 0);
                        (void)m.write_ptr(src_pp, 0);
                    }
                    m.set_return(static_cast<u32>(written));
                    return;
                }
                if (dst != 0) {
                    (void)m.write_u16(dst + written * 2, byte.value());
                }
                ++written;
            }
            m.set_return(std::numeric_limits<u32>::max());
        }
    });

    // size_t wcsspn(const u16* s, const u16* accept)
    //   -- length of the initial run made up only of characters from accept.
    exports.entries.push_back(ExportEntry{
        "wcsspn", 0,
        [](Marshaler& m) {
            const GuestAddr s2 = m.arg_ptr(0);
            const GuestAddr set = m.arg_ptr(1);
            std::vector<u16> accept;
            for (usize i = 0; i < 4096; ++i) {
                const u16 c = m.read_u16(set + i * 2).value_or(0);
                if (c == 0) break;
                accept.push_back(c);
            }
            usize i = 0;
            for (; i < (1u << 22); ++i) {
                const u16 c = m.read_u16(s2 + i * 2).value_or(0);
                if (c == 0) break;
                if (std::find(accept.begin(), accept.end(), c) == accept.end()) break;
            }
            m.set_return(static_cast<u32>(i));
        }
    });

    // size_t wcscspn(const u16* s, const u16* reject)
    //   -- length of the initial run containing NO character from reject.
    exports.entries.push_back(ExportEntry{
        "wcscspn", 0,
        [](Marshaler& m) {
            const GuestAddr s2 = m.arg_ptr(0);
            const GuestAddr set = m.arg_ptr(1);
            std::vector<u16> reject;
            for (usize i = 0; i < 4096; ++i) {
                const u16 c = m.read_u16(set + i * 2).value_or(0);
                if (c == 0) break;
                reject.push_back(c);
            }
            usize i = 0;
            for (; i < (1u << 22); ++i) {
                const u16 c = m.read_u16(s2 + i * 2).value_or(0);
                if (c == 0) break;
                if (std::find(reject.begin(), reject.end(), c) != reject.end()) break;
            }
            m.set_return(static_cast<u32>(i));
        }
    });
}

} // namespace vitreo::winapi
