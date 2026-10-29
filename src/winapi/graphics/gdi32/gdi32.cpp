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

namespace vitreo::winapi {

int& GdiManager::api_lock_depth() const noexcept {
    thread_local std::unordered_map<const GdiManager*, int> depths;
    return depths[this];
}

std::optional<u32> GdiManager::system_brush_color(Handle brush) const {
    ApiLock api_lock(*this);
    const auto resolver = m_system_brush_color;
    if (!resolver) return std::nullopt;
    ApiUnlock unlocked(*this);
    return resolver(brush);
}

// Global font registry — must precede select_object / delete_object usage
static std::unordered_map<Handle, GdiManager::FontInfo> s_font_registry;
std::mutex& gdi_font_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<Handle, GdiManager::FontInfo>& gdi_font_registry() {
    return s_font_registry;
}



// =============================================================================
// Device Capabilities
// =============================================================================

// get_device_caps: geometry_ops.cpp (device_caps).



// =============================================================================
// DC State
// =============================================================================

i32 GdiManager::set_bk_mode(Handle hdc, i32 mode) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0;
    i32 prev = it->second.bk_mode;
    it->second.bk_mode = mode;
    return prev;
}

u32 GdiManager::set_text_color(Handle hdc, u32 color) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0xFFFFFFFF; // CLR_INVALID
    u32 prev = it->second.text_color;
    it->second.text_color = color;
    return prev;
}

u32 GdiManager::set_bk_color(Handle hdc, u32 color) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0xFFFFFFFF; // CLR_INVALID
    u32 prev = it->second.bk_color;
    it->second.bk_color = color;
    return prev;
}




// =============================================================================
// Bitmap Creation with Pixel Buffer
// =============================================================================

Handle GdiManager::create_bitmap_from_rgba(i32 width, i32 height,
                                           std::span<const u8> rgba) {
    ApiLock api_lock(*this);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return 0;
    }
    const usize needed = static_cast<usize>(width) *
                         static_cast<usize>(height) * 4u;
    if (rgba.size() < needed) return 0;

    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    auto bmp = std::make_shared<SoftBitmap>();
    bmp->resize(width, height);
    std::memcpy(bmp->pixels.data(), rgba.data(), needed);
    bmp->dirty = true;
    m_bitmap_objects[h] = bmp;
    VITREO_TRACE("[gdi] create_bitmap_from_rgba(%dx%d) -> 0x%x\n", width,
                 height, static_cast<unsigned>(h));
    return h;
}

// =============================================================================
// SetBrushOrgEx — set brush origin
// =============================================================================

bool GdiManager::set_brush_org_ex(Handle hdc, i32 x, i32 y) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    it->second.brush_org_x = x;
    it->second.brush_org_y = y;
    return true;
}





// =============================================================================
// Ensure window framebuffer is at least the given dimensions
// =============================================================================

bool GdiManager::is_child_window(u64 hwnd) const {
    constexpr u32 WS_CHILD = 0x40000000u;
    if (m_hwnd_mgr == nullptr) return false;
    const auto* win = m_hwnd_mgr->find(static_cast<windowing::HWND>(hwnd));
    return win != nullptr && (win->style & WS_CHILD) != 0;
}

void GdiManager::ensure_framebuffer_size(u64 hwnd, i32 w, i32 h) {
    ApiLock api_lock(*this);
    if (hwnd == 0 || w <= 0 || h <= 0) return;

    // Record the actual client area dimensions for flush_window cropping
    m_window_client_size[hwnd] = {w, h};

    auto& fb = m_window_framebuffers[hwnd];
    if (!fb) {
        fb = std::make_shared<SoftBitmap>();
        fb->child_window = is_child_window(hwnd);
        fb->resize_window(w, h);
        return;
    }

    // WM_NCCALCSIZE's default preservation aligns old client pixels at the
    // upper-left. Growing storage must not erase that valid region: a paint
    // may cover only newly exposed pixels, or a resize may arrive mid-paint.
    // Window invalidation/WM_ERASEBKGND decides what must actually be redrawn.
    if (fb->width != w || fb->height != h) {
        const i32 old_width = fb->width;
        const i32 copy_width = std::min(old_width, w);
        const i32 copy_height = std::min(fb->height, h);
        auto old_pixels = std::move(fb->pixels);
        fb->resize_window(w, h);
        for (i32 y = 0; y < copy_height; ++y) {
            std::memcpy(fb->pixels.data() + static_cast<usize>(y) * w * 4u,
                        old_pixels.data() + static_cast<usize>(y) * old_width * 4u,
                        static_cast<usize>(copy_width) * 4u);
        }
    }

    // Also update any DCs that reference this window's framebuffer
    for (auto& [dc_handle, dc_state] : m_dc_states) {
        if (dc_state.associated_hwnd == hwnd) {
            dc_state.bitmap = fb;
        }
    }
}

// =============================================================================
// System Metrics
// =============================================================================

i32 GdiManager::get_system_metrics(i32 index) {
    ApiLock api_lock(*this);
    switch (index) {
        case SM_CXSCREEN: {
            if (m_virtual_screen_width > 0) return m_virtual_screen_width;
            i32 w = 1920, h = 1080;
            if (m_cocoa_bridge) {
                const auto bridge = m_cocoa_bridge;
                ApiUnlock unlocked(*this);
                bridge->get_screen_size(w, h);
            }
            return w;
        }
        case SM_CYSCREEN: {
            if (m_virtual_screen_height > 0) return m_virtual_screen_height;
            i32 w = 1920, h = 1080;
            if (m_cocoa_bridge) {
                const auto bridge = m_cocoa_bridge;
                ApiUnlock unlocked(*this);
                bridge->get_screen_size(w, h);
            }
            return h;
        }
        case SM_CYCAPTION: return 30; // match user32 GetSystemMetrics / AdjustWindowRect
        case SM_CXBORDER:  return 1;
        case SM_CYBORDER:  return 1;
        default:           return 0;
    }
}

} // namespace vitreo::winapi
