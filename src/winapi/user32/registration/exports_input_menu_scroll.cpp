// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_input_menu_scroll.cpp
/// @brief user32 extended export table, slice "input_menu_scroll".
///
/// Scroll, menu, keyboard, and accelerator exports.
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

void add_user32_ext_exports_input_menu_scroll(DllExports& exports, User32ExtManager& mgr) {
    // --- FindWindow, scroll, menu, keyboard, and accelerator exports ---

    exports.entries.push_back(
        {"RegisterWindowMessageA",
         0,
         [&mgr](Marshaler& m) { mgr.impl_RegisterWindowMessageA(m); },
         4});

    // FindWindowA/W(lpClassName, lpWindowName) → HWND. See
    // User32ExtManager::find_window_impl for the contract. Both A and W
    // forms are exported.
    exports.entries.push_back(
        {"FindWindowA", 0,
         [&mgr](Marshaler& m) { mgr.find_window_impl(m, /*ex=*/false, /*ansi=*/true); },
         8});
    exports.entries.push_back(
        {"FindWindowW", 0,
         [&mgr](Marshaler& m) { mgr.find_window_impl(m, /*ex=*/false, /*ansi=*/false); },
         8});

    // GetCursorPos(lpPoint) → BOOL — delegate to NSEvent mouseLocation.
    // GetPhysicalCursorPos is the same point: no process is DPI-scaled, so
    // physical and logical coordinates coincide (LogicalToPhysicalPoint is
    // the identity).
    const auto cursor_pos =
         [&mgr](Marshaler& m) {
            // getcursorpos_null: FALSE / ERROR_NOACCESS.
            if (m.arg_ptr(0) == 0) { set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0); return; }
            auto pt = m.arg_ptr(0);
            i32 x = 0;
            i32 y = 0;
            mgr.cursor_host().get_native_cursor_pos(x, y);
            (void)mgr.memory().write_u32(pt, static_cast<u32>(x));
            (void)mgr.memory().write_u32(pt + 4, static_cast<u32>(y));
            if (std::getenv("VITREO_INPUT_DIAG")) {
                std::fprintf(stderr, "[input] GetCursorPos -> (%d,%d)\n", x, y);
            }
            m.set_return(1u); // TRUE
         };
    exports.entries.push_back({"GetCursorPos", 0, cursor_pos, 4});
    exports.entries.push_back({"GetPhysicalCursorPos", 0, cursor_pos, 4});

    // DeleteMenu(hMenu, uPosition, uFlags) → BOOL
    exports.entries.push_back(
        {"DeleteMenu",
         0,
         [&mgr](Marshaler& m) { mgr.impl_DeleteMenu(m); },
         12});

    // UINT MapVirtualKeyA(UINT uCode, UINT uMapType)
    // An invalid uMapType fails with ERROR_INVALID_PARAMETER
    // (s2msg_input_probe mapvk_type5 / mapvk_type7).
    const auto map_vk = [](Marshaler& m) {
        u32 result = 0;
        if (!kbd_us::map_virtual_key(static_cast<u32>(m.arg(0)),
                                     static_cast<u32>(m.arg(1)), result)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(result);
    };
    exports.entries.push_back({"MapVirtualKeyA", 0, map_vk, 8});

    // UINT MapVirtualKeyExA(UINT uCode, UINT uMapType, HKL dwhkl)
    //
    // Vitreo exposes exactly one layout, so the HKL selects nothing; it is
    // still read so that a guest passing garbage sees no difference from
    // Windows, which ignores an unknown HKL for the active layout.
    exports.entries.push_back({"MapVirtualKeyExA", 0, map_vk, 12});

    // GetKeyNameTextA(lParam, lpString, cchSize) → int
    exports.entries.push_back(
        {"GetKeyNameTextA",
         0,
         [&mgr](Marshaler& m) {
            u32 lParam = static_cast<u32>(m.arg(0));
            auto buf   = m.arg_ptr(1);
            i32 cch    = static_cast<i32>(m.arg(2));

            // Extract scan code from bits 16-23 of lParam
            u32 scan_code = (lParam >> 16) & 0xFF;

            // Map common scan codes to key names
            const char* name = "";
            switch (scan_code) {
                case 0x01: name = "Esc"; break;
                case 0x0E: name = "Backspace"; break;
                case 0x0F: name = "Tab"; break;
                case 0x1C: name = "Enter"; break;
                case 0x39: name = "Space"; break;
                case 0x53: name = "Delete"; break;
                case 0x48: name = "Up"; break;
                case 0x50: name = "Down"; break;
                case 0x4B: name = "Left"; break;
                case 0x4D: name = "Right"; break;
                case 0x3B: name = "F1"; break;
                case 0x3C: name = "F2"; break;
                case 0x3D: name = "F3"; break;
                case 0x3E: name = "F4"; break;
                case 0x3F: name = "F5"; break;
                case 0x40: name = "F6"; break;
                case 0x41: name = "F7"; break;
                case 0x42: name = "F8"; break;
                case 0x43: name = "F9"; break;
                case 0x44: name = "F10"; break;
                case 0x57: name = "F11"; break;
                case 0x58: name = "F12"; break;
                default: {
                    // Try to map scan code to a letter
                    static const char sc_letters[] = "??1234567890??"
                                                     "?QWERTYUIOP??"
                                                     "?ASDFGHJKL???"
                                                     "?ZXCVBNM";
                    if (scan_code < sizeof(sc_letters) - 1 && sc_letters[scan_code] != '?') {
                        static char single[2] = {};
                        single[0] = sc_letters[scan_code];
                        single[1] = 0;
                        name = single;
                    }
                    break;
                }
            }

            i32 name_len = static_cast<i32>(strlen(name));
            if (cch <= 0 || buf == 0) { m.set_return(0u); return; }

            i32 copy_len = std::min(name_len, cch - 1);
            if (copy_len > 0) {
                (void)mgr.memory().write_bytes(buf, reinterpret_cast<const Byte*>(name),
                                               static_cast<usize>(copy_len));
            }
            (void)mgr.memory().write_u8(buf + static_cast<u32>(copy_len), 0);
            m.set_return(static_cast<u32>(copy_len));
         },
         12});

    // WinHelpA(hWndMain, lpszHelp, uCommand, dwData) → BOOL
    exports.entries.push_back(
        {"WinHelpA",
         0,
         [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(0); m.set_return(0); return; }
            auto hwnd = static_cast<u32>(m.arg(0));
            VITREO_TRACE("[user32_ext] WinHelpA(hwnd=0x%x) stub → TRUE\n", hwnd);
            (void)mgr;
            m.set_return(1u); // TRUE — no-op
         },
         16});

    // ChangeDisplaySettingsA(lpDevMode, dwFlags) → LONG
    exports.entries.push_back(
        {"ChangeDisplaySettingsA",
         0,
         [&mgr](Marshaler& m) {
            mgr.impl_ChangeDisplaySettingsA(m);
         },
         8});

    // DialogBoxIndirectParamA(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam) → INT_PTR
    exports.entries.push_back(
        {"DialogBoxIndirectParamA",
         0,
         [&mgr](Marshaler& m) { mgr.impl_DialogBoxIndirectParamA(m); },
         20});

    // MessageBeep(uType) → BOOL
    exports.entries.push_back(
        {"MessageBeep",
         0,
         [&mgr](Marshaler& m) {
            auto uType = static_cast<u32>(m.arg(0));
            VITREO_TRACE("[user32_ext] MessageBeep(type=0x%x) stub\n", uType);
            (void)mgr;
            m.set_return(1u); // TRUE — no-op
         },
         4});

    // GetActiveWindow() → HWND — 0 args, stdcall_bytes=0
    exports.entries.push_back(
        {"GetActiveWindow",
         0,
         [&mgr](Marshaler& m) {
                mgr.impl_GetActiveWindow(m);
         },
         0});

    // ======== Extended USER32 stubs (batch 2) ========

    // --- Scroll APIs ---
    exports.entries.push_back(ExportEntry{
        "EnableScrollBar", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_EnableScrollBar(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "ScrollWindow", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_ScrollWindow(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "SetScrollRange", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_SetScrollRange(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "GetScrollRange", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_GetScrollRange(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "ShowScrollBar", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_ShowScrollBar(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetScrollInfo", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_GetScrollInfo(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "SetScrollInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetScrollInfo(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "GetScrollPos", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetScrollPos(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetScrollPos", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetScrollPos(m); },
        16});

    // --- Menu APIs ---
    exports.entries.push_back(ExportEntry{
        "GetMenuDefaultItem", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuDefaultItem(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "SetMenuDefaultItem", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetMenuDefaultItem(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "IsMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsMenu(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "InsertMenuItemW", 0,
        [&mgr](Marshaler& m) { mgr.impl_InsertMenuItemW(m); },
        16});
    // InsertMenuItemA — the shared body does not read MENUITEMINFO.dwTypeData,
    // so it is charset-neutral.
    exports.entries.push_back(ExportEntry{
        "InsertMenuItemA", 0,
        [&mgr](Marshaler& m) { mgr.impl_InsertMenuItemA(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "DestroyMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_DestroyMenu(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetMenuItemInfoW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuItemInfoW(m); },
        16});
    // GetMenuItemInfoA — the shared body does not write MENUITEMINFO.dwTypeData,
    // so it is charset-neutral.
    exports.entries.push_back(ExportEntry{
        "GetMenuItemInfoA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuItemInfoA(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "ShowOwnedPopups", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_ShowOwnedPopups(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetMenuState", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuState(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetMenuStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuStringW(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "GetMenuItemID", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuItemID(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "InsertMenuW", 0,
        [&mgr](Marshaler& m) { mgr.impl_InsertMenuW(m); },
        20});
    // InsertMenuA — same arity/contract as W; Petite/SecuROM resolve this by name.
    exports.entries.push_back(ExportEntry{
        "InsertMenuA", 0,
        [&mgr](Marshaler& m) { mgr.impl_InsertMenuA(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "GetMenuItemCount", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuItemCount(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "SetMenuItemBitmaps", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetMenuItemBitmaps(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "GetMenuCheckMarkDimensions", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuCheckMarkDimensions(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "CreateMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateMenu(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "ModifyMenuA", 0,
        [&mgr](Marshaler& m) { mgr.impl_ModifyMenuA(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "ModifyMenuW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ModifyMenuW(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "RemoveMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_RemoveMenu(m); },
        12});

    // --- Keyboard/Input APIs ---
    exports.entries.push_back(ExportEntry{
        "GetKeyboardState", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetKeyboardState(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "SetKeyboardState", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetKeyboardState(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetKeyboardLayout", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetKeyboardLayout(m); },
        4});
    // int GetKeyboardType(int nTypeFlag) — report the IBM enhanced 101/102-key
    // keyboard (type 4), which is what the Cocoa key mapping models.
    exports.entries.push_back(ExportEntry{
        "GetKeyboardType", 0,
        [](Marshaler& m) {
            switch (static_cast<u32>(m.arg(0))) {
                case 0:  m.set_return(4u);  break; // keyboard type
                case 1:  m.set_return(0u);  break; // OEM subtype
                case 2:  m.set_return(12u); break; // function keys
                default:
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0u);
                    break;
            }
        },
        4});
    exports.entries.push_back(ExportEntry{
        "ToUnicodeEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_ToUnicodeEx(m); },
        28});
    exports.entries.push_back(ExportEntry{
        "MapVirtualKeyW", 0,
        [&mgr](Marshaler& m) { mgr.impl_MapVirtualKeyW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetKeyNameTextW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetKeyNameTextW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetAsyncKeyState", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetAsyncKeyState(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "MapVirtualKeyExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_MapVirtualKeyExW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetFocus", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetFocus(m); },
        0});

    // --- Accelerator Table APIs ---
    exports.entries.push_back(ExportEntry{
        "CreateAcceleratorTableA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateAcceleratorTableW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "CreateAcceleratorTableW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateAcceleratorTableW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "CopyAcceleratorTableA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyAcceleratorTableW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "CopyAcceleratorTableW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyAcceleratorTableW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "DestroyAcceleratorTable", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_ACCEL_HANDLE); m.set_return(0); return; }
        mgr.impl_DestroyAcceleratorTable(m); },
        4});
}

} // namespace vitreo::winapi
