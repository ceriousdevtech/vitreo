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
#include <map>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace vitreo::winapi {

// VITREO_D3D9_XFER_LOG: the first 400 surface/texture transfers with both
// objects' shape and the result, to place a readback/copy chain.
void D3D9Manager::log_transfer(Marshaler& m, const char* name, u32 src_arg, u32 dst_arg) {
    static const bool enabled = std::getenv("VITREO_D3D9_XFER_LOG") != nullptr;
    static std::map<std::string, int> shown;
    if (!enabled || shown[name]++ >= 4000) return;
    const auto describe = [&](u32 id, char* out, usize size) {
        if (ComObject* e = find(id, D3D9InterfaceId::Surface); e != nullptr && e->surface) {
            const SurfaceState& s = *e->surface;
            std::snprintf(out, size, "surf %08x %ux%u fmt=%u pool=%u usage=%x ms=%u lvl=%u",
                          id, s.width, s.height, s.format, s.pool, s.usage,
                          s.multi_sample_type, s.level);
        } else if (ComObject* t = find(id, D3D9InterfaceId::Texture);
                   t != nullptr && t->texture) {
            std::snprintf(out, size, "tex %08x fmt=%u", id, t->texture->format);
        } else {
            std::snprintf(out, size, "obj %08x", id);
        }
    };
    char a[160];
    char b[160] = "-";
    describe(static_cast<u32>(m.arg(src_arg)), a, sizeof(a));
    if (dst_arg != 0) describe(static_cast<u32>(m.arg(dst_arg)), b, sizeof(b));
    char rects[96] = "";
    if (std::strcmp(name, "StretchRect") == 0) {
        // RECT* pSourceRect (arg 2), RECT* pDestRect (arg 4).
        const auto read_rect = [&](GuestAddr at, char* out, usize size) {
            if (at == 0) { std::snprintf(out, size, "full"); return; }
            i32 v[4] = {};
            for (int i = 0; i < 4; ++i) {
                v[i] = static_cast<i32>(m.memory().read_u32(at + 4u * i).value_or(0));
            }
            std::snprintf(out, size, "(%d,%d,%d,%d)", v[0], v[1], v[2], v[3]);
        };
        char sr[40];
        char dr[40];
        read_rect(m.arg_ptr(2), sr, sizeof(sr));
        read_rect(m.arg_ptr(4), dr, sizeof(dr));
        std::snprintf(rects, sizeof(rects), " src=%s dst=%s filter=%u", sr, dr,
                      static_cast<u32>(m.arg(5)));
    }
    std::fprintf(stderr, "[d3d9-xfer] %s %s -> %s%s = 0x%08llx\n", name, a, b, rects,
                 static_cast<unsigned long long>(m.return_value() & 0xffffffffu));
}

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
        if (out != 0) (void)m.write_ptr(out, 0);
        if (width == 0 || height == 0 || out == 0 ||
            (multisample != D3DMULTISAMPLE_NONE && lockable != 0) ||
            !d3d9_multisample_supported(m_backend, multisample) ||
            d3d9_check_format_usage(format, 1 /* D3DRTYPE_SURFACE */,
                                    D3DUSAGE_RENDERTARGET) ==
                D3DERR_NOTAVAILABLE) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = create_surface_object(
            self, width, height, format, D3DUSAGE_RENDERTARGET,
            D3DPOOL_DEFAULT,
            static_cast<int>(SurfaceState::Kind::RenderTarget), 0, 0, 0,
            multisample);
        if (surface == 0 || m.write_ptr(out, surface).is_err()) {
            if (surface != 0) destroy_object(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (ComObject* created = find(surface, D3D9InterfaceId::Surface);
            created != nullptr && created->surface) {
            created->surface->lockable_target = lockable != 0;
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
        if (out != 0) (void)m.write_ptr(out, 0);
        if (width == 0 || height == 0 || out == 0 ||
            !d3d9_multisample_supported(m_backend, multisample) ||
            d3d9_check_format_usage(format, 1 /* D3DRTYPE_SURFACE */,
                                    D3DUSAGE_DEPTHSTENCIL) ==
                D3DERR_NOTAVAILABLE) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = create_surface_object(
            self, width, height, format, D3DUSAGE_DEPTHSTENCIL,
            D3DPOOL_DEFAULT,
            static_cast<int>(SurfaceState::Kind::DepthStencil), 0, 0, 0,
            multisample);
        if (surface == 0 || m.write_ptr(out, surface).is_err()) {
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
        log_transfer(m, "UpdateSurface", 1, 3);
        return;
    case 31: // UpdateTexture
        update_texture(m, self);
        log_transfer(m, "UpdateTexture", 1, 2);
        return;
    case 32: // GetRenderTargetData
        get_render_target_data(m, self);
        log_transfer(m, "GetRenderTargetData", 1, 2);
        return;
    case 33: // GetFrontBufferData
        get_front_buffer_data(m, self, static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)));
        return;
    case 34: // StretchRect
        stretch_rect(m, self);
        log_transfer(m, "StretchRect", 1, 3);
        return;
    case 35: // ColorFill
        color_fill(m, self);
        log_transfer(m, "ColorFill", 1, 0);
        return;

    case 36: { // CreateOffscreenPlainSurface
        const u32 width = static_cast<u32>(m.arg(1));
        const u32 height = static_cast<u32>(m.arg(2));
        const u32 format = static_cast<u32>(m.arg(3));
        const u32 pool = static_cast<u32>(m.arg(4));
        const GuestAddr out = m.arg_ptr(5);
        if (out != 0) (void)m.write_ptr(out, 0);
        if (width == 0 || height == 0 || out == 0 ||
            (pool != D3DPOOL_SCRATCH &&
             d3d9_check_format_usage(format, 1 /* D3DRTYPE_SURFACE */, 0) ==
                 D3DERR_NOTAVAILABLE)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = create_surface_object(
            self, width, height, format, 0, pool,
            static_cast<int>(SurfaceState::Kind::Offscreen), 0, 0);
        if (surface == 0 || m.write_ptr(out, surface).is_err()) {
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
