// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_texture_create.cpp
/// @brief Texture creation (2D and 3D) and the upload of initial subresource
///        data. Split out of d3d11.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

GuestAddr D3D11Manager::create_texture_2d(
    GuestAddr device_owner, const Texture2DDesc& desc,
    const void* initial_data, u32 initial_row_pitch,
    GuestAddr swap_chain_owner,
    std::unique_ptr<graphics::BackendTexture2D> opened_backend,
    u32 dimension,
    std::shared_ptr<graphics::BackendSharedMemory> opened_memory,
    u64 opened_nonce) {
    auto device = m_devices.find(device_owner);
    if (device == m_devices.end() || !ensure_storage()) {
        return 0;
    }

    const bool staging = desc.usage == D3D11_USAGE_STAGING;
    // A SHARED / KEYEDMUTEX resource can be opened by another device or
    // process: every opening is rebuilt from what the creator stores with
    // the shared storage (OpenSharedResource hands back the creator's
    // description, measured), identified by a nonce (storage ids recycle).
    const bool shared_flags =
        (desc.misc_flags & (D3D11_RESOURCE_MISC_SHARED |
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
    const bool opening = opened_backend != nullptr || opened_memory != nullptr;
    const u64 shared_nonce =
        opening ? opened_nonce : shared_flags ? new_shared_nonce() : 0u;
    std::unique_ptr<graphics::BackendTexture2D> backend_texture =
        std::move(opened_backend);
    if (!staging) {
        // Always create a Metal backend texture for GPU resources — including
        // DXGI swapchain back buffers. STAGING resources deliberately remain
        // CPU shadow storage and cross the GPU boundary only through Copy*.
        graphics::BackendTexture2DDesc backend_desc{};
        // A packed 4:2:2 video texture is its element format at half the
        // width: one host texel per pixel pair (dxgi_pixels_per_texel).
        backend_desc.width = desc.width / dxgi_pixels_per_texel(desc.format);
        backend_desc.height = desc.height;
        backend_desc.mip_levels = desc.mip_levels;
        backend_desc.cube =
            (desc.misc_flags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
        // Backend array_size remains the D3D face/slice count. The Metal
        // backend alone translates cube faces into native cube-array length.
        backend_desc.array_size = desc.array_size;
        backend_desc.pixel_format =
            backend_texture_format(desc.format, desc.bind_flags);
        // A shader view that swizzles (d3d11_format_caps.hpp,
        // view_swizzle_for) is a texture view like a format cast.
        const bool swizzled_reads =
            (desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
            d3d11::view_swizzle_for(
                desc.format,
                graphics::format_table(backend_desc.pixel_format).is_depth) !=
                d3d11::ViewSwizzle::Identity;
        // A packed video texture is read and written through its element
        // views, UNORM, UINT and R32_UINT alike (dxgi_video_view_plane).
        backend_desc.pixel_format_view =
            dxgi_format_is_typeless(desc.format) || swap_chain_owner != 0 ||
            swizzled_reads ||
            (dxgi_format_has_video_views(desc.format) &&
             !dxgi_format_is_planar(desc.format));
        backend_desc.render_target =
            (desc.bind_flags & D3D11_BIND_RENDER_TARGET) != 0;
        // A depth-format texture is a depth texture whatever it binds as: a
        // typeless or view-format depth resource with only SRV, or none (a
        // copy target), still allocates the depth format (measured legal on
        // Windows, d3d11_a02_create_matrix_probe [format x shape]).
        backend_desc.depth_stencil =
            (desc.bind_flags & D3D11_BIND_DEPTH_STENCIL) != 0 ||
            graphics::format_table(backend_desc.pixel_format).is_depth;
        backend_desc.shader_resource =
            (desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0;
        backend_desc.unordered_access =
            (desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        backend_desc.sample_count = desc.sample_count;
        // A tiled texture is address space whose tiles map into tile pools
        // (d3d11_tiled.cpp).
        backend_desc.tiled =
            (desc.misc_flags & D3D11_RESOURCE_MISC_TILED) != 0;
        // DEFAULT resources remain GPU-owned. UpdateSubresource and initial
        // data reach them through the backend's staging-buffer blit path;
        // forcing shared CPU-writable storage rejects compressed formats and
        // other texture shapes that are otherwise valid. Only DYNAMIC usage
        // exposes direct CPU writes through Map(WRITE_DISCARD).
        backend_desc.cpu_writable =
            desc.usage == D3D11_USAGE_DYNAMIC && swap_chain_owner == 0 &&
            !backend_desc.depth_stencil && desc.sample_count == 1;
        const auto swap_chain = m_swap_chains.find(swap_chain_owner);
        // Composition swap chains may be rebound to an HWND owned by another
        // guest process after their back buffer is created. Keep that buffer
        // IOSurface-backed so cross-process Present can hand the GPU resource
        // to the window owner without a synchronous CPU readback.
        backend_desc.shareable =
            (desc.misc_flags &
             (D3D11_RESOURCE_MISC_SHARED |
              D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0 ||
            (swap_chain != m_swap_chains.end() &&
             swap_chain->second.composition);
        if (backend_desc.shareable && !backend_texture) {
            backend_desc.shared_metadata = shared_texture_metadata(
                desc, shared_nonce != 0 ? shared_nonce : new_shared_nonce(),
                dimension);
        }
        if (!backend_texture) {
            backend_texture = m_backend.create_texture_2d(
                backend_desc, initial_data, initial_row_pitch);
        }
        if (!backend_texture || !backend_texture->valid()) return 0;
    }

    const GuestAddr address = allocate_object(m_texture_2d_vtable);
    if (address == 0) return 0;
    const GuestAddr dxgi_resource_interface = address + 0x20;
    const GuestAddr keyed_mutex_interface = address + 0x28;
    const GuestAddr dxgi_surface_interface = address + 0x30;
    const bool keyed =
        (desc.misc_flags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
    if (!write_pointer(dxgi_resource_interface, m_dxgi_resource_vtable) ||
        !write_pointer(dxgi_surface_interface, m_dxgi_surface_vtable) ||
        (keyed &&
         !write_pointer(keyed_mutex_interface, m_keyed_mutex_vtable))) {
        free_object(address);
        return 0;
    }
    std::unique_ptr<TiledState> tiled_state;
    if ((desc.misc_flags & D3D11_RESOURCE_MISC_TILED) != 0) {
        tiled_state = tiled_texture_state(desc, backend_texture.get());
        if (!tiled_state) {
            free_object(address);
            return 0;
        }
    }
    Texture2D texture{};
    texture.tiled = std::move(tiled_state);
    texture.interface_address = address;
    texture.dxgi_resource_interface = dxgi_resource_interface;
    texture.keyed_mutex_interface = keyed ? keyed_mutex_interface : 0;
    texture.dxgi_surface_interface = dxgi_surface_interface;
    texture.device_owner = device_owner;
    texture.swap_chain_owner = swap_chain_owner;
    texture.desc = desc;
    texture.backend = std::move(backend_texture);
    texture.linear = dimension == 2;  // D3D11_RESOURCE_DIMENSION_TEXTURE1D
    texture.shared_nonce = shared_nonce;
    if (texture.shared_nonce == 0 && texture.backend &&
        texture.backend->shared_handle() != 0) {
        // A composition swap chain's buffer: shared without SHARED flags.
        Texture2DDesc stored{};
        (void)parse_shared_texture_metadata(texture.backend->shared_metadata(),
                                            stored, texture.shared_nonce);
    }
    // A DEFAULT texture with CPU access (map on default) keeps the same
    // per-subresource shadow: the CPU image a Map reads back into and an
    // Unmap stores from (d3d11_context.cpp, map_texture).
    const bool cpu_mappable =
        staging || desc.usage == D3D11_USAGE_DYNAMIC ||
        (desc.usage == D3D11_USAGE_DEFAULT && desc.cpu_access_flags != 0);
    if (cpu_mappable) {
        // The storage format: identical to the view mapping except for
        // storage-only formats (R10G10B10_XR_BIAS_A2 is 32-bit opaque texels).
        const auto storage_format = d3d11::texture_storage_format(desc.format);
        const auto format = graphics::format_table(storage_format);
        const u32 block_bytes =
            graphics::compressed_block_bytes(storage_format);
        usize byte_count = 0;
        texture.subresources.reserve(
            static_cast<usize>(desc.mip_levels) * desc.array_size);
        for (u32 slice = 0; slice < desc.array_size; ++slice) {
            for (u32 mip = 0; mip < desc.mip_levels; ++mip) {
                Texture2D::SubresourceStorage storage{};
                storage.width = std::max(1u, desc.width >> mip);
                storage.height = std::max(1u, desc.height >> mip);
                const u32 storage_rows = format.is_compressed
                    ? (storage.height + 3u) / 4u
                    : dxgi_subresource_rows(desc.format, storage.height);
                // The guest-visible stride, which differs from the host
                // footprint for packed depth+stencil -- see
                // staging_bytes_per_pixel, measured on Windows.
                storage.row_pitch = format.is_compressed
                    ? ((storage.width + 3u) / 4u) * block_bytes
                    : storage.width * staging_bytes_per_pixel(
                                          desc.format, format.bytes_per_pixel);
                storage.offset = byte_count;
                storage.size = static_cast<usize>(storage.row_pitch) *
                               storage_rows;
                if (dxgi_format_is_planar(desc.format)) {
                    // Measured (d3d11_nv12_probe [layout]): Map reports the
                    // luma plane's size as DepthPitch (16x8 NV12: RowPitch
                    // 16, DepthPitch 128) though the subresource holds both
                    // planes.
                    storage.slice_pitch =
                        static_cast<usize>(storage.row_pitch) *
                        storage.height;
                }
                if (storage.size > std::numeric_limits<usize>::max() -
                                       byte_count) {
                    return 0;
                }
                byte_count += storage.size;
                texture.subresources.push_back(storage);
            }
        }
        texture.row_pitch = texture.subresources.front().row_pitch;
        if (staging && shared_flags) {
            // A shared STAGING resource: its bytes are shared memory every
            // opening maps (measured: GPU copies into and out of an opening
            // reach the creator's Map, d3d11_share_ers_probe).
            texture.shared_memory =
                opening ? std::move(opened_memory)
                        : m_backend.create_shared_memory(
                              byte_count,
                              shared_texture_metadata(desc, shared_nonce,
                                                      dimension));
            if (texture.shared_memory &&
                texture.shared_memory->size() >= byte_count) {
                texture.shadow.attach(texture.shared_memory->data(),
                                      byte_count);
            } else {
                texture.shared_memory.reset();
                d3d11_unimplemented_once(
                    "CreateTexture2D",
                    "shared STAGING resource without shared memory; created "
                    "unshared");
                texture.shadow.resize(byte_count);
            }
        } else {
            texture.shadow.resize(byte_count);
        }
        if (initial_data != nullptr) {
            const auto& storage = texture.subresources.front();
            const auto* source = static_cast<const u8*>(initial_data);
            const u32 storage_rows = format.is_compressed
                ? (storage.height + 3u) / 4u
                : dxgi_subresource_rows(desc.format, storage.height);
            for (u32 row = 0; row < storage_rows; ++row) {
                std::memcpy(
                    texture.shadow.data() + storage.offset +
                        static_cast<usize>(row) * storage.row_pitch,
                    source + static_cast<usize>(row) * initial_row_pitch,
                    storage.row_pitch);
            }
            if (texture.shared_memory) texture.shared_memory->note_written();
        }
    }
    if (keyed) attach_keyed_mutex(texture);
    if (swap_chain_owner != 0) {
        // A swap chain's buffer: the chain's own. The application has no
        // reference until GetBuffer, which then counts on the chain
        // (measured, a12 refmodel [swap]: GetBuffer's count is 1, the
        // chain's goes 1 -> 2, the device's does not move).
        texture.public_refs = 0;
        m_textures.emplace(address, std::move(texture));
        return address;
    }
    m_textures.emplace(address, std::move(texture));
    (void)add_ref(device->second.device_interface);
    return address;
}

GuestAddr D3D11Manager::create_texture_3d_object(
    GuestAddr device_owner, const Texture3DDesc& desc,
    std::unique_ptr<graphics::BackendTexture3D> backend,
    std::shared_ptr<graphics::BackendSharedMemory> memory, u64 nonce,
    bool opening) {
    auto device = m_devices.find(device_owner);
    if (device == m_devices.end() || !ensure_storage()) return 0;
    const bool staging = desc.usage == D3D11_USAGE_STAGING;
    // DEFAULT with CPU access: map on default (see create_texture_2d).
    const bool cpu_mappable =
        staging || desc.usage == D3D11_USAGE_DYNAMIC ||
        (desc.usage == D3D11_USAGE_DEFAULT && desc.cpu_access_flags != 0);
    if (!staging && !backend) return 0;
    const auto pixel_format = d3d11::texture_storage_format(desc.format);
    const auto format = graphics::format_table(pixel_format);
    // Map/Unmap and CopyResource address a CPU-mappable volume through
    // per-mip subresource storage: RowPitch, DepthPitch (one W slice) and
    // the stacked size, laid out like the 2D staging path.
    std::vector<Texture2D::SubresourceStorage> volume_storage;
    usize byte_count = 0;
    if (cpu_mappable) {
        const u32 block_bytes = graphics::compressed_block_bytes(pixel_format);
        for (u32 mip = 0; mip < desc.mip_levels; ++mip) {
            Texture2D::SubresourceStorage storage{};
            storage.width = std::max(1u, desc.width >> mip);
            storage.height = std::max(1u, desc.height >> mip);
            storage.depth = std::max(1u, desc.depth >> mip);
            const u32 rows = format.is_compressed ? (storage.height + 3u) / 4u
                                                  : storage.height;
            storage.row_pitch = format.is_compressed
                ? ((storage.width + 3u) / 4u) * block_bytes
                : storage.width * format.bytes_per_pixel;
            storage.slice_pitch = static_cast<usize>(storage.row_pitch) * rows;
            storage.size = storage.slice_pitch * storage.depth;
            storage.offset = byte_count;
            byte_count += storage.size;
            volume_storage.push_back(storage);
        }
    }
    const GuestAddr address = allocate_object(m_texture_2d_vtable);
    if (address == 0) return 0;
    // Every resource is a DXGI resource (IDXGIResource at object + 0x20,
    // measured on a Texture3D opening), and a KEYEDMUTEX one answers
    // IDXGIKeyedMutex (object + 0x28) -- the texture layout.
    const bool keyed =
        (desc.misc_flags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
    if (!write_pointer(address + 0x20, m_dxgi_resource_vtable) ||
        (keyed && !write_pointer(address + 0x28, m_keyed_mutex_vtable))) {
        free_object(address);
        return 0;
    }
    Texture2D texture{};
    texture.interface_address = address;
    texture.dxgi_resource_interface = address + 0x20;
    texture.keyed_mutex_interface = keyed ? address + 0x28 : 0;
    texture.device_owner = device_owner;
    texture.volume = true;
    texture.volume_desc = desc;
    texture.desc.width = desc.width;
    texture.desc.height = desc.height;
    texture.desc.mip_levels = desc.mip_levels;
    texture.desc.array_size = 1;
    texture.desc.format = desc.format;
    texture.desc.usage = desc.usage;
    texture.desc.bind_flags = desc.bind_flags;
    texture.desc.cpu_access_flags = desc.cpu_access_flags;
    texture.desc.misc_flags = desc.misc_flags;
    texture.subresources = std::move(volume_storage);
    texture.shared_nonce = nonce;
    texture.shared_opening = opening;
    const bool shared =
        (desc.misc_flags & (D3D11_RESOURCE_MISC_SHARED |
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
    if (cpu_mappable) {
        if (staging && shared) {
            if (!memory) {
                SharedResourceDesc metadata{};
                metadata.dimension = 4;  // D3D11_RESOURCE_DIMENSION_TEXTURE3D
                metadata.texture = texture.desc;
                metadata.depth = desc.depth;
                metadata.nonce = nonce;
                memory = m_backend.create_shared_memory(
                    byte_count, shared_resource_metadata(metadata));
            }
            if (memory && memory->size() >= byte_count) {
                texture.shared_memory = std::move(memory);
                texture.shadow.attach(texture.shared_memory->data(),
                                      byte_count);
            }
        }
        if (!texture.shadow.external()) texture.shadow.resize(byte_count);
        if (!texture.subresources.empty()) {
            texture.row_pitch = texture.subresources.front().row_pitch;
        }
    }
    texture.backend_3d = std::move(backend);
    if (keyed) attach_keyed_mutex(texture);
    m_textures.emplace(address, std::move(texture));
    (void)add_ref(device->second.device_interface);
    return address;
}

bool D3D11Manager::read_subresource_data(GuestAddr initial_data, u32 index,
                                         usize row_bytes, u32 rows,
                                         u32 depth, bool uses_row_pitch,
                                         bool uses_slice_pitch,
                                         std::vector<u8>& tight) const {
    // D3D11_SUBRESOURCE_DATA {const void* pSysMem; UINT SysMemPitch;
    // UINT SysMemSlicePitch;}: a pointer and two UINTs, 12 bytes on x86 and
    // 16 on x64 (d3d11.h). The array holds one record per subresource,
    // mip-major within each array slice.
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    const GuestAddr record =
        initial_data + static_cast<GuestAddr>(index) * (pointer_size + 8u);
    GuestAddr source = 0;
    const auto row_pitch = m_memory.read_u32(record + pointer_size);
    const auto slice_pitch = m_memory.read_u32(record + pointer_size + 4u);
    if (!read_pointer(record, source) || source == 0 ||
        row_pitch.is_err() || slice_pitch.is_err() || rows == 0 ||
        depth == 0 || row_bytes == 0) {
        return false;
    }
    // Measured: SysMemPitch 0 is E_INVALIDARG for a 2D/3D subresource (even
    // one row), SysMemSlicePitch 0 for a 3D one (even one slice); a 1D
    // texture reads neither.
    const usize pitch = uses_row_pitch ? row_pitch.value() : row_bytes;
    const usize slice = uses_slice_pitch
        ? slice_pitch.value()
        : pitch * rows;
    if (pitch == 0 || slice == 0) return false;
    const usize tight_slice = row_bytes * rows;
    if (tight_slice / rows != row_bytes ||
        tight_slice > std::numeric_limits<usize>::max() / depth) {
        return false;
    }
    tight.assign(tight_slice * depth, 0);
    std::vector<u8> row(row_bytes);
    for (u32 z = 0; z < depth; ++z) {
        for (u32 y = 0; y < rows; ++y) {
            const GuestAddr address = source +
                static_cast<GuestAddr>(z) * slice +
                static_cast<GuestAddr>(y) * pitch;
            if (m_memory.read_bytes(address, tight.data() + z * tight_slice +
                                                 y * row_bytes,
                                    row_bytes).is_err()) {
                return false;
            }
        }
    }
    return true;
}

bool D3D11Manager::upload_initial_subresource(Texture2D& texture, u32 mip,
                                              u32 slice, const u8* data,
                                              usize row_bytes, u32 rows) {
    if (texture.desc.usage == D3D11_USAGE_STAGING) {
        // A staging texture is CPU memory in the guest-visible layout.
        const usize index =
            static_cast<usize>(slice) * texture.desc.mip_levels + mip;
        if (index >= texture.subresources.size()) return false;
        const auto& storage = texture.subresources[index];
        if (storage.row_pitch < row_bytes) return false;
        for (u32 row = 0; row < rows; ++row) {
            std::memcpy(texture.shadow.data() + storage.offset +
                            static_cast<usize>(row) * storage.row_pitch,
                        data + static_cast<usize>(row) * row_bytes,
                        row_bytes);
        }
        return true;
    }
    if (texture.backend == nullptr) return false;
    if (is_packed_depth_stencil(texture.desc.format)) {
        return write_packed_depth_stencil(texture, mip, slice, data,
                                          row_bytes);
    }
    return texture.backend->update_slice(mip, slice, data,
                                         static_cast<u32>(row_bytes));
}

} // namespace vitreo::winapi
