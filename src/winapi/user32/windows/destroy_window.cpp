// SPDX-License-Identifier: LGPL-2.1-only

/// @file window.cpp
/// @brief CreateWindowExW/DestroyWindow implementation (WIN32-051)

#include "window.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/hwnd_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <optional>
#include <vector>

/// @file destroy_window.cpp
/// @brief destroy_window
///
/// Closed cut from window.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// destroy_window
// -----------------------------------------------------------------------------

void WindowManager::send_parent_notify(windowing::HWND hwnd, u32 event) {
    // Learn WM_PARENTNOTIFY: sent to the parent of a child window being
    // created or destroyed, and on up the chain; a window with
    // WS_EX_NOPARENTNOTIFY stops it (measured, s1win_msgseq2_probe
    // create_child_hidden / create_child_noparentnotify, s1win_msgseq_probe
    // destroy_child: wParam = MAKEWPARAM(event, child id), lParam = child).
    constexpr u32 kWsExNoParentNotify = 0x00000004u;
    const windowing::WindowData* child = m_hwnd_mgr.find(hwnd);
    if (!child) return;
    const u64 wparam = static_cast<u64>(event & 0xFFFFu) |
                       (static_cast<u64>(child->ctrl_id & 0xFFFFu) << 16);
    windowing::HWND cur = hwnd;
    for (u32 guard = 0; guard < 256; ++guard) {
        const windowing::WindowData* w = m_hwnd_mgr.find(cur);
        if (!w || (w->style & windowing::WS::CHILD) == 0 ||
            (w->ex_style & kWsExNoParentNotify) != 0) {
            return;
        }
        const windowing::HWND parent = w->parent;
        const windowing::WindowData* p = m_hwnd_mgr.find(parent);
        if (!p) return;
        if (m_wndproc_callback && p->wndproc != 0) {
            (void)m_wndproc_callback(parent, 0x0210u /* WM_PARENTNOTIFY */,
                                     wparam, static_cast<u64>(hwnd), p->wndproc);
        }
        cur = parent;
    }
}

bool WindowManager::destroy_window(windowing::HWND hwnd) {
    // Desktop / NULL HWND cannot be destroyed (matches Windows behaviour).
    if (hwnd == windowing::NULL_HWND || hwnd == windowing::HWND_DESKTOP) {
        if (hwnd == windowing::NULL_HWND) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        return false;
    }

    // 1. Find window
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        if (std::getenv("VITREO_WIN_DIAG"))
            fprintf(stderr, "[destroy] wm: hwnd=0x%x NOT in hwnd_mgr — bridge removal still attempted\n", hwnd);
        // The HWND is gone from the manager but its native view may still be
        // mapped (e.g. a stale reference, or it was only ever a Cocoa control).
        // Remove the native view anyway so it can't linger on screen.
        m_bridge.destroy_window(hwnd);
        // Measured (s1win_msgseq_probe destroy_again): an invalid or already
        // destroyed window is ERROR_INVALID_WINDOW_HANDLE.
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }

    // The order is measured (s1win_msgseq_probe destroy_child /
    // destroy_popup_visible, s1win_msgseq2_probe destroy_*):
    //  1. WM_PARENTNOTIFY(WM_DESTROY) up the parent chain;
    //  2. a visible window is hidden: a child by ShowWindow(SW_HIDE), a
    //     top-level window by SetWindowPos(SWP_HIDEWINDOW | SWP_NOACTIVATE |
    //     SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE), without WM_SHOWWINDOW;
    //  3. owned windows are destroyed completely;
    //  4. WM_DESTROY to the window and then every descendant (pre-order);
    //  5. WM_NCDESTROY to the descendants and then the window (post-order).
    send_parent_notify(hwnd, static_cast<u32>(windowing::WM::DESTROY));
    win = m_hwnd_mgr.find(hwnd);
    if (!win) return true;

    if ((win->style & windowing::WS::VISIBLE) != 0 && m_ops.show && m_ops.set_pos) {
        if ((win->style & windowing::WS::CHILD) != 0) {
            (void)m_ops.show(hwnd, windowing::SW::HIDE);
        } else {
            constexpr u32 kHide = 0x0080u | 0x0010u | 0x0004u | 0x0002u | 0x0001u;
            m_ops.set_pos(hwnd, 0, 0, 0, 0, kHide);
        }
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return true;
    }

    if (m_destroy_activation_callback) {
        m_destroy_activation_callback(hwnd);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return true;
    }

    const bool is_top_level =
        win->parent == windowing::HWND_DESKTOP ||
        win->parent == windowing::NULL_HWND;

    // Owned top-level windows go first, each destroyed completely.
    for (windowing::HWND candidate : m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
        if (candidate == hwnd) continue;
        const windowing::WindowData* top = m_hwnd_mgr.find(candidate);
        if (top && top->owner == hwnd) {
            (void)destroy_window(candidate);
        }
    }
    if (!m_hwnd_mgr.find(hwnd)) return true;

    destroy_tree(hwnd);

    if (is_top_level && m_lifecycle_callback) {
        m_lifecycle_callback(false, hwnd);
    }
    return true;
}

void WindowManager::destroy_tree(windowing::HWND root) {
    auto post = [&](windowing::HWND h, u32 msg) {
        windowing::MSG queued{};
        queued.hwnd = h;
        queued.message = msg;
        m_queue.post(queued);
    };
    // WM_DESTROY, pre-order. The tree is re-read at each level: a window
    // procedure may create or destroy windows while handling WM_DESTROY.
    std::vector<windowing::HWND> stack{root};
    while (!stack.empty()) {
        const windowing::HWND h = stack.back();
        stack.pop_back();
        const windowing::WindowData* w = m_hwnd_mgr.find(h);
        if (!w) continue;
        const u64 proc = w->wndproc;
        if (m_wndproc_callback && proc != 0) {
            (void)m_wndproc_callback(h, static_cast<u32>(windowing::WM::DESTROY), 0, 0, proc);
        } else {
            post(h, windowing::WM::DESTROY);
        }
        const auto kids = m_hwnd_mgr.enum_children(h);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }

    // WM_NCDESTROY, post-order: every descendant before its parent, then the
    // native object and the HWND are freed.
    std::function<void(windowing::HWND)> free_window = [&](windowing::HWND h) {
        const windowing::WindowData* w = m_hwnd_mgr.find(h);
        if (!w) return;
        for (const windowing::HWND kid : m_hwnd_mgr.enum_children(h)) free_window(kid);
        w = m_hwnd_mgr.find(h);
        if (!w) return;
        const u64 proc = w->wndproc;
        if (m_wndproc_callback && proc != 0) {
            (void)m_wndproc_callback(h, static_cast<u32>(windowing::WM::NCDESTROY), 0, 0, proc);
        } else {
            post(h, windowing::WM::NCDESTROY);
        }
        if (!m_hwnd_mgr.find(h)) return;
        // For a top-level window this closes the NSWindow; for a child
        // control it removes the NSView from its superview.
        m_bridge.destroy_window(h);
        // Per-window USER state kept outside WindowData (properties, menus).
        if (m_destroyed_callback) m_destroyed_callback(h);
        // Measured (winevent_sequence_probe): EVENT_OBJECT_DESTROY for each
        // destroyed window, after the HIDE of a visible one.
        constexpr u32 EVENT_OBJECT_DESTROY = 0x8001u;
        raise_win_event(EVENT_OBJECT_DESTROY, h);
        m_hwnd_mgr.destroy(h);
        ipc::hwnd_registry_unpublish(static_cast<u32>(h));
    };
    free_window(root);
}

} // namespace vitreo::winapi
