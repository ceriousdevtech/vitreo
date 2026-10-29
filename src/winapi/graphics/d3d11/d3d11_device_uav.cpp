// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_uav.cpp
/// @brief ID3D11Device::CreateUnorderedAccessView (vtable slot 8).
///
/// Split out of d3d11_device_methods.cpp; impl_DeviceMethod forwards the
/// slot here once it has found the device. The view-format rules it
/// shares with the other view creators are in d3d11_view_rules.hpp.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/d3d11/d3d11_view_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

using d3d11::supports_typed_uav_load;
using d3d11::supports_typed_uav_store;
using d3d11::typed_buffer_element_bytes;

void D3D11Manager::device_uav_method(Marshaler& m, usize slot, GuestAddr self,
                                     Device* device) {
    switch (slot) {
    case 8: {
        // CreateUnorderedAccessView(pResource, pDesc, ppUAView).
        const GuestAddr resource = m.arg_ptr(1);
        const GuestAddr desc_address = m.arg_ptr(2);
        const GuestAddr view_out = m.arg_ptr(3);
        if (view_out != 0) (void)m.write_ptr(view_out, 0);
        const GuestAddr device_owner = m_interfaces.find(self)->second.owner;
        const auto caps = format_caps_context(*device);
        // A u# register names either a texture or a buffer. Learn
        // (D3D11_UNORDERED_ACCESS_VIEW_DESC): Format at +0, ViewDimension at
        // +4, and for BUFFER the union at +8 holds FirstElement, NumElements
        // and Flags. An RWStructuredBuffer view uses DXGI_FORMAT_UNKNOWN and
        // counts elements of the buffer's structure byte stride.
        if (auto buffer = m_buffers.find(resource); buffer != m_buffers.end()) {
            if (buffer->second.device_owner != device_owner &&
                view_out != 0) {
                m.set_return(VIEW_OF_FOREIGN_RESOURCE);
                return;
            }
            const u32 stride = buffer->second.desc.structure_byte_stride;
            if ((buffer->second.desc.bind_flags &
                 D3D11_BIND_UNORDERED_ACCESS) == 0) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            u32 requested_format = 0;
            u32 dimension = D3D11_UAV_DIMENSION_BUFFER;
            u32 first_element = 0;
            u32 num_elements = 0;
            u32 uav_flags = 0;
            if (desc_address == 0) {
                // Learn (CreateUnorderedAccessView): a NULL desc views the
                // whole resource. A structured buffer is the one buffer that
                // names its elements without a desc -- {UNKNOWN, BUFFER, 0,
                // ByteWidth / StructureByteStride, 0}; a typed or raw buffer
                // needs the format only a desc supplies (measured,
                // d3d11_a03_views_probe "uav buf structured/typed/raw NULL
                // desc").
                if (stride == 0) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                num_elements = buffer->second.desc.byte_width / stride;
            } else {
                std::array<u32, 5> words{};
                for (u32 i = 0; i < words.size(); ++i) {
                    const auto word = m_memory.read_u32(desc_address + 4 * i);
                    if (word.is_err()) {
                        m.set_return(d3d11::E_INVALIDARG);
                        return;
                    }
                    words[i] = word.value();
                }
                requested_format = words[0];
                dimension = words[1];
                first_element = words[2];
                num_elements = words[3];
                uav_flags = words[4];
            }
            if (dimension != D3D11_UAV_DIMENSION_BUFFER) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            // Learn D3D11_BUFFER_UAV_FLAG.
            //
            // RAW makes this a byte-addressed RWByteAddressBuffer: no element
            // type, read and written as 32-bit words, its window counted in
            // 4-byte units -- the raw SRV's rule. APPEND and COUNTER each
            // attach a hidden counter: real GPU storage that
            // `pUAVInitialCounts` sets, `CopyStructureCount` reads, and a
            // shader moves with Append/Consume/IncrementCounter (DXBC
            // imm_atomic_alloc / imm_atomic_consume).
            constexpr u32 COUNTER_FLAGS = d3d11::BUFFER_UAV_FLAG_APPEND |
                                          d3d11::BUFFER_UAV_FLAG_COUNTER;
            const bool raw_view = (uav_flags & d3d11::BUFFER_UAV_FLAG_RAW) != 0;
            if (raw_view) {
                // Learn (D3D11_BUFFEREX_SRV_FLAG_RAW / the UAV equivalent): a
                // raw view needs a resource created with ALLOW_RAW_VIEWS and the
                // R32_TYPELESS format. Measured on Windows 2026-09-22: RAW
                // over a *structured* buffer is E_INVALIDARG, not a
                // not-implemented report -- Windows judges the combination
                // before it judges support, and so must this.
                constexpr u32 ALLOW_RAW_VIEWS = 0x20u;
                constexpr u32 R32_TYPELESS = 39u;
                // FirstElement 16-byte aligned, as for the raw SRV (measured,
                // d3d11_raw_view_alignment_probe).
                const bool well_formed =
                    (buffer->second.desc.misc_flags & ALLOW_RAW_VIEWS) != 0 &&
                    requested_format == R32_TYPELESS &&
                    (first_element & 3u) == 0;
                if (!well_formed) {
                    d3d11_diag("ID3D11Device::CreateUnorderedAccessView "
                               "buffer=0x%llx RAW -> E_INVALIDARG (needs "
                               "ALLOW_RAW_VIEWS and R32_TYPELESS; misc=0x%x "
                               "fmt=%u)",
                               static_cast<unsigned long long>(resource),
                               buffer->second.desc.misc_flags,
                               requested_format);
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                // A raw view has no counter: R32_TYPELESS is not the UNKNOWN
                // format APPEND and COUNTER require.
                if ((uav_flags & ~d3d11::BUFFER_UAV_FLAG_RAW) != 0) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
            } else if ((uav_flags & ~COUNTER_FLAGS) != 0) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            if ((uav_flags & COUNTER_FLAGS) == COUNTER_FLAGS) {
                // Learn presents APPEND and COUNTER as alternatives; a view
                // cannot be both an append buffer and a counted one.
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            if (!raw_view && stride != 0 && requested_format != 0) {
                // A structured view's elements are the resource's structures,
                // so it names no format: R32_FLOAT over a structured buffer is
                // E_INVALIDARG, with or without COUNTER (measured, "uav buf
                // struct R32_FLOAT (COUNTER)"). The counter flags below need
                // the same UNKNOWN format.
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            if ((uav_flags & COUNTER_FLAGS) != 0 && stride == 0) {
                // Learn (CopyStructureCount): the hidden counter belongs to "a
                // UAV of a Structured Buffer resource", and a structured view
                // names DXGI_FORMAT_UNKNOWN because the element size comes from
                // the resource.
                d3d11_diag("ID3D11Device::CreateUnorderedAccessView "
                           "buffer=0x%llx -> E_INVALIDARG (append/counter needs "
                           "a structured buffer and UNKNOWN format; stride=%u "
                           "fmt=%u)",
                           static_cast<unsigned long long>(resource), stride,
                           requested_format);
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            // As with a buffer SRV: a structured buffer names its element size
            // in the resource, a typed buffer names it in the view's DXGI
            // format -- which must be one a typed buffer UAV can hold
            // (BUFFER and TYPED_UNORDERED_ACCESS_VIEW; view_format_supports).
            u32 element_bytes = raw_view ? 4u : stride;
            if (element_bytes == 0) {
                if (!view_format_supports(requested_format,
                                          d3d11::FORMAT_SUPPORT_BUFFER, caps) ||
                    !view_format_supports(
                        requested_format,
                        d3d11::FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW,
                        caps)) {
                    d3d11_diag("ID3D11Device::CreateUnorderedAccessView "
                               "buffer=0x%llx -> E_INVALIDARG (fmt=%u is not "
                               "a typed buffer UAV format)",
                               static_cast<unsigned long long>(resource),
                               requested_format);
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                element_bytes = typed_buffer_element_bytes(requested_format);
                if (element_bytes == 0) {
                    d3d11_unimplemented_once(
                        "CreateUnorderedAccessView",
                        "a typed buffer UAV format the capability table "
                        "reports but no texture buffer can hold");
                    m.set_return(d3d11::E_NOTIMPL);
                    return;
                }
            }
            const u64 elements = buffer->second.desc.byte_width / element_bytes;
            if (num_elements == 0 || first_element >= elements ||
                num_elements > elements - first_element) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            if (view_out == 0) {
                m.set_return(1u);
                return;
            }
            // A typed view (RWBuffer<T>) is read and written through the
            // texture unit, exactly as a Buffer<T> SRV is read: the shader
            // declares a read_write texture_buffer, so the view needs a
            // writable texture alias over the buffer's own storage. RAW and
            // structured views stay plain buffers.
            std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer;
            if (stride == 0 && !raw_view) {
                if (buffer->second.backend) {
                    texture_buffer = m_backend.create_texture_buffer(
                        *buffer->second.backend,
                        backend_format(requested_format), first_element,
                        num_elements, /*shader_write=*/true);
                }
                if (!texture_buffer) {
                    static bool reported = false;
                    if (!reported) {
                        reported = true;
                        std::fprintf(
                            stderr,
                            "[vitreo] UNIMPLEMENTED d3d11!CreateUnorderedAccess"
                            "View: typed buffer UAV fmt=%u first=%u count=%u "
                            "has no writable texture view (a format the "
                            "backend cannot alias)\n",
                            requested_format, first_element, num_elements);
                    }
                    d3d11_diag("ID3D11Device::CreateUnorderedAccessView "
                               "buffer=0x%llx -> E_FAIL (no writable texture "
                               "view) fmt=%u first=%u count=%u",
                               static_cast<unsigned long long>(resource),
                               requested_format, first_element, num_elements);
                    m.set_return(d3d11::E_FAIL);
                    return;
                }
            }
            const GuestAddr view = create_buffer_unordered_access_view(
                buffer->second.device_owner, resource, requested_format,
                dimension, first_element, num_elements, element_bytes,
                uav_flags, std::move(texture_buffer));
            if (view == 0) {
                m.set_return(d3d11::E_FAIL);
                return;
            }
            if (m.write_ptr(view_out, view).is_err()) {
                (void)release_object(view);
                m.set_return(d3d11::E_POINTER);
                return;
            }
            d3d11_diag(
                "ID3D11Device::CreateUnorderedAccessView buffer=0x%llx -> "
                "0x%llx element=%u stride=%u first=%u count=%u",
                static_cast<unsigned long long>(resource),
                static_cast<unsigned long long>(view), element_bytes, stride,
                first_element, num_elements);
            m.set_return(d3d11::S_OK);
            return;
        }
        auto texture = m_textures.find(resource);
        if (texture != m_textures.end() && view_out != 0 &&
            texture->second.device_owner != device_owner) {
            m.set_return(VIEW_OF_FOREIGN_RESOURCE);
            return;
        }
        if (texture == m_textures.end() ||
            (texture->second.desc.bind_flags &
             D3D11_BIND_UNORDERED_ACCESS) == 0) {
            d3d11_diag(
                "ID3D11Device::CreateUnorderedAccessView REJECT resource "
                "0x%llx known=%u bind=0x%x",
                static_cast<unsigned long long>(resource),
                texture == m_textures.end() ? 0u : 1u,
                texture == m_textures.end()
                    ? 0u : texture->second.desc.bind_flags);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        u32 format = texture->second.desc.format;
        // Learn (CreateUnorderedAccessView, NULL desc): the whole resource in
        // its own format -- mip 0 and every slice. Over an array or a cubemap
        // that is TEXTURE2DARRAY {0, 0, ArraySize}, over a volume TEXTURE3D
        // {0, 0, Depth} (measured, d3d11_a03_views_probe "uav NULL tex2darr",
        // "uav NULL cube", "uav NULL tex3d").
        const bool cube = (texture->second.desc.misc_flags &
                           D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
        u32 dimension = texture->second.volume
            ? D3D11_UAV_DIMENSION_TEXTURE3D
            : texture->second.linear
                ? (texture->second.desc.array_size > 1
                       ? D3D11_UAV_DIMENSION_TEXTURE1DARRAY
                       : D3D11_UAV_DIMENSION_TEXTURE1D)
                : (texture->second.desc.array_size > 1 || cube
                       ? D3D11_UAV_DIMENSION_TEXTURE2DARRAY
                       : D3D11_UAV_DIMENSION_TEXTURE2D);
        u32 mip_slice = 0;
        u32 first_w_slice = 0;
        u32 w_size = 0xFFFFFFFFu;
        if (desc_address != 0) {
            const auto requested_format = m_memory.read_u32(desc_address);
            const auto requested_dimension =
                m_memory.read_u32(desc_address + 4);
            const auto requested_mip = m_memory.read_u32(desc_address + 8);
            if (requested_format.is_err() || requested_dimension.is_err() ||
                requested_mip.is_err()) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            format = requested_format.value() == 0
                ? format : requested_format.value();
            dimension = requested_dimension.value();
            mip_slice = requested_mip.value();
            first_w_slice = 0;
            w_size = 0xFFFFFFFFu;
            if (dimension == D3D11_UAV_DIMENSION_TEXTURE3D ||
                dimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY ||
                dimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) {
                // D3D11_TEX3D_UAV: MipSlice at +8, FirstWSlice at +12,
                // WSize at +16. D3D11_TEX1D_ARRAY_UAV and D3D11_TEX2D_ARRAY_UAV
                // put FirstArraySlice and ArraySize in the same union slots.
                const auto requested_first = m_memory.read_u32(desc_address + 12);
                const auto requested_size = m_memory.read_u32(desc_address + 16);
                if (requested_first.is_err() || requested_size.is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                first_w_slice = requested_first.value();
                w_size = requested_size.value();
            }
        }
        // The view format must be a typed UAV format (view_format_supports):
        // a typeless one -- including UNKNOWN over a typeless resource -- and
        // an sRGB one are refused (measured, "uav TYPELESS res NULL desc",
        // "uav TYPELESS res as UNORM_SRGB").
        if (!view_format_supports(
                format, d3d11::FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW,
                caps)) {
            d3d11_diag("ID3D11Device::CreateUnorderedAccessView REJECT "
                       "texture 0x%llx fmt=%u is not a typed UAV format",
                       static_cast<unsigned long long>(resource), format);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // create_unordered_access_view judges the rest -- dimension, mip,
        // slice and W windows, and the format against the resource's -- so a
        // validate-only call (ppUAView NULL) creates the view and releases it:
        // Learn, S_FALSE only when every check passes (measured, "uav 3D bad
        // window, ppView NULL" is E_INVALIDARG).
        const GuestAddr view = create_unordered_access_view(
            texture->second.device_owner, resource, format, dimension,
            mip_slice, first_w_slice, w_size);
        if (view == 0) {
            d3d11_diag(
                "ID3D11Device::CreateUnorderedAccessView REJECT texture "
                "0x%llx dim=%u fmt=%u (res fmt=%u) mip=%u volume=%u "
                "%ux%u arr=%u mips=%u",
                static_cast<unsigned long long>(resource), dimension, format,
                texture->second.desc.format, mip_slice,
                texture->second.volume ? 1u : 0u, texture->second.desc.width,
                texture->second.desc.height, texture->second.desc.array_size,
                texture->second.desc.mip_levels);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (view_out == 0) {
            (void)release_object(view);
            m.set_return(1u);
            return;
        }
        if (m.write_ptr(view_out, view).is_err()) {
            (void)release_object(view);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
