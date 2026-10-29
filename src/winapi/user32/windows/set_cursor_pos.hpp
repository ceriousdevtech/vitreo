#pragma once

#include "windowing/contracts/cursor_host.hpp"
#include "windowing/model/window_types.hpp"

namespace vitreo::winapi {

class Marshaler;

struct CursorPoint {
    i32 x = 0;
    i32 y = 0;
};

/// The position Windows actually gives the cursor for a requested (x, y).
///
/// Microsoft Learn, SetCursorPos: "If the new coordinates are not within the
/// screen rectangle set by the most recent ClipCursor function call, the
/// system automatically adjusts the coordinates so that the cursor stays
/// within the rectangle." Unclipped, that rectangle is the virtual screen
/// (GetClipCursor reports it; SM_[XY]VIRTUALSCREEN / SM_C[XY]VIRTUALSCREEN).
/// Both are RECTs, whose right and bottom edges are exclusive (Learn, RECT
/// structure), so the largest reachable position is (right - 1, bottom - 1).
///
/// @param clip The active ClipCursor rectangle, or nullptr when unclipped.
///             The cursor never leaves the screen either way, so the result
///             is also confined to @p screen.
[[nodiscard]] CursorPoint confine_cursor_point(
    i32 x, i32 y, const windowing::Rect& screen,
    const windowing::Rect* clip) noexcept;

/// BOOL SetCursorPos(int X, int Y). @p screen is the guest virtual-screen
/// rectangle (the same one GetSystemMetrics(SM_*VIRTUALSCREEN) reports).
void impl_SetCursorPos(Marshaler& m, windowing::CursorHost& cursor_host,
                       const windowing::Rect& screen);

} // namespace vitreo::winapi
