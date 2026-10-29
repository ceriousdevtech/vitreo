// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_surface.cpp
/// @brief IDirectDrawSurface creation, lifetime, flipping chain, description and
///        presentation.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "graphics/contracts/presentation_state.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace vitreo::winapi {

using namespace ddraw_internal;

void DirectDrawManager::present_surface(GuestAddr surface, SurfaceObject& state) {
    // Only the primary is wired to the host view. Offscreen unlock/blt must not
    // replace the displayed frame with a save-buffer's pixels.
    if ((state.caps & 0x200u) == 0) return;
    if (!m_surface_presenter || !m_memory) return;
    perf::count(perf::Counter::dd_present_request);
    const Byte* source = m_memory->host_ptr(state.pixels, state.pixel_bytes);
    if (!source || !m_surface_presenter->present_indexed(
            m_presentation.target(), *state.surface,
            reinterpret_cast<const u8*>(source), state.pixel_bytes)) {
        perf::count(perf::Counter::dd_present_unavailable);
        VITREO_TRACE("[ddraw] presentation unavailable surface=0x%llx\n",
                     static_cast<unsigned long long>(surface));
    }
}

u32 DirectDrawManager::create_surface(GuestAddr object, GuestAddr descriptor,
                                       GuestAddr output, GuestAddr outer) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (outer != 0) return ddraw::CLASS_E_NOAGGREGATION;
    if (descriptor == 0 || m_memory->read_u32(descriptor).value_or(0) != 108) {
        return ddraw::E_INVALIDARG;
    }
    constexpr u32 DDSD_CAPS = 0x1;
    constexpr u32 DDSD_HEIGHT = 0x2;
    constexpr u32 DDSD_WIDTH = 0x4;
    constexpr u32 DDSD_PIXELFORMAT = 0x1000;
    constexpr u32 DDSCAPS_OFFSCREENPLAIN = 0x40;
    constexpr u32 DDSCAPS_PRIMARYSURFACE = 0x200;
    // SYSTEMMEMORY (0x800) is accepted but ignored: Vitreo always backs
    // surfaces with ordinary guest RAM.
    const u32 desc_flags = m_memory->read_u32(descriptor + 4).value_or(0);
    const u32 req_height = m_memory->read_u32(descriptor + 8).value_or(0);
    const u32 req_width = m_memory->read_u32(descriptor + 12).value_or(0);
    const u32 caps = m_memory->read_u32(descriptor + 104).value_or(0);
    if ((desc_flags & DDSD_CAPS) == 0) {
        return ddraw::E_INVALIDARG;
    }
    const bool primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    const bool offscreen = (caps & DDSCAPS_OFFSCREENPLAIN) != 0;
    if (primary == offscreen) {
        // Exactly one of primary / offscreen-plain. Other cap combinations
        // (overlays, 3D devices) stay unimplemented. A complex flipping
        // primary is handled below and still satisfies this test, because it
        // carries DDSCAPS_PRIMARYSURFACE and not DDSCAPS_OFFSCREENPLAIN.
        return ddraw::E_NOTIMPL;
    }
    if (offscreen &&
        (desc_flags & (DDSD_HEIGHT | DDSD_WIDTH)) != (DDSD_HEIGHT | DDSD_WIDTH)) {
        return ddraw::E_INVALIDARG;
    }

    // A primary surface describes whatever the display is showing now. Under
    // DDSCL_NORMAL the guest never calls SetDisplayMode — it is adopting the
    // current desktop mode — so fall back to the host display rather than
    // failing. A guest that calls SetCooperativeLevel(NULL, DDSCL_NORMAL)
    // and then CreateSurface(PRIMARY) treats a failure as
    // "DirectDraw unavailable".
    graphics::DisplayMode mode = m_presentation.display_mode();
    if (mode.width == 0 || mode.height == 0) {
        u32 host_width = 0;
        u32 host_height = 0;
        if (m_display_size_cb) m_display_size_cb(host_width, host_height);
        if (host_width == 0 || host_height == 0) return ddraw::E_INVALIDARG;
        mode.width = host_width;
        mode.height = host_height;
    }
    if (mode.bits_per_pixel == 0) mode.bits_per_pixel = 32;
    if (primary) {
        m_presentation.set_display_mode(mode);
    }

    u32 width = primary ? mode.width : req_width;
    u32 height = primary ? mode.height : req_height;
    u32 bpp = mode.bits_per_pixel;
    if (offscreen && (desc_flags & DDSD_PIXELFORMAT) != 0) {
        const u32 pf_bpp = m_memory->read_u32(descriptor + 84).value_or(0);
        if (pf_bpp != 0) bpp = pf_bpp;
    }
    if (width == 0 || height == 0 || bpp == 0 || (bpp % 8) != 0) {
        return ddraw::E_INVALIDARG;
    }

    const u32 pitch = (width * bpp + 7) / 8;
    const u32 pixel_bytes = pitch * height;
    const u32 mapped_bytes = (pixel_bytes + 0xFFFu) & ~0xFFFu;
    const GuestAddr pixels = m_memory->find_free_gap(0x62000000, mapped_bytes);
    if (pixels == 0 || m_memory->map_region(
            pixels, mapped_bytes, MemoryProtection::ReadWrite).is_err()) {
        return 0x8007000Eu;
    }
    if (Byte* host = m_memory->host_ptr_mut(pixels, mapped_bytes)) {
        std::memset(host, 0, mapped_bytes);
    }
    const GuestAddr surface = m_next_object;
    m_next_object += 0x20;
    if (write_pointer(surface, m_surface_vtable).is_err() ||
        m_memory->write_u32(output, static_cast<u32>(surface)).is_err()) {
        (void)m_memory->unmap_region(pixels, mapped_bytes);
        return 0x8007000Eu;
    }
    auto core_surface = std::make_shared<graphics::CpuSurface>(
        width, height, bpp, pitch);
    m_surfaces.emplace(surface, SurfaceObject{
        1, caps, pixels, pixel_bytes, mapped_bytes, false, 0,
        false, 0, 0, 0, std::move(core_surface)});
    // A fullscreen guest asks for DDSCAPS_COMPLEX|DDSCAPS_FLIP and a back
    // buffer count, then reaches the buffers through GetAttachedSurface. The
    // chain is a ring: each link points at the next and the last closes back
    // onto the primary, which is what makes repeated Flip cycle rather than
    // run off the end. Each buffer gets its own pixel allocation of exactly
    // the primary's geometry, since Flip exchanges their contents.
    constexpr u32 DDSD_BACKBUFFERCOUNT = 0x20;
    constexpr u32 DDSCAPS_BACKBUFFER = 0x4;
    constexpr u32 DDSCAPS_COMPLEX = 0x8;
    constexpr u32 DDSCAPS_FLIP = 0x10;
    if (primary && (caps & (DDSCAPS_COMPLEX | DDSCAPS_FLIP)) ==
                       (DDSCAPS_COMPLEX | DDSCAPS_FLIP)) {
        u32 back_count = (desc_flags & DDSD_BACKBUFFERCOUNT) != 0
                             ? m_memory->read_u32(descriptor + 20).value_or(0)
                             : 0;
        // DirectDraw treats a complex flipping primary with no stated count as
        // a single back buffer rather than as an error.
        if (back_count == 0) back_count = 1;
        constexpr u32 kMaxBackBuffers = 3;
        if (back_count > kMaxBackBuffers) back_count = kMaxBackBuffers;
        const u32 back_caps =
            (caps & ~static_cast<u32>(DDSCAPS_PRIMARYSURFACE)) |
            DDSCAPS_BACKBUFFER;
        GuestAddr previous = surface;
        bool chain_ok = true;
        for (u32 i = 0; i < back_count && chain_ok; ++i) {
            const GuestAddr back_pixels =
                m_memory->find_free_gap(0x62000000, mapped_bytes);
            if (back_pixels == 0 ||
                m_memory->map_region(back_pixels, mapped_bytes,
                                     MemoryProtection::ReadWrite).is_err()) {
                chain_ok = false;
                break;
            }
            if (Byte* back_host =
                    m_memory->host_ptr_mut(back_pixels, mapped_bytes)) {
                std::memset(back_host, 0, mapped_bytes);
            }
            const GuestAddr back = m_next_object;
            m_next_object += 0x20;
            if (m_memory->write_u32(back,
                    static_cast<u32>(m_surface_vtable)).is_err()) {
                (void)m_memory->unmap_region(back_pixels, mapped_bytes);
                chain_ok = false;
                break;
            }
            auto back_core = std::make_shared<graphics::CpuSurface>(
                width, height, bpp, pitch);
            m_surfaces.emplace(back, SurfaceObject{
                1, back_caps, back_pixels, pixel_bytes, mapped_bytes, false, 0,
                false, 0, 0, 0, std::move(back_core), 0, surface});
            m_surfaces.at(previous).back_buffer = back;
            previous = back;
            VITREO_TRACE("[ddraw] CreateSurface back buffer %u/%u "
                         "caps=0x%x pixels=0x%llx -> surface=0x%llx\n",
                         i + 1, back_count, back_caps,
                         static_cast<unsigned long long>(back_pixels),
                         static_cast<unsigned long long>(back));
        }
        // Close the ring so Flip cycles instead of stopping at the tail.
        if (chain_ok && previous != surface) {
            m_surfaces.at(previous).back_buffer = surface;
        }
    }
    VITREO_TRACE("[ddraw] CreateSurface %s %ux%ux%u pitch=%u caps=0x%x "
                 "pixels=0x%llx -> surface=0x%llx\n",
                 primary ? "primary" : "offscreen",
                 width, height, bpp, pitch, caps,
                 static_cast<unsigned long long>(pixels),
                 static_cast<unsigned long long>(surface));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_query_interface(GuestAddr surface, GuestAddr iid,
                                               GuestAddr output) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_surface(surface)) return ddraw::DDERR_INVALIDOBJECT;

    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested),
            sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    // IDirectDrawSurface2 and 3 only append methods to IDirectDrawSurface, so
    // the existing vtable answers all three for every slot Vitreo implements.
    // A caller reaching the appended methods (PageLock, SetSurfaceDesc) lands
    // past the table, so they are not claimed beyond this.
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTDRAWSURFACE) &&
        !guid_equal(requested, IID_IDIRECTDRAWSURFACE2) &&
        !guid_equal(requested, IID_IDIRECTDRAWSURFACE3)) {
        VITREO_TRACE("[ddraw] surface QueryInterface %08x-%04x-%04x -> "
                     "E_NOINTERFACE\n", requested.data1, requested.data2,
                     requested.data3);
        return ddraw::E_NOINTERFACE;
    }
    (void)m_memory->write_u32(output, static_cast<u32>(surface));
    (void)surface_add_ref(surface);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_add_ref(GuestAddr surface) {
    SurfaceObject* state = find_surface(surface);
    return state ? ++state->ref_count : 0;
}

u32 DirectDrawManager::surface_release(GuestAddr surface) {
    SurfaceObject* state = find_surface(surface);
    if (!state) return 0;
    const u32 result = --state->ref_count;
    if (result == 0) {
        // Drop the presenter's cached CpuSurface* before destroying the object.
        // Quitting a mission / exiting used to Release the primary while Metal
        // still held that pointer from the last Unlock present → host SIGSEGV.
        if (m_surface_presenter && state->surface) {
            m_surface_presenter->invalidate_surface(state->surface.get());
        }
        const bool primary = (state->caps & 0x200u) != 0;
        // Primary pixel maps stay until process teardown: an in-flight present
        // upload already copied into a Metal texture, but unmapping during
        // exit races teardown. Offscreen save buffers unmap normally.
        if (!primary && m_memory && state->pixels != 0 &&
            state->mapped_bytes != 0) {
            (void)m_memory->unmap_region(state->pixels, state->mapped_bytes);
        }
        m_surfaces.erase(surface);
    }
    return result;
}

u32 DirectDrawManager::surface_set_palette(GuestAddr surface, GuestAddr palette) {
    SurfaceObject* surface_state = find_surface(surface);
    if (!surface_state) return ddraw::DDERR_INVALIDOBJECT;
    PaletteObject* palette_state = find_palette(palette);
    if (!palette_state) return ddraw::DDERR_INVALIDOBJECT;
    surface_state->surface->set_palette(palette_state->colors);
    VITREO_TRACE("[ddraw] Surface::SetPalette surface=0x%llx palette=0x%llx\n",
                 static_cast<unsigned long long>(surface),
                 static_cast<unsigned long long>(palette));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_is_lost(GuestAddr surface) {
    // Vitreo's surfaces live in ordinary guest memory and are never freed by a
    // mode switch, so they are never lost. DD_OK means "memory still attached".
    // A Lock wrapper calls IsLost/Restore when it sees DDERR_SURFACELOST.
    // Answering E_NOTIMPL here makes that recovery path look like a hard failure.
    if (!find_surface(surface)) return ddraw::DDERR_INVALIDOBJECT;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_get_attached_surface(GuestAddr surface,
                                                    GuestAddr caps,
                                                    GuestAddr output) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (caps == 0) return ddraw::E_INVALIDARG;
    // DDSCAPS is a bare struct whose dwCaps is the first field.
    const u32 wanted = m_memory->read_u32(caps).value_or(0);
    constexpr u32 DDSCAPS_BACKBUFFER = 0x4;
    constexpr u32 DDSCAPS_FRONTBUFFER = 0x20;
    GuestAddr found = 0;
    if ((wanted & DDSCAPS_BACKBUFFER) != 0) {
        found = state->back_buffer;
    } else if ((wanted & DDSCAPS_FRONTBUFFER) != 0) {
        found = state->front_buffer;
    }
    // GetAttachedSurface is documented to fail rather than guess when the
    // requested capabilities match no attachment, and a caller distinguishes
    // "chain absent" from "bad call" by exactly this code.
    if (found == 0) return ddraw::DDERR_NOTFOUND;
    SurfaceObject* attached = find_surface(found);
    if (!attached) return ddraw::DDERR_NOTFOUND;
    // The returned interface is handed out with a reference the caller owns.
    ++attached->ref_count;
    if (m_memory->write_u32(output, static_cast<u32>(found)).is_err()) {
        --attached->ref_count;
        return ddraw::E_INVALIDARG;
    }
    VITREO_TRACE("[ddraw] GetAttachedSurface surface=0x%llx caps=0x%x "
                 "-> 0x%llx\n",
                 static_cast<unsigned long long>(surface), wanted,
                 static_cast<unsigned long long>(found));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_flip(GuestAddr surface,
                                    GuestAddr override_surface, u32 flags) {
    (void)flags;
    if (!m_memory) return ddraw::E_INVALIDARG;
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    // Flip is defined only on a surface that belongs to a flipping chain.
    if (state->back_buffer == 0) return ddraw::DDERR_NOTFLIPPABLE;
    if (override_surface != 0 && !find_surface(override_surface)) {
        return ddraw::DDERR_INVALIDOBJECT;
    }
    if (state->locked) return ddraw::DDERR_SURFACEBUSY;

    // Walk the ring from this surface. create_surface closes the last link
    // back onto the primary, so the walk terminates by returning to its start.
    std::vector<GuestAddr> ring;
    GuestAddr cursor = surface;
    constexpr usize kMaxRing = 8;
    while (ring.size() < kMaxRing) {
        ring.push_back(cursor);
        SurfaceObject* link = find_surface(cursor);
        if (!link || link->back_buffer == 0) break;
        cursor = link->back_buffer;
        if (cursor == surface) break;
    }
    if (ring.size() < 2) return ddraw::DDERR_NOTFLIPPABLE;
    for (const GuestAddr link : ring) {
        const SurfaceObject* entry = find_surface(link);
        if (!entry) return ddraw::DDERR_INVALIDOBJECT;
        if (entry->locked) return ddraw::DDERR_SURFACEBUSY;
    }

    // Page-flip semantics: the interface pointers keep their identity and the
    // storage behind them rotates one step, so what the guest just drew into
    // the back buffer becomes the front buffer's pixels. Copying instead would
    // give the same picture but would not carry the older frames along the
    // ring, which a multi-buffer caller depends on.
    struct Storage {
        GuestAddr pixels;
        u32 pixel_bytes;
        u32 mapped_bytes;
    };
    std::vector<Storage> storage;
    storage.reserve(ring.size());
    for (const GuestAddr link : ring) {
        const SurfaceObject& entry = m_surfaces.at(link);
        storage.push_back({entry.pixels, entry.pixel_bytes,
                           entry.mapped_bytes});
    }
    // Rotate the guest pixel storage only. The CpuSurface objects stay bound
    // to their SurfaceObject: every link in the chain has identical geometry,
    // so the description still matches, and the presenter caches frame
    // identity on the CpuSurface pointer plus its generation
    // (`m_surface != &surface || m_surface_generation != surface.generation()`).
    // Rotating the objects made the primary present X, then Y, then X again --
    // and on X's return an unchanged generation reads as "nothing new", so the
    // presenter re-presents the previous frame. That surfaces as intermittent
    // stale, blank or torn frames, and only at speed: under the interpreter
    // generations always advance between visits and it stays hidden.
    for (usize i = 0; i < ring.size(); ++i) {
        const Storage& from = storage[(i + 1) % storage.size()];
        SurfaceObject& entry = m_surfaces.at(ring[i]);
        entry.pixels = from.pixels;
        entry.pixel_bytes = from.pixel_bytes;
        entry.mapped_bytes = from.mapped_bytes;
        // The bytes behind this surface just changed wholesale.
        if (entry.surface) entry.surface->mark_dirty();
    }

    SurfaceObject& front = m_surfaces.at(surface);
    present_surface(surface, front);
    VITREO_TRACE("[ddraw] Flip surface=0x%llx ring=%zu -> pixels=0x%llx\n",
                 static_cast<unsigned long long>(surface), ring.size(),
                 static_cast<unsigned long long>(front.pixels));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_restore(GuestAddr surface) {
    // Nothing to reallocate: the pixel mapping created at CreateSurface is still
    // valid. Returning DD_OK matches Windows for a surface that was never lost.
    if (!find_surface(surface)) return ddraw::DDERR_INVALIDOBJECT;
    return ddraw::DD_OK;
}

namespace {

/// Writes a DDPIXELFORMAT at `format`. Callers holding a DDSURFACEDESC pass
/// its embedded format at descriptor + 72; GetPixelFormat passes the guest's
/// standalone struct.
void write_surface_pixelformat(GuestMemory& memory, GuestAddr format,
                               u32 bpp) noexcept {
    constexpr u32 DDPF_RGB = 0x00000040;
    constexpr u32 DDPF_PALETTEINDEXED8 = 0x00000020;
    for (u32 offset = 0; offset < 32; offset += 4) {
        (void)memory.write_u32(format + offset, 0);
    }
    (void)memory.write_u32(format + 0, 32);  // DDPIXELFORMAT dwSize
    (void)memory.write_u32(format + 4,
                           bpp == 8 ? DDPF_PALETTEINDEXED8 : DDPF_RGB);
    (void)memory.write_u32(format + 12, bpp);
    if (bpp == 16) {
        (void)memory.write_u32(format + 16, 0xF800);
        (void)memory.write_u32(format + 20, 0x07E0);
        (void)memory.write_u32(format + 24, 0x001F);
    } else if (bpp == 24 || bpp == 32) {
        (void)memory.write_u32(format + 16, 0x00FF0000);
        (void)memory.write_u32(format + 20, 0x0000FF00);
        (void)memory.write_u32(format + 24, 0x000000FF);
    }
}

} // namespace

u32 DirectDrawManager::surface_get_pixel_format(GuestAddr surface,
                                                GuestAddr format) {
    // learn.microsoft.com: IDirectDrawSurface::GetPixelFormat fills a
    // DDPIXELFORMAT describing the surface's colour layout. Unlike
    // GetSurfaceDesc the struct stands alone, so dwSize is at offset 0.
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (!m_memory || format == 0) return ddraw::DDERR_INVALIDPARAMS;
    write_surface_pixelformat(*m_memory, format,
                              state->surface->bits_per_pixel());
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_get_surface_desc(GuestAddr surface,
                                                GuestAddr descriptor) {
    // learn.microsoft.com: IDirectDrawSurface::GetSurfaceDesc fills a
    // DDSURFACEDESC describing the surface; dwSize must be initialized.
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (!m_memory || descriptor == 0) return ddraw::DDERR_INVALIDPARAMS;
    if (m_memory->read_u32(descriptor).value_or(0) != 108) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    const u32 width = state->surface->width();
    const u32 height = state->surface->height();
    const u32 pitch = state->surface->pitch();
    const u32 bpp = state->surface->bits_per_pixel();
    constexpr u32 DDSD_CAPS = 0x1;
    constexpr u32 DDSD_HEIGHT = 0x2;
    constexpr u32 DDSD_WIDTH = 0x4;
    constexpr u32 DDSD_PITCH = 0x8;
    constexpr u32 DDSD_LPSURFACE = 0x800;
    constexpr u32 DDSD_PIXELFORMAT = 0x1000;
    (void)m_memory->write_u32(descriptor + 4,
                              DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH |
                                  DDSD_LPSURFACE | DDSD_PIXELFORMAT);
    (void)m_memory->write_u32(descriptor + 8, height);
    (void)m_memory->write_u32(descriptor + 12, width);
    (void)m_memory->write_u32(descriptor + 16, pitch);
    (void)m_memory->write_u32(descriptor + 36, static_cast<u32>(state->pixels));
    write_surface_pixelformat(*m_memory, descriptor + 72, bpp);
    (void)m_memory->write_u32(descriptor + 104, state->caps);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_set_color_key(GuestAddr surface, u32 flags,
                                             GuestAddr color_key) {
    // learn.microsoft.com: IDirectDrawSurface::SetColorKey — DDCKEY_SRCBLT
    // stores the source key used by DDBLT_KEYSRC / DDBLTFAST_SRCCOLORKEY.
    constexpr u32 DDCKEY_SRCBLT = 0x00000008;
    constexpr u32 DDCKEY_DESTBLT = 0x00000002;
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (!m_memory || color_key == 0) return ddraw::DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_DESTBLT) != 0) return ddraw::E_NOTIMPL;
    if ((flags & DDCKEY_SRCBLT) == 0) return ddraw::E_INVALIDARG;
    state->src_color_key_low = m_memory->read_u32(color_key + 0).value_or(0);
    state->src_color_key_high = m_memory->read_u32(color_key + 4).value_or(0);
    state->has_src_color_key = true;
    VITREO_TRACE("[ddraw] SetColorKey surface=0x%llx SRCBLT low=0x%x high=0x%x\n",
                 static_cast<unsigned long long>(surface),
                 state->src_color_key_low, state->src_color_key_high);
    return ddraw::DD_OK;
}

} // namespace vitreo::winapi
