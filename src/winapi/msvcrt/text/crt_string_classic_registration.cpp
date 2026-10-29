// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_classic_registration.cpp
/// @brief The narrow <ctype.h> exports, strstr, _strrev, wcscpy, iswxdigit, _mktemp,
///        wcsncpy, _itoa / _ltoa / _ultoa and wcschr.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/text/crt_wctype_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unistd.h>  // getpid for _mktemp

namespace vitreo::winapi {

void add_crt_classic_string_exports(DllExports& exports) {
    // ---- Pinball.exe: atoi, atol, isdigit, isalnum, isspace, strstr, wcscpy, _itoa, _ltoa ----



    // isdigit(int c) -> int
    exports.entries.push_back(ExportEntry{
        "isdigit", 0,
        [](Marshaler& m) {
            int c = static_cast<int>(m.arg(0)) & 0xFF;
            m.set_return(std::isdigit(c) ? 1u : 0u);
        }
    });

    // The remaining <ctype.h> classifiers. The CRT's classification is
    // locale-dependent; Vitreo only publishes the "C" locale, whose tables are
    // exactly the host's for the 0..255 range these take.
    struct CtypeEntry {
        const char* name;
        int (*fn)(int);
    };
    static constexpr CtypeEntry CTYPE_TABLE[] = {
        {"isalpha",  [](int c) { return std::isalpha(c);  }},
        {"isprint",  [](int c) { return std::isprint(c);  }},
        {"isgraph",  [](int c) { return std::isgraph(c);  }},
        {"ispunct",  [](int c) { return std::ispunct(c);  }},
        {"iscntrl",  [](int c) { return std::iscntrl(c);  }},
        {"islower",  [](int c) { return std::islower(c);  }},
        {"isupper",  [](int c) { return std::isupper(c);  }},
        {"isxdigit", [](int c) { return std::isxdigit(c); }},
    };
    for (const auto& entry : CTYPE_TABLE) {
        exports.entries.push_back(ExportEntry{
            entry.name, 0,
            [fn = entry.fn](Marshaler& m) {
                const int c = static_cast<int>(m.arg(0)) & 0xFF;
                m.set_return(fn(c) ? 1u : 0u);
            }
        });
    }

    // tolower(int c) -> int
    // Learn / C11 7.4.2.1: EOF and values outside 0..UCHAR_MAX are returned
    // unchanged (masking with 0xFF made tolower(EOF)==255 and broke
    // `tolower(getc(f)) != EOF` loops).
    exports.entries.push_back(ExportEntry{
        "tolower", 0,
        [](Marshaler& m) {
            const int c = static_cast<int>(m.arg(0));
            if (c < 0 || c > 255) {
                m.set_return(static_cast<u64>(static_cast<u32>(c)));
                return;
            }
            m.set_return(static_cast<u32>(
                std::tolower(static_cast<unsigned char>(c))));
        }
    });

    // isalnum(int c) -> int
    exports.entries.push_back(ExportEntry{
        "isalnum", 0,
        [](Marshaler& m) {
            int c = static_cast<int>(m.arg(0)) & 0xFF;
            m.set_return(std::isalnum(c) ? 1u : 0u);
        }
    });

    // isspace(int c) -> int
    exports.entries.push_back(ExportEntry{
        "isspace", 0,
        [](Marshaler& m) {
            int c = static_cast<int>(m.arg(0)) & 0xFF;
            m.set_return(std::isspace(c) ? 1u : 0u);
        }
    });

    // toupper(int c) -> int
    // Same contract as tolower: EOF and values outside 0..UCHAR_MAX unchanged.
    exports.entries.push_back(ExportEntry{
        "toupper", 0,
        [](Marshaler& m) {
            const int c = static_cast<int>(m.arg(0));
            if (c < 0 || c > 255) {
                m.set_return(static_cast<u64>(static_cast<u32>(c)));
                return;
            }
            m.set_return(static_cast<u32>(
                std::toupper(static_cast<unsigned char>(c))));
        }
    });

    // strstr(const char* haystack, const char* needle) -> char*
    exports.entries.push_back(ExportEntry{
        "strstr", 0,
        [](Marshaler& m) {
            GuestAddr hay_addr = m.arg_ptr(0);
            GuestAddr ndl_addr = m.arg_ptr(1);
            if (hay_addr == 0 || ndl_addr == 0) { m.set_return(0); return; }
            auto rh = m.read_ansi_string(hay_addr);
            auto rn = m.read_ansi_string(ndl_addr);
            if (rh.is_err() || rn.is_err()) { m.set_return(0); return; }
            const char* found = std::strstr(rh.value().c_str(), rn.value().c_str());
            if (!found) { m.set_return(0); return; }
            usize offset = static_cast<usize>(found - rh.value().c_str());
            m.set_return(hay_addr + static_cast<GuestAddr>(offset));
        }
    });

    // char* _strrev(char* str) — reverse in place; terminator stays put.
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/strrev-wcsrev-mbsrev-mbsrev-l
    exports.entries.push_back(ExportEntry{
        "_strrev", 0,
        [](Marshaler& m) {
            const GuestAddr str = m.arg_ptr(0);
            if (str == 0) { m.set_return(0); return; }
            auto r = m.read_ansi_string(str);
            if (r.is_err()) { m.set_return(str); return; }
            std::string s = r.value();
            std::reverse(s.begin(), s.end());
            for (size_t i = 0; i < s.size(); ++i) {
                (void)m.write_u8(str + static_cast<GuestAddr>(i),
                                 static_cast<u8>(s[i]));
            }
            (void)m.write_u8(str + static_cast<GuestAddr>(s.size()), 0);
            m.set_return(str);
        }
    });

    // wcscpy(wchar_t* dest, const wchar_t* src) -> wchar_t*
    exports.entries.push_back(ExportEntry{
        "wcscpy", 0,
        [](Marshaler& m) {
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            if (dest == 0 || src == 0) { m.set_return(dest); return; }
            // Copy 16-bit chars including null terminator
            for (usize i = 0; i < 65536; ++i) {
                auto ch = m.read_u16(src + static_cast<GuestAddr>(i * 2));
                u16 val = ch.is_ok() ? ch.value() : 0;
                (void)m.write_u16(dest + static_cast<GuestAddr>(i * 2), val);
                if (val == 0) break;
            }
            m.set_return(dest);
        }
    });



    // int iswxdigit(wint_t c)
    exports.entries.push_back(ExportEntry{
        "iswxdigit", 0,
        [](Marshaler& m) {
            const u16 c = static_cast<u16>(m.arg(0) & 0xFFFFu);
            m.set_return(win_iswxdigit(static_cast<u32>(c)) ? 1 : 0);
        }
    });

    // char* _mktemp(char* template) — replace trailing X's with pid digits.
    exports.entries.push_back(ExportEntry{
        "_mktemp", 0,
        [](Marshaler& m) {
            const GuestAddr tmpl = m.arg_ptr(0);
            if (tmpl == 0) {
                m.set_return(0);
                return;
            }
            auto value = m.read_ansi_string(tmpl);
            if (value.is_err() || value.value().empty()) {
                m.set_return(0);
                return;
            }
            std::string s = value.value();
            usize x_count = 0;
            while (x_count < s.size() && s[s.size() - 1 - x_count] == 'X') {
                ++x_count;
            }
            if (x_count == 0) {
                m.set_return(0);
                return;
            }
            unsigned pid = static_cast<unsigned>(::getpid());
            for (usize i = 0; i < x_count; ++i) {
                s[s.size() - 1 - i] =
                    static_cast<char>('0' + (pid % 10));
                pid /= 10;
            }
            if (m.write_ansi_string(tmpl, s).is_err()) {
                m.set_return(0);
                return;
            }
            m.set_return(tmpl);
        }
    });

    // wcsncpy(wchar_t* dest, const wchar_t* src, size_t count) -> wchar_t*
    exports.entries.push_back(ExportEntry{
        "wcsncpy", 0,
        [](Marshaler& m) {
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize count = static_cast<usize>(m.arg(2));
            if (dest == 0 || count == 0) { m.set_return(dest); return; }
            bool hit_null = false;
            for (usize i = 0; i < count; ++i) {
                u16 val = 0;
                if (!hit_null && src != 0) {
                    auto ch = m.read_u16(src + static_cast<GuestAddr>(i * 2));
                    val = ch.is_ok() ? ch.value() : 0;
                    if (val == 0) hit_null = true;
                }
                (void)m.write_u16(dest + static_cast<GuestAddr>(i * 2), val);
            }
            m.set_return(dest);
        }
    });

    // _itoa(int value, char* str, int radix) -> char*
    exports.entries.push_back(ExportEntry{
        "_itoa", 0,
        [](Marshaler& m) {
            i32 value = static_cast<i32>(m.arg(0));
            GuestAddr str = m.arg_ptr(1);
            i32 radix = static_cast<i32>(m.arg(2));
            if (str == 0 || radix < 2 || radix > 36) { m.set_return(0); return; }
            char buf[34];
            bool neg = (value < 0 && radix == 10);
            u32 uval = neg ? static_cast<u32>(-value) : static_cast<u32>(value);
            int pos = 0;
            if (uval == 0) { buf[pos++] = '0'; }
            else {
                while (uval != 0) {
                    u32 d = uval % static_cast<u32>(radix);
                    buf[pos++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
                    uval /= static_cast<u32>(radix);
                }
            }
            if (neg) buf[pos++] = '-';
            // Write reversed + NUL
            for (int i = 0; i < pos; ++i)
                (void)m.write_u8(str + static_cast<GuestAddr>(i), static_cast<u8>(buf[pos - 1 - i]));
            (void)m.write_u8(str + static_cast<GuestAddr>(pos), 0);
            m.set_return(str);
        }
    });

    // _ltoa(long value, char* str, int radix) -> char* (same as _itoa for 32-bit)
    exports.entries.push_back(ExportEntry{
        "_ltoa", 0,
        [](Marshaler& m) {
            i32 value = static_cast<i32>(m.arg(0));
            GuestAddr str = m.arg_ptr(1);
            i32 radix = static_cast<i32>(m.arg(2));
            if (str == 0 || radix < 2 || radix > 36) { m.set_return(0); return; }
            char buf[34];
            bool neg = (value < 0 && radix == 10);
            u32 uval = neg ? static_cast<u32>(-value) : static_cast<u32>(value);
            int pos = 0;
            if (uval == 0) { buf[pos++] = '0'; }
            else {
                while (uval != 0) {
                    u32 d = uval % static_cast<u32>(radix);
                    buf[pos++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
                    uval /= static_cast<u32>(radix);
                }
            }
            if (neg) buf[pos++] = '-';
            for (int i = 0; i < pos; ++i)
                (void)m.write_u8(str + static_cast<GuestAddr>(i), static_cast<u8>(buf[pos - 1 - i]));
            (void)m.write_u8(str + static_cast<GuestAddr>(pos), 0);
            m.set_return(str);
        }
    });

    // _ultoa(unsigned long value, char* str, int radix) -> char*
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/itoa-itow-i64toa-ui64toa-i64tow-ui64tow
    exports.entries.push_back(ExportEntry{
        "_ultoa", 0,
        [](Marshaler& m) {
            u32 uval = static_cast<u32>(m.arg(0));
            GuestAddr str = m.arg_ptr(1);
            i32 radix = static_cast<i32>(m.arg(2));
            if (str == 0 || radix < 2 || radix > 36) { m.set_return(0); return; }
            char buf[34];
            int pos = 0;
            if (uval == 0) { buf[pos++] = '0'; }
            else {
                while (uval != 0) {
                    u32 d = uval % static_cast<u32>(radix);
                    buf[pos++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
                    uval /= static_cast<u32>(radix);
                }
            }
            for (int i = 0; i < pos; ++i)
                (void)m.write_u8(str + static_cast<GuestAddr>(i), static_cast<u8>(buf[pos - 1 - i]));
            (void)m.write_u8(str + static_cast<GuestAddr>(pos), 0);
            m.set_return(str);
        }
    });

    // wcschr — wide string char search
    exports.entries.push_back(ExportEntry{
        "wcschr", 0,
        [](Marshaler& m) {
            GuestAddr str = m.arg_ptr(0);
            u16 ch = static_cast<u16>(m.arg(1));
            for (usize i = 0; i < 65536; ++i) {
                auto c = m.read_u16(str + static_cast<GuestAddr>(i * 2));
                if (c.is_err()) { m.set_return(0); return; }
                if (c.value() == ch) {
                    m.set_return(static_cast<u64>(str + i * 2));
                    return;
                }
                if (c.value() == 0) break;
            }
            m.set_return(0);
        }
    });
}

} // namespace vitreo::winapi
