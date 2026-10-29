// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_dib.cpp
/// @brief Device-independent bitmaps.
///
/// CreateDIBSection, SetDIBits / GetDIBits, StretchDIBits, and the header
/// parsing they share. A DIB is a self-describing pixel buffer: a header giving
/// dimensions, bit depth and compression, an optional palette, then rows.
///
/// Two things about that layout are easy to get wrong and are handled here.
/// Rows are bottom-up unless the height is negative, so a positive height means
/// the first row in memory is the *last* row on screen. And in a BI_RGB 32bpp
/// DIB the fourth byte of each pixel is reserved, not alpha — carrying it
/// through as alpha renders every frame black.
///
/// Split out of gdi32.cpp. Closed in both directions once the ternary raster
/// operation codes moved to gdi32_internal.hpp, which is where the bitmap half
/// of the file needed them from too.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

/// @file ellipse_draw_ellipse_bounded_by.cpp
/// @brief Ellipse — draw ellipse bounded by rectangle using midpoint algorithm
///
/// Closed cut from gdi32_dib.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Ellipse — draw ellipse bounded by rectangle using midpoint algorithm
// =============================================================================

bool GdiManager::ellipse(Handle hdc, i32 left, i32 top, i32 right, i32 bottom) {
    ApiLock api_lock(*this);
    if (path_record_ellipse(hdc, left, top, right, bottom)) return true;  // inside BeginPath
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || !it->second.bitmap) return false;

    auto& dc = it->second;
    auto& bmp = dc.bitmap;

    // Get brush color for fill
    u32 brush_color = 0xFFFFFF; // white default
    auto br_it = m_brush_colors.find(dc.selected_brush);
    if (br_it != m_brush_colors.end()) {
        brush_color = br_it->second;
    }
    u8 fill_r = static_cast<u8>(brush_color & 0xFF);
    u8 fill_g = static_cast<u8>((brush_color >> 8) & 0xFF);
    u8 fill_b = static_cast<u8>((brush_color >> 16) & 0xFF);

    // Get pen color for outline
    u32 pen_color = 0;
    i32 pen_width = 1;
    auto pen_it = m_pen_objects.find(dc.selected_pen);
    if (pen_it != m_pen_objects.end()) {
        pen_color = pen_it->second.color;
        pen_width = pen_it->second.width;
        if (pen_it->second.style == 5) { // PS_NULL
            pen_color = brush_color; // no visible outline
            pen_width = 0;
        }
    }
    u8 pen_r = static_cast<u8>(pen_color & 0xFF);
    u8 pen_g = static_cast<u8>((pen_color >> 8) & 0xFF);
    u8 pen_b = static_cast<u8>((pen_color >> 16) & 0xFF);

    // Compute ellipse center and radii
    float cx = (left + right) * 0.5f;
    float cy = (top + bottom) * 0.5f;
    float rx = (right - left) * 0.5f;
    float ry = (bottom - top) * 0.5f;
    if (rx <= 0 || ry <= 0) return true;

    // Fill the ellipse scanline by scanline
    for (i32 y = top; y < bottom; ++y) {
        float dy = (y + 0.5f - cy) / ry;
        if (dy * dy > 1.0f) continue;
        float half_w = rx * std::sqrt(1.0f - dy * dy);
        i32 x0 = static_cast<i32>(cx - half_w);
        i32 x1 = static_cast<i32>(cx + half_w);

        for (i32 x = x0; x <= x1; ++x) {
            if (!bmp->in_bounds(x, y)) continue;
            if (!dc.clip_rect.contains(x, y)) continue;
            u8* dp = bmp->pixel_at(x, y);
            dp[0] = fill_r; dp[1] = fill_g; dp[2] = fill_b; dp[3] = 0xFF;
        }
    }

    // Draw the outline if pen is not null
    if (pen_width > 0) {
        // Use parametric approach for outline
        i32 steps = static_cast<i32>((rx + ry) * 4);
        if (steps < 64) steps = 64;
        for (i32 i = 0; i < steps; ++i) {
            float t = static_cast<float>(i) / steps * 2.0f * 3.14159265f;
            i32 px = static_cast<i32>(cx + rx * std::cos(t));
            i32 py = static_cast<i32>(cy + ry * std::sin(t));
            for (i32 pw = 0; pw < pen_width; ++pw) {
                i32 ppx = px + pw - pen_width / 2;
                if (!bmp->in_bounds(ppx, py)) continue;
                if (!dc.clip_rect.contains(ppx, py)) continue;
                u8* dp = bmp->pixel_at(ppx, py);
                dp[0] = pen_r; dp[1] = pen_g; dp[2] = pen_b; dp[3] = 0xFF;
            }
        }
    }

    return true;
}

} // namespace vitreo::winapi
