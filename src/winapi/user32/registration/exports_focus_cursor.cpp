// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_focus_cursor.cpp
/// @brief user32 extended export table, slice "focus_cursor".
///
/// Focus, cursor, window placement and message-filter stubs.
///
/// One contiguous run of the former create_user32_ext_thunks body, moved
/// verbatim; user32_ext_registration.cpp calls the slices in registration
/// order.

#include "winapi/user32/registration/registration_parts.hpp"
#include "windowing/model/diag_flags.hpp"

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

void add_user32_ext_exports_focus_cursor(DllExports& exports, User32ExtManager& mgr) {
    // =========================================================================
    // Solitaire stubs — simple / minimal implementations
    // =========================================================================

    // SetFocus sets the calling input queue's keyboard focus, sends the
    // synchronous focus transition, and returns the previously focused HWND.
    exports.entries.push_back(ExportEntry{
        "SetFocus", 0,
        [&mgr](Marshaler& m) {
            // s2msg_input_probe: an invalid window fails with
            // ERROR_INVALID_WINDOW_HANDLE; a window of another (unattached)
            // thread with ERROR_ACCESS_DENIED, focus unchanged; success sets
            // the last error to 0 and returns the previous focus of the
            // caller's queue.
            const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            const auto focus = mgr.m_focus_hwnd;
            const auto prev = mgr.window_in_caller_queue(focus) ? focus : 0;
            if (std::getenv("VITREO_FOCUS_DIAG")) {
                std::fprintf(stderr, "[focus] guest SetFocus(0x%x) prev=0x%x\n",
                             hwnd, prev);
            }
            if (hwnd == windowing::NULL_HWND) {
                if (prev != windowing::NULL_HWND) {
                    (void)mgr.set_keyboard_focus(prev, false);
                }
                set_last_error(0);
                m.set_return(static_cast<u32>(prev));
                return;
            }
            const windowing::WindowData* window = mgr.hwnd_mgr().find(hwnd);
            if (window == nullptr) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            if (!mgr.window_in_caller_queue(hwnd)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                m.set_return(0u);
                return;
            }
            if (!window->enabled) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            (void)mgr.set_keyboard_focus(hwnd, true);
            set_last_error(0);
            m.set_return(static_cast<u32>(prev));
        },
        4});

    // CopyRect(LPRECT dst, const RECT* src) → copy 16 bytes
    exports.entries.push_back(ExportEntry{
        "CopyRect", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            if (dst != 0 && src != 0) {
                u8 buf[16];
                if (mgr.memory().read_bytes(src, buf, 16)) {
                    (void)mgr.memory().write_bytes(dst, buf, 16);
                }
            }
            m.set_return(1u);
        },
        8});

    // LoadStringA(HINSTANCE, UINT id, LPSTR buf, int cchMax) → load resource string as ANSI
    exports.entries.push_back(ExportEntry{
        "LoadStringA", 0,
        [&mgr](Marshaler& m) {
            u32 instance = static_cast<u32>(m.arg(0));
            u32 id = static_cast<u32>(m.arg(1));
            GuestAddr buf = m.arg_ptr(2);
            u32 cch_max = static_cast<u32>(m.arg(3));
            if (buf == 0 || cch_max == 0) {
                m.set_return(0u);
                return;
            }

            // String tables are stored in blocks of 16 strings.
            u16 block_id = static_cast<u16>((id / 16u) + 1u);
            u16 index_in_block = static_cast<u16>(id % 16u);

            auto entry = windowing::find_resource(
                mgr.memory(), mgr.rsrc_image_base(), mgr.rsrc_rva(), mgr.rsrc_size(),
                windowing::RT::STRING, block_id);

            if (!entry.data || entry.size < 2) {
                (void)mgr.memory().write_u8(buf, 0);
                m.set_return(0u);
                return;
            }

            // Walk length-prefixed UTF-16LE strings to the desired index.
            const u8* p = entry.data;
            const u8* end = entry.data + entry.size;
            for (u16 i = 0; i < 16; ++i) {
                if (p + 2 > end) break;
                u16 len = static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
                p += 2;
                u32 bytes = static_cast<u32>(len) * 2u;
                if (p + bytes > end) break;

                if (i == index_in_block) {
                    u32 copy_len = std::min<u32>(len, cch_max - 1);
                    for (u32 j = 0; j < copy_len; ++j) {
                        u16 ch = static_cast<u16>(p[j * 2] | (static_cast<u16>(p[j * 2 + 1]) << 8));
                        // Narrow to ANSI: keep chars <= 0xFF, replace others with '?'
                        u8 ansi = (ch <= 0xFF) ? static_cast<u8>(ch) : static_cast<u8>('?');
                        (void)mgr.memory().write_u8(buf + j, ansi);
                    }
                    (void)mgr.memory().write_u8(buf + copy_len, 0);
                    VITREO_TRACE("[user32_ext] LoadStringA(id=%u) -> %u chars\n", id, copy_len);
                    m.set_return(copy_len);
                    return;
                }
                p += bytes;
            }

            (void)mgr.memory().write_u8(buf, 0);
            m.set_return(0u);
        },
        16});

    // GetCapture() → returns HWND that has mouse capture
    exports.entries.push_back(ExportEntry{
        "GetCapture", 0,
        [&mgr](Marshaler& m) {
            // Learn GetCapture: the capture window "associated with the
            // current thread"; another queue's capture reads as NULL.
            const auto capture = mgr.hwnd_mgr().get_capture();
            m.set_return(mgr.window_in_caller_queue(capture) ? capture : 0u);
        },
        0});

    // GetMenu(HWND hWnd) → returns HMENU
    exports.entries.push_back(ExportEntry{
        "GetMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenu(m); },
        4});

    // ShowCursor(BOOL bShow) → returns display count
    exports.entries.push_back(ExportEntry{
        "ShowCursor", 0,
        [&mgr](Marshaler& m) {
            u32 bShow = static_cast<u32>(m.arg(0));
            if (bShow) {
                mgr.m_cursor_display_count++;
            } else {
                mgr.m_cursor_display_count--;
            }
            // Win32: the cursor is displayed when the internal count is >= 0.
            // Mirror that onto the native cursor so a guest that draws its own
            // software cursor does not show a doubled macOS cursor.
            mgr.set_native_cursor_visible(
                mgr.m_current_cursor != 0 && mgr.m_cursor_display_count >= 0);
            if (windowing::diag::input_diag()) {
                std::fprintf(stderr, "[input] ShowCursor show=%u count=%d\n",
                             bShow != 0 ? 1u : 0u,
                             mgr.m_cursor_display_count);
            }
            m.set_return(static_cast<u32>(mgr.m_cursor_display_count));
        },
        4});

    // IsIconic(HWND hWnd) → BOOL
    exports.entries.push_back(ExportEntry{
        "IsIconic", 0,
        [&mgr](Marshaler& m) {
            const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            const windowing::WindowData* win = mgr.hwnd_mgr().find(hwnd);
            // Measured (isiconic_invalid): ERROR_INVALID_WINDOW_HANDLE.
            if (!win && !mgr.window_exists(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            const bool iconic = win && win->minimized;
            VITREO_TRACE(
                "[user32_ext] IsIconic(hwnd=0x%x) -> %u\n",
                hwnd, iconic ? 1u : 0u);
            m.set_return(iconic ? 1u : 0u);
        },
        4});

    // AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu)
    exports.entries.push_back(ExportEntry{
        "AdjustWindowRect", 0,
        [&mgr](Marshaler& m) {
            // BOOL AdjustWindowRect(LPRECT, DWORD dwStyle, BOOL bMenu)
            // Identical to AdjustWindowRectEx with dwExStyle == 0.
            const GuestAddr rp = m.arg_ptr(0);
            const auto style = static_cast<u32>(m.arg(1));
            const bool has_menu = m.arg(2) != 0;
            if (rp == 0) { m.set_return(0u); return; }
            const bool ok =
                adjust_window_rect(mgr.memory(), rp, style, has_menu, 0);
            m.set_return(ok ? 1u : 0u);
        },
        12});

    exports.entries.push_back(ExportEntry{
        "RegisterWindowMessageW", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterWindowMessageW(m); },
        4});

    // BOOL ChangeWindowMessageFilterEx(HWND, UINT message, DWORD action,
    //                                   PCHANGEFILTERSTRUCT)
    //
    // UIPI message filtering. Vitreo runs every guest process at a single
    // integrity level, so no message is ever blocked by UIPI and the call
    // succeeds; the contract that matters is the arity, the parameter
    // validation and the CHANGEFILTERSTRUCT out-parameter.
    exports.entries.push_back(ExportEntry{
        "ChangeWindowMessageFilterEx", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 MSGFLT_RESET = 0, MSGFLT_ALLOW = 1,
                          MSGFLT_DISALLOW = 2;
            constexpr u32 MSGFLT_STATUS_ERROR = 0,
                          MSGFLT_STATUS_ALWAYS_ALLOW = 1,
                          MSGFLT_STATUS_ALWAYS_DISALLOW = 2;

            const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            const auto message = static_cast<u32>(m.arg(1));
            const auto action = static_cast<u32>(m.arg(2));
            const GuestAddr cfs = m.arg_ptr(3);

            if (hwnd == 0 || !mgr.hwnd_mgr().find(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            if (action != MSGFLT_RESET && action != MSGFLT_ALLOW &&
                action != MSGFLT_DISALLOW) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            if (cfs != 0) {
                // cbSize must be sizeof(CHANGEFILTERSTRUCT) == 8.
                const u32 cb_size = mgr.memory().read_u32(cfs).value_or(0);
                if (cb_size != 8) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0u);
                    return;
                }
                const u32 ext_status =
                    action == MSGFLT_ALLOW    ? MSGFLT_STATUS_ALWAYS_ALLOW
                    : action == MSGFLT_DISALLOW ? MSGFLT_STATUS_ALWAYS_DISALLOW
                                                : MSGFLT_STATUS_ERROR;
                (void)mgr.memory().write_u32(cfs + 4, ext_status);
            }
            VITREO_TRACE(
                "[user32_ext] ChangeWindowMessageFilterEx(hwnd=0x%x, msg=0x%x,"
                " action=%u) -> 1\n",
                hwnd, message, action);
            m.set_return(1u);
        },
        16});

    // BOOL ChangeWindowMessageFilter(UINT message, DWORD dwFlag)
    //
    // The process-wide predecessor of the Ex form; same UIPI reasoning.
    exports.entries.push_back(ExportEntry{
        "ChangeWindowMessageFilter", 0,
        [](Marshaler& m) {
            constexpr u32 MSGFLT_ADD = 1, MSGFLT_REMOVE = 2;
            const auto flag = static_cast<u32>(m.arg(1));
            if (flag != MSGFLT_ADD && flag != MSGFLT_REMOVE) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
        },
        8});

    exports.entries.push_back(ExportEntry{
        "RegisterShellHookWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterShellHookWindow(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "DeregisterShellHookWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeregisterShellHookWindow(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "GetLastInputInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetLastInputInfo(m); },
        4});

    // InvertRect(HDC hDC, const RECT* lprc) → BOOL (stub: TRUE)
    exports.entries.push_back(ExportEntry{
        "InvertRect", 0,
        [&mgr](Marshaler& m) { mgr.impl_InvertRect(m); },
        8});

    // SetCursorPos(int X, int Y) → BOOL. SetPhysicalCursorPos takes the same
    // point: no process is DPI-scaled, so physical equals logical.
    const auto set_cursor_pos = [&mgr](Marshaler& m) {
        impl_SetCursorPos(m, mgr.cursor_host(), mgr.guest_virtual_screen_rect());
    };
    exports.entries.push_back(ExportEntry{"SetCursorPos", 0, set_cursor_pos, 8});
    exports.entries.push_back(ExportEntry{"SetPhysicalCursorPos", 0, set_cursor_pos, 8});

    // ClientToScreen(HWND hWnd, LPPOINT lpPoint) → BOOL
    exports.entries.push_back(ExportEntry{
        "ClientToScreen", 0,
        [&mgr](Marshaler& m) {
            auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            GuestAddr pp = m.arg_ptr(1);
            // Measured (clienttoscreen_invalid): ERROR_INVALID_WINDOW_HANDLE.
            if (hwnd != windowing::HWND_DESKTOP && !mgr.window_exists(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            if (pp == 0) { m.set_return(0u); return; }

            // Read client POINT
            i32 px = static_cast<i32>(mgr.memory().read_u32(pp).value_or(0));
            i32 py = static_cast<i32>(mgr.memory().read_u32(pp + 4).value_or(0));

            // Convert client → screen using the client-area desktop origin
            // (outer origin + NC insets), not GetWindowRect's outer corner.
            auto origin = mgr.is_virtual_fullscreen_window(hwnd)
                ? windowing::Point{0, 0}
                : client_origin_to_desktop(mgr.hwnd_mgr(), hwnd);
            px += origin.x;
            py += origin.y;

            (void)mgr.memory().write_u32(pp,     static_cast<u32>(px));
            (void)mgr.memory().write_u32(pp + 4, static_cast<u32>(py));
            m.set_return(1u);
        },
        8});

    // GetKeyState(int nVirtKey) → SHORT — query the calling thread's
    // input-state table, which SetKeyboardState and message removal update.
    exports.entries.push_back(ExportEntry{
        "GetKeyState", 0,
        [&mgr](Marshaler& m) {
            u32 vk = static_cast<u32>(m.arg(0));
            i16 state = mgr.thread_key_state(vk);
            if (windowing::diag::input_diag()) {
                std::fprintf(stderr, "[input] GetKeyState vk=0x%02x -> 0x%04x\n",
                             vk, static_cast<u16>(state));
            }
            m.set_return(static_cast<u64>(static_cast<u16>(state)));
        },
        4});

    // CheckRadioButton(HWND hDlg, int first, int last, int check) → BOOL
    exports.entries.push_back(ExportEntry{
        "CheckRadioButton", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            auto hDlg  = static_cast<windowing::HWND>(m.arg(0));
            auto first = static_cast<u32>(m.arg(1));
            auto last  = static_cast<u32>(m.arg(2));
            auto check = static_cast<u32>(m.arg(3));

            VITREO_TRACE( "[user32_ext] CheckRadioButton(dlg=0x%x, first=%u, last=%u, check=%u)\n",
                         hDlg, first, last, check);

            auto children = mgr.hwnd_mgr().enum_children(hDlg);
            for (auto ch : children) {
                const auto* wd = mgr.hwnd_mgr().find(ch);
                if (!wd) continue;
                u32 id = wd->ctrl_id;
                if (id >= first && id <= last) {
                    bool should_check = (id == check);
                    mgr.control_host().set_button_check(ch, should_check);
                }
            }
            m.set_return(1u);
        },
        16});

    // GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl) → BOOL
    exports.entries.push_back(ExportEntry{
        "GetWindowPlacement", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 kWindowPlacementSize = 44;
            const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            const GuestAddr placement = m.arg_ptr(1);
            const windowing::WindowData* win = mgr.hwnd_mgr().find(hwnd);
            if (!win) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            if (placement == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            const auto length = m.read_u32(placement);
            if (length.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            // GetWindowPlacement does not check the caller's length; it
            // writes sizeof(WINDOWPLACEMENT) back (measured,
            // getwindowplacement_length0: TRUE, length 44).
            (void)length;
            (void)m.write_u32(placement, kWindowPlacementSize);

            const u32 show_cmd = win->minimized
                ? static_cast<u32>(windowing::SW::SHOWMINIMIZED)
                : win->maximized
                    ? static_cast<u32>(windowing::SW::SHOWMAXIMIZED)
                    : static_cast<u32>(windowing::SW::SHOWNORMAL);
            const std::array<u32, 10> values{
                0u,
                show_cmd,
                static_cast<u32>(win->minimized_position.x),
                static_cast<u32>(win->minimized_position.y),
                static_cast<u32>(win->maximized_position.x),
                static_cast<u32>(win->maximized_position.y),
                static_cast<u32>(win->normal_rect.left),
                static_cast<u32>(win->normal_rect.top),
                static_cast<u32>(win->normal_rect.right),
                static_cast<u32>(win->normal_rect.bottom)};
            for (usize i = 0; i < values.size(); ++i) {
                if (m.write_u32(placement + 4 + i * 4, values[i]).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0u);
                    return;
                }
            }
            m.set_return(1u);
        },
        8});

    // IsZoomed(HWND hWnd) → BOOL
    exports.entries.push_back(ExportEntry{
        "IsZoomed", 0,
        [&mgr](Marshaler& m) {
            const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
            const windowing::WindowData* win = mgr.hwnd_mgr().find(hwnd);
            // Measured (iszoomed_invalid): ERROR_INVALID_WINDOW_HANDLE.
            if (!win && !mgr.window_exists(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            const bool zoomed = win && win->maximized;
            VITREO_TRACE(
                "[user32_ext] IsZoomed(hwnd=0x%x) -> %u\n",
                hwnd, zoomed ? 1u : 0u);
            m.set_return(zoomed ? 1u : 0u);
        },
        4});

    // IsWindowArranged(HWND) — Windows 10 1903+. Arranged means snapped and is
    // distinct from normal, minimized, and maximized. Vitreo currently has no
    // snap-window state, so every valid or invalid HWND is not arranged.
    exports.entries.push_back(ExportEntry{
        "IsWindowArranged", 0,
        [](Marshaler& m) { m.set_return(0u); },
        4});

    // DrawMenuBar(HWND hWnd) → BOOL
    exports.entries.push_back(ExportEntry{
        "DrawMenuBar", 0,
        [&mgr](Marshaler& m) { mgr.impl_DrawMenuBar(m); },
        4});

    // GetSubMenu(HMENU hMenu, int nPos) → HMENU
    exports.entries.push_back(ExportEntry{
        "GetSubMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSubMenu(m); },
        8});

    // WaitMessage() → BOOL
    exports.entries.push_back(ExportEntry{
        "WaitMessage", 0,
        [&mgr](Marshaler& m) {
            bool ready = mgr.wait_message();
            VITREO_TRACE("[user32_ext] WaitMessage() → %u\n", ready ? 1u : 0u);
            m.set_return(ready ? 1u : 0u);
        },
        0});

    // UnionRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2) → BOOL
    // Learn: the smallest rectangle containing both; empty rectangles are
    // ignored. Measured (s1win_api_probe union*): one empty source yields
    // the other; two empty sources yield FALSE and an empty destination; a
    // NULL argument yields FALSE with the destination untouched.
    exports.entries.push_back(ExportEntry{
        "UnionRect", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr dst = m.arg_ptr(0);
            const GuestAddr src1 = m.arg_ptr(1);
            const GuestAddr src2 = m.arg_ptr(2);
            if (dst == 0 || src1 == 0 || src2 == 0) { m.set_return(0u); return; }
            auto read = [&](GuestAddr p, i32 (&r)[4]) {
                for (u32 i = 0; i < 4; ++i) {
                    r[i] = static_cast<i32>(mgr.memory().read_u32(p + i * 4).value_or(0));
                }
            };
            i32 a[4], b[4], out[4] = {0, 0, 0, 0};
            read(src1, a);
            read(src2, b);
            const bool a_empty = a[0] >= a[2] || a[1] >= a[3];
            const bool b_empty = b[0] >= b[2] || b[1] >= b[3];
            bool ok = true;
            if (a_empty && b_empty) {
                ok = false;
            } else if (a_empty) {
                for (u32 i = 0; i < 4; ++i) out[i] = b[i];
            } else if (b_empty) {
                for (u32 i = 0; i < 4; ++i) out[i] = a[i];
            } else {
                out[0] = std::min(a[0], b[0]);
                out[1] = std::min(a[1], b[1]);
                out[2] = std::max(a[2], b[2]);
                out[3] = std::max(a[3], b[3]);
            }
            for (u32 i = 0; i < 4; ++i) {
                (void)mgr.memory().write_u32(dst + i * 4, static_cast<u32>(out[i]));
            }
            m.set_return(ok ? 1u : 0u);
        },
        12});
}

} // namespace vitreo::winapi
