// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_icon_clipboard_window.cpp
/// @brief user32 extended export table, slice "icon_clipboard_window".
///
/// Batch-2 icon, clipboard, DDE-param, window-management, timer, dialog, hook, MDI and system-parameter entries.
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

void add_user32_ext_exports_icon_clipboard_window(DllExports& exports, User32ExtManager& mgr) {
    // --- Icon/Cursor APIs ---
    exports.entries.push_back(ExportEntry{
        "CopyIcon", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyIcon(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetIconInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIconInfo(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "DrawIconEx", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_DrawIconEx(m); },
        36});
    exports.entries.push_back(ExportEntry{
        "CopyImage", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyImage(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "DrawIcon", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_DrawIcon(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "DestroyIcon", 0,
        [&mgr](Marshaler& m) {
        // DestroyIcon(NULL): FALSE, last error untouched (s3gdi_icon_probe).
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_DestroyIcon(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "CreateCursor", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0 && m.arg(4) == 0 && m.arg(5) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_CreateCursor(m); },
        28});
    exports.entries.push_back(ExportEntry{
        "DestroyCursor", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_DestroyCursor(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetIconInfoExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIconInfoEx(m, true); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetIconInfoExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIconInfoEx(m, false); },
        8});

    // --- Clipboard APIs ---
    exports.entries.push_back(ExportEntry{
        "RegisterClipboardFormatW", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            mgr.impl_RegisterClipboardFormatW(m);
            },
        4});
    exports.entries.push_back(ExportEntry{
        "RegisterClipboardFormatA", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            mgr.impl_RegisterClipboardFormatA(m);
        },
        4});
    exports.entries.push_back(ExportEntry{
        "IsClipboardFormatAvailable", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsClipboardFormatAvailable(m); },
        4});

    // --- DDE APIs ---
    exports.entries.push_back(ExportEntry{
        "PackDDElParam", 0,
        [&mgr](Marshaler& m) { mgr.impl_PackDDElParam(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "FreeDDElParam", 0,
        [&mgr](Marshaler& m) { mgr.impl_FreeDDElParam(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetComboBoxInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetComboBoxInfo(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "UnpackDDElParam", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnpackDDElParam(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "ReuseDDElParam", 0,
        [&mgr](Marshaler& m) { mgr.impl_ReuseDDElParam(m); },
        20});

    // --- Window Management APIs ---
    exports.entries.push_back(ExportEntry{
        "UpdateLayeredWindow", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_UpdateLayeredWindow(m); },
        36});
    exports.entries.push_back(ExportEntry{
        "UpdateLayeredWindowIndirect", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0 || m.arg_ptr(1) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        mgr.impl_UpdateLayeredWindowIndirect(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetLayeredWindowAttributes", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetLayeredWindowAttributes(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "SetParent", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetParent(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "LockWindowUpdate", 0,
        [&mgr](Marshaler& m) { mgr.impl_LockWindowUpdate(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "BringWindowToTop", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_BringWindowToTop(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "SetWindowRgn", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetWindowRgn(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetWindowRgn", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowRgn(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetWindowPlacement", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_SetWindowPlacement(m); },
        8});
    // CloseWindow minimizes; OpenIcon restores (window_pos.cpp).
    exports.entries.push_back(ExportEntry{
        "CloseWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_CloseWindow(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "OpenIcon", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenIcon(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "BeginDeferWindowPos", 0,
        [&mgr](Marshaler& m) { mgr.impl_BeginDeferWindowPos(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "DeferWindowPos", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeferWindowPos(m); },
        32});
    exports.entries.push_back(ExportEntry{
        "EndDeferWindowPos", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_DWP_HANDLE); m.set_return(0); return; }
        mgr.impl_EndDeferWindowPos(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetTopWindow", 0,
        [&mgr](Marshaler& m) {
            // Measured (gettopwindow_invalid): ERROR_INVALID_WINDOW_HANDLE.
            const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            if (hwnd != 0 && !mgr.window_exists(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            mgr.impl_GetTopWindow(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindow(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetLastActivePopup", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_GetLastActivePopup(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "SetActiveWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetActiveWindow(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "WindowFromPoint", 0,
        [&mgr](Marshaler& m) { mgr.impl_WindowFromPoint(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "ChildWindowFromPointEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_ChildWindowFromPointEx(m); },
        16});

    // --- Timer/Message APIs ---
    exports.entries.push_back(ExportEntry{
        "GetDoubleClickTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDoubleClickTime(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "GetMessageTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMessageTime(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "NotifyWinEvent", 0,
        [&mgr](Marshaler& m) { mgr.impl_NotifyWinEvent(m); },
        16});

    // --- Rect Utility APIs ---
    exports.entries.push_back(ExportEntry{
        "GetUpdateRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetUpdateRect(m); },
        12});
    // int GetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase)
    // Report SIMPLEREGION when PaintingManager has a pending update, else
    // NULLREGION. Full HRGN geometry is still not modelled.
    exports.entries.push_back(ExportEntry{
        "GetUpdateRgn", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetUpdateRgn(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "SetRectEmpty", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_SetRectEmpty(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "AdjustWindowRectEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_AdjustWindowRectEx(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "EqualRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_EqualRect(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SubtractRect", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_SubtractRect(m); },
        12});

    // --- Class APIs ---
    exports.entries.push_back(ExportEntry{
        "GetClassLongW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassLongW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetClassLongPtrW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassLongPtrW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetClassNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassNameW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "IsChild", 0,
        [&mgr](Marshaler& m) {
            // Measured (ischild_invalid): an invalid parent is
            // ERROR_INVALID_WINDOW_HANDLE.
            const auto parent = static_cast<windowing::HWND>(m.arg(0));
            if (!mgr.window_exists(parent)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            mgr.impl_IsChild(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetDlgCtrlID", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDlgCtrlID(m); },
        4});

    // --- Dialog APIs ---
    exports.entries.push_back(ExportEntry{
        "CreateDialogIndirectParamW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateDialogIndirectParamW(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "GetNextDlgTabItem", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetNextDlgTabItem(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetNextDlgGroupItem", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetNextDlgGroupItem(m); },
        12});

    // --- Hook APIs ---
    exports.entries.push_back(ExportEntry{
        "SetWindowsHookExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetWindowsHookExW(m); },
        16});
    // SetWindowsHookExA — same HOOKPROC calling convention as W for
    // filter/CBT/etc. hooks (string-typed hooks are rare for games).
    exports.entries.push_back(ExportEntry{
        "SetWindowsHookExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetWindowsHookExW(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "CallMsgFilterW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallMsgFilterW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "CallMsgFilterA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallMsgFilterW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "CallNextHookEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallNextHookEx(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "UnhookWindowsHookEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnhookWindowsHookEx(m); },
        4});

    // --- Drawing APIs ---
    exports.entries.push_back(ExportEntry{
        "GetSysColorBrush", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSysColorBrush(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "DrawStateW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DrawStateW(m); },
        40});
    exports.entries.push_back(ExportEntry{
        "DrawStateA", 0,
        [&mgr](Marshaler& m) { mgr.impl_DrawStateA(m); },
        40});
    exports.entries.push_back(ExportEntry{
        "DrawTextExA", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_DrawTextExA(m); },
        24});
    exports.entries.push_back(ExportEntry{
        "DrawTextExW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_DrawTextExW(m); },
        24});
    exports.entries.push_back(ExportEntry{
        "TabbedTextOutA", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_TabbedTextOutA(m); },
        32});
    exports.entries.push_back(ExportEntry{
        "TabbedTextOutW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_TabbedTextOutW(m); },
        32});
    exports.entries.push_back(ExportEntry{
        "GrayStringA", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_GrayStringA(m); },
        36});
    exports.entries.push_back(ExportEntry{
        "GrayStringW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_GrayStringW(m); },
        36});
    exports.entries.push_back(ExportEntry{
        "DrawEdge", 0,
        [&mgr](Marshaler& m) { mgr.impl_DrawEdge(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "DrawFrameControl", 0,
        [&mgr](Marshaler& m) { mgr.impl_DrawFrameControl(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "GetWindowDC", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowDC(m); },
        4});

    // --- MDI APIs ---
    exports.entries.push_back(ExportEntry{
        "DefFrameProcW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DefFrameProcW(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "DefMDIChildProcW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DefMDIChildProcW(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "TranslateMDISysAccel", 0,
        [&mgr](Marshaler& m) { mgr.impl_TranslateMDISysAccel(m); },
        8});

    // --- Character APIs ---
    // CharUpperBuffW: create_resource_thunks (USER's case table).
    exports.entries.push_back(ExportEntry{
        "IsCharLowerW", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsCharLowerW(m); },
        4});

    // --- System Parameters ---
    exports.entries.push_back(ExportEntry{
        "SystemParametersInfoA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SystemParametersInfoA(m); },
        16});
}

} // namespace vitreo::winapi
