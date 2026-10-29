#pragma once

#include <array>
#include <vector>

#include "windowing/contracts/bitmap_host.hpp"
#include "windowing/contracts/cursor_host.hpp"
#include "windowing/contracts/menu_types.hpp"
#include "windowing/contracts/window_host.hpp"

#include <string>
#include <vector>

namespace vitreo::windowing {

struct HostScrollInfo {
    i32 n_min{0};
    i32 n_max{0};
    u32 n_page{0};
    i32 n_pos{0};
    i32 n_track_pos{0};
};

/// Aggregate composition boundary for USER32 host services.
class User32Host : public BitmapHost,
                   public CursorHost,
                   public WindowHost {
public:
    ~User32Host() override = default;

    /// SetWindowRgn. (left, top, right, bottom) bound the region; `parts`
    /// lists its non-overlapping rectangles (empty = the bounding rectangle
    /// alone). Window-relative Win32 pixels.
    virtual void set_window_rgn(HWND hwnd, bool has_rgn, i32 left, i32 top,
                                i32 right, i32 bottom, bool redraw,
                                const std::vector<std::array<i32, 4>>& parts) = 0;
    virtual void set_keyboard_focus_hwnd(HWND hwnd) = 0;

    [[nodiscard]] virtual bool has_native_window(HWND hwnd) const = 0;
    virtual void set_client_drawn_frame(HWND hwnd, bool client_drawn) = 0;
    virtual bool set_control_frame(HWND hwnd, const Rect& rect) = 0;
    virtual bool set_control_z_order(HWND hwnd, HWND insert_after) = 0;
    virtual void retire_local_host_after_peer_reparent(HWND hwnd) = 0;
    virtual bool set_control_parent(HWND child, HWND parent) = 0;
    [[nodiscard]] virtual bool has_control_view(HWND hwnd) const = 0;
    virtual bool ensure_hosted_peer_child(
        HWND parent, HWND child, const Rect& frame) = 0;

    [[nodiscard]] virtual std::vector<MonitorInfo>
    get_all_monitor_info() const = 0;
    virtual void get_screen_pixel_size(i32& out_w, i32& out_h) const = 0;
    virtual void set_window_fullscreen(HWND hwnd, bool enabled) = 0;
    /// FlashWindow / FlashWindowEx: start (@p start) or stop drawing the
    /// user's attention to @p hwnd — on macOS, Dock attention
    /// (.docs/windowing-native-presentation.md, "FlashWindow maps to Dock
    /// attention").
    virtual void request_user_attention(HWND hwnd, bool start) = 0;

    [[nodiscard]] virtual bool set_custom_cursor_rgba(
        u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
        const u8* rgba, usize byte_count) = 0;
    /// Install an animated cursor: @p frames in display order, @p delays_ms
    /// parallel to it, all frames @p width x @p height. Default: not
    /// supported, so the caller installs frame 0 as a still cursor instead.
    [[nodiscard]] virtual bool set_animated_cursor_rgba(
        u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
        const std::vector<std::vector<u8>>& frames,
        const std::vector<u32>& delays_ms) {
        (void)width; (void)height; (void)hotspot_x; (void)hotspot_y;
        (void)frames; (void)delays_ms;
        return false;
    }

    virtual void set_system_cursor(u32 idc) = 0;
    virtual void set_cursor_visible(bool visible) = 0;

    [[nodiscard]] virtual i16 get_native_key_state(u32 vk_code) const = 0;
    [[nodiscard]] virtual i16 get_native_async_key_state(u32 vk_code) = 0;

    [[nodiscard]] virtual bool get_control_scroll_info(
        HWND hwnd, HostScrollInfo& out) const = 0;

    virtual bool update_window_scrollbar(HWND, u32, const ScrollBarState&, bool) { return false; }
    virtual bool get_scrollbar_tracking(HWND, u32, i32&) const { return false; }

    virtual void append_menu_item(u32 hmenu, u16 item_id,
                                  const std::string& text,
                                  bool is_separator) = 0;
    virtual void register_popup_menu(u32 hmenu) = 0;
    virtual void enable_menu_item(u16 item_id, bool enabled) = 0;
    virtual void check_menu_item(u16 item_id, bool checked) = 0;
    virtual void install_menu_bar(HWND hwnd, const Win32MenuBar& menu_bar) = 0;

    [[nodiscard]] virtual u32 get_button_check(HWND hwnd) const = 0;

    virtual void clipboard_clear() = 0;
    [[nodiscard]] virtual bool clipboard_set_text(
        const std::string& utf8_text) = 0;
    [[nodiscard]] virtual std::string clipboard_get_text() = 0;
    /// The host pasteboard's change counter: it moves whenever any
    /// application (this one included) changes the contents.
    [[nodiscard]] virtual i64 clipboard_change_count() = 0;
};

} // namespace vitreo::windowing
