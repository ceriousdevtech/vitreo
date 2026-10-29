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

/// @file construction.cpp
/// @brief Construction
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Construction
// =============================================================================

GdiManager::GdiManager(HandleTable& handles)
    : m_handles(handles) {
    init_stock_objects();
}

GdiManager::~GdiManager() {
    stop_presentation();
    std::lock_guard font_lock(gdi_font_registry_mutex());
    for (const Handle h : m_owned_fonts) gdi_font_registry().erase(h);
}

Handle GdiManager::set_metafile_bits(std::span<const u8> data) {
    ApiLock api_lock(*this);
    if (data.empty()) return 0;
    const Handle hmf = allocate_gdi_handle(kHandleTypeMetaFile);
    m_metafile_objects.emplace(
        hmf, std::vector<u8>(data.begin(), data.end()));
    return hmf;
}

bool GdiManager::has_metafile(Handle hmf) const {
    ApiLock api_lock(*this);
    return m_metafile_objects.contains(hmf);
}

bool GdiManager::delete_metafile(Handle hmf) {
    ApiLock api_lock(*this);
    if (m_metafile_objects.erase(hmf) == 0) return false;
    (void)m_handles.close(hmf);
    return true;
}

void GdiManager::init_stock_objects() {
    ApiLock api_lock(*this);
    for (i32 id : {WHITE_BRUSH_ID, LTGRAY_BRUSH_ID, GRAY_BRUSH_ID, DKGRAY_BRUSH_ID,
                   BLACK_BRUSH_ID, NULL_BRUSH_ID, WHITE_PEN_ID, BLACK_PEN_ID, NULL_PEN_ID,
                   OEM_FIXED_FONT_ID, ANSI_FIXED_FONT_ID, ANSI_VAR_FONT_ID, SYSTEM_FONT_ID,
                   DEVICE_DEFAULT_FONT_ID, DEFAULT_PALETTE_ID, SYSTEM_FIXED_FONT_ID,
                   DEFAULT_GUI_FONT_ID, DC_BRUSH_ID, DC_PEN_ID, DEFAULT_BITMAP_ID}) {
        Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
        m_stock_objects[id] = h;
        m_stock_handles.insert(h);
    }
    // LOGPEN of the stock pens: width 0, the cosmetic one-pixel pen
    // (s3gdi_bitmap_probe "stock 6/7/8/19").
    m_pen_objects[m_stock_objects[WHITE_PEN_ID]] = PenInfo{0, 0, 0x00FFFFFF};
    m_pen_objects[m_stock_objects[BLACK_PEN_ID]] = PenInfo{0, 0, 0x00000000};
    m_pen_objects[m_stock_objects[NULL_PEN_ID]]  = PenInfo{5, 1, 0x00000000}; // PS_NULL, width 1
    m_pen_objects[m_stock_objects[DC_PEN_ID]]    = PenInfo{0, 0, 0x00000000};

    // Stock brushes (s3gdi_bitmap_probe "stock 0-5, 18"). NULL_BRUSH is
    // BS_NULL (1) and has no fill colour; DC_BRUSH reports white whatever
    // SetDCBrushColor holds.
    const std::pair<i32, u32> solid[] = {{WHITE_BRUSH_ID, 0x00FFFFFF}, {LTGRAY_BRUSH_ID, 0x00C0C0C0},
                                         {GRAY_BRUSH_ID, 0x00808080}, {DKGRAY_BRUSH_ID, 0x00404040},
                                         {BLACK_BRUSH_ID, 0x00000000}};
    for (const auto& [id, color] : solid) {
        m_brush_colors[m_stock_objects[id]] = color;
        m_brush_info[m_stock_objects[id]] = BrushInfo{0, color, 0};
    }
    m_brush_info[m_stock_objects[NULL_BRUSH_ID]] = BrushInfo{1, 0, 0};
    m_brush_info[m_stock_objects[DC_BRUSH_ID]] = BrushInfo{0, 0x00FFFFFF, 0};

    for (const auto& row : gdi::kGdiStockFonts) {
        FontInfo f;
        f.height = row.height;
        f.width = row.width;
        f.escapement = row.escapement;
        f.orientation = row.orientation;
        f.lf_weight = row.weight;
        f.weight = static_cast<u16>(row.weight);
        f.italic = row.italic != 0;
        f.underline = row.underline;
        f.strike_out = row.strike_out;
        f.char_set = row.char_set;
        f.out_precision = row.out_precision;
        f.clip_precision = row.clip_precision;
        f.quality = row.quality;
        f.pitch_and_family = row.pitch_and_family;
        f.face_name = row.face;
        {
            std::lock_guard font_lock(gdi_font_registry_mutex());
            gdi_font_registry()[m_stock_objects[row.id]] = std::move(f);
        }
        m_owned_fonts.insert(m_stock_objects[row.id]);
    }

    // The 1x1 monochrome bitmap a memory DC starts with.
    auto default_bitmap = std::make_shared<SoftBitmap>();
    default_bitmap->width = 1;
    default_bitmap->height = 1;
    default_bitmap->bpp = 1;
    default_bitmap->pixels.assign(4, 0);
    m_bitmap_objects[m_stock_objects[DEFAULT_BITMAP_ID]] = std::move(default_bitmap);
}

} // namespace vitreo::winapi
