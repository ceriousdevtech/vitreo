// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_dxgi_surface.cpp
/// @brief A resource's QueryInterface graph and the IDXGISurface2 entry
///        point.
///
/// texture_interface_for and buffer_interface_for answer one
/// QueryInterface graph from every facet of a resource, and
/// resource_query_interface hands a facet out. impl_DxgiSurfaceMethod
/// serves a 2D texture's IDXGISurface2 facet and routes the subresource,
/// buffer and SRV facets to d3d11_dxgi_surface_facets.cpp.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

std::string d3d11_dxgi_surface_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_DxgiSurface_%03zu", slot);
    return name;
}

// -----------------------------------------------------------------------------
// IDXGISurface2: a 2D texture's surface facet
// -----------------------------------------------------------------------------

GuestAddr D3D11Manager::texture_interface_for(GuestAddr address,
                                              const GuidBytes& iid) {
    // The QueryInterface graph of a resource, answered the same from every
    // facet so identity and transitivity hold (measured, a12 [qi]):
    //  - every resource: IUnknown, ID3D11DeviceChild, ID3D11Resource and its
    //    own dimension interface; IDXGIObject, IDXGIDeviceSubObject,
    //    IDXGIResource and IDXGIResource1 on the DXGI resource facet;
    //  - IDXGISurface/1/2 on the surface facet for a buffer, and for a 1D or
    //    2D texture of one subresource (a surface IS one subresource: a
    //    texture with mips or slices refuses it); never for a 3D texture;
    //  - IDXGIKeyedMutex for a SHARED_KEYEDMUTEX texture.
    //  - the D3D10 facet's interfaces (ID3D10DeviceChild, ID3D10Resource
    //    and the dimension's ID3D10Buffer / Texture1D / 2D / 3D), whatever
    //    device made the resource (measured, d3d10_contract_probe).
    const bool dxgi_resource = guid_equal(iid, IID_IDXGIOBJECT) ||
                               guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) ||
                               guid_equal(iid, IID_IDXGIRESOURCE) ||
                               guid_equal(iid, IID_IDXGIRESOURCE1);
    const bool dxgi_surface = guid_equal(iid, IID_IDXGISURFACE) ||
                              guid_equal(iid, IID_IDXGISURFACE1) ||
                              guid_equal(iid, IID_IDXGISURFACE2);
    const bool base = guid_equal(iid, IID_IUNKNOWN) ||
                      guid_equal(iid, IID_ID3D11DEVICECHILD) ||
                      guid_equal(iid, IID_ID3D11RESOURCE);
    if (auto buffer = m_buffers.find(address); buffer != m_buffers.end()) {
        if (base || guid_equal(iid, IID_ID3D11BUFFER)) return address;
        if (dxgi_resource) return buffer->second.dxgi_resource_interface;
        if (dxgi_surface) return buffer->second.dxgi_surface_interface;
        // A SHARED_KEYEDMUTEX buffer answers IDXGIKeyedMutex (measured,
        // d3d11_share_ers_probe [iid]).
        if (guid_equal(iid, IID_IDXGIKEYEDMUTEX) &&
            buffer->second.keyed_mutex_interface != 0) {
            return buffer->second.keyed_mutex_interface;
        }
        return d3d10_interface_for(address, iid);
    }
    auto texture = m_textures.find(address);
    if (texture == m_textures.end()) return 0;
    const auto& t = texture->second;
    const bool plain_2d = !t.volume && !t.linear;
    const bool single = t.desc.mip_levels == 1 && t.desc.array_size == 1;
    if (base ||
        (t.linear && guid_equal(iid, IID_ID3D11TEXTURE1D)) ||
        (plain_2d && (guid_equal(iid, IID_ID3D11TEXTURE2D) ||
                      guid_equal(iid, IID_ID3D11TEXTURE2D1))) ||
        (t.volume && (guid_equal(iid, IID_ID3D11TEXTURE3D) ||
                      guid_equal(iid, IID_ID3D11TEXTURE3D1)))) {
        return address;
    }
    if (dxgi_resource) return t.dxgi_resource_interface;
    if (dxgi_surface && !t.volume && single) return t.dxgi_surface_interface;
    if (guid_equal(iid, IID_IDXGIKEYEDMUTEX) && t.keyed_mutex_interface != 0) {
        return t.keyed_mutex_interface;
    }
    return d3d10_interface_for(address, iid);
}

u32 D3D11Manager::resource_query_interface(Marshaler& m,
                                           GuestAddr resource) {
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) return d3d11::E_POINTER;
    (void)m.write_ptr(object_out, 0);
    GuidBytes iid{};
    const GuestAddr result = read_guid(m.arg_ptr(1), iid)
        ? texture_interface_for(resource, iid)
        : 0;
    if (result == 0) {
        d3d11_diag(
            "resource 0x%llx QueryInterface "
            "iid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x -> E_NOINTERFACE",
            static_cast<unsigned long long>(resource),
            iid[3], iid[2], iid[1], iid[0], iid[5], iid[4],
            iid[7], iid[6], iid[8], iid[9], iid[10], iid[11],
            iid[12], iid[13], iid[14], iid[15]);
        return d3d11::E_NOINTERFACE;
    }
    // Every facet counts on the resource (measured: a texture with an
    // IDXGIResource reference held reads 2 from both).
    (void)retain_object(resource);
    if (m.write_ptr(object_out, result).is_err()) {
        (void)release_object(resource);
        return d3d11::E_POINTER;
    }
    return d3d11::S_OK;
}

GuestAddr D3D11Manager::buffer_interface_for(GuestAddr address,
                                             const GuidBytes& iid) {
    // One graph for buffers and textures (texture_interface_for); this name
    // is what the shared-resource open path asks with.
    if (m_buffers.find(address) == m_buffers.end()) return 0;
    return texture_interface_for(address, iid);
}

void D3D11Manager::impl_DxgiSurfaceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    if (m_subresource_surfaces.contains(self)) {
        subresource_surface_method(m, slot, self);
        return;
    }
    // The same table serves a buffer's facet (object + 0x30) and an SRV's
    // (object + 0x20).
    if (self >= 0x30) {
        if (auto buffer = m_buffers.find(self - 0x30);
            buffer != m_buffers.end() &&
            buffer->second.dxgi_surface_interface == self) {
            buffer_surface_method(m, slot, self - 0x30);
            return;
        }
    }
    if (self >= 0x20) {
        if (auto view = m_shader_resource_views.find(self - 0x20);
            view != m_shader_resource_views.end() &&
            view->second.dxgi_surface_interface == self) {
            srv_surface_method(m, slot, self - 0x20);
            return;
        }
    }
    const GuestAddr texture_address = self >= 0x30 ? self - 0x30 : 0;
    auto texture = m_textures.find(texture_address);
    if (texture == m_textures.end() ||
        texture->second.dxgi_surface_interface != self) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        m.set_return(resource_query_interface(m, texture_address));
        return;
    case 1:
        m.set_return(retain_object(texture_address));
        return;
    case 2:
        m.set_return(release_object(texture_address));
        return;
    // One store per resource, shared by its facets (measured).
    case 3:  // SetPrivateData(REFGUID, UINT, const void*)
        m.set_return(set_private_data(m, texture->second.private_data,
                                      texture_address));
        return;
    case 4:  // SetPrivateDataInterface(REFGUID, const IUnknown*)
        m.set_return(set_private_data_interface(
            m, texture->second.private_data, texture_address));
        return;
    case 5:  // GetPrivateData(REFGUID, UINT*, void*)
        m.set_return(get_private_data(m, texture->second.private_data,
                                      texture_address));
        return;
    case 6: {  // GetParent(REFIID, void**)
        // A swap chain's back buffer has the chain as its parent (Learn
        // IDXGIObject::GetParent; measured: the very chain).
        GuidBytes iid{};
        const GuestAddr chain = texture->second.swap_chain_owner;
        if (chain != 0 && m_swap_chains.contains(chain) &&
            read_guid(m.arg_ptr(1), iid) &&
            (guid_equal(iid, IID_IDXGISWAPCHAIN) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN1) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN2) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN3) ||
             guid_equal(iid, IID_IDXGISWAPCHAIN4))) {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(d3d11::E_POINTER);
                return;
            }
            ++m_swap_chains.at(chain).ref_count;
            (void)m.write_ptr(out, chain);
            m.set_return(d3d11::S_OK);
            return;
        }
        m.set_return(query_interface(m, texture->second.device_owner));
        return;
    }
    case 7:  // GetDevice(REFIID, void**)
        m.set_return(query_interface(m, texture->second.device_owner));
        return;
    case 8: {
        // GetDesc(DXGI_SURFACE_DESC*): Width, Height, Format, SampleDesc.
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const auto& d = texture->second.desc;
        const std::array<u32, 5> words{d.width, d.height, d.format,
                                       d.sample_count, d.sample_quality};
        (void)m_memory.write_bytes(out, reinterpret_cast<const Byte*>(words.data()),
                                   sizeof(words));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 9: {
        // Map(DXGI_MAPPED_RECT*, MapFlags): the D3D11 map of subresource 0.
        // DXGI_MAP_READ 1, WRITE 2, DISCARD 4.
        const GuestAddr rect = m.arg_ptr(1);
        const u32 flags = static_cast<u32>(m.arg(2));
        u32 map_type = 0;
        if ((flags & 4u) != 0) {
            map_type = (flags & 2u) != 0 ? 4u : 0u;  // WRITE_DISCARD
        } else if ((flags & 3u) == 3u) {
            map_type = 3u;  // READ_WRITE
        } else if ((flags & 1u) != 0) {
            map_type = 1u;  // READ
        } else if ((flags & 2u) != 0) {
            map_type = 2u;  // WRITE
        }
        if (rect == 0 || map_type == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        GuestAddr data = 0;
        u32 row_pitch = 0;
        u32 depth_pitch = 0;
        const u32 hr = map_texture(texture_address, 0, map_type, 0,
                                   texture->second.device_owner, data,
                                   row_pitch, depth_pitch);
        if (hr == d3d11::S_OK) {
            (void)m_memory.write_u32(rect, row_pitch);
            (void)write_pointer(rect + (m_is_64bit ? 8u : 4u), data);
        }
        // A surface the CPU cannot map (a DEFAULT texture, a WRITE of a
        // READ staging texture) is E_INVALIDARG, as through D3D11 (measured).
        m.set_return(hr);
        return;
    }
    case 10:  // Unmap()
        unmap_texture(texture_address, 0);
        m.set_return(d3d11::S_OK);
        return;
    case 11:  // GetDC(BOOL Discard, HDC*)
    case 12: {  // ReleaseDC(RECT*)
        // Only a GDI_COMPATIBLE texture has a DC. A texture without the flag
        // is INVALID_CALL with the HDC left untouched (measured).
        constexpr u32 MISC_GDI_COMPATIBLE = 0x200u;
        const bool gdi =
            (texture->second.desc.misc_flags & MISC_GDI_COMPATIBLE) != 0;
        if (gdi) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED dxgi!IDXGISurface1::"
                             "GetDC: GDI device contexts on D3D11 surfaces "
                             "-> E_NOTIMPL\n");
            }
            if (slot == 11 && m.arg_ptr(2) != 0) {
                (void)m.write_ptr(m.arg_ptr(2), 0);
            }
        }
        m.set_return(gdi ? d3d11::E_NOTIMPL : DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 13: {
        // GetResource(REFIID, void** ppParentResource, UINT* pSubresource).
        const GuestAddr out = m.arg_ptr(2);
        const GuestAddr subresource = m.arg_ptr(3);
        if (out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(out, 0);
        GuidBytes iid{};
        const GuestAddr result =
            read_guid(m.arg_ptr(1), iid) ? texture_interface_for(texture_address, iid)
                                         : 0;
        if (result == 0) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(texture_address);
        (void)m.write_ptr(out, result);
        if (subresource != 0) (void)m_memory.write_u32(subresource, 0);
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

}  // namespace vitreo::winapi
