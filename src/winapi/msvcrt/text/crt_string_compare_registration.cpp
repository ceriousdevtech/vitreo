// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_compare_registration.cpp
/// @brief _stricmp / _mbsnbcmp / _mbscmp / _wcsicmp / _strnicmp / _wcsnicmp and
///        _strdup / _wcsdup.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"
#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <cctype>
#include <cwchar>

namespace vitreo::winapi {

void add_crt_string_compare_exports(DllExports& exports, CrtStringManager& mgr,
                                    CrtMemoryManager& mem_mgr) {
    // ordinal 1: int _stricmp(const char* s1, const char* s2)
    // Learn: NULL → invalid-parameter + errno EINVAL + return _NLSCMPERROR
    // (0x7fffffff). Case fold via unsigned-char tolower ("C" locale).
    constexpr u32 kNlscmpError = 0x7fffffffu;
    auto icmp_fail = [&mem_mgr](Marshaler& m) {
        constexpr u32 EINVAL_VALUE = 22;
        mem_mgr.set_errno(EINVAL_VALUE);
        (void)mem_mgr.invalid_parameter();
        m.set_return(kNlscmpError);
    };
    exports.entries.push_back(
        {"_stricmp", 1, [&mem_mgr, icmp_fail](Marshaler& m) {
            const GuestAddr a = m.arg_ptr(0);
            const GuestAddr b = m.arg_ptr(1);
            if (a == 0 || b == 0) {
                icmp_fail(m);
                return;
            }
            for (usize i = 0;; ++i) {
                const auto ca = m.read_u8(a + static_cast<GuestAddr>(i));
                const auto cb = m.read_u8(b + static_cast<GuestAddr>(i));
                if (ca.is_err() || cb.is_err()) {
                    icmp_fail(m);
                    return;
                }
                const int va = std::tolower(
                    static_cast<unsigned char>(ca.value()));
                const int vb = std::tolower(
                    static_cast<unsigned char>(cb.value()));
                if (va != vb) {
                    m.set_return(static_cast<u64>(static_cast<u32>(
                        va < vb ? -1 : 1)));
                    return;
                }
                if (ca.value() == 0) {
                    m.set_return(0);
                    return;
                }
            }
        }});

    // ordinal 2: int _wcsicmp(const wchar_t* s1, const wchar_t* s2)
    exports.entries.push_back(
        // Learn (_mbsnbcmp): compare at most `count` *bytes* of two
        // multibyte strings, without regard to locale collation. Vitreo's
        // multibyte code page is single-byte, so a byte comparison is the
        // whole contract; the result is the sign of the first difference.
        {"_mbsnbcmp", 0, [&mem_mgr, icmp_fail](Marshaler& m) {
            const GuestAddr a = m.arg_ptr(0);
            const GuestAddr b = m.arg_ptr(1);
            const usize count = static_cast<usize>(m.arg(2));
            if (a == 0 || b == 0) {
                icmp_fail(m);
                return;
            }
            for (usize i = 0; i < count; ++i) {
                const auto ca = m.read_u8(a + static_cast<GuestAddr>(i));
                const auto cb = m.read_u8(b + static_cast<GuestAddr>(i));
                if (ca.is_err() || cb.is_err()) break;
                const unsigned va = ca.value();
                const unsigned vb = cb.value();
                if (va != vb) {
                    m.set_return(static_cast<u64>(static_cast<u32>(
                        va < vb ? -1 : 1)));
                    return;
                }
                if (va == 0) break;  // both terminated
            }
            m.set_return(0);
            (void)mem_mgr;
        }});

    // int _mbscmp(const unsigned char* s1, const unsigned char* s2)
    // UCRT mbstring/mbscmp.cpp: NULL -> EINVAL + handler + _NLSCMPERROR; with
    // a single-byte multibyte code page (Vitreo's only one) it is strcmp,
    // whose result is -1/0/+1.
    exports.entries.push_back(
        {"_mbscmp", 0, [icmp_fail](Marshaler& m) {
            const GuestAddr a = m.arg_ptr(0);
            const GuestAddr b = m.arg_ptr(1);
            if (a == 0 || b == 0) {
                icmp_fail(m);
                return;
            }
            for (usize i = 0;; ++i) {
                const unsigned va = m.read_u8(a + static_cast<GuestAddr>(i)).value_or(0);
                const unsigned vb = m.read_u8(b + static_cast<GuestAddr>(i)).value_or(0);
                if (va != vb) {
                    m.set_return(static_cast<u32>(va < vb ? -1 : 1));
                    return;
                }
                if (va == 0) break;
            }
            m.set_return(0);
        }});

    // ordinal 2: int _wcsicmp(const wchar_t* s1, const wchar_t* s2)
    exports.entries.push_back(
        {"_wcsicmp", 2, [&mem_mgr, icmp_fail](Marshaler& m) {
            const GuestAddr s1_addr = m.arg_ptr(0);
            const GuestAddr s2_addr = m.arg_ptr(1);
            if (s1_addr == 0 || s2_addr == 0) {
                icmp_fail(m);
                return;
            }
            usize len1 = guest_wcslen(m.memory(), s1_addr);
            usize len2 = guest_wcslen(m.memory(), s2_addr);
            auto ws1 = guest_read_wstr(m.memory(), s1_addr, len1);
            auto ws2 = guest_read_wstr(m.memory(), s2_addr, len2);
            int result = ::wcscasecmp(ws1.c_str(), ws2.c_str());
            m.set_return(static_cast<u64>(static_cast<i64>(result)));
            (void)mem_mgr;
        }});

    // int _wcsicoll(const wchar_t* string1, const wchar_t* string2)
    // Learn (_stricoll family): case-insensitive comparison by the locale's
    // LC_CTYPE and LC_COLLATE; NULL -> handler, errno EINVAL, _NLSCMPERROR.
    // Vitreo's CRT locale is "C" (see setlocale), where Windows compares
    // like _wcsicmp: measured by tests/winapi/msvcrt/oracle/crt_ucrt_s6_probe
    // "i" rows (x86 = x64), only A-Z fold (to a-z: '[' vs 'a' is -6), U+00C9
    // vs U+00E9 and fullwidth letters do not fold, and the result is the
    // difference of the first differing folded UTF-16 units (L"abc" vs
    // L"abcd" is -100). errno is set before the handler runs.
    exports.entries.push_back(
        {"_wcsicoll", 0, [icmp_fail](Marshaler& m) {
            const GuestAddr a = m.arg_ptr(0);
            const GuestAddr b = m.arg_ptr(1);
            if (a == 0 || b == 0) {
                icmp_fail(m);
                return;
            }
            const auto fold = [](u32 unit) {
                return unit >= 'A' && unit <= 'Z' ? unit + ('a' - 'A') : unit;
            };
            for (usize i = 0;; ++i) {
                const u32 ca = fold(m.read_u16(a + static_cast<GuestAddr>(i * 2)).value_or(0));
                const u32 cb = fold(m.read_u16(b + static_cast<GuestAddr>(i * 2)).value_or(0));
                if (ca != cb || ca == 0) {
                    m.set_return(static_cast<u32>(static_cast<i32>(ca) - static_cast<i32>(cb)));
                    return;
                }
            }
        }});

    // ordinal 3: int _strnicmp(const char* s1, const char* s2, size_t count)
    // Learn (_strnicmp): NULL → _NLSCMPERROR + EINVAL; count==0 compares equal
    // only when both pointers are non-NULL (oracle: (0,0,0) still fails).
    exports.entries.push_back(
        {"_strnicmp", 3, [icmp_fail](Marshaler& m) {
            const GuestAddr a = m.arg_ptr(0);
            const GuestAddr b = m.arg_ptr(1);
            const usize count = static_cast<usize>(m.arg(2));
            if (a == 0 || b == 0) {
                icmp_fail(m);
                return;
            }
            for (usize i = 0; i < count; ++i) {
                const auto ca = m.read_u8(a + static_cast<GuestAddr>(i));
                const auto cb = m.read_u8(b + static_cast<GuestAddr>(i));
                if (ca.is_err() || cb.is_err()) {
                    icmp_fail(m);
                    return;
                }
                const int va = std::tolower(
                    static_cast<unsigned char>(ca.value()));
                const int vb = std::tolower(
                    static_cast<unsigned char>(cb.value()));
                if (va != vb) {
                    m.set_return(static_cast<u64>(static_cast<u32>(
                        va < vb ? -1 : 1)));
                    return;
                }
                if (ca.value() == 0) break;
            }
            m.set_return(0);
        }});





    // ordinal 4: int _wcsnicmp(const wchar_t* s1, const wchar_t* s2, size_t count)
    exports.entries.push_back(
        {"_wcsnicmp", 4, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr s1_addr = m.arg_ptr(0);
            GuestAddr s2_addr = m.arg_ptr(1);
            usize count = static_cast<usize>(m.arg(2));
            usize cap = std::min(count, MAX_BULK_SIZE);
            auto ws1 = guest_read_wstr(m.memory(), s1_addr, cap);
            auto ws2 = guest_read_wstr(m.memory(), s2_addr, cap);
            int result = CrtStringManager::wcsnicmp(ws1.c_str(), ws2.c_str(), count);
            m.set_return(static_cast<u64>(static_cast<i64>(result)));
        }});

    // ordinal 5: char* _strdup(const char* s)
    // Learn strdup: allocates and copies the whole string (no length cap).
    exports.entries.push_back(
        {"_strdup", 5, [&mem_mgr](Marshaler& m) {
            GuestAddr src = m.arg_ptr(0);
            if (src == 0) { m.set_return(0); return; }
            const usize len = guest_strlen(m.memory(), src);
            GuestAddr dst = mem_mgr.crt_malloc(len + 1);
            if (dst == 0) { m.set_return(0); return; }
            for (usize i = 0; i < len; ++i) {
                const auto ch = m.read_u8(src + static_cast<GuestAddr>(i));
                (void)m.write_u8(dst + static_cast<GuestAddr>(i),
                                 ch.value_or(0));
            }
            (void)m.write_u8(dst + static_cast<GuestAddr>(len), 0);
            m.set_return(static_cast<u64>(dst));
        }});

    // ordinal 6: wchar_t* _wcsdup(const wchar_t* s)
    exports.entries.push_back(
        {"_wcsdup", 6, [&mgr, &mem_mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr src = m.arg_ptr(0);
            if (src == 0) { m.set_return(0); return; }
            // Count UTF-16LE code units at src to size the allocation
            usize wlen = 0;
            while (wlen < 65536) {
                auto r = m.read_u16(src + static_cast<GuestAddr>(wlen * 2));
                if (!r.is_ok() || r.value() == 0) break;
                ++wlen;
            }
            GuestAddr dst = mem_mgr.crt_malloc((wlen + 1) * 2);
            if (dst == 0) { m.set_return(0); return; }
            // Copy UTF-16LE units
            for (usize i = 0; i < wlen; ++i) {
                auto r = m.read_u16(src + static_cast<GuestAddr>(i * 2));
                u16 ch = r.is_ok() ? r.value() : 0;
                (void)m.write_u16(dst + static_cast<GuestAddr>(i * 2), ch);
            }
            (void)m.write_u16(dst + static_cast<GuestAddr>(wlen * 2), 0);
            m.set_return(static_cast<u64>(dst));
        }});
}

} // namespace vitreo::winapi
