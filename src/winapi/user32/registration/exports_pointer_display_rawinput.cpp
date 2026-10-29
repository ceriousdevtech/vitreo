// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_pointer_display_rawinput.cpp
/// @brief user32 extended export table, slice "pointer_display_rawinput".
///
/// Pointer info, display settings, WinEvent hooks, raw input and character-translation entries.
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

void add_user32_ext_exports_pointer_display_rawinput(DllExports& exports, User32ExtManager& mgr) {
    // ===== Additional Win32 stubs =====
    exports.entries.push_back(ExportEntry{
        "TrackMouseEvent", 0,
        [&mgr](Marshaler& m) { mgr.impl_TrackMouseEvent(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetPointerInfo", 0,
        [](Marshaler& m) {
            // The mouse is pointer id 1; its POINTER_INFO is the one recorded
            // for the calling thread's most recently retrieved pointer
            // message. Before any, and for any other id, ERROR_INVALID_PARAMETER
            // (measured).
            const u32 pointer_id = static_cast<u32>(m.arg(0));
            const GuestAddr out = m.arg_ptr(1);
            mouse_in_pointer::PointerInfo info{};
            const u32 error = mouse_in_pointer::pointer_info(
                ThreadManager::get_current_thread_id_static(), pointer_id, info);
            if (error != 0) {
                set_last_error(error);
                m.set_return(0u);
                return;
            }
            if (out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            const auto image =
                mouse_in_pointer::pointer_info_image(info, m.cpu_is_64bit());
            std::array<u8, 88> image32{};
            std::memcpy(image32.data(), image.bytes.data(), image32.size());
            const bool written = image.size == image.bytes.size()
                ? m.write_struct(out, image.bytes).is_ok()
                : m.write_struct(out, image32).is_ok();
            if (!written) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "EnableMouseInPointer", 0,
        [](Marshaler& m) {
            const auto result = mouse_in_pointer::enable(m.arg(0));
            if (!result.ok) set_last_error(result.error);
            m.set_return(result.ok ? 1u : 0u);
        },
        4});
    exports.entries.push_back(ExportEntry{
        "IsMouseInPointerEnabled", 0,
        [](Marshaler& m) {
            m.set_return(mouse_in_pointer::enabled() ? 1u : 0u);
        },
        0});
    exports.entries.push_back(ExportEntry{
        "AttachThreadInput", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_AttachThreadInput(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "ClipCursor", 0,
        // NULL frees the cursor ("ClipCursor(NULL)=1").
        [&mgr](Marshaler& m) { mgr.impl_ClipCursor(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetClipCursor", 0,
        // NULL: FALSE, error untouched ("GetClipCursor(NULL)=0 err=57005").
        [&mgr](Marshaler& m) { mgr.impl_GetClipCursor(m); },
        4});
    // SendNotifyMessageA/W: make_message_send_exports (message_send.cpp).
    exports.entries.push_back(ExportEntry{
        "SetMenuItemInfoW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_SetMenuItemInfoW(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "SetMenuItemInfoA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetMenuItemInfoA(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "GetMenuStringA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuStringA(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "RegisterDeviceNotificationW", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterDeviceNotificationW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "UnregisterDeviceNotification", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnregisterDeviceNotification(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "RegisterPowerSettingNotification", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_RegisterPowerSettingNotification(m);
        },
        12});
    exports.entries.push_back(ExportEntry{
        "UnregisterPowerSettingNotification", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_UnregisterPowerSettingNotification(m);
        },
        4});
    exports.entries.push_back(ExportEntry{
        "RegisterSuspendResumeNotification", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterSuspendResumeNotification(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "UnregisterSuspendResumeNotification", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnregisterSuspendResumeNotification(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "SetWinEventHook", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetWinEventHook(m); },
        28});
    exports.entries.push_back(ExportEntry{
        "UnhookWinEvent", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnhookWinEvent(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "FlashWindowEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_FlashWindowEx(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "OpenInputDesktop", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenInputDesktop(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "OpenDesktopA", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenDesktopA(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "OpenDesktopW", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenDesktopW(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "CloseDesktop", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_CloseDesktop(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "EnumDesktopWindows", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDesktopWindows(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetShellWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetShellWindow(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "EnumDisplaySettingsA", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplaySettingsA(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "EnumDisplaySettingsW", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplaySettingsW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "EnumDisplaySettingsExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplaySettingsExA(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "EnumDisplaySettingsExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplaySettingsExW(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "EnumDisplayDevicesA", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumDisplayDevicesA(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "ChangeDisplaySettingsExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_ChangeDisplaySettingsExA(m); },
        20});
    exports.entries.push_back(ExportEntry{
        // Learn (ChangeDisplaySettingsW): the two-argument form is
        // ChangeDisplaySettingsExW with no device name and no reserved
        // parameter. Vitreo never reprograms the display, so both report
        // DISP_CHANGE_SUCCESSFUL; the parameter shapes differ but neither
        // implementation reads them.
        "ChangeDisplaySettingsW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ChangeDisplaySettingsW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "ChangeDisplaySettingsExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ChangeDisplaySettingsExW(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "CreateIconFromResource", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateIconFromResource(m); },
        16});
    exports.entries.push_back(ExportEntry{
        "CreateIconFromResourceEx", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_CreateIconFromResourceEx(m); },
        28});
    exports.entries.push_back(ExportEntry{
        "CreateIconIndirect", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateIconIndirect(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "DialogBoxIndirectParamW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DialogBoxIndirectParamW(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "RegisterRawInputDevices", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_RegisterRawInputDevices(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "RegisterPointerDeviceNotifications", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterPointerDeviceNotifications(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetPointerDevices", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPointerDevices(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetRawInputDeviceList", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetRawInputDeviceList(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetRawInputBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetRawInputBuffer(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetRawInputDeviceInfoA", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(3) == 0) { set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0xFFFFFFFFu); return; }
            mgr.impl_GetRawInputDeviceInfoA(m);
        },
        16});
    // UINT GetRegisteredRawInputDevices(PRAWINPUTDEVICE, PUINT, UINT)
    exports.entries.push_back(ExportEntry{
        "GetRegisteredRawInputDevices", 0,
        [&mgr](Marshaler& m) {
            (void)mgr;
            const GuestAddr num_ptr = m.arg_ptr(1);
            if (num_ptr == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            // No persisted multi-device table yet: report zero registrations
            // (matches an empty RegisterRawInputDevices history).
            (void)m.write_u32(num_ptr, 0);
            m.set_return(0);
        },
        12});
    // BOOL LogicalToPhysicalPoint(HWND, LPPOINT) / PhysicalToLogicalPoint:
    // identity under Vitreo DPI (no process is scaled). Measured
    // (physical_cursor_probe, x86 = x64): a NULL or unknown window is FALSE /
    // ERROR_INVALID_WINDOW_HANDLE with the point untouched; a window succeeds
    // with the last error untouched.
    const auto point_conversion = [&mgr](Marshaler& m) {
        const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
        const GuestAddr pt = m.arg_ptr(1);
        if (hwnd == 0 ||
            (hwnd != windowing::HWND_DESKTOP && !mgr.window_exists(hwnd))) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0);
            return;
        }
        if (pt == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    };
    exports.entries.push_back(ExportEntry{"LogicalToPhysicalPoint", 0, point_conversion, 8});
    exports.entries.push_back(ExportEntry{"PhysicalToLogicalPoint", 0, point_conversion, 8});
    // BOOL SetWindowCompositionAttribute(HWND, WINDOWCOMPOSITIONATTRIBDATA*)
    exports.entries.push_back(ExportEntry{
        "SetWindowCompositionAttribute", 0,
        [](Marshaler& m) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED user32!SetWindowCompositionAttribute\n");
            }
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            m.set_return(0);
        },
        8});
    // Pointer device APIs (Win8+): export present for GetProcAddress; body refuses.
    exports.entries.push_back(ExportEntry{
        "GetPointerDeviceRects", 0,
        [](Marshaler& m) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED user32!GetPointerDeviceRects\n");
            }
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        },
        16});
    exports.entries.push_back(ExportEntry{
        "GetPointerPenInfoHistory", 0,
        [](Marshaler& m) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED user32!GetPointerPenInfoHistory\n");
            }
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        },
        16});
    exports.entries.push_back(ExportEntry{
        "GetPointerTouchInfo", 0,
        [](Marshaler& m) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED user32!GetPointerTouchInfo\n");
            }
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "GetPointerTouchInfoHistory", 0,
        [](Marshaler& m) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED user32!GetPointerTouchInfoHistory\n");
            }
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        },
        16});
    exports.entries.push_back(ExportEntry{
        "GetMessageExtraInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMessageExtraInfo(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "SetMessageExtraInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetMessageExtraInfo(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetQueueStatus", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetQueueStatus(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "EnumClipboardFormats", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumClipboardFormats(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetUpdatedClipboardFormats", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetUpdatedClipboardFormats(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "SetGestureConfig", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetGestureConfig(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "GetGestureConfig", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetGestureConfig(m); },
        24});
    exports.entries.push_back(ExportEntry{
        "GetClipboardData", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClipboardData(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetClipboardFormatNameA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClipboardFormatNameA(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetClipboardFormatNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClipboardFormatNameW(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetClipboardSequenceNumber", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClipboardSequenceNumber(m); },
        0});
    exports.entries.push_back(ExportEntry{
        "SendInput", 0,
        [&mgr](Marshaler& m) { mgr.impl_SendInput(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetCursorInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCursorInfo(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "IsWindowUnicode", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_IsWindowUnicode(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetWindowInfo", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_GetWindowInfo(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetClassLongA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassLongA(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "GetClassNameA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassNameA(m); },
        12});
    // Undecorated GetProcAddress("GetClassName") alias of GetClassNameA.
    exports.entries.push_back(ExportEntry{
        "GetClassName", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassNameA(m); },
        12});
    // WINAPI / stdcall: six 32-bit args (Microsoft Learn winuser ToUnicode).
    // A 20-byte cleanup leaves 4 bytes on the stack; the next `[esp+disp]`
    // then reads the scan code (e.g. 0x26) as a BOOL* and AVs.
    exports.entries.push_back(ExportEntry{
        "ToUnicode", 0,
        [&mgr](Marshaler& m) { mgr.impl_ToUnicode(m); },
        24});
}

} // namespace vitreo::winapi
