// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_display.cpp
/// @brief IDirect3DDevice9 cursor, gamma ramp and display-side entry points.
///
/// Vtable slots 10-12 and 19-22 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <array>
#include <vector>

namespace vitreo::winapi {

/// IDirect3DDevice9 vtable slots: SetCursorProperties, SetCursorPosition,
/// ShowCursor, GetRasterStatus, SetDialogBoxMode, SetGammaRamp and
/// GetGammaRamp.
void D3D9Manager::dispatch_device_display(Marshaler& m, usize slot,
                                          DeviceState& state) {
    switch (slot) {
    case 10: { // SetCursorProperties
        // Learn: XHotSpot, YHotSpot, IDirect3DSurface9* (D3DFMT_A8R8G8B8,
        // power-of-two size strictly less than the display mode). Windows
        // copies the bitmap into an OS cursor; Vitreo retains the guest
        // surface and, when a host image hook is wired, reads pixels and
        // installs them via CocoaBridge::set_custom_cursor_rgba.
        const u32 hotspot_x = static_cast<u32>(m.arg(1));
        const u32 hotspot_y = static_cast<u32>(m.arg(2));
        const u32 surface = static_cast<u32>(m.arg(3));
        if (surface == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        ComObject* cursor = find(surface, D3D9InterfaceId::Surface);
        if (cursor == nullptr || !cursor->surface) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const SurfaceState& bitmap = *cursor->surface;
        if (bitmap.format != D3DFMT_A8R8G8B8 || bitmap.width == 0 ||
            bitmap.height == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto is_power_of_two = [](u32 value) noexcept {
            return value != 0 && (value & (value - 1u)) == 0;
        };
        if (!is_power_of_two(bitmap.width) ||
            !is_power_of_two(bitmap.height)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 4> mode{};
        if (!current_display_mode(mode) || bitmap.width >= mode[0] ||
            bitmap.height >= mode[1]) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m_cursor_image_cb) {
            // Host path exists: refuse success unless the A8R8G8B8 surface is
            // readable and the host accepts the converted RGBA blob.
            const u32 bpp = 4;
            if (bitmap.pixels == 0 || m_memory == nullptr ||
                bitmap.pitch < bitmap.width * bpp) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const usize rgba_bytes =
                static_cast<usize>(bitmap.width) * bitmap.height * bpp;
            std::vector<u8> rgba(rgba_bytes);
            std::vector<u8> row(bitmap.pitch);
            for (u32 y = 0; y < bitmap.height; ++y) {
                const GuestAddr row_addr =
                    bitmap.pixels +
                    static_cast<GuestAddr>(y) * bitmap.pitch;
                if (m_memory->read_bytes(row_addr, row.data(), bitmap.pitch)
                        .is_err()) {
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
                u8* dst = rgba.data() +
                          static_cast<usize>(y) * bitmap.width * bpp;
                for (u32 x = 0; x < bitmap.width; ++x) {
                    const u8* src = row.data() + static_cast<usize>(x) * bpp;
                    // Little-endian D3DFMT_A8R8G8B8 is B,G,R,A in memory.
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                    dst += bpp;
                }
            }
            if (!m_cursor_image_cb(bitmap.width, bitmap.height, hotspot_x,
                                   hotspot_y, rgba.data(), rgba.size())) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
        if (state.cursor_surface != surface) {
            (void)add_ref(surface);
            if (state.cursor_surface != 0) {
                (void)release(state.cursor_surface);
            }
            state.cursor_surface = surface;
        }
        state.cursor_hotspot_x = hotspot_x;
        state.cursor_hotspot_y = hotspot_y;
        m.set_return(D3D_OK);
        return;
    }
    case 11: { // SetCursorPosition (void)
        // Learn: X/Y are virtual-desktop coordinates; Flags only matter for
        // exclusive Present-deferred updates. Host warp uses the same space as
        // user32 SetCursorPos when a callback is wired.
        state.cursor_x = static_cast<i32>(static_cast<u32>(m.arg(1)));
        state.cursor_y = static_cast<i32>(static_cast<u32>(m.arg(2)));
        (void)m.arg(3); // D3DCURSOR_IMMEDIATE_UPDATE — deferred path N/A
        if (m_cursor_position_cb) {
            m_cursor_position_cb(state.cursor_x, state.cursor_y);
        }
        m.set_return(0);
        return;
    }
    case 12: { // ShowCursor
        // Learn: returns whether the cursor was previously visible (BOOL), not
        // the Win32 ShowCursor display count.
        const bool show = m.arg(1) != 0;
        const bool was_visible = state.cursor_visible;
        state.cursor_visible = show;
        if (m_cursor_visible_cb) {
            m_cursor_visible_cb(show);
        }
        m.set_return(was_visible ? 1u : 0u);
        return;
    }

    case 19: // GetRasterStatus
        // Learn: GetRasterStatus(iSwapChain, pRasterStatus). iSwapChain is the
        // CreateDevice-implicit ordinal (same as GetSwapChain), not additional.
        get_raster_status(m, static_cast<u32>(m.arg(1)), m.arg_ptr(2));
        return;
    case 20: // SetDialogBoxMode
        // GDI dialogs over a D3D9 back buffer are not supported; Windows
        // rejects the request when the device cannot honor it.
        m.set_return(D3DERR_INVALIDCALL);
        return;
    case 21: { // SetGammaRamp (void)
        // Learn: SetGammaRamp(iSwapChain, Flags, const D3DGAMMARAMP*).
        // The ramp maps framebuffer levels to display levels immediately.
        // Host cannot program the DAC; present applies the LUT instead.
        const u32 swap_chain = static_cast<u32>(m.arg(1));
        const u32 flags = static_cast<u32>(m.arg(2));
        const GuestAddr ramp = m.arg_ptr(3);
        if (swap_chain != 0 || ramp == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.memory()
                .read_bytes(ramp, reinterpret_cast<Byte*>(state.gamma_ramp.data()),
                            D3DGAMMARAMP_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.gamma_ramp_flags = flags;
        if (m_backend != nullptr) {
            m_backend->set_present_gamma_ramp(
                d3d9_gamma_ramp_is_identity(state.gamma_ramp)
                    ? nullptr
                    : state.gamma_ramp.data());
        }
        m.set_return(0);
        return;
    }
    case 22: { // GetGammaRamp (void)
        // Learn: GetGammaRamp(iSwapChain, D3DGAMMARAMP*).
        const u32 swap_chain = static_cast<u32>(m.arg(1));
        const GuestAddr ramp = m.arg_ptr(2);
        if (swap_chain != 0 || ramp == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.memory()
                .write_bytes(ramp,
                             reinterpret_cast<const Byte*>(state.gamma_ramp.data()),
                             D3DGAMMARAMP_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
