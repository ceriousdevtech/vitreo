#pragma once

#include "windowing/contracts/control_host.hpp"
#include "windowing/contracts/display_types.hpp"
#include "windowing/model/window_types.hpp"

#include <functional>

namespace vitreo::windowing {

/// Platform presentation operations used by USER32 window state and desktop
/// metrics. Win32 policy and HWND state remain owned by USER32.
class WindowHost : public ControlHost {
public:
    // Returns true when USER32 delivered the guest resize notifications.
    using NativeResizeCallback = std::function<bool(HWND, i32, i32)>;
    using NativeShowStateCallback =
        std::function<void(HWND, bool minimized, bool maximized)>;
    /// Resolve WM_NCHITTEST synchronously for guest-owned frames. The host
    /// needs the result while handling the originating native mouse-down so
    /// AppKit can own caption dragging and resizing.
    using NativeHitTestCallback = std::function<i32(HWND, i32, i32)>;
    /// The platform moved a top-level window itself (a user drag the host
    /// window manager ran, or a display change): the new CLIENT-area origin in
    /// Win32 screen coordinates. USER32 then updates the window position and
    /// sends the guest what a user move sends on Windows.
    using NativeMoveSizeCallback = std::function<void(HWND, bool entering)>;
    using NativeMoveCallback = std::function<void(HWND, i32 client_x, i32 client_y)>;

    virtual ~WindowHost() = default;

    virtual void set_visible(HWND hwnd, bool visible) = 0;
    /// Show a window for SW_SHOWNOACTIVATE / SW_SHOWNA / SWP_NOACTIVATE: on
    /// screen, but it must not become the key (active) window.
    virtual void show_without_activation(HWND hwnd) { set_visible(hwnd, true); }
    /// Constant opacity of a layered top-level window
    /// (SetLayeredWindowAttributes LWA_ALPHA; 255 = opaque). Returns false
    /// when the host cannot apply it to this window.
    virtual bool set_window_opacity(HWND hwnd, u8 alpha) {
        (void)hwnd;
        (void)alpha;
        return false;
    }
    /// UpdateLayeredWindow composition: the window shows exactly its
    /// premultiplied surface, alpha 0 is a see-through, click-through hole
    /// (Learn: layered hit testing follows shape and transparency), with no
    /// backdrop plate or system shadow. false restores ordinary opaque
    /// presentation. Returns false when the host cannot apply it.
    virtual bool set_window_per_pixel_alpha(HWND hwnd, bool enabled) {
        (void)hwnd;
        (void)enabled;
        return false;
    }
    virtual void set_frame(HWND hwnd, const Rect& rect) = 0;
    virtual Rect zoom_window(HWND hwnd) = 0;
    virtual void set_minimized(HWND hwnd, bool minimized) = 0;
    virtual bool exchange_app_active(bool active) = 0;
    virtual void set_active_hwnd(HWND hwnd) = 0;

    virtual void get_screen_size(i32& out_w, i32& out_h) const = 0;
    [[nodiscard]] virtual MonitorInfo get_primary_monitor_info() const = 0;
    [[nodiscard]] virtual VirtualDesktopInfo get_virtual_desktop_info() const = 0;

    virtual void set_native_resize_callback(NativeResizeCallback callback) = 0;
    virtual void set_native_show_state_callback(
        NativeShowStateCallback callback) = 0;
    virtual void set_native_hit_test_callback(NativeHitTestCallback callback) {
        (void)callback;
    }
    virtual void set_native_move_callback(NativeMoveCallback callback) {
        (void)callback;
    }

    /// Begin an interactive window move, as DefWindowProc does for
    /// WM_NCLBUTTONDOWN(HTCAPTION) and WM_SYSCOMMAND(SC_MOVE). A guest that
    /// paints its own caption drags by sending one of those, so the host must
    /// hand the gesture to the platform window manager.
    /// @return true when the host started the drag.
    [[nodiscard]] virtual bool begin_native_window_drag(HWND hwnd) {
        (void)hwnd;
        return false;
    }

    /// Begin an interactive window RESIZE, as DefWindowProc does for
    /// WM_NCLBUTTONDOWN(HT sizing border) and WM_SYSCOMMAND(SC_SIZE).
    /// `edge` is the HT code naming the border being pulled (HT::LEFT ..
    /// HT::BOTTOMRIGHT).
    ///
    /// The move counterpart above can hand the whole gesture to AppKit
    /// (performWindowDragWithEvent:). There is no AppKit equivalent for
    /// sizing, so a host implementing this must track the drag itself --
    /// WITHOUT a modal event loop. Dequeuing the shared queue from a nested
    /// loop blocks the main thread and kills clicks process-wide; that is
    /// what forced the previous resize implementation to be reverted.
    /// @return true when the host started the resize.
    [[nodiscard]] virtual bool begin_native_window_resize(HWND hwnd,
                                                          u32 edge) {
        (void)hwnd;
        (void)edge;
        return false;
    }

    /// Platform gesture boundaries. USER32 delivers the Windows lifecycle on
    /// the owner thread; platform callbacks must not execute guest code.
    virtual void set_native_move_size_callback(NativeMoveSizeCallback callback) {
        (void)callback;
    }
};

} // namespace vitreo::windowing
