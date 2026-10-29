// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9.cpp
/// @brief D3D9 guest COM object model and the IDirect3D9 factory interface.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

/// @file construction.cpp
/// @brief Construction
///
/// Closed cut from d3d9.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Construction
// =============================================================================

D3D9Manager::D3D9Manager() = default;

D3D9Manager::~D3D9Manager() {
    d3d9_cs_drain(true);
    clear_exclusive_fullscreen();
}

void D3D9Manager::apply_exclusive_fullscreen(const DeviceState& state) {
    u32 window = state.present_field(PP_DEVICE_WINDOW);
    if (window == 0) window = state.focus_window;
    const bool want = state.present_field(PP_WINDOWED) == 0 && window != 0;
    if (!want) {
        clear_exclusive_fullscreen();
        return;
    }

    const u32 width = state.back_buffer_width();
    const u32 height = state.back_buffer_height();
    const u32 format = state.present_field(PP_BACK_BUFFER_FORMAT);
    u32 bits = d3d9_format_bytes_per_pixel(format) * 8u;
    if (bits == 0) bits = 32;

    if (m_fullscreen_hwnd != 0 && m_fullscreen_hwnd != window) {
        if (m_fullscreen_cb) m_fullscreen_cb(m_fullscreen_hwnd, false);
        if (m_display_mode_cb) {
            m_display_mode_cb(m_fullscreen_hwnd, 0, 0, 0);
        }
    }
    if (m_display_mode_cb) {
        m_display_mode_cb(window, width, height, bits);
    }
    if (m_fullscreen_cb) m_fullscreen_cb(window, true);
    m_fullscreen_hwnd = window;
    m_exclusive_mode_width = width;
    m_exclusive_mode_height = height;
    VITREO_TRACE("[d3d9] exclusive fullscreen hwnd=0x%x %ux%ux%u\n", window,
                 width, height, bits);
}

void D3D9Manager::clear_exclusive_fullscreen() {
    if (m_fullscreen_hwnd == 0) return;
    if (m_fullscreen_cb) m_fullscreen_cb(m_fullscreen_hwnd, false);
    if (m_display_mode_cb) {
        m_display_mode_cb(m_fullscreen_hwnd, 0, 0, 0);
    }
    VITREO_TRACE("[d3d9] exclusive fullscreen cleared hwnd=0x%x\n",
                 m_fullscreen_hwnd);
    m_fullscreen_hwnd = 0;
    m_exclusive_mode_width = 0;
    m_exclusive_mode_height = 0;
}

graphics::BackendTexture2D* D3D9Manager::default_white_texture() {
    if (m_default_white_texture && m_default_white_texture->valid()) {
        return m_default_white_texture.get();
    }
    if (m_backend == nullptr || !m_backend->available()) {
        return nullptr;
    }
    graphics::BackendTexture2DDesc desc{};
    desc.width = 1;
    desc.height = 1;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    desc.shader_resource = true;
    desc.cpu_writable = true;
    // Opaque white — Windows samples white when SetTexture is NULL.
    const u8 white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    m_default_white_texture =
        m_backend->create_texture_2d(desc, white, sizeof(white));
    if (!m_default_white_texture || !m_default_white_texture->valid()) {
        m_default_white_texture.reset();
        return nullptr;
    }
    return m_default_white_texture.get();
}

void D3D9Manager::set_interface_thunks(D3D9InterfaceId iface,
                                       InterfaceThunks thunks) {
    const auto index = static_cast<usize>(iface);
    if (index >= D3D9_INTERFACE_COUNT) return;
    m_thunks[index] = std::move(thunks);
}

void D3D9Manager::set_method_thunks(MethodThunks thunks) {
    set_interface_thunks(D3D9InterfaceId::Direct3D9,
                         InterfaceThunks(thunks.begin(), thunks.end()));
}

} // namespace vitreo::winapi
