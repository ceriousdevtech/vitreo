// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_cube_texture.cpp
/// @brief IDirect3DCubeTexture9 dispatch.

#include "winapi/graphics/d3d9/d3d9_resources_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_resources_detail;

// =============================================================================
// IDirect3DCubeTexture9
// =============================================================================

void D3D9Manager::dispatch_cube_texture(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::CubeTexture)) return;
    if (dispatch_resource_base(m, slot, self, D3DRTYPE_CUBETEXTURE)) return;

    ComObject* entry = find(self, D3D9InterfaceId::CubeTexture);
    if (entry == nullptr || !entry->texture || !entry->texture->cube) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    TextureState& texture = *entry->texture;

    const auto face_level_surface = [&](u32 face, u32 level) -> u32 {
        if (face >= D3DCUBE_FACE_COUNT || level >= texture.levels) return 0;
        const usize index =
            static_cast<usize>(face) * texture.levels + level;
        if (index >= texture.level_surfaces.size()) return 0;
        if (texture.level_surfaces[index] != 0) {
            return texture.level_surfaces[index];
        }
        u32 edge = texture.width >> level;
        if (edge == 0) edge = 1;
        const u32 surface = create_surface_object(
            entry->device, edge, edge, texture.format, texture.usage,
            texture.pool,
            static_cast<int>(SurfaceState::Kind::TextureLevel), self, level,
            face);
        if (surface != 0) texture.level_surfaces[index] = surface;
        return surface;
    };

    switch (slot) {
    case 11: { // SetLOD
        const u32 previous = texture.lod;
        if (texture.pool == D3DPOOL_MANAGED) {
            texture.lod = std::min(static_cast<u32>(m.arg(1)),
                                   texture.levels - 1);
            m.set_return(previous);
            return;
        }
        m.set_return(0);
        return;
    }
    case 12: // GetLOD
        m.set_return(texture.pool == D3DPOOL_MANAGED ? texture.lod : 0);
        return;
    case 13: // GetLevelCount
        m.set_return(texture.levels);
        return;
    case 14: { // SetAutoGenFilterType
        const u32 filter = static_cast<u32>(m.arg(1));
        if ((texture.usage & D3DUSAGE_AUTOGENMIPMAP) == 0) {
            m.set_return(D3D_OK);
            return;
        }
        if (!is_supported_autogen_filter(filter)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const bool dirty = texture.auto_gen_filter != filter;
        texture.auto_gen_filter = filter;
        if (dirty) {
            generate_autogen_mips(m, texture, entry->device, self);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 15: // GetAutoGenFilterType
        m.set_return(texture.auto_gen_filter);
        return;
    case 16: { // GenerateMipSubLevels
        if (texture.levels > 1) {
            generate_autogen_mips(m, texture, entry->device, self);
        }
        m.set_return(0);
        return;
    }
    case 17: { // GetLevelDesc(Level, pDesc)
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        // Any face at this level has the same dimensions; use +X.
        const u32 surface = face_level_surface(0, level);
        ComObject* surface_entry =
            surface != 0 ? find(surface, D3D9InterfaceId::Surface) : nullptr;
        if (out == 0 || surface_entry == nullptr || !surface_entry->surface) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 8> desc{};
        write_surface_desc(desc, *surface_entry->surface);
        if (m.memory()
                .write_bytes(out, reinterpret_cast<const Byte*>(desc.data()),
                             D3DSURFACE_DESC_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 18: { // GetCubeMapSurface(FaceType, Level, ppSurface)
        const u32 face = static_cast<u32>(m.arg(1));
        const u32 level = static_cast<u32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)m.write_u32(out, 0);
        const u32 surface = face_level_surface(face, level);
        if (out == 0 || surface == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(surface);
        if (m.write_u32(out, surface).is_err()) {
            (void)release(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 19: { // LockRect(FaceType, Level, pLockedRect, pRect, Flags)
        const u32 face = static_cast<u32>(m.arg(1));
        const u32 level = static_cast<u32>(m.arg(2));
        const GuestAddr locked_rect_out = m.arg_ptr(3);
        const GuestAddr rect_in = m.arg_ptr(4);
        const u32 flags = static_cast<u32>(m.arg(5));
        const u32 surface = face_level_surface(face, level);
        ComObject* surface_entry =
            surface != 0 ? find(surface, D3D9InterfaceId::Surface) : nullptr;
        if (locked_rect_out == 0 || surface_entry == nullptr ||
            !surface_entry->surface) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        SurfaceState& level_state = *surface_entry->surface;
        if (level_state.locked || level_state.pixels == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        d3d9_cs_drain();
        GuestAddr locked_bits = 0;
        if (!locked_rect_view(m, level_state, rect_in, flags, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.write_u32(locked_rect_out, level_state.pitch).is_err() ||
            m.write_ptr(locked_rect_out + (m.cpu_is_64bit() ? 8 : 4), locked_bits)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        level_state.locked = true;
        level_state.lock_flags = flags;
        m.set_return(D3D_OK);
        return;
    }
    case 20: { // UnlockRect(FaceType, Level)
        const u32 face = static_cast<u32>(m.arg(1));
        const u32 level = static_cast<u32>(m.arg(2));
        if (face >= D3DCUBE_FACE_COUNT || level >= texture.levels) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const usize index =
            static_cast<usize>(face) * texture.levels + level;
        if (index >= texture.level_surfaces.size() ||
            texture.level_surfaces[index] == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        ComObject* surface_entry =
            find(texture.level_surfaces[index], D3D9InterfaceId::Surface);
        if (surface_entry == nullptr || !surface_entry->surface ||
            !surface_entry->surface->locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        SurfaceState& level_state = *surface_entry->surface;
        level_state.locked = false;
        const bool cpu_owned_pool =
            texture.pool == D3DPOOL_MANAGED ||
            texture.pool == D3DPOOL_SYSTEMMEM;
        const bool allow_upload =
            (level_state.lock_flags & D3DLOCK_READONLY) == 0 || cpu_owned_pool;
        if (allow_upload && texture.backend) {
            std::vector<u8> pixels(level_state.pixel_bytes);
            if (m.memory()
                    .read_bytes(level_state.pixels, pixels.data(),
                                pixels.size())
                    .is_ok()) {
                std::vector<u8> expanded;
                const u8* upload = pixels.data();
                u32 upload_pitch = level_state.pitch;
                if (d3d9_is_palettized_format(texture.format)) {
                    DeviceState* device_state = nullptr;
                    if (ComObject* device_entry =
                            find(entry->device, D3D9InterfaceId::Device);
                        device_entry != nullptr &&
                        device_entry->device_state) {
                        device_state = device_entry->device_state.get();
                    }
                    const u8* palette =
                        device_state != nullptr
                            ? resolve_current_palette_entries(*device_state)
                            : nullptr;
                    if (d3d9_expand_palettized_to_bgra8(
                            texture.format, level_state.width,
                            level_state.height, pixels.data(),
                            level_state.pitch, palette, expanded)) {
                        upload = expanded.data();
                        upload_pitch = level_state.width * 4u;
                        if (device_state != nullptr) {
                            texture.palettized_upload_serial =
                                device_state->texture_palette_serial;
                        }
                    }
                } else {
                    const bool expanded_bump =
                        d3d9_expand_special_bump_to_rgba32f(
                            texture.format, level_state.width,
                            level_state.height, pixels.data(),
                            level_state.pitch, expanded);
                    if (expanded_bump || d3d9_expand_lock_to_bgra8(
                            texture.format, level_state.width,
                            level_state.height, pixels.data(),
                            level_state.pitch, expanded)) {
                        upload = expanded.data();
                        upload_pitch = level_state.width *
                                       (expanded_bump ? 16u : 4u);
                    }
                }
                if (!texture.backend->update_slice(level, face, upload,
                                                   upload_pitch)) {
                    VITREO_TRACE("[d3d9] CubeTexture::UnlockRect: backend "
                                 "rejected face=%u level=%u\n",
                                 face, level);
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
                if (level == 0 &&
                    (texture.usage & D3DUSAGE_AUTOGENMIPMAP) != 0 &&
                    texture.levels > 1) {
                    generate_autogen_mips(m, texture, entry->device, self);
                }
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    case 21: // AddDirtyRect(FaceType, pDirtyRect)
        m.set_return(D3D_OK);
        return;
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
