// SPDX-License-Identifier: LGPL-2.1-only

/// @file geometry.cpp
/// @brief Where a window is, and converting between the spaces that answer it.
///
/// SetWindowPos and MoveWindow, the parent and ancestor queries, and the
/// coordinate mappings — ScreenToClient, MapWindowPoints, MapDialogRect.
///
/// Three coordinate spaces meet here and a guest expects all three to agree:
/// desktop coordinates, a window's client area, and dialog units, which are
/// derived from the dialog font rather than from pixels. The non-client insets
/// that separate the first two are Vitreo's own numbers, since a Cocoa title
/// bar is not a Win32 caption.
///
/// Gathered from user32_ext.cpp rather than cut: the thirteen functions were
/// spread across the file. Nothing they define is used elsewhere; the one
/// file-local they needed, diag_logs_enabled, is now declared in
/// user32_internal.hpp.

#include "winapi/user32/painting/frame_drawing.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/windowpos_layout.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/result.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace vitreo::winapi {

void User32ExtManager::recalculate_client_area(
    windowing::HWND hwnd,
    const windowing::Rect& new_outer,
    const windowing::Rect& old_outer,
    windowing::HWND insert_after,
    u32 windowpos_flags) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return;

    auto default_client = [&]() {
        i32 border = 0;
        if (win->style & windowing::WS::THICKFRAME) {
            border = 8;
        } else if (win->style & windowing::WS::DLGFRAME) {
            border = 4;
        } else if (win->style & windowing::WS::BORDER) {
            border = 1;
        }
        windowing::Rect result{
            new_outer.left + border,
            new_outer.top + border,
            new_outer.right - border,
            new_outer.bottom - border};
        constexpr u32 kWsCaption = windowing::WS::CAPTION;
        constexpr u32 kWsExToolWindow = 0x00000080u;
        if ((win->style & kWsCaption) == kWsCaption) {
            result.top += (win->ex_style & kWsExToolWindow) ? 22 : 30;
        }
        if (win->has_menu) result.top += 20;
        // Match the standard scroll-bar dimensions reported by GetSystemMetrics.
        if (win->style & windowing::WS::VSCROLL) result.right -= 20;
        if (win->style & windowing::WS::HSCROLL) result.bottom -= 20;

        return result;
    };

    windowing::Rect client = default_client();
    if (m_msgsend_mgr && win->wndproc != 0 && m_windowpos_scratch != 0) {
        const GuestAddr wp = m_windowpos_scratch;
        const GuestAddr params = m_windowpos_scratch + 0x100;
        auto write_rect = [&](GuestAddr address, const windowing::Rect& rect) {
            (void)m_memory.write_u32(address + 0, static_cast<u32>(rect.left));
            (void)m_memory.write_u32(address + 4, static_cast<u32>(rect.top));
            (void)m_memory.write_u32(address + 8, static_cast<u32>(rect.right));
            (void)m_memory.write_u32(address + 12, static_cast<u32>(rect.bottom));
        };
        write_rect(params + 0, new_outer);
        write_rect(params + 16, old_outer);
        write_rect(params + 32, windowing::Rect{
            old_outer.left + win->client_offset.x,
            old_outer.top + win->client_offset.y,
            old_outer.left + win->client_offset.x + win->client_rect.width(),
            old_outer.top + win->client_offset.y + win->client_rect.height()});

        WindowPosLayout{m_is_64bit}.write(m_memory, wp, hwnd, insert_after,
            new_outer.left, new_outer.top, new_outer.width(), new_outer.height(), windowpos_flags);
        if (m_is_64bit) {
            (void)m_memory.write_u64(params + 48, wp);
        } else {
            (void)m_memory.write_u32(params + 48, static_cast<u32>(wp));
        }

        (void)m_msgsend_mgr->send_message(
            hwnd, windowing::WM::NCCALCSIZE, 1, params);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
        const auto left = m_memory.read_u32(params + 0);
        const auto top = m_memory.read_u32(params + 4);
        const auto right = m_memory.read_u32(params + 8);
        const auto bottom = m_memory.read_u32(params + 12);
        if (left.is_ok() && top.is_ok() && right.is_ok() && bottom.is_ok()) {
            windowing::Rect guest_client{
                static_cast<i32>(left.value()),
                static_cast<i32>(top.value()),
                static_cast<i32>(right.value()),
                static_cast<i32>(bottom.value())};
            if (guest_client.right >= guest_client.left &&
                guest_client.bottom >= guest_client.top) {
                client = guest_client;
            }
        }
    }

    win->client_offset = {
        client.left - new_outer.left,
        client.top - new_outer.top};
    // Learn: keep a true zero client after WM_NCCALCSIZE / SetWindowPos.
    // Native backing may still use a 1×1 AppKit surface separately.
    win->client_rect = {0, 0, client.width(), client.height()};
    const bool client_drawn =
        windowing::nccalcsize_guest_owns_frame(new_outer, client, win->style);
    static const bool input_diag = std::getenv("VITREO_INPUT_DIAG") != nullptr;
    if (input_diag && win->client_drawn_frame != client_drawn) {
        std::fprintf(stderr,
                     "[input] WM_NCCALCSIZE hwnd=0x%x style=0x%x outer=(%d,%d,%d,%d) "
                     "client=(%d,%d,%d,%d)\n",
                     hwnd, win->style, new_outer.left, new_outer.top,
                     new_outer.right, new_outer.bottom, client.left, client.top,
                     client.right, client.bottom);
    }
    if (win->client_drawn_frame != client_drawn) {
        win->client_drawn_frame = client_drawn;
        if (m_host.has_native_window(hwnd)) {
            m_host.set_client_drawn_frame(hwnd, client_drawn);
        }
    }
}

void User32ExtManager::impl_ScreenToClient(Marshaler& m) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    GuestAddr pp = m.arg_ptr(1);
    // Measured (screentoclient_invalid): ERROR_INVALID_WINDOW_HANDLE.
    if (hwnd != windowing::HWND_DESKTOP && !window_exists(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (pp == 0) { m.set_return(0u); return; }

    // Read the screen-space POINT
    i32 px = static_cast<i32>(m_memory.read_u32(pp).value_or(0));
    i32 py = static_cast<i32>(m_memory.read_u32(pp + 4).value_or(0));

    // Client-area desktop origin (outer + NC). Pairs with ClientToScreen so
    // GetClientRect → ClientToScreen → ScreenToClient round-trips.
    auto origin = is_virtual_fullscreen_window(hwnd)
        ? windowing::Point{0, 0}
        : client_origin_to_desktop(m_hwnd_mgr, hwnd);
    px -= origin.x;
    py -= origin.y;

    (void)m_memory.write_u32(pp,     static_cast<u32>(px));
    (void)m_memory.write_u32(pp + 4, static_cast<u32>(py));

    m.set_return(1); // TRUE
}

void User32ExtManager::impl_SetWindowPos(Marshaler& m) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    auto hWndInsertAfter = static_cast<windowing::HWND>(m.arg(1));
    i32 x = static_cast<i32>(m.arg(2));
    i32 y = static_cast<i32>(m.arg(3));
    i32 cx = static_cast<i32>(m.arg(4));
    i32 cy = static_cast<i32>(m.arg(5));
    u32 flags = static_cast<u32>(m.arg(6));

    if (std::getenv("VITREO_WIN_DIAG")) {
        const auto* target = m_hwnd_mgr.find(hwnd);
        fprintf(stderr, "[swp] t=%.3f SetWindowPos hwnd=0x%x -> (%d,%d %dx%d) flags=0x%x "
                "caller_tid=%u owner_tid=%u\n",
                windowing::present_view_timestamp(), hwnd, x, y, cx, cy, flags,
                ThreadManager::get_current_thread_id_static(),
                target ? target->owner_thread_id : 0u);
    }

    // Measured (s1win_msgseq_probe swp_invalid_flags, s1win_msgseq2_probe
    // swp flag bits): unknown flag bits are ERROR_INVALID_FLAGS.
    if ((flags & ~kValidSetWindowPosFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0);
        return;
    }
    // Measured (s1win_api_probe swp_insert_after_invalid): an
    // hWndInsertAfter that is neither a special value nor a window is
    // ERROR_INVALID_WINDOW_HANDLE.
    if ((flags & 0x0004u /*SWP_NOZORDER*/) == 0 &&
        hWndInsertAfter != windowing::HWND_TOP &&
        hWndInsertAfter != windowing::HWND_BOTTOM &&
        hWndInsertAfter != windowing::HWND_TOPMOST &&
        hWndInsertAfter != windowing::HWND_NOTOPMOST &&
        !window_exists(hWndInsertAfter)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    if (m_hwnd_mgr.find(hwnd) != nullptr) {
        const bool ok = apply_set_window_pos(
            hwnd, hWndInsertAfter, x, y, cx, cy, flags, true);
        // Success leaves the last error untouched (measured on Windows 11:
        // last_error_on_success_user_probe.expected).
        m.set_return(ok ? 1 : 0);
        return;
    }

    // Learn: SetWindowPos takes any HWND. HWNDs are desktop-global, so a
    // GPU process may SetWindowPos a browser-owned compositor child after
    // SetParent. SWP_ASYNCWINDOWPOS posts to the owner thread when the
    // caller is on a different input queue; cross-process always is.
    const auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd));
    if (remote && remote->owner_host_pid != 0 &&
        remote->owner_host_pid != static_cast<u32>(::getpid())) {
        const bool ok = ipc::remote_set_window_pos(
            static_cast<pid_t>(remote->owner_host_pid),
            static_cast<u32>(hwnd), static_cast<u32>(hWndInsertAfter),
            x, y, cx, cy, flags);
        if (ok) {
            VITREO_TRACE(
                "[user32_ext] SetWindowPos(hwnd=0x%x) peer pid=%u → TRUE\n",
                hwnd, remote->owner_host_pid);
            m.set_return(1);
            return;
        }
        VITREO_TRACE(
            "[user32_ext] SetWindowPos(hwnd=0x%x) peer pid=%u → FALSE\n",
            hwnd, remote->owner_host_pid);
        m.set_return(0);
        return;
    }

    set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
    VITREO_TRACE("[user32_ext] SetWindowPos(hwnd=0x%x) invalid → FALSE\n", hwnd);
    m.set_return(0);
}

bool User32ExtManager::apply_set_parent(windowing::HWND child,
                                        windowing::HWND parent,
                                        windowing::HWND& previous) {
    if (parent == windowing::NULL_HWND) parent = windowing::HWND_DESKTOP;
    previous = windowing::NULL_HWND;
    if (!m_hwnd_mgr.is_valid(child)) return false;
    if (!m_hwnd_mgr.set_parent(child, parent, previous)) return false;
    (void)ipc::hwnd_registry_set_parent(static_cast<u32>(child),
                                        static_cast<u32>(parent));
    if (previous == parent) return true;

    const auto parent_entry =
        ipc::hwnd_registry_lookup(static_cast<u32>(parent));
    if (parent_entry && parent_entry->owner_host_pid != 0 &&
        parent_entry->owner_host_pid != static_cast<u32>(::getpid())) {
        // Parent lives in another host process. Retire the local composition
        // host popup so it cannot cover the owner's window. Do not issue a
        // synchronous HostPeer RPC here: SetParent runs while the owner may
        // already be blocked on that process, and a nested remote_memory
        // round-trip deadlocks the desktop. The owner materializes the peer
        // NSView lazily on PresentWindowSurface.
        m_host.retire_local_host_after_peer_reparent(child);
    } else {
        (void)m_host.set_control_parent(child, parent);
    }
    return true;
}

void User32ExtManager::impl_MoveWindow(Marshaler& m) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    i32 x     = static_cast<i32>(m.arg(1));
    i32 y     = static_cast<i32>(m.arg(2));
    i32 cx    = static_cast<i32>(m.arg(3));
    i32 cy    = static_cast<i32>(m.arg(4));
    bool repaint = m.arg(5) != 0;

    if (std::getenv("VITREO_WIN_DIAG"))
        fprintf(stderr, "[move] MoveWindow hwnd=0x%x -> (%d,%d %dx%d)\n", hwnd, x, y, cx, cy);

    if (m_hwnd_mgr.find(hwnd) == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    // MoveWindow is SetWindowPos(SWP_NOZORDER | SWP_NOACTIVATE), with
    // SWP_NOREDRAW when bRepaint is FALSE (measured, s1win_msgseq_probe
    // movewindow_hidden: CHANGING flags 0x14; movewindow_visible_norepaint:
    // 0x1c). The engine invalidates what the move exposes.
    constexpr u32 SWP_NOZORDER = 0x0004;
    constexpr u32 SWP_NOREDRAW = 0x0008;
    constexpr u32 SWP_NOACTIVATE = 0x0010;
    const bool moved = apply_set_window_pos(
        hwnd, windowing::HWND_TOP, x, y, cx, cy,
        SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0u : SWP_NOREDRAW),
        /*send_messages=*/true);
    VITREO_TRACE( "[user32_ext] MoveWindow(hwnd=0x%x, %d,%d,%dx%d, repaint=%d) → %d\n",
                 hwnd, x, y, cx, cy, repaint, moved ? 1 : 0);
    m.set_return(moved ? 1 : 0);
}

void User32ExtManager::impl_GetParent(Marshaler& m) {
    // Learn GetParent: a child window's parent; for a top-level window with
    // WS_POPUP, its owner; otherwise NULL (measured, s1win_api_probe:
    // getparent_owned_overlapped is NULL, getparent_invalid is
    // ERROR_INVALID_WINDOW_HANDLE, message-only windows and the desktop
    // report NULL).
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    windowing::HWND parent = windowing::NULL_HWND;
    if (!win) {
        if (hwnd != windowing::HWND_DESKTOP) {
            if (const auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
                if (remote->parent_hwnd != windowing::HWND_DESKTOP &&
                    remote->parent_hwnd != windowing::HWND_MESSAGE) {
                    parent = static_cast<windowing::HWND>(remote->parent_hwnd);
                }
            } else {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            }
        }
    } else if (!win->is_message_only) {
        // A WS_CHILD window whose parent is the desktop reports the desktop
        // window (measured, s1win_msgseq2_probe getparent_after_desktop_null
        // is FALSE after SetParent(child, GetDesktopWindow())).
        const bool child = (win->style & windowing::WS::CHILD) != 0 &&
                           win->parent != windowing::NULL_HWND;
        if (child) {
            parent = win->parent;
        } else if ((win->style & windowing::WS::POPUP) != 0) {
            parent = win->owner;
        }
    }
    VITREO_TRACE( "[user32_ext] GetParent(hwnd=0x%x) → 0x%x\n", hwnd, parent);
    m.set_return(static_cast<u64>(parent));
}

void User32ExtManager::impl_GetAncestor(Marshaler& m) {
    // HWND GetAncestor(HWND hwnd, UINT gaFlags)
    // Learn: GA_PARENT does not return the owner (unlike GetParent). GA_ROOT
    // walks parents to the top-level window. GA_ROOTOWNER walks GetParent
    // (parent/owner) to the owned root. Measured (s1win_api_probe
    // getancestor_*): an invalid window is ERROR_INVALID_WINDOW_HANDLE, a
    // gaFlags outside 1..3 is ERROR_INVALID_PARAMETER, the desktop has no
    // ancestor, and a message-only window's parent is the (non-desktop)
    // message root while its root is itself.
    constexpr u32 kGaParent = 1;
    constexpr u32 kGaRoot = 2;
    constexpr u32 kGaRootOwner = 3;

    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(1));

    auto get_parent_chain = [this](windowing::HWND h) -> windowing::HWND {
        const windowing::WindowData* win = m_hwnd_mgr.find(h);
        if (!win) {
            return windowing::NULL_HWND;
        }
        if ((win->style & windowing::WS::CHILD) != 0) {
            windowing::HWND parent = win->parent;
            if (parent == windowing::HWND_DESKTOP ||
                parent == windowing::HWND_MESSAGE) {
                return windowing::NULL_HWND;
            }
            return parent;
        }
        // GetParent's rule: only a WS_POPUP window reports its owner.
        return (win->style & windowing::WS::POPUP) != 0 ? win->owner
                                                        : windowing::NULL_HWND;
    };

    if (hwnd == windowing::HWND_DESKTOP) {
        if (flags < kGaParent || flags > kGaRootOwner) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(0);
        return;
    }
    const windowing::WindowData* self = m_hwnd_mgr.find(hwnd);
    if (!self) {
        if (!ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0);
            return;
        }
        // Another process's window: only its published parent is known.
        windowing::HWND result = windowing::NULL_HWND;
        if (flags == kGaParent) {
            const auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd));
            result = remote ? static_cast<windowing::HWND>(remote->parent_hwnd)
                            : windowing::NULL_HWND;
        } else if (flags == kGaRoot || flags == kGaRootOwner) {
            result = hwnd;
        } else {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(static_cast<u64>(result));
        return;
    }
    if (flags < kGaParent || flags > kGaRootOwner) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    windowing::HWND result = windowing::NULL_HWND;
    if (flags == kGaParent) {
        if (self->is_message_only || self->parent == windowing::HWND_MESSAGE) {
            result = windowing::HWND_MESSAGE;
        } else if (self->parent == windowing::NULL_HWND) {
            result = windowing::HWND_DESKTOP;
        } else {
            // Child → real parent; top-level → HWND_DESKTOP (not owner).
            result = self->parent;
        }
    } else if (flags == kGaRoot) {
        windowing::HWND cur = hwnd;
        for (u32 guard = 0; guard < 256; ++guard) {
            const windowing::WindowData* win = m_hwnd_mgr.find(cur);
            if (!win) break;
            const windowing::HWND parent = win->parent;
            if (parent == windowing::HWND_DESKTOP ||
                parent == windowing::NULL_HWND ||
                parent == windowing::HWND_MESSAGE || win->is_message_only ||
                !m_hwnd_mgr.is_valid(parent)) {
                result = cur;
                break;
            }
            cur = parent;
        }
    } else {
        windowing::HWND cur = hwnd;
        for (u32 guard = 0; guard < 256; ++guard) {
            const windowing::HWND parent = get_parent_chain(cur);
            if (parent == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(parent)) {
                result = cur;
                break;
            }
            cur = parent;
        }
    }

    VITREO_TRACE(
        "[user32_ext] GetAncestor(hwnd=0x%x flags=%u) → 0x%x\n",
        hwnd, flags, result);
    m.set_return(static_cast<u64>(result));
}

void User32ExtManager::impl_GetDialogBaseUnits(Marshaler& m) {
    // DWORD GetDialogBaseUnits(void)
    // Returns packed value: low word = base unit X, high word = base unit Y.
    // Windows standard: based on the System font (tmAveCharWidth=8, tmHeight=16).
    constexpr u32 base_x = 8;
    constexpr u32 base_y = 16;
    m.set_return((base_y << 16) | base_x);
}

void User32ExtManager::impl_MapDialogRect(Marshaler& m) {
    // BOOL MapDialogRect(HWND hDlg, LPRECT lpRect): converts with the
    // dialog's own base units (its template font). Measured
    // (s1win_misc_probe mapdialogrect*): a window that is not a dialog is
    // ERROR_WINDOW_NOT_DIALOG.
    const auto dlg = static_cast<windowing::HWND>(m.arg(0));
    GuestAddr rect_ptr = m.arg_ptr(1);
    const windowing::WindowData* dwin = m_hwnd_mgr.find(dlg);
    if (!dwin) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    i32 base_x = 8, base_y = 16;
    if (!dwin->is_dialog ||
        (m_dialog_mgr && !m_dialog_mgr->dialog_base_units(dlg, base_x, base_y) && false)) {
        set_last_error(1420);  // ERROR_WINDOW_NOT_DIALOG
        m.set_return(0u);
        return;
    }
    if (m_dialog_mgr) (void)m_dialog_mgr->dialog_base_units(dlg, base_x, base_y);
    if (rect_ptr == 0) {
        m.set_return(0u);
        return;
    }

    auto l = m_memory.read_u32(rect_ptr + 0);
    auto t = m_memory.read_u32(rect_ptr + 4);
    auto r = m_memory.read_u32(rect_ptr + 8);
    auto b = m_memory.read_u32(rect_ptr + 12);
    if (l.is_err() || t.is_err() || r.is_err() || b.is_err()) {
        m.set_return(0u);
        return;
    }

    i32 left   = static_cast<i32>(l.value());
    i32 top    = static_cast<i32>(t.value());
    i32 right  = static_cast<i32>(r.value());
    i32 bottom = static_cast<i32>(b.value());

    // MulDiv(x, base, 4 or 8), rounded.
    auto conv = [](i32 v, i32 base, i32 div) {
        const i64 n = static_cast<i64>(v) * base;
        return static_cast<i32>(n >= 0 ? (n + div / 2) / div : (n - div / 2) / div);
    };
    left   = conv(left, base_x, 4);
    right  = conv(right, base_x, 4);
    top    = conv(top, base_y, 8);
    bottom = conv(bottom, base_y, 8);

    (void)m_memory.write_u32(rect_ptr + 0, static_cast<u32>(left));
    (void)m_memory.write_u32(rect_ptr + 4, static_cast<u32>(top));
    (void)m_memory.write_u32(rect_ptr + 8, static_cast<u32>(right));
    (void)m_memory.write_u32(rect_ptr + 12, static_cast<u32>(bottom));

    m.set_return(1u);
}

void User32ExtManager::impl_MapWindowPoints(Marshaler& m) {
    // int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints)
    auto from = static_cast<windowing::HWND>(m.arg(0));
    auto to = static_cast<windowing::HWND>(m.arg(1));
    GuestAddr pts_ptr = m.arg_ptr(2);
    u32 c = static_cast<u32>(m.arg(3));

    // Learn: NULL (or HWND_DESKTOP) names screen coordinates. Measured
    // (mapwindowpoints_invalid_from): any other invalid window is
    // ERROR_INVALID_WINDOW_HANDLE; the return value packs the x and y
    // offsets even for cPoints == 0 (mapwindowpoints_zero_count).
    for (const windowing::HWND h : {from, to}) {
        if (h != windowing::NULL_HWND && h != windowing::HWND_DESKTOP &&
            !window_exists(h)) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0u);
            return;
        }
    }

    // MapWindowPoints maps client-space points between windows (MS Learn).
    auto o_from = client_origin_to_desktop(m_hwnd_mgr, from);
    auto o_to = client_origin_to_desktop(m_hwnd_mgr, to);
    i32 dx = o_from.x - o_to.x;
    i32 dy = o_from.y - o_to.y;

    for (u32 i = 0; pts_ptr != 0 && i < c; ++i) {
        GuestAddr p = pts_ptr + i * 8;
        auto x = m_memory.read_u32(p + 0);
        auto y = m_memory.read_u32(p + 4);
        if (x.is_err() || y.is_err()) {
            break;
        }
        i32 nx = static_cast<i32>(x.value()) + dx;
        i32 ny = static_cast<i32>(y.value()) + dy;
        (void)m_memory.write_u32(p + 0, static_cast<u32>(nx));
        (void)m_memory.write_u32(p + 4, static_cast<u32>(ny));
    }

    u32 ret = (static_cast<u32>(static_cast<u16>(dx)) & 0xFFFFu) |
              (static_cast<u32>(static_cast<u16>(dy)) << 16);
    m.set_return(ret);
}

void User32ExtManager::impl_DrawFocusRect(Marshaler& m) {
    // BOOL DrawFocusRect(HDC hDC, const RECT* lprc): painting/frame_drawing.cpp.
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr rect = m.arg_ptr(1);
    Rect rc{};
    if (m_gdi_mgr == nullptr || rect == 0 ||
        m.memory().read_bytes(rect, reinterpret_cast<u8*>(&rc), sizeof(rc)).is_err()) {
        m.set_return(0u);
        return;
    }
    m.set_return(user_draw::draw_focus_rect(*m_gdi_mgr, hdc, rc) ? 1u : 0u);
}

void User32ExtManager::impl_InvertRect(Marshaler& m) {
    // BOOL InvertRect(HDC hDC, const RECT* lprc): DSTINVERT over the rectangle.
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr rect = m.arg_ptr(1);
    Rect rc{};
    if (m_gdi_mgr == nullptr || rect == 0 ||
        m.memory().read_bytes(rect, reinterpret_cast<u8*>(&rc), sizeof(rc)).is_err()) {
        m.set_return(0u);
        return;
    }
    m.set_return(user_draw::invert_rect(*m_gdi_mgr, hdc, rc) ? 1u : 0u);
}

void User32ExtManager::impl_SetParent(Marshaler& m) {
    // HWND SetParent(HWND hWndChild, HWND hWndNewParent). Measured
    // (s1win_msgseq2_probe setparent_*): an invalid child or parent is
    // ERROR_INVALID_WINDOW_HANDLE; the child itself or one of its
    // descendants as the new parent is ERROR_INVALID_PARAMETER.
    const auto child = static_cast<windowing::HWND>(m.arg(0));
    auto parent = static_cast<windowing::HWND>(m.arg(1));
    if (parent == windowing::NULL_HWND) parent = windowing::HWND_DESKTOP;
    const bool parent_valid =
        parent == windowing::HWND_DESKTOP ||
        parent == windowing::HWND_MESSAGE ||
        m_hwnd_mgr.is_valid(parent) ||
        ipc::hwnd_registry_is_window(static_cast<u32>(parent));
    const bool child_valid = m_hwnd_mgr.is_valid(child) ||
                             ipc::hwnd_registry_is_window(static_cast<u32>(child));
    if (!parent_valid || !child_valid || child == windowing::HWND_DESKTOP) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    for (windowing::HWND cur = parent, guard = 0; guard < 256; ++guard) {
        if (cur == child) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        const windowing::WindowData* w = m_hwnd_mgr.find(cur);
        if (!w) break;
        cur = w->parent;
    }

    windowing::HWND previous = windowing::NULL_HWND;
    const bool local_parent = parent == windowing::HWND_DESKTOP || m_hwnd_mgr.is_valid(parent);
    if (m_hwnd_mgr.is_valid(child) && local_parent) {
        // Measured (setparent_child_to_desktop / setparent_child_back /
        // setparent_between_visible_parents): a visible window is hidden with
        // ShowWindow(SW_HIDE), moved to the new parent at the same
        // parent-relative position with SetWindowPos(SWP_NOSIZE |
        // SWP_NOZORDER), and shown again with ShowWindow(SW_SHOWNA). A window
        // moved to the desktop gains WS_CLIPSIBLINGS.
        const windowing::WindowData* win = m_hwnd_mgr.find(child);
        const bool was_visible = (win->style & windowing::WS::VISIBLE) != 0;
        if (was_visible) (void)show_window_cmd(child, windowing::SW::HIDE);
        if (!apply_set_parent(child, parent, previous)) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0u);
            return;
        }
        if (windowing::WindowData* moved = m_hwnd_mgr.find(child)) {
            if (parent == windowing::HWND_DESKTOP) moved->style |= windowing::WS::CLIPSIBLINGS;
            reposition_after_reparent(child);
        }
        if (was_visible && m_hwnd_mgr.find(child)) {
            (void)show_window_cmd(child, windowing::SW::SHOWNA);
        }
    } else if (m_hwnd_mgr.is_valid(child)) {
        if (!apply_set_parent(child, parent, previous)) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0u);
            return;
        }
    } else {
        const auto child_owner = ipc::hwnd_registry_lookup(
            static_cast<u32>(child));
        const auto previous_remote = ipc::hwnd_registry_set_parent(
            static_cast<u32>(child), static_cast<u32>(parent));
        if (!previous_remote.has_value()) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0u);
            return;
        }
        previous = static_cast<windowing::HWND>(*previous_remote);
        if (child_owner && child_owner->owner_host_pid != 0 &&
            child_owner->owner_host_pid != static_cast<u32>(::getpid())) {
            (void)ipc::remote_set_parent(
                static_cast<pid_t>(child_owner->owner_host_pid),
                static_cast<u32>(child), static_cast<u32>(parent));
            (void)ipc::remote_refresh_window_pixels(
                static_cast<pid_t>(child_owner->owner_host_pid),
                static_cast<u32>(child));
        }
        // Parent process side: host a peer NSView for the foreign child so
        // PresentWindowSurface can target the child HWND without covering
        // sibling windows on this parent.
        if (m_hwnd_mgr.is_valid(parent) || m_host.has_control_view(parent)) {
            const i32 width =
                child_owner && child_owner->client_w > 0
                    ? child_owner->client_w
                    : 1;
            const i32 height =
                child_owner && child_owner->client_h > 0
                    ? child_owner->client_h
                    : 1;
            (void)m_host.ensure_hosted_peer_child(
                parent, child, windowing::Rect{0, 0, width, height});
        }
    }
    VITREO_TRACE("[u32ext] SetParent child=0x%x parent=0x%x -> 0x%x\n",
                 child, parent, previous);
    m.set_return(previous);
}

void User32ExtManager::impl_SetRectEmpty(Marshaler& m) {
    GuestAddr lprc = m.arg_ptr(0);
    if (lprc) {
        (void)m.write_u32(lprc + 0, 0);
        (void)m.write_u32(lprc + 4, 0);
        (void)m.write_u32(lprc + 8, 0);
        (void)m.write_u32(lprc + 12, 0);
    }
    VITREO_TRACE("[u32ext] SetRectEmpty -> TRUE\n");
    m.set_return(1u);
}

void User32ExtManager::impl_EqualRect(Marshaler& m) {
    GuestAddr lprc1 = m.arg_ptr(0);
    GuestAddr lprc2 = m.arg_ptr(1);
    bool equal = false;
    if (lprc1 && lprc2) {
        equal = (m.read_u32(lprc1).value_or(0) == m.read_u32(lprc2).value_or(0)) &&
                (m.read_u32(lprc1 + 4).value_or(0) == m.read_u32(lprc2 + 4).value_or(0)) &&
                (m.read_u32(lprc1 + 8).value_or(0) == m.read_u32(lprc2 + 8).value_or(0)) &&
                (m.read_u32(lprc1 + 12).value_or(0) == m.read_u32(lprc2 + 12).value_or(0));
    }
    VITREO_TRACE("[u32ext] EqualRect -> %u\n", equal ? 1u : 0u);
    m.set_return(equal ? 1u : 0u);
}

void User32ExtManager::impl_SubtractRect(Marshaler& m) {
    GuestAddr lprcDst = m.arg_ptr(0);
    GuestAddr lprcSrc1 = m.arg_ptr(1);
    (void)m.arg_ptr(2); // lprcSrc2
    if (lprcDst && lprcSrc1) {
        (void)m.write_u32(lprcDst + 0, m.read_u32(lprcSrc1 + 0).value_or(0));
        (void)m.write_u32(lprcDst + 4, m.read_u32(lprcSrc1 + 4).value_or(0));
        (void)m.write_u32(lprcDst + 8, m.read_u32(lprcSrc1 + 8).value_or(0));
        (void)m.write_u32(lprcDst + 12, m.read_u32(lprcSrc1 + 12).value_or(0));
    }
    VITREO_TRACE("[u32ext] SubtractRect -> TRUE\n");
    m.set_return(1u);
}

} // namespace vitreo::winapi
