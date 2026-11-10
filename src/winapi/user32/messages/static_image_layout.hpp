// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file static_image_layout.hpp
/// @brief Where an SS_BITMAP / SS_ICON static puts itself once it has an image.
///
/// Learn, Static Control Styles (Winuser.h):
///  - SS_BITMAP / SS_ICON: "the control automatically sizes itself to
///    accommodate the bitmap" (icon); nWidth/nHeight are ignored.
///  - SS_CENTERIMAGE: "The control is not resized, so that a bitmap too large
///    for the control will be clipped."
///  - SS_REALSIZECONTROL: the bitmap is fitted to the control, not the
///    control to the bitmap.
///  - SS_RIGHTJUST: "The lower right corner ... is to remain fixed when the
///    control is resized. Only the top and left sides are adjusted."

#include "windowing/model/window_types.hpp"

#include <vitreo/common.hpp>

#include <cstring>

namespace vitreo::winapi {

inline constexpr u32 kSsRealSizeControl = 0x0040;
inline constexpr u32 kSsCenterImage = 0x0200;
inline constexpr u32 kSsRightJust = 0x0400;

/// The window rect (parent client coordinates) a static with @p style takes
/// after showing an image of @p image_w x @p image_h. @p window and @p client
/// are its current window and client rects; their size difference is the
/// non-client border kept around the image. Returns @p window unchanged when
/// the style keeps the control's size or the image is empty.
[[nodiscard]] inline windowing::Rect static_image_window_rect(
    const windowing::Rect& window, const windowing::Rect& client, u32 style,
    i32 image_w, i32 image_h) noexcept {
    if ((style & (kSsCenterImage | kSsRealSizeControl)) != 0) return window;
    if (image_w <= 0 || image_h <= 0) return window;
    const i32 border_w = window.width() - client.width();
    const i32 border_h = window.height() - client.height();
    const i32 w = image_w + (border_w > 0 ? border_w : 0);
    const i32 h = image_h + (border_h > 0 ? border_h : 0);
    if ((style & kSsRightJust) != 0) {
        return {window.right - w, window.bottom - h, window.right, window.bottom};
    }
    return {window.left, window.top, window.left + w, window.top + h};
}

/// Width and height of a packed DIB (RT_BITMAP resource): BITMAPCOREHEADER
/// (12 bytes, WORD dimensions) or BITMAPINFOHEADER and later (LONG
/// dimensions; a negative biHeight is a top-down DIB). False when @p dib is
/// too short to hold its header.
[[nodiscard]] inline bool packed_dib_size(const u8* dib, usize size, i32& width,
                                          i32& height) noexcept {
    if (dib == nullptr || size < 12) return false;
    u32 header = 0;
    std::memcpy(&header, dib, 4);
    if (header == 12) {
        u16 w = 0, h = 0;
        std::memcpy(&w, dib + 4, 2);
        std::memcpy(&h, dib + 6, 2);
        width = w;
        height = h;
        return true;
    }
    if (header < 16 || size < 12) return false;
    i32 w = 0, h = 0;
    std::memcpy(&w, dib + 4, 4);
    std::memcpy(&h, dib + 8, 4);
    width = w;
    height = h < 0 ? -h : h;
    return true;
}

}  // namespace vitreo::winapi
