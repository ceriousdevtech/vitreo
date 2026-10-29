// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_window_objects.cpp
/// @brief user32 extended export table, slice "window_objects".
///
/// Window and class longs, window stations, desktops, monitors and the classic-game stubs.
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

void add_user32_ext_exports_window_objects(DllExports& exports, User32ExtManager& mgr) {
    exports.entries.push_back(ExportEntry{
        "MapDialogRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_MapDialogRect(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "MapWindowPoints", 0,
        [&mgr](Marshaler& m) { mgr.impl_MapWindowPoints(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "MessageBoxIndirectW", 0,
        [&mgr](Marshaler& m) { mgr.impl_MessageBoxIndirectW(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "OpenClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenClipboard(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "RemovePropA", 0,
        [&mgr](Marshaler& m) { mgr.impl_RemovePropA(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "RemovePropW", 0,
        [&mgr](Marshaler& m) { mgr.impl_RemovePropW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "ScreenToClient", 0,
        [&mgr](Marshaler& m) { mgr.impl_ScreenToClient(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "DrawFocusRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_DrawFocusRect(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "ReleaseDC", 0,
        // A NULL window releases a screen DC ("ReleaseDC(NULL, screen)=1").
        [&mgr](Marshaler& m) { mgr.impl_ReleaseDC(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "SetClassLongW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetClassLongW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "SetClassLongPtrW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetClassLongPtrW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "SystemParametersInfoW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SystemParametersInfoW(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "SetClipboardData", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetClipboardData(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "SetClipboardViewer", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetClipboardViewer(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "SetCursor", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetCursor(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "SetPropA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetPropA(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "SetPropW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetPropW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "SetTimer", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetTimer(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "KillTimer", 0,
        [&mgr](Marshaler& m) { mgr.impl_KillTimer(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "SetWindowLongW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetWindowLongW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "SetWindowLongPtrW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetWindowLongPtrW(m); },
        12});

    // The A forms differ for GWLP_WNDPROC: installing a procedure through
    // them makes the window an ANSI window (IsWindowUnicode).
    exports.entries.push_back(ExportEntry{
        "SetWindowLongA", 0,
        [&mgr](Marshaler& m) { mgr.window_long_set(m, false, true); },
        12});

    exports.entries.push_back(ExportEntry{
        "GetWindowLongA", 0,
        [&mgr](Marshaler& m) { mgr.window_long_get(m, false, true); },
        8});

    exports.entries.push_back(ExportEntry{
        "CallWindowProcA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallWindowProcA(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "SetWindowPos", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_SetWindowPos(m); },
        28});

    exports.entries.push_back(ExportEntry{
        "MoveWindow", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_MoveWindow(m); },
        24});

    exports.entries.push_back(ExportEntry{
        "GetParent", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetParent(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "TrackPopupMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_TrackPopupMenu(m); },
        28});

    // BOOL TrackPopupMenuEx(HMENU, UINT uFlags, int x, int y, HWND,
    //                       LPTPMPARAMS lptpm) — 6 args, not TrackPopupMenu's
    // 7. Registering it with the stub's stdcall_bytes=0 left the caller's
    // stack 24 bytes deep. Popup tracking itself is unimplemented; both
    // entry points report the same honest FALSE.
    exports.entries.push_back(ExportEntry{
        "TrackPopupMenuEx", 0,
        [&mgr](Marshaler& m) { mgr.track_popup_menu(m, true); },
        24});

    exports.entries.push_back(ExportEntry{
        "RealGetWindowClassW", 0,
        [&mgr](Marshaler& m) { mgr.impl_RealGetWindowClassW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "RealGetWindowClassA", 0,
        [&mgr](Marshaler& m) { mgr.impl_RealGetWindowClassA(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "IsHungAppWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsHungAppWindow(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "RegisterTouchWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterTouchWindow(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "UnregisterTouchWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnregisterTouchWindow(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "IsTouchWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsTouchWindow(m); },
        8});
    // BOOL GetTouchInputInfo(HTOUCHINPUT, UINT, PTOUCHINPUT, int)
    exports.entries.push_back(ExportEntry{
        "GetTouchInputInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetTouchInputInfo(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "wsprintfA", 0,
        [&mgr](Marshaler& m) { mgr.impl_wsprintfA(m); },
        0}); // cdecl

    exports.entries.push_back(ExportEntry{
        "wsprintfW", 0,
        [&mgr](Marshaler& m) { mgr.impl_wsprintfW(m); },
        0}); // cdecl

    // wvsprintfA/W(lpOut, lpFmt, va_list) — stdcall, 3 args (12 bytes)
    exports.entries.push_back(ExportEntry{
        "wvsprintfA", 0,
        [&mgr](Marshaler& m) { mgr.impl_wvsprintfA(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "wvsprintfW", 0,
        [&mgr](Marshaler& m) { mgr.impl_wvsprintfW(m); },
        12});

    // WaitForInputIdle(hProcess, dwMilliseconds) — Learn: 0 when idle,
    // WAIT_TIMEOUT, or WAIT_FAILED (user32_windows_probe wfii_*).
    exports.entries.push_back(ExportEntry{
        "WaitForInputIdle", 0,
        [&mgr](Marshaler& m) { mgr.impl_WaitForInputIdle(m); },
        8});

    // SetClassLongA/W(hWnd, nIndex, dwNewLong); the W entry is registered
    // above. The class record is real (WindowClassManager): see
    // User32ExtManager::class_long_set.
    exports.entries.push_back(ExportEntry{
        "SetClassLongA", 0,
        [&mgr](Marshaler& m) { mgr.class_long_set(m, 4); },
        12});

    exports.entries.push_back(ExportEntry{
        "AllowSetForegroundWindow", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_AllowSetForegroundWindow(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "EnumChildWindows", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumChildWindows(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "GetMonitorInfoW", 0,
        [&mgr](Marshaler& m) {
        // GetMonitorInfo(NULL): measured ERROR_INVALID_MONITOR_HANDLE (1461).
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_MONITOR_HANDLE); m.set_return(0); return; }
        mgr.impl_GetMonitorInfoW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "GetProcessWindowStation", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetProcessWindowStation(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "CreateWindowStationW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateWindowStationW(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "OpenWindowStationW", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenWindowStationW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "SetProcessWindowStation", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetProcessWindowStation(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "CloseWindowStation", 0,
        [&mgr](Marshaler& m) { mgr.impl_CloseWindowStation(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "GetThreadDesktop", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetThreadDesktop(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "CreateDesktopW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateDesktopW(m); },
        24});

    exports.entries.push_back(ExportEntry{
        "SetThreadDesktop", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadDesktop(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "SwitchDesktop", 0,
        [&mgr](Marshaler& m) { mgr.impl_SwitchDesktop(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "GetUserObjectSecurity", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetUserObjectSecurity(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "SetUserObjectSecurity", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetUserObjectSecurity(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "GetUserObjectInformationW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetUserObjectInformationW(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "GetUserObjectInformationA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetUserObjectInformationA(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "MonitorFromPoint", 0,
        [&mgr](Marshaler& m) { mgr.impl_MonitorFromPoint(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "MonitorFromRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_MonitorFromRect(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "MonitorFromWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_MonitorFromWindow(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "EnumDisplayMonitors", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplayMonitors(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "EnumDisplayDevicesW", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplayDevicesW(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "GetMonitorInfoA", 0,
        [&mgr](Marshaler& m) {
        // GetMonitorInfo(NULL): measured ERROR_INVALID_MONITOR_HANDLE (1461).
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_MONITOR_HANDLE); m.set_return(0); return; }
        mgr.impl_GetMonitorInfoA(m); },
        8});

    // PostThreadMessageW — now handled by MessageSendManager (real queue posting)

    exports.entries.push_back(ExportEntry{
        "RedrawWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_RedrawWindow(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "SetDlgItemInt", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetDlgItemInt(m); },
        16});

    // ----- Minesweeper / classic game stubs -----
    exports.entries.push_back(ExportEntry{
        "CheckMenuItem", 0,
        [&mgr](Marshaler& m) { mgr.impl_CheckMenuItem(m); },
        12});

    // Learn: CheckMenuRadioItem(hmenu, first, last, check, flags) — 5 args.
    exports.entries.push_back(ExportEntry{
        "CheckMenuRadioItem", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
                m.set_return(0u);
                return;
            }
            mgr.impl_CheckMenuRadioItem(m);
        },
        20});

    exports.entries.push_back(ExportEntry{
        "SetMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetMenu(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "LoadMenuW", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND); m.set_return(0); return; }
            mgr.impl_LoadMenuW(m);
            },
        8});

    // LoadMenuA — identical to LoadMenuW for the MAKEINTRESOURCE form, which is
    // the only one the RT_MENU lookup resolves. Without this entry the import
    // resolves to the missing-export stub, whose 0xFFFFFFFF return reaches
    // CreateWindowEx as the window's HMENU.
    exports.entries.push_back(ExportEntry{
        "LoadMenuA", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadMenuA(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "ReleaseCapture", 0,
        [&mgr](Marshaler& m) { mgr.impl_ReleaseCapture(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "SetCapture", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetCapture(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "WinHelpW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(0); m.set_return(0); return; }
        mgr.impl_WinHelpW(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "GetMenuItemRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuItemRect(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "TranslateAcceleratorA", 0,
        [&mgr](Marshaler& m) { mgr.impl_TranslateAcceleratorW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "TranslateAcceleratorW", 0,
        [&mgr](Marshaler& m) { mgr.impl_TranslateAcceleratorW(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "LoadAcceleratorsA", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadAcceleratorsA(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "LoadAcceleratorsW", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadAcceleratorsW(m); },
        8});
}

} // namespace vitreo::winapi
