/// @file show_window.cpp
/// @brief ShowWindow/UpdateWindow/IsWindowVisible/EnableWindow (WIN32-052)

#include "windowing/model/diag_flags.hpp"
#include "show_window.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include <unistd.h>
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/messages/message_queue.hpp"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <optional>

namespace vitreo::winapi {

static inline bool diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

static windowing::HWND resolve_zero_hwnd(windowing::HwndManager& hwnd_mgr,
                                         windowing::HWND hwnd) {
    if (hwnd != windowing::NULL_HWND) {
        return hwnd;
    }
    auto top = hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    if (!top.empty()) {
        return top.back();
    }
    return windowing::NULL_HWND;
}

// =============================================================================
// ShowWindowManager
// =============================================================================

ShowWindowManager::ShowWindowManager(
    windowing::HwndManager& hwnd_mgr,
    windowing::WindowHost& window_host,
    windowing::MessageQueue& queue
)
    : m_hwnd_mgr{hwnd_mgr}
    , m_window_host{&window_host}
    , m_control_host{window_host}
    , m_queue{queue}
{}

ShowWindowManager::ShowWindowManager(
    windowing::HwndManager& hwnd_mgr,
    windowing::ControlHost& control_host,
    windowing::MessageQueue& queue)
    : m_hwnd_mgr{hwnd_mgr}
    , m_window_host{nullptr}
    , m_control_host{control_host}
    , m_queue{queue}
{}

// ---------------------------------------------------------------------------
// send_or_post – synchronous send when possible, queue fallback otherwise
// ---------------------------------------------------------------------------
void ShowWindowManager::send_or_post(windowing::HWND hwnd, u64 wndproc,
                                     u32 msg, u64 wParam, u64 lParam) {
    if (m_msgsend_mgr && wndproc != 0) {
        if (diag_logs_enabled()) {
            std::fprintf(stderr,
                "[msg-diag] send_or_post SEND hwnd=0x%x msg=0x%x lParam=0x%llx\n",
                hwnd, msg, static_cast<unsigned long long>(lParam));
        }
        (void)m_msgsend_mgr->send_message(hwnd, msg, wParam, lParam);
        return;
    }
    if (diag_logs_enabled()) {
        std::fprintf(stderr,
            "[msg-diag] send_or_post POST hwnd=0x%x msg=0x%x lParam=0x%llx (no msgsend=%d wndproc=0x%llx)\n",
            hwnd, msg, static_cast<unsigned long long>(lParam),
            m_msgsend_mgr != nullptr, static_cast<unsigned long long>(wndproc));
    }
    windowing::MSG queued{};
    queued.hwnd    = hwnd;
    queued.message = msg;
    queued.wParam  = wParam;
    queued.lParam  = lParam;
    m_queue.post(queued);
}

void ShowWindowManager::invalidate_visible_descendants(
    windowing::HWND parent) {
    if (!m_paint_mgr) {
        return;
    }
    for (windowing::HWND child : m_hwnd_mgr.enum_children(parent)) {
        const windowing::WindowData* child_data = m_hwnd_mgr.find(child);
        if (!child_data || !child_data->visible) {
            continue;
        }
        m_paint_mgr->invalidate_rect(
            static_cast<u64>(child), nullptr, true);
        invalidate_visible_descendants(child);
    }
}

void ShowWindowManager::update_owned_popups_for_owner(windowing::HWND owner, bool show) {
    auto top = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    for (windowing::HWND hwnd : top) {
        windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
        if (!win || win->owner != owner) {
            continue;
        }

        if (show) {
            if (!win->hidden_by_owner) {
                continue;
            }
            win->hidden_by_owner = false;
            win->visible = true;
            m_window_host->set_visible(hwnd, true);

            windowing::MSG msg_show{};
            msg_show.hwnd    = hwnd;
            msg_show.message = windowing::WM::SHOWWINDOW;
            msg_show.wParam  = 1;
            m_queue.post(msg_show);
        } else {
            if (!win->visible) {
                continue;
            }
            win->hidden_by_owner = true;
            win->visible = false;
            m_window_host->set_visible(hwnd, false);

            windowing::MSG msg_show{};
            msg_show.hwnd    = hwnd;
            msg_show.message = windowing::WM::SHOWWINDOW;
            msg_show.wParam  = 0;
            m_queue.post(msg_show);
        }
    }
}

// -----------------------------------------------------------------------------
// show_window
// -----------------------------------------------------------------------------

u64 ShowWindowManager::show_window_result(windowing::HWND hwnd, int cmd_show) {
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr, "[input] ShowWindow hwnd=0x%x cmd=%d\n", hwnd, cmd_show);
    }
    // The USER32 manager owns the window-position engine: ShowWindow is a
    // WM_SHOWWINDOW plus a SetWindowPos transaction there (window_pos.cpp).
    if (m_user32_ext_mgr) return m_user32_ext_mgr->show_window_cmd(hwnd, cmd_show);
    return show_window(hwnd, cmd_show) ? 1u : 0u;
}

bool ShowWindowManager::show_window(windowing::HWND hwnd, int cmd_show) {
    if (m_user32_ext_mgr) {
        const windowing::WindowData* w = m_hwnd_mgr.find(hwnd);
        const bool prev = w && (w->style & windowing::WS::VISIBLE) != 0;
        (void)m_user32_ext_mgr->show_window_cmd(hwnd, cmd_show);
        return prev;
    }
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return false;
    }

    const bool prev_visible = win->visible;
    const bool prev_minimized = win->minimized;
    const bool prev_maximized = win->maximized;
    const bool activates =
        cmd_show == windowing::SW::SHOWNORMAL ||
        cmd_show == windowing::SW::SHOW ||
        cmd_show == windowing::SW::SHOWDEFAULT ||
        cmd_show == windowing::SW::SHOWMINIMIZED ||
        cmd_show == windowing::SW::SHOWMAXIMIZED ||
        cmd_show == windowing::SW::RESTORE;
    const bool activating_top_level =
        activates && win->enabled &&
        (win->parent == windowing::HWND_DESKTOP ||
         win->parent == windowing::NULL_HWND);

    // AppKit may synchronously report windowDidBecomeKey while the native
    // visibility operation below is still on the stack. USER32 owns the sent
    // activation transition, so reserve it first and prevent that callback
    // from turning WM_ACTIVATEAPP into delayed queue work.
    if (activating_top_level && m_user32_ext_mgr) {
        (void)m_window_host->exchange_app_active(true);
    }

    const auto set_visible_state = [&](bool visible) {
        win->visible = visible;
        if (visible) {
            win->style |= windowing::WS::VISIBLE;
        } else {
            win->style &= ~windowing::WS::VISIBLE;
        }
        m_window_host->set_visible(hwnd, visible);
    };

    const auto remember_normal_geometry = [&] {
        if (!win->minimized && !win->maximized) {
            win->normal_rect = win->rect;
            win->normal_client_rect = win->client_rect;
        }
    };

    const auto set_state_bits = [&](bool minimized, bool maximized) {
        // Measured (winevent_sequence_probe): EVENT_SYSTEM_MINIMIZESTART when
        // a window is minimized, EVENT_SYSTEM_MINIMIZEEND when it leaves the
        // minimized state.
        if (minimized != win->minimized && m_user32_ext_mgr != nullptr) {
            m_user32_ext_mgr->notify_win_event(minimized ? 0x0016u : 0x0017u, hwnd);
        }
        win->minimized = minimized;
        win->maximized = !minimized && maximized;
        win->style &= ~(windowing::WS::MINIMIZE |
                        windowing::WS::MAXIMIZE);
        if (win->minimized) {
            win->style |= windowing::WS::MINIMIZE;
        } else if (win->maximized) {
            win->style |= windowing::WS::MAXIMIZE;
        }
    };

    const auto size_lparam = [&] {
        const i32 cw = win->client_rect.width();
        const i32 ch = win->client_rect.height();
        return static_cast<u64>(
            (cw & 0xFFFF) | ((ch & 0xFFFF) << 16));
    };

    const auto send_size = [&](u64 size_type) {
        send_or_post(hwnd, win->wndproc, windowing::WM::SIZE,
                     size_type, size_lparam());
    };

    const auto send_move = [&] {
        const u64 move_lp = static_cast<u64>(
            (win->rect.left & 0xFFFF) |
            ((win->rect.top & 0xFFFF) << 16));
        send_or_post(hwnd, win->wndproc, windowing::WM::MOVE, 0, move_lp);
    };

    const auto restore_normal = [&] {
        set_state_bits(false, false);
        m_window_host->set_minimized(hwnd, false);
        if (win->normal_rect.width() > 0 &&
            win->normal_rect.height() > 0) {
            win->rect = win->normal_rect;
            if (win->normal_client_rect.width() > 0 &&
                win->normal_client_rect.height() > 0) {
                win->client_rect = win->normal_client_rect;
            } else {
                win->client_rect = windowing::Rect{
                    0, 0, win->normal_rect.width(),
                    win->normal_rect.height()};
            }
            const i32 client_w = win->client_rect.width();
            const i32 client_h = win->client_rect.height();
            // Client area in screen coordinates (outer + NC insets).
            const i32 cx = win->rect.left + win->client_offset.x;
            const i32 cy = win->rect.top + win->client_offset.y;
            const windowing::Rect client_frame{cx, cy,
                                               cx + client_w, cy + client_h};
            m_window_host->set_frame(hwnd, client_frame);
        }
        win->restore_to_maximized = false;
    };

    const auto minimize = [&] {
        remember_normal_geometry();
        if (win->maximized) {
            win->restore_to_maximized = true;
        }
        set_state_bits(true, false);
        set_visible_state(true);
        m_window_host->set_minimized(hwnd, true);
        if (!prev_minimized) {
            send_size(1); // SIZE_MINIMIZED
        }
        update_owned_popups_for_owner(hwnd, false);
    };

    const auto maximize = [&] {
        remember_normal_geometry();
        set_state_bits(false, true);
        win->restore_to_maximized = false;
        m_window_host->set_minimized(hwnd, false);
        set_visible_state(true);

        const windowing::Rect zoomed = m_window_host->zoom_window(hwnd);
        const i32 cw = zoomed.right - zoomed.left;
        const i32 ch = zoomed.bottom;
        if (cw > 0 && ch > 0) {
            win->rect = windowing::Rect{
                zoomed.left, 0, zoomed.right, ch};
            win->client_rect = windowing::Rect{0, 0, cw, ch};
        }
        if (!prev_maximized || prev_minimized) {
            VITREO_TRACE(
                "[show] SW_MAXIMIZE: zoomed hwnd=0x%x to %dx%d\n",
                hwnd, win->client_rect.width(),
                win->client_rect.height());
            send_size(2); // SIZE_MAXIMIZED
        }
        update_owned_popups_for_owner(hwnd, true);
    };

    switch (cmd_show) {
    case windowing::SW::HIDE:
        if (prev_visible) {
            // Learn ShowWindow: SW_HIDE hides the window and activates
            // another window.
            if (m_user32_ext_mgr) {
                m_user32_ext_mgr->release_activation(hwnd, false);
                win = m_hwnd_mgr.find(hwnd);
                if (!win) return prev_visible;
            }
            set_visible_state(false);
            update_owned_popups_for_owner(hwnd, false);
        }
        break;

    case windowing::SW::SHOWNORMAL:
    case windowing::SW::SHOWDEFAULT: {
        const bool state_changed = win->minimized || win->maximized;
        // SW_SHOWNORMAL restores original geometry only from a minimized,
        // maximized, or arranged state. A hidden normal window is displayed
        // at its current size and position; resetting it here can reapply
        // stale creation geometry and duplicate native frame chrome.
        if (state_changed) {
            restore_normal();
        }
        set_visible_state(true);
        if (state_changed || !prev_visible) {
            send_size(0); // SIZE_RESTORED
            send_move();
        }
        update_owned_popups_for_owner(hwnd, true);
        break;
    }

    case windowing::SW::SHOWNOACTIVATE: {
        const bool state_changed = win->minimized || win->maximized;
        if (state_changed) {
            restore_normal();
        }
        set_visible_state(true);
        if (state_changed || !prev_visible) {
            send_size(0);
            send_move();
        }
        update_owned_popups_for_owner(hwnd, true);
        break;
    }

    case windowing::SW::SHOW:
    case windowing::SW::SHOWNA:
        set_visible_state(true);
        if (!prev_visible) {
            send_size(win->minimized ? 1u : win->maximized ? 2u : 0u);
            send_move();
        }
        update_owned_popups_for_owner(hwnd, !win->minimized);
        break;

    case windowing::SW::MINIMIZE:
    case windowing::SW::FORCEMINIMIZE:
        // Learn ShowWindow: SW_MINIMIZE activates the next top-level window
        // in the z-order.
        if (m_user32_ext_mgr) {
            m_user32_ext_mgr->release_activation(hwnd, false);
            win = m_hwnd_mgr.find(hwnd);
            if (!win) return prev_visible;
        }
        minimize();
        break;

    case windowing::SW::SHOWMINIMIZED:
    case windowing::SW::SHOWMINNOACTIVE:
        minimize();
        break;

    case windowing::SW::SHOWMAXIMIZED:
        maximize();
        break;

    case windowing::SW::RESTORE: {
        if (win->minimized && win->restore_to_maximized) {
            maximize();
        } else {
            const bool state_changed = win->minimized || win->maximized;
            restore_normal();
            set_visible_state(true);
            if (state_changed || !prev_visible) {
                send_size(0);
                send_move();
            }
            update_owned_popups_for_owner(hwnd, true);
        }
        break;
    }

    default:
        // ShowWindow documents commands 0 through 11. Keep an unknown command
        // from inventing a state transition; matching SW_SHOW is the narrowest
        // historical fallback and preserves the current size/state.
        set_visible_state(true);
        break;
    }

    if (win->visible != prev_visible) {
        // WM_SHOWWINDOW is sent to the WindowProc as part of the synchronous
        // ShowWindow visibility transition.  Queue only when the runtime has
        // no callback dispatcher (the unit-test/headless fallback).
        send_or_post(hwnd, win->wndproc, windowing::WM::SHOWWINDOW,
                     win->visible ? 1u : 0u, 0);

        if (m_user32_ext_mgr != nullptr) {
            constexpr u32 EVENT_OBJECT_SHOW = 0x8002u;
            constexpr u32 EVENT_OBJECT_HIDE = 0x8003u;
            m_user32_ext_mgr->notify_win_event(
                win->visible ? EVENT_OBJECT_SHOW : EVENT_OBJECT_HIDE, hwnd);
        }

        if (win->visible && m_paint_mgr) {
            m_paint_mgr->invalidate_rect(
                static_cast<u64>(hwnd), nullptr, true);
            invalidate_visible_descendants(hwnd);
        }
    }

    // Activating ShowWindow commands make a top-level window active before
    // returning. Cocoa reports key-window changes asynchronously, so that
    // callback cannot provide this synchronous Win32 state.
    if (activating_top_level) {
        m_window_host->set_active_hwnd(hwnd);
        if (m_user32_ext_mgr) {
            (void)m_user32_ext_mgr->activate_application(hwnd);
        }
    }

    return prev_visible;
}

bool ShowWindowManager::set_window_placement(
    windowing::HWND hwnd, u32 flags, int show_cmd,
    windowing::Point minimized_position,
    windowing::Point maximized_position,
    windowing::Rect normal_rect) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return false;
    }

    // Microsoft documents that SetWindowPlacement adjusts a placement that
    // would leave the window completely off-screen. WINDOWPLACEMENT uses work-
    // area coordinates for ordinary top-level windows and screen coordinates
    // for tool/child windows.
    constexpr u32 WS_EX_TOOLWINDOW = 0x00000080u;
    const auto monitor = m_window_host->get_primary_monitor_info();
    windowing::Rect placement_bounds{
        monitor.work_left,
        monitor.work_top,
        monitor.work_right,
        monitor.work_bottom};
    if ((win->parent == windowing::HWND_DESKTOP ||
         win->parent == windowing::NULL_HWND) &&
        (win->ex_style & WS_EX_TOOLWINDOW) == 0) {
        placement_bounds = windowing::Rect{
            0,
            0,
            monitor.work_right - monitor.work_left,
            monitor.work_bottom - monitor.work_top};
    }
    const bool completely_offscreen =
        normal_rect.right <= placement_bounds.left ||
        normal_rect.left >= placement_bounds.right ||
        normal_rect.bottom <= placement_bounds.top ||
        normal_rect.top >= placement_bounds.bottom;
    if (completely_offscreen && normal_rect.width() > 0 &&
        normal_rect.height() > 0) {
        const i32 width = normal_rect.width();
        const i32 height = normal_rect.height();
        const i32 max_left = std::max(
            placement_bounds.left,
            placement_bounds.right -
                std::min(width, placement_bounds.width()));
        const i32 max_top = std::max(
            placement_bounds.top,
            placement_bounds.bottom -
                std::min(height, placement_bounds.height()));
        const i32 adjusted_left = std::clamp(
            normal_rect.left, placement_bounds.left, max_left);
        const i32 adjusted_top = std::clamp(
            normal_rect.top, placement_bounds.top, max_top);
        normal_rect = windowing::Rect{
            adjusted_left, adjusted_top,
            adjusted_left + width, adjusted_top + height};
    }

    const i32 prior_normal_w = win->normal_rect.width();
    const i32 prior_normal_h = win->normal_rect.height();
    i32 normal_nc_width =
        prior_normal_w - win->normal_client_rect.width();
    i32 normal_nc_height =
        prior_normal_h - win->normal_client_rect.height();
    // A zero-sized CreateWindow leaves normal_rect/client both empty, so the
    // NC delta is 0. Prefer the chrome budget recorded at create time.
    if (normal_nc_width == 0 && normal_nc_height == 0 &&
        (win->client_offset.x != 0 || win->client_offset.y != 0)) {
        normal_nc_width = win->client_offset.x * 2;
        normal_nc_height = win->client_offset.y + win->client_offset.x;
    }
    const windowing::Rect normal_client{
        0,
        0,
        std::max(0, normal_rect.width() - normal_nc_width),
        std::max(0, normal_rect.height() - normal_nc_height)};

    win->minimized_position = minimized_position;
    win->maximized_position = maximized_position;
    win->normal_rect = normal_rect;
    win->normal_client_rect = normal_client;
    win->restore_to_maximized =
        show_cmd == windowing::SW::SHOWMINIMIZED && (flags & 0x0002u) != 0;

    // Learn (SetWindowPlacement): rcNormalPosition is the restored frame.
    // ShowWindow(SW_SHOWNORMAL) intentionally keeps an already-normal window's
    // live geometry (apps may have MoveWindow'd while hidden). Placement must
    // still stamp that restored frame before the show transition. A window
    // may be created at 0×0 and then placed with SetWindowPlacement.
    const bool show_restored =
        show_cmd == windowing::SW::SHOWNORMAL ||
        show_cmd == windowing::SW::SHOWDEFAULT ||
        show_cmd == windowing::SW::SHOW ||
        show_cmd == windowing::SW::SHOWNA ||
        show_cmd == windowing::SW::SHOWNOACTIVATE ||
        show_cmd == windowing::SW::RESTORE;
    if (show_restored && normal_rect.width() > 0 &&
        normal_rect.height() > 0) {
        win->rect = normal_rect;
        win->client_rect = normal_client;
        const i32 ncx = normal_rect.left + win->client_offset.x;
        const i32 ncy = normal_rect.top + win->client_offset.y;
        const windowing::Rect client_frame{
            ncx, ncy,
            ncx + normal_client.width(),
            ncy + normal_client.height()};
        m_window_host->set_frame(hwnd, client_frame);
    }

    (void)show_window(hwnd, show_cmd);

    // Entering minimized/maximized state remembers the pre-transition rect.
    // SetWindowPlacement's supplied restored geometry is authoritative, so
    // retain it for the later SW_RESTORE/GetWindowPlacement round trip.
    win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return false;
    }
    win->minimized_position = minimized_position;
    win->maximized_position = maximized_position;
    win->normal_rect = normal_rect;
    win->normal_client_rect = normal_client;
    if (win->minimized) {
        win->restore_to_maximized =
            show_cmd == windowing::SW::SHOWMINIMIZED &&
            (flags & 0x0002u) != 0;
    }
    return true;
}

// -----------------------------------------------------------------------------
// update_window
// -----------------------------------------------------------------------------

bool ShowWindowManager::update_window(windowing::HWND hwnd) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return false;
    }

    if (!win->visible) {
        return true;
    }

    // Real Win32 UpdateWindow sends WM_PAINT directly to the wndproc
    // (bypassing the queue) if the window has a non-empty update region.
    if (m_paint_mgr && m_msgloop_mgr &&
        m_paint_mgr->needs_paint(static_cast<u64>(hwnd))) {
        windowing::MSG paint_msg{};
        paint_msg.hwnd    = hwnd;
        paint_msg.message = windowing::WM::PAINT;
        m_msgloop_mgr->dispatch_message(paint_msg);
        return true;
    }

    // An empty update region makes UpdateWindow a successful no-op. Windows
    // does not post a speculative WM_PAINT in this case.
    return true;
}

// -----------------------------------------------------------------------------
// is_window_visible
// -----------------------------------------------------------------------------

bool ShowWindowManager::is_window_visible(windowing::HWND hwnd) const {
    // Learn (nf-winuser-iswindowvisible): the window and every ancestor must
    // have WS_VISIBLE. Peer HWNDs publish that bit in hwnd_registry.
    const auto registry_visible = [](u32 h) -> std::optional<bool> {
        const auto entry = ipc::hwnd_registry_lookup(h);
        if (!entry) return std::nullopt;
        return entry->visible;
    };
    const auto registry_parent = [](u32 h) -> u32 {
        const auto entry = ipc::hwnd_registry_lookup(h);
        return entry ? entry->parent_hwnd : 0;
    };

    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        const auto vis = registry_visible(static_cast<u32>(hwnd));
        if (!vis.has_value()) return false;
        if (!*vis) return false;
        u32 current = registry_parent(static_cast<u32>(hwnd));
        while (current != 0 && current != windowing::HWND_DESKTOP) {
            if (const auto* local = m_hwnd_mgr.find(
                    static_cast<windowing::HWND>(current))) {
                if ((local->style & windowing::WS::VISIBLE) == 0) return false;
                current = local->parent;
                continue;
            }
            const auto pvis = registry_visible(current);
            if (!pvis.has_value() || !*pvis) return false;
            current = registry_parent(current);
        }
        return true;
    }

    // Learn: IsWindowVisible tests the WS_VISIBLE style on the window and
    // every ancestor (Remarks). The WindowData::visible bool tracks the same
    // bit via ShowWindow / SetWindowLong(GWL_STYLE).
    if ((win->style & windowing::WS::VISIBLE) == 0) {
        return false;
    }

    // Walk parent chain — if any ancestor is invisible, return false.
    // A foreign parent is resolved through the capsule HWND registry.
    windowing::HWND current = win->parent;
    while (current != windowing::HWND_DESKTOP && current != windowing::NULL_HWND) {
        const windowing::WindowData* parent = m_hwnd_mgr.find(current);
        if (parent) {
            if ((parent->style & windowing::WS::VISIBLE) == 0) {
                return false;
            }
            current = parent->parent;
            continue;
        }
        const auto pvis = registry_visible(static_cast<u32>(current));
        if (!pvis.has_value() || !*pvis) {
            return false;
        }
        current = static_cast<windowing::HWND>(
            registry_parent(static_cast<u32>(current)));
    }

    return true;
}

// =============================================================================
// Thunk Implementations
// =============================================================================

/// VITREO_MOUSE_DIAG also records popup show/hide, so the probe file shows the
/// tooltip appearing next to the mouse messages around it.
static void diag_note_show(windowing::HWND hwnd, u32 cmd, u32 style) {
    static std::FILE* const f = [] () -> std::FILE* {
        const char* path = std::getenv("VITREO_MOUSE_DIAG");
        if (path == nullptr || *path == '\0') return nullptr;
        return std::fopen((std::string(path) + ".show").c_str(), "w");
    }();
    if (f == nullptr) return;
    std::fprintf(f, "ShowWindow hwnd=%x cmd=%u style=%x\n",
                 static_cast<unsigned>(hwnd), cmd, style);
    std::fflush(f);
}

void impl_ShowWindow(Marshaler& m, ShowWindowManager& sm) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    hwnd = resolve_zero_hwnd(sm.hwnd_mgr(), hwnd);
    auto cmd_show = static_cast<int>(m.arg(1));
    if (std::getenv("VITREO_WIN_DIAG")) {
        // The caller matters as much as the command: "who hid this window"
        // is not answerable from the hwnd and cmd alone, and a guest that
        // hides its own main window is indistinguishable from Vitreo doing
        // it without this.
        fprintf(stderr, "[show] ShowWindow hwnd=0x%x cmd=%d caller_ret=0x%llx\n",
                hwnd, cmd_show,
                static_cast<unsigned long long>(m.guest_return_address()));
    }
    VITREO_TRACE( "[show] ShowWindow(hwnd=0x%x cmd=%d)\n", hwnd, cmd_show);
    {
        const auto* wd = sm.hwnd_mgr().find(hwnd);
        diag_note_show(hwnd, static_cast<u32>(cmd_show),
                       wd != nullptr ? wd->style : 0u);
    }
    // Learn: ShowWindow takes any window of the desktop. A window another
    // Vitreo process owns is shown by that process, on its owner thread.
    if (sm.hwnd_mgr().find(hwnd) == nullptr) {
        const auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd));
        if (remote && remote->owner_host_pid != 0 &&
            remote->owner_host_pid != static_cast<u32>(::getpid())) {
            u64 result = 0;
            if (ipc::remote_show_window(static_cast<pid_t>(remote->owner_host_pid),
                                        static_cast<u32>(hwnd), cmd_show, result)) {
                VITREO_TRACE("[show] ShowWindow(hwnd=0x%x) peer pid=%u -> 0x%llx\n", hwnd,
                             remote->owner_host_pid, static_cast<unsigned long long>(result));
                m.set_return(result);
                return;
            }
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(0);
            return;
        }
    }
    const u64 show_result = sm.show_window_result(hwnd, cmd_show);

    m.set_return(show_result);
}

// BOOL ShowWindowAsync(HWND hWnd, int nCmdShow). Learn: "posts a
// show-window event to the message queue of the given window"; nonzero when
// the operation was started. Measured (u32_char_dde_probe
// "-- showwindowasync", interactive session):
//  * the event is deferred even for the caller's own window: nothing changes
//    and no WM_SHOWWINDOW is sent until that thread next checks its queue,
//    and GetQueueStatus reports it as QS_SENDMESSAGE;
//  * the return value is 1 whether or not the visibility changes;
//  * an invalid or NULL window is ERROR_INVALID_WINDOW_HANDLE, nCmdShow above
//    SW_MAX (11) is ERROR_INVALID_PARAMETER and nothing is queued.
// The event is queued as a notify-style sent message to the owning thread,
// which runs ShowWindow there the next time it checks its queue.
static void impl_ShowWindowAsync(Marshaler& m, ShowWindowManager& sm) {
    constexpr u32 kSwMax = 11;
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 cmd_show = m.arg32(1);
    const auto owner = hwnd != windowing::NULL_HWND
                           ? sm.hwnd_mgr().owner_thread_id(hwnd)
                           : std::nullopt;
    if (!owner || sm.hwnd_mgr().find(hwnd) == nullptr) {
        if (hwnd != windowing::NULL_HWND &&
            ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!ShowWindowAsync: "
                                     "window of another process\n");
            }
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        } else {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        m.set_return(0u);
        return;
    }
    if (cmd_show > kSwMax) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 sender = ThreadManager::get_current_thread_id_static();
    ShowWindowManager* manager = &sm;
    const bool queued = sm.queue().notify_thread_send(
        sender, *owner, [manager, hwnd, cmd_show]() -> u64 {
            if (manager->hwnd_mgr().find(hwnd) == nullptr) return 0;
            (void)manager->show_window_result(hwnd, static_cast<int>(cmd_show));
            return 0;
        });
    m.set_return(queued ? 1u : 0u);
}

void impl_UpdateWindow(Marshaler& m, ShowWindowManager& sm) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    hwnd = resolve_zero_hwnd(sm.hwnd_mgr(), hwnd);
    // Not a window: FALSE, error untouched ("UpdateWindow(bad hwnd)=0").
    if (sm.hwnd_mgr().find(hwnd) == nullptr && !ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
        m.set_return(0u);
        return;
    }
    sm.update_window(hwnd);
    m.set_return(1);
}

void impl_IsWindowVisible(Marshaler& m, ShowWindowManager& sm) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    // Measured (s1win_api_probe iswindowvisible_invalid): an invalid window
    // is ERROR_INVALID_WINDOW_HANDLE.
    if (sm.hwnd_mgr().find(hwnd) == nullptr &&
        !ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    bool visible = sm.is_window_visible(hwnd);
    VITREO_TRACE("[show] IsWindowVisible(hwnd=0x%x) -> %s\n",
                 hwnd, visible ? "TRUE" : "FALSE");
    m.set_return(visible ? 1u : 0u);
}

void impl_EnableWindow(Marshaler& m, ShowWindowManager& sm) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    bool enable = m.arg(1) != 0;
    VITREO_TRACE( "[show] EnableWindow(hwnd=0x%x enable=%s)\n",
                 hwnd, enable ? "TRUE" : "FALSE");
    if (sm.hwnd_mgr().find(hwnd) == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    bool prev_enabled = sm.enable_window(hwnd, enable);

    // Win32 contract: returns TRUE if window was previously *disabled*, FALSE otherwise
    m.set_return(prev_enabled ? 0u : 1u);
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_show_window_exports(ShowWindowManager& sm) {
    DllExports exports;
    // ShowWindow (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "ShowWindow", 1,
        [&sm](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            impl_ShowWindow(m, sm);
            },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ShowWindowAsync", 0,
        [&sm](Marshaler& m) { impl_ShowWindowAsync(m, sm); },
        8
    });
    // UpdateWindow (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "UpdateWindow", 2,
        [&sm](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        impl_UpdateWindow(m, sm); },
        4
    });
    // IsWindowVisible (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "IsWindowVisible", 3,
        [&sm](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        impl_IsWindowVisible(m, sm); },
        4
    });
    // EnableWindow (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "EnableWindow", 4,
        [&sm](Marshaler& m) { impl_EnableWindow(m, sm); },
        8
    });
    return exports;
}

} // namespace vitreo::winapi
