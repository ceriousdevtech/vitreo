#pragma once
/// @file ansi_char_message.hpp
/// @brief ANSI <-> Unicode translation of character messages (WM_CHAR,
///        WM_DEADCHAR, WM_SYSCHAR, WM_SYSDEADCHAR, WM_IME_CHAR) where USER
///        crosses the A/W boundary: PostMessageA / PostThreadMessageA store
///        the Unicode form, GetMessageA / PeekMessageA return the ANSI form,
///        and DispatchMessage converts for a procedure of the other charset.
///
/// Measured on Windows 11 (code page 1252) by
/// tools/oracle_probe/s2msg_ansi_msg_probe (.expected / .x86.expected):
///  - Unicode -> ANSI uses WideCharToMultiByte's best fit (U+03B1 -> 'a',
///    a lone surrogate -> '?'). x64 converts the low two UTF-16 units of
///    wParam into the low two bytes (0x100020AC -> 0x3F80); x86 converts the
///    low unit only (0x100020AC -> 0x80).
///  - ANSI -> Unicode converts the low two bytes into the low two UTF-16
///    units on both (0x80 -> 0x20AC, 0x12340080 -> 0x20AC, 0x20AC ->
///    0x2000AC).
///  - Every other message, WM_KEYDOWN and application messages included,
///    passes untouched.

#include <vitreo/common.hpp>
#include "winapi/kernel32/locale/codepage_convert.hpp"

namespace vitreo::winapi::ansi_char_message {

[[nodiscard]] constexpr bool is_char_message(u32 message) noexcept {
    return message == 0x0102 /* WM_CHAR */ ||
           message == 0x0103 /* WM_DEADCHAR */ ||
           message == 0x0106 /* WM_SYSCHAR */ ||
           message == 0x0107 /* WM_SYSDEADCHAR */ ||
           message == 0x0286 /* WM_IME_CHAR */;
}

/// Unicode wParam -> ANSI wParam (CP_ACP).
[[nodiscard]] inline u64 to_ansi(u64 wparam, bool is_64bit) {
    const u32 units = is_64bit ? 2u : 1u;
    u64 out = 0;
    for (u32 i = 0; i < units; ++i) {
        const u16 unit = static_cast<u16>(wparam >> (16u * i));
        u8 bytes[4] = {};
        const auto r = codepage::wide_to_multi_byte(
            codepage::kCpAcp, 0, &unit, 1, bytes, sizeof(bytes), nullptr, false);
        out |= static_cast<u64>(r.count != 0 ? bytes[0] : 0u) << (8u * i);
    }
    return out;
}

/// ANSI wParam -> Unicode wParam (CP_ACP).
[[nodiscard]] inline u64 to_unicode(u64 wparam) {
    u64 out = 0;
    for (u32 i = 0; i < 2; ++i) {
        const u8 byte = static_cast<u8>(wparam >> (8u * i));
        u16 unit = 0;
        const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, &byte, 1,
                                                    &unit, 1);
        out |= static_cast<u64>(r.count != 0 ? unit : 0u) << (16u * i);
    }
    return out;
}

}  // namespace vitreo::winapi::ansi_char_message
