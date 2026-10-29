// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32.cpp
/// @brief GDI32 device context stubs (WIN32-063)

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

/// @file object_creation.cpp
/// @brief Object Creation
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Object Creation
// =============================================================================

Handle GdiManager::create_brush_indirect() {
    ApiLock api_lock(*this);
    return create_solid_brush(0);
}

Handle GdiManager::create_solid_brush(u32 color) {
    ApiLock api_lock(*this);
    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_brush_colors[h] = color;
    m_brush_info[h] = BrushInfo{0, color, 0};
    return h;
}

Handle GdiManager::create_hatch_brush(u32 hatch, u32 color) {
    ApiLock api_lock(*this);
    // Learn (CreateHatchBrush): HS_HORIZONTAL..HS_DIAGCROSS; anything else
    // fails with the last error untouched (s3gdi_bitmap_probe
    // "indirect-hatch99").
    if (hatch > 5) return 0;
    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_brush_info[h] = BrushInfo{2, color, hatch};
    return h;
}

Handle GdiManager::create_pattern_brush(Handle bitmap) {
    ApiLock api_lock(*this);
    auto surface = bitmap_surface(bitmap);
    if (!surface || surface->width <= 0 || surface->height <= 0) return 0;
    if (m_dib_sections.contains(bitmap)) sync_dib_from_guest(bitmap);
    Handle brush = allocate_gdi_handle(kHandleTypeGdiObject);
    m_brush_patterns[brush] = std::make_shared<SoftBitmap>(*surface);
    // LOGBRUSH of a pattern brush: BS_PATTERN, lbHatch = the bitmap
    // (s3gdi_bitmap_probe "pattern ... hatch_is_bmp=1").
    m_brush_info[brush] = BrushInfo{3, 0, bitmap};
    return brush;
}

bool GdiManager::lookup_brush_color(Handle hbrush, u32* color) const {
    ApiLock api_lock(*this);
    u32 c = 0;
    if (const auto info = m_brush_info.find(hbrush); info != m_brush_info.end()) {
        if (info->second.style != 0) return false;
        c = info->second.color;
    } else if (const auto it = m_brush_colors.find(hbrush); it != m_brush_colors.end()) {
        c = it->second;
    } else if (m_system_brush_color) {
        const auto sys = system_brush_color(hbrush);
        if (!sys) return false;
        c = *sys;
    } else {
        return false;
    }
    if (color) *color = c;
    return true;
}

const GdiManager::BrushInfo* GdiManager::lookup_brush(Handle hbrush) const {
    ApiLock api_lock(*this);
    const auto it = m_brush_info.find(hbrush);
    return it == m_brush_info.end() ? nullptr : &it->second;
}

Handle GdiManager::create_ext_pen(ExtPenInfo info, u32* error) {
    ApiLock api_lock(*this);
    constexpr u32 kInvalidParameter = 87;
    auto fail = [&] {
        if (error) *error = kInvalidParameter;
        return Handle{0};
    };
    const u32 type = info.style & 0x000F0000u;   // PS_TYPE_MASK
    const u32 style = info.style & 0x0000000Fu;  // PS_STYLE_MASK
    const u32 endcap = info.style & 0x00000F00u; // PS_ENDCAP_MASK
    const u32 join = info.style & 0x0000F000u;   // PS_JOIN_MASK
    const bool geometric = type == 0x00010000u;
    if ((type != 0 && !geometric) || style > 8 || endcap > 0x200u || join > 0x2000u ||
        (info.style & 0xFFF000F0u) != 0) {
        return fail();
    }
    // Learn (ExtCreatePen): a cosmetic pen is one pixel wide, solid-coloured,
    // without caps or joins, and never PS_INSIDEFRAME; PS_ALTERNATE is
    // cosmetic-only; a style array only with PS_USERSTYLE, at most 16.
    if (!geometric) {
        if (info.width != 1 || info.brush_style != 0 || endcap != 0 || join != 0 ||
            style == 6) {
            return fail();
        }
    } else if (style == 8) {
        return fail();
    }
    if (style == 7) {
        if (info.style_entries.empty() || info.style_entries.size() > 16) return fail();
    } else if (!info.style_entries.empty()) {
        return fail();
    }
    switch (info.brush_style) {
    case 0: case 1: break;                           // BS_SOLID, BS_NULL
    case 2: if (info.hatch > 5) return fail(); break; // BS_HATCHED
    case 3:                                          // BS_PATTERN: not a DIB section
        if (object_type(static_cast<Handle>(info.hatch)) != OBJ_BITMAP_TYPE ||
            m_dib_sections.contains(static_cast<Handle>(info.hatch))) {
            return fail();
        }
        break;
    default:
        return fail();
    }
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    // Drawing uses the plain pen path; a null-brush geometric pen draws
    // nothing, like PS_NULL.
    const i32 draw_style = info.brush_style == 1 ? 5 : static_cast<i32>(style);
    m_pen_objects[h] = PenInfo{draw_style, geometric ? static_cast<i32>(info.width) : 1,
                               info.color};
    m_extpen_objects[h] = std::move(info);
    return h;
}

u32 GdiManager::set_dc_brush_color(Handle hdc, u32 color) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0xFFFFFFFFu;  // CLR_INVALID
    const u32 prev = it->second.dc_brush_color;
    it->second.dc_brush_color = color;
    return prev;
}

u32 GdiManager::set_dc_pen_color(Handle hdc, u32 color) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0xFFFFFFFFu;
    const u32 prev = it->second.dc_pen_color;
    it->second.dc_pen_color = color;
    return prev;
}

i32 GdiManager::set_stretch_blt_mode(Handle hdc, i32 mode) {
    ApiLock api_lock(*this);
    // Windows stores any value and answers the previous one
    // (s3gdi_blit_probe "SetStretchBltMode(5)=3", then "(0)=5"); modes
    // other than BLACKONWHITE, WHITEONBLACK and HALFTONE sample like
    // COLORONCOLOR.
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0;
    const i32 prev = it->second.stretch_mode;
    it->second.stretch_mode = mode;
    return prev;
}

i32 GdiManager::get_stretch_blt_mode(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    return it == m_dc_states.end() ? 0 : it->second.stretch_mode;
}

Handle GdiManager::create_font(FontInfo info) {
    ApiLock api_lock(*this);
    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    VITREO_TRACE("[gdi] CreateFont handle=0x%x height=%d weight=%d italic=%d\n", h,
                 info.height, info.lf_weight, info.italic);
    {
        std::lock_guard font_lock(gdi_font_registry_mutex());
        gdi_font_registry()[h] = std::move(info);
    }
    m_owned_fonts.insert(h);
    return h;
}

Handle GdiManager::create_font_indirect(i32 height, u16 weight, bool italic,
                                        std::u16string face_name) {
    ApiLock api_lock(*this);
    FontInfo info;
    info.height = height;
    info.weight = weight;
    info.lf_weight = weight;
    info.italic = italic;
    info.face_name = std::move(face_name);
    return create_font(std::move(info));
}

Handle GdiManager::create_font_w(i32 height, u16 weight, bool italic,
                                 std::u16string face_name) {
    ApiLock api_lock(*this);
    return create_font_indirect(height, weight, italic, std::move(face_name));
}

const GdiManager::FontInfo* GdiManager::lookup_font(Handle h) {
    std::lock_guard font_lock(gdi_font_registry_mutex());
    auto it = gdi_font_registry().find(h);
    return (it != gdi_font_registry().end()) ? &it->second : nullptr;
}

float GdiManager::font_height_to_pt(i32 lfHeight) {
    // lfHeight < 0: absolute value is character height in pixels at 96 DPI
    // lfHeight > 0: cell height (larger)
    // lfHeight = 0: default (match 8pt dialog template → 11pt macOS)
    if (lfHeight == 0) return 11.0f;
    float px = static_cast<float>(lfHeight < 0 ? -lfHeight : lfHeight);
    // Convert Windows pixel height to visually equivalent macOS point size:
    //   Win32 pt = px × 72/96
    //   macOS pt = Win32 pt × 1.25  (the macOS system font is wider than Tahoma,
    //             so we use a slightly smaller multiplier than the exact 96/72)
    //   Combined: px × 1.0 (use pixel height directly as macOS pt)
    float pt = std::roundf(px);
    return (pt < 8.0f) ? 8.0f : pt;
}

Handle GdiManager::create_ic_w() {
    ApiLock api_lock(*this);
    // Allocate a tracked DC so that GetDeviceCaps and other operations work.
    Handle h = allocate_gdi_handle(kHandleTypeDC);
    auto& dc = m_dc_states[h];
    dc = DcState{};
    dc.associated_hwnd = 0; // information context — no window
    dc.screen_dc = true;    // the display's DC (GetObjectType OBJ_DC)
    dc.selected_brush = m_stock_objects[WHITE_BRUSH_ID];
    dc.selected_pen = m_stock_objects[BLACK_PEN_ID];
    dc.selected_font = m_stock_objects[SYSTEM_FONT_ID];
    dc.selected_palette = m_stock_objects[DEFAULT_PALETTE_ID];
    return h;
}

} // namespace vitreo::winapi
