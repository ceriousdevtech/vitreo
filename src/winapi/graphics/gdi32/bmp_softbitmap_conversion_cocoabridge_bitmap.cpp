// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32.cpp
/// @brief GDI32 device context stubs (WIN32-063)

#include "bmp_decode.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"
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

/// @file bmp_softbitmap_conversion_cocoabridge_bitmap.cpp
/// @brief BMP → SoftBitmap conversion for CocoaBridge bitmap handles
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// BMP → SoftBitmap conversion for CocoaBridge bitmap handles
// =============================================================================

std::shared_ptr<SoftBitmap> GdiManager::cocoa_bitmap_to_soft(Handle handle) {
    ApiLock api_lock(*this);
    if (!m_cocoa_bridge) return nullptr;

    std::vector<u8> bmp_snapshot;
    const auto bridge = m_cocoa_bridge;
    {
        ApiUnlock unlocked(*this);
        const auto* data = bridge->get_bitmap_bmp_data(static_cast<u32>(handle));
        if (!data) return nullptr;
        bmp_snapshot = *data;
    }
    const auto* bmp_data = &bmp_snapshot;

    // One BMP decoder serves GDI, LoadImage and WIC alike.
    auto decoded = decode_bmp_file(
        std::span<const u8>(bmp_data->data(), bmp_data->size()));
    if (!decoded) return nullptr;

    auto soft = std::make_shared<SoftBitmap>();
    soft->width = decoded->width;
    soft->height = decoded->height;
    // ICONINFO consumers must distinguish an RGB bitmap plus AND mask from
    // a 32-bit alpha image. The image decoder's opaque presentation alpha is
    // not evidence that the source bitmap contains an alpha channel.
    //
    // These handles come from LoadBitmap / LoadImage, which build a bitmap
    // compatible with the display: a 1 bpp DIB is monochrome (and blits
    // through the destination's text/background colours) only when its
    // colour table is black and white. Treating a {red,white} table as
    // monochrome turned every red pixel into the background colour.
    u16 display_bpp = 32;
    for (const auto& [id, value] : gdi::kGdiDisplayCaps) {
        if (id == BITSPIXEL) display_bpp = static_cast<u16>(value);
    }
    soft->bpp = compatible_bitmap_bpp(*decoded, display_bpp);
    soft->pixels = std::move(decoded->rgba);
    return soft;
}

bool GdiManager::delete_object(Handle obj) {
    ApiLock api_lock(*this);
    // Deleting a stock object is a successful no-op (s3gdi_bitmap_probe
    // "del_stock=1"); so is a GetSysColorBrush brush, which is a stock brush
    // on Windows.
    if (m_stock_handles.contains(obj)) return true;
    if (m_system_brush_color && system_brush_color(obj)) return true;
    const u32 type = object_type(obj);
    if (type == 0 || type == OBJ_DC_TYPE || type == OBJ_MEMDC_TYPE) {
        // Not a GDI object, or a double delete: FALSE, last error untouched.
        VITREO_TRACE("[gdi] DeleteObject: unknown handle 0x%x -> FALSE\n", obj);
        return false;
    }
    if (m_pending_delete.contains(obj)) return false;
    if (object_is_selected(obj)) {
        // Windows answers TRUE and frees the object once it is deselected
        // (s3gdi_bitmap_probe "deferred").
        m_pending_delete.insert(obj);
        return true;
    }
    free_object(obj);
    return true;
}

void GdiManager::free_object(Handle obj) {
    ApiLock api_lock(*this);
    const auto unmap = m_unmap_section_view;
    GuestAddr section_view = 0;
    m_palette_objects.erase(obj);
    if (const auto dib = m_dib_sections.find(obj); dib != m_dib_sections.end()) {
        // A section-backed DIB unmaps its view; the section handle stays the
        // caller's (Learn: CreateDIBSection remarks). Otherwise the bits are
        // a block of the DIB scratch region.
        if (const auto view = m_dib_section_views.find(obj); view != m_dib_section_views.end()) {
            section_view = view->second;
            m_dib_section_views.erase(view);
        } else {
            dib_scratch_free(dib->second.guest_bits,
                             dib->second.stride * static_cast<u32>(dib->second.height));
        }
        m_dib_sections.erase(dib);
    }
    m_bitmap_objects.erase(obj);
    m_bitmap_owner_dc.erase(obj);
    m_pen_objects.erase(obj);
    m_extpen_objects.erase(obj);
    m_brush_colors.erase(obj);
    m_brush_info.erase(obj);
    m_brush_patterns.erase(obj);
    if (m_owned_fonts.erase(obj) != 0) {
        std::lock_guard font_lock(gdi_font_registry_mutex());
        gdi_font_registry().erase(obj);
    }
    m_text_faces.erase(obj);
    m_region_objects.erase(obj);
    m_metafile_objects.erase(obj);
    (void)m_handles.close(obj);
    if (section_view != 0 && unmap) {
        ApiUnlock unlocked(*this);
        unmap(section_view);
    }
}

} // namespace vitreo::winapi
