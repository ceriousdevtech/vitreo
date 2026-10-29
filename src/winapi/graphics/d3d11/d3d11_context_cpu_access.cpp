// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_cpu_access.cpp
/// @brief ID3D11DeviceContext's CPU-side data paths.
///
/// Slots 48 and 116 (UpdateSubresource[1]), 14 (Map) and 15 (Unmap).
/// impl_ContextMethod (d3d11_context.cpp) routes exactly these slots here.
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

void D3D11Manager::context_cpu_access_method(Marshaler& m, usize slot,
                                             GuestAddr self) {
    switch (slot) {
    // Slot 116 is ID3D11DeviceContext1::UpdateSubresource1: the same six
    // arguments plus CopyFlags, ignored per the Learn note above. Its one real
    // behavioural difference from slot 48 is that pDstBox does not offset
    // pSrcData when the call is recorded into a software command list, and this
    // frontend has no deferred contexts for that to be observable through.
    case 116:
    case 48: {
        // UpdateSubresource — Learn IDL slot 48 (was miswired at 47).
        // UpdateSubresource1 (116) adds CopyFlags: measured
        // (d3d11_fl111_probe "copyflags"), only 0, D3D11_COPY_NO_OVERWRITE
        // and D3D11_COPY_DISCARD update; anything else drops the call.
        if (slot == 116 && static_cast<u32>(m.arg(7)) > 0x2u) {
            d3d11_diag("UpdateSubresource1 CopyFlags 0x%x -> dropped",
                       static_cast<u32>(m.arg(7)));
            m.set_return(0);
            return;
        }
        const GuestAddr resource = m.arg_ptr(1);
        const u32 subresource = static_cast<u32>(m.arg(2));
        const GuestAddr box = m.arg_ptr(3);
        const GuestAddr data = m.arg_ptr(4);
        const u32 row_pitch = static_cast<u32>(m.arg(5));
        auto buffer = m_buffers.find(resource);
        if (buffer != m_buffers.end()) {
            // Learn UpdateSubresource(1), pDstBox: "Coordinates are in bytes
            // for buffers"; top/front 0 and bottom/back 1. What Windows drops
            // (tools/oracle_probe/d3d11_a06_resource_ops_probe.expected, U2-U9):
            //   * DYNAMIC, IMMUTABLE and STAGING destinations (Learn: "cannot
            //     be used as a destination");
            //   * a constant buffer with a box, except UpdateSubresource1 with
            //     D3D11_COPY_NO_OVERWRITE or DISCARD and 16-byte-aligned
            //     extents (D3D11.3 5.3.4.3.1);
            //   * a structured-buffer box that splits a structure (5.6.8);
            //   * an empty box, or one outside the buffer (undefined; WARP
            //     removes the device, dropping is the safe outcome).
            const auto& bdesc = buffer->second.desc;
            u32 left = 0, right = bdesc.byte_width;
            bool apply = subresource == 0 && data != 0 &&
                         buffer->second.backend != nullptr &&
                         bdesc.usage == D3D11_USAGE_DEFAULT;
            if (apply && box != 0) {
                std::array<u32, 6> region{};
                for (usize index = 0; apply && index < region.size();
                     ++index) {
                    const auto value =
                        m_memory.read_u32(box + index * sizeof(u32));
                    if (value.is_err()) apply = false;
                    else region[index] = value.value();
                }
                left = region[0];
                right = region[3];
                const bool is_cb =
                    (bdesc.bind_flags & D3D11_BIND_CONSTANT_BUFFER) != 0;
                const u32 copy_flags =
                    slot == 116 ? static_cast<u32>(m.arg(7)) : 0u;
                constexpr u32 kCopyNoOverwrite = 0x1u;  // D3D11_COPY_NO_OVERWRITE
                constexpr u32 kCopyDiscard = 0x2u;      // D3D11_COPY_DISCARD
                const u32 stride =
                    (bdesc.misc_flags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
                        ? bdesc.structure_byte_stride : 0u;
                apply = apply && left < right && right <= bdesc.byte_width &&
                        region[1] == 0 && region[4] == 1 && region[2] == 0 &&
                        region[5] == 1 &&
                        (!is_cb ||
                         ((copy_flags & (kCopyNoOverwrite | kCopyDiscard)) &&
                          left % 16 == 0 && right % 16 == 0)) &&
                        (stride == 0 ||
                         (left % stride == 0 && right % stride == 0));
            }
            if (!apply) {
                d3d11_diag("ID3D11DeviceContext::UpdateSubresource "
                           "buffer=0x%llx dropped (usage=%u box=%s)",
                           static_cast<unsigned long long>(resource),
                           bdesc.usage, box != 0 ? "yes" : "no");
                m.set_return(0);
                return;
            }
            std::vector<u8> bytes(right - left);
            if (m_memory.read_bytes(data, bytes.data(), bytes.size()).is_ok()) {
                // Ordered in the command stream (the backend renames, or for
                // GPU-written contents queues a copy), so work recorded
                // before this keeps the old bytes. UpdateSubresource1's
                // CopyFlags (Learn D3D11_COPY_FLAGS): NO_OVERWRITE "The
                // existing contents of the resource cannot be overwritten" --
                // the application's promise, written in place; DISCARD "The
                // existing contents ... are undefined and can be discarded".
                const u32 update_flags =
                    slot == 116 ? static_cast<u32>(m.arg(7)) : 0u;
                const graphics::BufferWrite mode =
                    (update_flags & 0x2u) != 0  // D3D11_COPY_DISCARD
                        ? graphics::BufferWrite::Discard
                    : (update_flags & 0x1u) != 0  // D3D11_COPY_NO_OVERWRITE
                        ? graphics::BufferWrite::NoOverwrite
                        : graphics::BufferWrite::Ordered;
                const bool ok = buffer->second.backend->update(
                    left, bytes.data(), bytes.size(), mode);
                // Over GPU-written contents the update is a copy queued
                // after that work: a Map(READ) must wait for it as well.
                if (buffer->second.gpu_newer) note_gpu_use(buffer->second);
                // A tiled buffer keeps no shadow (d3d11_tiled.cpp).
                if (!buffer->second.tiled) {
                    if (buffer->second.shadow.size() < bdesc.byte_width) {
                        buffer->second.shadow.resize(bdesc.byte_width);
                    }
                    std::memcpy(buffer->second.shadow.data() + left,
                                bytes.data(), bytes.size());
                    ++buffer->second.shadow_version;
                }
                if (buffer->second.desc.byte_width <= 256 &&
                    bytes.size() >= sizeof(float) * 4) {
                    const auto* f =
                        reinterpret_cast<const float*>(bytes.data());
                    d3d11_diag(
                        "ID3D11DeviceContext::UpdateSubresource "
                        "buffer=0x%llx bytes=%u -> %s "
                        "float4=(%g,%g,%g,%g)",
                        static_cast<unsigned long long>(resource),
                        buffer->second.desc.byte_width,
                        ok ? "ok" : "failed",
                        static_cast<double>(f[0]),
                        static_cast<double>(f[1]),
                        static_cast<double>(f[2]),
                        static_cast<double>(f[3]));
                } else {
                    d3d11_diag(
                        "ID3D11DeviceContext::UpdateSubresource "
                        "buffer=0x%llx bytes=%u -> %s",
                        static_cast<unsigned long long>(resource),
                        buffer->second.desc.byte_width,
                        ok ? "ok" : "failed");
                }
            }
            m.set_return(0);
            return;
        }
        auto texture = m_textures.find(resource);
        if (m_diag_transfer_log != nullptr && texture != m_textures.end())
            [[unlikely]] {
            diag_log_large_texture_write(
                slot == 116 ? "UpdateSubresource1" : "UpdateSubresource",
                resource, subresource, 0, 0, 0, box, 0, 0, 0, row_pitch,
                static_cast<u32>(m.arg(6)),
                data == 0 ? "no data" : "called");
        }
        if (texture != m_textures.end() && data != 0 &&
            (texture->second.backend || texture->second.backend_3d)) {
            const auto& tdesc = texture->second.desc;
            // Learn (UpdateSubresource): a resource "created with
            // D3D11_USAGE_DYNAMIC or D3D11_USAGE_IMMUTABLE cannot be used as a
            // destination"; Windows drops those and STAGING (a06 probe U10,
            // U11).
            if (tdesc.usage != D3D11_USAGE_DEFAULT) {
                d3d11_diag("ID3D11DeviceContext::UpdateSubresource "
                           "texture=0x%llx usage=%u -> dropped",
                           static_cast<unsigned long long>(resource),
                           tdesc.usage);
                m.set_return(0);
                return;
            }
            const auto upload_format =
                graphics::format_table(backend_format(tdesc.format));
            if (texture->second.volume || upload_format.is_compressed) {
                (void)update_texture_box(texture->second, subresource, box,
                                         data, row_pitch,
                                         static_cast<u32>(m.arg(6)));
                m.set_return(0);
                return;
            }
            const u32 mip_level = subresource % tdesc.mip_levels;
            const u32 array_slice = subresource / tdesc.mip_levels;
            if (array_slice >= tdesc.array_size) {
                m.set_return(0);
                return;
            }
            const auto fmt = graphics::format_table(
                backend_format(tdesc.format));
            if (fmt.bytes_per_pixel != 0 && !fmt.is_compressed) {
                const u32 level_width =
                    std::max(1u, tdesc.width >> mip_level);
                const u32 level_height =
                    std::max(1u, tdesc.height >> mip_level);
                std::array<u32, 6> region{
                    0, 0, 0, level_width, level_height, 1,
                };
                bool region_readable = true;
                if (box != 0) {
                    for (usize index = 0; index < region.size(); ++index) {
                        const auto value = m_memory.read_u32(
                            box + index * sizeof(u32));
                        if (value.is_err()) {
                            region_readable = false;
                            break;
                        }
                        region[index] = value.value();
                    }
                }
                if (!region_readable) {
                    m.set_return(0);
                    return;
                }
                const u32 left = region[0];
                const u32 top = region[1];
                const u32 front = region[2];
                const u32 right = region[3];
                const u32 bottom = region[4];
                const u32 back = region[5];
                if (left >= right || top >= bottom || front >= back) {
                    m.set_return(0);
                    return;
                }
                const u32 width = right - left;
                const u32 height = bottom - top;
                // The guest's pixel (half a host texel of a packed 4:2:2
                // format, staging_bytes_per_pixel).
                const usize packed_pitch =
                    static_cast<usize>(width) *
                    staging_bytes_per_pixel(tdesc.format,
                                            fmt.bytes_per_pixel);
                // A packed 4:2:2 box covers whole pixel pairs: one with an
                // odd left or right edge writes nothing (measured,
                // d3d11_video_formats_probe [layout] "box1,1-5,3",
                // "box0,2-7,4"). The host texture addresses pairs.
                const u32 ppt = dxgi_pixels_per_texel(tdesc.format);
                if (((left | right) % ppt) != 0) {
                    d3d11_diag("ID3D11DeviceContext::UpdateSubresource "
                               "texture=0x%llx box=(%u,%u)-(%u,%u) splits a "
                               "4:2:2 pixel pair -> dropped",
                               static_cast<unsigned long long>(resource),
                               left, top, right, bottom);
                    m.set_return(0);
                    return;
                }
                const u32 pitch =
                    row_pitch != 0
                        ? row_pitch
                        : static_cast<u32>(packed_pitch);
                const bool valid_region =
                    front == 0 && back == 1 && right <= level_width &&
                    bottom <= level_height && pitch >= packed_pitch &&
                    static_cast<usize>(pitch) <=
                        std::numeric_limits<usize>::max() / height;
                if (!valid_region) {
                    m.set_return(0);
                    return;
                }
                if (dxgi_format_is_planar(tdesc.format)) {
                    // Planar video: the box is in luma texels and takes the
                    // chroma samples it covers; pSrcData holds the box's
                    // luma rows, then its chroma rows, at SrcRowPitch.
                    // Measured (d3d11_nv12_probe [layout] "box"): a box
                    // with an odd edge writes nothing.
                    const bool even =
                        ((left | top | right | bottom) & 1u) == 0;
                    const u32 rows = dxgi_subresource_rows(tdesc.format,
                                                           height);
                    std::vector<u8> planes(static_cast<usize>(pitch) * rows);
                    const bool ok =
                        even &&
                        m_memory
                            .read_bytes(data, planes.data(),
                                        static_cast<usize>(pitch) *
                                                (rows - 1u) +
                                            packed_pitch)
                            .is_ok() &&
                        texture->second.backend->update_region(
                            mip_level, array_slice, planes.data(), pitch,
                            left, top, width, height);
                    d3d11_diag(
                        "ID3D11DeviceContext::UpdateSubresource planar "
                        "texture=0x%llx box=(%u,%u)-(%u,%u) -> %s",
                        static_cast<unsigned long long>(resource), left, top,
                        right, bottom, ok ? "ok" : "dropped");
                    m.set_return(0);
                    return;
                }
                const usize nbytes = static_cast<usize>(pitch) * height;
                std::vector<u8> bytes(nbytes);
                // Only what the box covers: the last row ends after its
                // texels, not at the next row's pitch (reading a full pitch
                // there ran past the caller's buffer).
                const usize needed =
                    static_cast<usize>(pitch) * (height - 1) + packed_pitch;
                if (m_memory.read_bytes(data, bytes.data(), needed).is_ok()) {
                    const bool ok = texture->second.backend->update_region(
                        mip_level, array_slice, bytes.data(), pitch,
                        left / ppt, top, width / ppt, height);
                    if (ok && subresource <
                                  texture->second.subresources.size()) {
                        const auto& storage =
                            texture->second.subresources[subresource];
                        if (storage.row_pitch >=
                                static_cast<usize>(level_width) *
                                    fmt.bytes_per_pixel &&
                            storage.offset + storage.size <=
                                texture->second.shadow.size()) {
                            for (u32 row = 0; row < height; ++row) {
                                std::memcpy(
                                    texture->second.shadow.data() +
                                        storage.offset +
                                        static_cast<usize>(top + row) *
                                            storage.row_pitch +
                                        static_cast<usize>(left) *
                                            fmt.bytes_per_pixel,
                                    bytes.data() +
                                        static_cast<usize>(row) * pitch,
                                    packed_pitch);
                            }
                        }
                    }
                    d3d11_diag(
                        "ID3D11DeviceContext::UpdateSubresource "
                        "texture=0x%llx subresource=%u "
                        "box=(%u,%u)-(%u,%u) -> %s",
                        static_cast<unsigned long long>(resource),
                        subresource, left, top, right, bottom,
                        ok ? "ok" : "failed");
                }
            }
            m.set_return(0);
            return;
        }
        m.set_return(0);
        return;
    }
    case 14: {
        const GuestAddr resource = m.arg_ptr(1);
        const u32 subresource = static_cast<u32>(m.arg(2));
        const u32 map_type = static_cast<u32>(m.arg(3));
        const u32 map_flags = static_cast<u32>(m.arg(4));
        const GuestAddr mapped_out = m.arg_ptr(5);
        const GuestAddr context_owner =
            m_interfaces.find(self)->second.owner;
        if (m_textures.find(resource) != m_textures.end()) {
            // A NULL pMappedResource maps a DEFAULT texture with CPU access
            // (Learn Map, "NULL pointers for pMappedResource"); map_texture
            // refuses it for every other texture.
            GuestAddr data = 0;
            u32 row_pitch = 0;
            u32 depth_pitch = 0;
            const u32 hr = map_texture(resource, subresource, map_type,
                                       map_flags, context_owner, data,
                                       row_pitch, depth_pitch,
                                       mapped_out != 0);
            if (m_diag_transfer_log != nullptr) [[unlikely]] {
                diag_log_staging_map(resource, subresource, map_type,
                                     map_flags, hr, data, row_pitch);
            }
            if (mapped_out == 0) {
                m.set_return(hr);
                return;
            }
            if (hr != d3d11::S_OK) {
                // Measured (d3d11_buffer_map_pitch_probe.expected): a
                // rejected Map sets pData to NULL and leaves both pitches.
                (void)write_pointer(mapped_out, 0);
            } else {
                const usize pointer_size = m_is_64bit ? 8u : 4u;
                if (!write_pointer(mapped_out, data) ||
                    m_memory.write_u32(mapped_out + pointer_size, row_pitch)
                        .is_err() ||
                    m_memory.write_u32(mapped_out + pointer_size + 4u,
                                       depth_pitch).is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
            }
            m.set_return(hr);
            return;
        }

        if (m_buffers.find(resource) == m_buffers.end() || mapped_out == 0) {
            d3d11_diag("ID3D11DeviceContext::Map invalid resource=0x%llx "
                       "out=0x%llx",
                       static_cast<unsigned long long>(resource),
                       static_cast<unsigned long long>(mapped_out));
            // Measured (d3d11_buffer_map_pitch_probe.expected): a rejected
            // Map sets pData to NULL and leaves RowPitch/DepthPitch alone.
            if (mapped_out != 0) (void)write_pointer(mapped_out, 0);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        GuestAddr data = 0;
        u32 pitch = 0;
        const u32 hr = map_buffer(resource, subresource, map_type, map_flags,
                                  context_owner, data, pitch);
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_staging_map(resource, subresource, map_type, map_flags,
                                 hr, data, pitch);
        }
        if (hr != d3d11::S_OK) {
            (void)write_pointer(mapped_out, 0);
            m.set_return(hr);
            return;
        }
        if (!write_pointer(mapped_out, data) ||
            m_memory.write_u32(
                mapped_out + (m_is_64bit ? 8u : 4u), pitch).is_err() ||
            m_memory.write_u32(
                mapped_out + (m_is_64bit ? 12u : 8u), pitch).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 15: {
        const GuestAddr resource = m.arg_ptr(1);
        const u32 subresource = static_cast<u32>(m.arg(2));
        if (m_textures.find(resource) != m_textures.end()) {
            unmap_texture(resource, subresource);
            m.set_return(0);
            return;
        }
        unmap_buffer(resource, subresource);
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
