// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_search_registration.cpp
/// @brief strpbrk / _mbspbrk / _mbstrlen / strcspn / strspn / wcspbrk.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <array>
#include <vector>

namespace vitreo::winapi {

void add_crt_charset_search_exports(DllExports& exports) {
    // Build a 256-bit charset by walking a guest ANSI string until NUL.
    auto load_charset = [](Marshaler& m, GuestAddr ctrl,
                           std::array<bool, 256>& set) -> bool {
        set.fill(false);
        if (ctrl == 0) return false;
        for (usize i = 0;; ++i) {
            auto b = m.memory().read_u8(ctrl + static_cast<GuestAddr>(i));
            if (b.is_err()) return false;
            if (b.value() == 0) return true;
            set[b.value()] = true;
        }
    };

    // char* strpbrk(const char* string, const char* control)
    exports.entries.push_back(
        {"strpbrk", 0, [load_charset](Marshaler& m) {
            const GuestAddr string_ptr = m.arg_ptr(0);
            const GuestAddr control_ptr = m.arg_ptr(1);
            if (string_ptr == 0 || control_ptr == 0) {
                m.set_return(0);
                return;
            }
            std::array<bool, 256> set{};
            if (!load_charset(m, control_ptr, set)) {
                m.set_return(0);
                return;
            }
            for (usize i = 0;; ++i) {
                auto b = m.memory().read_u8(string_ptr +
                                            static_cast<GuestAddr>(i));
                if (b.is_err() || b.value() == 0) {
                    m.set_return(0);
                    return;
                }
                if (set[b.value()]) {
                    m.set_return(string_ptr + static_cast<GuestAddr>(i));
                    return;
                }
            }
        }});

    // unsigned char* _mbspbrk(...) — single-byte code page ≡ strpbrk.
    exports.entries.push_back(
        {"_mbspbrk", 0, [load_charset](Marshaler& m) {
            const GuestAddr string_ptr = m.arg_ptr(0);
            const GuestAddr control_ptr = m.arg_ptr(1);
            if (string_ptr == 0 || control_ptr == 0) {
                m.set_return(0);
                return;
            }
            std::array<bool, 256> set{};
            if (!load_charset(m, control_ptr, set)) {
                m.set_return(0);
                return;
            }
            for (usize i = 0;; ++i) {
                auto b = m.memory().read_u8(string_ptr +
                                            static_cast<GuestAddr>(i));
                if (b.is_err() || b.value() == 0) {
                    m.set_return(0);
                    return;
                }
                if (set[b.value()]) {
                    m.set_return(string_ptr + static_cast<GuestAddr>(i));
                    return;
                }
            }
        }});

    // size_t _mbstrlen(const char* str)
    //
    // Counts multibyte characters, returning (size_t)-1 only for an invalid
    // multibyte sequence. The current locale is single-byte ("C"), where every
    // byte is one character and no sequence is invalid, so the count is the
    // byte length. Same single-byte reasoning as _mbspbrk above.
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbstrlen-mbstrlen-l
    exports.entries.push_back(
        {"_mbstrlen", 0, [](Marshaler& m) {
            const GuestAddr p = m.arg_ptr(0);
            if (p == 0) {
                m.set_return(static_cast<u64>(static_cast<usize>(-1)));
                return;
            }
            m.set_return(static_cast<u64>(guest_strlen(m.memory(), p)));
        }});

    // size_t strcspn(const char* string, const char* control)
    exports.entries.push_back(
        {"strcspn", 0, [load_charset](Marshaler& m) {
            const GuestAddr string_ptr = m.arg_ptr(0);
            const GuestAddr control_ptr = m.arg_ptr(1);
            if (string_ptr == 0 || control_ptr == 0) {
                m.set_return(0);
                return;
            }
            std::array<bool, 256> set{};
            if (!load_charset(m, control_ptr, set)) {
                m.set_return(0);
                return;
            }
            for (usize i = 0;; ++i) {
                auto b = m.memory().read_u8(string_ptr +
                                            static_cast<GuestAddr>(i));
                if (b.is_err() || b.value() == 0) {
                    m.set_return(static_cast<u64>(i));
                    return;
                }
                if (set[b.value()]) {
                    m.set_return(static_cast<u64>(i));
                    return;
                }
            }
        }});

    // size_t strspn(const char* str, const char* accept)
    exports.entries.push_back(
        {"strspn", 0, [load_charset](Marshaler& m) {
            const GuestAddr string_ptr = m.arg_ptr(0);
            const GuestAddr accept_ptr = m.arg_ptr(1);
            if (string_ptr == 0 || accept_ptr == 0) {
                m.set_return(0);
                return;
            }
            std::array<bool, 256> set{};
            if (!load_charset(m, accept_ptr, set)) {
                m.set_return(0);
                return;
            }
            for (usize i = 0;; ++i) {
                auto b = m.memory().read_u8(string_ptr +
                                            static_cast<GuestAddr>(i));
                if (b.is_err() || b.value() == 0 || !set[b.value()]) {
                    m.set_return(static_cast<u64>(i));
                    return;
                }
            }
        }});

    // wchar_t* wcspbrk(const wchar_t* str, const wchar_t* accept)
    exports.entries.push_back(
        {"wcspbrk", 0, [](Marshaler& m) {
            const GuestAddr str_ptr = m.arg_ptr(0);
            const GuestAddr acc_ptr = m.arg_ptr(1);
            if (str_ptr == 0 || acc_ptr == 0) {
                m.set_return(0);
                return;
            }
            // Compare UTF-16 code units directly: narrowing to std::string
            // first would make the match index meaningless for non-ASCII.
            std::vector<u16> accept;
            for (GuestAddr a = acc_ptr;; a += 2) {
                auto ch = m.read_u16(a);
                if (ch.is_err()) { m.set_return(0); return; }
                if (ch.value() == 0) break;
                accept.push_back(ch.value());
            }
            for (GuestAddr a = str_ptr;; a += 2) {
                auto ch = m.read_u16(a);
                if (ch.is_err()) { m.set_return(0); return; }
                if (ch.value() == 0) break;
                if (std::find(accept.begin(), accept.end(), ch.value()) !=
                    accept.end()) {
                    m.set_return(a);
                    return;
                }
            }
            m.set_return(0);
        }});
}

} // namespace vitreo::winapi
