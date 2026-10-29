#pragma once
/// @file window.hpp
/// @brief CreateWindowExW/DestroyWindow implementation (WIN32-051)

#include <vitreo/common.hpp>
#include <functional>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"

namespace vitreo::winapi {

class Marshaler;
class PaintingManager;
constexpr i32 CW_USEDEFAULT = static_cast<i32>(0x80000000);

// Scratch region for CREATESTRUCT passed to WM_NCCREATE/WM_CREATE (48 bytes)
// Only one window is created at a time so a single 64-byte slot is sufficient.
static constexpr GuestAddr USER32_CREATESTRUCT_BASE = 0x5D700000;
static constexpr u32       USER32_CREATESTRUCT_SIZE = 0x1000; // 4 KB

// Scratch region for WINDOWPOS passed to WM_WINDOWPOSCHANGING/CHANGED.
// Each WINDOWPOS is 28 bytes; a single page provides plenty of slots so that
// nested SetWindowPos calls (e.g. a wndproc that calls SetWindowPos again
// during WM_WINDOWPOSCHANGING) do not overwrite a parent struct still in use.
static constexpr GuestAddr USER32_WINDOWPOS_BASE = 0x5D701000;
static constexpr u32       USER32_WINDOWPOS_SIZE = 0x1000; // 4 KB

/// Callback for synchronous message send (WM_NCCREATE/WM_CREATE)
/// Signature: (hwnd, msg, wParam, lParam, wndproc) -> LRESULT
using WndProcCallback = std::function<u64(windowing::HWND, u32, u64, u64, u64)>;

/// Callback invoked when CreateWindowEx needs to auto-load a menu from
/// the WNDCLASS lpszMenuName.  Signature: (hwnd, menu_name)
using MenuLoadCallback = std::function<void(windowing::HWND, const std::wstring&)>;
using WindowLifecycleCallback =
    std::function<void(bool, windowing::HWND)>;
using WindowActivationCallback = std::function<void(windowing::HWND)>;

/// The window-position engine (User32ExtManager) that CreateWindowEx and
/// DestroyWindow hand their show / state changes to, so they produce the same
/// WM_SHOWWINDOW / WM_WINDOWPOS* / activation sequence as ShowWindow and
/// SetWindowPos (measured, s1win_msgseq2_probe create_* / destroy_*).
struct WindowOps {
    /// ShowWindow(hwnd, cmd).
    std::function<u64(windowing::HWND, int)> show;
    /// SetWindowPos with the caller's flags (HWND_TOP insert-after).
    std::function<void(windowing::HWND, i32, i32, i32, i32, u32)> set_pos;
    /// Apply WS_MINIMIZE / WS_MAXIMIZE given at creation.
    std::function<void(windowing::HWND, bool /*minimize*/)> initial_state;
};

class WindowManager {
public:
    WindowManager(
        windowing::HwndManager& hwnd_mgr,
        WindowClassManager& class_mgr,
        windowing::CocoaBridge& bridge,
        windowing::MessageQueue& queue
    );

    bool initialize_system_control(Marshaler& m, windowing::HWND hwnd,
                                   std::wstring_view base_class, u64 create_struct, bool ansi);

    /// Set callback for synchronous WndProc dispatch during window creation
    void set_wndproc_callback(WndProcCallback cb) { m_wndproc_callback = std::move(cb); }

    /// Set callback for auto-loading a menu from a WNDCLASS lpszMenuName
    void set_menu_load_callback(MenuLoadCallback cb) { m_menu_load_callback = std::move(cb); }
    void set_lifecycle_callback(WindowLifecycleCallback cb) {
        m_lifecycle_callback = std::move(cb);
    }
    void set_activation_callback(WindowActivationCallback cb) {
        m_activation_callback = std::move(cb);
    }
    /// Sink for the WinEvents window lifetime raises (EVENT_OBJECT_CREATE /
    /// DESTROY / NAMECHANGE; tools/oracle_probe/winevent_sequence_probe).
    void set_win_event_callback(std::function<void(u32, windowing::HWND)> cb) {
        m_win_event_callback = std::move(cb);
    }
    void raise_win_event(u32 event, windowing::HWND hwnd) const {
        if (m_win_event_callback) m_win_event_callback(event, hwnd);
    }
    /// Called before WM_DESTROY so activation and focus leave the window
    /// while it is still valid.
    void set_destroy_activation_callback(WindowActivationCallback cb) {
        m_destroy_activation_callback = std::move(cb);
    }
    /// Called for every window after WM_NCDESTROY, before its HWND is freed.
    void set_destroyed_callback(std::function<void(windowing::HWND)> cb) {
        m_destroyed_callback = std::move(cb);
    }
    void set_painting_manager(PaintingManager* mgr) {
        m_paint_mgr = mgr;
    }

    void set_createstruct_scratch(GuestAddr address) { m_createstruct_scratch = address; }
    void set_window_ops(WindowOps ops) { m_ops = std::move(ops); }

    /// IsMenu: CreateWindowEx rejects a non-child window's hMenu that is not a
    /// menu handle with ERROR_INVALID_MENU_HANDLE (measured,
    /// s1win_style_probe create_popup_bad_menu).
    void set_menu_validator(std::function<bool(u64)> fn) { m_menu_validator = std::move(fn); }

    /// Child layered windows need a manifest that targets Windows 8 or later
    /// (Learn: Window Features, "Layered Windows").
    void set_child_layered_allowed(bool allowed) { m_child_layered_allowed = allowed; }

    /// CreateWindowExW core logic
    /// Returns HWND or NULL_HWND on failure
    /// marshaler_ptr: if non-null, used to write CREATESTRUCT to guest scratch memory
    windowing::HWND create_window(
        u32 ex_style,
        std::wstring_view class_name,
        std::wstring_view window_name,
        u32 style,
        i32 x, i32 y,
        i32 width, i32 height,
        windowing::HWND parent,
        u64 menu,
        u64 hinstance,
        u64 param,        // lpCreateParams
        GuestAddr cls_guest_ptr  = 0,  // original guest class name ptr (for CREATESTRUCT)
        GuestAddr name_guest_ptr = 0,  // original guest window name ptr (for CREATESTRUCT)
        Marshaler* marshaler = nullptr, // caller-owned; survives nested/concurrent creates
        bool ansi_api = false          // CreateWindowExA: charset of a system-class window
    );

    /// DestroyWindow core logic
    bool destroy_window(windowing::HWND hwnd);

private:
    windowing::HwndManager& m_hwnd_mgr;
    WindowClassManager& m_class_mgr;
    windowing::CocoaBridge& m_bridge;
    windowing::MessageQueue& m_queue;
    WndProcCallback m_wndproc_callback;
    MenuLoadCallback m_menu_load_callback;
    WindowLifecycleCallback m_lifecycle_callback;
    WindowActivationCallback m_activation_callback;
    WindowActivationCallback m_destroy_activation_callback;
    PaintingManager* m_paint_mgr{nullptr};
    GuestAddr m_createstruct_scratch{0};
    std::function<bool(u64)> m_menu_validator;
    bool m_child_layered_allowed{true};
    std::function<void(windowing::HWND)> m_destroyed_callback;
    WindowOps m_ops;
    std::function<void(u32, windowing::HWND)> m_win_event_callback;

    /// Sends WM_PARENTNOTIFY(event) for @p hwnd to its parent and every
    /// ancestor while the window it concerns is a child without
    /// WS_EX_NOPARENTNOTIFY (Learn: WM_PARENTNOTIFY).
    void send_parent_notify(windowing::HWND hwnd, u32 event);
    /// DestroyWindow's teardown of @p hwnd and its descendants, after the
    /// top-level call has hidden it and destroyed its owned windows.
    void destroy_tree(windowing::HWND hwnd);

    // Write CREATESTRUCT to guest scratch memory, return guest addr or 0
    GuestAddr write_createstruct(Marshaler* marshaler, u32 ex_style, const std::wstring& class_name,
                                  const std::wstring& window_name, u32 style,
                                  i32 x, i32 y, i32 w, i32 h,
                                  windowing::HWND parent, u64 menu, u64 hinstance,
                                  u64 param, GuestAddr cls_ptr, GuestAddr name_ptr);

    // Resolve CW_USEDEFAULT to actual values.  Only overlapped windows get
    // system-chosen placement; pop-up and child windows resolve it to zero.
    void resolve_defaults(i32& x, i32& y, i32& w, i32& h,
                          bool overlapped) const;
};

// Thunks
void impl_CreateWindowExW(Marshaler& m, WindowManager& wm);
void impl_DestroyWindow(Marshaler& m, WindowManager& wm);

[[nodiscard]] DllExports make_window_exports(WindowManager& wm);

} // namespace vitreo::winapi
