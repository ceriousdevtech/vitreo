// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_volume.cpp
/// @brief IDirect3DVolumeTexture9 and IDirect3DVolume9 dispatch.

#include "winapi/graphics/d3d9/d3d9_resources_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_resources_detail;

// =============================================================================
// IDirect3DVolumeTexture9 / IDirect3DVolume9
// =============================================================================

namespace {

void write_volume_desc(std::array<u32, 7>& desc,
                       const D3D9Manager::VolumeState& volume) {
    desc[0] = volume.format;
    desc[1] = D3DRTYPE_VOLUME;
    desc[2] = volume.usage;
    desc[3] = volume.pool;
    desc[4] = volume.width;
    desc[5] = volume.height;
    desc[6] = volume.depth;
}

/// Push a locked volume level into BackendTexture3D when present.
/// Returns false only when the backend rejects a required upload.
[[nodiscard]] bool upload_volume_unlock(
    Marshaler& m, D3D9Manager::TextureState& texture,
    const D3D9Manager::VolumeState& volume) {
    if (!texture.backend_3d || !texture.backend_3d->valid()) return true;
    const bool cpu_owned_pool = texture.pool == D3DPOOL_MANAGED ||
                                texture.pool == D3DPOOL_SYSTEMMEM;
    const bool allow_upload =
        (volume.lock_flags & D3DLOCK_READONLY) == 0 || cpu_owned_pool;
    if (!allow_upload || volume.pixels == 0 || volume.pixel_bytes == 0) {
        return true;
    }
    std::vector<u8> pixels(volume.pixel_bytes);
    if (m.memory()
            .read_bytes(volume.pixels, pixels.data(), pixels.size())
            .is_err()) {
        return true;
    }
    // Expand each depth slice when the guest format is not backend-native.
    std::vector<u8> expanded;
    const u8* upload = pixels.data();
    u32 upload_row = volume.row_pitch;
    u32 upload_slice = volume.slice_pitch;
    std::vector<u8> first_slice;
    const bool expanded_bump = d3d9_expand_special_bump_to_rgba32f(
        volume.format, volume.width, volume.height, pixels.data(),
        volume.row_pitch, first_slice);
    if (expanded_bump ||
        d3d9_expand_lock_to_bgra8(volume.format, volume.width, volume.height,
                                  pixels.data(), volume.row_pitch,
                                  first_slice)) {
        const u32 expanded_bpp = expanded_bump ? 16u : 4u;
        expanded.resize(static_cast<usize>(volume.width) * volume.height *
                        expanded_bpp * volume.depth);
        const u32 dst_slice = volume.width * volume.height * expanded_bpp;
        std::memcpy(expanded.data(), first_slice.data(), first_slice.size());
        for (u32 z = 1; z < volume.depth; ++z) {
            std::vector<u8> slice;
            const u8* src =
                pixels.data() + static_cast<usize>(z) * volume.slice_pitch;
            const bool slice_ok = expanded_bump
                ? d3d9_expand_special_bump_to_rgba32f(
                      volume.format, volume.width, volume.height, src,
                      volume.row_pitch, slice)
                : d3d9_expand_lock_to_bgra8(
                      volume.format, volume.width, volume.height, src,
                      volume.row_pitch, slice);
            if (!slice_ok) {
                return false;
            }
            std::memcpy(expanded.data() + static_cast<usize>(z) * dst_slice,
                        slice.data(), slice.size());
        }
        upload = expanded.data();
        upload_row = volume.width * expanded_bpp;
        upload_slice = dst_slice;
    }
    if (!texture.backend_3d->update(volume.level, upload, upload_row,
                                    upload_slice)) {
        VITREO_TRACE("[d3d9] UnlockBox: backend rejected volume level %u "
                     "(%ux%ux%u)\n",
                     volume.level, volume.width, volume.height, volume.depth);
        return false;
    }
    return true;
}

/// Resolve pBits for a D3DLOCKED_BOX, honoring an optional D3DBOX subregion.
[[nodiscard]] bool locked_box_view(Marshaler& m,
                                   const D3D9Manager::VolumeState& volume,
                                   GuestAddr box_in, u32 flags,
                                   GuestAddr& bits_out) {
    bits_out = volume.pixels;
    if (box_in == 0) return true;
    if ((flags & D3DLOCK_DISCARD) != 0) return false;

    std::array<u32, 6> box{};
    if (m.memory()
            .read_bytes(box_in, reinterpret_cast<Byte*>(box.data()),
                        sizeof(box))
            .is_err()) {
        return false;
    }
    const u32 left = box[0];
    const u32 top = box[1];
    const u32 right = box[2];
    const u32 bottom = box[3];
    const u32 front = box[4];
    const u32 back = box[5];
    if (right <= left || bottom <= top || back <= front ||
        right > volume.width || bottom > volume.height ||
        back > volume.depth) {
        return false;
    }
    const u32 bpp = d3d9_format_bytes_per_pixel(volume.format);
    if (bpp == 0) {
        // Compressed volume formats are not lockable through this path yet.
        return false;
    }
    const usize offset = static_cast<usize>(front) * volume.slice_pitch +
                         static_cast<usize>(top) * volume.row_pitch +
                         static_cast<usize>(left) * bpp;
    if (offset >= volume.pixel_bytes) return false;
    bits_out = volume.pixels + offset;
    return true;
}

[[nodiscard]] bool write_locked_box(Marshaler& m, GuestAddr out,
                                    const D3D9Manager::VolumeState& volume,
                                    GuestAddr bits) {
    if (out == 0) return false;
    return m.write_u32(out, volume.row_pitch).is_ok() &&
           m.write_u32(out + 4, volume.slice_pitch).is_ok() &&
           m.write_ptr(out + 8, bits).is_ok();
}

} // namespace

void D3D9Manager::dispatch_volume_texture(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::VolumeTexture)) {
        return;
    }
    if (dispatch_resource_base(m, slot, self, D3DRTYPE_VOLUMETEXTURE)) {
        return;
    }

    ComObject* entry = find(self, D3D9InterfaceId::VolumeTexture);
    if (entry == nullptr || !entry->texture || !entry->texture->volume) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    TextureState& texture = *entry->texture;

    const auto level_volume = [&](u32 level) -> u32 {
        if (level >= texture.levels) return 0;
        if (texture.level_surfaces[level] != 0) {
            return texture.level_surfaces[level];
        }
        u32 level_width = texture.width >> level;
        u32 level_height = texture.height >> level;
        u32 level_depth = texture.depth >> level;
        if (level_width == 0) level_width = 1;
        if (level_height == 0) level_height = 1;
        if (level_depth == 0) level_depth = 1;
        const u32 volume = create_volume_object(
            entry->device, level_width, level_height, level_depth,
            texture.format, texture.usage, texture.pool, self, level);
        if (volume != 0) texture.level_surfaces[level] = volume;
        return volume;
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
    case 16: { // GenerateMipSubLevels (void return)
        if (texture.levels > 1) {
            generate_autogen_mips(m, texture, entry->device, self);
        }
        m.set_return(0);
        return;
    }
    case 17: { // GetLevelDesc(Level, pDesc)
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        const u32 volume_obj = level_volume(level);
        ComObject* volume_entry =
            volume_obj != 0 ? find(volume_obj, D3D9InterfaceId::Volume)
                            : nullptr;
        if (out == 0 || volume_entry == nullptr || !volume_entry->volume) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 7> desc{};
        write_volume_desc(desc, *volume_entry->volume);
        if (m.memory()
                .write_bytes(out, reinterpret_cast<const Byte*>(desc.data()),
                             D3DVOLUME_DESC_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 18: { // GetVolumeLevel(Level, ppVolumeLevel)
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        const u32 volume_obj = level_volume(level);
        if (out == 0 || volume_obj == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(volume_obj);
        if (m.write_u32(out, volume_obj).is_err()) {
            (void)release(volume_obj);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 19: { // LockBox(Level, pLockedVolume, pBox, Flags)
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr locked_out = m.arg_ptr(2);
        const GuestAddr box_in = m.arg_ptr(3);
        const u32 flags = static_cast<u32>(m.arg(4));
        const u32 volume_obj = level_volume(level);
        ComObject* volume_entry =
            volume_obj != 0 ? find(volume_obj, D3D9InterfaceId::Volume)
                            : nullptr;
        if (locked_out == 0 || volume_entry == nullptr ||
            !volume_entry->volume) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VolumeState& level_state = *volume_entry->volume;
        if (level_state.locked || level_state.pixels == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        d3d9_cs_drain();
        GuestAddr locked_bits = 0;
        if (!locked_box_view(m, level_state, box_in, flags, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (!write_locked_box(m, locked_out, level_state, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        level_state.locked = true;
        level_state.lock_flags = flags;
        m.set_return(D3D_OK);
        return;
    }
    case 20: { // UnlockBox(Level)
        const u32 level = static_cast<u32>(m.arg(1));
        if (level >= texture.levels || texture.level_surfaces[level] == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        ComObject* volume_entry =
            find(texture.level_surfaces[level], D3D9InterfaceId::Volume);
        if (volume_entry == nullptr || !volume_entry->volume ||
            !volume_entry->volume->locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VolumeState& level_state = *volume_entry->volume;
        level_state.locked = false;
        if (!upload_volume_unlock(m, texture, level_state)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // D3DUSAGE_AUTOGENMIPMAP: dirtying level 0 regenerates the chain.
        if (level == 0 &&
            (texture.usage & D3DUSAGE_AUTOGENMIPMAP) != 0 &&
            texture.levels > 1) {
            generate_autogen_mips(m, texture, entry->device, self);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 21: // AddDirtyBox(pDirtyBox)
        m.set_return(D3D_OK);
        return;
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

void D3D9Manager::dispatch_volume(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::Volume)) return;

    ComObject* entry = find(self, D3D9InterfaceId::Volume);
    if (entry == nullptr || !entry->volume) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    VolumeState& volume = *entry->volume;

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 4: // SetPrivateData
    case 5: // GetPrivateData
    case 6: // FreePrivateData
        // IDirect3DVolume9 owns the same GUID→blob store as resources.
        dispatch_private_data(m, slot, *entry);
        return;
    case 7: { // GetContainer
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        const u32 container =
            volume.container != 0 ? volume.container : entry->device;
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
    case 8: { // GetDesc
        const GuestAddr out = m.arg_ptr(1);
        std::array<u32, 7> desc{};
        write_volume_desc(desc, volume);
        if (out == 0 ||
            m.memory()
                .write_bytes(out, reinterpret_cast<const Byte*>(desc.data()),
                             D3DVOLUME_DESC_SIZE)
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 9: { // LockBox(pLockedVolume, pBox, Flags)
        const GuestAddr locked_out = m.arg_ptr(1);
        const GuestAddr box_in = m.arg_ptr(2);
        const u32 flags = static_cast<u32>(m.arg(3));
        if (locked_out == 0 || volume.pixels == 0 || volume.locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        d3d9_cs_drain();
        GuestAddr locked_bits = 0;
        if (!locked_box_view(m, volume, box_in, flags, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (!write_locked_box(m, locked_out, volume, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        volume.locked = true;
        volume.lock_flags = flags;
        m.set_return(D3D_OK);
        return;
    }
    case 10: { // UnlockBox
        if (!volume.locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        volume.locked = false;
        if (volume.container != 0) {
            ComObject* parent =
                find(volume.container, D3D9InterfaceId::VolumeTexture);
            if (parent != nullptr && parent->texture &&
                parent->texture->volume) {
                TextureState& texture = *parent->texture;
                if (!upload_volume_unlock(m, texture, volume)) {
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
                // D3DUSAGE_AUTOGENMIPMAP: dirtying level 0 regenerates the chain.
                if (volume.level == 0 &&
                    (texture.usage & D3DUSAGE_AUTOGENMIPMAP) != 0 &&
                    texture.levels > 1) {
                    generate_autogen_mips(m, texture, parent->device,
                                           volume.container);
                }
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
