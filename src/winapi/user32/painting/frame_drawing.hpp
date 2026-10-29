// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file frame_drawing.hpp
/// @brief user32's frame and focus drawing on any DC (frame_drawing.cpp).

#include <vitreo/common.hpp>
#include "winapi/kernel32/handle.hpp"
#include "winapi/user32/painting/painting.hpp"

namespace vitreo::winapi {
class GdiManager;
}

namespace vitreo::winapi::user_draw {

/// DrawEdge; @p rc is adjusted for BF_ADJUST. False for a bad DC.
bool draw_edge(GdiManager& gdi, Handle hdc, Rect& rc, u32 edge, u32 flags);
/// DrawFrameControl; false (logged) for the kinds not drawn here.
bool draw_frame_control(GdiManager& gdi, Handle hdc, Rect& rc, u32 type, u32 state);
bool draw_focus_rect(GdiManager& gdi, Handle hdc, const Rect& rc);
bool invert_rect(GdiManager& gdi, Handle hdc, const Rect& rc);

}  // namespace vitreo::winapi::user_draw
