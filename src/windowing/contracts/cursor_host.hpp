#pragma once

#include <vitreo/common.hpp>
#include "windowing/model/window_types.hpp"

namespace vitreo::windowing {

/// Platform cursor position operations. Coordinates use the guest's Win32
/// virtual-screen space; mapping to native displays remains host-specific.
class CursorHost {
public:
    virtual ~CursorHost() = default;

    virtual void get_native_cursor_pos(i32& out_x, i32& out_y) const = 0;
    [[nodiscard]] virtual bool set_native_cursor_pos(i32 x, i32 y) = 0;
    // Both rectangles are in guest screen coordinates; right/bottom are
    // exclusive, and a zero extent is valid. Native failures return false.
    [[nodiscard]] virtual bool set_cursor_clip(bool enabled, i32 left, i32 top,
                                 i32 right, i32 bottom, const Rect& guest_screen) = 0;
    virtual void update_cursor_clip_screen(const Rect& guest_screen) = 0;
    [[nodiscard]] virtual bool get_cursor_clip(
        i32& left, i32& top, i32& right, i32& bottom) const = 0;
    [[nodiscard]] virtual u32 last_input_tick_count() const noexcept = 0;
};

} // namespace vitreo::windowing
