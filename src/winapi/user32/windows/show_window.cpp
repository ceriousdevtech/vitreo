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
        bool changed = false;
        const auto committed = m_hwnd_mgr.update_geometry(hwnd, [&](windowing::WindowData& win) {
            if (win.owner != owner) return;
            if (show) {
                if (!win.hidden_by_owner) return;
                win.hidden_by_owner = false;
                win.visible = true;
            } else {
                if (!win.visible) return;
                win.hidden_by_owner = true;
                win.visible = false;
            }
            changed = true;
        });
        if (!committed || !changed) continue;
        m_window_host->set_visible(hwnd, show);
        if (!m_hwnd_mgr.update_geometry(hwnd, [](windowing::WindowData&) {})) continue;
        windowing::MSG msg_show{};
        msg_show.hwnd    = hwnd;
        msg_show.message = windowing::WM::SHOWWINDOW;
        msg_show.wParam  = show ? 1u : 0u;
        m_queue.post(msg_show);
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

    // Keep only a committed local copy across host and guest callbacks. Every
    // mutation reacquires the HWND and copies its resulting state under the
    // owner lock; callbacks below never retain a live WindowData pointer.
    windowing::WindowData state;
    const auto commit = [&](const auto& update) {
        return m_hwnd_mgr.update_geometry(hwnd, [&](windowing::WindowData& live) {
            update(live);
            state = live;
        }).has_value();
    };
    const auto refresh = [&] { return commit([](windowing::WindowData&) {}); };
    if (!refresh()) return false;
    const auto* win = &state;
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
    // visibility operation below is still on the stack. Reserve activation
    // first without holding the HWND owner lock across the host call.
    if (activating_top_level && m_user32_ext_mgr) {
        (void)m_window_host->exchange_app_active(true);
        if (!refresh()) return false;
    }

    const auto set_visible_state = [&](bool visible) {
        if (!commit([&](windowing::WindowData& live) {
            live.visible = visible;
            if (visible) live.style |= windowing::WS::VISIBLE;
            else live.style &= ~windowing::WS::VISIBLE;
        })) return false;
        m_window_host->set_visible(hwnd, visible);
        return refresh();
    };
    const auto remember_normal_geometry = [&] {
        return commit([](windowing::WindowData& live) {
            if (!live.minimized && !live.maximized) {
                live.normal_rect = live.rect;
                live.normal_client_rect = live.client_rect;
            }
        });
    };
    const auto set_state_bits = [&](bool minimized, bool maximized) {
        if (!refresh()) return false;
        // Measured EVENT_SYSTEM_MINIMIZESTART/END ordering is retained; the
        // notification can reenter USER32, so reacquire for the final commit.
        if (minimized != win->minimized && m_user32_ext_mgr != nullptr) {
            m_user32_ext_mgr->notify_win_event(minimized ? 0x0016u : 0x0017u, hwnd);
        }
        return commit([&](windowing::WindowData& live) {
            live.minimized = minimized;
            live.maximized = !minimized && maximized;
            live.style &= ~(windowing::WS::MINIMIZE | windowing::WS::MAXIMIZE);
            if (live.minimized) live.style |= windowing::WS::MINIMIZE;
            else if (live.maximized) live.style |= windowing::WS::MAXIMIZE;
        });
    };
    const auto send_size = [&](u64 size_type) {
        if (!refresh()) return false;
        const i32 cw = win->client_rect.width();
        const i32 ch = win->client_rect.height();
        const u64 size_lp = static_cast<u64>((cw & 0xFFFF) | ((ch & 0xFFFF) << 16));
        send_or_post(hwnd, win->wndproc, windowing::WM::SIZE, size_type, size_lp);
        return refresh();
    };
    const auto send_move = [&] {
        if (!refresh()) return false;
        const u64 move_lp = static_cast<u64>(
            (win->rect.left & 0xFFFF) | ((win->rect.top & 0xFFFF) << 16));
        send_or_post(hwnd, win->wndproc, windowing::WM::MOVE, 0, move_lp);
        return refresh();
    };
    const auto restore_normal = [&] {
        if (!set_state_bits(false, false)) return false;
        m_window_host->set_minimized(hwnd, false);
        std::optional<windowing::Rect> frame;
        if (!commit([&](windowing::WindowData& live) {
            if (live.normal_rect.width() > 0 && live.normal_rect.height() > 0) {
                live.rect = live.normal_rect;
                if (live.normal_client_rect.width() > 0 &&
                    live.normal_client_rect.height() > 0) {
                    live.client_rect = live.normal_client_rect;
                } else {
                    live.client_rect = {0, 0, live.normal_rect.width(), live.normal_rect.height()};
                }
                const i32 cx = live.rect.left + live.client_offset.x;
                const i32 cy = live.rect.top + live.client_offset.y;
                frame = windowing::Rect{cx, cy, cx + live.client_rect.width(), cy + live.client_rect.height()};
            }
        })) return false;
        if (frame) m_window_host->set_frame(hwnd, *frame);
        return commit([](windowing::WindowData& live) { live.restore_to_maximized = false; });
    };
    const auto minimize = [&] {
        if (!remember_normal_geometry()) return false;
        if (!commit([](windowing::WindowData& live) {
            if (live.maximized) live.restore_to_maximized = true;
        })) return false;
        if (!set_state_bits(true, false) || !set_visible_state(true)) return false;
        m_window_host->set_minimized(hwnd, true);
        if (!refresh()) return false;
        if (!prev_minimized && !send_size(1)) return false;
        update_owned_popups_for_owner(hwnd, false);
        return refresh();
    };
    const auto maximize = [&] {
        if (!remember_normal_geometry() || !set_state_bits(false, true)) return false;
        if (!commit([](windowing::WindowData& live) { live.restore_to_maximized = false; })) return false;
        m_window_host->set_minimized(hwnd, false);
        if (!refresh() || !set_visible_state(true)) return false;
        const windowing::Rect zoomed = m_window_host->zoom_window(hwnd);
        const i32 cw = zoomed.right - zoomed.left;
        const i32 ch = zoomed.bottom;
        if (!commit([&](windowing::WindowData& live) {
            if (cw > 0 && ch > 0) {
                live.rect = {zoomed.left, 0, zoomed.right, ch};
                live.client_rect = {0, 0, cw, ch};
            }
        })) return false;
        if (!prev_maximized || prev_minimized) {
            VITREO_TRACE("[show] SW_MAXIMIZE: zoomed hwnd=0x%x to %dx%d\n",
                hwnd, win->client_rect.width(), win->client_rect.height());
            if (!send_size(2)) return false;
        }
        update_owned_popups_for_owner(hwnd, true);
        return refresh();
    };

    switch (cmd_show) {
    case windowing::SW::HIDE:
        if (prev_visible) {
            if (m_user32_ext_mgr) {
                m_user32_ext_mgr->release_activation(hwnd, false);
                if (!refresh()) return false;
            }
            if (!set_visible_state(false)) return false;
            update_owned_popups_for_owner(hwnd, false);
            if (!refresh()) return false;
        }
        break;
    case windowing::SW::SHOWNORMAL:
    case windowing::SW::SHOWDEFAULT:
    case windowing::SW::SHOWNOACTIVATE: {
        const bool state_changed = win->minimized || win->maximized;
        // A normal window retains its current frame; restore only from a
        // minimized/maximized state, as before this ownership conversion.
        if (state_changed && !restore_normal()) return false;
        if (!set_visible_state(true)) return false;
        if ((state_changed || !prev_visible) && (!send_size(0) || !send_move())) return false;
        update_owned_popups_for_owner(hwnd, true);
        if (!refresh()) return false;
        break;
    }
    case windowing::SW::SHOW:
    case windowing::SW::SHOWNA:
        if (!set_visible_state(true)) return false;
        if (!prev_visible && (!send_size(win->minimized ? 1u : win->maximized ? 2u : 0u) || !send_move())) return false;
        update_owned_popups_for_owner(hwnd, !win->minimized);
        if (!refresh()) return false;
        break;
    case windowing::SW::MINIMIZE:
    case windowing::SW::FORCEMINIMIZE:
        if (m_user32_ext_mgr) {
            m_user32_ext_mgr->release_activation(hwnd, false);
            if (!refresh()) return false;
        }
        if (!minimize()) return false;
        break;
    case windowing::SW::SHOWMINIMIZED:
    case windowing::SW::SHOWMINNOACTIVE:
        if (!minimize()) return false;
        break;
    case windowing::SW::SHOWMAXIMIZED:
        if (!maximize()) return false;
        break;
    case windowing::SW::RESTORE:
        if (win->minimized && win->restore_to_maximized) {
            if (!maximize()) return false;
        } else {
            const bool state_changed = win->minimized || win->maximized;
            if (!restore_normal() || !set_visible_state(true)) return false;
            if ((state_changed || !prev_visible) && (!send_size(0) || !send_move())) return false;
            update_owned_popups_for_owner(hwnd, true);
            if (!refresh()) return false;
        }
        break;
    default:
        if (!set_visible_state(true)) return false;
        break;
    }

    if (win->visible != prev_visible) {
        send_or_post(hwnd, win->wndproc, windowing::WM::SHOWWINDOW, win->visible ? 1u : 0u, 0);
        if (!refresh()) return false;
        if (m_user32_ext_mgr != nullptr) {
            constexpr u32 EVENT_OBJECT_SHOW = 0x8002u;
            constexpr u32 EVENT_OBJECT_HIDE = 0x8003u;
            m_user32_ext_mgr->notify_win_event(win->visible ? EVENT_OBJECT_SHOW : EVENT_OBJECT_HIDE, hwnd);
            if (!refresh()) return false;
        }
        if (win->visible && m_paint_mgr) {
            m_paint_mgr->invalidate_rect(static_cast<u64>(hwnd), nullptr, true);
            invalidate_visible_descendants(hwnd);
            if (!refresh()) return false;
        }
    }
    if (activating_top_level) {
        m_window_host->set_active_hwnd(hwnd);
        if (!refresh()) return false;
        if (m_user32_ext_mgr) {
            (void)m_user32_ext_mgr->activate_application(hwnd);
            if (!refresh()) return false;
        }
    }
    return prev_visible;
}

bool ShowWindowManager::set_window_placement(
    windowing::HWND hwnd, u32 flags, int show_cmd,
    windowing::Point minimized_position,
    windowing::Point maximized_position,
    windowing::Rect normal_rect) {
    if (!m_hwnd_mgr.update_geometry(hwnd, [](windowing::WindowData&) {})) return false;
    const auto monitor = m_window_host->get_primary_monitor_info();
    windowing::Rect normal_client{};
    std::optional<windowing::Rect> frame;
    const bool show_restored =
        show_cmd == windowing::SW::SHOWNORMAL || show_cmd == windowing::SW::SHOWDEFAULT ||
        show_cmd == windowing::SW::SHOW || show_cmd == windowing::SW::SHOWNA ||
        show_cmd == windowing::SW::SHOWNOACTIVATE || show_cmd == windowing::SW::RESTORE;
    const auto committed = m_hwnd_mgr.update_geometry(hwnd, [&](windowing::WindowData& win) {
        constexpr u32 WS_EX_TOOLWINDOW = 0x00000080u;
        windowing::Rect bounds{monitor.work_left, monitor.work_top, monitor.work_right, monitor.work_bottom};
        if ((win.parent == windowing::HWND_DESKTOP || win.parent == windowing::NULL_HWND) &&
            (win.ex_style & WS_EX_TOOLWINDOW) == 0) {
            bounds = {0, 0, monitor.work_right - monitor.work_left, monitor.work_bottom - monitor.work_top};
        }
        const bool offscreen = normal_rect.right <= bounds.left || normal_rect.left >= bounds.right ||
                               normal_rect.bottom <= bounds.top || normal_rect.top >= bounds.bottom;
        if (offscreen && normal_rect.width() > 0 && normal_rect.height() > 0) {
            const i32 width = normal_rect.width(), height = normal_rect.height();
            const i32 max_left = std::max(bounds.left, bounds.right - std::min(width, bounds.width()));
            const i32 max_top = std::max(bounds.top, bounds.bottom - std::min(height, bounds.height()));
            const i32 left = std::clamp(normal_rect.left, bounds.left, max_left);
            const i32 top = std::clamp(normal_rect.top, bounds.top, max_top);
            normal_rect = {left, top, left + width, top + height};
        }
        i32 nc_width = win.normal_rect.width() - win.normal_client_rect.width();
        i32 nc_height = win.normal_rect.height() - win.normal_client_rect.height();
        if (nc_width == 0 && nc_height == 0 && (win.client_offset.x != 0 || win.client_offset.y != 0)) {
            nc_width = win.client_offset.x * 2;
            nc_height = win.client_offset.y + win.client_offset.x;
        }
        normal_client = {0, 0, std::max(0, normal_rect.width() - nc_width),
                              std::max(0, normal_rect.height() - nc_height)};
        win.minimized_position = minimized_position;
        win.maximized_position = maximized_position;
        win.normal_rect = normal_rect;
        win.normal_client_rect = normal_client;
        win.restore_to_maximized = show_cmd == windowing::SW::SHOWMINIMIZED && (flags & 0x0002u) != 0;
        if (show_restored && normal_rect.width() > 0 && normal_rect.height() > 0) {
            win.rect = normal_rect;
            win.client_rect = normal_client;
            const i32 x = normal_rect.left + win.client_offset.x;
            const i32 y = normal_rect.top + win.client_offset.y;
            frame = windowing::Rect{x, y, x + normal_client.width(), y + normal_client.height()};
        }
    });
    if (!committed) return false;
    if (frame) m_window_host->set_frame(hwnd, *frame);
    (void)show_window(hwnd, show_cmd);
    // The supplied restored frame remains authoritative after ShowWindow's
    // callbacks and its normal/minimized/maximized geometry bookkeeping.
    return m_hwnd_mgr.update_geometry(hwnd, [&](windowing::WindowData& win) {
        win.minimized_position = minimized_position;
        win.maximized_position = maximized_position;
        win.normal_rect = normal_rect;
        win.normal_client_rect = normal_client;
        if (win.minimized) win.restore_to_maximized =
            show_cmd == windowing::SW::SHOWMINIMIZED && (flags & 0x0002u) != 0;
    }).has_value();
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
