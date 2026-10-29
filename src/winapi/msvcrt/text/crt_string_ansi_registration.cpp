// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_ansi_registration.cpp
/// @brief strcmp / strncmp / strchr / _mbschr / _mbsnbcpy / strrchr / strncpy / strncat.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

void add_crt_ansi_string_exports(DllExports& exports, CrtMemoryManager& mem_mgr) {
    // ------------------------------------------------------------------
    // Missing ANSI string ops (cdecl, stdcall_bytes=0)
    //
    // Implemented as bounded byte-level loops.  Apps inline these
    // through MSVC CRT all the time; missing exports = IAT failure.
    // ------------------------------------------------------------------

    // strcmp(const char* a, const char* b) -> int
    exports.entries.push_back(ExportEntry{
        "strcmp", 0,
        [](Marshaler& m) {
            GuestAddr a = m.arg_ptr(0);
            GuestAddr b = m.arg_ptr(1);
            for (usize i = 0; i < (1u << 24); ++i) {
                u8 ca = m.read_u8(a + i).value_or(0);
                u8 cb = m.read_u8(b + i).value_or(0);
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

    // strncmp(const char* a, const char* b, size_t n) -> int
    exports.entries.push_back(ExportEntry{
        "strncmp", 0,
        [](Marshaler& m) {
            GuestAddr a = m.arg_ptr(0);
            GuestAddr b = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            for (usize i = 0; i < n; ++i) {
                u8 ca = m.read_u8(a + i).value_or(0);
                u8 cb = m.read_u8(b + i).value_or(0);
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

    // strchr(const char* s, int c) -> char*
    exports.entries.push_back(ExportEntry{
        "strchr", 0,
        [](Marshaler& m) {
            GuestAddr s = m.arg_ptr(0);
            u8 ch = static_cast<u8>(m.arg(1) & 0xFF);
            for (usize i = 0; i < (1u << 24); ++i) {
                u8 c = m.read_u8(s + i).value_or(0);
                if (c == ch) { m.set_return(s + i); return; }
                if (c == 0) break;
            }
            // strchr matches the terminator if ch == 0
            if (ch == 0) {
                // fall through; loop already broke at terminator
            }
            m.set_return(0);
        }
    });

    // unsigned char* _mbschr(const unsigned char* str, unsigned int c)
    // Learn: multibyte strchr; NULL → invalid-parameter + NULL + EINVAL.
    // Vitreo's MBCS page is single-byte, so this is strchr with validation.
    exports.entries.push_back(ExportEntry{
        "_mbschr", 0,
        [&mem_mgr](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            const GuestAddr s = m.arg_ptr(0);
            const u8 ch = static_cast<u8>(m.arg(1) & 0xFF);
            if (s == 0) {
                mem_mgr.set_errno(EINVAL_VALUE);
                (void)mem_mgr.invalid_parameter();
                m.set_return(0);
                return;
            }
            for (usize i = 0;; ++i) {
                const auto c = m.read_u8(s + static_cast<GuestAddr>(i));
                if (c.is_err()) {
                    m.set_return(0);
                    return;
                }
                if (c.value() == ch) {
                    m.set_return(s + static_cast<GuestAddr>(i));
                    return;
                }
                if (c.value() == 0) break;
            }
            m.set_return(0);
        }
    });

    // unsigned char* _mbsnbcpy(unsigned char* dest, const unsigned char* src,
    //                          size_t count) — strncpy-style byte copy with
    // null padding to `count` (Learn _mbsnbcpy; maps to strncpy when !_MBCS).
    exports.entries.push_back(ExportEntry{
        "_mbsnbcpy", 0,
        [&mem_mgr](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            const GuestAddr dst = m.arg_ptr(0);
            const GuestAddr src = m.arg_ptr(1);
            const usize count = static_cast<usize>(m.arg(2));
            if (dst == 0 || src == 0) {
                mem_mgr.set_errno(EINVAL_VALUE);
                (void)mem_mgr.invalid_parameter();
                m.set_return(0);
                return;
            }
            usize i = 0;
            for (; i < count; ++i) {
                const auto c = m.read_u8(src + static_cast<GuestAddr>(i));
                const u8 b = c.value_or(0);
                (void)m.write_u8(dst + static_cast<GuestAddr>(i), b);
                if (b == 0) {
                    ++i;
                    break;
                }
            }
            for (; i < count; ++i) {
                (void)m.write_u8(dst + static_cast<GuestAddr>(i), 0);
            }
            m.set_return(static_cast<u64>(dst));
        }
    });

    // strrchr(const char* s, int c) -> char*
    exports.entries.push_back(ExportEntry{
        "strrchr", 0,
        [](Marshaler& m) {
            GuestAddr s = m.arg_ptr(0);
            u8 ch = static_cast<u8>(m.arg(1) & 0xFF);
            GuestAddr last = 0;
            for (usize i = 0; i < (1u << 24); ++i) {
                u8 c = m.read_u8(s + i).value_or(0);
                if (c == ch) last = s + i;
                if (c == 0) break;
            }
            m.set_return(last);
        }
    });

    // strncpy(char* dst, const char* src, size_t n) -> char*
    exports.entries.push_back(ExportEntry{
        "strncpy", 0,
        [](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            bool hit_zero = false;
            for (usize i = 0; i < n; ++i) {
                u8 c = hit_zero ? 0 : m.read_u8(src + i).value_or(0);
                if (c == 0) hit_zero = true;
                (void)m.write_u8(dst + i, c);
            }
            m.set_return(dst);
        }
    });

    // strncat(char* dst, const char* src, size_t n) -> char*
    exports.entries.push_back(ExportEntry{
        "strncat", 0,
        [](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize n = static_cast<usize>(m.arg(2));
            usize end = 0;
            for (; end < (1u << 24); ++end) {
                u8 c = m.read_u8(dst + end).value_or(0);
                if (c == 0) break;
            }
            usize i = 0;
            for (; i < n; ++i) {
                u8 c = m.read_u8(src + i).value_or(0);
                if (c == 0) break;
                (void)m.write_u8(dst + end + i, c);
            }
            (void)m.write_u8(dst + end + i, 0);
            m.set_return(dst);
        }
    });
}

} // namespace vitreo::winapi
