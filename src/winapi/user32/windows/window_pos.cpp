// SPDX-License-Identifier: LGPL-2.1-only

/// @file window_pos.cpp
/// @brief The window-position engine behind SetWindowPos, MoveWindow,
/// BringWindowToTop, ShowWindow, CloseWindow/OpenIcon and DeferWindowPos.
///
/// Contract: Microsoft Learn SetWindowPos, ShowWindow, WINDOWPOS,
/// WM_WINDOWPOSCHANGING/CHANGED, WM_SHOWWINDOW, DeferWindowPos. Learn names
/// the messages; their order and the WINDOWPOS flags USER adds are measured on
/// Windows 11 by tools/oracle_probe/s1win_msgseq_probe.c and
/// s1win_msgseq2_probe.c (.expected alongside):
///
///  * WM_WINDOWPOSCHANGING is always sent (unless SWP_NOSENDCHANGING) and
///    may edit the operation; WM_WINDOWPOSCHANGED follows only when something
///    changed. WM_GETMINMAXINFO is DefWindowProc's reply to CHANGING.
///  * WM_NCCALCSIZE when the size or frame changes; WM_NCPAINT then
///    WM_ERASEBKGND synchronously when a visible window becomes visible or
///    grows; activation (or WM_CHILDACTIVATE) before WM_WINDOWPOSCHANGED.
///  * CHANGED's flags add SWP_NOREDRAW for a window hidden before and after,
///    SWP_NOCLIENTSIZE / SWP_NOCLIENTMOVE for an unchanged client, and
///    SWP_NOZORDER when the Z order did not move.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/user32_errors.hpp"
#include "winapi/user32/windows/windowpos_layout.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/min_track.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "ipc/hwnd_registry.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

namespace {

constexpr u32 SWP_NOSIZE         = 0x0001;
constexpr u32 SWP_NOMOVE         = 0x0002;
constexpr u32 SWP_NOZORDER       = 0x0004;
constexpr u32 SWP_NOREDRAW       = 0x0008;
constexpr u32 SWP_NOACTIVATE     = 0x0010;
constexpr u32 SWP_FRAMECHANGED   = 0x0020;
constexpr u32 SWP_SHOWWINDOW     = 0x0040;
constexpr u32 SWP_HIDEWINDOW     = 0x0080;
constexpr u32 SWP_NOCOPYBITS     = 0x0100;
constexpr u32 SWP_NOSENDCHANGING = 0x0400;
constexpr u32 SWP_NOCLIENTSIZE   = 0x0800;  // internal, WM_WINDOWPOSCHANGED
constexpr u32 SWP_NOCLIENTMOVE   = 0x1000;  // internal, WM_WINDOWPOSCHANGED
constexpr u32 SWP_STATECHANGED   = 0x8000;  // internal, min/max transitions

constexpr u32 WM_CHILDACTIVATE = 0x0022;
constexpr u32 WM_QUERYOPEN     = 0x0013;
constexpr u32 WM_NCPAINT       = 0x0085;
constexpr u32 WM_KILLFOCUS     = 0x0008;

/// SM_CXMINIMIZED / SM_CYMINIMIZED (measured: minimized windows are 160x28 at
/// (-32000, -32000); s1win_msgseq2_probe showwindow_minimize_activating).
constexpr i32 kMinimizedWidth = 160;
constexpr i32 kMinimizedHeight = 28;
constexpr i32 kMinimizedParked = -32000;

/// Saves and restores the shared WINDOWPOS / NCCALCSIZE scratch around a
/// transaction.
struct ScratchSave {
    GuestMemory& memory;
    GuestAddr address;
    std::array<Byte, 0x240> bytes{};
    bool saved{false};
    ScratchSave(GuestMemory& mem, GuestAddr addr) : memory(mem), address(addr) {
        if (address) saved = memory.read_bytes(address, bytes.data(), bytes.size()).is_ok();
    }
    ~ScratchSave() {
        if (saved) (void)memory.write_bytes(address, bytes.data(), bytes.size());
    }
};

bool top_level(const windowing::WindowData& win) {
    return win.parent == windowing::HWND_DESKTOP ||
           win.parent == windowing::NULL_HWND;
}

} // namespace

bool User32ExtManager::effectively_visible(windowing::HWND hwnd) const {
    // IsWindowVisible: WS_VISIBLE on the window and every ancestor.
    windowing::HWND cur = hwnd;
    for (u32 guard = 0; guard < 256; ++guard) {
        const windowing::WindowData* w = m_hwnd_mgr.find(cur);
        if (!w) return cur == windowing::HWND_DESKTOP;
        if ((w->style & windowing::WS::VISIBLE) == 0) return false;
        if (w->parent == windowing::HWND_DESKTOP || w->parent == windowing::NULL_HWND ||
            w->parent == windowing::HWND_MESSAGE) {
            return true;
        }
        cur = w->parent;
    }
    return false;
}

bool User32ExtManager::apply_set_window_pos(windowing::HWND hwnd,
                                            windowing::HWND hWndInsertAfter,
                                            i32 x, i32 y, i32 cx, i32 cy,
                                            u32 flags, bool send_messages, bool host_geometry_applied) {
    if (!m_hwnd_mgr.find(hwnd)) return false;
    // A WndProc can synchronously move another window and then continue
    // reading its own WINDOWPOS/NCCALCSIZE_PARAMS; keep the enclosing
    // transaction's structures as nested stack frames would on Windows.
    ScratchSave save(m_memory, send_messages ? m_windowpos_scratch : 0);
    SwpTransaction txn{};
    txn.hwnd = hwnd;
    txn.after = hWndInsertAfter;
    txn.x = x; txn.y = y; txn.cx = cx; txn.cy = cy;
    txn.flags = flags;
    txn.send = send_messages;
    if (!swp_changing(txn)) return true;
    // Native geometry has already happened in AppKit. Do not replay an older
    // queued resize into the host while it is tracking the user's pointer.
    // A guest WndProc that changes the proposal still gets its correction.
    txn.host_geometry_applied = host_geometry_applied &&
        txn.x == x && txn.y == y && txn.cx == cx && txn.cy == cy &&
        txn.flags == flags;
    if (!swp_apply(txn)) return true;
    swp_finish(txn);
    return true;
}

void User32ExtManager::reposition_after_reparent(windowing::HWND hwnd) {
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return;
    ScratchSave save(m_memory, m_windowpos_scratch);
    SwpTransaction txn{};
    txn.hwnd = hwnd;
    txn.after = windowing::HWND_TOP;
    txn.x = win->rect.left;
    txn.y = win->rect.top;
    txn.flags = SWP_NOSIZE | SWP_NOZORDER;
    txn.send = true;
    txn.reparented = true;
    if (!swp_changing(txn)) return;
    if (!swp_apply(txn)) return;
    swp_finish(txn);
}

void User32ExtManager::swp_notify(const SwpTransaction& txn, u32 msg, u64 wParam, u64 lParam) {
    const windowing::WindowData* win = m_hwnd_mgr.find(txn.hwnd);
    if (!win) return;
    if (txn.send && m_msgsend_mgr && win->wndproc != 0) {
        (void)m_msgsend_mgr->send_message(txn.hwnd, msg, wParam, lParam);
        return;
    }
    windowing::MSG queued{};
    queued.hwnd = txn.hwnd;
    queued.message = msg;
    queued.wParam = wParam;
    queued.lParam = lParam;
    m_queue.post(queued);
}

bool User32ExtManager::swp_changing(SwpTransaction& txn) {
    windowing::WindowData* win = m_hwnd_mgr.find(txn.hwnd);
    if (!win) return false;
    // Native common controls apply their own sizing rules even without a
    // guest WndProc. CCS_NORESIZE explicitly opts out of their default size.
    if ((txn.flags & SWP_NOSIZE) == 0 && m_msgsend_mgr)
        m_msgsend_mgr->adjust_common_control_size(txn.hwnd, txn.cx, txn.cy);

    txn.was_visible = win->visible;
    txn.was_effectively_visible = effectively_visible(txn.hwnd);
    txn.old_rect = win->rect;
    txn.old_client = win->client_rect;
    txn.old_client_offset = win->client_offset;
    txn.old_ex = win->ex_style;
    txn.old_siblings = m_hwnd_mgr.enum_children(
        win->parent == windowing::NULL_HWND ? windowing::HWND_DESKTOP : win->parent);

    // Measured (s1win_msgseq_probe swp_negative_size): a negative extent
    // reaches WM_WINDOWPOSCHANGING as 0.
    if ((txn.flags & SWP_NOSIZE) == 0) {
        txn.cx = std::max(txn.cx, 0);
        txn.cy = std::max(txn.cy, 0);
    }
    if ((txn.flags & SWP_NOSENDCHANGING) != 0 || txn.changing_sent || !txn.send ||
        m_windowpos_scratch == 0) {
        return true;
    }
    // WM_WINDOWPOSCHANGING requires lParam to point at a WINDOWPOS struct
    // (28 bytes on x86, 40 on x64) with the *proposed* values. It is an
    // in/out contract: the procedure (and DefWindowProc, which applies
    // WM_GETMINMAXINFO here) may change the operation.
    const GuestAddr wp_addr = m_windowpos_scratch;
    const WindowPosLayout layout{m_is_64bit};
    layout.write(m_memory, wp_addr, txn.hwnd, txn.after, txn.x, txn.y, txn.cx, txn.cy, txn.flags);
    swp_notify(txn, windowing::WM::WINDOWPOSCHANGING, 0, wp_addr);
    if (!m_hwnd_mgr.find(txn.hwnd)) return false;
    swp_read_back(txn, wp_addr);
    txn.changing_sent = true;
    return true;
}

void User32ExtManager::swp_read_back(SwpTransaction& txn, GuestAddr wp_addr) {
    const WindowPosLayout layout{m_is_64bit};
    const u32 base = layout.scalar_offset();
    txn.after = static_cast<windowing::HWND>(
        m_memory.read_u32(wp_addr + (m_is_64bit ? 8 : 4)).value_or(static_cast<u32>(txn.after)));
    txn.x = static_cast<i32>(m_memory.read_u32(wp_addr + base + 0).value_or(static_cast<u32>(txn.x)));
    txn.y = static_cast<i32>(m_memory.read_u32(wp_addr + base + 4).value_or(static_cast<u32>(txn.y)));
    txn.cx = static_cast<i32>(m_memory.read_u32(wp_addr + base + 8).value_or(static_cast<u32>(txn.cx)));
    txn.cy = static_cast<i32>(m_memory.read_u32(wp_addr + base + 12).value_or(static_cast<u32>(txn.cy)));
    txn.flags = m_memory.read_u32(wp_addr + base + 16).value_or(txn.flags);
}

bool User32ExtManager::swp_apply(SwpTransaction& txn) {
    windowing::WindowData* win = m_hwnd_mgr.find(txn.hwnd);
    if (!win) return false;
    const windowing::HWND hwnd = txn.hwnd;
    windowing::Rect new_rect = win->rect;
    if ((txn.flags & SWP_NOMOVE) == 0) {
        const i32 w = new_rect.width();
        const i32 h = new_rect.height();
        new_rect = windowing::Rect{txn.x, txn.y, txn.x + w, txn.y + h};
    }
    if ((txn.flags & SWP_NOSIZE) == 0) {
        new_rect.right = new_rect.left + std::max(txn.cx, 0);
        new_rect.bottom = new_rect.top + std::max(txn.cy, 0);
    }
    if (hwnd == m_virtual_fullscreen_hwnd &&
        m_virtual_display_width > 0 && m_virtual_display_height > 0) {
        new_rect = {0, 0, m_virtual_display_width, m_virtual_display_height};
    }
    txn.new_rect = new_rect;
    txn.outer_moved = new_rect.left != txn.old_rect.left || new_rect.top != txn.old_rect.top ||
                      txn.reparented;
    txn.outer_sized = new_rect.width() != txn.old_rect.width() ||
                      new_rect.height() != txn.old_rect.height();
    if (txn.outer_sized || (txn.flags & SWP_FRAMECHANGED) != 0) {
        recalculate_client_area(hwnd, new_rect, txn.old_rect, txn.after, txn.flags);
    }
    // Placement callbacks may destroy the window; native Windows still
    // reports successful completion without touching a replacement HWND.
    win = m_hwnd_mgr.find(hwnd);
    if (!win) return false;
    win->rect = new_rect;

    // Publish the identity alongside the geometry (hwnd_registry_publish
    // treats an empty class/title as "unchanged"); a window whose first
    // publish comes from a placement would otherwise stay anonymous.
    ipc::hwnd_registry_publish(
        static_cast<u32>(hwnd),
        win->client_rect.right - win->client_rect.left,
        win->client_rect.bottom - win->client_rect.top,
        win->owner_thread_id,
        wstring_to_utf8(win->class_name),
        wstring_to_utf8(win->title));

    // A no-op placement must not force a synchronous AppKit frame update and
    // repaint. A minimized window's parked rectangle is USER32 state only: the
    // host shows it miniaturized in the Dock (.docs/windowing-native-
    // presentation.md), so the frame is not pushed while minimized.
    if ((txn.outer_moved || txn.outer_sized || (txn.flags & SWP_FRAMECHANGED)) &&
        !win->minimized && hwnd != m_applying_native_move && !txn.host_geometry_applied) {
        if (m_host.has_native_window(hwnd)) {
            // While this window is the virtual full-screen target the guest
            // believes the display *is* its chosen mode; keep the native
            // window covering the display and let the presenter scale.
            if (hwnd != m_virtual_fullscreen_hwnd) {
                const i32 cw = win->client_rect.right - win->client_rect.left;
                const i32 ch = win->client_rect.bottom - win->client_rect.top;
                // The host frame is the CLIENT area in screen coordinates.
                const i32 fx = new_rect.left + win->client_offset.x;
                const i32 fy = new_rect.top + win->client_offset.y;
                m_host.set_frame(hwnd, windowing::Rect{fx, fy, fx + cw, fy + ch});
            }
        } else {
            (void)m_host.set_control_frame(hwnd, new_rect);
        }
    }

    if ((txn.flags & SWP_NOZORDER) == 0) {
        (void)m_hwnd_mgr.set_z_order(hwnd, txn.after);
        (void)m_host.set_control_z_order(hwnd, txn.after);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return false;
        txn.z_changed = m_hwnd_mgr.enum_children(
                            win->parent == windowing::NULL_HWND ? windowing::HWND_DESKTOP
                                                                : win->parent) != txn.old_siblings ||
                        ((win->ex_style ^ txn.old_ex) & 0x8u) != 0;
    }

    if (txn.flags & SWP_SHOWWINDOW) {
        win->visible = true;
        win->style |= windowing::WS::VISIBLE;
        if (!txn.was_visible) {
            // SWP_NOACTIVATE shows must not make the native window key
            // (Learn SetWindowPos: "does not activate the window").
            if ((txn.flags & SWP_NOACTIVATE) != 0) m_host.show_without_activation(hwnd);
            else m_host.set_visible(hwnd, true);
        }
    } else if (txn.flags & SWP_HIDEWINDOW) {
        win->visible = false;
        win->style &= ~windowing::WS::VISIBLE;
        if (txn.was_visible) m_host.set_visible(hwnd, false);
    }
    if (win->visible != txn.was_visible) {
        ipc::hwnd_registry_set_visible(static_cast<u32>(hwnd), win->visible);
    }
    return true;
}

void User32ExtManager::swp_finish(SwpTransaction& txn) {
    const windowing::HWND hwnd = txn.hwnd;
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return;
    const bool now_effectively_visible = effectively_visible(hwnd);

    // WinEvents (measured, tools/oracle_probe/winevent_sequence_probe):
    // SHOW/HIDE on a visibility change, else LOCATIONCHANGE (0x800B, not
    // STATECHANGE 0x800A) on a move or size, then REORDER on the parent
    // (OBJID_CLIENT; the desktop for top-level windows) when the sibling
    // order changed. They precede the activation's FOREGROUND/FOCUS events.
    {
        constexpr u32 EVENT_OBJECT_SHOW = 0x8002u;
        constexpr u32 EVENT_OBJECT_HIDE = 0x8003u;
        constexpr u32 EVENT_OBJECT_REORDER = 0x8004u;
        constexpr u32 EVENT_OBJECT_LOCATIONCHANGE = 0x800Bu;
        constexpr i32 OBJID_CLIENT = -4;
        const windowing::HWND parent =
            win->parent == windowing::NULL_HWND ? windowing::HWND_DESKTOP : win->parent;
        if (win->visible != txn.was_visible) {
            notify_win_event(win->visible ? EVENT_OBJECT_SHOW : EVENT_OBJECT_HIDE, hwnd);
        } else if (txn.outer_moved || txn.outer_sized) {
            notify_win_event(EVENT_OBJECT_LOCATIONCHANGE, hwnd);
        }
        if (txn.z_changed) notify_win_event(EVENT_OBJECT_REORDER, parent, OBJID_CLIENT);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
    }

    // Activation happens inside the transaction, before WM_WINDOWPOSCHANGED
    // (measured, s1win_msgseq2_probe showwindow_show_popup). A child gets
    // WM_CHILDACTIVATE instead (swp_child_show_hidden_parent).
    // The choice follows WS_CHILD, not the parent: a WS_CHILD window moved
    // to the desktop by SetParent still gets WM_CHILDACTIVATE (measured,
    // s1win_msgseq_probe setparent_child_to_desktop).
    if ((txn.flags & (SWP_NOACTIVATE | SWP_HIDEWINDOW)) == 0 && win->enabled) {
        if ((win->style & windowing::WS::CHILD) == 0) {
            if (std::getenv("VITREO_FOCUS_DIAG")) {
                std::fprintf(stderr,
                    "[focus] window-pos activation hwnd=0x%x flags=0x%x style=0x%x ex=0x%x visible=%d rect=(%d,%d,%d,%d)\n",
                    hwnd, txn.flags, win->style, win->ex_style,
                    static_cast<int>(win->visible), win->rect.left, win->rect.top,
                    win->rect.right, win->rect.bottom);
            }
            if (win->visible) (void)change_active_window(hwnd, windowing::WA::ACTIVE);
        } else if (txn.send) {
            swp_notify(txn, WM_CHILDACTIVATE, 0, 0);
        }
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
    }


    // Newly visible or newly exposed client pixels are invalid. SWP_NOREDRAW
    // leaves repainting to the caller; SWP_NOCOPYBITS discards the old
    // contents. Windows erases them synchronously: WM_NCPAINT then
    // WM_ERASEBKGND before WM_WINDOWPOSCHANGED (measured,
    // swp_showwindow_noactivate / showwindow_maximize).
    // A minimized window has no client area to erase (measured: no
    // WM_NCPAINT / WM_ERASEBKGND in showwindow_minimize_activating).
    bool exposed = false;
    if (m_paint_mgr && now_effectively_visible && (txn.flags & SWP_NOREDRAW) == 0 &&
        !win->minimized) {
        // Learn Window Class Styles: CS_HREDRAW/CS_VREDRAW discard the
        // entire client on a change of that dimension, including shrinking.
        const bool class_redraw =
            ((win->class_style & CS::HREDRAW) &&
             win->client_rect.width() != txn.old_client.width()) ||
            ((win->class_style & CS::VREDRAW) &&
             win->client_rect.height() != txn.old_client.height());
        if (!txn.was_effectively_visible || (txn.flags & SWP_NOCOPYBITS) || class_redraw) {
            m_paint_mgr->invalidate_rect(hwnd, nullptr, true);
            exposed = true;
            if (!txn.was_effectively_visible) {
                // Becoming visible exposes the visible children too: each
                // gets WM_PAINT, as with ShowWindow (show_window.cpp
                // invalidate_visible_descendants). VCL shows a page's
                // children before the page; without this they never painted
                // (Inno Setup's white MainPanel stayed window-grey).
                std::vector<windowing::HWND> pending = m_hwnd_mgr.enum_children(hwnd);
                while (!pending.empty()) {
                    const windowing::HWND child = pending.back();
                    pending.pop_back();
                    const windowing::WindowData* data = m_hwnd_mgr.find(child);
                    if (data == nullptr || !data->visible) continue;
                    m_paint_mgr->invalidate_rect(static_cast<u64>(child), nullptr, true);
                    for (windowing::HWND grandchild : m_hwnd_mgr.enum_children(child)) {
                        pending.push_back(grandchild);
                    }
                }
            }
        } else {
            const i32 width = win->client_rect.width();
            const i32 height = win->client_rect.height();
            const i32 old_width = std::max(0, txn.old_client.width());
            const i32 old_height = std::max(0, txn.old_client.height());
            if (width > old_width) {
                const Rect r{old_width, 0, width, height};
                m_paint_mgr->invalidate_rect(hwnd, &r, true);
                exposed = true;
            }
            if (height > old_height) {
                const Rect r{0, old_height, std::min(width, old_width), height};
                m_paint_mgr->invalidate_rect(hwnd, &r, true);
                exposed = true;
            }
        }
    }
    // A child window that appears is erased by its parent at once, while
    // its own erase waits for its WM_PAINT (measured,
    // setparent_between_visible_parents: the new parent's WM_ERASEBKGND and
    // no WM_NCPAINT / WM_ERASEBKGND to the child).
    const bool child_appeared = exposed && !top_level(*win) &&
                                (win->style & windowing::WS::CHILD) != 0 &&
                                !txn.was_effectively_visible;
    if (child_appeared && txn.send) {
        const windowing::HWND parent = win->parent;
        const windowing::WindowData* p = m_hwnd_mgr.find(parent);
        if (p && (p->style & windowing::WS::CLIPCHILDREN) == 0) {
            const Rect r{win->rect.left, win->rect.top, win->rect.right, win->rect.bottom};
            m_paint_mgr->invalidate_rect(parent, &r, true);
            (void)m_paint_mgr->erase_now(parent);
        }
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
    } else if (exposed && txn.send && m_msgsend_mgr && win->wndproc != 0) {
        swp_notify(txn, WM_NCPAINT, 1, 0);
        if (!m_hwnd_mgr.find(hwnd)) return;
        (void)m_paint_mgr->erase_now(hwnd);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
    }

    // Hiding a child exposes its parent, which erases the uncovered area
    // at once (measured, s1win_msgseq2_probe setparent_between_visible_parents:
    // the parent's WM_ERASEBKGND comes before the child's
    // WM_WINDOWPOSCHANGED).
    if (m_paint_mgr && txn.was_effectively_visible && !now_effectively_visible &&
        (txn.flags & SWP_HIDEWINDOW) != 0 && !top_level(*win) &&
        (txn.flags & SWP_NOREDRAW) == 0 && effectively_visible(win->parent)) {
        const windowing::HWND parent = win->parent;
        const Rect r{txn.old_rect.left, txn.old_rect.top, txn.old_rect.right, txn.old_rect.bottom};
        m_paint_mgr->invalidate_rect(parent, &r, true);
        if (txn.send) (void)m_paint_mgr->erase_now(parent);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
    }

    // WM_WINDOWPOSCHANGED describes the FINAL state.
    const windowing::Rect& new_rect = txn.new_rect;
    const bool client_moved =
        txn.reparented ||
        txn.old_rect.left + txn.old_client_offset.x != new_rect.left + win->client_offset.x ||
        txn.old_rect.top + txn.old_client_offset.y != new_rect.top + win->client_offset.y;
    const bool client_sized = txn.old_client.width() != win->client_rect.width() ||
                              txn.old_client.height() != win->client_rect.height();
    u32 changed_flags = (txn.flags & ~(SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE |
                                       0u)) |
        (client_moved ? 0u : SWP_NOCLIENTMOVE) | (client_sized ? 0u : SWP_NOCLIENTSIZE);
    // SWP_SHOWWINDOW / SWP_HIDEWINDOW stay only when visibility changed
    // (measured, showwindow_minimize_activating: CHANGING 0x8174, CHANGED
    // 0x8134 on an already visible window).
    if (txn.was_visible == win->visible) changed_flags &= ~(SWP_SHOWWINDOW | SWP_HIDEWINDOW);
    if (!txn.z_changed) changed_flags |= SWP_NOZORDER;
    if (!txn.was_effectively_visible && !now_effectively_visible) changed_flags |= SWP_NOREDRAW;
    const bool changed = txn.outer_moved || txn.outer_sized ||
                         txn.was_visible != win->visible ||
                         (txn.flags & SWP_FRAMECHANGED) || (txn.flags & SWP_STATECHANGED) ||
                         txn.z_changed;
    if (changed && m_windowpos_scratch != 0) {
        const GuestAddr wp_changed_addr = m_windowpos_scratch + 0x40;
        WindowPosLayout{m_is_64bit}.write(m_memory, wp_changed_addr, hwnd, txn.after,
            new_rect.left, new_rect.top, new_rect.width(), new_rect.height(), changed_flags);
        swp_notify(txn, windowing::WM::WINDOWPOSCHANGED, 0, wp_changed_addr);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return;
    }

    // A guest WndProc receives WM_MOVE/WM_SIZE only by forwarding
    // WM_WINDOWPOSCHANGED to DefWindowProc. Keep the queue-only fallback for
    // native test/control windows that have no guest procedure.
    const bool default_notifications = win->wndproc == 0 || !m_msgsend_mgr;
    if (changed && default_notifications) {
        if (client_moved) {
            swp_notify(txn, windowing::WM::MOVE, 0, static_cast<u64>(
                (new_rect.left & 0xFFFF) | ((new_rect.top & 0xFFFF) << 16)));
        }
        if (client_sized) {
            const i32 cw = win->client_rect.right - win->client_rect.left;
            const i32 ch = win->client_rect.bottom - win->client_rect.top;
            swp_notify(txn, windowing::WM::SIZE, 0,
                       static_cast<u64>((cw & 0xFFFF) | ((ch & 0xFFFF) << 16)));
        }
    }

    // Hiding the active window moves activation elsewhere after the hide
    // (measured, showwindow_hide_visible_popup: WM_NCACTIVATE / WM_ACTIVATE /
    // WM_ACTIVATEAPP / WM_KILLFOCUS follow WM_WINDOWPOSCHANGED).
    if ((txn.flags & SWP_HIDEWINDOW) && txn.was_visible) {
        release_activation(hwnd, false);
    }

    VITREO_TRACE("[user32_ext] SetWindowPos(hwnd=0x%x after=0x%x x=%d y=%d cx=%d cy=%d flags=0x%x) → TRUE\n",
                 hwnd, txn.after, txn.x, txn.y, txn.cx, txn.cy, txn.flags);
}

// ---------------------------------------------------------------------------
// ShowWindow
// ---------------------------------------------------------------------------

windowing::Rect User32ExtManager::maximized_rect(windowing::HWND hwnd) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return {};
    // A child maximizes into its parent's client area (measured,
    // showwindow_child_maximize).
    if (!top_level(*win)) {
        const windowing::WindowData* parent = m_hwnd_mgr.find(win->parent);
        const i32 border = win->client_offset.x;
        const i32 w = (parent ? parent->client_rect.width() : win->rect.width()) + 2 * border;
        const i32 h = (parent ? parent->client_rect.height() : win->rect.height()) + 2 * border;
        windowing::Rect r{-border, -border, w - border, h - border};
        // A child maximizing gets WM_GETMINMAXINFO too (measured,
        // showwindow_child_maximize); ptMaxPosition is in parent-client
        // coordinates.
        if (m_msgsend_mgr && win->wndproc != 0) {
            auto frame = m_thread_scratch.reserve(40);
            const GuestAddr mmi = frame.address();
            if (mmi != 0) {
                i32 min_track_w = 0, min_track_h = 0;
                default_min_track_size(win->style, win->ex_style, min_track_w, min_track_h);
                const i32 values[10] = {0, 0, w, h, -border, -border,
                                        min_track_w, min_track_h, w, h};
                for (u32 i = 0; i < 10; ++i) (void)m_memory.write_u32(mmi + i * 4, static_cast<u32>(values[i]));
                (void)m_msgsend_mgr->send_message(hwnd, windowing::WM::GETMINMAXINFO, 0, mmi);
                const i32 mw = static_cast<i32>(m_memory.read_u32(mmi + 8).value_or(static_cast<u32>(w)));
                const i32 mh = static_cast<i32>(m_memory.read_u32(mmi + 12).value_or(static_cast<u32>(h)));
                const i32 px = static_cast<i32>(m_memory.read_u32(mmi + 16).value_or(static_cast<u32>(-border)));
                const i32 py = static_cast<i32>(m_memory.read_u32(mmi + 20).value_or(static_cast<u32>(-border)));
                r = windowing::Rect{px, py, px + mw, py + mh};
            }
        }
        return r;
    }
    // Top level: the work area of the monitor holding the window (Learn:
    // maximize fills the work area; .docs/windowing-native-presentation.md
    // "Maximize = visible work area"), with the frame hanging outside it so
    // the client area — and a guest-drawn frame's client area — fills it.
    const auto monitors = m_host.get_all_monitor_info();
    windowing::MonitorInfo chosen{};
    bool found = false;
    const i32 cx = (win->normal_rect.left + win->normal_rect.right) / 2;
    const i32 cy = (win->normal_rect.top + win->normal_rect.bottom) / 2;
    for (const auto& mon : monitors) {
        if (cx >= mon.left && cx < mon.right && cy >= mon.top && cy < mon.bottom) {
            chosen = mon;
            found = true;
            break;
        }
    }
    if (!found) {
        for (const auto& mon : monitors) {
            if (mon.primary) { chosen = mon; found = true; break; }
        }
    }
    if (!found) chosen = m_host.get_primary_monitor_info();
    const i32 border = win->client_offset.x;
    windowing::Rect r{chosen.work_left - border, chosen.work_top - border,
                      chosen.work_right + border, chosen.work_bottom + border};

    // WM_GETMINMAXINFO: the procedure may move or resize the maximized
    // rectangle (Learn: MINMAXINFO ptMaxPosition / ptMaxSize, relative to
    // the monitor). Sent before the state change (measured, showwindow_maximize).
    if (m_msgsend_mgr && win->wndproc != 0) {
        auto frame = m_thread_scratch.reserve(40);
        const GuestAddr mmi = frame.address();
        if (mmi != 0) {
            i32 min_track_w = 0, min_track_h = 0;
            default_min_track_size(win->style, win->ex_style, min_track_w, min_track_h);
            const i32 values[10] = {
                0, 0,
                r.width(), r.height(),
                r.left - chosen.left, r.top - chosen.top,
                min_track_w, min_track_h,
                chosen.right - chosen.left + 2 * border + 12,
                chosen.bottom - chosen.top + 2 * border + 12};
            for (u32 i = 0; i < 10; ++i) (void)m_memory.write_u32(mmi + i * 4, static_cast<u32>(values[i]));
            (void)m_msgsend_mgr->send_message(hwnd, windowing::WM::GETMINMAXINFO, 0, mmi);
            const i32 w = static_cast<i32>(m_memory.read_u32(mmi + 8).value_or(static_cast<u32>(r.width())));
            const i32 h = static_cast<i32>(m_memory.read_u32(mmi + 12).value_or(static_cast<u32>(r.height())));
            const i32 px = static_cast<i32>(m_memory.read_u32(mmi + 16).value_or(0));
            const i32 py = static_cast<i32>(m_memory.read_u32(mmi + 20).value_or(0));
            r = windowing::Rect{chosen.left + px, chosen.top + py,
                                chosen.left + px + w, chosen.top + py + h};
        }
    }
    return r;
}

u64 User32ExtManager::show_window_cmd(windowing::HWND hwnd, int cmd) {
    namespace SW = windowing::SW;
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    // Measured (showwindow_invalid_cmd): an unknown command is
    // ERROR_INVALID_PARAMETER and nothing is sent.
    if (cmd < SW::HIDE || cmd > SW::FORCEMINIMIZE) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    // Measured: ShowWindow reports a previously visible window as 0x18.
    constexpr u64 kWasVisible = 0x18;
    const bool prev_visible = (win->style & windowing::WS::VISIBLE) != 0;
    const u64 result = prev_visible ? kWasVisible : 0;
    const bool is_active = m_active_window == hwnd;
    const bool is_top = top_level(*win);

    auto send = [&](u32 msg, u64 w, u64 l) {
        if (m_msgsend_mgr && win->wndproc != 0) {
            (void)m_msgsend_mgr->send_message(hwnd, msg, w, l);
        }
        win = m_hwnd_mgr.find(hwnd);
    };
    auto send_first_size_move = [&] {
        if (!win || !win->pending_size_move) return;
        win->pending_size_move = false;
        const u32 kind = win->minimized ? 1u : win->maximized ? 2u : 0u;
        const auto origin = win->rect;
        send(windowing::WM::SIZE, kind, static_cast<u64>(
            (win->client_rect.width() & 0xFFFF) | ((win->client_rect.height() & 0xFFFF) << 16)));
        if (!win) return;
        const i32 mx = origin.left + win->client_offset.x;
        const i32 my = origin.top + win->client_offset.y;
        send(windowing::WM::MOVE, 0, static_cast<u64>((mx & 0xFFFF) | ((my & 0xFFFF) << 16)));
    };
    auto set_state = [&](bool minimized, bool maximized) {
        // Measured (winevent_sequence_probe): EVENT_SYSTEM_MINIMIZESTART /
        // MINIMIZEEND as a window enters / leaves the minimized state.
        if (minimized != win->minimized) {
            notify_win_event(minimized ? 0x0016u : 0x0017u, win->handle);
        }
        win->minimized = minimized;
        win->maximized = !minimized && maximized;
        win->style &= ~(windowing::WS::MINIMIZE | windowing::WS::MAXIMIZE);
        if (win->minimized) win->style |= windowing::WS::MINIMIZE;
        else if (win->maximized) win->style |= windowing::WS::MAXIMIZE;
    };
    auto remember_normal = [&] {
        if (!win->minimized && !win->maximized) {
            win->normal_rect = win->rect;
            win->normal_client_rect = win->client_rect;
        }
    };

    // A child whose parent chain is hidden only has WS_VISIBLE toggled after
    // WM_SHOWWINDOW: no WM_WINDOWPOSCHANGING/CHANGED (measured,
    // s1win_msgseq_probe child_showwindow_hide / child_showwindow_show).
    if (!is_top && !effectively_visible(win->parent) &&
        (cmd == SW::HIDE || cmd == SW::SHOW || cmd == SW::SHOWNA ||
         cmd == SW::SHOWNOACTIVATE || cmd == SW::SHOWNORMAL || cmd == SW::SHOWDEFAULT) &&
        !win->minimized && !win->maximized) {
        const bool show = cmd != SW::HIDE;
        if (show == prev_visible && cmd != SW::SHOWNA && cmd != SW::SHOWNOACTIVATE) return result;
        send(windowing::WM::SHOWWINDOW, show ? 1u : 0u, 0);
        if (!win) return result;
        win->visible = show;
        if (show) win->style |= windowing::WS::VISIBLE;
        else win->style &= ~windowing::WS::VISIBLE;
        if (show != prev_visible) m_host.set_visible(hwnd, show);
        return result;
    }

    switch (cmd) {
    case SW::HIDE: {
        if (!prev_visible) return 0;  // measured: nothing sent
        send(windowing::WM::SHOWWINDOW, 0, 0);
        if (!win) return result;
        const u32 flags = SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE |
                          (is_active ? 0u : (SWP_NOACTIVATE | SWP_NOZORDER));
        (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, 0, 0, 0, 0, flags, true);
        return result;
    }
    case SW::MINIMIZE:
    case SW::SHOWMINIMIZED:
    case SW::SHOWMINNOACTIVE:
    case SW::FORCEMINIMIZE: {
        if (win->minimized && prev_visible) return result;
        if (!prev_visible) {
            send(windowing::WM::SHOWWINDOW, 1, 0);
            if (!win) return result;
        }
        // Minimizing the focused window removes the keyboard focus first
        // (measured, showwindow_minimize_activating: WM_KILLFOCUS).
        if (is_active && cmd != SW::SHOWMINNOACTIVE) {
            if (m_focus_hwnd != windowing::NULL_HWND) {
                (void)set_keyboard_focus(m_focus_hwnd, false);
            } else {
                send(WM_KILLFOCUS, 0, 0);
            }
            win = m_hwnd_mgr.find(hwnd);
            if (!win) return result;
        }
        remember_normal();
        win->restore_to_maximized = win->maximized;
        set_state(true, false);
        win->minimized_position = {kMinimizedParked, kMinimizedParked};
        m_host.set_minimized(hwnd, true);
        u32 flags = SWP_STATECHANGED | SWP_NOCOPYBITS | SWP_SHOWWINDOW | SWP_FRAMECHANGED;
        if (cmd != SW::SHOWMINIMIZED) flags |= SWP_NOACTIVATE;
        if (cmd == SW::MINIMIZE || cmd == SW::FORCEMINIMIZE) flags |= SWP_NOZORDER;
        (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, kMinimizedParked, kMinimizedParked,
                                   kMinimizedWidth, kMinimizedHeight, flags, true);
        owned_windows_follow_owner(hwnd, /*owner_minimized=*/true);
        win = m_hwnd_mgr.find(hwnd);
        if (win) send_first_size_move();
        return result;
    }
    case SW::SHOWMAXIMIZED: {
        if (win->maximized && prev_visible) return result;
        if (!prev_visible) {
            send(windowing::WM::SHOWWINDOW, 1, 0);
            if (!win) return result;
        }
        const bool was_min = win->minimized;
        if (was_min) {
            send(WM_QUERYOPEN, 0, 0);
            if (!win) return result;
        }
        remember_normal();
        const windowing::Rect r = maximized_rect(hwnd);
        win = m_hwnd_mgr.find(hwnd);
        if (!win) return result;
        set_state(false, true);
        win->restore_to_maximized = false;
        if (was_min) m_host.set_minimized(hwnd, false);
        u32 flags = SWP_STATECHANGED | SWP_FRAMECHANGED | (prev_visible ? 0u : SWP_SHOWWINDOW);
        (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, r.left, r.top, r.width(), r.height(),
                                   flags, true);
        owned_windows_follow_owner(hwnd, /*owner_minimized=*/false);
        win = m_hwnd_mgr.find(hwnd);
        if (win) send_first_size_move();
        return result;
    }
    default:
        break;
    }

    // The showing commands: SW_SHOWNORMAL, SW_SHOWNOACTIVATE, SW_SHOW,
    // SW_SHOWNA, SW_RESTORE, SW_SHOWDEFAULT.
    const bool activate = cmd == SW::SHOWNORMAL || cmd == SW::SHOW ||
                          cmd == SW::RESTORE || cmd == SW::SHOWDEFAULT;
    const bool restores = cmd == SW::SHOWNORMAL || cmd == SW::SHOWNOACTIVATE ||
                          cmd == SW::RESTORE || cmd == SW::SHOWDEFAULT;
    const bool leaves_state = win->minimized || win->maximized;
    if (restores && leaves_state) {
        if (!prev_visible) {
            send(windowing::WM::SHOWWINDOW, 1, 0);
            if (!win) return result;
        }
        const bool was_min = win->minimized;
        if (was_min) {
            // Learn WM_QUERYOPEN: sent to an icon when the user requests
            // that it be restored.
            send(WM_QUERYOPEN, 0, 0);
            if (!win) return result;
        }
        if (was_min && win->restore_to_maximized && cmd == SW::RESTORE) {
            const windowing::Rect r = maximized_rect(hwnd);
            win = m_hwnd_mgr.find(hwnd);
            if (!win) return result;
            set_state(false, true);
            m_host.set_minimized(hwnd, false);
            (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, r.left, r.top, r.width(),
                                       r.height(), SWP_STATECHANGED | SWP_FRAMECHANGED |
                                       SWP_NOCOPYBITS | (activate ? 0u : SWP_NOACTIVATE), true);
        } else {
            const windowing::Rect r = win->normal_rect;
            set_state(false, false);
            win->restore_to_maximized = false;
            if (was_min) m_host.set_minimized(hwnd, false);
            (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, r.left, r.top, r.width(),
                                       r.height(), SWP_STATECHANGED | SWP_FRAMECHANGED |
                                       (was_min ? SWP_NOCOPYBITS : 0u) |
                                       (prev_visible ? 0u : SWP_SHOWWINDOW) |
                                       (activate ? 0u : SWP_NOACTIVATE), true);
        }
        owned_windows_follow_owner(hwnd, /*owner_minimized=*/false);
        win = m_hwnd_mgr.find(hwnd);
        if (win) send_first_size_move();
        // Restoring the active window gives it the keyboard focus back and
        // re-sends WM_ACTIVATE(WA_ACTIVE) without the minimized flag
        // (measured, showwindow_shownormal_from_min / openicon: WM_SETFOCUS
        // then WM_ACTIVATE after WM_SIZE).
        if (win && was_min && activate && m_active_window == hwnd) {
            if (m_focus_hwnd == windowing::NULL_HWND) (void)set_keyboard_focus(hwnd, true);
            win = m_hwnd_mgr.find(hwnd);
            if (win) send(windowing::WM::ACTIVATE, windowing::WA::ACTIVE, 0);
        }
        return result;
    }

    if (prev_visible) {
        // Already visible: SW_SHOW / SW_SHOWNORMAL do nothing; SW_SHOWNA and
        // SW_SHOWNOACTIVATE repeat WM_SHOWWINDOW and a no-change
        // WM_WINDOWPOSCHANGING (measured, showwindow_showna_again /
        // showwindow_show_again).
        if (cmd == SW::SHOWNA || cmd == SW::SHOWNOACTIVATE) {
            send(windowing::WM::SHOWWINDOW, 1, 0);
            if (!win) return result;
            (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, 0, 0, 0, 0,
                                       SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE,
                                       true);
        }
        return result;
    }

    send(windowing::WM::SHOWWINDOW, 1, 0);
    if (!win) return result;
    // Measured: SW_SHOWNOACTIVATE 0x57 (showwindow_shownoactivate), SW_SHOWNA
    // 0x53 (overlapped_swp_then_first_show), SW_SHOW 0x43
    // (create_overlapped_plain_visible); a WS_CHILD window adds
    // SWP_NOACTIVATE | SWP_NOZORDER (setparent_child_to_desktop).
    const bool child_style = (win->style & windowing::WS::CHILD) != 0;
    u32 flags = SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
    if (!activate || child_style) flags |= SWP_NOACTIVATE;
    if (cmd == SW::SHOWNOACTIVATE || child_style) flags |= SWP_NOZORDER;
    // A window created minimized and hidden is shown minimized.
    if (win->minimized) m_host.set_minimized(hwnd, true);
    (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, 0, 0, 0, 0, flags, true);
    win = m_hwnd_mgr.find(hwnd);
    if (win) send_first_size_move();
    return result;
}

void User32ExtManager::apply_initial_state(windowing::HWND hwnd, bool minimize) {
    // CreateWindowEx with WS_MINIMIZE / WS_MAXIMIZE: after WM_CREATE, one
    // SetWindowPos into the state (measured, s1win_msgseq2_probe
    // create_popup_minimize_hidden / create_overlapped_minimize_hidden:
    // flags SWP_STATECHANGED | SWP_NOCOPYBITS | SWP_FRAMECHANGED |
    // SWP_NOACTIVATE, the 160x28 icon at the bottom-left of the work area;
    // create_popup_maximize_hidden / s1win_msgseq_probe
    // create_maximized_hidden: WM_GETMINMAXINFO, then SWP_STATECHANGED |
    // SWP_FRAMECHANGED | SWP_NOACTIVATE).
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return;
    win->normal_rect = win->rect;
    win->normal_client_rect = win->client_rect;
    if (minimize) {
        win->minimized = true;
        win->maximized = false;
        win->style = (win->style & ~windowing::WS::MAXIMIZE) | windowing::WS::MINIMIZE;
        const windowing::MonitorInfo mon = m_host.get_primary_monitor_info();
        const i32 x = mon.work_left;
        const i32 y = mon.work_bottom - kMinimizedHeight;
        win->minimized_position = {x, y};
        (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, x, y, kMinimizedWidth,
                                   kMinimizedHeight,
                                   SWP_STATECHANGED | SWP_NOCOPYBITS | SWP_FRAMECHANGED |
                                   SWP_NOACTIVATE, true);
        return;
    }
    const windowing::Rect r = maximized_rect(hwnd);
    win = m_hwnd_mgr.find(hwnd);
    if (!win) return;
    win->maximized = true;
    win->minimized = false;
    win->style = (win->style & ~windowing::WS::MINIMIZE) | windowing::WS::MAXIMIZE;
    (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, r.left, r.top, r.width(), r.height(),
                               SWP_STATECHANGED | SWP_FRAMECHANGED | SWP_NOACTIVATE, true);
}

void User32ExtManager::owned_windows_follow_owner(windowing::HWND owner,
                                                  bool owner_minimized) {
    // Learn (Window Features, "Owned Windows"): "An owned window is hidden
    // when its owner is minimized." WM_SHOWWINDOW carries SW_PARENTCLOSING (1)
    // or SW_PARENTOPENING (3) in lParam (Learn: WM_SHOWWINDOW).
    constexpr u64 kSwParentClosing = 1;
    constexpr u64 kSwParentOpening = 3;
    for (const windowing::HWND h : m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
        windowing::WindowData* w = m_hwnd_mgr.find(h);
        if (!w || w->owner != owner || h == owner) continue;
        const bool hide = owner_minimized && w->visible;
        const bool show = !owner_minimized && w->hidden_by_owner;
        if (!hide && !show) continue;
        if (m_msgsend_mgr && w->wndproc != 0) {
            (void)m_msgsend_mgr->send_message(h, windowing::WM::SHOWWINDOW, show ? 1u : 0u,
                                              show ? kSwParentOpening : kSwParentClosing);
        }
        w = m_hwnd_mgr.find(h);
        if (!w) continue;
        w->hidden_by_owner = hide;
        (void)apply_set_window_pos(h, windowing::HWND_TOP, 0, 0, 0, 0,
                                   (hide ? SWP_HIDEWINDOW : SWP_SHOWWINDOW) | SWP_NOSIZE |
                                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE, true);
    }
}

// BOOL CloseWindow(HWND hWnd) — Learn: "Minimizes (but does not destroy)
// the specified window." BOOL OpenIcon(HWND hWnd) — restores a minimized
// window. Measured (s1win_msgseq2_probe closewindow / openicon): the same
// sequences as SW_SHOWMINIMIZED / SW_SHOWNORMAL; both return TRUE, also when
// there is nothing to do; an invalid window is ERROR_INVALID_WINDOW_HANDLE.
void User32ExtManager::impl_CloseWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    (void)show_window_cmd(hwnd, windowing::SW::SHOWMINIMIZED);
    m.set_return(1u);
}

void User32ExtManager::impl_OpenIcon(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    (void)show_window_cmd(hwnd, windowing::SW::SHOWNORMAL);
    m.set_return(1u);
}

// ---------------------------------------------------------------------------
// BeginDeferWindowPos / DeferWindowPos / EndDeferWindowPos
//
// Learn DeferWindowPos: the positions are applied together by
// EndDeferWindowPos. Measured (s1win_msgseq2_probe "-- defer"): every
// WM_WINDOWPOSCHANGING is sent first (in DeferWindowPos order), then each
// window's WM_NCCALCSIZE, WM_WINDOWPOSCHANGED and WM_MOVE/WM_SIZE; a negative
// count is ERROR_INVALID_PARAMETER; an unknown HDWP is ERROR_INVALID_DWP_HANDLE;
// an invalid window is ERROR_INVALID_WINDOW_HANDLE and leaves the HDWP usable;
// invalid flags are ERROR_INVALID_FLAGS.
// ---------------------------------------------------------------------------

void User32ExtManager::impl_BeginDeferWindowPos(Marshaler& m) {
    const i32 count = static_cast<i32>(m.arg(0));
    if (count < 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_defer_mutex);
    const u32 h = m_next_deferred_pos++;
    m_deferred[h] = {};
    m_deferred[h].reserve(static_cast<usize>(std::min(count, 64)));
    m.set_return(h);
}

void User32ExtManager::impl_DeferWindowPos(Marshaler& m) {
    const u32 hdwp = static_cast<u32>(m.arg(0));
    const auto hwnd = static_cast<windowing::HWND>(m.arg(1));
    const auto after = static_cast<windowing::HWND>(m.arg(2));
    const i32 x = static_cast<i32>(m.arg(3));
    const i32 y = static_cast<i32>(m.arg(4));
    const i32 cx = static_cast<i32>(m.arg(5));
    const i32 cy = static_cast<i32>(m.arg(6));
    const u32 flags = static_cast<u32>(m.arg(7));
    std::lock_guard lock(m_defer_mutex);
    auto it = m_deferred.find(hdwp);
    if (it == m_deferred.end()) {
        set_last_error(error_codes::ERROR_INVALID_DWP_HANDLE);
        m.set_return(0u);
        return;
    }
    if ((flags & ~kValidSetWindowPosFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    // A second entry for the same window replaces the first.
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [hwnd](const DeferredPos& p) { return p.hwnd == hwnd; }),
               list.end());
    list.push_back(DeferredPos{hwnd, after, x, y, cx, cy, flags});
    m.set_return(hdwp);
}

void User32ExtManager::impl_EndDeferWindowPos(Marshaler& m) {
    const u32 hdwp = static_cast<u32>(m.arg(0));
    std::vector<DeferredPos> list;
    {
        std::lock_guard lock(m_defer_mutex);
        auto it = m_deferred.find(hdwp);
        if (it == m_deferred.end()) {
            set_last_error(error_codes::ERROR_INVALID_DWP_HANDLE);
            m.set_return(0u);
            return;
        }
        list = std::move(it->second);
        m_deferred.erase(it);
    }
    // Measured (s1win_msgseq2_probe end_mixed): windows with different
    // parents make EndDeferWindowPos return TRUE with
    // ERROR_HWNDS_HAVE_DIFF_PARENT and move none of them.
    {
        std::optional<windowing::HWND> parent;
        for (const auto& p : list) {
            const windowing::WindowData* w = m_hwnd_mgr.find(p.hwnd);
            if (!w) continue;
            const windowing::HWND pp =
                w->parent == windowing::NULL_HWND ? windowing::HWND_DESKTOP : w->parent;
            if (parent && *parent != pp) {
                set_last_error(user32_errors::ERROR_HWNDS_HAVE_DIFF_PARENT);
                m.set_return(1u);
                return;
            }
            parent = pp;
        }
    }
    ScratchSave save(m_memory, m_windowpos_scratch);
    std::vector<SwpTransaction> txns;
    txns.reserve(list.size());
    for (const auto& p : list) {
        SwpTransaction txn{};
        txn.hwnd = p.hwnd;
        txn.after = p.after;
        txn.x = p.x; txn.y = p.y; txn.cx = p.cx; txn.cy = p.cy;
        txn.flags = p.flags;
        txn.send = true;
        txns.push_back(std::move(txn));
    }
    // Every WM_WINDOWPOSCHANGING first, each in its own WINDOWPOS (a later
    // procedure must not see an earlier window's edits), then the geometry
    // (WM_NCCALCSIZE) of every window, then each WM_WINDOWPOSCHANGED.
    for (auto& txn : txns) {
        if (!m_hwnd_mgr.is_valid(txn.hwnd)) { txn.dead = true; continue; }
        if (!swp_changing(txn)) txn.dead = true;
    }
    for (auto& txn : txns) {
        if (!txn.dead && !swp_apply(txn)) txn.dead = true;
    }
    for (auto& txn : txns) {
        if (!txn.dead) swp_finish(txn);
    }
    m.set_return(1u);
}

} // namespace vitreo::winapi
