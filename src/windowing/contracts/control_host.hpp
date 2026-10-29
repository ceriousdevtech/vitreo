#pragma once

#include "windowing/contracts/display_types.hpp"
#include "windowing/model/window_types.hpp"

#include <string>
#include <string_view>

namespace vitreo::windowing {

class MessageQueue;

struct TreeNotify {
    enum class Kind {
        Selection,
        Expanding,
        Collapsing,
    };

    HWND parent;
    HWND ctrl;
    u32 ctrl_id;
    u64 h_item;
    u64 lparam;
    Kind kind{Kind::Selection};
};

[[nodiscard]] inline bool decode_tab_notify_lparam(
    u64 lparam, HWND* ctrl_out,
    bool* selection_changing_out) noexcept {
    constexpr u64 marker_base = 0x5654525400000000ULL;
    constexpr u64 marker_mask = 0xFFFFFFFE00000000ULL;
    constexpr u64 changing_bit = 0x0000000100000000ULL;
    if ((lparam & marker_mask) != marker_base) {
        return false;
    }
    if (ctrl_out) {
        *ctrl_out = static_cast<HWND>(lparam & 0xFFFFFFFFULL);
    }
    if (selection_changing_out) {
        *selection_changing_out = (lparam & changing_bit) != 0;
    }
    return true;
}

/// Platform host operations used by Win32 control/window text and enabled-state
/// APIs. Windows-visible state and validation remain owned by user32.
class ControlHost {
public:
    virtual ~ControlHost() = default;

    [[nodiscard]] virtual bool set_control_text(
        HWND hwnd, const std::wstring& text) = 0;
    [[nodiscard]] virtual std::wstring get_control_text(HWND hwnd) const = 0;
    virtual void set_title(HWND hwnd, std::wstring_view title) = 0;
    [[nodiscard]] virtual bool set_control_enabled(HWND hwnd, bool enabled) = 0;
    virtual bool set_button_check(HWND hwnd, bool checked) = 0;
    virtual void apply_auto_button_click(HWND hwnd) = 0;

    virtual void add_child_view(
        HWND parent, HWND child, const WindowData& data, u16 class_ordinal,
        const std::wstring& class_name, const std::wstring& text) = 0;
    virtual bool set_control_image_data(
        HWND hwnd, const u8* data, usize size) = 0;
    virtual bool set_control_font(
        HWND hwnd, float point_size, u16 weight, bool italic) = 0;
    /// As set_control_font, in the guest's face: the native control must
    /// draw with the font GDI measured the guest's layout with.
    virtual bool set_control_font_face(HWND hwnd, const std::u16string& face,
                                       float point_size, u16 weight, bool italic) {
        (void)face;
        return set_control_font(hwnd, point_size, weight, italic);
    }
    /// USER32 owns this control's painting and input (its class procedure
    /// draws every pixel, e.g. an owner-draw LISTBOX): mouse input over its
    /// guest-painted surface goes to the HWND as WM_* messages instead of to
    /// the native view underneath.
    virtual void set_control_owns_input(HWND hwnd) { (void)hwnd; }
    virtual void add_container_view(
        HWND parent, HWND container, const WindowData& data) = 0;
    virtual VoidResult create_window(HWND hwnd, const WindowData& data) = 0;
    virtual void destroy_window(HWND hwnd) = 0;
    virtual void set_visible(HWND hwnd, bool visible) = 0;
    [[nodiscard]] virtual MonitorInfo get_primary_monitor_info() const = 0;
    virtual bool activate_window(HWND hwnd) = 0;

    /// TrackMouseEvent: arm one-shot mouse-leave / mouse-hover notification
    /// for `hwnd`. The host posts WM_MOUSELEAVE once the pointer is over a
    /// different window (or none), and WM_MOUSEHOVER once it has rested
    /// inside `hwnd` for `hover_ms`. Windows cancels a request when it fires,
    /// so each is genuinely one-shot.
    /// @param leave  arm TME_LEAVE
    /// @param hover  arm TME_HOVER
    /// @param hover_ms dwell before WM_MOUSEHOVER; 0 selects the system default
    /// SetCapture/ReleaseCapture: while a window holds capture, Win32 sends it
    /// every mouse message regardless of where the pointer is. Pass NULL_HWND
    /// to release. The HwndManager remains the authority the guest reads back
    /// through GetCapture; this is the routing mirror the event pump needs.
    virtual void set_mouse_capture(HWND hwnd) = 0;

    virtual void track_mouse_event(HWND hwnd, bool leave, bool hover,
                                   u32 hover_ms) = 0;
    /// TrackMouseEvent(TME_CANCEL): disarm whichever requests are named.
    virtual void cancel_mouse_tracking(HWND hwnd, bool leave, bool hover) = 0;
    /// TrackMouseEvent(TME_QUERY): what is currently armed for `hwnd`.
    virtual void query_mouse_tracking(HWND hwnd, bool& leave, bool& hover,
                                      u32& hover_ms) const = 0;

    /// Show the tooltip control `hwnd`'s bubble at a screen point, or hide it.
    /// The text is the tool's current text; the point is where TTM_TRACKPOSITION
    /// last put it.
    virtual void show_tooltip(HWND hwnd, const std::wstring& text,
                              i32 screen_x, i32 screen_y) = 0;
    virtual void hide_tooltip(HWND hwnd) = 0;
    /// Measured bubble size for `text` at `max_width` (0 = unconstrained),
    /// which TTM_GETBUBBLESIZE reports.
    virtual void measure_tooltip(const std::wstring& text, i32 max_width,
                                 i32& out_width, i32& out_height) const = 0;

    virtual void pump_events(MessageQueue& queue) = 0;
    virtual void wait_events(MessageQueue& queue, u32 timeout_ms) = 0;
    virtual bool pop_tree_notify(TreeNotify& out) = 0;
    [[nodiscard]] virtual i32 tab_get_item_count(HWND hwnd) const = 0;
};

} // namespace vitreo::windowing
