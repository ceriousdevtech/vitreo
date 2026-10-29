#pragma once
#include "windowing/model/mouse_key_flags.hpp"
/// @file cocoa_bridge.hpp
/// @brief Cocoa bridge interface for windowing (WINDOW-003)

#include "windowing/model/window_types.hpp"
#include "windowing/model/notify_icon_types.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/contracts/menu_types.hpp"
#include "windowing/contracts/user32_host.hpp"
#include "windowing/contracts/syslink_markup.hpp"
#include "windowing/contracts/theme_host.hpp"
#include "windowing/cocoa/cursor_warp_deltas.hpp"
#include "windowing/cocoa/wheel_delta.hpp"
#include <vitreo/common.hpp>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <deque>

namespace vitreo::windowing {

/// The app the host uses to open https links, and the guest path
/// AssocQueryString reports for that association.
struct HostHttpHandler {
    std::string display_name;
    std::string guest_executable;
    std::string command;
    std::string host_executable;
};
[[nodiscard]] HostHttpHandler host_http_handler();
/// Launch that host app when @p guest_executable is the path just reported.
[[nodiscard]] bool launch_host_http_handler(const std::string& guest_executable,
                                            const std::string& parameters);

// Cocoa NSWindow style mask values (from AppKit)
namespace NSWindowStyleMask {
    constexpr u64 Borderless     = 0;
    constexpr u64 Titled         = 1 << 0;
    constexpr u64 Closable       = 1 << 1;
    constexpr u64 Miniaturizable = 1 << 2;
    constexpr u64 Resizable      = 1 << 3;
    constexpr u64 FullScreen     = 1 << 14;
} // namespace NSWindowStyleMask

/// Result of rendering text to an off-screen RGBA pixel buffer via Core Text.
struct RenderedTextBuffer {
    std::vector<u8> pixels; ///< RGBA, row-major, top-down, width*height*4 bytes
    i32 width  = 0;
    i32 height = 0;
    i32 baseline = 0; ///< Baseline measured from the top of the raster buffer.
};

/// Subtype of the application-defined NSEvent wake_main_runloop() posts.
constexpr short kVitreoMainWakeSubtype = 0x5657;  // 'VW'
/// True (and the wake slot is re-armed) when @p ns_event (an NSEvent*) is that
/// wake event; the caller drops it instead of dispatching it.
bool vtr_consume_main_wake_event(void* ns_event) noexcept;
/// An armed wait's signal survives native-event dequeue until the next arm.
bool vtr_main_wake_pending() noexcept;
/// True while AppKit is reading an event, including unarmed native sends.
bool vtr_main_event_wait_active() noexcept;
/// VITREO_GUI_PARK_DIAG counters.
extern std::atomic<u64> g_gui_wake_posted, g_gui_wake_suppressed_flag,
    g_gui_wake_not_parked, g_gui_wake_consumed;

/// VITREO_LOG_PRESENT_VIEW timestamps: wall-clock seconds (mod 100000) with
/// microsecond resolution, comparable across processes.
[[nodiscard]] double present_view_timestamp();

class CocoaBridge final : public User32Host, public ThemeHost {
public:
    CocoaBridge();
    ~CocoaBridge();

    CocoaBridge(const CocoaBridge&) = delete;
    CocoaBridge& operator=(const CocoaBridge&) = delete;

    // === Style Mapping (pure logic, no Cocoa) ===

    /// Map Windows WS_* style flags to NSWindow style mask
    [[nodiscard]] static u64 map_style_to_cocoa(u32 ws_style) noexcept;
    /// Style mask of a window whose guest paints its own frame: borderless or
    /// resizable, and always miniaturizable so SW_MINIMIZE reaches the Dock.
    [[nodiscard]] static u64 guest_chrome_style_mask(u32 ws_style) noexcept;

    /// True when a top-level HWND should advertise macOS Space fullscreen
    /// (green button / Ctrl+Cmd+F) while windowed.
    [[nodiscard]] static bool allows_space_fullscreen(
        u32 ws_style, u32 ex_style, bool is_dialog,
        bool client_drawn_frame) noexcept;

    /// Exclusive guest mode also hosts as a Space, even for borderless popups
    /// that do not advertise Full Screen while windowed.
    [[nodiscard]] static bool uses_space_fullscreen_collection(
        bool exclusive, u32 ws_style, u32 ex_style, bool is_dialog,
        bool client_drawn_frame) noexcept;

    /// Whether a top-level's first show makes it key (activates it). Learn
    /// ShowWindow: SW_SHOW / SW_SHOWNORMAL activate the window; Learn
    /// SetForegroundWindow: a process may take the foreground when it "is the
    /// foreground process" or "was started by the foreground process". A
    /// CreateProcess descendant (session child) therefore activates when it is
    /// using its first-show grant with this show or
    /// when it already is the active application, and a new taskbar window
    /// shown by the active application takes activation from a sibling as a
    /// Win32 activating ShowWindow does. A background child never steals it.
    [[nodiscard]] static bool first_show_takes_key(
        bool first_show, bool first_show_grant, bool show_without_activation,
        bool activating_recipe, bool session_child, bool foreground_process,
        bool owns_taskbar_window, bool preferred_is_self_or_none) noexcept {
        if (!(first_show || first_show_grant) || show_without_activation ||
            !activating_recipe) {
            return false;
        }
        const bool new_foreground_window =
            foreground_process && owns_taskbar_window && first_show;
        if (session_child && !first_show_grant && !new_foreground_window) {
            return false;
        }
        // The session root keeps its sticky preferred key window: a browser
        // window and its helper HWND. A child's new foreground window is the
        // activation its ShowWindow performs.
        return preferred_is_self_or_none ||
               (session_child && new_foreground_window);
    }

    /// Whether the SetForegroundWindow host verb may activate the app and key
    /// the window. Learn SetForegroundWindow: only a process that is the
    /// foreground process, or was started by it (the start grant), may;
    /// otherwise "Windows flashes the taskbar button" and the call returns
    /// zero. A background game re-focusing its viewport after Cmd-Tab must
    /// not pull its Space back over the app the user switched to.
    [[nodiscard]] static bool foreground_request_allowed(
        bool app_active, bool holds_start_grant) noexcept {
        return app_active || holds_start_grant;
    }

    /// The NSWindow level a guest top-level gets. A window in a Space is
    /// alone on that Space: it never needs a raised level, and one would keep
    /// it above other apps. Raising an owned window above a full-screen
    /// sibling (FullScreenAuxiliary) applies only while the app is active;
    /// WS_EX_TOPMOST is a floating window, as before.
    enum class HostLevel { Normal, Floating, AboveFullscreen };
    [[nodiscard]] static HostLevel host_window_level(
        bool topmost, bool in_space, bool above_fullscreen_sibling,
        bool app_active) noexcept {
        if (in_space) return HostLevel::Normal;
        if (above_fullscreen_sibling && app_active) {
            return HostLevel::AboveFullscreen;
        }
        return topmost ? HostLevel::Floating : HostLevel::Normal;
    }

    /// Whether a visibility sync may order a window front or activate it. A
    /// ShowWindow on a window that is already on screen, while another
    /// application is active, changes nothing on the host: Win32 does not
    /// bring a background process's window to the foreground ("An application
    /// cannot force a window to the foreground while the user is working with
    /// another window", Learn SetForegroundWindow), and on macOS raising it
    /// would cover the app the user switched to and stop the Space slide.
    [[nodiscard]] static bool visibility_sync_may_raise(
        bool first_show, bool app_active) noexcept {
        return first_show || app_active;
    }

    /// What reconcile_space_fullscreen does: nothing while a transition runs
    /// (AppKit ignores a toggle then, and the style mask is not final);
    /// enter when the Space is wanted, the window is on screen (an
    /// ordered-out window cannot form one) and the app is active (forming a
    /// Space switches to it: a background app must not pull the user there;
    /// activation reconciles); exit when it is not wanted.
    enum class SpaceAction { None, Enter, Exit };
    [[nodiscard]] static SpaceAction space_reconcile_action(
        bool desired, bool in_space, bool in_transition, bool visible,
        bool app_active) noexcept {
        if (in_transition) return SpaceAction::None;
        if (desired && !in_space) {
            return visible && app_active ? SpaceAction::Enter
                                         : SpaceAction::None;
        }
        if (!desired && in_space) return SpaceAction::Exit;
        return SpaceAction::None;
    }

    /// Whether a press may start an AppKit window drag. Win32 moves a window
    /// only for WM_NCHITTEST HTCAPTION (Learn WM_NCHITTEST / DefWindowProc
    /// WM_NCLBUTTONDOWN) on a guest-drawn frame; a window in a Space or in
    /// exclusive mode never moves (.docs/windowing-native-presentation.md
    /// rule 3), and any other hit code -- HTCLIENT, HTNOWHERE, HTTRANSPARENT
    /// (-1), HTERROR (-2) -- is client input.
    [[nodiscard]] static bool guest_frame_drag_allowed(
        i32 hit_test, bool client_drawn_frame, bool fullscreen,
        bool in_space) noexcept {
        constexpr i32 kHtCaption = 2;
        return hit_test == kHtCaption && client_drawn_frame && !fullscreen &&
               !in_space;
    }

    /// Exclusive / CDS Space hides the menu bar and Dock (no edge reveal).
    /// Document Full Screen keeps AppKit AutoHide.
    [[nodiscard]] static bool exclusive_space_hides_system_ui(
        bool exclusive) noexcept;

    [[nodiscard]] bool exclusive_space_hides_system_ui(HWND hwnd) const;

    /// Whether guest hiding requires a display-wide CoreGraphics hide.
    /// Windowed guests use AppKit cursor rectangles so another macOS window
    /// can immediately restore its own cursor; exclusive fullscreen owns the
    /// display and may use CGDisplayHideCursor -- only while the app is
    /// active: the cursor is a shared resource (Learn ClipCursor / ShowCursor),
    /// and the app the user switched to must have it.
    [[nodiscard]] static bool host_cursor_should_hide(
        bool guest_hidden, bool exclusive, bool has_installed_os_cursor,
        bool app_active = true) noexcept;

    /// True when the NSWindow must be a non-opaque clear plate: WS_EX_LAYERED
    /// per-pixel alpha, or a borderless popup that covers the display (skinned
    /// host). Ordinary popup dialogs stay opaque.
    [[nodiscard]] static bool composition_host_should_be_clear(
        u32 ws_style, u32 ex_style, i32 client_w, i32 client_h,
        i32 screen_w, i32 screen_h) noexcept;

    /// CreateProcess descendants are independent Windows processes. On macOS
    /// they share one originating session as accessory apps, so launch and
    /// first ShowWindow must not activateIgnoringOtherApps.
    [[nodiscard]] static bool macos_session_is_child_process() noexcept;

    /// Translate a macOS hardware key code to a Windows virtual-key code.
    [[nodiscard]] static u32 map_native_keycode_to_windows_vk(u16 key_code) noexcept;

    /// Queue the cursor-negotiation message Windows delivers immediately
    /// before client-area mouse input, followed by the mouse message itself.
    /// Host input enters Vitreo through an asynchronous bridge, so preserving
    /// this pair and its order is the observable contract available here.
    void post_client_mouse_message(MessageQueue& queue, HWND target,
                                   u32 mouse_message, u64 w_param,
                                   u64 l_param);

    /// Map a point inside a fitted presentation viewport to guest pixels.
    /// Returns false when the point is outside the viewport or dimensions are
    /// invalid. When `clamp_outside` is true, points in the letterbox bars are
    /// clamped to the nearest viewport edge before mapping — the right contract
    /// for exclusive full-screen modes, where Windows has no letterbox and the
    /// cursor cannot leave the guest display.
    [[nodiscard]] static bool map_presentation_point(double point_x,
                                                     double point_y,
                                                     double viewport_x,
                                                     double viewport_y,
                                                     double viewport_width,
                                                     double viewport_height,
                                                     u32 surface_width,
                                                     u32 surface_height,
                                                     Point& mapped,
                                                     bool clamp_outside = false) noexcept;

    /// Whether `retarget_presentation_mouse` should run the Metal drawable
    /// transform. Host events now enter as view points in every mode, so
    /// retarget is the single place that maps through the presented surface.
    [[nodiscard]] static bool retarget_applies_presentation_drawable_map(
        bool exclusive_fullscreen) noexcept {
        (void)exclusive_fullscreen;
        return true;
    }

    /// Whether GetCursorPos should map through the presented guest surface
    /// (CAMetalLayer drawable / GDI framebuffer) rather than raw view points.
    /// Exclusive fullscreen always does. Windowed must as well while the
    /// cursor is over the content — D3D9 Reset to a listed pixel mode leaves
    /// the Cocoa view in points and the backbuffer in the chosen mode, and
    /// WM_MOUSEMOVE already remaps through that fit in retarget.
    [[nodiscard]] static bool native_cursor_pos_maps_presentation(
        bool exclusive_fullscreen, bool cursor_in_content) noexcept {
        return exclusive_fullscreen || cursor_in_content;
    }

    /// Inverse of map_presentation_point: map guest pixels back to a point
    /// inside the fitted viewport. Needed to place the cursor where a guest
    /// asks, since the guest reasons in its own surface coordinates.
    [[nodiscard]] static bool unmap_presentation_point(i32 guest_x, i32 guest_y,
                                                       double viewport_x,
                                                       double viewport_y,
                                                       double viewport_width,
                                                       double viewport_height,
                                                       u32 surface_width,
                                                       u32 surface_height,
                                                       double& point_x,
                                                       double& point_y) noexcept;

    /// Map a guest ClipCursor rectangle through an aspect-fit viewport into
    /// view-local bounds. ClipCursor uses the same screen space as
    /// GetCursorPos; exclusive fullscreen must not treat those as 1:1 host
    /// desktop pixels (that pins the cursor to a guest-resolution box).
    [[nodiscard]] static bool guest_clip_to_viewport_bounds(
        i32 left, i32 top, i32 right, i32 bottom, double viewport_x,
        double viewport_y, double viewport_width, double viewport_height,
        u32 surface_width, u32 surface_height, double& out_x_min,
        double& out_y_min, double& out_x_max, double& out_y_max) noexcept;

    /// Whether an installed guest ClipCursor may confine the host cursor.
    /// Learn (ClipCursor): the cursor is a shared resource that a window may
    /// hold only while active, and Windows drops the confinement when the
    /// owning application stops being foreground — so an inactive app, or one
    /// whose windows are all minimised/hidden, must not keep the cursor.
    /// The guest rectangle itself is retained; only enforcement stops.
    [[nodiscard]] static bool guest_clip_should_apply(
        bool clip_installed, bool app_active,
        bool any_visible_unminimized_window) noexcept {
        return clip_installed && app_active && any_visible_unminimized_window;
    }

    /// Whether host mouse motion that no guest window received still belongs
    /// to the guest. Learn (Raw Input overview, RAWINPUTDEVICE): without
    /// RIDEV_INPUTSINK an application receives raw input while it is in the
    /// foreground, and DISCL_FOREGROUND DirectInput devices stay acquired on
    /// the same terms. Foreground is activation, not the window under the
    /// cursor, so motion over another display or app still counts.
    [[nodiscard]] static bool offwindow_motion_reaches_guest(
        bool app_active, bool any_guest_window) noexcept {
        return app_active && any_guest_window;
    }

    // === Coordinate Translation (pure logic) ===

    /// Convert Windows rect (top-left origin, Y-down) to Cocoa frame (bottom-left origin, Y-up)
    /// @param win_rect Windows rect
    /// @param screen_height Height of the screen
    /// @return Cocoa-compatible rect with adjusted Y
    [[nodiscard]] static Rect windows_to_cocoa_rect(const Rect& win_rect,
                                                     i32 screen_height) noexcept;

    /// Convert Cocoa frame back to Windows rect
    [[nodiscard]] static Rect cocoa_to_windows_rect(const Rect& cocoa_rect,
                                                     i32 screen_height) noexcept;

    // === Window Management (stubs for now) ===

    /// Create native window backing for an HWND. Stub: records creation.
    VoidResult create_window(HWND hwnd, const WindowData& data) override;

    /// Destroy native window. Stub: records destruction.
    void destroy_window(HWND hwnd) override;

    /// Close and release every native NSWindow / control view.
    ///
    /// Guest ExitProcess / crash only sets an exit flag; the macOS host often
    /// stays alive in ProcessSession::wait_for_workers(). Without this, wizard
    /// dialogs remain on screen after 0xC0000005. Must run on the AppKit thread.
    void destroy_all_windows();

    /// Stop cross-thread AppKit pumps. Guest GetMessage workers blocked in
    /// pump_events must not wait forever on the main CFRunLoop while teardown
    /// joins those same workers.
    void begin_shutdown() noexcept;

    /// True after begin_shutdown(). AppKit terminate: must not kill a live
    /// guest; only teardown may allow NSTerminateNow.
    [[nodiscard]] bool shutting_down() const noexcept {
        return m_shutdown.load(std::memory_order_acquire);
    }

    /// A live Windows process may legitimately have no native windows (tray
    /// applications and out-of-process UI hosts do this routinely). AppKit
    /// must not treat that interval as permission to terminate the host.
    [[nodiscard]] bool automatic_termination_guard_active() const noexcept {
        return m_automatic_termination_guard_active;
    }

    /// Dock Quit / Cmd+Q: post WM_CLOSE to every top-level HWND so the guest
    /// can save or cancel. If no such window exists, post WM_QUIT.
    void request_guest_quit();

    /// Run the main CFRunLoop briefly so pending vtr_run_on_main_sync blocks
    /// can complete during teardown. No-op off the main thread.
    void service_main_runloop(u32 timeout_ms) noexcept;

    /// Wake the main CFRunLoop from any thread so MsgWait/GetMessage parked in
    /// wait_events observes a just-signaled ntsync object without waiting out
    /// the slice timeout.
    /// Wake the AppKit thread out of nextEventMatchingMask:untilDate:. A bare
    /// CFRunLoopWakeUp does NOT do that (AppKit keeps waiting for an event or
    /// the date), so this posts one coalesced application-defined NSEvent that
    /// the pump recognises and drops. Safe from any thread.
    void wake_main_runloop() noexcept;
    /// Arm/disarm the wake: set true BEFORE the final readiness probe that
    /// precedes a main-thread park, false after the park returns.
    void set_main_thread_parked(bool parked) noexcept;

    /// Show/hide native window
    void set_visible(HWND hwnd, bool visible) override;

    /// Apply SetWindowRgn (MS Learn: nothing outside the region is displayed).
    /// @p has_rgn false clears the region (full window visible again).
    /// Empty region (right<=left or bottom<=top) hides the window entirely.
    /// A skinned wizard host uses CreateRectRgn(0,0,0,0) for that.
    void set_window_rgn(HWND hwnd, bool has_rgn, i32 left, i32 top,
                        i32 right, i32 bottom, bool redraw,
                        const std::vector<std::array<i32, 4>>& parts) override;

    /// Update window title
    void set_title(HWND hwnd, std::wstring_view title) override;
    /// Switch native non-client ownership after WM_NCCALCSIZE. A full-window
    /// guest client rect removes Cocoa's standard title chrome; default
    /// processing restores the style-derived native frame.
    void set_client_drawn_frame(HWND hwnd, bool client_drawn) override;

    /// Move/resize native window
    void set_frame(HWND hwnd, const Rect& rect) override;

    /// Maximize (zoom) a native window to fill the screen.
    /// Returns the resulting content size (client rect) in Win32 pixels.
    Rect zoom_window(HWND hwnd) override;

    /// Mirror the Win32 iconic state onto the native NSWindow. Restoring from
    /// iconic state does not choose normal versus maximized geometry; User32
    /// applies that Windows policy before calling this native delegation.
    void set_minimized(HWND hwnd, bool minimized) override;

    /// Show or hide the native macOS mouse cursor via CGDisplayHideCursor /
    /// ShowCursor, tracking a single hidden state so the calls stay paired.
    /// Used when a guest hides the OS cursor via `ShowCursor(FALSE)`.
    /// Idempotent. Do not install a blank NSCursor — AppKit rejects it as
    /// the system arrow on the next click.
    void set_cursor_visible(bool visible) override;

    /// Install a custom OS cursor from tightly packed top-down RGBA8 pixels.
    /// Hotspot is in pixels from the upper-left of the image (D3D9 /
    /// Win32 SetCursorProperties space). Replaces any previous custom cursor.
    /// Returns false when dimensions/bytes are invalid or AppKit rejects the
    /// image; does not leave a partial custom cursor installed on failure.
    [[nodiscard]] bool set_custom_cursor_rgba(u32 width, u32 height,
                                              u32 hotspot_x, u32 hotspot_y,
                                              const u8* rgba,
                                              usize byte_count) override;

    [[nodiscard]] bool set_animated_cursor_rgba(
        u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
        const std::vector<std::vector<u8>>& frames,
        const std::vector<u32>& delays_ms) override;

    /// Install a stock host cursor (arrow, I-beam, crosshair, pointing hand)
    /// and drop any custom image. @p idc is a Win32 IDC_* ordinal; unknown
    /// values become the arrow.
    void set_system_cursor(u32 idc) override;

    /// Currently installed NSCursor* (custom or stock), or null.
    [[nodiscard]] void* installed_ns_cursor() const { return m_custom_ns_cursor; }

    /// Re-[NSCursor set] the installed shape after a view/window rebuild
    /// (exclusive fullscreen, new Metal view). AppKit otherwise falls back
    /// to the default arrow. Also re-hides the host pointer if the guest or
    /// an exclusive Space still requires it.
    /// Re-assert the installed cursor for a mouse event AppKit has just
    /// dispatched. Cursor rects are disabled while a button is held, so
    /// without this a click-and-hold reverts to the host arrow. Main thread.
    void reassert_cursor_for_mouse_event(void* ns_event);

    void reapply_installed_cursor();

    /// Re-apply CG cursor hide/show from guest + exclusive state. Space
    /// transitions and setPresentationOptions can show the pointer without
    /// clearing m_cursor_hidden.
    void apply_host_cursor_visibility();

    /// True while SetCursor(NULL) / ShowCursor(FALSE) has hidden the host
    /// pointer. cursorUpdate: must install the invisible cursor in that state.
    [[nodiscard]] bool is_cursor_hidden() const { return m_cursor_hidden; }

    /// Show a modal alert for MessageBoxA/W and return the Win32 button ID
    /// (IDOK/IDCANCEL/IDYES/IDNO/IDABORT/IDRETRY/IDIGNORE) the user chose.
    /// @param caption  Dialog title.
    /// @param text     Message body.
    /// @param type     The Win32 uType; only the MB_TYPE_MASK button set is
    ///                 honored today (icons and modality flags are ignored).
    [[nodiscard]] int show_message_box(const std::string& caption,
                                       const std::string& text,
                                       unsigned int type);

    /// Show a modal alert for TaskDialog/TaskDialogIndirect and return the id
    /// of the button pressed.
    ///
    /// A task dialog is a message box with caller-supplied button labels, a
    /// two-level message (a bold instruction over body text) and an optional
    /// verification checkbox, so it cannot be expressed as a uType.
    /// @param title        Window title.
    /// @param instruction  Main instruction, shown as the bold heading.
    /// @param content      Body text.
    /// @param buttons      Buttons in left-to-right order; must not be empty.
    /// @param default_button Index into `buttons` of the default, or 0.
    /// @param verification_text Checkbox label, or empty for no checkbox.
    /// @param[in,out] verification_checked Initial state in, final state out.
    /// @param warning      Present the alert with the caution style.
    [[nodiscard]] int show_task_dialog(
        const std::string& title,
        const std::string& instruction,
        const std::string& content,
        const std::vector<TaskDialogButton>& buttons,
        int default_button,
        const std::string& verification_text,
        bool& verification_checked,
        bool warning);

    /// Put a top-level window into (or out of) macOS Space fullscreen — the
    /// host presentation for DirectDraw/Direct3D exclusive mode and for
    /// document Full Screen. The guest keeps its own back-buffer resolution
    /// (e.g. 640x480); the presenter scales it, aspect-preserved, to fill
    /// the Space. Idempotent; AppKit restores the pre-fullscreen frame on
    /// disable.
    ///
    /// Guest exclusive contracts (mode, cursor clip, aspect-fit) stay. Cmd-Tab
    /// leaves the Space like any other Mac fullscreen app.
    void set_window_fullscreen(HWND hwnd, bool enable) override;
    void request_user_attention(HWND hwnd, bool start) override;
    void show_without_activation(HWND hwnd) override;
    bool set_window_opacity(HWND hwnd, u8 alpha) override;
    bool set_window_per_pixel_alpha(HWND hwnd, bool enabled) override;

    /// Win32 ClipCursor — confine the macOS cursor to a screen rectangle
    /// (Windows Y-down coordinates matching GetCursorPos / monitors). In
    /// exclusive fullscreen those guest coordinates are mapped through the
    /// presentation surface, same as SetCursorPos. Pass enabled=false to
    /// release a guest clip (exclusive-mode confinement may still apply).
    void set_cursor_clip(bool enabled, i32 left, i32 top, i32 right,
                         i32 bottom) override;

    /// Current guest ClipCursor rectangle, if any.
    [[nodiscard]] bool get_cursor_clip(i32& left, i32& top, i32& right,
                                       i32& bottom) const override;

    /// Move/resize a child control/container view.
    /// @return true if the control view exists.
    bool set_control_frame(HWND ctrl_hwnd, const Rect& rect) override;

    /// Move an existing child NSView beneath another local window/control.
    bool set_control_parent(HWND child_hwnd, HWND parent_hwnd) override;

    /// Host a peer-process WS_CHILD HWND as a local NSView under @p parent.
    /// Required when SetParent embeds a GPU-owned DirectComposition child into
    /// a browser-owned parent: AppKit cannot move NSViews across processes, so
    /// the parent process owns the visible presentation surface for that HWND.
    bool ensure_hosted_peer_child(HWND parent_hwnd, HWND child_hwnd,
                                  const Rect& frame) override;

    /// Size a hosted peer NSView that is still a SetParent placeholder to the
    /// current DXGI/DComp surface. IDCompositionVisual::SetContent draws the
    /// swapchain bitmap at (0,0) at the bitmap's own size, and a later
    /// SetContent replaces that bitmap — so a 256×256 probe must not freeze a
    /// later 700×440 composition surface. Present must not resize an HWND that
    /// already has a real SetWindowPos/CreateWindow size (Learn: Present draws
    /// into the client area; it does not change the window). Preserves the
    /// Win32 top-left (not the Cocoa origin): a 1×1 host at client (0,0) must
    /// stay top-left-aligned after growing, or Y-up conversion leaves the
    /// surface clipped below the parent.
    /// Resize every hosted peer child view hosted in `ns_window` to that
    /// window's current content bounds. Called on a NATIVE (user-driven)
    /// resize, which never goes through the guest's SetWindowPos and so never
    /// republishes the child's registered size. See the definition.
    void resize_hosted_peer_children_for_window(void* ns_window);

    bool grow_hosted_peer_child_to_present_size(HWND hwnd, i32 width,
                                                i32 height);

    /// True when @p hwnd has a local child/control NSView (including peer hosts).
    [[nodiscard]] bool has_control_view(HWND hwnd) const override;

    /// Current NSView size for a control/peer host (logical points).
    [[nodiscard]] bool control_view_size(HWND hwnd, i32& out_w,
                                         i32& out_h) const;

    /// Current NSView frame in the parent view's coordinate system.
    [[nodiscard]] bool control_view_frame(HWND hwnd, i32& out_x, i32& out_y,
                                          i32& out_w, i32& out_h) const;

    /// True when @p hwnd's NSView is stacked above @p sibling in the same parent.
    [[nodiscard]] bool control_view_is_above(HWND hwnd, HWND sibling) const;

    /// Control views (NSTextField, NSButton) in @p hwnd's native superview,
    /// @p hwnd's own included; 0 without a view. One Win32 control is one
    /// view: an extra one under the same parent is a stale presentation.
    [[nodiscard]] usize sibling_control_views(HWND hwnd) const;

    /// True when @p hwnd's presentation view is currently hidden. Reads the
    /// VIEW, not any cached state, so a test can catch a cache that stopped
    /// matching reality.
    [[nodiscard]] bool presentation_view_is_hidden(HWND hwnd) const;

    /// True when @p hwnd is a cross-process peer host (NSView only; no local
    /// WindowData/wndproc). Mouse must not be retargeted here — dispatch drops
    /// messages for unknown HWNDs.
    [[nodiscard]] bool is_hosted_peer_child(HWND hwnd) const;
    /// Same HWND selection the input pump uses for a client-area click.
    [[nodiscard]] HWND resolve_client_mouse_target(
        HWND top_level, i32 client_x, i32 client_y,
        HWND* containing_hwnd = nullptr) const;
    /// Translate one native wheel event; shared by both event-pump paths.
    void translate_scroll_wheel(void* ns_event, MessageQueue& queue);

    /// After a child is SetParent'd into a foreign process, detach its local
    /// NSView and orderOut any now-empty top-level composition host window so
    /// it cannot cover the real desktop window with a dark popup.
    void retire_local_host_after_peer_reparent(HWND child_hwnd) override;

    /// Restack a child NSView to match SetWindowPos Z-order (HWND_TOP /
    /// HWND_BOTTOM / sibling). DirectComposition content on that HWND is
    /// clipped by sibling windows unless this child is above them.
    bool set_control_z_order(HWND hwnd, HWND insert_after) override;

    /// Drop every cached presentation fact for @p hwnd, because its view was
    /// created, replaced or destroyed. A new view starts HIDDEN, so a cache
    /// describing the old one would make the next show look redundant.
    void forget_presentation_view_state(HWND hwnd);
    /// Move every WS_EX_TRANSPARENT child view of @p parent_view (an NSView*)
    /// directly below its lowest non-transparent sibling child window,
    /// keeping the transparent children's order among themselves. Views that
    /// are not child windows (the parent's own painted surface) do not move.
    /// Call on the main thread after any change to @p parent_view's child
    /// order.
    void restack_transparent_children(void* parent_view);

    /// Drop cached presentation geometry for @p hwnd. Called from any path that
    /// changes a control's frame outside grow_hosted_peer_child_to_present_size,
    /// so a cached size can never outlive an external resize.
    void note_control_frame_changed(HWND hwnd);

    /// Add a child control view to a parent window's content view.
    /// @param parent_hwnd  Parent HWND (can be a native window OR a container view)
    /// @param child_hwnd   Child control HWND
    /// @param data         WindowData for the child (position/size in data.rect)
    /// @param class_ordinal Standard control class ordinal (0x0080-0x0085), or 0
    /// @param class_name   Custom control class name (e.g., "msctls_progress32"), empty if ordinal
    /// @param text         Control text/label
    void add_child_view(HWND parent_hwnd, HWND child_hwnd,
                        const WindowData& data,
                        u16 class_ordinal, const std::wstring& class_name,
                        const std::wstring& text) override;

    /// Replace an uninitialized superclass view without destroying its HWND,
    /// descendants, capture, or registration/order state.
    bool rebind_child_control(HWND hwnd, const WindowData& data, u16 ordinal,
                              const std::wstring& base_class, const std::wstring& text);

    /// Add a generic container view as a child of a parent (used for WS_CHILD dialogs).
    /// The created view is stored in the control map using container_hwnd.
    void add_container_view(HWND parent_hwnd, HWND container_hwnd,
                            const WindowData& data) override;

    /// Pump events from native layer into message queue
    void set_mouse_capture(HWND hwnd) override;
    void track_mouse_event(HWND hwnd, bool leave, bool hover,
                           u32 hover_ms) override;
    void cancel_mouse_tracking(HWND hwnd, bool leave, bool hover) override;
    void query_mouse_tracking(HWND hwnd, bool& leave, bool& hover,
                              u32& hover_ms) const override;
    void show_tooltip(HWND hwnd, const std::wstring& text,
                      i32 screen_x, i32 screen_y) override;
    void hide_tooltip(HWND hwnd) override;
    void measure_tooltip(const std::wstring& text, i32 max_width,
                         i32& out_width, i32& out_height) const override;

    void pump_events(MessageQueue& queue) override;

    /// Bind sent-message dispatch to the guest thread while it waits for
    /// synchronous AppKit work. Posted input is never retrieved here.
    class NativeWaitScope {
    public:
        explicit NativeWaitScope(std::function<void()> dispatch);
        ~NativeWaitScope();
        NativeWaitScope(const NativeWaitScope&) = delete;
        NativeWaitScope& operator=(const NativeWaitScope&) = delete;
    private:
        std::function<void()> m_previous;
    };

    /// Coalescing clients use this host tick without coupling it to guest waits.
    static void defer_presentation(std::function<void()> callback);

    /// Mark the next `pump_events` / nested AppKit drain as a host-only input
    /// pump (Present / Wait / long host ops). Sync guest hit-tests are skipped.
    void begin_host_input_pump() noexcept { ++m_host_input_pump_depth; }
    void end_host_input_pump() noexcept {
        if (m_host_input_pump_depth > 0) --m_host_input_pump_depth;
    }
    [[nodiscard]] bool in_host_input_pump() const noexcept {
        return m_host_input_pump_depth > 0;
    }

    /// Drain macOS events into the guest queue from the HOST MAIN THREAD's own
    /// run loop, as they arrive, instead of only when a guest thread asks.
    ///
    /// The guest UI thread is not the host main thread, so
    /// it parks in MessageQueue::wait_for_input_for_thread, a condition
    /// variable on the guest queue. An arriving NSEvent does not signal that
    /// variable, because nothing has pumped it in yet, so the thread slept
    /// its whole 16 ms slice while the main thread sat idle in CFRunLoop.
    /// Measured event age at dequeue: p50 3.4 ms, p95 11.0 ms, max 26.8 ms,
    /// against a 222 us queue wait and an 808 us wndproc. Nothing was
    /// polling too slowly; nothing woke the waiter at all.
    ///
    /// A kCFRunLoopBeforeWaiting observer fires exactly when AppKit has
    /// finished delivering events and the loop is about to sleep, which is the
    /// moment the drain should happen. Idempotent; safe to call repeatedly.
    void install_main_runloop_drain();

    /// True once that observer is installed: native input then reaches the
    /// guest queue (and wakes its waiters) without any guest thread pumping,
    /// so a blocked GetMessage need not poll the main thread.
    [[nodiscard]] static bool main_runloop_drain_active() noexcept;

    /// Bind the guest queue before native windows are created so delegate
    /// notifications cannot be lost before the first event pump.
    void set_message_queue(MessageQueue& queue) { m_queue = &queue; }

    /// Wait for (and process) native events for up to timeout_ms.
    ///
    /// This is intended for Win32 APIs which conceptually *block* waiting for
    /// messages (e.g. GetMessage / modal loops). Using AppKit's event wait
    /// instead of a pure usleep() gives Cocoa a chance to run its runloop
    /// machinery (window updates, timers, etc.) while we idle.
    void wait_events(MessageQueue& queue, u32 timeout_ms) override;
    /// Recheck completion after publishing the native wait's wake interest.
    void wait_events(MessageQueue& queue, u32 timeout_ms,
                     std::function<bool()> ready);
    /// wait_events, reporting whether it actually waited. False means it
    /// returned at once WITHOUT waiting or observing a wake: live resize,
    /// teardown, the app not launched yet, or the native dispatch scope held
    /// by an enclosing dispatch. A caller that loops on the wait backs off on
    /// false only; a wake it consumed is progress and must not be delayed.
    [[nodiscard]] bool wait_events_waited(MessageQueue& queue, u32 timeout_ms);

private:
    bool wait_events_impl(MessageQueue& queue, u32 timeout_ms,
                          std::function<bool()> ready);

public:
    /// True after finish_app_launch (finishLaunching). SendMessage host waits
    /// must not call nextEvent before this or they park forever.
    [[nodiscard]] bool is_app_launched() const { return m_app_launched; }

    /// Entry in a VFS directory listing
    struct VfsDirEntry {
        std::string name;       ///< Filename
        bool is_directory;      ///< True if directory
    };

    /// Callback type for listing VFS directories
    /// @param windows_path Windows path (e.g., "C:\Users")
    /// @return List of entries in that directory
    using VfsListCallback = std::function<std::vector<VfsDirEntry>(const std::string& windows_path)>;

    /// Callback type for creating a VFS directory
    /// @param windows_path Full Windows path of directory to create
    /// @return true if creation succeeded
    using VfsCreateDirCallback = std::function<bool(const std::string& windows_path)>;

    /// Show a custom VFS folder picker dialog (sandboxed to Capsule VFS).
    /// @param initial_path Initial Windows path (e.g., "C:\")
    /// @param title Title for the dialog
    /// @param list_callback Callback to list directory contents from VFS
    /// @param create_dir_callback Callback to create a new directory in VFS
    /// @return Selected Windows folder path, or empty string if cancelled
    [[nodiscard]] std::string show_vfs_folder_picker(
        const std::string& initial_path,
        const std::string& title,
        VfsListCallback list_callback,
        VfsCreateDirCallback create_dir_callback);

    /// Set the text of a child control (for SetWindowText/SetDlgItemText)
    /// @param ctrl_hwnd The HWND of the control to update
    /// @param text The new text to set
    /// @return true if the control was found and updated
    bool set_control_text(HWND ctrl_hwnd,
                          const std::wstring& text) override;

    /// Activate a Windows dialog mnemonic from a native Control/Option key event.
    bool activate_native_mnemonic(void* ns_window, u32 character);

    /// Register a BMP file image and return a bitmap handle suitable for STM_SETIMAGE.
    /// Input must be a full BMP file (BITMAPFILEHEADER + DIB).
    /// @return non-zero handle on success.
    [[nodiscard]] u32 register_bitmap_bmp(
        const std::vector<u8>& bmp_file) override;
    void set_control_owns_input(HWND hwnd) override;
    /// OBM_CHECKBOXES drawn with AppKit checkbox / radio cells.
    [[nodiscard]] u32 register_oem_bitmap(u32 id) override;
    /// An AppKit checkbox / radio glyph, straight-alpha RGBA top-down.
    [[nodiscard]] bool render_button_glyph(ButtonGlyph glyph, i32 width, i32 height,
                                           std::vector<u8>& rgba) override;

    /// ThemeHost: AppKit's semantic colours under @p appearance, resolved on
    /// the main thread (cocoa_theme_palette.mm).
    [[nodiscard]] bool theme_palette(ThemeAppearance appearance, ThemePalette& out) override;

    /// Get bitmap dimensions. Returns true if handle is valid.
    struct BitmapDims { i32 width = 0; i32 height = 0; u16 planes = 1; u16 bpp = 32; };
    [[nodiscard]] bool get_bitmap_dims(u32 handle, BitmapDims& out) const;
    /// Get the raw BMP file data for a bitmap handle. Returns nullptr if not found.
    [[nodiscard]] const std::vector<u8>* get_bitmap_bmp_data(u32 handle) const;

    /// Set a bitmap image on a control (typically a STATIC created with SS_BITMAP).
    /// @return true if the control exists and was updated.
    bool set_control_bitmap(HWND ctrl_hwnd, u32 bitmap_handle);

    /// Set an image on a control from raw image file data (ICO, BMP, PNG, etc.).
    /// NSImage is used to decode the data. Useful for SS_ICON controls in dialogs.
    /// @return true if the control was found and image was set.
    /// Show a decoded image (straight-alpha RGBA, top-down) in an image
    /// control (SS_ICON / SS_BITMAP); an empty @p rgba clears it.
    /// @return true if the control is an image view.
    bool set_control_image_rgba(HWND ctrl_hwnd, u32 width, u32 height,
                                const std::vector<u8>& rgba);

    bool set_control_image_data(
        HWND ctrl_hwnd, const u8* data, size_t len) override;

    /// Enable or disable a child control (for EnableWindow)
    /// Maps to NSControl setEnabled: / NSView setAlphaValue: for visual feedback
    /// @param ctrl_hwnd The HWND of the control
    /// @param enable    true to enable, false to disable (gray out)
    /// @return true if the control was found
    bool set_control_enabled(HWND ctrl_hwnd, bool enable) override;

    /// Reactivate a top-level native window after a modal child closes.
    /// @return true if the window was found.
    bool activate_window(HWND hwnd) override;

    /// Raise a top-level window above a game-style full-screen guest window.
    ///
    /// Full-screen guest windows sit at NSMainMenuWindowLevel+1 so they cover
    /// the menu bar and Dock. A dialog created afterwards defaults to
    /// NSNormalWindowLevel and is therefore ordered *behind* it — invisible and
    /// unreachable, while the guest waits in a modal loop. Windows puts an
    /// owned dialog above its owner, so any other top-level window is lifted
    /// above the full-screen one while that state lasts.
    ///
    /// `ns_window` is an NSWindow*. Must be called on the main thread.
    /// No-op when `hwnd` is itself the full-screen window or none is active.
    void elevate_window_above_fullscreen(HWND hwnd, void* ns_window);

    /// Drop windows previously lifted by elevate_window_above_fullscreen back
    /// to the normal level, so they do not float above other applications once
    /// the guest leaves full-screen.
    void restore_elevated_window_levels();

    /// Win32 keeps owned top-level windows above their owner. AppKit's click
    /// makeKeyAndOrderFront on a borderless skinned host buries owned wizard
    /// pages under the splash. Re-order every visible window owned by
    /// `owner_hwnd` above it; prefer key focus on an owned dialog.
    /// Must run on the main thread.
    void restack_owned_windows_above(HWND owner_hwnd);

    /// Set progress bar range (for PBM_SETRANGE/PBM_SETRANGE32)
    /// @param ctrl_hwnd The HWND of the progress bar control
    /// @param min_val Minimum value
    /// @param max_val Maximum value
    /// @return true if the control was found and updated
    bool set_progress_range(HWND ctrl_hwnd, double min_val, double max_val);

    /// Set progress bar position (for PBM_SETPOS)
    /// @param ctrl_hwnd The HWND of the progress bar control
    /// @param position Current position value
    /// @return true if the control was found and updated
    bool set_progress_value(HWND ctrl_hwnd, double position);

    /// Switch a progress bar to/from indeterminate (marquee) mode
    /// (for PBM_SETMARQUEE / PBS_MARQUEE).
    /// @param ctrl_hwnd The HWND of the progress bar control
    /// @param enable true to start the marquee animation, false to stop and revert
    ///               to determinate mode
    /// @return true if the control was found and updated
    bool set_progress_marquee(HWND ctrl_hwnd, bool enable);

    /// Set button/radio/checkbox check state (for BM_SETCHECK)
    /// @param ctrl_hwnd The HWND of the button control
    /// @param checked   true = checked, false = unchecked
    /// @return true if the control was found and updated
    bool set_button_check(HWND ctrl_hwnd, bool checked) override;

    /// Mark a fresh WS_GROUP boundary so the next radio buttons join a new group.
    /// macOS groups NSButtonTypeRadio by (action, superview); we give each Win32
    /// WS_GROUP group a distinct click selector so the groups stay independent.
    void begin_radio_group();

    /// Apply BS_AUTORADIOBUTTON / BS_AUTOCHECKBOX semantics after a native click
    /// (MS Learn: system updates check state before BN_CLICKED reaches the parent).
    void apply_auto_button_click(HWND ctrl_hwnd) override;

    /// Get button/radio/checkbox check state (for BM_GETCHECK / IsDlgButtonChecked)
    /// @param ctrl_hwnd The HWND of the button control
    /// @return 1 (BST_CHECKED) if checked, 0 (BST_UNCHECKED) otherwise
    u32 get_button_check(HWND ctrl_hwnd) const override;

    /// Like get_button_check, but returns false when there is no native NSButton
    /// (caller should fall back to the host-side BM_* map).
    [[nodiscard]] bool try_get_button_check(HWND ctrl_hwnd, u32& out_checked) const;

    /// Get the text of a child control (for GetWindowText/GetDlgItemText)
    /// @param ctrl_hwnd The HWND of the control to read
    /// @return The control's current text, or empty wstring if not found
    std::wstring get_control_text(HWND ctrl_hwnd) const override;

    /// Load RTF (or plain) bytes into a RichEdit-backed NSTextView.
    /// @return true if the control exists and content was applied
    bool set_control_rich_content(HWND ctrl_hwnd, const u8* data, size_t len,
                                  bool is_rtf);

    /// Query Win32-style vertical SCROLLINFO for an NSScrollView-backed control
    /// (multiline EDIT / RichEdit). Returns false if the hwnd is not scrollable.
    using ScrollInfoOut = HostScrollInfo;
    [[nodiscard]] bool get_control_scroll_info(
        HWND ctrl_hwnd, ScrollInfoOut& out) const override;

    /// Set vertical scroll position for an NSScrollView-backed control (EM_SETSCROLLPOS).
    bool set_control_scroll_pos(HWND ctrl_hwnd, i32 x, i32 y);

    /// Read vertical scroll position (EM_GETSCROLLPOS).
    [[nodiscard]] bool get_control_scroll_pos(HWND ctrl_hwnd, i32& x, i32& y) const;

    /// EM_SETSEL for NSTextField / multiline NSTextView edits.
    /// @param start  Selection start (chars); <0 clamps to 0
    /// @param end    Selection end; <0 means "to end of text" (Win32 -1)
    bool set_edit_selection(HWND ctrl_hwnd, i32 start, i32 end);

    /// Notify a RichEdit parent that scroll state may have changed (EN_VSCROLL).
    void notify_richedit_scroll(HWND ctrl_hwnd);

    /// Set font on a control (for WM_SETFONT / dialog template font)
    /// @param ctrl_hwnd  The HWND of the control
    /// @param pt_size    Font size in points
    /// @param weight     Font weight (400=normal, 700=bold)
    /// @param italic     True for italic
    /// @return true if the control was found
    bool set_control_font(
        HWND ctrl_hwnd, float pt_size, u16 weight, bool italic) override;
    bool set_control_font_face(HWND ctrl_hwnd, const std::u16string& face, float pt_size,
                               u16 weight, bool italic) override;

    // === EDIT control layout queries (cocoa_edit.mm) ===
    // Indices are NSString (UTF-16) indices of the displayed string. user32's
    // EDIT class procedure owns the Windows-visible text and state.

    /// Live selection; false when the view has no live text (a single-line
    /// field that is not being edited).
    [[nodiscard]] bool edit_get_selection(HWND hwnd, u32& start, u32& end) const;
    /// Apply a selection without moving focus; false when not live.
    bool edit_set_selection(HWND hwnd, u32 start, u32 end);
    bool edit_scroll_caret(HWND hwnd);
    void edit_set_read_only(HWND hwnd, bool read_only);
    void edit_set_cue_banner(HWND hwnd, const std::wstring& cue);
    /// Start index of each visual line of a multiline view (empty otherwise).
    [[nodiscard]] std::vector<u32> edit_visual_line_starts(HWND hwnd) const;
    [[nodiscard]] i32 edit_first_visible_line(HWND hwnd) const;
    /// Scroll by whole lines (or pages when @p page) and columns; returns
    /// the lines actually scrolled.
    i32 edit_scroll_lines(HWND hwnd, i32 lines, i32 columns, bool page = false);
    [[nodiscard]] bool edit_pos_from_char(HWND hwnd, u32 index, i32& x, i32& y) const;
    [[nodiscard]] bool edit_char_from_pos(HWND hwnd, i32 x, i32 y, u32& index) const;
    [[nodiscard]] bool edit_get_rect(HWND hwnd, Rect& rect) const;
    void edit_set_margins(HWND hwnd, i32 left, i32 right);
    /// Tab stops in points (one value: a stop every that many points).
    void edit_set_tab_stops(HWND hwnd, const std::vector<f64>& points);

    /// BM_SETCHECK: BST_UNCHECKED / BST_CHECKED / BST_INDETERMINATE.
    bool set_button_state(HWND ctrl_hwnd, u32 check);

    // === SysLink (WC_LINK) presentation ===
    // user32 owns the link items and their LIS_* state; the view renders them
    // and answers layout queries against the same layout it draws.

    /// Replace the rendered content: parsed markup, one LIS_* state per item,
    /// and the control's LWS_* style bits.
    void syslink_set_content(HWND hwnd, const syslink::Markup& markup,
                             const std::vector<u32>& states, u32 style);
    /// Index of the link item whose text lies under client point (x, y), or -1.
    [[nodiscard]] i32 syslink_hit_test(HWND hwnd, i32 x, i32 y) const;
    /// Size of the laid-out text wrapped at @p max_width (0 = unwrapped).
    [[nodiscard]] bool syslink_ideal_size(HWND hwnd, i32 max_width, i32& cx,
                                          i32& cy) const;

    // === ComboBox support (NSPopUpButton-backed) ===

    /// Add an item to a combo box; insert if index >= 0, append otherwise.
    /// @return inserted item index, or -1 on failure.
    struct ToolbarButton { i32 image = -1; u32 command = 0; u8 state = 0; u8 style = 0; u64 data = 0; std::wstring text; u64 string_ref = 0; };
    bool rebar_attach_child(HWND rebar, HWND child);
    i32 toolbar_add_bitmap(HWND hwnd, u32 bitmap, u32 count);
    bool toolbar_insert_buttons(HWND hwnd, i32 index, const std::vector<ToolbarButton>& buttons);
    bool toolbar_set_state(HWND hwnd, u32 command, u8 mask, bool on);
    bool update_window_scrollbar(HWND hwnd, u32 bar, const ScrollBarState& state, bool redraw) override;
    bool get_scrollbar_tracking(HWND hwnd, u32 bar, i32& position) const override;
    void toolbar_set_size(HWND hwnd, i32 width, i32 height);
    i32 toolbar_width(HWND hwnd) const;
    std::vector<ToolbarButton> toolbar_buttons(HWND hwnd) const;

    bool status_set_parts(HWND hwnd, const std::vector<i32>& edges);
    bool status_set_text(HWND hwnd, u32 part, const std::wstring& text);
    std::wstring status_get_text(HWND hwnd, u32 part) const;
    std::vector<i32> status_get_parts(HWND hwnd) const;

    i32 combo_add_item(HWND ctrl_hwnd, const std::wstring& text, i32 index = -1);

    /// Clear all combo items.
    /// @return true if control exists and is a combo box.
    bool combo_reset_content(HWND ctrl_hwnd);

    /// Get number of items in a combo box.
    i32 combo_get_count(HWND ctrl_hwnd) const;

    /// Set current selection index.
    /// @return selected index on success, -1 on failure.
    i32 combo_set_cur_sel(HWND ctrl_hwnd, i32 index);

    /// Get current selection index.
    i32 combo_get_cur_sel(HWND ctrl_hwnd) const;

    /// Get item text by index.
    std::wstring combo_get_item_text(HWND ctrl_hwnd, i32 index) const;

    /// CB_SETITEMDATA — store per-item LPARAM (NSMenuItem.tag).
    /// @return true if index is valid.
    bool combo_set_item_data(HWND ctrl_hwnd, i32 index, u64 data);

    /// CB_GETITEMDATA — retrieve per-item LPARAM.
    /// @param ok set false if index is invalid.
    u64 combo_get_item_data(HWND ctrl_hwnd, i32 index, bool* ok = nullptr) const;

    // === Tab control support (NSTabView-backed SysTabControl32) ===

    i32 tab_insert_item(HWND ctrl_hwnd, i32 index, const std::wstring& text,
                        u64 lparam = 0);
    bool tab_delete_item(HWND ctrl_hwnd, i32 index);
    i32 tab_get_item_count(HWND ctrl_hwnd) const override;
    i32 tab_get_cur_sel(HWND ctrl_hwnd) const;
    i32 tab_set_cur_sel(HWND ctrl_hwnd, i32 index);
    bool tab_get_item_rect(HWND ctrl_hwnd, i32 index, Rect& rect) const;
    /// Retrieve tab label text for TCM_GETITEM (Learn: TCIF_TEXT).
    [[nodiscard]] bool tab_get_item_text(
        HWND ctrl_hwnd, i32 index, std::wstring& text_out) const;
    /// Retrieve per-tab LPARAM for TCM_GETITEM (Learn: TCIF_PARAM).
    [[nodiscard]] bool tab_get_item_lparam(
        HWND ctrl_hwnd, i32 index, u64& lparam_out) const;
    /// Update tab attributes for TCM_SETITEM (Learn: TCIF_TEXT / TCIF_PARAM).
    [[nodiscard]] bool tab_set_item(
        HWND ctrl_hwnd, i32 index,
        const std::optional<std::wstring>& text,
        const std::optional<u64>& lparam);
    /// Draw Win32-like tab strip chrome into the native view.
    void draw_tab_strip(HWND ctrl_hwnd);
    /// User clicked a tab cell on the native strip.
    void tab_strip_clicked(HWND ctrl_hwnd, i32 index, bool notify);

    /// Queue the WM_NOTIFY generated by a native tab selection.
    /// The marker payload is marshaled into a guest NMHDR by user32 before
    /// the message is exposed to the application.
    void enqueue_tab_notify(HWND ctrl_hwnd, bool selection_changing);

    /// Decode an internal native-tab notification marker.
    [[nodiscard]] static bool decode_tab_notify_lparam(
        u64 lparam, HWND* ctrl_out, bool* selection_changing_out) noexcept;

    // === ListBox support (NSScrollView + NSTextView-backed) ===

    /// Add an item to a listbox; insert if index >= 0, append otherwise.
    /// @return inserted item index, or -1 on failure.
    i32 listbox_add_item(HWND ctrl_hwnd, const std::wstring& text, i32 index = -1);

    /// Delete an item from a listbox by index.
    /// @return remaining item count, or -1 on failure.
    i32 listbox_delete_item(HWND ctrl_hwnd, i32 index);

    /// Clear all listbox items.
    /// @return true if control exists and was reset.
    bool listbox_reset_content(HWND ctrl_hwnd);

    /// Get number of items in a listbox.
    i32 listbox_get_count(HWND ctrl_hwnd) const;

    /// Set current selected item index.
    /// @return selected index, or -1 on failure.
    i32 listbox_set_cur_sel(HWND ctrl_hwnd, i32 index);

    /// Get current selected item index.
    i32 listbox_get_cur_sel(HWND ctrl_hwnd) const;

    /// Get listbox item text by index.
    std::wstring listbox_get_item_text(HWND ctrl_hwnd, i32 index) const;

    /// Record a selection made in the native table view (so LB_GETCURSEL
    /// reflects the user's click). Does not touch the UI.
    void set_listbox_ui_selection(HWND ctrl_hwnd, i32 index);

    // === ListView (SysListView32) support ===

    /// Append a new line to a listview-backed NSTextView.
    /// @return new item index, or -1 if hwnd is not a listview.
    i32 listview_insert_item(HWND ctrl_hwnd, const std::wstring& text);

    /// Replace a specific line in a listview.
    /// @return true if replacement succeeded.
    bool listview_set_item_text(HWND ctrl_hwnd, i32 item_index, const std::wstring& text);

    /// Get number of items in a listview.
    i32 listview_get_item_count(HWND ctrl_hwnd) const;

    /// Scroll the listview to make the specified item visible.
    void listview_ensure_visible(HWND ctrl_hwnd, i32 item_index);

    // === TreeView (SysTreeView32) support ===

    /// Insert an item under @p h_parent (0 = root) with the given label and
    /// app-defined lParam. Returns the new HTREEITEM (a small non-zero id), or
    /// 0 on failure. Records parent/child + lParam so TVM_GETITEM / selection
    /// notifications can resolve them.
    u64 treeview_insert_item(HWND ctrl_hwnd, u64 h_parent,
                             const std::wstring& text, u64 lparam,
                             bool has_children = false);

    /// Read an item's text and/or lParam. Returns false if not a treeview /
    /// item not found.
    bool treeview_get_item(HWND ctrl_hwnd, u64 h_item,
                           std::wstring* text_out, u64* lparam_out) const;

    /// Delete one item and its descendants, or all items when h_item is
    /// TVI_ROOT. Returns false if the control or requested item is unknown.
    bool treeview_delete_item(HWND ctrl_hwnd, u64 h_item);

    /// Currently-selected HTREEITEM (0 if none).
    u64 treeview_get_selection(HWND ctrl_hwnd) const;

    /// Set the selection (updates the native control; does NOT itself fire a
    /// notification — callers fire TVN_SELCHANGED explicitly).
    void treeview_select_item(HWND ctrl_hwnd, u64 h_item);

    /// Resolve a treeview's parent HWND + control id (for WM_NOTIFY routing).
    bool treeview_notify_target(HWND ctrl_hwnd, HWND* parent_out, u32* id_out) const;

    /// Record a user-click selection and queue a TVN_SELCHANGED notification.
    /// Called from the native NSOutlineView selection delegate.
    void enqueue_tree_notify(HWND ctrl_hwnd, u64 h_item);

    /// Queue TVN_ITEMEXPANDING for a native disclosure-triangle expansion.
    void enqueue_tree_expand(HWND ctrl_hwnd, u64 h_item);

    /// Queue TVN_ITEMEXPANDING with TVE_COLLAPSE for a native collapse.
    void enqueue_tree_collapse(HWND ctrl_hwnd, u64 h_item);

    /// Post a window message into the cached guest message queue (for native
    /// control callbacks, e.g. EN_CHANGE from an edit field). No-op until the
    /// queue is connected.
    void post_window_message(HWND hwnd, u32 msg, u64 wParam, u64 lParam);

    /// Pop the next pending tree selection notification (from a user click).
    /// Returns false if none. Used by the message loop to synthesize WM_NOTIFY.
    using TreeNotify = windowing::TreeNotify;
    bool pop_tree_notify(TreeNotify& out) override;

    // === Test Helpers ===

    /// Snapshot a top-level window or child control's current Cocoa visual
    /// (including NSImageView bitmaps / native controls) as top-down RGBA.
    /// Used to honour BitBlt from GetDC(NULL) screen coordinates. A wizard
    /// parks its pages off the primary monitor and captures them through the
    /// screen DC.
    /// @return true and fills @p out_rgba (width*height*4) on success
    [[nodiscard]] bool capture_window_rgba(HWND hwnd, std::vector<u8>& out_rgba,
                                           i32& out_width, i32& out_height);

    /// Update the content of a window with RGBA pixel data
    void update_window_pixels(HWND hwnd, const u8* rgba, i32 width, i32 height,
                              bool above_native_children = false);

    /// DirectShow video renderer overlay: IVideoWindow is a WS_CHILD of the
    /// owner HWND (Learn put_Owner / put_Visible) and paints above the owner's
    /// client area, including a D3D swapchain. Hosted as a borderless child
    /// NSWindow so CAMetalLayer cannot composite over the video frames.
    void present_video_overlay(HWND hwnd, const u8* rgba, i32 width,
                               i32 height, i32 dest_left = 0, i32 dest_top = 0,
                               i32 dest_width = 0, i32 dest_height = 0);
    void hide_video_overlay(HWND hwnd);

    /// Return whether a child HWND currently has a guest-painted pixel surface.
    [[nodiscard]] bool has_guest_pixel_surface(HWND hwnd) const;

    /// True when a foreign DComp Metal surface is parented to the child control
    /// (Win32-clipped) rather than floated on the top-level contentView.
    [[nodiscard]] bool foreign_presentation_is_control_local(HWND hwnd) const;

    /// Return whether a container's guest surface is behind its native children.
    [[nodiscard]] bool guest_pixel_surface_is_behind_native_children(
        HWND hwnd) const;

    /// Install a parsed Win32 menu bar as native macOS NSMenu.
    /// Menu item clicks post WM_COMMAND to the message queue for the given HWND.
    void install_menu_bar(HWND hwnd, const Win32MenuBar& menu_bar) override;

    /// Update the check state of a menu item by its WM_COMMAND ID.
    /// @param item_id  The Win32 menu item ID
    /// @param checked  true = checked, false = unchecked
    void check_menu_item(u16 item_id, bool checked) override;

    /// Enable or disable (gray out) a menu item by its WM_COMMAND ID.
    void enable_menu_item(u16 item_id, bool enabled) override;

    /// Append an item to a live popup/submenu identified by its HMENU handle.
    /// @param hmenu       The Win32 HMENU handle of the target popup
    /// @param item_id     WM_COMMAND ID for the new item (0 for separators)
    /// @param text        Display text in UTF-8 (empty for separators)
    /// @param is_separator true to add a separator bar
    void append_menu_item(
        u32 hmenu, u16 item_id, const std::string& text,
        bool is_separator) override;

    /// Register a popup menu handle so AppendMenuW can find the NSMenu later.
    /// Called from CreatePopupMenu.
    void register_popup_menu(u32 hmenu) override;

    /// Set the macOS process and application-menu name before AppKit launch.
    /// The containing generated app bundle separately owns the Dock label.
    void set_app_name(const std::string& name);

    /// Set the macOS Dock icon from raw .ICO file data for an unbundled launch.
    /// A declared bundle icon remains authoritative so LaunchServices can apply
    /// its native presentation. The unbundled fallback applies an equivalent
    /// neutral rounded-square presentation before publishing the icon.
    void set_app_icon(const std::vector<u8>& ico_data);

    /// Check if a native window was created for this HWND (for testing)
    bool has_native_window(HWND hwnd) const override;

    /// True when the HWND's NSWindow is ordered on screen (ShowWindow visible).
    bool native_window_is_visible(HWND hwnd) const;

    /// True when the HWND's NSWindow is AppKit-opaque. Client-drawn DComp
    /// hosts must not be, or premultiplied swap-chain alpha composites onto
    /// a solid white plate.
    bool native_window_is_opaque(HWND hwnd) const;
    bool native_window_is_resizable(HWND hwnd) const;

    /// True when the NSWindow is AppKit click-through (WS_EX_TRANSPARENT).
    bool native_window_ignores_mouse_events(HWND hwnd) const;

    /// True when the content view's hitTest at a client point returns a view.
    bool native_content_hits_point(HWND hwnd, i32 x, i32 y) const;

    /// CGImage alpha info of the HWND's GDI pixel layer, or -1 if none.
    int native_pixel_contents_alpha_info(HWND hwnd) const;

    /// Pixel size of the image an image control (NSImageView) shows, or
    /// nullopt when it shows none or is not an image view.
    std::optional<std::pair<i32, i32>> native_control_image_size(HWND hwnd) const;

    /// Get number of native windows
    usize native_window_count() const;

    /// Return a dedicated native presentation NSView for a top-level HWND.
    /// The window retains the view; callers must not release it.
    ///
    /// When `hwnd` has no local NSWindow (common for OOP GPU children that
    /// present to a browser-process HWND), creates and retains a detached
    /// Metal-capable NSView so DXGI CreateSwapChain can still allocate a real
    /// presentable surface. Visible composition into the foreign window is a
    /// separate step; the swap chain side effects themselves are real.
    void* native_presentation_view(HWND hwnd);
    /// Return a root-hosted Metal view for a foreign child surface that must
    /// composite above the target HWND's native descendants.
    void* native_foreign_presentation_view(HWND hwnd);

    /// Show or hide a HWND's Metal presentation view. Used to keep an empty
    /// DirectComposition clear plate from covering peer GDI and browser
    /// siblings before the first textured frame arrives.
    /// @return true when the state actually changed. The peer-present path
///         uses that to raise z-order only on the transition to visible,
///         rather than on every frame.
    [[nodiscard]] bool set_presentation_view_hidden(HWND hwnd, bool hidden);

    /// VITREO_LOG_PRESENT_VIEW: dump a presented view's geometry, visibility
    /// and ancestor chain. A frame that reaches Cocoa but never appears is
    /// invisible for a reason this prints: hidden ancestor, zero frame, or
    /// clipped placement.
    void debug_describe_presentation_view(HWND hwnd, const char* tag);
    /// VITREO_LOG_PRESENT_VIEW: describe every presentation view hosted in
    /// `ns_window` (window visibility transitions).
    void debug_describe_window_presentation_views(void* ns_window,
                                                  const char* tag);

    /// Callback type for native window resize events.
    /// Parameters: hwnd, new client width, new client height.
    /// Register a callback invoked when the user resizes a native window.
    /// The callback should update the HwndManager's WindowData so that
    /// GetClientRect returns the correct new dimensions.
    void set_native_resize_callback(NativeResizeCallback cb) override;
    void set_native_move_callback(NativeMoveCallback cb) override {
        m_move_callback = std::move(cb);
    }
    void set_native_move_size_callback(NativeMoveSizeCallback cb) override {
        m_move_size_callback = std::move(cb);
    }
    /// Main-thread gesture boundaries, deduplicated across AppKit delegates
    /// and explicit guest-caption/border tracking.
    void on_native_move_size(HWND hwnd, bool entering);
    void finish_native_move_size_for_window(void* ns_window);
    void finish_native_window_resize(HWND hwnd, bool notify = true);
    /// windowDidMove: a move the host performed (not one set_frame applied).
    void on_native_move(HWND hwnd, void* ns_window);

    /// Called from the window delegate when the native window is resized.
    bool on_native_resize(HWND hwnd, i32 client_w, i32 client_h);

    /// Callback type for native minimize/maximize button transitions.
    /// Keep the HWND-owned Windows show state synchronized when the user uses
    /// native window chrome rather than calling ShowWindow.
    void set_native_show_state_callback(NativeShowStateCallback cb) override;
    void on_native_show_state(HWND hwnd, bool minimized, bool maximized);

    void set_native_hit_test_callback(NativeHitTestCallback cb) override;

    /// Ask a guest-owned frame for WM_NCHITTEST while the native mouse-down
    /// event is live. Returns true when AppKit consumed an HTCAPTION drag.
    /// A press on a window's native macOS chrome (its title bar or frame,
    /// outside contentLayoutRect) is non-client input. AppKit moves or
    /// resizes the window itself, and on Windows the application gets no
    /// client button messages for such a gesture: WM_NCLBUTTONDOWN goes to
    /// DefWindowProc's move/size loop, which holds the mouse until release.
    /// Returns true for that press and for the drags and release belonging
    /// to it, so the pump posts no client message for them. The guest
    /// already sees the move through WM_ENTERSIZEMOVE / WM_MOVE /
    /// WM_EXITSIZEMOVE. A window that holds mouse capture still gets its
    /// input, as it does on Windows.
    [[nodiscard]] bool native_chrome_mouse_event(void* ns_event, HWND hwnd);
    bool perform_guest_frame_mouse_down(void* ns_event, HWND hwnd,
                                        i32 client_x, i32 client_y);

    /// WindowHost: begin an interactive move for a guest-painted caption.
    [[nodiscard]] bool begin_native_window_drag(HWND hwnd) override;
    [[nodiscard]] bool begin_native_window_resize(HWND hwnd,
                                                  u32 edge) override;


    /// Get the macOS screen size in Cocoa points (windowing / SM_CXSCREEN).
    void get_screen_size(i32& out_w, i32& out_h) const override;

    /// Host framebuffer in device pixels for D3D9 / EnumDisplaySettings.
    void get_screen_pixel_size(i32& out_w, i32& out_h) const override;

    /// Returns true while a window is being live-resized (mouse held on border).
    bool in_live_resize() const { return m_in_live_resize.load(std::memory_order_relaxed); }

    /// Set the live-resize tracking flag (called from delegate callbacks).
    void set_live_resize(bool v) { m_in_live_resize.store(v, std::memory_order_relaxed); }

    /// Render a UTF-8 text string to an off-screen RGBA pixel buffer using
    /// Core Text.  The caller can then blit the result into a GDI framebuffer.
    /// @param utf8_text   Text (UTF-8)
    /// @param pt_size     Font size in macOS points
    /// @param weight      Font weight (400=normal, 700=bold)
    /// @param italic      True for italic style
    /// @param text_color  Win32 COLORREF (0x00BBGGRR)
    /// @return Rendered pixel buffer with dimensions; empty on failure.
    [[nodiscard]] RenderedTextBuffer render_text_to_buffer(
        const std::string& utf8_text,
        float pt_size, u16 weight, bool italic,
        u32 text_color, const std::u16string& face_name = {});

    /// Measure text extents using Core Text without rendering.
    /// @param utf8_text  Text (UTF-8)
    /// @param pt_size    Font size in macOS points
    /// @param weight     Font weight (400=normal, 700=bold)
    /// @param italic     True for italic style
    /// @param[out] out_width  Measured width in pixels
    /// @param[out] out_height Measured height in pixels
    void measure_text(const std::string& utf8_text,
                      float pt_size, u16 weight, bool italic,
                      i32& out_width, i32& out_height,
                      std::vector<i32>* advances = nullptr,
                      const std::u16string& face_name = {});

    // === Native Delegation: Clipboard (NSPasteboard) ===

    /// Copy UTF-8 text to the macOS general pasteboard.
    /// @return true on success.
    bool clipboard_set_text(const std::string& utf8_text) override;

    /// Get UTF-8 text from the macOS general pasteboard.
    /// @return text or empty string if no text on clipboard.
    [[nodiscard]] std::string clipboard_get_text() override;
    [[nodiscard]] i64 clipboard_change_count() override;

    /// Clear the macOS general pasteboard.
    void clipboard_clear() override;

    // === Native Delegation: Monitor Info (NSScreen) ===

    using MonitorInfo = windowing::MonitorInfo;
    using VirtualDesktopInfo = windowing::VirtualDesktopInfo;

    /// Get primary monitor dimensions from NSScreen.mainScreen.
    [[nodiscard]] MonitorInfo get_primary_monitor_info() const override;

    /// All attached NSScreens in Win32 screen coordinates (Y-down).
    [[nodiscard]] std::vector<MonitorInfo>
    get_all_monitor_info() const override;

private:
    /// Cache for get_all_monitor_info(). Enumerating NSScreen means an
    /// autoreleasepool plus a deviceDescription dictionary per screen, and the
    /// guest asks constantly. The UI calls MonitorFromWindow,
    /// MonitorFromPoint, and GetMonitorInfo from inside its mouse handler, which
    /// measured at ~10% of the entire wndproc dispatch cost on every mouse
    /// message. The answer only changes when the display configuration does,
    /// so a CGDisplayRegisterReconfigurationCallback bumps the generation and
    /// everything else reads the cache. Mutable: the accessor is const, and
    /// guest threads reach it through the user32 thunks, hence the mutex.
    mutable std::mutex m_monitor_cache_mu;
    mutable std::vector<MonitorInfo> m_monitor_cache;
    mutable u32 m_monitor_cache_generation = 0;

public:

    /// Bounding virtual desktop matching SM_CX/Y/XVIRTUALSCREEN.
    [[nodiscard]] VirtualDesktopInfo get_virtual_desktop_info() const override;

    // === Native Delegation: Input State (NSEvent) ===

    /// Get the current modifier/key state.
    /// Returns a bitmask compatible with GetKeyState return values.
    /// @param vk_code Windows virtual key code
    /// @return High bit set if key is down, low bit set if toggled (caps/num lock)
    [[nodiscard]] i16 get_native_key_state(u32 vk_code) const override;

    /// Return asynchronous key state and consume any pending mouse-button press.
    [[nodiscard]] i16 get_native_async_key_state(u32 vk_code) override;

    /// Record a native key transition for USER32 key-state polling APIs.
    void note_native_key_transition(u32 vk_code, bool down) noexcept;

    using NativeKeyTransitionCallback = std::function<void(u32, bool)>;
    using NativeMouseButtonCallback = std::function<void(u32, bool, HWND)>;
    /// (screen_x, screen_y, hwnd, device_dx, device_dy): the cursor position
    /// in guest screen space plus the DEVICE motion of this event (NSEvent
    /// deltaX/deltaY). Raw input and DirectInput report device motion,
    /// which SetCursorPos warps, ClipCursor and screen edges do not touch.
    using NativeMouseMotionCallback =
        std::function<void(i32, i32, HWND, double, double)>;

    /// Publish normalized host input after Cocoa coordinates and key codes have
    /// been translated to their Windows-visible forms.
    void set_native_input_callbacks(NativeKeyTransitionCallback key,
                                    NativeMouseButtonCallback button,
                                    NativeMouseMotionCallback motion) {
        m_native_key_transition_callback = std::move(key);
        m_native_mouse_button_callback = std::move(button);
        m_native_mouse_motion_callback = std::move(motion);
    }

    /// (delta, horizontal, hwnd): wheel movement in WHEEL_DELTA units, the
    /// same signed delta and target WM_MOUSEWHEEL / WM_MOUSEHWHEEL carry, for
    /// raw input (RI_MOUSE_WHEEL) and the DirectInput Z axis.
    using NativeMouseWheelCallback = std::function<void(i32, bool, HWND)>;
    void set_native_wheel_callback(NativeMouseWheelCallback wheel) {
        m_native_mouse_wheel_callback = std::move(wheel);
    }

    /// Tracked physical state of a key, without the modifier-flag fallback
    /// get_native_key_state applies. Used to fill the previous-key-state bit
    /// of a key message's lParam and to diff modifier transitions.
    [[nodiscard]] bool native_key_is_down(u32 vk_code) const noexcept;

    /// Translate a macOS flagsChanged modifier mask into Windows key messages.
    /// macOS reports modifier press/release as NSEventTypeFlagsChanged rather
    /// than keyDown/keyUp, so each changed modifier is diffed against the
    /// tracked state and posted as WM_KEYDOWN/WM_KEYUP for its virtual key.
    /// @param modifier_flags NSEventModifierFlags from the event
    void post_modifier_transitions(HWND target, u64 modifier_flags,
                                   MessageQueue& queue);

    /// Record a native mouse-button transition for USER32 and DirectInput.
    void note_native_mouse_button_transition(u32 vk_code, bool down,
                                             HWND hwnd) noexcept;

    /// Publish a mouse position already translated into guest screen space.
    void note_native_mouse_motion(i32 screen_x, i32 screen_y, double device_dx,
                                  double device_dy,
                                  HWND hwnd) noexcept;

    /// The same for the host mouse event @p ns_event (NSEvent*): its device
    /// motion is its delta less the cursor warps Vitreo made before it
    /// (CursorWarpDeltas). Main-thread only.
    void note_native_mouse_motion(i32 screen_x, i32 screen_y, void* ns_event,
                                  HWND hwnd) noexcept;

    /// The device motion of host mouse event @p ns_event (NSEvent*): its
    /// delta less the warps recorded before it. Main-thread only.
    void native_mouse_device_delta(void* ns_event, double& device_dx,
                                   double& device_dy) noexcept;

    /// Publish the device motion of a host mouse event that no guest window
    /// receives: the cursor is over another display, the desktop, or another
    /// app's window. Raw input and DirectInput follow the foreground
    /// application, not the cursor, so while this app is active the motion
    /// still belongs to the guest. Main-thread only.
    void note_offwindow_mouse_motion(void* ns_event) noexcept;

    /// Record that a cursor warp at host time @p time (the NSEvent timestamp
    /// clock) moved the cursor by (@p dx, @p dy) points, y down. Main-thread only.
    void note_host_cursor_warp(double time, double dx, double dy) noexcept;

    /// Return the GetTickCount-compatible tick at which the combined macOS
    /// user session last received keyboard, mouse, or tablet input.
    [[nodiscard]] u32 last_input_tick_count() const noexcept override;

    /// Get the current mouse cursor position in screen coordinates.
    /// Coordinates are translated to Windows convention (Y-down from top-left).
    void get_native_cursor_pos(i32& out_x, i32& out_y) const override;

    /// Move the native cursor to a guest screen position (Windows convention,
    /// Y-down). Counterpart to get_native_cursor_pos, so the value a guest
    /// writes and the value it reads back agree.
    ///
    /// This is load-bearing for mouse capture. Games implement a captured
    /// pointer by warping the cursor to the window centre every frame and
    /// treating the next reported position as a relative delta. Without a
    /// real warp those deltas are nonsense and the pointer drifts.
    /// @return true if the cursor was moved.
    bool set_native_cursor_pos(i32 x, i32 y) override;

    // === Native Delegation: File Dialogs (NSOpenPanel / NSSavePanel) ===

    struct FileDialogResult {
        bool ok = false;           ///< true if user selected a file
        std::string selected_path; ///< Windows path of selected file
    };

    /// Show a native Open File dialog.
    /// @param initial_dir Windows path for initial directory (empty = default)
    /// @param filter      File type filter description (e.g., "*.txt")
    /// @param title       Dialog title
    /// @return Selected file path or empty if cancelled.
    [[nodiscard]] FileDialogResult show_open_file_dialog(
        const std::string& initial_dir,
        const std::string& filter,
        const std::string& title);

    /// Show a native Save File dialog.
    [[nodiscard]] FileDialogResult show_save_file_dialog(
        const std::string& initial_dir,
        const std::string& default_name,
        const std::string& filter,
        const std::string& title);

    /// The Common Item Dialog (IFileOpenDialog / IFileSaveDialog) as an
    /// NSOpenPanel / NSSavePanel. Paths are host paths.
    struct ItemDialogRequest {
        bool save = false;
        bool pick_folders = false;      ///< FOS_PICKFOLDERS
        bool multi_select = false;      ///< FOS_ALLOWMULTISELECT
        bool show_hidden = false;       ///< FOS_FORCESHOWHIDDEN
        std::string title;
        std::string ok_label;           ///< SetOkButtonLabel -> prompt
        std::string file_name_label;    ///< SetFileNameLabel -> nameFieldLabel
        std::string file_name;          ///< SetFileName
        std::string initial_dir;        ///< SetFolder / SetDefaultFolder
        /// The selected filter's extensions ("txt"), empty or "*" for all.
        std::vector<std::string> extensions;
        /// Host directories backing the guest's drives. The capsule is
        /// isolated: anything outside them is disabled in the panel and a
        /// choice there is refused with an alert. Empty = no restriction.
        std::vector<std::string> allowed_roots;
    };
    struct ItemDialogResult {
        bool ok = false;
        std::vector<std::string> paths;
    };
    [[nodiscard]] ItemDialogResult show_item_dialog(const ItemDialogRequest& request);

    // === Native Delegation: Shell Execute (NSWorkspace) ===

    /// Open a URL or file path via NSWorkspace (macOS equivalent of ShellExecute).
    /// @param target URL string or file path
    /// @return true if NSWorkspace accepted the open request
    bool shell_open(const std::string& target);

private:
    struct NativeWindowState {
        HWND hwnd;
        bool visible = false;
        /// The pending show came from SW_SHOWNOACTIVATE / SW_SHOWNA /
        /// SWP_NOACTIVATE: order the window in without making it key.
        bool show_without_activation = false;
        bool is_dialog = false;
        u32 guest_style = 0;
        u32 guest_ex_style = 0;
        bool client_drawn_frame = false;
        HWND owner = NULL_HWND;       ///< Win32 owner (not parent) for z-order
        std::wstring title;
        Rect frame;
        void* ns_window = nullptr;    ///< NSWindow* (opaque to avoid ObjC in header)
        void* ns_delegate = nullptr;  ///< VtrWindowDelegate* (opaque)
        std::vector<void*> button_handlers;  ///< VtrButtonHandler* objects for child buttons
        // Full-screen state: saved frame/style-mask to restore on exit.
        /// The Space the window should be in (exclusive owner or the
        /// borderless rule). AppKit's actual state is the window's
        /// NSWindowStyleMaskFullScreen; reconcile_space_fullscreen drives
        /// one toward the other.
        bool fullscreen = false;
        bool borderless_fullscreen = false;
        /// An AppKit enter/exit transition is running (windowWill* to
        /// windowDid* / windowDidFailTo*): the style mask is not final, and
        /// no second toggle may be issued until it ends.
        bool space_transition = false;
        /// The running transition was requested by this bridge (not by the
        /// user's green button / Ctrl-Cmd-F), so its end re-checks the
        /// desired state, which may have changed meanwhile.
        bool space_toggle_ours = false;
        /// SetWindowRgn installed an empty region — window must not be shown
        /// even if ShowWindow asks for visible (a skinned wizard host).
        bool empty_window_rgn = false;
        /// A non-rectangular SetWindowRgn made the window see-through outside
        /// the region; these restore what it was when the region is removed.
        /// What the window shows before it paints (WindowData::erase_color).
        std::optional<u32> erase_color;
        bool region_shaped = false;
        bool pre_region_opaque = true;
        bool pre_region_shadow = true;
        void* pre_region_background = nullptr;  // retained NSColor
        /// UpdateLayeredWindow owns the pixels: present the surface's alpha
        /// on a clear, shadowless, non-opaque window.
        bool layered_alpha = false;
        bool pre_layered_opaque = true;
        bool pre_layered_shadow = true;
        void* pre_layered_background = nullptr;  // retained NSColor
        /// Class was registered with CS_DBLCLKS, so the guest expects
        /// WM_LBUTTONDBLCLK/WM_RBUTTONDBLCLK on the second click of a pair.
        bool wants_dblclk = false;
        Rect saved_frame{};           ///< Cocoa frame (pixels) before fullscreen
        /// Cocoa screen frame selected when exclusive fullscreen began.  Guest
        /// SetWindowPos/MoveWindow still update `frame`, but must not move the
        /// native borderless presentation away from this display rectangle.
        Rect fullscreen_screen_frame{};
        u64  saved_style_mask = 0;    ///< NSWindow styleMask before fullscreen
        bool level_elevated = false;  ///< lifted above a full-screen window
    };

    /// True when the guest Win32 outer rect intersects any attached monitor.
    /// A wizard parks pages past SM_CXSCREEN for screen-DC BitBlt capture.
    /// Those must stay orderOut so AppKit does not clamp them onto the
    /// primary display as a second visible white-box wizard.
    [[nodiscard]] bool guest_rect_intersects_any_monitor(const Rect& r) const;

    /// True when a display-covering borderless popup owns other top-level
    /// HWNDs. Those owned windows are the UI; presenting the host is the
    /// black/chromeless plate seen between wizard pages.
    [[nodiscard]] bool composition_host_defers_to_owned(
        const NativeWindowState& state) const;
    void sync_borderless_fullscreen(HWND hwnd);

    /// NSWindow that should host owned dialogs of this owner-family, or
    /// NULL_HWND. Borderless skin hosts are skipped so pages are not embedded
    /// into a window that stays orderOut.
    [[nodiscard]] HWND family_presentation_hwnd(HWND owner) const;

    /// True when an owned dialog should be hosted as a container in the
    /// family's presentation window (extra wizard pages). Only
    /// chromeless dialogs are pages: a dialog with its own WS_CAPTION, and a
    /// stock DialogBox owned by a captioned top-level, keep their NSWindow.
    [[nodiscard]] bool should_embed_owned_dialog(HWND dialog, u32 dialog_style,
                                                 HWND owner) const;

    /// True when this NSView backs a control the guest hid with
    /// ShowWindow(SW_HIDE), so window-level visibility syncs leave it hidden.
    [[nodiscard]] bool control_is_hidden_by_guest(void* ns_view) const;

    /// Move an owned dialog that already has its own NSWindow into the
    /// family's presentation window as a container view.
    void adopt_extra_family_dialog(HWND hwnd);
    void adopt_extra_dialogs_in_family(HWND root);

    /// Order the native window front or out based on Win32 visibility, empty
    /// region, and whether the guest rect intersects a real monitor.
    void sync_native_window_visibility(NativeWindowState& state);

    /// The first eligible child window consumes its launch activation grant.
    /// This is independent of the session's single Dock identity.
    bool m_used_child_first_show_grant{false};
    std::atomic<void*> m_dock_session{nullptr}; ///< Session observer, owned on AppKit main thread.

    /// True while exclusive-fullscreen windows are ordered out / demoted
    /// because NSApp is inactive (Cmd-Tab). Guest still believes it is
    /// exclusive; only the host presentation is suspended.
    bool m_fullscreen_presentation_suspended = false;
    void* m_app_activation_observer = nullptr; ///< VtrAppActivationObserver*

    std::atomic<bool> m_app_active{false}; ///< app-activation state for WM_ACTIVATEAPP dedup
    /// Launched with the start grant (VITREO_FOREGROUND_GRANT=1) and not yet
    /// activated: the right is used by the first activation.
    std::atomic<bool> m_holds_start_grant{false};
    /// A refused background SetForegroundWindow has asked for Dock
    /// attention since the app was last active.
    std::atomic<bool> m_foreground_refusal_flashed{false};
    /// Windows AppKit reports entirely occluded (windowDidChangeOcclusionState).
    mutable std::mutex m_occlusion_mutex;
    std::vector<HWND> m_occluded_windows;
    /// Main thread only: process trees this active application yields to,
    /// the applications it already yielded to, and the polling timer.
    std::vector<i32> m_foreground_grant_roots;
    std::vector<i32> m_foreground_yielded;
    void* m_foreground_grant_timer = nullptr;
    void tick_foreground_grants();
    void end_foreground_grants();
    std::atomic<HWND> m_active_hwnd{NULL_HWND}; ///< guest HWND of the current key window
    /// Sticky foreground top-level for visibility sync. Survives transient
    /// AppKit resign gaps so a sibling ShowWindow/present cannot steal key
    /// merely because `m_active_hwnd` was cleared mid-transition.
    std::atomic<HWND> m_preferred_key_hwnd{NULL_HWND};
    std::atomic<HWND> m_keyboard_focus_hwnd{NULL_HWND};
    std::atomic<HWND> m_pending_activation_hwnd{NULL_HWND};
public:
    /// Atomically set the app-active flag and return the PREVIOUS value. Used to
    /// post WM_ACTIVATEAPP only on a real activation transition (a window
    /// becoming/resigning key fires per-event. Without dedup a dialog's
    /// reaction re-keys the window, and the WM_ACTIVATEAPP feedback storm
    /// freezes the UI).
    bool exchange_app_active(bool active) override {
        return m_app_active.exchange(active);
    }

    void set_active_hwnd(HWND hwnd) override {
        m_active_hwnd.store(hwnd, std::memory_order_release);
    }
    void clear_active_hwnd(HWND hwnd) {
        m_active_hwnd.compare_exchange_strong(hwnd, NULL_HWND, std::memory_order_acq_rel);
    }
    [[nodiscard]] HWND active_hwnd() const {
        return m_active_hwnd.load(std::memory_order_acquire);
    }
    /// Mirror USER32's process input-queue focus into the native event pump.
    /// The AppKit key window identifies the active top-level HWND, but Win32
    /// keyboard messages belong to the focused child when one is set.
    void set_keyboard_focus_hwnd(HWND hwnd) override {
        m_keyboard_focus_hwnd.store(hwnd, std::memory_order_release);
    }
    [[nodiscard]] HWND keyboard_focus_hwnd() const {
        return m_keyboard_focus_hwnd.load(std::memory_order_acquire);
    }
    [[nodiscard]] HWND keyboard_target(HWND native_top_level) const {
        const HWND focused = keyboard_focus_hwnd();
        return focused != NULL_HWND ? focused : native_top_level;
    }

    /// Route native activation through USER32's sent-message boundary:
    /// (hwnd, true, by_mouse) when a guest top-level becomes key, and
    /// (hwnd, false, false) when the application loses activation. USER32
    /// sends WM_ACTIVATEAPP, WM_NCACTIVATE, WM_ACTIVATE and the focus
    /// messages before the native transition completes.
    /// (hwnd, active, mouse_message): @p mouse_message is the button-down
    /// message of the click that made @p hwnd key (WM_LBUTTONDOWN,
    /// WM_RBUTTONDOWN, WM_MBUTTONDOWN), or 0. Returns the WM_MOUSEACTIVATE
    /// answer (MA_ACTIVATE.. MA_NOACTIVATEANDEAT), or 0 when none was asked.
    using NativeApplicationActivationCallback =
        std::function<u32(HWND, bool, u32)>;
    void set_native_application_activation_callback(
        NativeApplicationActivationCallback callback) {
        m_native_application_activation_callback = std::move(callback);
    }
    using NativeKeyboardFocusCallback = std::function<void(HWND, bool)>;
    void set_native_keyboard_focus_callback(
        NativeKeyboardFocusCallback callback) {
        m_native_keyboard_focus_callback = std::move(callback);
    }
    void on_native_window_became_key(HWND hwnd);
    void on_native_window_resigned_key(HWND hwnd);
    void on_native_did_enter_space_fullscreen(HWND hwnd);
    void on_native_did_exit_space_fullscreen(HWND hwnd);
    /// windowDidChangeOcclusionState: whether any part of the window is
    /// visible (NSWindowOcclusionStateVisible). A window in a Space the user
    /// left, or miniaturized, is not.
    void on_native_occlusion_changed(HWND hwnd, bool visible);
    /// True while AppKit reports @p hwnd entirely occluded. Any thread.
    [[nodiscard]] bool window_occluded(HWND hwnd) const;
    /// windowWillEnterFullScreen / windowWillExitFullScreen.
    void on_native_space_transition_began(HWND hwnd);
    /// windowDidFailToEnterFullScreen / windowDidFailToExitFullScreen.
    void on_native_space_transition_failed(HWND hwnd);
    /// Toggle the window's Space toward its desired state when AppKit can
    /// take the request now (main thread).
    void reconcile_space_fullscreen(HWND hwnd);
    void apply_exclusive_space_system_ui(bool hide);
    void sync_exclusive_space_system_ui();

    /// NSApplication became/resigned active — suspend or restore exclusive
    /// fullscreen presentation so Cmd-Tab does not leave a menu-bar-level
    /// window covering every other app.
    void on_app_did_resign_active();
    void on_app_did_become_active();
    /// applicationShouldHandleReopen: a Dock click. With no window showing
    /// and none minimized, the notification-area icon the reopen target
    /// names gets NotifyIconInput::Activate.
    void on_dock_reopen(bool has_visible_windows);
    void start_dock_session();
    void stop_dock_session();
    void publish_dock_session(bool became_key = false);
    [[nodiscard]] bool route_dock_session();
    [[nodiscard]] HWND dock_session_target() const;
    void activate_dock_session_target();
    [[nodiscard]] bool dock_session_has_tray_target() const;
    void broadcast_dock_session_quit();
    /// The AppUserModelID group the Dock tile of @p hwnd belongs to
    /// (NULL_HWND: the process's own identity). See dock_session_election.hpp.
    [[nodiscard]] std::string dock_session_group(HWND hwnd) const;
    /// Installed by the runtime: shell::effective_app_user_model_id for this
    /// process. Without it every member of a family is one group.
    void set_app_user_model_id_resolver(std::function<std::string(HWND)> resolver);

    // --- Notification area (Shell_NotifyIcon) as menu-bar status items ---
    // (cocoa_notify_icons.mm). Present/withdraw/balloon calls return at once
    // and run on the main thread; input is reported on the main thread.
    using NotifyIconInputHandler =
        std::function<void(u64 key, NotifyIconInput input, i32 x, i32 y)>;
    void set_notify_icon_input_handler(NotifyIconInputHandler handler);
    void set_notify_icon_reopen_target(std::function<std::optional<u64>()> target);
    void present_notify_icon(u64 key, const NotifyIconPresentation& view);
    void withdraw_notify_icon(u64 key);
    /// The status item's rectangle in guest screen coordinates.
    [[nodiscard]] std::optional<Rect> notify_icon_frame(u64 key);
    void show_notify_icon_balloon(u64 key, const NotifyIconBalloon& balloon);
    void hide_notify_icon_balloon(u64 key);
    /// Called by the status item's target and balloon views (main thread).
    void report_notify_icon_input(u64 key, NotifyIconInput input);

    // === Foreground grant (Learn SetForegroundWindow / AllowSetForegroundWindow)
    //
    // A process "started by the foreground process" may take the foreground;
    // one that can set the foreground can pass the ability on. On macOS 14+
    // activation is cooperative: the active application must yield before
    // another one's activation request is honoured (NSApplication
    // -yieldActivationToApplication:, "the other app should request to be
    // activated ... by calling activate"). Every guest process is its own
    // macOS application, so the active one yields to the process tree its
    // guest launched; the start grant travels down CreateProcess in the
    // environment so each descendant knows it holds it.

    /// Environment entry a launched child receives (VITREO_FOREGROUND_GRANT).
    static constexpr const char* kForegroundGrantEnv = "VITREO_FOREGROUND_GRANT";
    [[nodiscard]] static std::string foreground_grant_env_entry(bool granted) {
        return std::string(kForegroundGrantEnv) + (granted ? "=1" : "=0");
    }
    /// Whether a child launched now is "started by the foreground process"
    /// (or by one that may set it): the guest application is active, or this
    /// process still holds the start grant it was launched with.
    [[nodiscard]] bool process_may_set_foreground() const;
    /// After a launch with the grant: while this application stays active,
    /// yield activation to @p child and every application descending from
    /// it, until one of them is activated or @p child exits.
    void grant_foreground_to_process_tree(i32 child_pid);
    /// Whether @p pid is @p ancestor or descends from it, walking
    /// @p parent_of (which returns 0 when unknown). Bounded and cycle-safe.
    [[nodiscard]] static bool pid_descends_from(
        i32 pid, i32 ancestor, const std::function<i32(i32)>& parent_of);

    /// Suppress native resize feedback while Vitreo applies guest-requested
    /// geometry. USER32 already owns that state; reflecting Cocoa's frame back
    /// would count native title chrome twice and relayout guest controls.
    void set_suppress_resize_notify(bool b) { m_suppress_resize.store(b, std::memory_order_relaxed); }
    bool resize_notify_suppressed() const { return m_suppress_resize.load(std::memory_order_relaxed); }
private:
    std::atomic<bool> m_suppress_resize{false};
    NativeApplicationActivationCallback
        m_native_application_activation_callback;
public:
    /// A click made @p declined key but its WM_MOUSEACTIVATE answer was
    /// MA_NOACTIVATE(ANDEAT): give key status back to @p stays_active without
    /// reordering any window. Callable from any thread.
    void restore_key_after_declined_activation(HWND declined, HWND stays_active);
private:
    /// A WM_MOUSEACTIVATE answer of MA_ACTIVATEANDEAT / MA_NOACTIVATEANDEAT
    /// discards the button-down of that click for this window (Learn).
    std::atomic<HWND> m_mouse_activate_eat_hwnd{NULL_HWND};
    NativeKeyboardFocusCallback m_native_keyboard_focus_callback;

    std::unordered_map<HWND, NativeWindowState> m_windows;

    /// One armed TrackMouseEvent request. Windows cancels a request when it
    /// fires, so each flag is genuinely one-shot.
    struct MouseTracking {
        bool leave{false};
        bool hover{false};
        u32 hover_ms{0};
        i32 origin_x{0};
        i32 origin_y{0};
        double hover_deadline{0.0};   ///< CACurrentMediaTime seconds
    };
    std::unordered_map<HWND, MouseTracking> m_mouse_tracking;
    /// HWND the pointer was last seen over, which is what leave is measured
    /// against.
    HWND m_mouse_last_hwnd{NULL_HWND};
    /// Window holding mouse capture, or NULL_HWND.
    std::atomic<HWND> m_mouse_capture_hwnd{NULL_HWND};
    /// The window whose native chrome is being pressed (native_chrome_mouse_event),
    /// and which buttons (bit 0 left, bit 1 right) are down on it.
    HWND m_native_chrome_press_hwnd = NULL_HWND;
    u32 m_native_chrome_press_buttons = 0;
    /// Last client-space pointer position seen by the pump, which is the
    /// origin a newly armed hover dwell measures from.
    i32 m_last_mouse_x{0};
    i32 m_last_mouse_y{0};
    /// Deliver any WM_MOUSELEAVE / WM_MOUSEHOVER the current pointer position
    /// has made due. Called from the event pump.
    void flush_mouse_tracking(MessageQueue& queue, HWND over_hwnd,
                              i32 client_x, i32 client_y);

    /// Live tooltip bubbles, keyed by the tooltip control's HWND.
    std::unordered_map<HWND, void*> m_tooltip_panels;
    /// Detached Metal presentation views for HWNDs without a local NSWindow
    /// (OOP GPU CreateSwapChain). Values are (__bridge_retained) NSView*.
    mutable std::unordered_map<HWND, void*> m_detached_presentation_views;
    /// Local Metal presentation views indexed by the HWND whose client area
    /// they render. Values are unretained and live in the native view tree.
    mutable std::unordered_map<HWND, void*> m_presentation_views;
    /// Last hidden state pushed to each presentation view.
    ///
    /// Every peer present used to call set_presentation_view_hidden(false)
    /// and set_control_z_order(HWND_TOP) unconditionally, and each of those
    /// is a SYNCHRONOUS main-thread round trip. After the first present both
    /// are no-ops -- the view is already visible and already on top -- but the
    /// cost was paid anyway, per child, per frame. With ~37 hosted children
    /// that is over a hundred serialized hops per UI frame on a single main
    /// thread. Remembering what was last requested lets the common case skip
    /// the hop entirely.
    ///
    /// Guarded by `m_presentation_state_mutex` because presents arrive on IPC
    /// worker threads, not the main thread.
    mutable std::mutex m_presentation_state_mutex;
    mutable std::unordered_map<HWND, bool> m_presentation_view_hidden;
    /// Last size `grow_hosted_peer_child_to_present_size` actually applied.
    ///
    /// That call is made once per hosted child PER PRESENT and its body is a
    /// synchronous main-thread round trip. In steady state the size has not
    /// changed, so the hop decides to do nothing -- but the calling IPC worker
    /// is parked for a main-thread run-loop turn to find that out, once per
    /// child per frame. One launcher hosts ~36 children.
    ///
    /// Invalidated by every other path that can move or resize the view
    /// (`note_control_frame_changed`), because a cache that outlived an
    /// external resize would skip a grow that was genuinely needed.
    mutable std::unordered_map<HWND, std::pair<i32, i32>> m_peer_present_size;
    std::unordered_map<u64, void*> m_window_scrollers; // (HWND << 1) | axis; owned by view tree.
    std::unordered_map<HWND, void*> m_controls;  ///< Child HWND → NSView* for SetWindowText
    /// Controls the guest hid with ShowWindow(SW_HIDE). Re-showing the host
    /// window must not resurrect them: a dialog that reflows itself (hiding
    /// the controls it does not need) would otherwise keep painting them.
    std::unordered_set<HWND> m_controls_hidden_by_guest;
    /// Peer-process WS_CHILD HWNDs hosted as local NSViews after SetParent.
    std::unordered_set<HWND> m_hosted_peer_children;
    /// Child HWNDs created with WS_EX_TRANSPARENT.
    ///
    /// Measured on Windows 11 (tools/oracle_probe/child_surface_order_probe.cpp,
    /// 48 cases): a WS_EX_TRANSPARENT child never covers a sibling. That holds
    /// whether it validates, erases with a class brush or FillRects its whole
    /// area, and whether it sits at HWND_TOP or HWND_BOTTOM. It holds for GDI,
    /// layered and DirectComposition siblings alike. A non-transparent sibling
    /// at HWND_TOP does cover all three kinds.
    ///
    /// So these views are composited beneath their siblings whatever their
    /// HWND z-order. See restack_transparent_children().
    std::unordered_set<HWND> m_transparent_children;
    /// Explicit SetParent relationship for peer-process compositor hosts.
    /// The HWND relationship remains stable when an OOP GPU process replaces
    /// its probe surface and the native view hierarchy is rebuilt.
    std::unordered_map<HWND, HWND> m_hosted_peer_parents;
    /// Win32 top-left captured at SetParent. Reconstructing it from an
    /// oversized Cocoa frame during a probe-surface replacement is ambiguous.
    std::unordered_map<HWND, std::pair<i32, i32>> m_hosted_peer_origins;
    /// Placeholder peer hosts whose NSView size came from Present, not from
    /// CreateWindow/SetWindowPos. Later composition surfaces may resize them.
    std::unordered_set<HWND> m_hosted_peer_present_sized;
    std::unordered_map<HWND, void*> m_guest_pixel_views; ///< Child HWND → VtrPixelView*
    /// Child HWNDs whose guest-painted surface takes mouse input itself
    /// (set_control_owns_input). Main thread only.
    std::unordered_set<HWND> m_input_owning_controls;
    std::unordered_map<HWND, void*> m_video_overlays; ///< Owner HWND → NSWindow*
    struct NativeControlState {
        HWND parent{0};
        HWND owner{0};
        u32 style{0};
        u16 class_ordinal{0};
        u32 ctrl_id{0};
        u32 radio_group{0}; ///< Non-zero for radio buttons (WS_GROUP-aware)
    };
    std::unordered_map<HWND, NativeControlState> m_control_state;
    std::unordered_map<HWND, std::vector<HWND>> m_control_order;
    std::unordered_map<HWND, std::vector<HWND>> m_tab_order;
    std::unordered_map<HWND, std::unordered_map<u32, HWND>> m_mnemonics;
    std::unordered_map<HWND, u32> m_pending_static_mnemonic;
    struct ToolbarControl { std::vector<ToolbarButton> buttons; std::vector<void*> images; i32 width = 24; i32 height = 22; };
    std::unordered_map<HWND, ToolbarControl> m_toolbars;
    void render_toolbar(HWND hwnd);
    void forget_common_control(HWND hwnd);
    struct StatusParts { std::vector<i32> edges; std::vector<std::wstring> text; };
    std::unordered_map<HWND, StatusParts> m_status_parts;
    void render_status_parts(HWND hwnd);
    std::unordered_map<HWND, std::vector<u64>> m_combo_item_data;
    std::unordered_map<HWND, std::vector<std::wstring>> m_listbox_items; ///< ListBox line storage
    std::unordered_map<HWND, i32> m_listbox_selection; ///< ListBox selection index
    std::unordered_map<HWND, std::vector<std::wstring>> m_listview_items; ///< SysListView32 line storage

    struct TabControlData {
        void* tab_view{nullptr}; ///< VtrTabStripView*
        HWND parent{0};
        u32 ctrl_id{0};
        std::vector<std::wstring> labels;
        std::vector<u64> lparams;
        std::vector<void*> page_views; ///< optional dialog page NSViews*
        i32 selected{0};
        usize next_page_index{0};
    };
    std::unordered_map<HWND, TabControlData> m_tab_controls;

    // WS_GROUP-aware radio grouping: each Win32 WS_GROUP group gets a distinct
    // native click selector so macOS treats it as an independent radio group.
    u32 m_radio_group_seq{0};     ///< monotonic group id source
    u32 m_active_radio_group{0};  ///< group id the next radios join

    // SysTreeView32 state, keyed by control HWND.
    struct TreeItemData {
        u64 parent{0};
        u64 lparam{0};
        bool has_children{false};
        std::wstring text;
        std::vector<u64> children;
    };
    struct TreeViewData {
        std::unordered_map<u64, TreeItemData> items; ///< HTREEITEM → item
        std::vector<u64> roots;
        u64 next_hitem{1};
        u64 selected{0};
        void* outline{nullptr};    ///< NSOutlineView*
        void* controller{nullptr}; ///< VtrTreeController* (data source/delegate)
        HWND parent{0};
        u32 ctrl_id{0};
    };
    std::unordered_map<HWND, TreeViewData> m_treeviews;
    std::deque<TreeNotify> m_tree_notifies; ///< pending user-click selections
    std::mutex m_tree_notify_mutex;         ///< guards m_tree_notifies (main<->guest)
    std::atomic<bool> m_has_tree_notify{false}; ///< cheap fast-path for pop_tree_notify

    std::unordered_map<u32, void*> m_bitmaps;  ///< HBITMAP(handle) → NSImage* (retained)
    std::unordered_map<u32, BitmapDims> m_bitmap_dims; ///< HBITMAP(handle) → dimensions
    std::unordered_map<u32, std::vector<u8>> m_bitmap_bmp_data; ///< raw BMP file bytes
    u32 m_next_bitmap_handle = 0x60000000;

    /// NSMenuItem* keyed by Win32 menu item ID (for CheckMenuItem/EnableMenuItem)
    std::unordered_map<u16, void*> m_menu_items_by_id;

    /// Pending check states queued before install_menu_bar (true = checked)
    std::unordered_map<u16, bool> m_pending_check_states;

    /// NSMenu* keyed by Win32 HMENU handle (for AppendMenuW to find submenus)
    std::unordered_map<u32, void*> m_submenus_by_handle;

    /// The HWND that owns the menu bar (for WM_COMMAND dispatch on new items)
    HWND m_menu_target_hwnd{0};

    bool m_app_initialized = false;
    bool m_app_launched = false;     ///< True once finishLaunching has been called
    bool m_automatic_termination_guard_active = false;
    bool m_app_name_set = false;  ///< True once the app name has been set from the first window
    bool m_cursor_hidden = false; ///< True while the native cursor is hidden (main thread only)
    /// Whether this bridge owns one outstanding CGDisplayHideCursor call.
    /// Pair against this bit; CGCursorIsVisible is deprecated and does not
    /// expose the process-owned hide count reliably on current macOS.
    bool m_host_cursor_hide_applied = false;
    /// Retained NSCursor* per animation frame, the parallel delays, the
    /// current step, and the retained one-shot NSTimer that advances it.
    /// Empty/null whenever a still cursor is installed.
    std::vector<void*> m_cursor_anim_frames;
    std::vector<u32> m_cursor_anim_delays;
    usize m_cursor_anim_step{0};
    void* m_cursor_anim_timer{nullptr};

    /// Drop any running animation. Main thread.
    void stop_cursor_animation();
    /// Show the next frame and schedule the one after it. Main thread.
    void advance_cursor_animation();
    /// Arm the one-shot timer for the current frame's delay. Main thread.
    void schedule_cursor_animation_tick();

    /// Retained NSCursor* for the last successful set_custom_cursor_rgba.
    void* m_custom_ns_cursor = nullptr;

    /// Last visibility requested through set_cursor_visible(), visible to any
    /// thread: -1 unknown, 0 hidden, 1 shown. One guest drives
    /// SetCursor(NULL) once per frame on its software-cursor path, and each
    /// of those used to
    /// pay a *synchronous* hop onto the Cocoa main thread while that thread
    /// was busy draining window-server events. Caching the request lets the
    /// no-op calls skip the hop entirely.
    std::atomic<int> m_cursor_visible_req{-1};
    /// Bumped whenever m_custom_ns_cursor is swapped. A visibility request
    /// that matches m_cursor_visible_req must still hop when the shape moved
    /// on underneath it, so the cache compares both.
    std::atomic<unsigned> m_cursor_shape_gen{0};
    std::atomic<unsigned> m_cursor_shape_applied{0};

    /// Guest ClipCursor rect in Windows Y-down screen coordinates.
    bool m_guest_cursor_clip_enabled = false;
    Rect m_guest_cursor_clip_win{};
    /// Exclusive fullscreen HWND whose display currently owns auto-confine
    /// (0 = none). Takes effect when the app is active and presentation is
    /// not suspended.
    HWND m_exclusive_cursor_confine_hwnd{NULL_HWND};
    /// Effective confinement in Cocoa global coordinates (bottom-left origin).
    bool m_cursor_clip_cocoa_active = false;
    /// The exclusive guest clip is confining to the whole display only because
    /// the presentation surface could not map it yet; enforce_cursor_clip
    /// recomputes it until the surface can.
    bool m_cursor_clip_surface_pending = false;
    double m_clip_cocoa_x_min = 0;
    double m_clip_cocoa_y_min = 0;
    double m_clip_cocoa_x_max = 0;
    double m_clip_cocoa_y_max = 0;
    void* m_cursor_clip_monitor = nullptr; ///< id NSEvent local monitor
    void* m_cursor_clip_global_monitor = nullptr; ///< id NSEvent global monitor
    /// id NSEvent global monitor: mouse motion macOS routes to other apps
    /// while this one is active (note_offwindow_mouse_motion).
    void* m_offwindow_motion_monitor = nullptr;
    /// Warp the host cursor to @p x, @p y (CG global, top-left origin) and
    /// record the displacement for CursorWarpDeltas. Main-thread only.
    [[nodiscard]] bool warp_host_cursor(double x, double y);
    CursorWarpDeltas m_cursor_warp_deltas;  ///< main thread

    /// Recompute effective Cocoa clip from guest + exclusive state and (re)install
    /// the mouse-moved monitor. Main-thread only.
    void refresh_cursor_clip_state();
    /// Warp the cursor inside the active clip if it has left. Main-thread only.
    void enforce_cursor_clip();
    /// Map guest screen coords (GetCursorPos / ClipCursor space) to Cocoa
    /// global bottom-left coordinates via the window's presentation surface.
    /// @p require_surface: fail when no presented surface owns the point
    /// (the exclusive ClipCursor path, which then holds the whole display)
    /// instead of mapping it 1:1 through the content view (SetCursorPos).
    /// Main-thread only.
    [[nodiscard]] bool map_guest_screen_to_cocoa(HWND hwnd, i32 guest_x,
                                                 i32 guest_y, double& cocoa_x,
                                                 double& cocoa_y,
                                                 bool require_surface = false) const;
    std::vector<u8> m_pending_icon;  ///< Unbundled-launch ICO fallback for the Dock tile
    /// key -> retained VtrStatusItemHost (cocoa_notify_icons.mm). Main thread.
    std::unordered_map<u64, void*> m_notify_icons;
    NotifyIconInputHandler m_notify_icon_input;
    std::function<std::optional<u64>()> m_notify_icon_reopen_target;
    std::function<std::string(HWND)> m_app_user_model_id_resolver;
    MessageQueue* m_queue = nullptr;  ///< Cached queue for delegate callbacks
    std::atomic<bool> m_in_live_resize{false}; ///< True while NSWindow is in live resize tracking
    std::atomic<bool> m_shutdown{false}; ///< Teardown: skip/bail cross-thread pumps
    u32 m_mouse_buttons{0};  ///< Bit 0 = left, bit 1 = right (tracked across events)
    WheelDeltaAccumulator m_wheel_delta_x;
    WheelDeltaAccumulator m_wheel_delta_y;
    std::atomic<u32> m_async_mouse_presses{0}; ///< Mouse buttons pressed since async query
    std::array<std::atomic<u64>, 4> m_native_keys_down{};
    std::array<std::atomic<u64>, 4> m_async_key_presses{};
    NativeKeyTransitionCallback m_native_key_transition_callback;
    NativeMouseButtonCallback m_native_mouse_button_callback;
    NativeMouseMotionCallback m_native_mouse_motion_callback;
    NativeMouseWheelCallback m_native_mouse_wheel_callback;
    NativeResizeCallback m_resize_callback;  ///< Callback to update HwndManager on native resize
    NativeMoveCallback m_move_callback;  ///< Callback to update HwndManager on a native move
    NativeShowStateCallback m_show_state_callback;
    NativeHitTestCallback m_hit_test_callback;
    int m_host_input_pump_depth{0};

    /// Shared pixel buffer for live resize: guest stores here, main-thread
    /// timer reads and blits.  Protected by m_pending_px_mutex.
    struct PendingPixels {
        std::vector<u8> data;
        i32 width  = 0;
        i32 height = 0;
        bool dirty = false;
    };
    std::mutex m_pending_px_mutex;
    std::unordered_map<HWND, PendingPixels> m_pending_pixels;
public:
    /// Blit pending pixels for the given HWND (must be called on main thread).
    void blit_pending_pixels(HWND hwnd);
private:

    /// Bootstrap NSApplication on first window creation
    void ensure_app_initialized();
    /// Complete NSApp launch with the given window title as process name
    void finish_app_launch(const std::wstring& title);

    /// Find the HWND associated with a native NSWindow pointer
    HWND find_hwnd_for_window(void* ns_win) const;

    /// Retarget top-level client coordinates to a composition-backed child
    /// HWND when the native hit view is that child's Metal presentation view.
    HWND retarget_presentation_mouse(HWND target, i32& client_x,
                                     i32& client_y,
                                     HWND* containing_hwnd = nullptr) const;

    // Main-thread-only host gesture state. No Windows messages live here.
    NativeMoveSizeCallback m_move_size_callback;
    std::unordered_set<HWND> m_active_move_sizes;
    std::atomic<bool> m_owns_native_resize{false};
};

} // namespace vitreo::windowing
