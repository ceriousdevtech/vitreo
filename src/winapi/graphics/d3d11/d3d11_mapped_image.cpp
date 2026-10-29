// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_mapped_image.cpp
/// @brief How a Map image is laid out and copied.
///
/// The layout of a texture subresource's mapping image, the copies between
/// that image and the subresource's shadow, the GPU readback and upload of a
/// DEFAULT texture behind a Map, and ID3D11Device3::WriteToSubresource /
/// ReadFromSubresource, which address the same image.
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

namespace {

// Additive offset tables of a mapping image: the image offset of shadow unit
// (x, y, z) is x_offsets[x] + y_offsets[y] + z_offsets[z]. A standard-swizzle
// page index is linear in the page coordinates and the three interleave masks
// are disjoint, so the swizzled offset splits the same way as a pitched one.
struct ImageOffsets {
    std::vector<u64> x, y, z;
};

}  // namespace

D3D11Manager::MappedImageLayout D3D11Manager::mapped_image_layout(
    const Texture2D& texture, u32 subresource) const {
    MappedImageLayout out{};
    if (subresource >= texture.subresources.size()) return out;
    const auto& storage = texture.subresources[subresource];
    const u32 format = texture.desc.format;
    const auto info =
        graphics::format_table(d3d11::texture_storage_format(format));
    const bool planar = dxgi_format_is_planar(format);
    const bool map_on_default = texture.desc.usage == D3D11_USAGE_DEFAULT;
    constexpr u32 ROW_MAJOR = 1;
    constexpr u32 STANDARD_SWIZZLE_64K = 2;
    // The subresource in elements: a 4x4 block, a packed 4:2:2 pixel pair,
    // or a texel (d3d11_texmap_probe [swz] f107/f68: a YUY2 / R8G8_B8G8
    // element is its 32-bit pair).
    const u32 rows = info.is_compressed
        ? (storage.height + 3u) / 4u
        : dxgi_subresource_rows(format, storage.height);
    const u32 pixels_per_element =
        info.is_compressed ? 4u : dxgi_pixels_per_texel(format);
    out.elements_w =
        (storage.width + pixels_per_element - 1u) / pixels_per_element;
    out.elements_h = rows;
    out.depth = std::max(1u, storage.depth);
    out.element_bytes =
        out.elements_w != 0 ? storage.row_pitch / out.elements_w : 0u;
    bool swizzled = false;
    bool pitched = false;
    if (map_on_default) {
        // Measured (d3d11_texmap_probe [map], [pitch]): a DEFAULT texture
        // hands the CPU a pointer only through ROW_MAJOR or
        // 64K_STANDARD_SWIZZLE, or as a Texture1D (rows padded to 4 bytes
        // like ROW_MAJOR's); planar video (NV12, P010) never maps.
        if (planar) return out;
        if (texture.linear || texture.layout == ROW_MAJOR) {
            pitched = true;
        } else if (texture.layout == STANDARD_SWIZZLE_64K) {
            swizzled = true;
        } else {
            return out;
        }
    } else if (texture.layout >= STANDARD_SWIZZLE_64K) {
        // A STAGING or DYNAMIC texture created with 64K_STANDARD_SWIZZLE --
        // or any value past it -- maps swizzled (measured, [other] "O
        // 2D1L2/2D1L3 u2/u3": RowPitch 65536, the GPU copy de-swizzles).
        if (planar) return out;
        swizzled = true;
    }
    if (swizzled) {
        if (!d3d11_swizzle::layout_for(out.element_bytes, texture.volume,
                                       out.elements_w, out.elements_h,
                                       out.depth, out.swizzle)) {
            return out;
        }
        out.kind = MappedImageLayout::Kind::Swizzled;
        out.row_pitch = static_cast<u32>(out.swizzle.row_pitch());
        out.depth_pitch = static_cast<u32>(out.swizzle.depth_pitch());
        out.size = static_cast<usize>(out.swizzle.size());
        return out;
    }
    if (pitched) {
        // Measured: RowPitch is the row's bytes rounded up to 4 (R8 129
        // wide: 132; R8G8 3 wide: 8), DepthPitch RowPitch x rows (x blocks
        // rows for BC), one DepthPitch per W slice.
        out.kind = MappedImageLayout::Kind::Pitched;
        out.row_pitch = (storage.row_pitch + 3u) & ~3u;
        out.depth_pitch = out.row_pitch * rows;
        out.size = static_cast<usize>(out.depth_pitch) * out.depth;
        return out;
    }
    out.kind = MappedImageLayout::Kind::Storage;
    out.row_pitch = storage.row_pitch;
    out.depth_pitch = static_cast<u32>(
        storage.slice_pitch != 0 ? storage.slice_pitch : storage.size);
    out.size = storage.size;
    return out;
}

namespace {

// The shadow is addressed in units of the guest's own layout: a block, or a
// pixel of staging_bytes_per_pixel (half a packed 4:2:2 pair). A unit's image
// offset for a swizzled layout is its element's offset plus its byte within
// the element.
ImageOffsets image_offsets(const D3D11MappedImageLayout& layout,
                           u32 unit_bytes, u32 units_w, u32 rows, u32 depth) {
    ImageOffsets o;
    o.x.resize(units_w);
    o.y.resize(rows);
    o.z.resize(depth);
    using Kind = D3D11MappedImageLayout::Kind;
    if (layout.kind == Kind::Swizzled) {
        const auto& sw = layout.swizzle;
        for (u32 x = 0; x < units_w; ++x) {
            const u32 byte = x * unit_bytes;
            o.x[x] = sw.offset(byte / sw.element_bytes, 0, 0) +
                     byte % sw.element_bytes;
        }
        for (u32 y = 0; y < rows; ++y) o.y[y] = sw.offset(0, y, 0);
        for (u32 z = 0; z < depth; ++z) o.z[z] = sw.offset(0, 0, z);
    } else {
        for (u32 x = 0; x < units_w; ++x) {
            o.x[x] = static_cast<u64>(x) * unit_bytes;
        }
        for (u32 y = 0; y < rows; ++y) {
            o.y[y] = static_cast<u64>(y) * layout.row_pitch;
        }
        for (u32 z = 0; z < depth; ++z) {
            o.z[z] = static_cast<u64>(z) * layout.depth_pitch;
        }
    }
    return o;
}

// Unit size of the shadow rows of @p texture: a block, or a pixel.
u32 shadow_unit_bytes(u32 format, const D3D11MappedImageLayout& l) {
    const u32 ppt = dxgi_pixels_per_texel(format);
    return ppt > 1u ? l.element_bytes / ppt : l.element_bytes;
}

}  // namespace

void D3D11Manager::store_mapped_image(const Texture2D& texture,
                                      u32 subresource,
                                      const MappedImageLayout& layout,
                                      u8* image) const {
    const auto& storage = texture.subresources[subresource];
    const u8* shadow = texture.shadow.data() + storage.offset;
    if (layout.kind == MappedImageLayout::Kind::Storage) {
        std::memcpy(image, shadow, storage.size);
        return;
    }
    // Bytes past the texels (row padding, the rest of a partial page) read 0.
    std::memset(image, 0, layout.size);
    const u32 unit = shadow_unit_bytes(texture.desc.format, layout);
    if (unit == 0) return;
    const u32 units_w = storage.row_pitch / unit;
    const auto o = image_offsets(layout, unit, units_w, layout.elements_h,
                                 layout.depth);
    const usize slice_pitch = storage.slice_pitch != 0
        ? storage.slice_pitch
        : static_cast<usize>(storage.row_pitch) * layout.elements_h;
    for (u32 z = 0; z < layout.depth; ++z) {
        for (u32 y = 0; y < layout.elements_h; ++y) {
            const u8* row = shadow + z * slice_pitch +
                            static_cast<usize>(y) * storage.row_pitch;
            u8* base = image + o.z[z] + o.y[y];
            if (layout.kind == MappedImageLayout::Kind::Pitched) {
                std::memcpy(base, row, storage.row_pitch);
                continue;
            }
            for (u32 x = 0; x < units_w; ++x) {
                std::memcpy(base + o.x[x], row + static_cast<usize>(x) * unit,
                            unit);
            }
        }
    }
}

void D3D11Manager::load_mapped_image(Texture2D& texture, u32 subresource,
                                     const MappedImageLayout& layout,
                                     const u8* image) {
    const auto& storage = texture.subresources[subresource];
    u8* shadow = texture.shadow.data() + storage.offset;
    if (layout.kind == MappedImageLayout::Kind::Storage) {
        std::memcpy(shadow, image, storage.size);
        return;
    }
    const u32 unit = shadow_unit_bytes(texture.desc.format, layout);
    if (unit == 0) return;
    const u32 units_w = storage.row_pitch / unit;
    const auto o = image_offsets(layout, unit, units_w, layout.elements_h,
                                 layout.depth);
    const usize slice_pitch = storage.slice_pitch != 0
        ? storage.slice_pitch
        : static_cast<usize>(storage.row_pitch) * layout.elements_h;
    for (u32 z = 0; z < layout.depth; ++z) {
        for (u32 y = 0; y < layout.elements_h; ++y) {
            u8* row = shadow + z * slice_pitch +
                      static_cast<usize>(y) * storage.row_pitch;
            const u8* base = image + o.z[z] + o.y[y];
            if (layout.kind == MappedImageLayout::Kind::Pitched) {
                std::memcpy(row, base, storage.row_pitch);
                continue;
            }
            for (u32 x = 0; x < units_w; ++x) {
                std::memcpy(row + static_cast<usize>(x) * unit, base + o.x[x],
                            unit);
            }
        }
    }
}

bool D3D11Manager::read_default_subresource(Texture2D& texture,
                                            u32 subresource) {
    // Every command recorded before the Map is ordered before the readback
    // (readback_slice / readback_w_slice run after the recorded work and
    // wait for it), so the CPU sees the GPU's writes (measured, [data]
    // "after UpdateSubresource" / "after CopySubresourceRegion" / "gpu").
    auto& storage = texture.subresources[subresource];
    if (storage.offset > texture.shadow.size() ||
        storage.size > texture.shadow.size() - storage.offset) {
        return false;
    }
    u8* dst = texture.shadow.data() + storage.offset;
    const u32 mips = std::max(1u, texture.desc.mip_levels);
    const u32 mip = subresource % mips;
    const u32 slice = subresource / mips;
    if (texture.volume) {
        if (texture.backend_3d == nullptr) return false;
        for (u32 z = 0; z < storage.depth; ++z) {
            if (!texture.backend_3d->readback_w_slice(
                    mip, z, dst + z * storage.slice_pitch,
                    storage.row_pitch)) {
                return false;
            }
        }
        return true;
    }
    if (is_packed_depth_stencil(texture.desc.format)) {
        return read_packed_depth_stencil(texture, mip, slice, dst,
                                         storage.row_pitch);
    }
    return texture.backend != nullptr &&
           texture.backend->readback_slice(mip, slice, dst, storage.row_pitch);
}

bool D3D11Manager::write_default_subresource(Texture2D& texture,
                                             u32 subresource) {
    // In order with the recorded work, like UpdateSubresource: draws recorded
    // before the Map keep what they read, later ones see the CPU's bytes
    // (measured, [data] "after write" read through a GPU copy).
    auto& storage = texture.subresources[subresource];
    if (storage.offset > texture.shadow.size() ||
        storage.size > texture.shadow.size() - storage.offset) {
        return false;
    }
    const u8* src = texture.shadow.data() + storage.offset;
    const u32 mips = std::max(1u, texture.desc.mip_levels);
    const u32 mip = subresource % mips;
    const u32 slice = subresource / mips;
    if (texture.volume) {
        return texture.backend_3d != nullptr &&
               texture.backend_3d->update_region(
                   mip, src, storage.row_pitch,
                   static_cast<u32>(storage.slice_pitch), 0, 0, 0,
                   storage.width, storage.height, storage.depth);
    }
    if (is_packed_depth_stencil(texture.desc.format)) {
        return write_packed_depth_stencil(texture, mip, slice, src,
                                          storage.row_pitch);
    }
    const u32 ppt = dxgi_pixels_per_texel(texture.desc.format);
    return texture.backend != nullptr &&
           texture.backend->update_region(mip, slice, src, storage.row_pitch,
                                          0, 0, storage.width / ppt,
                                          storage.height);
}

void D3D11Manager::transfer_mapped_subresource(Marshaler& m, bool write) {
    // ID3D11Device3::WriteToSubresource(pDstResource, DstSubresource,
    //   pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch) and
    // ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch,
    //   pSrcResource, SrcSubresource, pSrcBox).
    // Learn: the resource is a DEFAULT texture mapped with a NULL
    // D3D11_MAPPED_SUBRESOURCE; a NULL box is the whole subresource from
    // offset 0; an empty box (left >= right, top >= bottom or front >= back)
    // is a no-op. Measured (d3d11_texmap_probe [data]): the data is the
    // subresource's linear rows whatever the texture's layout -- a
    // ROW_MAJOR or 64K_STANDARD_SWIZZLE mapping reads and writes the same
    // CPU image as its pointer -- block rows for BC, boxes in texels; a
    // write into a READ mapping is dropped at its Unmap ("undef" 2/5/8/11,
    // unmap_texture). Outside that
    // state the runtime is undefined (a texture that is not mapped faults
    // inside d3d11 on WARP, "undef" 0/1/...; one without CPU access is
    // ignored, 12/13): here it is ignored.
    const char* method = write ? "WriteToSubresource" : "ReadFromSubresource";
    const GuestAddr resource = m.arg_ptr(write ? 1 : 4);
    const u32 subresource = static_cast<u32>(m.arg(write ? 2 : 5));
    const GuestAddr box_address = m.arg_ptr(write ? 3 : 6);
    const GuestAddr data = m.arg_ptr(write ? 4 : 1);
    const u32 data_row_pitch = static_cast<u32>(m.arg(write ? 5 : 2));
    const u32 data_depth_pitch = static_cast<u32>(m.arg(write ? 6 : 3));
    auto texture = m_textures.find(resource);
    if (texture == m_textures.end() ||
        texture->second.desc.usage != D3D11_USAGE_DEFAULT ||
        texture->second.desc.cpu_access_flags == 0 ||
        subresource >= texture->second.subresources.size() ||
        !texture->second.subresources[subresource].mapped) {
        d3d11_diag("ID3D11Device3::%s ignored: resource=0x%llx sub=%u is "
                   "not a mapped DEFAULT texture",
                   method, static_cast<unsigned long long>(resource),
                   subresource);
        return;
    }
    auto& t = texture->second;
    auto& storage = t.subresources[subresource];
    const auto layout = mapped_image_layout(t, subresource);
    const auto info =
        graphics::format_table(d3d11::texture_storage_format(t.desc.format));
    // The shadow's units: blocks for BC, else pixels of
    // staging_bytes_per_pixel (a packed 4:2:2 pixel is half a pair).
    const u32 ppt = dxgi_pixels_per_texel(t.desc.format);
    const u32 units_per_row = info.is_compressed
        ? (storage.width + 3u) / 4u
        : storage.width;
    const u32 unit = units_per_row != 0 ? storage.row_pitch / units_per_row
                                        : 0u;
    const u32 rows = info.is_compressed ? (storage.height + 3u) / 4u
                                        : storage.height;
    const u32 depth = std::max(1u, storage.depth);
    (void)ppt;
    u32 left = 0, top = 0, front = 0;
    u32 right = storage.width, bottom = storage.height, back = depth;
    if (box_address != 0) {
        std::array<u32, 6> box{};
        for (u32 i = 0; i < 6; ++i) {
            const auto value = m_memory.read_u32(box_address + i * 4u);
            if (value.is_err()) return;
            box[i] = value.value();
        }
        left = box[0];
        top = box[1];
        front = box[2];
        right = box[3];
        bottom = box[4];
        back = box[5];
        if (left >= right || top >= bottom || front >= back) return;
    }
    if (right > storage.width || bottom > storage.height || back > depth ||
        unit == 0) {
        d3d11_diag("ID3D11Device3::%s ignored: box outside subresource %u",
                   method, subresource);
        return;
    }
    // The box in shadow units and rows.
    const u32 ux0 = info.is_compressed ? left / 4u : left;
    const u32 ux1 = info.is_compressed ? (right + 3u) / 4u : right;
    const u32 uy0 = info.is_compressed ? top / 4u : top;
    const u32 uy1 = info.is_compressed ? (bottom + 3u) / 4u : bottom;
    const usize row_bytes = static_cast<usize>(ux1 - ux0) * unit;
    const usize slice_pitch = storage.slice_pitch != 0
        ? storage.slice_pitch
        : static_cast<usize>(storage.row_pitch) * rows;
    // The CPU image: the shadow (no pointer layout), or the guest's mapping
    // (row-major, standard swizzle, Texture1D) through which the
    // application may also be reading and writing.
    const bool through_mapping =
        layout.kind != MappedImageLayout::Kind::None &&
        storage.mapped_address != 0;
    std::vector<u8> mapping;
    u8* image = nullptr;
    ImageOffsets offsets;
    if (through_mapping) {
        image = m_memory.host_ptr_mut(storage.mapped_address, layout.size);
        if (image == nullptr) {
            mapping.resize(layout.size);
            if (m_memory.read_bytes(storage.mapped_address, mapping.data(),
                                    mapping.size()).is_err()) {
                return;
            }
            image = mapping.data();
        }
        offsets = image_offsets(layout, unit, storage.row_pitch / unit, rows,
                                depth);
    }
    std::vector<u8> row(row_bytes);
    for (u32 z = front; z < back; ++z) {
        for (u32 y = uy0; y < uy1; ++y) {
            const GuestAddr guest =
                data + static_cast<GuestAddr>(z - front) * data_depth_pitch +
                static_cast<GuestAddr>(y - uy0) * data_row_pitch;
            u8* shadow_row = t.shadow.data() + storage.offset +
                             z * slice_pitch +
                             static_cast<usize>(y) * storage.row_pitch +
                             static_cast<usize>(ux0) * unit;
            if (write) {
                if (m_memory.read_bytes(guest, row.data(), row_bytes)
                        .is_err()) {
                    return;
                }
                if (!through_mapping) {
                    std::memcpy(shadow_row, row.data(), row_bytes);
                    continue;
                }
                u8* base = image + offsets.z[z] + offsets.y[y];
                for (u32 x = ux0; x < ux1; ++x) {
                    std::memcpy(base + offsets.x[x],
                                row.data() + static_cast<usize>(x - ux0) * unit,
                                unit);
                }
                continue;
            }
            if (!through_mapping) {
                std::memcpy(row.data(), shadow_row, row_bytes);
            } else {
                const u8* base = image + offsets.z[z] + offsets.y[y];
                for (u32 x = ux0; x < ux1; ++x) {
                    std::memcpy(row.data() + static_cast<usize>(x - ux0) * unit,
                                base + offsets.x[x], unit);
                }
            }
            if (m_memory.write_bytes(guest, row.data(), row_bytes).is_err()) {
                return;
            }
        }
    }
    if (write) {
        if (through_mapping && !mapping.empty()) {
            (void)m_memory.write_bytes(storage.mapped_address, mapping.data(),
                                       mapping.size());
        }
        storage.cpu_dirty = true;
    }
}

} // namespace vitreo::winapi
