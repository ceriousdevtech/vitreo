// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_dpi_displayconfig.cpp
/// @brief user32 extended export table, slice "dpi_displayconfig".
///
/// DPI awareness, the Ptr getters/setters, display config and VkKeyScan entries.
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

void add_user32_ext_exports_dpi_displayconfig(DllExports& exports, User32ExtManager& mgr) {
    // ------------------------------------------------------------------------
    // DPI awareness (Win 8.1 / 10+). Every window is the synthetic default
    // 96 DPI with PER_MONITOR_AWARE_V2 context.
    // ------------------------------------------------------------------------

    // DPI_AWARENESS_CONTEXT pseudo-handles (Win10 SDK constants):
    //   -1 UNAWARE, -2 SYSTEM_AWARE, -3 PER_MONITOR_AWARE,
    //   -4 PER_MONITOR_AWARE_V2, -5 UNAWARE_GDISCALED
    exports.entries.push_back(ExportEntry{
        "SetProcessDpiAwarenessContext", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetProcessDpiAwarenessContext(m); },
        4});
    // void DisableProcessWindowsGhosting(void)
    //
    // Turns off the "(Not Responding)" ghost window the desktop window manager
    // substitutes for an unresponsive top-level window. Vitreo never creates a
    // ghost window, so the process is already in the state this requests. It
    // returns void: leaving the generic stub in place would still be wrong,
    // because that stub also pops the wrong number of stdcall bytes.
    exports.entries.push_back(ExportEntry{
        "DisableProcessWindowsGhosting", 0,
        [](Marshaler& m) { (void)m; },
        0});
    exports.entries.push_back(ExportEntry{
        "SetProcessDPIAware", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetProcessDPIAware(m); },
        0});
    // BOOL IsProcessDPIAware(void) — TRUE once SetProcessDPIAware /
    // SetProcessDpiAwarenessContext made the process system-or-per-monitor aware.
    exports.entries.push_back(ExportEntry{
        "IsProcessDPIAware", 0,
        [&mgr](Marshaler& m) {
            const i64 ctx = mgr.process_dpi_awareness_context();
            // UNAWARE (-1) and UNAWARE_GDISCALED (-5) → FALSE; others TRUE.
            m.set_return((ctx == -1 || ctx == -5) ? 0u : 1u);
        },
        0});
    exports.entries.push_back(ExportEntry{
        "GetThreadDpiAwarenessContext", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.thread_dpi_awareness_context()));
        },
        0});
    exports.entries.push_back(ExportEntry{
        "SetThreadDpiAwarenessContext", 0,
        [&mgr](Marshaler& m) {
            const i64 context = static_cast<i32>(m.arg(0));
            m.set_return(static_cast<u64>(
                mgr.set_thread_dpi_awareness_context(context)));
        },
        4});
    exports.entries.push_back(ExportEntry{
        "GetWindowDpiAwarenessContext", 0,
        [&mgr](Marshaler& m) {
            const auto* window = mgr.hwnd_mgr().find(m.arg(0));
            m.set_return(window == nullptr
                ? 0u
                : static_cast<u64>(mgr.process_dpi_awareness_context()));
        },
        4});
    exports.entries.push_back(ExportEntry{
        "GetAwarenessFromDpiAwarenessContext", 0,
        [](Marshaler& m) {
            // DPI_AWARENESS: -1 INVALID, 0 UNAWARE, 1 SYSTEM_AWARE,
            // 2 PER_MONITOR_AWARE. V2 has the same awareness as PMv1.
            const i32 context = static_cast<i32>(m.arg(0));
            switch (context) {
            case -1:
            case -5: m.set_return(0u); break;
            case -2: m.set_return(1u); break;
            case -3:
            case -4: m.set_return(2u); break;
            default: m.set_return(static_cast<u32>(-1)); break;
            }
        },
        4});
    exports.entries.push_back(ExportEntry{
        "IsValidDpiAwarenessContext", 0,
        [](Marshaler& m) {
            const i32 context = static_cast<i32>(m.arg(0));
            m.set_return(context >= -5 && context <= -1 ? 1u : 0u);
        },
        4});
    exports.entries.push_back(ExportEntry{
        "AreDpiAwarenessContextsEqual", 0,
        [](Marshaler& m) {
            u32 a = static_cast<u32>(m.arg(0));
            u32 b = static_cast<u32>(m.arg(1));
            m.set_return(a == b ? 1u : 0u);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "GetDpiForWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDpiForWindow(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetDpiForSystem", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.system_dpi()); },
        0});
    exports.entries.push_back(ExportEntry{
        "GetSystemDpiForProcess", 0,
        [&mgr](Marshaler& m) { (void)m.arg(0); m.set_return(mgr.system_dpi()); },
        4});
    exports.entries.push_back(ExportEntry{
        "GetSystemMetricsForDpi", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemMetricsForDpi(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SystemParametersInfoForDpi", 0,
        [&mgr](Marshaler& m) { mgr.impl_SystemParametersInfoForDpi(m); },
        20});
    exports.entries.push_back(ExportEntry{
        "AdjustWindowRectExForDpi", 0,
        [](Marshaler& m) {
            // BOOL AdjustWindowRectExForDpi(LPRECT, DWORD, BOOL, DWORD, UINT dpi)
            // Measured (s1win_api_probe awrexfordpi_*): dpi 0 is FALSE /
            // ERROR_INVALID_PARAMETER with the rectangle untouched; the
            // sizing frame, caption and menu scale with the DPI.
            const GuestAddr rp = m.arg_ptr(0);
            const u32 style = static_cast<u32>(m.arg(1));
            const bool has_menu = m.arg32(2) != 0;
            const u32 ex_style = static_cast<u32>(m.arg(3));
            const u32 dpi = m.arg32(4);
            if (dpi == 0 || rp == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            i32 l = 0, t = 0, r = 0, b = 0;
            nc_metrics::insets_for_dpi(style, ex_style, has_menu, dpi, l, t, r, b);
            const i32 left = static_cast<i32>(m.memory().read_u32(rp).value_or(0)) - l;
            const i32 top = static_cast<i32>(m.memory().read_u32(rp + 4).value_or(0)) - t;
            const i32 right = static_cast<i32>(m.memory().read_u32(rp + 8).value_or(0)) + r;
            const i32 bottom = static_cast<i32>(m.memory().read_u32(rp + 12).value_or(0)) + b;
            (void)m.memory().write_u32(rp, static_cast<u32>(left));
            (void)m.memory().write_u32(rp + 4, static_cast<u32>(top));
            (void)m.memory().write_u32(rp + 8, static_cast<u32>(right));
            (void)m.memory().write_u32(rp + 12, static_cast<u32>(bottom));
            m.set_return(1);
        },
        20});
    exports.entries.push_back(ExportEntry{
        "EnableNonClientDpiScaling", 0,
        [](Marshaler& m) { (void)m.arg(0); m.set_return(1); },
        4});

    // UINT_PTR SetCoalescableTimer(HWND, UINT_PTR, UINT, TIMERPROC, ULONG tolerance)
    // SetTimer plus a coalescing allowance. Firing on time is always within
    // the allowance; only the documented range is validated (Learn:
    // TIMERV_DEFAULT_COALESCING 0, TIMERV_NO_COALESCING 0xFFFFFFFF, else at
    // most TIMERV_COALESCING_MAX; s2msg_queue_probe coalescable_* measured
    // 0 / ERROR_INVALID_PARAMETER beyond it).
    exports.entries.push_back(ExportEntry{
        "SetCoalescableTimer", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 TIMERV_NO_COALESCING = 0xFFFFFFFFu;
            constexpr u32 TIMERV_COALESCING_MAX = 0x7FFFFFF5u;
            const u32 tolerance = m.arg32(4);
            if (tolerance != TIMERV_NO_COALESCING &&
                tolerance > TIMERV_COALESCING_MAX) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            mgr.impl_SetTimer(m);
        },
        20});

    // -- ANSI forms of the Ptr* getters/setters --
    // SetWindowLongPtrA(GWLP_WNDPROC) makes the window an ANSI window.
    exports.entries.push_back(ExportEntry{
        "GetWindowLongPtrA", 0,
        [&mgr](Marshaler& m) { mgr.window_long_get(m, true, true); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetWindowLongPtrA", 0,
        [&mgr](Marshaler& m) { mgr.window_long_set(m, true, true); },
        12});
    exports.entries.push_back(ExportEntry{
        "GetClassLongPtrA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetClassLongPtrW(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetClassLongPtrA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetClassLongPtrW(m); },
        12});

    // CCD display-config APIs — backed by host monitor data so
    // EnumDisplayMonitors and QueryDisplayConfig stay coherent (Learn).
    exports.entries.push_back(ExportEntry{
        "GetDisplayConfigBufferSizes", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDisplayConfigBufferSizes(m); },
        12});
    exports.entries.push_back(ExportEntry{
        "QueryDisplayConfig", 0,
        [&mgr](Marshaler& m) { mgr.impl_QueryDisplayConfig(m); },
        24});
    exports.entries.push_back(ExportEntry{
        "DisplayConfigGetDeviceInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_DisplayConfigGetDeviceInfo(m); },
        4});
    exports.entries.push_back(ExportEntry{
        "DisplayConfigSetDeviceInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_DisplayConfigSetDeviceInfo(m); },
        4});

    // CreateDialogIndirectParamA — same dialog path as the W export.
    // Template strings are Unicode (Microsoft Learn). A NULL stub makes
    // creation fail after the template has already been loaded.
    exports.entries.push_back(ExportEntry{
        "CreateDialogIndirectParamA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateDialogIndirectParamA(m); },
        20});

    // UINT GetRawInputData(HRAWINPUT, UINT, LPVOID, PUINT, UINT)
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdata
    exports.entries.push_back(ExportEntry{
        "GetRawInputData", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetRawInputData(m); },
        20});

    // SHORT VkKeyScanExA(CHAR ch, HKL dwhkl)
    // SHORT VkKeyScan[Ex]{A,W}(ch[, hkl]): low byte VK, high byte shift
    // state, -1 when no key types the character -- the measured US layout
    // (kbd_layout.hpp; s2msg_input_probe vkkeyscanex*). The A forms first
    // widen the byte through the ANSI code page.
    const auto vk_scan_ansi = [](Marshaler& m) {
        const u8 byte = static_cast<u8>(m.arg(0));
        u16 wide = 0;
        const auto decoded = codepage::multi_byte_to_wide(
            codepage::ansi_code_page(), 0, &byte, 1, &wide, 1);
        const u16 result = decoded.ok() && decoded.count == 1
            ? kbd_us::vk_key_scan(wide) : 0xFFFFu;
        m.set_return(static_cast<u64>(static_cast<u32>(static_cast<i32>(static_cast<i16>(result)))));
    };
    const auto vk_scan_wide = [](Marshaler& m) {
        const u16 result = kbd_us::vk_key_scan(static_cast<u16>(m.arg(0)));
        m.set_return(static_cast<u64>(static_cast<u32>(static_cast<i32>(static_cast<i16>(result)))));
    };
    exports.entries.push_back(ExportEntry{"VkKeyScanExA", 0, vk_scan_ansi, 8});
    exports.entries.push_back(ExportEntry{"VkKeyScanExW", 0, vk_scan_wide, 8});
    exports.entries.push_back(ExportEntry{"VkKeyScanA", 0, vk_scan_ansi, 4});
    exports.entries.push_back(ExportEntry{"VkKeyScanW", 0, vk_scan_wide, 4});

    // void keybd_event(BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo)
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-keybd_event
    exports.entries.push_back(ExportEntry{
        "keybd_event", 0,
        [&mgr](Marshaler& m) { mgr.impl_keybd_event(m); },
        16});
}

} // namespace vitreo::winapi
