// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_create.cpp
/// @brief IDirect3DDevice9 device-owned surfaces and the create/transfer
///        entry points.
///
/// Vtable slots 23-36 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_device_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <array>
#include <memory>
#include <utility>

namespace vitreo::winapi {

// =============================================================================
// Surfaces owned by the device
// =============================================================================

u32 D3D9Manager::create_surface_object(u32 device, u32 width, u32 height,
                                       u32 format, u32 usage, u32 pool,
                                       int kind, u32 container, u32 level,
                                       u32 face, u32 multi_sample_type) {
    const u32 object = alloc_com_object(D3D9InterfaceId::Surface, device);
    if (object == 0) return 0;

    ComObject* entry = find(object, D3D9InterfaceId::Surface);
    entry->surface = std::make_unique<SurfaceState>();
    SurfaceState& surface = *entry->surface;
    surface.kind = static_cast<SurfaceState::Kind>(kind);
    surface.width = width;
    surface.height = height;
    surface.format = format;
    surface.usage = usage;
    surface.pool = pool;
    surface.container = container;
    surface.level = level;
    surface.face = face;
    surface.multi_sample_type = multi_sample_type;
    const unsigned sample_count =
        d3d9_raster_sample_count(multi_sample_type, 0, m_backend);
    const bool msaa = sample_count > 1;

    // A depth stencil does not need guest-visible pixels. MSAA surfaces are
    // not lockable. Everything else does: D3D9 requires the pointer to stay
    // valid across Lock/Unlock pairs (including lockable CreateRenderTarget).
    const bool lockable = (usage & D3DUSAGE_DEPTHSTENCIL) == 0 && !msaa;
    if (lockable) {
        u32 pitch = 0;
        usize bytes = 0;
        if (!d3d9_surface_storage(format, width, height, pitch, bytes)) {
            VITREO_TRACE("[d3d9] surface: D3DFORMAT %u has no known pixel "
                         "size\n", format);
            destroy_object(object);
            return 0;
        }
        surface.pitch = pitch;
        surface.pixel_bytes = bytes;
        surface.pixels = map_lock_region(surface.pixel_bytes);
        if (surface.pixels == 0) {
            VITREO_TRACE("[d3d9] surface: could not map %zu bytes of guest "
                         "pixel memory\n", surface.pixel_bytes);
            destroy_object(object);
            return 0;
        }
        // Standalone CreateRenderTarget surfaces need a host colour attachment
        // so Clear/Draw/ColorFill/GetRenderTargetData can resolve a backend
        // (RT-texture levels already share the parent texture backend).
        if (surface.kind == SurfaceState::Kind::RenderTarget &&
            m_backend != nullptr && m_backend->available()) {
            const graphics::PixelFormat backend_format =
                d3d9_backend_format(format);
            if (backend_format != graphics::PixelFormat::Unknown) {
                graphics::BackendTexture2DDesc desc{};
                desc.width = width;
                desc.height = height;
                desc.mip_levels = 1;
                desc.array_size = 1;
                desc.pixel_format = backend_format;
                desc.shader_resource = true;
                desc.render_target = true;
                desc.cpu_writable = false;
                desc.sample_count = sample_count;
                auto backend = m_backend->create_texture_2d(desc, nullptr, 0);
                if (backend && backend->valid()) {
                    const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
                    (void)backend->clear_render_target(black);
                    surface.backend = std::move(backend);
                } else {
                    VITREO_TRACE("[d3d9] surface: shared backend rejected "
                                 "%ux%u CreateRenderTarget fmt=%u "
                                 "(guest pixels only)\n",
                                 width, height, format);
                }
            }
        }
    } else if ((usage & D3DUSAGE_DEPTHSTENCIL) != 0 && m_backend != nullptr &&
               m_backend->available()) {
        // Depth-stencil surfaces need a real Metal attachment so texture RTs
        // can depth-test.
        const graphics::PixelFormat backend_format =
            d3d9_backend_format(format);
        if (backend_format == graphics::PixelFormat::Unknown) {
            VITREO_TRACE("[d3d9] surface: depth format %u has no shared-core "
                         "mapping\n", format);
            destroy_object(object);
            return 0;
        }
        graphics::BackendTexture2DDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.mip_levels = 1;
        desc.array_size = 1;
        desc.pixel_format = backend_format;
        desc.depth_stencil = true;
        desc.sample_count = sample_count;
        auto backend = m_backend->create_texture_2d(desc, nullptr, 0);
        if (!backend || !backend->valid()) {
            VITREO_TRACE("[d3d9] surface: shared backend rejected %ux%u "
                         "depth format %u\n", width, height, format);
            destroy_object(object);
            return 0;
        }
        surface.backend = std::move(backend);
    } else if (msaa && (usage & D3DUSAGE_RENDERTARGET) != 0 &&
               surface.kind == SurfaceState::Kind::RenderTarget &&
               m_backend != nullptr && m_backend->available()) {
        const graphics::PixelFormat backend_format =
            d3d9_backend_format(format);
        if (backend_format == graphics::PixelFormat::Unknown) {
            destroy_object(object);
            return 0;
        }
        graphics::BackendTexture2DDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.mip_levels = 1;
        desc.array_size = 1;
        desc.pixel_format = backend_format;
        desc.render_target = true;
        desc.sample_count = sample_count;
        auto backend = m_backend->create_texture_2d(desc, nullptr, 0);
        if (!backend || !backend->valid()) {
            VITREO_TRACE("[d3d9] surface: shared backend rejected %ux%u "
                         "MSAA CreateRenderTarget fmt=%u samples=%u\n",
                         width, height, format, sample_count);
            destroy_object(object);
            return 0;
        }
        const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
        (void)backend->clear_render_target(black);
        surface.backend = std::move(backend);
    }
    return object;
}

/// IDirect3DDevice9 vtable slots: the Create* resource entry points and
/// the surface transfer calls (UpdateSurface .. ColorFill).
void D3D9Manager::dispatch_device_create(Marshaler& m, usize slot,
                                         u32 self) {
    switch (slot) {
    case 23: // CreateTexture
        create_texture(m, self);
        return;
    case 24: // CreateVolumeTexture
        create_volume_texture(m, self);
        return;
    case 25: // CreateCubeTexture
        create_cube_texture(m, self);
        return;
    case 26: // CreateVertexBuffer
        create_buffer(m, self, false);
        return;
    case 27: // CreateIndexBuffer
        create_buffer(m, self, true);
        return;

    case 28: { // CreateRenderTarget
        const u32 width = static_cast<u32>(m.arg(1));
        const u32 height = static_cast<u32>(m.arg(2));
        const u32 format = static_cast<u32>(m.arg(3));
        const u32 multisample = static_cast<u32>(m.arg(4));
        const u32 lockable = static_cast<u32>(m.arg(6));
        const GuestAddr out = m.arg_ptr(7);
        if (out != 0) (void)m.write_u32(out, 0);
        if (width == 0 || height == 0 || out == 0 ||
            (multisample != D3DMULTISAMPLE_NONE && lockable != 0) ||
            !d3d9_multisample_supported(m_backend, multisample)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = create_surface_object(
            self, width, height, format, D3DUSAGE_RENDERTARGET,
            D3DPOOL_DEFAULT,
            static_cast<int>(SurfaceState::Kind::RenderTarget), 0, 0, 0,
            multisample);
        if (surface == 0 || m.write_u32(out, surface).is_err()) {
            if (surface != 0) destroy_object(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] CreateRenderTarget -> 0x%08x %ux%u fmt=%u\n",
                     surface, width, height, format);
        m.set_return(D3D_OK);
        return;
    }

    case 29: { // CreateDepthStencilSurface
        const u32 width = static_cast<u32>(m.arg(1));
        const u32 height = static_cast<u32>(m.arg(2));
        const u32 format = static_cast<u32>(m.arg(3));
        const u32 multisample = static_cast<u32>(m.arg(4));
        const GuestAddr out = m.arg_ptr(7);
        if (out != 0) (void)m.write_u32(out, 0);
        if (width == 0 || height == 0 || out == 0 ||
            !d3d9_multisample_supported(m_backend, multisample)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = create_surface_object(
            self, width, height, format, D3DUSAGE_DEPTHSTENCIL,
            D3DPOOL_DEFAULT,
            static_cast<int>(SurfaceState::Kind::DepthStencil), 0, 0, 0,
            multisample);
        if (surface == 0 || m.write_u32(out, surface).is_err()) {
            if (surface != 0) destroy_object(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] CreateDepthStencilSurface -> 0x%08x %ux%u "
                     "fmt=%u\n",
                     surface, width, height, format);
        m.set_return(D3D_OK);
        return;
    }

    case 30: // UpdateSurface
        update_surface(m, self);
        return;
    case 31: // UpdateTexture
        update_texture(m, self);
        return;
    case 32: // GetRenderTargetData
        get_render_target_data(m, self);
        return;
    case 33: // GetFrontBufferData
        get_front_buffer_data(m, self, static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)));
        return;
    case 34: // StretchRect
        stretch_rect(m, self);
        return;
    case 35: // ColorFill
        color_fill(m, self);
        return;

    case 36: { // CreateOffscreenPlainSurface
        const u32 width = static_cast<u32>(m.arg(1));
        const u32 height = static_cast<u32>(m.arg(2));
        const u32 format = static_cast<u32>(m.arg(3));
        const u32 pool = static_cast<u32>(m.arg(4));
        const GuestAddr out = m.arg_ptr(5);
        if (out != 0) (void)m.write_u32(out, 0);
        if (width == 0 || height == 0 || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = create_surface_object(
            self, width, height, format, 0, pool,
            static_cast<int>(SurfaceState::Kind::Offscreen), 0, 0);
        if (surface == 0 || m.write_u32(out, surface).is_err()) {
            if (surface != 0) destroy_object(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
