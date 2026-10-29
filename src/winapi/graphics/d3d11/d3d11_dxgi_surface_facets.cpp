// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_dxgi_surface_facets.cpp
/// @brief The non-texture IDXGISurface facets.
///
/// subresource_surface_method (an IDXGIResource1::CreateSubresourceSurface
/// object), buffer_surface_method (a buffer's facet) and
/// srv_surface_method (an SRV's facet). impl_DxgiSurfaceMethod
/// (d3d11_dxgi_surface.cpp) routes each call to its facet.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

void D3D11Manager::subresource_surface_method(Marshaler& m, usize slot,
                                              GuestAddr self) {
    // An IDXGIResource1::CreateSubresourceSurface object: its own COM
    // identity and reference count, standing for one subresource.
    auto surface = m_subresource_surfaces.find(self);
    const GuestAddr texture_address = surface->second.texture;
    const u32 subresource = surface->second.subresource;
    auto texture = m_textures.find(texture_address);
    if (texture == m_textures.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    const auto& desc = texture->second.desc;
    const u32 mip = subresource % std::max(desc.mip_levels, 1u);
    switch (slot) {
    case 0: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDXGIOBJECT) &&
             !guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) &&
             !guid_equal(iid, IID_IDXGISURFACE) &&
             !guid_equal(iid, IID_IDXGISURFACE1) &&
             !guid_equal(iid, IID_IDXGISURFACE2))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        ++surface->second.ref_count;
        (void)m.write_ptr(out, self);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(++surface->second.ref_count);
        return;
    case 2: {
        const u32 count = --surface->second.ref_count;
        if (count == 0) {
            m_subresource_surfaces.erase(surface);
            free_object(self);
            (void)release_texture(texture_address);
        }
        m.set_return(count);
        return;
    }
    case 3:
        m.set_return(set_private_data(m, surface->second.private_data));
        return;
    case 4:
        m.set_return(
            set_private_data_interface(m, surface->second.private_data));
        return;
    case 5:
        m.set_return(get_private_data(m, surface->second.private_data));
        return;
    case 6:
    case 7:
        m.set_return(query_interface(m, texture->second.device_owner));
        return;
    case 8: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const std::array<u32, 5> words{std::max(desc.width >> mip, 1u),
                                       std::max(desc.height >> mip, 1u),
                                       desc.format, desc.sample_count,
                                       desc.sample_quality};
        (void)m_memory.write_bytes(out,
                                   reinterpret_cast<const Byte*>(words.data()),
                                   sizeof(words));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 13: {
        // GetResource(riid, ppParentResource, pSubresource).
        const GuestAddr out = m.arg_ptr(2);
        const GuestAddr index_out = m.arg_ptr(3);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        const GuestAddr result = read_guid(m.arg_ptr(1), iid)
            ? texture_interface_for(texture_address, iid)
            : 0;
        if (result == 0) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(texture_address);
        (void)m.write_ptr(out, result);
        if (index_out != 0) (void)m_memory.write_u32(index_out, subresource);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 9: {
        // Map / Unmap / GetDC / ReleaseDC go to the subresource.
        const GuestAddr rect = m.arg_ptr(1);
        const u32 flags = static_cast<u32>(m.arg(2));
        const u32 map_type = (flags & 3u) == 3u ? 3u
            : (flags & 1u) != 0                ? 1u
            : (flags & 2u) != 0 ? ((flags & 4u) != 0 ? 4u : 2u)
                                : 0u;
        if (rect == 0 || map_type == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        GuestAddr data = 0;
        u32 row_pitch = 0;
        u32 depth_pitch = 0;
        const u32 hr = map_texture(texture_address, subresource, map_type, 0,
                                   texture->second.device_owner, data,
                                   row_pitch, depth_pitch);
        if (hr == d3d11::S_OK) {
            (void)m_memory.write_u32(rect, row_pitch);
            (void)write_pointer(rect + (m_is_64bit ? 8u : 4u), data);
        }
        m.set_return(hr);
        return;
    }
    case 10:
        unmap_texture(texture_address, subresource);
        m.set_return(d3d11::S_OK);
        return;
    default:
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
}

void D3D11Manager::buffer_surface_method(Marshaler& m, usize slot,
                                         GuestAddr buffer_address) {
    // A buffer's IDXGISurface facet (measured, a12 refmodel [facet]): a
    // ByteWidth x 1 surface of DXGI_FORMAT_UNKNOWN, one sample; Map goes to
    // the buffer (a STAGING buffer maps, a DEFAULT one is E_INVALIDARG);
    // GetParent(IDXGIDevice) is the device; there is no DC.
    auto buffer = m_buffers.find(buffer_address);
    switch (slot) {
    case 0:
        m.set_return(resource_query_interface(m, buffer_address));
        return;
    case 1:
        m.set_return(retain_object(buffer_address));
        return;
    case 2:
        m.set_return(release_object(buffer_address));
        return;
    case 3:
        m.set_return(set_private_data(m, buffer->second.private_data,
                                      buffer_address));
        return;
    case 4:
        m.set_return(set_private_data_interface(
            m, buffer->second.private_data, buffer_address));
        return;
    case 5:
        m.set_return(get_private_data(m, buffer->second.private_data,
                                      buffer_address));
        return;
    case 6:  // GetParent
    case 7:  // GetDevice
        m.set_return(query_interface(m, buffer->second.device_owner));
        return;
    case 8: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const std::array<u32, 5> words{buffer->second.desc.byte_width, 1u,
                                       0u, 1u, 0u};
        (void)m_memory.write_bytes(
            out, reinterpret_cast<const Byte*>(words.data()), sizeof(words));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 9: {
        // Map(DXGI_MAPPED_RECT*, MapFlags): DXGI_MAP_READ 1, WRITE 2,
        // DISCARD 4, as for a texture's surface.
        const GuestAddr rect = m.arg_ptr(1);
        const u32 flags = static_cast<u32>(m.arg(2));
        u32 map_type = 0;
        if ((flags & 4u) != 0) {
            map_type = (flags & 2u) != 0 ? 4u : 0u;
        } else if ((flags & 3u) == 3u) {
            map_type = 3u;
        } else if ((flags & 1u) != 0) {
            map_type = 1u;
        } else if ((flags & 2u) != 0) {
            map_type = 2u;
        }
        if (rect == 0 || map_type == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        GuestAddr data = 0;
        u32 pitch = 0;
        const u32 hr = map_buffer(buffer_address, 0, map_type, 0,
                                  buffer->second.device_owner, data, pitch);
        if (hr == d3d11::S_OK) {
            (void)m_memory.write_u32(rect, pitch);
            (void)write_pointer(rect + (m_is_64bit ? 8u : 4u), data);
        }
        m.set_return(hr);
        return;
    }
    case 10:
        unmap_buffer(buffer_address, 0);
        m.set_return(d3d11::S_OK);
        return;
    case 11:  // GetDC: never GDI-compatible (measured INVALID_CALL, HDC
    case 12:  // untouched)
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    case 13: {
        // GetResource(REFIID, void**, UINT* pSubresource): the buffer.
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        const GuestAddr result = read_guid(m.arg_ptr(1), iid)
            ? texture_interface_for(buffer_address, iid)
            : 0;
        if (result == 0) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(buffer_address);
        (void)m.write_ptr(out, result);
        if (m.arg_ptr(3) != 0) (void)m_memory.write_u32(m.arg_ptr(3), 0);
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::srv_surface_method(Marshaler& m, usize slot,
                                      GuestAddr view_address) {
    // An SRV's IDXGISurface facet (measured, a12 refmodel [facet]): the
    // view's count and identity; GetDesc describes the viewed resource;
    // GetDevice is the device but GetParent(IDXGIDevice) is E_NOINTERFACE;
    // Map is E_INVALIDARG. It answers only IDXGISurface, so the Surface1/2
    // slots are unreachable.
    auto view = m_shader_resource_views.find(view_address);
    switch (slot) {
    case 0:
        m.set_return(srv_query_interface(m, view_address));
        return;
    case 1:
        m.set_return(retain_object(view_address));
        return;
    case 2:
        m.set_return(release_object(view_address));
        return;
    case 3:
        m.set_return(set_private_data(m, view->second.private_data,
                                      view_address));
        return;
    case 4:
        m.set_return(set_private_data_interface(
            m, view->second.private_data, view_address));
        return;
    case 5:
        m.set_return(get_private_data(m, view->second.private_data,
                                      view_address));
        return;
    case 6: {
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_ptr(out, 0);
        m.set_return(out == 0 ? d3d11::E_INVALIDARG : d3d11::E_NOINTERFACE);
        return;
    }
    case 7:
        m.set_return(query_interface(m, view->second.device_owner));
        return;
    case 8: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::array<u32, 5> words{0u, 1u, 0u, 1u, 0u};
        if (auto texture = m_textures.find(view->second.texture);
            texture != m_textures.end()) {
            const auto& d = texture->second.desc;
            words = {d.width, d.height, d.format, d.sample_count,
                     d.sample_quality};
        } else if (auto buffer = m_buffers.find(view->second.buffer);
                   buffer != m_buffers.end()) {
            words[0] = buffer->second.desc.byte_width;
        }
        (void)m_memory.write_bytes(
            out, reinterpret_cast<const Byte*>(words.data()), sizeof(words));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 9:
        m.set_return(d3d11::E_INVALIDARG);
        return;
    default:
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
}

}  // namespace vitreo::winapi
