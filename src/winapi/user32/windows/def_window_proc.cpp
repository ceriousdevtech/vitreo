#include "winapi/user32/windows/min_track.hpp"
#include "winapi/user32/ansi_text.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/user32/painting/sys_colors.hpp"
#include "winapi/user32/windows/windowpos_layout.hpp"
/// @file def_window_proc.cpp
/// @brief DefWindowProcW default message handling (WIN32-054)

#include <cstdlib>
#include "def_window_proc.hpp"
#include "window.hpp"
#include "windowing/model/window_types.hpp"
#include "windowing/model/diag_flags.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/messages/mouse_in_pointer.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

using namespace windowing;

// =============================================================================
// DefWindowProcManager
// =============================================================================

DefWindowProcManager::DefWindowProcManager(
    HwndManager& hwnd_mgr,
    MessageQueue& queue,
    GuestMemory& memory
)
    : m_hwnd_mgr{hwnd_mgr}
    , m_queue{queue}
    , m_memory{memory}
{}

// -----------------------------------------------------------------------------
// def_window_proc
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::def_window_proc(HWND hwnd, u32 msg, u64 wParam, u64 lParam) {
    if (windowing::diag::input_diag() &&
        ((msg >= 0x00A0 && msg <= 0x00AF) || msg == 0x0112 ||
         msg == 0x0231 || msg == 0x0232 || msg == 0x0024)) {
        std::fprintf(stderr,
                     "[input] DefWindowProc hwnd=0x%x msg=0x%04x wParam=0x%llx\n",
                     hwnd, msg, static_cast<unsigned long long>(wParam));
    }
    switch (msg) {
        case WM::MOUSEWHEEL:
        case WM::MOUSEHWHEEL: {
            // Microsoft Learn WM_MOUSE[H]WHEEL: default processing sends
            // an unhandled wheel message up the parent chain. Its position
            // is already in screen coordinates; preserve both parameters.
            const auto* window = m_hwnd_mgr.find(hwnd);
            if (window != nullptr && (window->style & WS::CHILD) != 0 &&
                window->parent != NULL_HWND &&
                window->parent != HWND_DESKTOP && m_msgsend_mgr != nullptr) {
                // Oracle wheel_parent_probe: default processing returns zero
                // even when the parent's handler returns a nonzero LRESULT.
                (void)m_msgsend_mgr->send_message(window->parent, msg, wParam, lParam);
            }
            return 0;
        }
        case mouse_in_pointer::WM_POINTERUPDATE:
        case mouse_in_pointer::WM_POINTERDOWN:
        case mouse_in_pointer::WM_POINTERUP:
        case mouse_in_pointer::WM_POINTERWHEEL:
        case mouse_in_pointer::WM_POINTERHWHEEL:
            // An unhandled mouse pointer message yields the mouse message it
            // was generated from, retrieved next (measured on Windows).
            if (auto legacy = mouse_in_pointer::promote(
                    ThreadManager::get_current_thread_id_static(), hwnd, msg,
                    wParam, lParam)) {
                m_queue.post_input_front(*legacy);
            }
            return 0;
        case WM::NCCREATE:      return 1; // TRUE - allow window creation
        case WM::CREATE:        return 0; // 0 means continue creation (not -1 which aborts)
        case WM::CLOSE:         return handle_close(hwnd);
        case WM::NCCALCSIZE:    return handle_nccalcsize(hwnd, lParam);
        case WM::NCHITTEST:     return handle_nchittest(hwnd, lParam);
        case WM::NCLBUTTONDOWN:
            return handle_nclbuttondown(hwnd, wParam);
        case WM::GETTEXT:       return handle_gettext(hwnd, wParam, lParam);
        case WM::SETTEXT:       return handle_settext(hwnd, lParam);
        case WM::GETTEXTLENGTH: return handle_gettextlength(hwnd);
        case WM::ERASEBKGND:    return handle_erasebkgnd(hwnd);
        case WM::QUERYOPEN:     return handle_queryopen();
        case WM::SYSCOMMAND:    return handle_syscommand(hwnd, wParam);
        case WM::PAINT:         return handle_paint(hwnd);
        case WM::SETCURSOR:     return handle_setcursor(hwnd, lParam);
        case WM::WINDOWPOSCHANGING:
            return handle_windowposchanging(hwnd, lParam);
        case WM::WINDOWPOSCHANGED:
            return handle_windowposchanged(hwnd, lParam);
        case WM::ACTIVATE: {
            // Learn WM_ACTIVATE: if the window is being activated and is not
            // minimized, DefWindowProc sets the keyboard focus to it.
            const WindowData* win = m_hwnd_mgr.find(hwnd);
            if ((wParam & 0xFFFFu) != WA::INACTIVE && (wParam >> 16) == 0 &&
                win != nullptr && !win->minimized && m_focus_window) {
                m_focus_window(hwnd);
            }
            return 0;
        }
        case WM::NCACTIVATE:
            // Learn WM_NCACTIVATE: DefWindowProc returns TRUE, allowing the
            // change. The native frame is drawn by the host, not here.
            return 1;
        case WM::MOUSEACTIVATE: {
            // Learn WM_MOUSEACTIVATE: DefWindowProc passes a child window's
            // message to its parent first and uses a nonzero answer;
            // otherwise the window is activated (MA_ACTIVATE).
            const WindowData* window = m_hwnd_mgr.find(hwnd);
            if (window != nullptr && (window->style & WS::CHILD) != 0 &&
                window->parent != NULL_HWND && window->parent != HWND_DESKTOP &&
                m_msgsend_mgr != nullptr) {
                const u64 parent_answer =
                    m_msgsend_mgr->send_message(window->parent, msg, wParam, lParam);
                if (parent_answer != 0) return parent_answer;
            }
            return 1;  // MA_ACTIVATE
        }
        default:
            if (windowing::diag::input_diag() &&
                msg >= 0x00A0 && msg <= 0x00AF) {
                std::fprintf(stderr,
                             "[input] DefWindowProc unhandled NC msg=0x%04x "
                             "hwnd=0x%x wParam=0x%llx\n",
                             msg, hwnd,
                             static_cast<unsigned long long>(wParam));
            }
            return 0; // Unhandled messages return 0
    }
}

u64 DefWindowProcManager::handle_windowposchanged(HWND hwnd, u64 lParam) {
    const auto* window = m_hwnd_mgr.find(hwnd);
    if (!window || !lParam) return 0;
    const auto flags = m_memory.read_u32(
        lParam + WindowPosLayout{m_is_64bit}.scalar_offset() + 16);
    if (flags.is_err()) return 0;
    // Snapshot before sending: WM_MOVE/WM_SIZE may reenter SetWindowPos.
    const auto rect = window->rect;
    const auto client = window->client_rect;
    const auto origin = window->client_offset;
    const auto style = window->style;
    const auto send = [&](u32 message, u64 wparam, u64 lparam) {
        if (m_msgsend_mgr) {
            (void)m_msgsend_mgr->send_message(hwnd, message, wparam, lparam);
        } else {
            MSG msg{};
            msg.hwnd = hwnd;
            msg.message = message;
            msg.wParam = wparam;
            msg.lParam = lparam;
            m_queue.post(msg);
        }
    };
    const auto pack = [](i32 x, i32 y) -> u32 {
        return (static_cast<u32>(x) & 0xffffu) |
               ((static_cast<u32>(y) & 0xffffu) << 16);
    };
    if ((flags.value() & 0x1000u) == 0) { // No client-origin change (WINDOWPOSCHANGED)
        send(WM::MOVE, 0, pack(rect.left + origin.x, rect.top + origin.y));
    }
    // WM_SIZE also follows a minimize / maximize / restore even when the
    // client size is unchanged (measured, s1win_msgseq2_probe
    // create_popup_minimize_hidden: SWP_NOCLIENTSIZE | SWP_STATECHANGED
    // still sends WM_SIZE(SIZE_MINIMIZED)).
    if ((flags.value() & 0x0800u) == 0 || (flags.value() & 0x8000u) != 0) {
        const u32 kind = (style & WS::MINIMIZE) ? 1u :
                         (style & WS::MAXIMIZE) ? 2u : 0u;
        send(WM::SIZE, kind, pack(client.width(), client.height()));
    }
    return 0;
}

u64 DefWindowProcManager::handle_windowposchanging(HWND hwnd, u64 lParam) {
    WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win || lParam == 0 || !m_msgsend_mgr || m_windowpos_scratch == 0) {
        return 0;
    }

    // Microsoft documents that DefWindowProc sends WM_GETMINMAXINFO while
    // processing WM_WINDOWPOSCHANGING for overlapped or thick-frame windows,
    // then validates the proposed size. MINMAXINFO is five POINTs (40 bytes)
    // in both Win32 and Win64.
    const bool overlapped = (win->style & (WS::POPUP | WS::CHILD)) == 0;
    if (!overlapped && (win->style & WS::THICKFRAME) == 0) {
        return 0;
    }

    constexpr u32 SWP_NOSIZE = 0x0001;
    const GuestAddr wp = static_cast<GuestAddr>(lParam);
    const u32 base = WindowPosLayout{m_is_64bit}.scalar_offset();
    const u32 flags = m_memory.read_u32(wp + base + 16).value_or(SWP_NOSIZE);
    if ((flags & SWP_NOSIZE) != 0) {
        return 0;
    }

    const GuestAddr mmi = m_windowpos_scratch + 0x200;
    const WindowData* desktop = m_hwnd_mgr.find(HWND_DESKTOP);
    const i32 screen_w = desktop ? desktop->rect.width() : 1920;
    const i32 screen_h = desktop ? desktop->rect.height() : 1080;
    i32 min_track_w = 0, min_track_h = 0;
    default_min_track_size(win->style, win->ex_style, min_track_w, min_track_h);
    const i32 defaults[10] = {
        0, 0, screen_w, screen_h, 0, 0,
        min_track_w, min_track_h,
        screen_w + 12, screen_h + 12};
    for (u32 i = 0; i < 10; ++i) {
        (void)m_memory.write_u32(mmi + i * 4, static_cast<u32>(defaults[i]));
    }

    (void)m_msgsend_mgr->send_message(hwnd, WM::GETMINMAXINFO, 0, mmi);

    i32 cx = static_cast<i32>(m_memory.read_u32(wp + base + 8).value_or(0));
    i32 cy = static_cast<i32>(m_memory.read_u32(wp + base + 12).value_or(0));
    const i32 min_w = static_cast<i32>(m_memory.read_u32(mmi + 24).value_or(0));
    const i32 min_h = static_cast<i32>(m_memory.read_u32(mmi + 28).value_or(0));
    const i32 max_w = static_cast<i32>(m_memory.read_u32(mmi + 32).value_or(0));
    const i32 max_h = static_cast<i32>(m_memory.read_u32(mmi + 36).value_or(0));
    VITREO_TRACE("[defwnd] WM_WINDOWPOSCHANGING hwnd=0x%x proposed=%dx%d "
                 "track=%dx%d..%dx%d\n",
                 hwnd, cx, cy, min_w, min_h, max_w, max_h);
    // A minimized window keeps its icon size (measured,
    // create_overlapped_minimize_hidden: 160x28 despite a 136x39 track).
    if (win->style & WS::MINIMIZE) return 0;
    if (min_w > 0 && cx < min_w) cx = min_w;
    if (min_h > 0 && cy < min_h) cy = min_h;
    if (max_w > 0 && cx > max_w) cx = max_w;
    if (max_h > 0 && cy > max_h) cy = max_h;
    (void)m_memory.write_u32(wp + base + 8, static_cast<u32>(cx));
    (void)m_memory.write_u32(wp + base + 12, static_cast<u32>(cy));
    return 0;
}

u64 DefWindowProcManager::handle_setcursor(HWND hwnd, u64 lParam) {
    // Learn WM_SETCURSOR: when the cursor is in the client area and the mouse
    // is not captured, DefWindowProc sets the class cursor. A missing class
    // cursor must not force IDC_ARROW — that undoes SetCursor(NULL) when the
    // window hides the pointer, or falls through to DefWindowProc with
    // hCursor == NULL.
    const u16 hit = static_cast<u16>(lParam & 0xFFFFu);
    if (hit != HT::CLIENT) {
        return 0;
    }
    u32 cursor = 0;
    if (const WindowData* win = m_hwnd_mgr.find(hwnd); win != nullptr &&
        m_wclass_mgr != nullptr) {
        if (const WindowClassData* cls =
                m_wclass_mgr->find_by_atom(win->class_atom)) {
            cursor = cls->hcursor;
        }
    }
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] DefWindowProc WM_SETCURSOR hwnd=0x%x class "
                     "cursor=0x%x\n",
                     static_cast<unsigned>(hwnd), cursor);
    }
    if (cursor != 0 && m_apply_cursor) {
        m_apply_cursor(cursor);
    }
    return 1; // TRUE — handled; stop further processing
}

// -----------------------------------------------------------------------------
// handle_nccalcsize
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_nccalcsize(HWND hwnd, u64 lParam) {
    const WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win || lParam == 0) {
        return 0;
    }

    // For both forms of WM_NCCALCSIZE, the first object at lParam is the
    // proposed RECT. With wParam=TRUE it is rgrc[0] in NCCALCSIZE_PARAMS.
    const GuestAddr rect = static_cast<GuestAddr>(lParam);
    i32 left = static_cast<i32>(m_memory.read_u32(rect + 0).value_or(0));
    i32 top = static_cast<i32>(m_memory.read_u32(rect + 4).value_or(0));
    i32 right = static_cast<i32>(m_memory.read_u32(rect + 8).value_or(0));
    i32 bottom = static_cast<i32>(m_memory.read_u32(rect + 12).value_or(0));

    // A minimized window has no client area: DefWindowProc collapses it to
    // the window's top-left corner (measured, s1win_msgseq2_probe
    // create_popup_minimize_hidden: WM_SIZE 0x0 after WM_CREATE).
    if (win->style & WS::MINIMIZE) {
        (void)m_memory.write_u32(rect + 8, static_cast<u32>(left));
        (void)m_memory.write_u32(rect + 12, static_cast<u32>(top));
        return 0;
    }

    // The same frame AdjustWindowRectEx adds (nc_metrics.hpp), plus the
    // scroll bars, which are non-client too.
    const auto in = nc_metrics::insets(win->style, win->ex_style, win->has_menu, true);
    left += in.left;
    top += in.top;
    right -= in.right;
    bottom -= in.bottom;

    if (right < left) right = left;
    if (bottom < top) bottom = top;
    (void)m_memory.write_u32(rect + 0, static_cast<u32>(left));
    (void)m_memory.write_u32(rect + 4, static_cast<u32>(top));
    (void)m_memory.write_u32(rect + 8, static_cast<u32>(right));
    (void)m_memory.write_u32(rect + 12, static_cast<u32>(bottom));
    return 0;
}

// -----------------------------------------------------------------------------
// handle_close
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_close(HWND hwnd) {
    m_destroy_triggered = true;

    // Win32 contract: WM_CLOSE default handler calls DestroyWindow synchronously.
    if (m_window_mgr) {
        m_window_mgr->destroy_window(hwnd);
    } else {
        // Fallback: post WM_DESTROY if WindowManager not available
        MSG msg{};
        msg.hwnd    = hwnd;
        msg.message = WM::DESTROY;
        m_queue.post(msg);
    }

    return 0;
}

// -----------------------------------------------------------------------------
// handle_nchittest
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_nchittest(HWND /* hwnd */, u64 /* lParam */) {
    // Everything is client area for now
    return HT::CLIENT;
}

// -----------------------------------------------------------------------------
// handle_gettext
// -----------------------------------------------------------------------------

namespace {
/// Set while DefWindowProcA runs: its text messages use CP_ACP strings.
thread_local bool t_dwp_ansi = false;
} // namespace

u64 DefWindowProcManager::handle_gettext(HWND hwnd, u64 wParam, u64 lParam) {
    // WM_GETTEXT: wParam is the buffer size in characters (including the
    // NUL); the result is the number copied. Measured (s1win_misc_probe
    // dwp_gettext_*): a zero-sized buffer copies nothing and returns 0.
    const WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return 0;
    }
    const u32 max_count = static_cast<u32>(wParam);
    if (max_count == 0 || lParam == 0) {
        return 0;
    }
    const GuestAddr buf = static_cast<GuestAddr>(lParam);
    if (t_dwp_ansi) {
        const std::string text = ansi_text::to_ansi(win->title);
        const u32 n = std::min(static_cast<u32>(text.size()), max_count - 1);
        for (u32 i = 0; i < n; ++i) (void)m_memory.write_u8(buf + i, static_cast<u8>(text[i]));
        (void)m_memory.write_u8(buf + n, 0);
        return n;
    }
    const u32 n = std::min(static_cast<u32>(win->title.size()), max_count - 1);
    for (u32 i = 0; i < n; ++i) (void)m_memory.write_u16(buf + i * 2, static_cast<u16>(win->title[i]));
    (void)m_memory.write_u16(buf + n * 2, 0);
    return n;
}

// -----------------------------------------------------------------------------
// handle_settext
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_settext(HWND hwnd, u64 lParam) {
    // WM_SETTEXT: a NULL string clears the text (measured,
    // dwp_settext_null: TRUE, length 0 afterwards).
    WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return 0;
    }
    std::wstring new_title;
    if (lParam != 0) {
        const GuestAddr ptr = static_cast<GuestAddr>(lParam);
        if (t_dwp_ansi) {
            std::string bytes;
            for (u32 off = 0; off < 65536; ++off) {
                auto c = m_memory.read_u8(ptr + off);
                if (c.is_err() || c.value() == 0) break;
                bytes.push_back(static_cast<char>(c.value()));
            }
            new_title = ansi_text::to_wide(bytes);
        } else {
            for (u32 off = 0; off < 32768; off++) {
                auto ch_r = m_memory.read_u16(ptr + off * 2);
                if (ch_r.is_err() || ch_r.value() == 0) break;
                new_title.push_back(static_cast<wchar_t>(ch_r.value()));
            }
        }
    }
    win->title = std::move(new_title);
    // Learn (WM_SETTEXT): DefWindowProc sets AND displays the window text.
    // VCL forms set their caption this way, not through SetWindowText.
    if (m_display_text) m_display_text(hwnd, win->title);
    // Measured (winevent_sequence_probe): a text change raises
    // EVENT_OBJECT_NAMECHANGE on the window.
    constexpr u32 EVENT_OBJECT_NAMECHANGE = 0x800Cu;
    if (m_window_mgr) m_window_mgr->raise_win_event(EVENT_OBJECT_NAMECHANGE, hwnd);
    return 1;
}

// -----------------------------------------------------------------------------
// handle_gettextlength
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_gettextlength(HWND hwnd) {
    const WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return 0;
    }
    if (t_dwp_ansi) return static_cast<u64>(ansi_text::to_ansi(win->title).size());
    return static_cast<u64>(win->title.size());
}

// -----------------------------------------------------------------------------
// handle_erasebkgnd
// -----------------------------------------------------------------------------


u64 DefWindowProcManager::handle_erasebkgnd(HWND hwnd) {
    // If we don't have GDI or class managers, just claim success
    if (!m_gdi_mgr || !m_wclass_mgr) return 1;

    // Find the window to get its class atom
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) return 1;

    // Look up the class to get the background brush
    const WindowClassData* cls = m_wclass_mgr->find_by_atom(win->class_atom);
    if (!cls) return 1;

    u32 hbrush = cls->hbr_background;
    if (hbrush == 0) return 1; // No background brush — don't erase

    // Determine the color. Learn (WNDCLASSEXW): hbrBackground is a brush
    // handle or a system colour value, COLOR_xxx + 1. The colour values come
    // first, as on Windows; no GDI handle is that small (kFirstGdiHandle).
    u32 color = 0xFFFFFF; // Default white
    if (const auto sys = sys_colors::from_brush(hbrush)) {
        color = *sys;
    } else if (m_gdi_mgr->lookup_brush_color(static_cast<Handle>(hbrush), &color)) {
        // A real GDI brush handle
    } else {
        // Unknown brush — use window background (white)
        color = 0xFFFFFF;
    }

    // Get the window DC and fill the client rect
    Handle hdc = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
    if (hdc == 0) return 1;

    // Get client dimensions from window
    i32 width = win->client_rect.right - win->client_rect.left;
    i32 height = win->client_rect.bottom - win->client_rect.top;

    // Fill the background
    if (!m_gdi_mgr->fill_rect_with_brush(
            hdc, 0, 0, width, height, static_cast<Handle>(hbrush))) {
        m_gdi_mgr->fill_rect_gdi(hdc, 0, 0, width, height, color);
    }

    return 1; // Background was erased
}

// -----------------------------------------------------------------------------
// handle_queryopen
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_queryopen() {
    return 1; // TRUE
}

// -----------------------------------------------------------------------------
// handle_syscommand
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_nclbuttondown(HWND hwnd, u64 wParam) {
    // Learn DefWindowProc: WM_NCLBUTTONDOWN on HTCAPTION begins a modal move.
    // A guest that paints its own caption reports HTCLIENT for that strip and
    // drags with the documented `ReleaseCapture(); SendMessage(hwnd,
    // WM_NCLBUTTONDOWN, HTCAPTION, ...)` idiom, so this is the only path that
    // moves such a window.
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] WM_NCLBUTTONDOWN hwnd=0x%x hit=%llu\n",
                     hwnd, static_cast<unsigned long long>(wParam & 0xFFFF));
    }
    const u64 ht = wParam & 0xFFFF;

    // The eight sizing borders. Learn DefWindowProc begins a modal SIZE loop
    // here; the host does the equivalent natively. A border hit that falls
    // through to the HTCAPTION test returns 0, so a window that paints its
    // own frame and reports these hits cannot be resized.
    if (HT::is_sizing_border(ht)) {
        if (m_begin_window_resize) {
            (void)m_begin_window_resize(hwnd, static_cast<u32>(ht));
        }
        return 0;
    }
    if (ht != HT::CAPTION) {
        return 0;
    }
    if (m_begin_window_drag) {
        (void)m_begin_window_drag(hwnd);
    }
    return 0;
}

u64 DefWindowProcManager::handle_syscommand(HWND hwnd, u64 wParam) {
    u64 cmd = wParam & 0xFFF0; // Mask out menu handle bits
    if (cmd == SC::SIZE) {
        // SC_SIZE carries the edge in the low nibble as 1..8 (WMSZ_LEFT..
        // WMSZ_BOTTOMRIGHT), the keyboard/menu equivalent of grabbing a
        // border. WMSZ_* and HT::LEFT..BOTTOMRIGHT are both ordered
        // left,right,top,topleft,topright,bottom,bottomleft,bottomright, so
        // the mapping is a constant offset. A bare SC_SIZE (nibble 0) is the
        // keyboard "Size" menu command with no edge chosen yet; there is no
        // keyboard sizing mode here, so treat it as the bottom-right grip.
        const u64 wmsz = wParam & 0x000F;
        const u32 edge = (wmsz >= 1 && wmsz <= 8)
                             ? static_cast<u32>(HT::LEFT + (wmsz - 1))
                             : static_cast<u32>(HT::BOTTOMRIGHT);
        if (m_begin_window_resize) {
            (void)m_begin_window_resize(hwnd, edge);
        }
        return 0;
    }
    if (cmd == SC::MOVE) {
        // The keyboard/menu equivalent of the caption drag above.
        if (m_begin_window_drag) {
            (void)m_begin_window_drag(hwnd);
        }
        return 0;
    }
    if (cmd == SC::CLOSE) {
        // Post WM_CLOSE to the window
        MSG msg{};
        msg.hwnd    = hwnd;
        msg.message = WM::CLOSE;
        m_queue.post(msg);
        return 0;
    }
    if (m_syscommand_show) {
        if (cmd == SC::RESTORE) {
            m_syscommand_show(hwnd, SW::RESTORE);
            return 0;
        }
        if (cmd == SC::MINIMIZE) {
            m_syscommand_show(hwnd, SW::MINIMIZE);
            return 0;
        }
        if (cmd == SC::MAXIMIZE) {
            m_syscommand_show(hwnd, SW::SHOWMAXIMIZED);
            return 0;
        }
    }
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] DefWindowProc unhandled SC cmd=0x%llx hwnd=0x%x\n",
                     static_cast<unsigned long long>(cmd), hwnd);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// handle_paint
// -----------------------------------------------------------------------------

u64 DefWindowProcManager::handle_paint(HWND hwnd) {
    // Win32 default: call BeginPaint/EndPaint to validate the update region.
    // This prevents infinite WM_PAINT loops when the app doesn't handle WM_PAINT.
    if (m_paint_mgr) {
        m_paint_mgr->validate_rect(static_cast<u64>(hwnd), nullptr);
    }
    return 0;
}

// =============================================================================
// Thunk Implementations
// =============================================================================

static void def_window_proc_thunk(Marshaler& m, DefWindowProcManager& dwp, bool ansi) {
    auto hwnd   = static_cast<windowing::HWND>(m.arg(0));
    auto msg    = static_cast<u32>(m.arg(1));
    auto wParam = m.arg(2);
    auto lParam = m.arg(3);
    // Measured (s1win_misc_probe dwp_invalid_hwnd): 0 /
    // ERROR_INVALID_WINDOW_HANDLE for a window that does not exist.
    if (!dwp.window_exists(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    dwp.set_is_64bit(m.cpu_is_64bit());
    const bool previous = t_dwp_ansi;
    t_dwp_ansi = ansi;
    u64 result = dwp.def_window_proc(hwnd, msg, wParam, lParam);
    t_dwp_ansi = previous;
    m.set_return(static_cast<u64>(result));
}

void impl_DefWindowProcW(Marshaler& m, DefWindowProcManager& dwp) {
    def_window_proc_thunk(m, dwp, false);
}

void impl_DefWindowProcA(Marshaler& m, DefWindowProcManager& dwp) {
    // The text messages (WM_SETTEXT / WM_GETTEXT / WM_GETTEXTLENGTH) carry
    // CP_ACP strings; everything else is shared with the W form.
    def_window_proc_thunk(m, dwp, true);
}

// DefDlgProc provides the default processing for dialog windows whose custom
// window proc (e.g. PuTTY's config-box class) falls through to it. It is the
// default-window-proc behavior plus dialog extras; for the messages that matter
// to real dialogs (notably WM_PAINT, which MUST be validated or the window
// re-posts WM_PAINT forever) routing to def_window_proc is correct. Previously
// DefDlgProcA/W were missing-export stubs returning a bogus value, so dialog
// WM_PAINT never validated (paint storm) and no default dialog behavior ran.
u64 DefWindowProcManager::def_dlg_proc(HWND hwnd, u32 msg, u64 wParam,
                                       u64 lParam) {
    if (msg == WM::ACTIVATE) {
        // Learn DefDlgProc WM_ACTIVATE: restores focus to the saved control
        // when activated, otherwise saves the control that has focus.
        if (m_dialog_activate) {
            m_dialog_activate(hwnd, (wParam & 0xFFFFu) != WA::INACTIVE);
        }
        return 0;
    }
    return def_window_proc(hwnd, msg, wParam, lParam);
}

void impl_DefDlgProcW(Marshaler& m, DefWindowProcManager& dwp) {
    auto hwnd   = static_cast<windowing::HWND>(m.arg(0));
    auto msg    = static_cast<u32>(m.arg(1));
    auto wParam = m.arg(2);
    auto lParam = m.arg(3);
    dwp.set_is_64bit(m.cpu_is_64bit());
    u64 result = dwp.def_dlg_proc(hwnd, msg, wParam, lParam);
    m.set_return(static_cast<u64>(result));
}

void impl_DefDlgProcA(Marshaler& m, DefWindowProcManager& dwp) {
    const bool previous = t_dwp_ansi;
    t_dwp_ansi = true;
    impl_DefDlgProcW(m, dwp);
    t_dwp_ansi = previous;
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_def_window_proc_exports(DefWindowProcManager& dwp) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "DefWindowProcW", 1,
        [&dwp](Marshaler& m) { impl_DefWindowProcW(m, dwp); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "DefWindowProcA", 2,
        [&dwp](Marshaler& m) { impl_DefWindowProcA(m, dwp); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "DefDlgProcW", 0,
        [&dwp](Marshaler& m) { impl_DefDlgProcW(m, dwp); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "DefDlgProcA", 0,
        [&dwp](Marshaler& m) { impl_DefDlgProcA(m, dwp); },
        16
    });
    return exports;
}

} // namespace vitreo::winapi
