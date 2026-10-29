// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_resources.cpp
/// @brief IDirect3DSurface9 dispatch and palettized-texture sync.
///
/// The other D3D9 resource interfaces live in sibling parts:
/// d3d9_texture_create.cpp (CreateTexture, CreateCubeTexture,
/// CreateVolumeTexture), d3d9_texture_autogen.cpp (D3DUSAGE_AUTOGENMIPMAP
/// sublevels), d3d9_texture.cpp (IDirect3DTexture9), d3d9_cube_texture.cpp
/// (IDirect3DCubeTexture9), d3d9_volume.cpp (IDirect3DVolumeTexture9 and
/// IDirect3DVolume9) and d3d9_buffer.cpp (IDirect3DVertexBuffer9 and
/// IDirect3DIndexBuffer9). The helpers they share are in
/// d3d9_resources_internal.hpp.
///
/// Locking is the contract that matters most here: D3D9 hands the application
/// a raw pointer into resource memory. Vitreo maps guest-visible memory for
/// the resource's whole lifetime and pushes the locked range into the shared
/// backend on unlock, so pixels and vertices the guest writes are the ones the
/// GPU receives.

#include "winapi/graphics/d3d9/d3d9_resources_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_resources_detail;

// =============================================================================
// IDirect3DSurface9
// =============================================================================

void D3D9Manager::dispatch_surface(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::Surface)) return;
    if (dispatch_resource_base(m, slot, self, D3DRTYPE_SURFACE)) return;

    ComObject* entry = find(self, D3D9InterfaceId::Surface);
    if (entry == nullptr || !entry->surface) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    SurfaceState& surface = *entry->surface;

    switch (slot) {
    case 11: { // GetContainer
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        // The container is the texture or swap chain that owns the surface;
        // resolving it by IID needs the same interface-identity check
        // QueryInterface performs on the container itself.
        const u32 container =
            surface.container != 0 ? surface.container : entry->device;
        if (out == 0 || container == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(container);
        if (m.write_u32(out, container).is_err()) {
            (void)release(container);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 12: { // GetDesc
        const GuestAddr out = m.arg_ptr(1);
        std::array<u32, 8> desc{};
        write_surface_desc(desc, surface);
        if (out == 0 ||
            m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(desc.data()),
                D3DSURFACE_DESC_SIZE).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 13: { // LockRect
        const GuestAddr locked_rect_out = m.arg_ptr(1);
        const GuestAddr rect_in = m.arg_ptr(2);
        const u32 flags = static_cast<u32>(m.arg(3));
        if (d3d9_texture_lifetime_diag_enabled() &&
            is_large_texture(surface.width, surface.height)) {
            std::fprintf(stderr,
                         "[d3d9-texture-life] surface-lock "
                         "surface=0x%08x texture=0x%08x %ux%u "
                         "rect=0x%llx flags=0x%x pixels=0x%llx\n",
                         self, surface.container, surface.width,
                         surface.height,
                         static_cast<unsigned long long>(rect_in), flags,
                         static_cast<unsigned long long>(surface.pixels));
        }
        if (locked_rect_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (surface.pixels == 0 ||
            surface.multi_sample_type != D3DMULTISAMPLE_NONE) {
            VITREO_TRACE("[d3d9] IDirect3DSurface9::LockRect -> "
                         "D3DERR_INVALIDCALL (surface is not lockable)\n");
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (surface.locked) {
            // Windows rejects a second lock of the same surface.
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        d3d9_cs_drain();
        GuestAddr locked_bits = 0;
        if (!locked_rect_view(m, surface, rect_in, flags, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Keep AppKit responsive around large host transfers. This only
        // enqueues native input; it never dispatches guest Win32 messages.
        if (surface.width >= 256u && surface.height >= 256u) {
            pump_host_gui();
        }
        // D3DLOCKED_RECT is { INT Pitch; void* pBits; }.
        if (m.write_u32(locked_rect_out, surface.pitch).is_err() ||
            m.write_ptr(locked_rect_out + (m.cpu_is_64bit() ? 8 : 4), locked_bits).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        surface.locked = true;
        surface.lock_flags = flags;
        static u32 surf_lock_ok = 0;
        ++surf_lock_ok;
        // Always TRACE large stills (800×600 and up).
        if (surf_lock_ok == 1 || (surf_lock_ok & 0xFFu) == 0 ||
            (surface.width >= 800u && surface.height >= 600u)) {
            VITREO_TRACE("[d3d9] IDirect3DSurface9::LockRect ok #%u "
                         "%ux%u flags=0x%x\n",
                         surf_lock_ok, surface.width, surface.height, flags);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 14: { // UnlockRect
        const auto unlock_started = std::chrono::steady_clock::now();
        if (!surface.locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        surface.locked = false;
        // A texture level's pixels must reach the backend texture; a back
        // buffer or offscreen surface has no shared-core object to push to
        // until a blit path exists.
        //
        // D3DLOCK_READONLY normally means "skip GPU writeback". For MANAGED /
        // SYSTEMMEM, Lock returns our CPU staging buffer. Callers still write
        // it during decompress (flags 0x810 = NOSYSLOCK|READONLY) and
        // skipping the upload leaves the textures uninitialized. Always push
        // those pools.
        const bool cpu_owned_pool =
            surface.pool == D3DPOOL_MANAGED ||
            surface.pool == D3DPOOL_SYSTEMMEM;
        const bool allow_upload =
            (surface.lock_flags & D3DLOCK_READONLY) == 0 || cpu_owned_pool;
        if (surface.kind == SurfaceState::Kind::TextureLevel &&
            surface.container != 0 && allow_upload) {
            ComObject* owner = find(surface.container,
                                    D3D9InterfaceId::Texture);
            if (owner == nullptr) {
                owner = find(surface.container, D3D9InterfaceId::CubeTexture);
            }
            if (owner != nullptr && owner->texture &&
                owner->texture->backend) {
                // Keep the menu interactive during large slideshow uploads
                // (1600×1200 ≈ 7.7 MiB) which run with no PeekMessage.
                if (surface.width >= 256u && surface.height >= 256u) {
                    pump_host_gui();
                }

                // Prefer a direct host pointer for native BGRA locks — avoids
                // a full guest→vector copy of a large surface.
                const u8* upload = nullptr;
                u32 upload_pitch = surface.pitch;
                std::vector<u8> pixels;
                std::vector<u8> expanded;
                const bool native_bgra =
                    surface.format == 21u /* A8R8G8B8 */ ||
                    surface.format == 22u /* X8R8G8B8 */;
                const bool palettized =
                    d3d9_is_palettized_format(owner->texture->format);
                const Byte* host = m.memory().host_ptr(
                    surface.pixels, surface.pixel_bytes);
                if (host != nullptr && native_bgra && !palettized) {
                    upload = host;
                } else {
                    pixels.resize(surface.pixel_bytes);
                    if (m.memory().read_bytes(surface.pixels, pixels.data(),
                                              pixels.size()).is_ok()) {
                        upload = pixels.data();
                        if (palettized) {
                            // Learn: sample through the device's current
                            // texture palette (not a per-texture palette).
                            DeviceState* device_state = nullptr;
                            if (ComObject* device_entry =
                                    find(owner->device,
                                         D3D9InterfaceId::Device);
                                device_entry != nullptr &&
                                device_entry->device_state) {
                                device_state = device_entry->device_state.get();
                            }
                            const u8* palette =
                                device_state != nullptr
                                    ? resolve_current_palette_entries(
                                          *device_state)
                                    : nullptr;
                            if (d3d9_expand_palettized_to_bgra8(
                                    owner->texture->format, surface.width,
                                    surface.height, pixels.data(),
                                    surface.pitch, palette, expanded)) {
                                upload = expanded.data();
                                upload_pitch = surface.width * 4u;
                                if (device_state != nullptr) {
                                    owner->texture->palettized_upload_serial =
                                        device_state->texture_palette_serial;
                                }
                            }
                        } else {
                            const bool expanded_bump =
                                d3d9_expand_special_bump_to_rgba32f(
                                    surface.format, surface.width,
                                    surface.height, pixels.data(),
                                    surface.pitch, expanded);
                            if (expanded_bump || d3d9_expand_lock_to_bgra8(
                                    surface.format, surface.width,
                                    surface.height, pixels.data(),
                                    surface.pitch, expanded)) {
                                upload = expanded.data();
                                upload_pitch = surface.width *
                                               (expanded_bump ? 16u : 4u);
                            }
                        }
                    }
                }
                if (upload != nullptr) {
                    bool ok = false;
                    const unsigned slice =
                        owner->texture->cube ? surface.face : 0u;
                    // Large stills: upload in horizontal strips and pump
                    // AppKit between them so UnlockRect itself cannot freeze
                    // the menu for tens of milliseconds on 1600×1200.
                    constexpr u32 kStrip = 64u;
                    if (!owner->texture->cube && surface.width >= 256u &&
                        surface.height >= 256u && native_bgra &&
                        surface.height > kStrip) {
                        ok = true;
                        for (u32 y = 0; y < surface.height; y += kStrip) {
                            const u32 h = std::min(kStrip, surface.height - y);
                            const u8* row =
                                upload + static_cast<usize>(y) * upload_pitch;
                            if (!owner->texture->backend->update_rows(
                                    surface.level, row, upload_pitch, y, h)) {
                                ok = false;
                                break;
                            }
                            pump_host_gui();
                        }
                        if (!ok) {
                            // Fall back to a single full-level update.
                            ok = owner->texture->backend->update_slice(
                                surface.level, slice, upload, upload_pitch);
                        }
                    } else {
                        ok = owner->texture->backend->update_slice(
                            surface.level, slice, upload, upload_pitch);
                    }
                    if (!ok) {
                        VITREO_TRACE("[d3d9] IDirect3DSurface9::UnlockRect: "
                                     "backend rejected upload %ux%u\n",
                                     surface.width, surface.height);
                        m.set_return(D3DERR_INVALIDCALL);
                        return;
                    }
                    if (d3d9_texture_lifetime_diag_enabled() &&
                        is_large_texture(surface.width, surface.height)) {
                        std::fprintf(stderr,
                                     "[d3d9-texture-life] surface-upload "
                                     "surface=0x%08x texture=0x%08x %ux%u "
                                     "level=%u pitch=%u\n",
                                     self, surface.container, surface.width,
                                     surface.height, surface.level,
                                     upload_pitch);
                    }
                    static u32 surf_unlock_ok = 0;
                    ++surf_unlock_ok;
                    if (surf_unlock_ok == 1 || (surf_unlock_ok & 0xFFu) == 0 ||
                        (surface.width >= 800u && surface.height >= 600u)) {
                        VITREO_TRACE("[d3d9] IDirect3DSurface9::UnlockRect "
                                     "upload ok #%u %ux%u level=%u\n",
                                     surf_unlock_ok, surface.width,
                                     surface.height, surface.level);
                    }
                    // D3DUSAGE_AUTOGENMIPMAP: dirtying level 0 regenerates the
                    // rest of the chain (glyphs / LOD paths).
                    if (ok && surface.level == 0 &&
                        (owner->texture->usage & D3DUSAGE_AUTOGENMIPMAP) != 0 &&
                        owner->texture->levels > 1) {
                        generate_autogen_mips(m, *owner->texture,
                                               owner->device, surface.container);
                    }
                }
                if (surface.width >= 256u && surface.height >= 256u) {
                    pump_host_gui();
                }
            }
        }
        report_slow_resource_call("surface UnlockRect", unlock_started,
                                  surface.width, surface.height);
        m.set_return(D3D_OK);
        return;
    }
    case 15: // GetDC
    case 16: // ReleaseDC
        VITREO_TRACE("[d3d9] IDirect3DSurface9 GDI interop -> E_NOTIMPL "
                     "(no HDC over a D3D9 surface)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

void D3D9Manager::sync_palettized_texture(TextureState& texture,
                                          DeviceState& state, bool force) {
    if (!d3d9_is_palettized_format(texture.format) || !texture.backend ||
        m_memory == nullptr) {
        return;
    }
    if (!force && texture.palettized_upload_serial ==
                      state.texture_palette_serial) {
        return;
    }
    const u8* palette = resolve_current_palette_entries(state);
    for (const u32 surface_obj : texture.level_surfaces) {
        if (surface_obj == 0) continue;
        ComObject* surface_entry =
            find(surface_obj, D3D9InterfaceId::Surface);
        if (surface_entry == nullptr || !surface_entry->surface ||
            surface_entry->surface->pixels == 0 ||
            surface_entry->surface->pixel_bytes == 0) {
            continue;
        }
        SurfaceState& level_state = *surface_entry->surface;
        std::vector<u8> pixels(level_state.pixel_bytes);
        if (m_memory
                ->read_bytes(level_state.pixels, pixels.data(), pixels.size())
                .is_err()) {
            continue;
        }
        std::vector<u8> expanded;
        if (!d3d9_expand_palettized_to_bgra8(
                texture.format, level_state.width, level_state.height,
                pixels.data(), level_state.pitch, palette, expanded)) {
            continue;
        }
        const u32 upload_pitch = level_state.width * 4u;
        bool ok = false;
        if (texture.cube) {
            ok = texture.backend->update_slice(level_state.level,
                                               level_state.face,
                                               expanded.data(), upload_pitch);
        } else {
            ok = texture.backend->update(level_state.level, expanded.data(),
                                         upload_pitch);
        }
        if (!ok) continue;
    }
    texture.palettized_upload_serial = state.texture_palette_serial;
}

void D3D9Manager::sync_bound_palettized_textures(DeviceState& state) {
    for (u32 slot = 0; slot < D3D9_MAX_SAMPLERS; ++slot) {
        const u32 texture_obj = state.textures[slot];
        if (texture_obj == 0) continue;
        ComObject* entry = find(texture_obj, D3D9InterfaceId::Texture);
        if (entry == nullptr) {
            entry = find(texture_obj, D3D9InterfaceId::CubeTexture);
        }
        if (entry == nullptr || !entry->texture) continue;
        sync_palettized_texture(*entry->texture, state, false);
    }
}

} // namespace vitreo::winapi
