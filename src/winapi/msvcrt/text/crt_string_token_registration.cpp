// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_token_registration.cpp
/// @brief memchr / strtok and the collation exports (strcoll, _stricoll, strxfrm,
///        wcscoll, wcsxfrm).
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <cctype>
#include <string>
#include <vector>

namespace vitreo::winapi {

void add_crt_token_collate_exports(DllExports& exports, CrtMemoryManager& mem_mgr) {
    // double strtod(const char* str, char** end) / float strtof(...)
    //
    // Both return in ST(0) on x86 and XMM0 on x64; strtof narrows to float
    // first so the guest sees the same rounding Windows would produce.
    // strtod/strtof/atof/_wtof and wcstod/wcstof live in crt_strtod.cpp.

    // void* memchr(const void* buf, int c, size_t count)
    exports.entries.push_back({"memchr", 0, [](Marshaler& m) {
        const GuestAddr buf = m.arg_ptr(0);
        const auto needle = static_cast<u8>(m.arg(1) & 0xFF);
        const auto count = static_cast<usize>(m.arg(2));
        for (usize i = 0; i < count; ++i) {
            auto byte = m.read_u8(buf + i);
            if (byte.is_err()) break;
            if (byte.value() == needle) {
                m.set_return(buf + i);
                return;
            }
        }
        m.set_return(0);
    }});

    // char* strtok(char* str, const char* delim)
    //
    // The saved position is per-thread in msvcrt, and handlers run on the
    // host thread backing the guest thread, so thread_local reproduces the
    // Windows-visible behavior of concurrent tokenizers not clobbering
    // each other.
    exports.entries.push_back({"strtok", 0, [](Marshaler& m) {
        static thread_local GuestAddr s_strtok_cursor = 0;

        const GuestAddr str = m.arg_ptr(0);
        auto delim = m.read_ansi_string(m.arg_ptr(1));
        if (delim.is_err()) {
            m.set_return(0);
            return;
        }
        const std::string& seps = delim.value();
        const auto is_delim = [&seps](u8 c) {
            return seps.find(static_cast<char>(c)) != std::string::npos;
        };

        GuestAddr cursor = str != 0 ? str : s_strtok_cursor;
        if (cursor == 0) {
            m.set_return(0);
            return;
        }

        // Skip leading delimiters.
        for (;;) {
            auto c = m.read_u8(cursor);
            if (c.is_err() || c.value() == 0) {
                s_strtok_cursor = 0;
                m.set_return(0);
                return;
            }
            if (!is_delim(c.value())) break;
            ++cursor;
        }

        const GuestAddr token = cursor;
        for (;;) {
            auto c = m.read_u8(cursor);
            if (c.is_err() || c.value() == 0) {
                // Final token: nothing left for the next call.
                s_strtok_cursor = 0;
                break;
            }
            if (is_delim(c.value())) {
                (void)m.write_u8(cursor, 0);
                s_strtok_cursor = cursor + 1;
                break;
            }
            ++cursor;
        }
        m.set_return(token);
    }});

    // char* strtok_s(char* str, const char* delimiters, char** context)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/strtok-s-strtok-s-l-wcstok-s-wcstok-s-l-mbstok-s-mbstok-s-l
    // Error conditions: context NULL, delimiters NULL, or str NULL with
    // *context NULL -> errno EINVAL, handler, NULL. Measured by
    // tests/winapi/msvcrt/oracle/crt_ucrt_s6_probe "t" rows (x86 = x64):
    // errno is set before the handler runs and *context is left alone on
    // every error. Otherwise str (or *context when str is NULL) is scanned
    // past leading delimiters; with nothing left the call returns NULL and
    // *context points at the terminator. A token ends at the next delimiter,
    // which becomes NUL with *context just past it, or at the terminator,
    // where *context is left. Delimiters may differ between calls. The "C"
    // locale is single-byte, so delimiters are bytes.
    exports.entries.push_back({"strtok_s", 0, [&mem_mgr](Marshaler& m) {
        constexpr u32 kEinval = 22;
        const GuestAddr str = m.arg_ptr(0);
        const GuestAddr delimiters = m.arg_ptr(1);
        const GuestAddr context = m.arg_ptr(2);
        m.set_return(0);
        const auto fail = [&] {
            mem_mgr.set_errno(kEinval);
            (void)mem_mgr.invalid_parameter();
        };
        if (context == 0 || delimiters == 0) {
            fail();
            return;
        }
        GuestAddr cursor = str;
        if (cursor == 0) {
            auto saved = m.read_ptr(context);
            if (saved.is_err() || saved.value() == 0) {
                fail();
                return;
            }
            cursor = static_cast<GuestAddr>(saved.value());
        }
        auto delim = m.read_ansi_string(delimiters);
        if (delim.is_err()) return;
        const std::string& seps = delim.value();
        const auto is_delim = [&seps](u8 c) {
            return seps.find(static_cast<char>(c)) != std::string::npos;
        };
        for (;;) {
            const u8 c = m.read_u8(cursor).value_or(0);
            if (c == 0) {
                (void)m.write_ptr(context, cursor);
                return;
            }
            if (!is_delim(c)) break;
            ++cursor;
        }
        const GuestAddr token = cursor;
        for (;;) {
            const u8 c = m.read_u8(cursor).value_or(0);
            if (c == 0) break;
            if (is_delim(c)) {
                (void)m.write_u8(cursor, 0);
                ++cursor;
                break;
            }
            ++cursor;
        }
        (void)m.write_ptr(context, cursor);
        m.set_return(token);
    }});

    // int strcoll(const char* a, const char* b)
    // int _stricoll(const char* a, const char* b)
    //
    // Vitreo's CRT locale is "C" (see setlocale above), where collation order
    // is plain byte order — so these reduce to strcmp / _stricmp. Returning a
    // locale-sensitive order here would contradict what setlocale reports.
    const auto ansi_collate = [](Marshaler& m, bool fold_case) {
        auto a = m.read_ansi_string(m.arg_ptr(0));
        auto b = m.read_ansi_string(m.arg_ptr(1));
        if (a.is_err() || b.is_err()) {
            m.set_return(0);
            return;
        }
        std::string lhs = a.value();
        std::string rhs = b.value();
        if (fold_case) {
            for (char& c : lhs) c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
            for (char& c : rhs) c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
        }
        const int cmp = lhs.compare(rhs);
        m.set_return(static_cast<u64>(static_cast<u32>(
            cmp < 0 ? -1 : (cmp > 0 ? 1 : 0))));
    };
    exports.entries.push_back({"strcoll", 0,
        [ansi_collate](Marshaler& m) { ansi_collate(m, false); }});
    exports.entries.push_back({"_stricoll", 0,
        [ansi_collate](Marshaler& m) { ansi_collate(m, true); }});

    // size_t strxfrm(char* dst, const char* src, size_t count)
    //
    // In the "C" locale the transformed form is the string itself, so this is
    // a bounded copy that still reports the full length the caller needs.
    exports.entries.push_back({"strxfrm", 0, [](Marshaler& m) {
        const GuestAddr dst = m.arg_ptr(0);
        auto src = m.read_ansi_string(m.arg_ptr(1));
        const auto count = static_cast<usize>(m.arg(2));
        if (src.is_err()) {
            m.set_return(0);
            return;
        }
        const std::string& text = src.value();
        if (count > text.size()) {
            (void)m.write_ansi_string(dst, text);
        } else if (count > 0) {
            // Not enough room: the CRT leaves dst unspecified but must not
            // overrun, so write exactly `count` bytes of the prefix.
            for (usize i = 0; i < count; ++i) {
                (void)m.write_u8(dst + i, static_cast<u8>(text[i]));
            }
        }
        m.set_return(static_cast<u64>(text.size()));
    }});

    // int wcscoll(const wchar_t* a, const wchar_t* b)
    exports.entries.push_back({"wcscoll", 0, [](Marshaler& m) {
        const GuestAddr a = m.arg_ptr(0);
        const GuestAddr b = m.arg_ptr(1);
        for (usize i = 0;; ++i) {
            auto lhs = m.read_u16(a + i * 2);
            auto rhs = m.read_u16(b + i * 2);
            if (lhs.is_err() || rhs.is_err()) {
                m.set_return(0);
                return;
            }
            if (lhs.value() != rhs.value()) {
                m.set_return(static_cast<u64>(static_cast<u32>(
                    lhs.value() < rhs.value() ? -1 : 1)));
                return;
            }
            if (lhs.value() == 0) {
                m.set_return(0);
                return;
            }
        }
    }});

    // size_t wcsxfrm(wchar_t* dst, const wchar_t* src, size_t count)
    exports.entries.push_back({"wcsxfrm", 0, [](Marshaler& m) {
        const GuestAddr dst = m.arg_ptr(0);
        const GuestAddr src = m.arg_ptr(1);
        const auto count = static_cast<usize>(m.arg(2));
        std::vector<u16> text;
        for (usize i = 0;; ++i) {
            auto unit = m.read_u16(src + i * 2);
            if (unit.is_err()) {
                m.set_return(0);
                return;
            }
            if (unit.value() == 0) break;
            text.push_back(unit.value());
        }
        const usize copy = count > text.size() ? text.size() + 1
                                               : count;
        for (usize i = 0; i < copy; ++i) {
            (void)m.write_u16(dst + i * 2,
                              i < text.size() ? text[i] : u16{0});
        }
        m.set_return(static_cast<u64>(text.size()));
    }});
}

} // namespace vitreo::winapi
