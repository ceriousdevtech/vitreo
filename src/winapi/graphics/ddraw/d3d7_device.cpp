// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d7_device.cpp
/// @brief IDirect3DDevice7 lifecycle, scene bracketing, render target and clear.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <vector>

namespace vitreo::winapi {

using namespace ddraw_internal;

// =============================================================================
// IDirect3DDevice7
// =============================================================================

DirectDrawManager::DeviceObject* DirectDrawManager::find_device(
    GuestAddr device) {
    auto it = m_devices.find(device);
    return it == m_devices.end() ? nullptr : &it->second;
}

namespace {

/// Pack a D3DCOLOR (0xAARRGGBB) into one pixel of a surface with @p bpp bits.
/// Returns false for formats the DirectDraw surface layer does not describe
/// with fixed channel masks — an 8-bit surface is palettized, so a literal
/// colour has no defined index.
bool pack_clear_color(u32 color, u32 bpp, u32& out) {
    switch (bpp) {
        case 32:
            // 32-bit DirectDraw surfaces here use the same 0x00FF0000 /
            // 0x0000FF00 / 0x000000FF masks GetDisplayMode reports, which is
            // D3DCOLOR's own layout.
            out = color;
            return true;
        case 16: {
            // RGB565, matching the 16-bit masks GetDisplayMode reports.
            const u32 r = ((color >> 16) & 0xFF) >> 3;
            const u32 g = ((color >> 8) & 0xFF) >> 2;
            const u32 b = (color & 0xFF) >> 3;
            out = (r << 11) | (g << 5) | b;
            return true;
        }
        default:
            return false;
    }
}

} // namespace

u32 DirectDrawManager::device_query_interface(GuestAddr device, GuestAddr iid,
                                              GuestAddr output) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_device(device)) return ddraw::DDERR_INVALIDOBJECT;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECT3DDEVICE7)) {
        // Earlier device interfaces (IDirect3DDevice / 2 / 3) are separate
        // vtables this object does not implement, so they are refused rather
        // than aliased onto the version 7 layout.
        return ddraw::E_NOINTERFACE;
    }
    (void)m_memory->write_u32(output, static_cast<u32>(device));
    return device_add_ref(device);
}

u32 DirectDrawManager::device_add_ref(GuestAddr device) {
    DeviceObject* state = find_device(device);
    if (!state) return 0;
    return ++state->ref_count;
}

u32 DirectDrawManager::device_release(GuestAddr device) {
    DeviceObject* state = find_device(device);
    if (!state) return 0;
    if (state->ref_count > 1) return --state->ref_count;

    // The device holds a reference on its render target, its bound textures
    // and the DirectDraw object it was created from; all three are dropped
    // together with the last device reference.
    const GuestAddr owner = state->owner;
    const GuestAddr target = state->state.render_target;
    std::array<GuestAddr, d3d7::MAX_TEXTURE_STAGES> textures =
        state->state.textures;
    m_devices.erase(device);
    if (target != 0) (void)surface_release(target);
    for (const GuestAddr texture : textures) {
        if (texture != 0) (void)surface_release(texture);
    }
    if (owner != 0) (void)release(owner);
    VITREO_TRACE("[d3d7] device 0x%llx released\n",
                 static_cast<unsigned long long>(device));
    return 0;
}

u32 DirectDrawManager::device_get_caps(GuestAddr device, GuestAddr out_desc) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_desc == 0) return ddraw::E_INVALIDARG;
    if (!write_device_desc(out_desc, state->kind)) return ddraw::E_INVALIDARG;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_enum_texture_formats(GuestAddr device,
                                                   GuestAddr callback,
                                                   GuestAddr context) {
    if (!find_device(device)) return ddraw::DDERR_INVALIDOBJECT;
    if (callback == 0) return ddraw::E_INVALIDARG;
    (void)context;
    // A D3D7 texture is a DirectDrawSurface the device samples from. Nothing
    // in this implementation samples a surface yet, so there is no format it
    // can honestly offer: enumerating one would promise a texture upload path
    // that does not exist. The empty walk is the truthful answer until the
    // draw path lands.
    VITREO_TRACE("[d3d7] EnumTextureFormats -> none (no texture sampling "
                 "path yet)\n");
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_begin_scene(GuestAddr device) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (state->state.in_scene) return ddraw::D3DERR_SCENE_IN_SCENE;
    state->state.in_scene = true;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_end_scene(GuestAddr device) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (!state->state.in_scene) return ddraw::D3DERR_SCENE_NOT_IN_SCENE;
    state->state.in_scene = false;
    // EndScene does not present. A D3D7 caller shows the frame by flipping or
    // blitting the render target through DirectDraw, which is the path that
    // already reaches the host view.
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_direct3d(GuestAddr device,
                                           GuestAddr out_d3d) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_d3d == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_d3d, 0);
    if (!ensure_storage()) return ddraw::E_INVALIDARG;
    // IDirect3D7 shares the DirectDraw object's lifetime but needs its own
    // pointer, exactly as QueryInterface hands one back.
    const GuestAddr d3d7 = d3d7_interface_for(state->owner);
    if (d3d7 == 0) return ddraw::E_INVALIDARG;
    (void)write_pointer(out_d3d, d3d7);
    (void)add_ref(state->owner);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_set_render_target(GuestAddr device,
                                                GuestAddr surface, u32 flags) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    // dwFlags is documented as reserved and must be zero.
    if (flags != 0) return ddraw::E_INVALIDARG;
    SurfaceObject* target = find_surface(surface);
    if (target == nullptr) return ddraw::DDERR_INVALIDPARAMS;

    const GuestAddr previous = state->state.render_target;
    (void)surface_add_ref(surface);
    state->state.render_target = surface;
    if (previous != 0 && previous != surface) (void)surface_release(previous);
    // Changing the render target resets the viewport to cover all of it,
    // which is what a caller that swaps targets without re-setting the
    // viewport depends on.
    state->state.viewport.x = 0;
    state->state.viewport.y = 0;
    state->state.viewport.width = target->surface ? target->surface->width() : 0;
    state->state.viewport.height =
        target->surface ? target->surface->height() : 0;
    state->state.viewport.min_z = 0.0f;
    state->state.viewport.max_z = 1.0f;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_get_render_target(GuestAddr device,
                                                GuestAddr out_surface) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_surface == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_surface,
                              static_cast<u32>(state->state.render_target));
    if (state->state.render_target != 0) {
        (void)surface_add_ref(state->state.render_target);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_clear(GuestAddr device, u32 count,
                                    GuestAddr rects, u32 flags, u32 color,
                                    u32 depth_bits, u32 stencil) {
    DeviceObject* state = find_device(device);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    (void)depth_bits;
    (void)stencil;
    if (flags == 0) return ddraw::E_INVALIDARG;
    // A device built on a plain render-target surface has no attached depth
    // or stencil buffer, so asking to clear one is a real error rather than
    // something to silently swallow.
    if ((flags & d3d7::clear_flag::ZBUFFER) != 0) {
        return ddraw::D3DERR_ZBUFFER_NOTPRESENT;
    }
    if ((flags & d3d7::clear_flag::STENCIL) != 0) {
        return ddraw::D3DERR_STENCILBUFFER_NOTPRESENT;
    }
    if ((flags & d3d7::clear_flag::TARGET) == 0) return ddraw::E_INVALIDARG;

    SurfaceObject* target = find_surface(state->state.render_target);
    if (target == nullptr || !target->surface) return ddraw::DDERR_INVALIDOBJECT;
    const u32 width = target->surface->width();
    const u32 height = target->surface->height();
    const u32 pitch = target->surface->pitch();
    const u32 bpp = target->surface->bits_per_pixel();
    u32 packed = 0;
    if (!pack_clear_color(color, bpp, packed)) {
        // An indexed render target has no defined mapping from a D3DCOLOR.
        VITREO_TRACE("[d3d7] Clear on %u-bpp target has no colour mapping\n",
                     bpp);
        return ddraw::DDERR_INVALIDPARAMS;
    }
    Byte* pixels = m_memory->host_ptr_mut(target->pixels, target->pixel_bytes);
    if (pixels == nullptr) return ddraw::DDERR_INVALIDOBJECT;

    // dwCount == 0 with a null rect list clears the whole viewport, which is
    // how a caller asks for a full-screen clear.
    struct ClearRect { u32 left, top, right, bottom; };
    std::vector<ClearRect> areas;
    const d3d7::Viewport& viewport = state->state.viewport;
    if (count == 0 || rects == 0) {
        areas.push_back({viewport.x, viewport.y,
                         viewport.x + viewport.width,
                         viewport.y + viewport.height});
    } else {
        // D3DRECT is four LONGs: x1, y1, x2, y2.
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr entry = rects + i * 16;
            const i32 x1 = static_cast<i32>(m_memory->read_u32(entry).value_or(0));
            const i32 y1 =
                static_cast<i32>(m_memory->read_u32(entry + 4).value_or(0));
            const i32 x2 =
                static_cast<i32>(m_memory->read_u32(entry + 8).value_or(0));
            const i32 y2 =
                static_cast<i32>(m_memory->read_u32(entry + 12).value_or(0));
            if (x2 <= x1 || y2 <= y1) continue;
            areas.push_back({static_cast<u32>(x1 < 0 ? 0 : x1),
                             static_cast<u32>(y1 < 0 ? 0 : y1),
                             static_cast<u32>(x2 < 0 ? 0 : x2),
                             static_cast<u32>(y2 < 0 ? 0 : y2)});
        }
    }

    for (const ClearRect& area : areas) {
        const u32 left = area.left > width ? width : area.left;
        const u32 top = area.top > height ? height : area.top;
        const u32 right = area.right > width ? width : area.right;
        const u32 bottom = area.bottom > height ? height : area.bottom;
        for (u32 y = top; y < bottom; ++y) {
            Byte* row = pixels + static_cast<usize>(y) * pitch;
            if (bpp == 32) {
                auto* out = reinterpret_cast<u32*>(row);
                for (u32 x = left; x < right; ++x) out[x] = packed;
            } else {
                auto* out = reinterpret_cast<u16*>(row);
                for (u32 x = left; x < right; ++x) {
                    out[x] = static_cast<u16>(packed);
                }
            }
        }
    }
    target->surface->mark_dirty();
    present_surface(state->state.render_target, *target);
    return ddraw::DD_OK;
}

} // namespace vitreo::winapi
