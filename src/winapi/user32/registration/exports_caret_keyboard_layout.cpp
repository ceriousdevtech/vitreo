// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_caret_keyboard_layout.cpp
/// @brief user32 extended export table, slice "caret_keyboard_layout".
///
/// Caret, cursor, class-word and keyboard-layout identity entries.
///
/// One contiguous run of the former create_user32_ext_thunks body, moved
/// verbatim; user32_ext_registration.cpp calls the slices in registration
/// order.

#include "winapi/user32/registration/registration_parts.hpp"

#include "ipc/hwnd_registry.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/user32/windows/set_cursor_pos.hpp"
#include "winapi/user32/messages/mouse_in_pointer.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/user32/windows/kbd_layout.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <set>

#include <mutex>

#include <unordered_map>

namespace vitreo::winapi {

void add_user32_ext_exports_caret_keyboard_layout(DllExports& exports, User32ExtManager& mgr) {
    // ------------------------------------------------------------------
    // Caret, cursor, keyboard, ancestor, class word, device notification,
    // and FlashWindow. Each export is implemented; none of them is the
    // 0xFFFFFFFF missing-export fail-thunk.
    // ------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "CreateCaret", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateCaret(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "DestroyCaret", 0,
        [&mgr](Marshaler& m) { mgr.impl_DestroyCaret(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "GetCaretPos", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCaretPos(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetCaretBlinkTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCaretBlinkTime(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "SetCaretBlinkTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetCaretBlinkTime(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "SetCaretPos", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetCaretPos(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "HideCaret", 0,
        [&mgr](Marshaler& m) { mgr.impl_ShowHideCaret(m, false); },
        4});
    exports.entries.push_back(ExportEntry{
        "ShowCaret", 0,
        [&mgr](Marshaler& m) { mgr.impl_ShowHideCaret(m, true); },
        4});
    exports.entries.push_back(ExportEntry{
        "FlashWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_FlashWindow(m); },
        8});
    // int GetKeyboardLayoutList(int nBuff, HKL* lpList): the one layout
    // Vitreo has. nBuff 0 asks for the count; an HKL is pointer-sized.
    exports.entries.push_back(ExportEntry{
        "GetKeyboardLayoutList", 0,
        [](Marshaler& m) {
            const i32 nBuff = static_cast<i32>(m.arg(0));
            const GuestAddr list = m.arg_ptr(1);
            if (nBuff == 0) {
                m.set_return(1u);
                return;
            }
            if (list == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            if (m.cpu_is_64bit()) {
                (void)m.write_u64(list, kbd_us::kHkl);
            } else {
                (void)m.write_u32(list, kbd_us::kHkl);
            }
            m.set_return(1u);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "RegisterDeviceNotificationA", 0,
        // The filter structures differ from W only in the name strings
        // after the type field, which registration does not read.
        [&mgr](Marshaler& m) { mgr.impl_RegisterDeviceNotificationW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetCursor", 0,
        [&mgr](Marshaler& m) {
            // Microsoft Learn, GetCursor: return the current cursor handle,
            // or NULL when no cursor is installed. SetCursor owns this
            // process-visible state; fabricating an unrelated handle prevents
            // toolkits from resolving the current shape and can make their
            // WM_SETCURSOR path replace it with NULL.
            m.set_return(static_cast<u64>(mgr.m_current_cursor));
        },
        0});
    exports.entries.push_back(ExportEntry{
        "GetClassWord", 0,
        [&mgr](Marshaler& m) { mgr.class_long_get(m, 2); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetClassWord", 0,
        [&mgr](Marshaler& m) { mgr.class_long_set(m, 2); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetAncestor", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetAncestor(m); },
        8});
    // HKL ActivateKeyboardLayout(HKL hkl, UINT Flags): the previous layout
    // -- always the one layout -- for that layout or HKL_NEXT / HKL_PREV
    // (1 / 0); 0 with the last error untouched for a layout that is not
    // loaded (s2msg_input_probe activatelayout_*).
    exports.entries.push_back(ExportEntry{
        "ActivateKeyboardLayout", 0,
        [](Marshaler& m) {
            const u64 hkl = m.cpu_is_64bit() ? m.arg(0) : static_cast<u32>(m.arg(0));
            (void)m.arg(1);
            const bool known = hkl == kbd_us::kHkl || hkl == 0 || hkl == 1;
            m.set_return(known ? static_cast<u64>(kbd_us::kHkl) : 0u);
        },
        8});

    // ------------------------------------------------------------------------
    // Keyboard layout identity.
    //
    // Vitreo exposes exactly one layout, US English (HKL 0x04090409, KLID
    // "00000409"). GetKeyboardLayoutName and MapVirtualKey answer that same
    // layout.
    // ------------------------------------------------------------------------
    static constexpr u32 kUsEnglishHkl = 0x04090409u;
    static constexpr char kUsEnglishKlid[] = "00000409";

    // BOOL GetKeyboardLayoutNameA(LPSTR pwszKLID)
    //
    // The buffer is KL_NAMELENGTH (9) chars: 8 hex digits plus the terminator.
    exports.entries.push_back(ExportEntry{
        "GetKeyboardLayoutNameA", 0,
        [](Marshaler& m) {
            const GuestAddr buffer = m.arg_ptr(0);
            if (buffer == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            if (m.write_ansi_string(buffer, kUsEnglishKlid).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
        },
        4});

    exports.entries.push_back(ExportEntry{
        "GetKeyboardLayoutNameW", 0,
        [](Marshaler& m) {
            const GuestAddr buffer = m.arg_ptr(0);
            if (buffer == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            for (u32 i = 0; i < 8; ++i) {
                (void)m.write_u16(buffer + i * 2,
                                  static_cast<u8>(kUsEnglishKlid[i]));
            }
            (void)m.write_u16(buffer + 16, 0);
            m.set_return(1u);
        },
        4});

    // HKL LoadKeyboardLayoutA(LPCSTR pwszKLID, UINT Flags)
    //
    // Only the one layout Vitreo implements can be loaded. Any other KLID is
    // reported as unavailable rather than silently aliased to US English,
    // because a guest that gets a handle back will assume the requested layout
    // is in effect and mis-translate every key.
    const auto load_keyboard_layout = [](Marshaler& m, bool wide) {
        auto klid = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
        (void)m.arg(1);  // Flags
        if (klid.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        if (klid.value() != kUsEnglishKlid) {
            VITREO_TRACE("[u32ext] LoadKeyboardLayout(\"%s\") -> NULL "
                         "(only \"%s\" is implemented)\n",
                         klid.value().c_str(), kUsEnglishKlid);
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            m.set_return(0u);
            return;
        }
        m.set_return(kUsEnglishHkl);
    };
    exports.entries.push_back(ExportEntry{
        "LoadKeyboardLayoutA", 0,
        [load_keyboard_layout](Marshaler& m) {
            load_keyboard_layout(m, false);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "LoadKeyboardLayoutW", 0,
        [load_keyboard_layout](Marshaler& m) {
            load_keyboard_layout(m, true);
        },
        8});

    // BOOL UnloadKeyboardLayout(HKL hkl) — the built-in layout cannot be
    // unloaded, which is exactly what Windows reports for the system default.
    exports.entries.push_back(ExportEntry{
        "UnloadKeyboardLayout", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
        },
        4});

    // int ToAsciiEx(UINT uVirtKey, UINT uScanCode, const BYTE* lpKeyState,
    //               LPWORD lpChar, UINT uFlags, HKL dwhkl)
    //
    // Returns 1 and writes one character when the key maps to a printable
    // character in the US layout, 0 when it does not. Shift and Caps Lock are
    // honored because the caller passes the full 256-byte key state.
    const auto to_ascii = [](Marshaler& m, bool has_hkl) {
        const auto virt_key = static_cast<u32>(m.arg(0));
        (void)m.arg(1);  // scan code — the VK is authoritative here
        const GuestAddr key_state = m.arg_ptr(2);
        const GuestAddr out_char = m.arg_ptr(3);
        (void)m.arg(4);  // uFlags (menu-active)
        if (has_hkl) (void)m.arg(5);

        if (out_char == 0) {
            m.set_return(0u);
            return;
        }

        bool shift = false;
        bool caps = false;
        bool ctrl = false;
        if (key_state != 0) {
            auto vk_shift = m.read_u8(key_state + 0x10);   // VK_SHIFT
            auto vk_ctrl = m.read_u8(key_state + 0x11);    // VK_CONTROL
            auto vk_capital = m.read_u8(key_state + 0x14); // VK_CAPITAL
            shift = vk_shift.is_ok() && (vk_shift.value() & 0x80) != 0;
            ctrl = vk_ctrl.is_ok() && (vk_ctrl.value() & 0x80) != 0;
            caps = vk_capital.is_ok() && (vk_capital.value() & 0x01) != 0;
        }

        u32 ch = map_virtual_key(virt_key, 2);  // MAPVK_VK_TO_CHAR
        if (ch == 0) {
            m.set_return(0u);
            return;
        }

        if (virt_key >= 'A' && virt_key <= 'Z') {
            const bool upper = shift != caps;
            if (!upper) ch += 0x20;
            if (ctrl) ch = virt_key - 'A' + 1;  // Ctrl+A .. Ctrl+Z
        } else if (shift) {
            // US-layout shifted forms of the digit row and OEM keys.
            switch (virt_key) {
            case '1': ch = '!'; break;
            case '2': ch = '@'; break;
            case '3': ch = '#'; break;
            case '4': ch = '$'; break;
            case '5': ch = '%'; break;
            case '6': ch = '^'; break;
            case '7': ch = '&'; break;
            case '8': ch = '*'; break;
            case '9': ch = '('; break;
            case '0': ch = ')'; break;
            case 0xBA: ch = ':'; break;
            case 0xBB: ch = '+'; break;
            case 0xBC: ch = '<'; break;
            case 0xBD: ch = '_'; break;
            case 0xBE: ch = '>'; break;
            case 0xBF: ch = '?'; break;
            case 0xC0: ch = '~'; break;
            case 0xDB: ch = '{'; break;
            case 0xDC: ch = '|'; break;
            case 0xDD: ch = '}'; break;
            case 0xDE: ch = '"'; break;
            default: break;
            }
        }

        (void)m.write_u16(out_char, static_cast<u16>(ch & 0xFF));
        m.set_return(1u);
    };
    exports.entries.push_back(ExportEntry{
        "ToAscii", 0,
        [to_ascii](Marshaler& m) { to_ascii(m, false); },
        20});
    exports.entries.push_back(ExportEntry{
        "ToAsciiEx", 0,
        [to_ascii](Marshaler& m) { to_ascii(m, true); },
        24});
}

} // namespace vitreo::winapi
