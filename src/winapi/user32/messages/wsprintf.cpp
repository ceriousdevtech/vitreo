// SPDX-License-Identifier: LGPL-2.1-only

/// @file wsprintf.cpp
/// @brief wsprintf / wvsprintf (CDECL) format helpers and thunks.
///
/// The printf-style USER32 string formatters. Calling convention is CDECL
/// with a varargs-shaped guest frame, which is why they own a small format
/// engine rather than forwarding to CRT printf.
///
/// Closed cut from user32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/ansi_text.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ---------- wsprintf (CDECL) ----------

namespace {

/// Length prefix on a wsprintf conversion.
enum class WsprintfLength { Default, Short, Long };

/// Format one call of the wsprintf/wvsprintf family.
///
/// Windows accepts the `h` (short) and `l`/`w` (long/wide) prefixes on every
/// conversion, and they are not decoration. A formatter that does not consume
/// the prefix does not merely lose precision — it fails to recognise the
/// conversion at all and copies the spec through as literal text. That is how
/// RollerCoaster Tycoon 3's setup engine came to create a file named
/// "%hx.rra": it asked for `%hx` and got the four characters back.
///
/// The prefix also selects character width on the string conversions: `%ls`
/// names a wide string even from the ANSI entry point, and `%hs` an ANSI one
/// even from the wide entry point.
///
/// Both the ANSI and wide entry points share this one implementation, working
/// in UTF-16 code units; the ANSI callers write the low byte of each unit,
/// which is what they did when they narrowed strings themselves.
[[nodiscard]] std::vector<u16> format_wsprintf(
    Marshaler& m,
    const std::vector<u16>& fmt,
    const std::function<u64()>& next_arg,
    const std::function<GuestAddr()>& next_ptr,
    bool wide_variant) {
    std::vector<u16> out;
    out.reserve(fmt.size() + 64);

    const auto append_ascii = [&out](std::string_view text) {
        for (const char c : text) {
            out.push_back(static_cast<u16>(static_cast<unsigned char>(c)));
        }
    };

    for (usize i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != u'%') { out.push_back(fmt[i]); continue; }

        const usize spec_start = i;
        ++i;
        if (i >= fmt.size()) { out.push_back(u'%'); break; }

        // Flags, width and precision are reconstructed into a host format
        // string for the numeric conversions, and applied by hand for strings.
        // wsprintf's flags are '-', '#' and '0' only (Learn); '+' and ' '
        // are not flags and fall to the unknown-conversion rule below
        // (measured, s1win_misc_probe wsprintfa_flags: "%+d" -> "+d").
        std::string host_spec = "%";
        bool left_justify = false;
        for (; i < fmt.size(); ++i) {
            const u16 c = fmt[i];
            if (c != u'-' && c != u'0' && c != u'#') break;
            if (c == u'-') left_justify = true;
            host_spec.push_back(static_cast<char>(c));
        }
        usize width = 0;
        for (; i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9'; ++i) {
            width = width * 10 + static_cast<usize>(fmt[i] - u'0');
            host_spec.push_back(static_cast<char>(fmt[i]));
        }
        usize precision = std::string::npos;
        if (i < fmt.size() && fmt[i] == u'.') {
            host_spec.push_back('.');
            ++i;
            usize value = 0;
            for (; i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9'; ++i) {
                value = value * 10 + static_cast<usize>(fmt[i] - u'0');
                host_spec.push_back(static_cast<char>(fmt[i]));
            }
            precision = value;
        }

        WsprintfLength length = WsprintfLength::Default;
        bool int64 = false;
        for (bool consuming = true; consuming && i < fmt.size();) {
            switch (fmt[i]) {
                case u'h': length = WsprintfLength::Short; ++i; break;
                case u'l': case u'w': case u'L':
                    length = WsprintfLength::Long; ++i; break;
                case u'I':
                    // "%I64d": a 64-bit integer (measured, wsprintfa_unsupported).
                    ++i;
                    if (i + 1 < fmt.size() && fmt[i] == u'6' && fmt[i + 1] == u'4') {
                        i += 2;
                        int64 = true;
                    }
                    length = WsprintfLength::Long;
                    break;
                default: consuming = false; break;
            }
        }
        const auto next_int64 = [&]() -> u64 {
            if (m.cpu_is_64bit()) return next_arg();
            const u64 lo = static_cast<u32>(next_arg());
            const u64 hi = static_cast<u32>(next_arg());
            return lo | (hi << 32);
        };
        if (i >= fmt.size()) {
            // Nothing to convert: emit what was parsed, verbatim.
            for (usize k = spec_start; k < fmt.size(); ++k) out.push_back(fmt[k]);
            break;
        }

        const u16 conv = fmt[i];
        // On the wide entry points %s/%c default to wide and %S/%C to ANSI;
        // on the ANSI ones it is the other way round. An explicit prefix wins.
        const auto is_wide_text = [&](bool inverted) {
            const bool base = wide_variant != inverted;
            if (length == WsprintfLength::Short) return false;
            if (length == WsprintfLength::Long)  return true;
            return base;
        };

        char buffer[64];
        switch (conv) {
            case u'%': out.push_back(u'%'); break;

            case u'd': case u'i': {
                if (int64) {
                    std::snprintf(buffer, sizeof(buffer), (host_spec + "lld").c_str(),
                                  static_cast<long long>(next_int64()));
                    append_ascii(buffer);
                    break;
                }
                const u64 raw = next_arg();
                const i32 value = length == WsprintfLength::Short
                    ? static_cast<i32>(static_cast<i16>(static_cast<u16>(raw)))
                    : static_cast<i32>(static_cast<u32>(raw));
                std::snprintf(buffer, sizeof(buffer), (host_spec + "d").c_str(),
                              value);
                append_ascii(buffer);
                break;
            }
            case u'p': {
                // A pointer, zero-padded hex to the pointer width (measured:
                // 16 digits on x64, 8 on x86).
                const u64 raw = next_arg();
                if (m.cpu_is_64bit()) {
                    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(raw));
                } else {
                    std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<u32>(raw));
                }
                append_ascii(buffer);
                break;
            }
            case u'u': case u'x': case u'X': case u'o': {
                if (int64) {
                    host_spec += "ll";
                    host_spec.push_back(static_cast<char>(conv));
                    std::snprintf(buffer, sizeof(buffer), host_spec.c_str(),
                                  static_cast<unsigned long long>(next_int64()));
                    append_ascii(buffer);
                    break;
                }
                const u64 raw = next_arg();
                const u32 value = length == WsprintfLength::Short
                    ? static_cast<u32>(static_cast<u16>(raw))
                    : static_cast<u32>(raw);
                host_spec.push_back(static_cast<char>(conv));
                std::snprintf(buffer, sizeof(buffer), host_spec.c_str(), value);
                append_ascii(buffer);
                break;
            }
            case u'c': case u'C': {
                const u64 raw = next_arg();
                const u16 unit = is_wide_text(conv == u'C')
                    ? static_cast<u16>(raw)
                    : static_cast<u16>(raw & 0xFF);
                usize pad = width > 1 ? width - 1 : 0;
                if (!left_justify) while (pad-- > 0) out.push_back(u' ');
                out.push_back(unit);
                if (left_justify) while (pad-- > 0) out.push_back(u' ');
                break;
            }
            case u's': case u'S': {
                const GuestAddr pointer = next_ptr();
                std::wstring text;  // a NULL string formats as empty (measured)
                if (is_wide_text(conv == u'S')) {
                    if (auto r = m.read_wide_string(pointer); r.is_ok()) {
                        text = utf8_to_wstring(r.value());
                    }
                } else {
                    if (auto r = m.read_ansi_string(pointer); r.is_ok()) {
                        text = ansi_text::to_wide(r.value());
                    }
                }
                if (precision != std::string::npos && text.size() > precision) {
                    text.resize(precision);
                }
                usize pad = width > text.size() ? width - text.size() : 0;
                if (!left_justify) while (pad-- > 0) out.push_back(u' ');
                for (wchar_t c : text) out.push_back(static_cast<u16>(c));
                if (left_justify) while (pad-- > 0) out.push_back(u' ');
                break;
            }
            default:
                // Unknown conversion character: it is output without the
                // '%' (measured: "%f" -> "f", "%+d" -> "+d").
                (void)spec_start;
                out.push_back(conv);
                break;
        }
    }
    return out;
}

/// wsprintf output is limited to 1024 characters (Learn; measured
/// wsprintfa_1024_limit: 1024 returned and stored).
constexpr usize kWsprintfLimit = 1024;

/// The ANSI entry points store the formatted UTF-16 text in CP_ACP.
[[nodiscard]] std::string narrow_output(const std::vector<u16>& out) {
    std::wstring w;
    w.reserve(out.size());
    for (u16 u : out) w.push_back(static_cast<wchar_t>(u));
    std::string a = ansi_text::to_ansi(w);
    if (a.size() > kWsprintfLimit) a.resize(kWsprintfLimit);
    return a;
}

/// Widen an ANSI format string into the code units the shared formatter takes.
[[nodiscard]] std::vector<u16> widen_format(std::string_view text) {
    std::vector<u16> out;
    for (wchar_t c : ansi_text::to_wide(std::string(text))) out.push_back(static_cast<u16>(c));
    return out;
}

} // namespace

void User32ExtManager::impl_wsprintfA(Marshaler& m) {
    const GuestAddr lpOut = m.arg_ptr(0);
    const GuestAddr lpFmt = m.arg_ptr(1);
    if (lpOut == 0 || lpFmt == 0) { m.set_return(0); return; }
    auto fmt_res = m.read_ansi_string(lpFmt);
    if (fmt_res.is_err()) { m.set_return(0); return; }

    u32 var_idx = 2; // first vararg slot on the guest stack
    const auto out = format_wsprintf(
        m, widen_format(fmt_res.value()),
        [&m, &var_idx]() -> u64 { return m.arg(var_idx++); },
        [&m, &var_idx]() -> GuestAddr { return m.arg_ptr(var_idx++); },
        /*wide_variant=*/false);

    const std::string narrow = narrow_output(out);
    (void)m_memory.write_bytes(lpOut, reinterpret_cast<const Byte*>(narrow.data()), narrow.size());
    (void)m_memory.write_u8(lpOut + narrow.size(), 0);
    const std::string& preview = narrow;
    VITREO_TRACE("[user32_ext] wsprintfA(\"%s\") -> \"%s\" (len=%u)\n",
                 fmt_res.value().c_str(), preview.c_str(),
                 static_cast<u32>(out.size()));
    m.set_return(static_cast<u32>(narrow.size()));
}

/// Read a NUL-terminated UTF-16 format string out of guest memory.
static std::vector<u16> read_wide_format(GuestMemory& memory, GuestAddr addr) {
    std::vector<u16> fmt;
    fmt.reserve(256);
    for (usize i = 0; ; ++i) {
        auto unit = memory.read_u16(addr + i * 2);
        if (unit.is_err() || unit.value() == 0) break;
        fmt.push_back(unit.value());
    }
    return fmt;
}

void User32ExtManager::impl_wsprintfW(Marshaler& m) {
    const GuestAddr lpOut = m.arg_ptr(0);
    const GuestAddr lpFmt = m.arg_ptr(1);
    if (lpOut == 0 || lpFmt == 0) { m.set_return(0); return; }

    u32 var_idx = 2;
    const auto out = format_wsprintf(
        m, read_wide_format(m_memory, lpFmt),
        [&m, &var_idx]() -> u64 { return m.arg(var_idx++); },
        [&m, &var_idx]() -> GuestAddr { return m.arg_ptr(var_idx++); },
        /*wide_variant=*/true);

    const usize count = std::min(out.size(), kWsprintfLimit);
    for (usize j = 0; j < count; ++j) {
        (void)m_memory.write_u16(lpOut + j * 2, out[j]);
    }
    (void)m_memory.write_u16(lpOut + count * 2, 0);

    VITREO_TRACE("[user32_ext] wsprintfW(len=%u)\n", static_cast<u32>(count));
    m.set_return(static_cast<u32>(count));
}

// int wvsprintfA(LPSTR lpOut, LPCSTR lpFmt, va_list arglist)
// Same formatting as wsprintfA, but arguments come from the va_list pointer
// (an array of DWORD-sized slots in guest memory) rather than the guest stack.
void User32ExtManager::impl_wvsprintfA(Marshaler& m) {
    const GuestAddr lpOut = m.arg_ptr(0);
    const GuestAddr lpFmt = m.arg_ptr(1);
    GuestAddr va          = m.arg_ptr(2);
    if (lpOut == 0 || lpFmt == 0) { m.set_return(0); return; }
    auto fmt_res = m.read_ansi_string(lpFmt);
    if (fmt_res.is_err()) { m.set_return(0); return; }

    // Arguments come from a va_list in guest memory: an array of DWORD slots.
    // A va_list is an array of pointer-sized slots: 4 bytes under the x86
    // cdecl stack layout, 8 bytes under the MS x64 ABI, which homes every
    // variadic argument in an 8-byte slot.
    const bool wide_slots = m.cpu_is_64bit();
    const auto next_slot = [this, &va, wide_slots]() -> u64 {
        if (wide_slots) {
            auto v = m_memory.read_u64(va);
            va += 8;
            return v.is_ok() ? v.value() : 0u;
        }
        auto v = m_memory.read_u32(va);
        va += 4;
        return v.is_ok() ? static_cast<u64>(v.value()) : 0u;
    };
    const auto out = format_wsprintf(
        m, widen_format(fmt_res.value()), next_slot,
        [&next_slot]() -> GuestAddr {
            return static_cast<GuestAddr>(next_slot());
        },
        /*wide_variant=*/false);

    const std::string narrow = narrow_output(out);
    (void)m_memory.write_bytes(lpOut, reinterpret_cast<const Byte*>(narrow.data()), narrow.size());
    (void)m_memory.write_u8(lpOut + narrow.size(), 0);
    const std::string& preview = narrow;
    VITREO_TRACE("[user32_ext] wvsprintfA(\"%s\") -> \"%s\" (len=%u)\n",
                 fmt_res.value().c_str(), preview.c_str(),
                 static_cast<u32>(out.size()));
    m.set_return(static_cast<u32>(narrow.size()));
}

void User32ExtManager::impl_wvsprintfW(Marshaler& m) {
    const GuestAddr lpOut = m.arg_ptr(0);
    const GuestAddr lpFmt = m.arg_ptr(1);
    GuestAddr va          = m.arg_ptr(2);
    if (lpOut == 0 || lpFmt == 0) { m.set_return(0); return; }

    // A va_list is an array of pointer-sized slots: 4 bytes under the x86
    // cdecl stack layout, 8 bytes under the MS x64 ABI, which homes every
    // variadic argument in an 8-byte slot.
    const bool wide_slots = m.cpu_is_64bit();
    const auto next_slot = [this, &va, wide_slots]() -> u64 {
        if (wide_slots) {
            auto v = m_memory.read_u64(va);
            va += 8;
            return v.is_ok() ? v.value() : 0u;
        }
        auto v = m_memory.read_u32(va);
        va += 4;
        return v.is_ok() ? static_cast<u64>(v.value()) : 0u;
    };
    const auto out = format_wsprintf(
        m, read_wide_format(m_memory, lpFmt), next_slot,
        [&next_slot]() -> GuestAddr {
            return static_cast<GuestAddr>(next_slot());
        },
        /*wide_variant=*/true);

    const usize count = std::min(out.size(), kWsprintfLimit);
    for (usize j = 0; j < count; ++j) {
        (void)m_memory.write_u16(lpOut + j * 2, out[j]);
    }
    (void)m_memory.write_u16(lpOut + count * 2, 0);

    VITREO_TRACE("[user32_ext] wvsprintfW(len=%u)\n", static_cast<u32>(count));
    m.set_return(static_cast<u32>(count));
}

} // namespace vitreo::winapi
