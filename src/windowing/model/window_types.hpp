#pragma once
/// @file window_types.hpp
/// @brief Common windowing types and constants (WINDOW-001)

#include <vitreo/common.hpp>
#include <string>
#include <optional>
#include <vector>

namespace vitreo::windowing {

// HWND type — opaque handle value
using HWND = u32;
using ATOM = u16;

// Special HWND values.  The desktop is a real, non-NULL HWND and must not
// overlap the allocated window range (which starts at 0x00010000).
constexpr HWND HWND_DESKTOP   = 0x0000FFFE;
constexpr HWND HWND_BROADCAST = 0xFFFF;
constexpr HWND HWND_TOP       = 0;
constexpr HWND HWND_BOTTOM    = 1;
constexpr HWND HWND_TOPMOST   = static_cast<HWND>(-1);
constexpr HWND HWND_NOTOPMOST = static_cast<HWND>(-2);
/// Parent for message-only windows (CreateWindowEx hWndParent). Learn:
/// https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features
constexpr HWND HWND_MESSAGE   = static_cast<HWND>(-3);
constexpr HWND NULL_HWND      = 0;

// Window styles (WS_*)
namespace WS {
    constexpr u32 OVERLAPPED   = 0x00000000;
    constexpr u32 POPUP        = 0x80000000;
    constexpr u32 CHILD        = 0x40000000;
    constexpr u32 MINIMIZE     = 0x20000000;
    constexpr u32 VISIBLE      = 0x10000000;
    constexpr u32 DISABLED     = 0x08000000;
    constexpr u32 CLIPSIBLINGS = 0x04000000;
    constexpr u32 CLIPCHILDREN = 0x02000000;
    constexpr u32 MAXIMIZE     = 0x01000000;
    constexpr u32 CAPTION      = 0x00C00000;
    constexpr u32 BORDER       = 0x00800000;
    constexpr u32 DLGFRAME     = 0x00400000;
    constexpr u32 VSCROLL      = 0x00200000;
    constexpr u32 HSCROLL      = 0x00100000;
    constexpr u32 SYSMENU      = 0x00080000;
    constexpr u32 THICKFRAME   = 0x00040000;
    constexpr u32 MINIMIZEBOX  = 0x00020000;
    constexpr u32 MAXIMIZEBOX  = 0x00010000;
    constexpr u32 OVERLAPPEDWINDOW = OVERLAPPED | CAPTION | SYSMENU
                                   | THICKFRAME | MINIMIZEBOX | MAXIMIZEBOX;
    constexpr u32 POPUPWINDOW  = POPUP | BORDER | SYSMENU;
} // namespace WS

// Rect structure
struct Rect {
    i32 left   = 0;
    i32 top    = 0;
    i32 right  = 0;
    i32 bottom = 0;

    [[nodiscard]] i32 width()  const noexcept { return right - left; }
    [[nodiscard]] i32 height() const noexcept { return bottom - top; }
};

// Point structure
struct Point {
    i32 x = 0;
    i32 y = 0;
};

struct ScrollBarState {
    i32 minimum = 0, maximum = 100, position = 0;
    u32 page = 0;
    bool initialized = false, visible = false, enabled = true;
};

// Window data stored per HWND
struct WindowData {
    struct OwnerDrawList {
        std::vector<u64> items;
        /// Item text, parallel to `items`, kept only for LBS_HASSTRINGS.
        /// LBS_OWNERDRAWFIXED|LBS_HASSTRINGS is a normal combination: the
        /// control stores the strings and the parent still draws each row, so
        /// LB_GETTEXT has to work.
        std::vector<std::wstring> strings;
        /// Per-item heights from WM_MEASUREITEM, parallel to `items`, for
        /// LBS_OWNERDRAWVARIABLE (fixed lists use item_height).
        std::vector<i32> heights;
        i32 selection = -1, top = 0, item_height = 16;
        bool measured = false, redraw = true, painting = false;
        /// The host was told this list takes its own mouse input.
        bool input_claimed = false;
    } owner_draw_list;
    ScrollBarState scroll_bars[2]; // SB_HORZ / SB_VERT, lifetime belongs to HWND.
    /// HFONT from WM_SETFONT. Windows selects it into the DC before the
    /// control (or its owner, via WM_DRAWITEM) draws; WM_GETFONT returns it.
    u64 hfont = 0;
    HWND handle      = NULL_HWND;
    HWND parent      = HWND_DESKTOP;
    HWND owner       = NULL_HWND;
    /// Guest thread that created this window. Win32 routes posted window
    /// messages, paint work, and window timers to this thread's message queue.
    u32  owner_thread_id = 1;
    ATOM class_atom   = 0;
    /// Registration serial of the window's class (WindowClassManager). Two
    /// modules may register the same class name and so share one atom; the
    /// serial keeps GetClassLong/SetClassLong on the window's own class.
    u32  class_id     = 0;
    /// hInstance passed to CreateWindowEx (GWLP_HINSTANCE). Measured: a
    /// system-class window reports the creator's instance, not user32's
    /// (s1win_class_probe button_window_hinstance_is_caller).
    u64  hinstance    = 0;
    /// cbWndExtra allocated when the window was created. SetClassLong(
    /// GCL_CBWNDEXTRA) does not resize existing windows (Learn:
    /// SetClassLongPtrW), so index validation uses this, not the class.
    u32  wnd_extra_size = 0;
    /// IsWindowUnicode: the class charset, or for a system class the charset
    /// of the CreateWindowEx call; SetWindowLongPtrA/W(GWLP_WNDPROC) switches
    /// it (measured, s1win_class_probe / s1win_api_probe).
    bool unicode      = true;
    /// ShowWindow still owes the first WM_SIZE / WM_MOVE: CreateWindowEx sends
    /// them at once for pop-up and child windows but leaves an overlapped
    /// window's for its first ShowWindow (measured, s1win_msgseq2_probe
    /// create_* and s1win_msgseq_probe showwindow_shownoactivate).
    bool pending_size_move = false;
    /// The colour DefWindowProc's WM_ERASEBKGND would paint from the class
    /// brush (COLORREF), or nullopt for a NULL brush. Decides what a window
    /// shows before it has painted: that colour, or Windows' unpainted black.
    std::optional<u32> erase_color;
    /// Class name as returned by GetClassName (MS Learn system classes use
    /// mixed case: "Button", "Static", "#32770", …). A wizard compares
    /// these strings exactly when handling page WM_COMMAND.
    std::wstring class_name;
    /// Base native control initialized through a saved system WNDPROC.
    /// Separate from the guest-visible superclass identity above.
    std::wstring native_control_class;
    [[nodiscard]] const std::wstring& control_class_name() const noexcept {
        return native_control_class.empty() ? class_name : native_control_class;
    }
    u32  style       = 0;
    u32  ex_style    = 0;
    /// Registered class style (CS_*), distinct from the WS_* window style.
    /// CS_DBLCLKS (0x0008) decides whether the window receives
    /// WM_LBUTTONDBLCLK / WM_RBUTTONDBLCLK.
    u32  class_style = 0;
    Rect rect        = {};
    Rect client_rect = {};
    /// Client-area top-left relative to rect. Unlike style-derived metrics,
    /// this preserves the rectangle returned by WM_NCCALCSIZE for custom
    /// non-client layouts.
    Point client_offset = {};
    /// True when WM_NCCALCSIZE makes the client occupy the full outer rect,
    /// removing the standard frame/caption.
    bool client_drawn_frame = false;
    std::wstring title;
    /// EM_SETLIMITTEXT cap. Learn default is 32,767 TCHARs. Limits user
    /// typing only — does not truncate existing text or WM_SETTEXT.
    u32  edit_text_limit = 32767;
    u64  wndproc     = 0;   // Guest function pointer
    u64  user_data   = 0;   // GWLP_USERDATA (application-defined)
    u64  msg_result  = 0;   // DWL_MSGRESULT (dialog message result)
    u32  ctrl_id     = 0;   // Control ID (hMenu for WS_CHILD, from dialog template id)
    bool visible     = false;
    bool hidden_by_owner = false; // hidden due to owner minimize/ShowOwnedPopups(FALSE)
    bool enabled     = true;
    /// Windows-visible show state. These mirror WS_MINIMIZE / WS_MAXIMIZE but
    /// are kept explicit so placement retains the normal rectangle while the
    /// current rect represents a minimized or maximized presentation.
    bool minimized   = false;
    bool maximized   = false;
    bool restore_to_maximized = false;
    Point minimized_position{-1, -1};
    Point maximized_position{-1, -1};
    Rect normal_rect = {};
    Rect normal_client_rect = {};
    bool has_menu    = false;  // True if a menu bar is attached (affects non-client size)
    bool is_dialog   = false;  // Created by DialogManager; enables dialog-only native layout handling
    /// Created with hWndParent == HWND_MESSAGE. No desktop ownership, no
    /// native NSWindow. Message-only HWNDs are how a UI-thread pump and its
    /// message window receive posted work.
    bool is_message_only = false;
    /// UxTheme WTNCA_* flags applied to this window's non-client area.
    ///
    /// SetWindowThemeAttribute updates these through its dwMask contract.  The
    /// Cocoa bridge may consume them when drawing native non-client chrome;
    /// retaining them here makes the Windows-visible state belong to the HWND.
    u32 theme_nonclient_flags = 0;
    std::vector<HWND> children;
    // Per-window "extra bytes" (the class's cbWndExtra region). Get/SetWindowLong(Ptr)
    // at positive offsets read/write here. Grown lazily on first write. A
    // dialog stashes a context pointer here via SetWindowLongPtr and
    // reads it back in its wndproc; dropping it sends the guest into an
    // infinite loop.
    std::vector<u8> extra_bytes;
};

// ShowWindow constants
namespace SW {
    constexpr int HIDE            = 0;
    constexpr int SHOWNORMAL      = 1;
    constexpr int SHOWMINIMIZED   = 2;
    constexpr int SHOWMAXIMIZED   = 3;
    constexpr int SHOWNOACTIVATE  = 4;
    constexpr int SHOW            = 5;
    constexpr int MINIMIZE        = 6;
    constexpr int SHOWMINNOACTIVE = 7;
    constexpr int SHOWNA          = 8;
    constexpr int RESTORE         = 9;
    constexpr int SHOWDEFAULT     = 10;
    constexpr int FORCEMINIMIZE   = 11;
} // namespace SW

/// One button of a task dialog: the id TaskDialogIndirect reports when it is
/// pressed, and the label to show. Lives here rather than on CocoaBridge so
/// the WinAPI layer can name it without depending on the Cocoa header.
struct TaskDialogButton {
    int id{0};
    std::string label;
};

/// Whether a WM_NCCALCSIZE result hands the frame to the guest
/// (.docs/windowing-native-presentation.md, GuestOwnedFrame): either the whole
/// window is client, or the window has WS_CAPTION but no caption band is left
/// above the client -- its top inset is at most the frame border. The second is
/// the Windows 10+ custom title bar (Chromium/Electron frameless windows): the
/// client takes the caption and keeps only the standard left/right/bottom
/// resize borders, which DWM draws invisibly, and when maximized the top inset
/// is just the off-screen border. The guest paints its own caption either way,
/// so a native title bar on top would be double chrome. The border widths are
/// the default frame recalculate_client_area uses (8 sizing, 4 dialog, 1).
[[nodiscard]] inline bool nccalcsize_guest_owns_frame(
        const Rect& outer, const Rect& client,
        u32 style) noexcept {
    if (client.left == outer.left && client.top == outer.top &&
        client.right == outer.right && client.bottom == outer.bottom) {
        return true;
    }
    if ((style & WS::CAPTION) != WS::CAPTION) return false;
    const i32 border = (style & WS::THICKFRAME) ? 8
                       : (style & WS::DLGFRAME) ? 4
                       : (style & WS::BORDER)   ? 1 : 0;
    return client.top - outer.top <= border;
}

} // namespace vitreo::windowing
