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

/// @file stock_objects.cpp
/// @brief Stock Objects
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Stock Objects
// =============================================================================

Handle GdiManager::get_stock_object(i32 id) {
    ApiLock api_lock(*this);
    auto it = m_stock_objects.find(id);
    return (it != m_stock_objects.end()) ? it->second : Handle{0};
}

Handle GdiManager::select_object(Handle hdc, Handle obj, u32* error) {
    ApiLock api_lock(*this);
    auto set_error = [&](u32 code) {
        if (error) *error = code;
    };
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) {
        set_error(6);  // ERROR_INVALID_HANDLE (s3gdi_bitmap_probe "select-null-dc")
        return Handle{0};
    }
    DcState& dc = it->second;
    // An object deleted while selected is freed once no DC holds it.
    auto release = [&](Handle old) {
        if (old != 0 && m_pending_delete.contains(old) && !object_is_selected(old)) {
            m_pending_delete.erase(old);
            free_object(old);
        }
    };

    switch (object_type(obj)) {
    case OBJ_BITMAP_TYPE: {
        // Bitmaps go into memory DCs only, and a non-stock bitmap into one DC
        // at a time (s3gdi_bitmap_probe "select-into-second-dc",
        // "select-bitmap-into-screen-dc": NULL, last error untouched).
        if (!dc.memory_dc) return Handle{0};
        const Handle prev = dc.selected_bitmap_handle;
        if (prev == obj) return prev;
        const bool stock = m_stock_handles.contains(obj);
        if (!stock) {
            const auto owner = m_bitmap_owner_dc.find(obj);
            if (owner != m_bitmap_owner_dc.end() && owner->second != hdc) return Handle{0};
        }
        auto surface = bitmap_surface(obj);
        if (!surface) return Handle{0};
        if (const auto owner = m_bitmap_owner_dc.find(prev);
            owner != m_bitmap_owner_dc.end() && owner->second == hdc) {
            m_bitmap_owner_dc.erase(owner);
        }
        dc.selected_bitmap_handle = obj;
        dc.bitmap = std::move(surface);
        if (!stock) m_bitmap_owner_dc[obj] = hdc;
        if (std::getenv("VITREO_GDI_DIAG")) {
            VITREO_TRACE("[gdi-diag] SelectObject bitmap dc=0x%x obj=0x%x prev=0x%x size=%dx%d\n",
                         hdc, obj, prev, dc.bitmap->width, dc.bitmap->height);
        }
        release(prev);
        return prev;
    }
    case OBJ_PEN_TYPE:
    case OBJ_EXTPEN_TYPE: {
        const Handle prev = dc.selected_pen;
        dc.selected_pen = obj;
        release(prev);
        return prev;
    }
    case OBJ_BRUSH_TYPE: {
        const Handle prev = dc.selected_brush;
        dc.selected_brush = obj;
        release(prev);
        return prev;
    }
    case OBJ_FONT_TYPE: {
        const Handle prev = dc.selected_font;
        dc.selected_font = obj;
        release(prev);
        return prev;
    }
    case OBJ_REGION_TYPE:
        // SelectObject(region) is SelectClipRgn and answers the complexity.
        return static_cast<Handle>(select_clip_rgn(hdc, obj));
    case OBJ_PAL_TYPE:
        // Palettes go through SelectPalette ("select-palette": NULL,
        // ERROR_INVALID_FUNCTION).
        set_error(1);
        return Handle{0};
    default:
        return Handle{0};
    }
}

} // namespace vitreo::winapi
