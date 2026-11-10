#pragma once

#include "windowing/model/window_types.hpp"
#include <algorithm>
#include <optional>
#include <cmath>

namespace vitreo::windowing {

// Windows cursor_clip_contract_probe: the branches are ordered. In particular,
// a zero-width span maps a point below the minimum to lo, but a point at or
// above it to hi - 1. Use a wider intermediate before virtual-screen clipping
// so an extreme guest RECT cannot overflow.
[[nodiscard]] constexpr i64 confine_cursor_axis(i64 value, i32 lo,
                                               i32 hi) noexcept {
    if (value < lo) return lo;
    if (value >= hi) return static_cast<i64>(hi) - 1;
    return value;
}

[[nodiscard]] inline Point confine_cursor_coordinates(i64 x, i64 y,
    const Rect& screen, const Rect* clip) noexcept {
    if (clip) {
        x = confine_cursor_axis(x, clip->left, clip->right);
        y = confine_cursor_axis(y, clip->top, clip->bottom);
    }
    x = confine_cursor_axis(x, screen.left, screen.right);
    y = confine_cursor_axis(y, screen.top, screen.bottom);
    return {static_cast<i32>(x), static_cast<i32>(y)};
}

[[nodiscard]] inline Point confine_cursor_point(Point point, const Rect& screen,
                                                const Rect* clip) noexcept {
    return confine_cursor_coordinates(point.x, point.y, screen, clip);
}

// A touching intersection is valid: Windows preserves zero-width/height
// clips, including a point on the virtual desktop's right/bottom edge.
[[nodiscard]] inline std::optional<Rect> normalize_cursor_clip(
    const Rect& requested, const Rect& screen) noexcept {
    if (requested.right < requested.left || requested.bottom < requested.top)
        return std::nullopt;
    Rect result{std::max(requested.left, screen.left),
                std::max(requested.top, screen.top),
                std::min(requested.right, screen.right),
                std::min(requested.bottom, screen.bottom)};
    if (result.right < result.left || result.bottom < result.top)
        return std::nullopt;
    return result;
}

[[nodiscard]] inline bool cursor_clip_is_point(const Rect& clip) noexcept {
    return static_cast<i64>(clip.right) - clip.left <= 1 &&
           static_cast<i64>(clip.bottom) - clip.top <= 1;
}

// Learn (ClipCursor): the system adjusts a cursor position only when it lies
// outside the confining rectangle. A point clip has no interior, so every
// sample is repositioned onto that point. Host bounds are the clip's outer
// edges; a position on the max edge is still inside.
[[nodiscard]] inline bool cursor_clip_should_reposition_host(
    double x, double y, double x_min, double y_min, double x_max, double y_max,
    bool point_clip) noexcept {
    if (point_clip) return true;
    return x < x_min || y < y_min || x > x_max || y > y_max;
}

// A nonzero Windows relative packet clamps BOTH axes. An idle/zero packet
// does not reclamp a degenerate rectangle (Windows motion oracle, both bits).
[[nodiscard]] inline Point move_point_cursor_clip(Point previous, double dx,
    double dy, const Rect& screen, const Rect& clip) noexcept {
    if ((dx == 0 && dy == 0) || !std::isfinite(dx) || !std::isfinite(dy))
        return previous;
    const auto axis = [](double value, i32 lo, i32 hi) -> i64 {
        if (value < lo) return lo;
        if (value >= hi) return static_cast<i64>(hi) - 1;
        return static_cast<i64>(value);
    };
    const i64 x = axis(static_cast<double>(previous.x) + dx, clip.left, clip.right);
    const i64 y = axis(static_cast<double>(previous.y) + dy, clip.top, clip.bottom);
    return {static_cast<i32>(confine_cursor_axis(x, screen.left, screen.right)),
            static_cast<i32>(confine_cursor_axis(y, screen.top, screen.bottom))};
}

// Main-thread-only native ownership. Failed native operations never invent an
// acquired/released state. A cached warp prevents repeatedly applying the
// directional zero-extent clamp to its own stationary result.
class CursorClipNativeState {
public:
    [[nodiscard]] bool owns_point_lock() const noexcept { return m_owned; }
    void invalidate_position() noexcept { m_position_valid = false; m_guest.reset(); }
    void note_warp(double x, double y) noexcept {
        m_x = x; m_y = y; m_position_valid = true; m_guest.reset();
    }
    void note_guest_point(Point point) noexcept { m_guest = point; }
    [[nodiscard]] std::optional<Point> guest_point() const noexcept { return m_guest; }
    [[nodiscard]] bool is_last_warp(double x, double y) const noexcept {
        return m_position_valid && x == m_x && y == m_y;
    }
    template <typename Associate>
    bool acquire_point_lock(Associate&& associate) {
        if (!associate(false)) return false;
        m_owned = true;
        return true;
    }
    template <typename Associate>
    bool release_point_lock(Associate&& associate) {
        invalidate_position();
        if (!m_owned) return true;
        if (!associate(true)) return false;
        m_owned = false;
        return true;
    }
private:
    bool m_owned = false;
    bool m_position_valid = false;
    double m_x = 0, m_y = 0;
    std::optional<Point> m_guest;
};

} // namespace vitreo::windowing
