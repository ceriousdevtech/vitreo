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

/// @file device_context_operations.cpp
/// @brief Device Context Operations
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Device Context Operations
// =============================================================================

Handle GdiManager::get_dc(Handle hwnd) {
    ApiLock api_lock(*this);
    Handle h = allocate_gdi_handle(kHandleTypeDC);
    auto& dc = m_dc_states[h];
    dc = DcState{};
    dc.associated_hwnd = hwnd;
    dc.screen_dc = hwnd == 0;
    dc.selected_brush = m_stock_objects[WHITE_BRUSH_ID];
    dc.selected_pen = m_stock_objects[BLACK_PEN_ID];
    dc.selected_font = m_stock_objects[SYSTEM_FONT_ID];
    dc.selected_palette = m_stock_objects[DEFAULT_PALETTE_ID];

    // For window DCs, create or share the window framebuffer
    if (hwnd != 0) {
        // Determine the window's actual client area dimensions
        i32 fb_w = 320, fb_h = 320; // fallback default
        bool sized = false;
        if (m_hwnd_mgr) {
            if (const auto* win = m_hwnd_mgr->find(
                    static_cast<windowing::HWND>(hwnd))) {
                i32 cw = win->client_rect.right - win->client_rect.left;
                i32 ch = win->client_rect.bottom - win->client_rect.top;
                if (cw > 0 && ch > 0) {
                    fb_w = cw;
                    fb_h = ch;
                    sized = true;
                }
            }
        }
        // Peer-process HWND (GetDC on a window owned by another host
        // process): use the capsule hwnd registry so the framebuffer matches
        // the real client.
        if (!sized) {
            if (auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
                if (remote->client_w > 0 && remote->client_h > 0) {
                    fb_w = remote->client_w;
                    fb_h = remote->client_h;
                }
            }
        }

        auto& fb = m_window_framebuffers[hwnd];
        if (!fb) {
            fb = std::make_shared<SoftBitmap>();
            fb->child_window = is_child_window(hwnd);
            fb->resize_window(fb_w, fb_h);
            // resize_window() marks dirty for the init fill, but GetDC itself is
            // not a paint. Callers measure statics with GetDC +
            // GetTextExtent + ReleaseDC; flushing that blank buffer creates
            // a guest overlay that hides the native text field.
            fb->dirty = false;
            m_window_client_size[hwnd] = {fb_w, fb_h};
        } else if (fb->width != fb_w || fb->height != fb_h) {
            // Framebuffer size differs from current client area — resize it
            // so flush_window sends the correct dimensions.
            ensure_framebuffer_size(hwnd, fb_w, fb_h);
            fb->dirty = false;
        } else {
            // Same size — still track the client area for flush_window cropping
            m_window_client_size[hwnd] = {fb_w, fb_h};
        }
        dc.bitmap = fb;
    }
    if (std::getenv("VITREO_GDI_DIAG")) {
        std::fprintf(stderr,
                     "[gdi-diag] GetDC hwnd=0x%x -> dc=0x%x "
                     "surface=%p size=%dx%d\n",
                     hwnd, h, static_cast<void*>(dc.bitmap.get()),
                     dc.bitmap ? dc.bitmap->width : 0,
                     dc.bitmap ? dc.bitmap->height : 0);
    }
    return h;
}

i32 GdiManager::pixel_format(Handle hdc) const {
    ApiLock api_lock(*this);
    const Handle hwnd = window_from_dc(hdc);
    const auto it = m_pixel_formats.find(hwnd != 0 ? hwnd : hdc);
    return it == m_pixel_formats.end() ? 0 : it->second;
}

bool GdiManager::pixel_format_ever_set() const {
    ApiLock api_lock(*this);
    return m_pixel_format_ever_set;
}

u32 GdiManager::set_pixel_format(Handle hdc, i32 format, i32 max_format) {
    ApiLock api_lock(*this);
    constexpr u32 ERROR_INVALID_HANDLE_CODE = 6;
    constexpr u32 ERROR_INVALID_PIXEL_FORMAT_CODE = 2000;
    constexpr u32 ERROR_INVALID_PARAMETER_CODE = 87;
    if (hdc == 0) return ERROR_INVALID_HANDLE_CODE;
    // wgl_contract_probe64.expected setpf-index0 / setpf-index-count+1 /
    // setpf-index-neg: an index outside 1..count is ERROR_INVALID_PARAMETER.
    if (format < 1 || format > max_format) return ERROR_INVALID_PARAMETER_CODE;
    const Handle hwnd = window_from_dc(hdc);
    const Handle key = hwnd != 0 ? hwnd : hdc;
    const auto it = m_pixel_formats.find(key);
    if (it != m_pixel_formats.end()) {
        return it->second == format ? 0 : ERROR_INVALID_PIXEL_FORMAT_CODE;
    }
    m_pixel_formats.emplace(key, format);
    m_pixel_format_ever_set = true;
    return 0;
}

void GdiManager::window_destroyed(Handle hwnd) {
    ApiLock api_lock(*this);
    if (hwnd != 0) m_pixel_formats.erase(hwnd);
}

usize GdiManager::live_object_count() const {
    ApiLock api_lock(*this);
    usize count = m_dc_states.size() + m_bitmap_objects.size() +
                  m_pen_objects.size() + m_brush_colors.size() +
                  m_region_objects.size() + m_palette_objects.size() +
                  m_metafile_objects.size();
    // Stock objects live in the same tables but are not the process's own.
    for (const Handle h : m_stock_handles) {
        if (m_bitmap_objects.count(h) || m_pen_objects.count(h) ||
            m_brush_colors.count(h) || m_region_objects.count(h) ||
            m_palette_objects.count(h)) {
            if (count > 0) --count;
        }
    }
    return count;
}

Handle GdiManager::window_from_dc(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0;
    // Windows associates the display DC returned by GetDC(NULL) with the
    // desktop window. Keep the screen marker separate from associated_hwnd so
    // screen DCs remain non-window drawing surfaces internally.
    if (it->second.screen_dc) {
        return static_cast<Handle>(
            m_hwnd_mgr ? m_hwnd_mgr->desktop() : windowing::HWND_DESKTOP);
    }
    return static_cast<Handle>(it->second.associated_hwnd);
}

i32 GdiManager::release_dc(Handle hwnd, Handle hdc) {
    ApiLock api_lock(*this);
    // If this was a window DC, flush the framebuffer to screen
    auto it = m_dc_states.find(hdc);
    if (it != m_dc_states.end()) {
        u64 dc_hwnd = it->second.associated_hwnd;
        if (dc_hwnd == 0 && hwnd != 0) dc_hwnd = hwnd;
        if (dc_hwnd != 0) {
            flush_window(dc_hwnd);
            // Presentation suspends all owner locks. Independent DC creation
            // may rehash the table, so reacquire the iterator afterwards.
            it = m_dc_states.find(hdc);
            if (it == m_dc_states.end()) return 0;
        }
        if (std::getenv("VITREO_GDI_DIAG")) {
            std::fprintf(stderr,
                         "[gdi-diag] ReleaseDC arg-hwnd=0x%x dc=0x%x "
                         "dc-hwnd=0x%llx surface=%p\n",
                         hwnd, hdc,
                         static_cast<unsigned long long>(
                             it->second.associated_hwnd),
                         static_cast<void*>(it->second.bitmap.get()));
        }
    }
    // Only window and screen DCs are released; a DC released twice, or a
    // memory DC, is 0 (s3gdi_userpaint_probe "ReleaseDC=1 again=0",
    // "ReleaseDC(bad dc)=0").
    if (it == m_dc_states.end() || it->second.memory_dc) return 0;
    m_dc_states.erase(it);
    (void)m_handles.close(hdc);
    return 1;
}

void GdiManager::set_dc_clip(Handle hdc, const ClipRect& clip) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it != m_dc_states.end()) it->second.clip_rect = clip;
}

Handle GdiManager::create_compatible_dc(Handle hdc) {
    ApiLock api_lock(*this);
    // Learn (CreateCompatibleDC): NULL means the screen; any other handle
    // must be a live DC.
    if (hdc != 0 && !m_dc_states.contains(hdc)) return 0;
    Handle h = get_dc(0); // Create a new DC with no associated window

    // Windows memory DCs start with the stock 1x1 monochrome bitmap selected
    // (s3gdi_bitmap_probe "default-bitmap": GetStockObject(21)), which is
    // also what CreateCompatibleBitmap on a fresh memory DC is compatible with.
    const Handle default_bmp = m_stock_objects[DEFAULT_BITMAP_ID];
    auto it = m_dc_states.find(h);
    if (it != m_dc_states.end()) {
        it->second.screen_dc = false;
        it->second.memory_dc = true;
        it->second.selected_bitmap_handle = default_bmp;
        it->second.bitmap = m_bitmap_objects[default_bmp];
    }

    if (std::getenv("VITREO_GDI_DIAG")) {
        VITREO_TRACE("[gdi-diag] CreateCompatibleDC -> dc=0x%x default-bmp=0x%x\n", h,
                     default_bmp);
    }
    return h;
}

bool GdiManager::delete_dc(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    if (std::getenv("VITREO_GDI_DIAG")) {
        VITREO_TRACE("[gdi-diag] DeleteDC dc=0x%x hwnd=0x%llx surface=%p\n", hdc,
                     static_cast<unsigned long long>(it->second.associated_hwnd),
                     static_cast<void*>(it->second.bitmap.get()));
    }
    const DcState state = std::move(it->second);
    m_dc_states.erase(it);
    // A DC without a window (a memory DC) carries its own pixel format; a
    // reused handle value must not inherit it (final review, Finding 7).
    if (state.associated_hwnd == 0) m_pixel_formats.erase(hdc);
    // The bitmap is free to be selected elsewhere again, and objects deleted
    // while selected here go now (s3gdi_bitmap_probe "deferred").
    if (const auto owner = m_bitmap_owner_dc.find(state.selected_bitmap_handle);
        owner != m_bitmap_owner_dc.end() && owner->second == hdc) {
        m_bitmap_owner_dc.erase(owner);
    }
    for (const Handle obj : {state.selected_bitmap_handle, state.selected_brush,
                             state.selected_pen, state.selected_font}) {
        if (obj != 0 && m_pending_delete.contains(obj) && !object_is_selected(obj)) {
            m_pending_delete.erase(obj);
            free_object(obj);
        }
    }
    (void)m_handles.close(hdc);
    return true;
}

} // namespace vitreo::winapi
