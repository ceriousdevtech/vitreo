#pragma once
/// @file def_window_proc.hpp
/// @brief DefWindowProcW default message handling (WIN32-054)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"
#include "core/memory/guest_memory.hpp"

#include <functional>

namespace vitreo::winapi {
    class GdiManager;
    class MessageSendManager;
    class WindowClassManager;
    class WindowManager;
    class PaintingManager;
}

namespace vitreo::winapi {

class Marshaler;

// Hit test results
namespace HT {
    constexpr u64 ERROR_VAL   = static_cast<u64>(-2); // HTERROR
    constexpr u64 TRANSPARENT = static_cast<u64>(-1); // HTTRANSPARENT
    constexpr u64 NOWHERE     = 0;
    constexpr u64 CLIENT      = 1;
    constexpr u64 CAPTION     = 2;
    constexpr u64 SYSMENU     = 3;
    constexpr u64 GROWBOX     = 4;   // == HTSIZE
    constexpr u64 MENU        = 5;
    constexpr u64 HSCROLL     = 6;
    constexpr u64 VSCROLL     = 7;
    constexpr u64 MINBUTTON   = 8;
    constexpr u64 MAXBUTTON   = 9;
    // The sizing border codes. A guest that paints its own frame reports
    // these from its WM_NCHITTEST, and until they existed here every one of
    // them fell through handle_nclbuttondown's HTCAPTION test and did
    // nothing -- which is the whole of "the window cannot be resized".
    constexpr u64 LEFT        = 10;
    constexpr u64 RIGHT       = 11;
    constexpr u64 TOP         = 12;
    constexpr u64 TOPLEFT     = 13;
    constexpr u64 TOPRIGHT    = 14;
    constexpr u64 BOTTOM      = 15;
    constexpr u64 BOTTOMLEFT  = 16;
    constexpr u64 BOTTOMRIGHT = 17;
    constexpr u64 BORDER      = 18;
    constexpr u64 CLOSE       = 20;
    constexpr u64 HELP        = 21;

    /// True for the eight sizing borders/corners above.
    [[nodiscard]] constexpr bool is_sizing_border(u64 ht) noexcept {
        return ht >= LEFT && ht <= BOTTOMRIGHT;
    }
}

// System command values
namespace SC {
    constexpr u64 CLOSE    = 0xF060;
    constexpr u64 SIZE     = 0xF000;
    constexpr u64 MOVE     = 0xF010;
    constexpr u64 MINIMIZE = 0xF020;
    constexpr u64 MAXIMIZE = 0xF030;
    constexpr u64 RESTORE  = 0xF120;
}

class DefWindowProcManager {
public:
    /// A window DefWindowProc can act on (local, or the desktop).
    [[nodiscard]] bool window_exists(windowing::HWND hwnd) const {
        return hwnd == windowing::HWND_DESKTOP || m_hwnd_mgr.find(hwnd) != nullptr;
    }
    DefWindowProcManager(
        windowing::HwndManager& hwnd_mgr,
        windowing::MessageQueue& queue,
        GuestMemory& memory
    );

    /// Default window procedure. Returns LRESULT.
    u64 def_window_proc(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);

private:
    u64 handle_close(windowing::HWND hwnd);
    u64 handle_nccalcsize(windowing::HWND hwnd, u64 lParam);
    u64 handle_nchittest(windowing::HWND hwnd, u64 lParam);
    u64 handle_nclbuttondown(windowing::HWND hwnd, u64 wParam);
    u64 handle_gettext(windowing::HWND hwnd, u64 wParam, u64 lParam);
    u64 handle_settext(windowing::HWND hwnd, u64 lParam);
    u64 handle_gettextlength(windowing::HWND hwnd);
    u64 handle_erasebkgnd(windowing::HWND hwnd);
    u64 handle_queryopen();
    u64 handle_syscommand(windowing::HWND hwnd, u64 wParam);
    u64 handle_paint(windowing::HWND hwnd);
    u64 handle_setcursor(windowing::HWND hwnd, u64 lParam);
    u64 handle_windowposchanging(windowing::HWND hwnd, u64 lParam);
    u64 handle_windowposchanged(windowing::HWND hwnd, u64 lParam);

    windowing::HwndManager& m_hwnd_mgr;
    windowing::MessageQueue& m_queue;
    GuestMemory& m_memory;
    GdiManager* m_gdi_mgr = nullptr;
    WindowClassManager* m_wclass_mgr = nullptr;
    WindowManager* m_window_mgr = nullptr;
    PaintingManager* m_paint_mgr = nullptr;
    MessageSendManager* m_msgsend_mgr = nullptr;
    GuestAddr m_windowpos_scratch = 0;
    bool m_is_64bit = false;
    /// Applies a guest HCURSOR (0 hides) to the host cursor.
    std::function<void(u32 hcursor)> m_apply_cursor;
    std::function<void(windowing::HWND, int)> m_syscommand_show;
    std::function<bool(windowing::HWND)> m_begin_window_drag;
    std::function<bool(windowing::HWND, u32)> m_begin_window_resize;
    std::function<void(windowing::HWND)> m_focus_window;
    std::function<void(windowing::HWND, bool)> m_dialog_activate;
    std::function<void(windowing::HWND, const std::wstring&)> m_display_text;

    // Track if a DestroyWindow was triggered (for testing)
    bool m_destroy_triggered = false;
public:
    void set_is_64bit(bool value) { m_is_64bit = value; }
    bool was_destroy_triggered() const { return m_destroy_triggered; }
    void reset_destroy_flag() { m_destroy_triggered = false; }

    void set_gdi_manager(GdiManager* mgr) { m_gdi_mgr = mgr; }
    void set_wclass_manager(WindowClassManager* mgr) { m_wclass_mgr = mgr; }
    void set_window_manager(WindowManager* mgr) { m_window_mgr = mgr; }
    void set_paint_manager(PaintingManager* mgr) { m_paint_mgr = mgr; }
    void set_message_send_manager(MessageSendManager* mgr) {
        m_msgsend_mgr = mgr;
    }
    void set_windowpos_scratch(GuestAddr address) {
        m_windowpos_scratch = address;
    }
    void set_apply_cursor(std::function<void(u32 hcursor)> fn) {
        m_apply_cursor = std::move(fn);
    }
    /// DefWindowProc WM_SYSCOMMAND SC_RESTORE/SC_MINIMIZE/SC_MAXIMIZE
    /// (Learn: DefWindowProc carries out the predefined window-menu actions).
    void set_syscommand_show(std::function<void(windowing::HWND, int)> fn) {
        m_syscommand_show = std::move(fn);
    }

    /// Begin a user-driven window move for @p hwnd. Returns true when the
    /// host took ownership of the drag.
    /// Host hook for an interactive resize. `edge` is the HT sizing code
    /// (HT::LEFT .. HT::BOTTOMRIGHT) that names which border is being pulled.
    void set_begin_window_resize(
        std::function<bool(windowing::HWND, u32)> fn) {
        m_begin_window_resize = std::move(fn);
    }
    void set_begin_window_drag(std::function<bool(windowing::HWND)> fn) {
        m_begin_window_drag = std::move(fn);
    }
    /// SetFocus for DefWindowProc WM_ACTIVATE.
    void set_focus_window(std::function<void(windowing::HWND)> fn) {
        m_focus_window = std::move(fn);
    }
    /// DefDlgProc WM_ACTIVATE focus save/restore (hwnd, activating).
    void set_dialog_activate(std::function<void(windowing::HWND, bool)> fn) {
        m_dialog_activate = std::move(fn);
    }
    /// DefWindowProc WM_SETTEXT "sets and displays the window text" (Learn):
    /// pushes the new text to the host title bar / control.
    void set_display_text(
        std::function<void(windowing::HWND, const std::wstring&)> fn) {
        m_display_text = std::move(fn);
    }
    /// DefDlgProc: the dialog-specific defaults, then DefWindowProc.
    u64 def_dlg_proc(windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam);


};

// Thunks
void impl_DefWindowProcW(Marshaler& m, DefWindowProcManager& dwp);
void impl_DefWindowProcA(Marshaler& m, DefWindowProcManager& dwp);

[[nodiscard]] DllExports make_def_window_proc_exports(DefWindowProcManager& dwp);

} // namespace vitreo::winapi
