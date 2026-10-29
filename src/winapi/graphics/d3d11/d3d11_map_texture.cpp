// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_map_texture.cpp
/// @brief The texture half of ID3D11DeviceContext::Map / Unmap.
///
/// map_texture and unmap_texture, shared with IDXGISurface::Map / Unmap.
/// The image layout and copies they use are in d3d11_mapped_image.cpp.
///
/// Split out of d3d11_context.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

// The texture half of ID3D11DeviceContext::Map, shared with
// IDXGISurface::Map: validate, stage the subresource in guest memory, and
// report where. Returns the HRESULT.
u32 D3D11Manager::map_texture(GuestAddr resource, u32 subresource,
                              u32 map_type, u32 map_flags,
                              GuestAddr context_owner, GuestAddr& data_out,
                              u32& row_pitch_out, u32& depth_pitch_out,
                              bool want_pointer) {
    auto texture = m_textures.find(resource);
    if (texture == m_textures.end()) return d3d11::E_INVALIDARG;
    const u32 subresource_count =
        texture->second.desc.mip_levels *
        texture->second.desc.array_size;
    const u32 cpu = texture->second.desc.cpu_access_flags;
    const bool staging =
        texture->second.desc.usage == D3D11_USAGE_STAGING;
    const bool dynamic =
        texture->second.desc.usage == D3D11_USAGE_DYNAMIC;
    if (texture->second.desc.usage == D3D11_USAGE_DEFAULT && cpu != 0) {
        // Map on default (D3D11_OPTIONS2.MapOnDefaultTextures), measured on
        // Windows (d3d11_texmap_probe [map]):
        //  * READ, WRITE, READ_WRITE and WRITE_NO_OVERWRITE, each within the
        //    CPU access flags; WRITE_DISCARD and anything else E_INVALIDARG;
        //  * MapFlags 0 or DO_NOT_WAIT;
        //  * pMappedResource may be NULL (Learn Map, "NULL pointers for
        //    pMappedResource"); a pointer is handed out only by a layout that
        //    defines one (mapped_image_layout), else E_INVALIDARG;
        //  * mapping a mapped subresource again: NULL E_INVALIDARG, a
        //    pointer the same mapping; other subresources map independently;
        //  * deferred contexts refuse it (deferred_map: DYNAMIC only).
        auto& t = texture->second;
        const bool can_read = (cpu & D3D11_CPU_ACCESS_READ) != 0;
        const bool can_write = (cpu & D3D11_CPU_ACCESS_WRITE) != 0;
        const bool type_ok =
            (map_type == D3D11_MAP_READ && can_read) ||
            ((map_type == D3D11_MAP_WRITE ||
              map_type == D3D11_MAP_WRITE_NO_OVERWRITE) && can_write) ||
            (map_type == D3D11_MAP_READ_WRITE && can_read && can_write);
        if (t.device_owner != context_owner ||
            subresource >= subresource_count || !type_ok ||
            (map_flags & ~D3D11_MAP_FLAG_DO_NOT_WAIT) != 0 ||
            dxgi_format_is_planar(t.desc.format)) {
            d3d11_diag("ID3D11DeviceContext::Map invalid DEFAULT texture "
                       "resource=0x%llx subresource=%u type=%u flags=0x%x",
                       static_cast<unsigned long long>(resource), subresource,
                       map_type, map_flags);
            return d3d11::E_INVALIDARG;
        }
        const auto layout = mapped_image_layout(t, subresource);
        const bool has_pointer =
            layout.kind != MappedImageLayout::Kind::None;
        if (want_pointer && !has_pointer) return d3d11::E_INVALIDARG;
        auto& storage = t.subresources[subresource];
        if (storage.mapped) {
            if (!want_pointer) return d3d11::E_INVALIDARG;
            data_out = storage.mapped_address;
            row_pitch_out = layout.row_pitch;
            depth_pitch_out = layout.depth_pitch;
            return d3d11::S_OK;
        }
        // DO_NOT_WAIT: WAS_STILL_DRAWING while GPU work submitted before this
        // Map has not finished (the backend does not track which textures a
        // draw touched, so any unfinished work counts); the flush hands over
        // what is still recording so a polling caller sees it retire.
        if ((map_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0) {
            const u64 generation = m_backend.recording_generation();
            if (generation > 1 &&
                !m_backend.generation_complete(generation - 1)) {
                (void)m_backend.flush_completion();
                return kDxgiErrorWasStillDrawing;
            }
        }
        if (!read_default_subresource(t, subresource)) {
            d3d11_unimplemented_once(
                "Map", "reading back a DEFAULT texture of DXGI format " +
                           std::to_string(t.desc.format));
            return 0x8007000Eu;  // E_OUTOFMEMORY
        }
        if (has_pointer) {
            // Whole host pages, host-page aligned: a mapped resource is hot
            // guest memory, and a host page it only partly fills would send
            // every guest access to it through the checked helper. See
            // round_up_host_page.
            const usize mapped_size =
                static_cast<usize>(round_up_host_page(layout.size));
            if (storage.mapped_address == 0) {
                GuestAddr address = m_memory.find_free_gap(
                    m_next_map_hint, mapped_size, kLinearHostPageSize);
                if (address == 0) {
                    address = m_memory.find_free_gap(
                        0x10000000ull, mapped_size, kLinearHostPageSize);
                }
                if (address == 0 ||
                    m_memory.map_region(address, mapped_size,
                                        MemoryProtection::ReadWrite)
                        .is_err()) {
                    return 0x8007000Eu;
                }
                storage.mapped_address = address;
                storage.mapped_size = mapped_size;
                m_next_map_hint = address + mapped_size;
            }
            std::vector<u8> image(layout.size);
            store_mapped_image(t, subresource, layout, image.data());
            if (m_memory.write_bytes(storage.mapped_address, image.data(),
                                     image.size()).is_err()) {
                return d3d11::E_INVALIDARG;
            }
            data_out = storage.mapped_address;
            row_pitch_out = layout.row_pitch;
            depth_pitch_out = layout.depth_pitch;
        } else {
            data_out = 0;
        }
        storage.mapped = true;
        storage.mapped_type = map_type;
        storage.cpu_dirty = false;
        d3d11_diag("ID3D11DeviceContext::Map DEFAULT texture=0x%llx sub=%u "
                   "type=%u layout=%u -> %s",
                   static_cast<unsigned long long>(resource), subresource,
                   map_type, t.layout, has_pointer ? "pointer" : "NULL");
        return d3d11::S_OK;
    }
    // A NULL pMappedResource maps only a DEFAULT texture (measured: STAGING
    // E_INVALIDARG, d3d11_texmap_probe "undef" 14/15).
    if (!want_pointer) return d3d11::E_INVALIDARG;
    const bool staging_map_ok =
        staging &&
        ((map_type == D3D11_MAP_READ &&
          (cpu & D3D11_CPU_ACCESS_READ) != 0) ||
         (map_type == D3D11_MAP_WRITE &&
          (cpu & D3D11_CPU_ACCESS_WRITE) != 0) ||
         (map_type == D3D11_MAP_READ_WRITE &&
          (cpu & D3D11_CPU_ACCESS_READ) != 0 &&
          (cpu & D3D11_CPU_ACCESS_WRITE) != 0));
    // Learn: WRITE_DISCARD is the writable mapping contract for a
    // dynamic texture. Mapping its top subresource invalidates the
    // previous contents; WRITE_NO_OVERWRITE is buffer-only.
    const bool dynamic_map_ok =
        dynamic && subresource == 0 &&
        cpu == D3D11_CPU_ACCESS_WRITE &&
        map_type == D3D11_MAP_WRITE_DISCARD;
    if (texture->second.device_owner != context_owner ||
        subresource >= subresource_count ||
        (map_flags != 0 &&
         (map_flags != D3D11_MAP_FLAG_DO_NOT_WAIT ||
          map_type == D3D11_MAP_WRITE_DISCARD)) ||
        (!staging_map_ok && !dynamic_map_ok)) {
        d3d11_diag(
            "ID3D11DeviceContext::Map invalid texture "
            "resource=0x%llx subresource=%u type=%u flags=0x%x",
            static_cast<unsigned long long>(resource),
            subresource, map_type, map_flags);
        return d3d11::E_INVALIDARG;
    }
    // Measured (d3d11_share_ers_probe "STAGING ... map="): a STAGING
    // resource opened from a shared handle does not Map -- E_OUTOFMEMORY --
    // on the opening device or in another process; only its creator maps
    // it. GPU copies into and out of the opening reach the shared memory.
    if (staging && texture->second.shared_opening) {
        return d3d11::E_OUTOFMEMORY;
    }
    // What the CPU sees: the shadow's bytes, or for a texture created with
    // 64K_STANDARD_SWIZZLE (or any layout past it) their swizzled image.
    const auto layout = mapped_image_layout(texture->second, subresource);
    if (layout.kind == MappedImageLayout::Kind::None) {
        d3d11_unimplemented_once(
            "Map", "texture layout " + std::to_string(texture->second.layout) +
                       " of DXGI format " +
                       std::to_string(texture->second.desc.format));
        return d3d11::E_INVALIDARG;
    }
    auto& storage = texture->second.subresources[subresource];
    if (storage.mapped) {
        // Measured (a06 probe M7): mapping a subresource that is already
        // mapped succeeds and hands back the same pointer; the guest's pages
        // already hold what it has read or written so far. A write access
        // widens what the one Unmap publishes.
        data_out = storage.mapped_address;
        row_pitch_out = layout.row_pitch;
        depth_pitch_out = layout.depth_pitch;
        if (map_type != D3D11_MAP_READ &&
            storage.mapped_type == D3D11_MAP_READ) {
            storage.mapped_type = map_type;
        }
        return d3d11::S_OK;
    }
    // Learn (Map, D3D11_MAP_FLAG_DO_NOT_WAIT): while a GPU -> staging copy
    // into the subresource has not executed, Map waits for it -- or, with
    // DO_NOT_WAIT, returns DXGI_ERROR_WAS_STILL_DRAWING (a06 gpu probe: DNW
    // texture). Polling submits the work, so the loop makes progress.
    if (!storage.pending.empty()) {
        if ((map_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0 &&
            !staging_readbacks_ready(texture->second, subresource)) {
            return kDxgiErrorWasStillDrawing;
        }
        resolve_staging_readbacks(texture->second, subresource);
    }
    if (texture->second.shared_memory) {
        // Shared STAGING memory: this device's GPU copies into or out of it
        // (DO_NOT_WAIT polls them, Learn Map) and what other devices and
        // processes published are complete before the CPU touches it.
        const u64 generation = texture->second.shared_memory_gen;
        if ((map_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0 &&
            generation != 0 && generation != ~0ull &&
            !m_backend.generation_complete(generation)) {
            if (generation >= m_backend.recording_generation()) {
                (void)m_backend.flush_completion();
            }
            return kDxgiErrorWasStillDrawing;
        }
        acquire_shared_staging(texture->second);
    }

    if (storage.mapped_address == 0) {
        // Whole host pages, host-page aligned: see the pointer path above.
        const usize mapped_size =
            static_cast<usize>(round_up_host_page(layout.size));
        GuestAddr address = m_memory.find_free_gap(
            m_next_map_hint, mapped_size, kLinearHostPageSize);
        if (address == 0) {
            address = m_memory.find_free_gap(0x10000000ull, mapped_size,
                                             kLinearHostPageSize);
        }
        if (address == 0 ||
            m_memory.map_region(
                address, mapped_size,
                MemoryProtection::ReadWrite).is_err()) {
            return 0x8007000Eu;
        }
        storage.mapped_address = address;
        storage.mapped_size = mapped_size;
        m_next_map_hint = address + mapped_size;
    }

    const bool expose_previous_contents =
        map_type != D3D11_MAP_WRITE_DISCARD;
    if (expose_previous_contents) {
        bool written = false;
        if (layout.kind == MappedImageLayout::Kind::Storage) {
            written = m_memory.write_bytes(
                storage.mapped_address,
                texture->second.shadow.data() + storage.offset,
                storage.size).is_ok();
        } else {
            std::vector<u8> image(layout.size);
            store_mapped_image(texture->second, subresource, layout,
                               image.data());
            written = m_memory.write_bytes(storage.mapped_address,
                                           image.data(), image.size())
                          .is_ok();
        }
        if (!written) return d3d11::E_INVALIDARG;
    }
    data_out = storage.mapped_address;
    row_pitch_out = layout.row_pitch;
    depth_pitch_out = layout.depth_pitch;
    storage.mapped = true;
    storage.mapped_type = map_type;
    d3d11_diag(
        "ID3D11DeviceContext::Map staging texture=0x%llx "
        "type=%u -> guest=0x%llx row=%u depth=%zu",
        static_cast<unsigned long long>(resource), map_type,
        static_cast<unsigned long long>(
            storage.mapped_address),
        storage.row_pitch, storage.size);
    return d3d11::S_OK;
}

// The texture half of ID3D11DeviceContext::Unmap, shared with
// IDXGISurface::Unmap.
void D3D11Manager::unmap_texture(GuestAddr resource, u32 subresource) {
    auto texture = m_textures.find(resource);
    if (texture == m_textures.end()) return;
    if (subresource >= texture->second.subresources.size() ||
        !texture->second.subresources[subresource].mapped) {
        d3d11_diag(
            "ID3D11DeviceContext::Unmap invalid texture "
            "resource=0x%llx subresource=%u",
            static_cast<unsigned long long>(resource), subresource);
        return;
    }
    auto& storage = texture->second.subresources[subresource];
    if (texture->second.desc.usage == D3D11_USAGE_DEFAULT &&
        texture->second.desc.cpu_access_flags != 0) {
        // Map on default: what a writable mapping wrote, through the pointer
        // or WriteToSubresource, is the texture's contents after the Unmap
        // (measured, d3d11_texmap_probe [data] "after write", "after pointer
        // write"); a READ mapping stores nothing -- a WriteToSubresource
        // into it is gone after the Unmap ("undef" 2/5/8/11). Only a changed
        // image goes back to the GPU.
        auto& t = texture->second;
        const auto layout = mapped_image_layout(t, subresource);
        const bool writable = storage.mapped_type != D3D11_MAP_READ;
        bool changed = writable && storage.cpu_dirty;
        if (writable && layout.kind != MappedImageLayout::Kind::None &&
            storage.mapped_address != 0) {
            std::vector<u8> image(layout.size);
            std::vector<u8> before(t.shadow.data() + storage.offset,
                                   t.shadow.data() + storage.offset +
                                       storage.size);
            if (m_memory.read_bytes(storage.mapped_address, image.data(),
                                    image.size()).is_ok()) {
                load_mapped_image(t, subresource, layout, image.data());
                changed = changed ||
                          std::memcmp(before.data(),
                                      t.shadow.data() + storage.offset,
                                      storage.size) != 0;
            }
        }
        const bool stored = !changed || write_default_subresource(t, subresource);
        if (!stored) {
            d3d11_unimplemented_once(
                "Unmap", "storing a DEFAULT texture of DXGI format " +
                             std::to_string(t.desc.format));
        }
        storage.mapped = false;
        storage.mapped_type = 0;
        storage.cpu_dirty = false;
        d3d11_diag("ID3D11DeviceContext::Unmap DEFAULT texture=0x%llx sub=%u "
                   "-> %s",
                   static_cast<unsigned long long>(resource), subresource,
                   !changed ? "unchanged" : stored ? "stored" : "failed");
        return;
    }
    const bool writable =
        storage.mapped_type == D3D11_MAP_WRITE ||
        storage.mapped_type == D3D11_MAP_READ_WRITE ||
        storage.mapped_type == D3D11_MAP_WRITE_DISCARD;
    bool copied = true;
    const auto image_layout =
        mapped_image_layout(texture->second, subresource);
    if (writable) {
        // Reuse the texture's staging buffer. Constructing a vector
        // here allocated and ZERO-FILLED storage.size bytes on every
        // single frame upload, only to overwrite all of it on the next
        // line. resize() never shrinks the capacity, so after the first
        // call this is free.
        // Read the guest's bytes STRAIGHT INTO the shadow and upload
        // from there. The previous form staged through map_scratch and
        // then memcpy'd scratch -> shadow, so every Unmap copied the
        // region three times (guest -> scratch, scratch -> shadow,
        // scratch -> GPU) when two copies do the same work: the scratch
        // and the shadow always ended up holding identical bytes.
        //
        // This is the hottest API in a live capture --
        // 26,298 Unmaps costing 6.76 s at 257 us each, the largest
        // single non-blocking cost in the profile -- and it is
        // memcpy-bound, so removing a whole copy of the region is the
        // win. `shadow` stays authoritative for CopyResource /
        // CopySubresourceRegion / UpdateSubresource, which read it.
        //
        // Bounds are checked rather than assumed: `offset + size`
        // indexing the shadow was previously implicit in the memcpy,
        // and a mismatched subresource layout would have written past
        // the buffer instead of failing.
        // Bounds FIRST: forming `shadow.data() + offset` is undefined
        // when the shadow is empty (data() may be null), which aborts
        // under hardening even though the pointer is never
        // dereferenced.
        const bool in_bounds =
            storage.offset <= texture->second.shadow.size() &&
            storage.size <=
                texture->second.shadow.size() - storage.offset;
        if (!in_bounds) {
            copied = false;
        } else {
            u8* const shadow_dst =
                texture->second.shadow.data() + storage.offset;
            // A partial read leaves the shadow partially updated where
            // the old form left it stale. Neither is meaningful: the
            // guest has just declared these bytes to be the texture's
            // contents, so if its own mapped range is unreadable the
            // contents are undefined either way. The failure is
            // reported below exactly as before.
            if (image_layout.kind == MappedImageLayout::Kind::Swizzled) {
                // A standard-swizzle mapping: its image back into the
                // shadow's rows.
                std::vector<u8> image(image_layout.size);
                copied = m_memory.read_bytes(storage.mapped_address,
                                             image.data(), image.size())
                             .is_ok();
                if (copied) {
                    load_mapped_image(texture->second, subresource,
                                      image_layout, image.data());
                }
            } else {
                copied = m_memory.read_bytes(storage.mapped_address,
                                             shadow_dst,
                                             storage.size).is_ok();
            }
            if (copied && texture->second.desc.usage ==
                    D3D11_USAGE_DYNAMIC) {
                const u32 mip =
                    subresource % texture->second.desc.mip_levels;
                const u32 slice =
                    subresource / texture->second.desc.mip_levels;
                if (texture->second.volume) {
                    copied = texture->second.backend_3d != nullptr &&
                        texture->second.backend_3d->update(
                            mip, shadow_dst, storage.row_pitch,
                            static_cast<u32>(storage.slice_pitch));
                } else {
                    // Learn (D3D11_MAP_WRITE_DISCARD): the old contents stay
                    // what the draws and dispatches recorded before this Map
                    // read (a06 gpu probe: WDS dynamic tex), so the upload
                    // goes to renamed storage, not over texels still queued.
                    if (storage.mapped_type == D3D11_MAP_WRITE_DISCARD &&
                        texture->second.backend != nullptr) {
                        (void)texture->second.backend->discard_storage();
                    }
                    copied = texture->second.backend != nullptr &&
                        texture->second.backend->update_slice(
                            mip, slice, shadow_dst, storage.row_pitch);
                }
            }
        }
    }
    // The guest's writes went straight into shared STAGING memory: other
    // devices see them after this device's next synchronization point.
    if (writable && copied && texture->second.shared_memory) {
        texture->second.shared_memory->note_written();
    }
    storage.mapped = false;
    storage.mapped_type = 0;
    d3d11_diag(
        "ID3D11DeviceContext::Unmap staging texture=0x%llx "
        "bytes=%zu -> %s",
        static_cast<unsigned long long>(resource),
        storage.size,
        copied ? "ok" : "failed");
}

} // namespace vitreo::winapi
