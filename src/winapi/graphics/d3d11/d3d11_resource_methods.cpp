// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_resource_methods.cpp
/// @brief The vtables of the objects a device creates.
///
/// Buffers, 2-D textures, the three view types, queries, the DXGI resource
/// interface every resource also answers to, and the annotation interface a
/// profiler talks to.
///
/// These share a shape: each is mostly GetDesc, GetType, the IUnknown trio, and
/// a private-data pair, all reading storage that d3d11.cpp owns. That is why
/// they are one file rather than eight — the interesting differences are a few
/// slots each, not whole implementations.
///
/// Split out of d3d11.cpp once the constants moved to d3d11_internal.hpp.

#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::impl_BufferMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto buffer = m_buffers.find(self);
    if (buffer == m_buffers.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        m.set_return(resource_query_interface(m, self));
        return;
    // WP-R1 (com_direct): the record is in hand, so skip visit_child's walk
    // of every object map (and release_internal's) -- same counts, same
    // parent references, same release_buffer.
    case 1:
        m.set_return(com_direct::active() ? retain_child(buffer->second)
                                          : retain_object(self));
        return;
    case 2:
        m_releasing_caller = m.guest_return_address();
        m.set_return(com_direct::active()
                         ? release_child(self, buffer->second,
                                         &D3D11Manager::release_buffer)
                         : release_object(self));
        m_releasing_caller = 0;
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(buffer->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, buffer->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, buffer->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, buffer->second.private_data));
        return;
    case 7: {
        const GuestAddr type_out = m.arg_ptr(1);
        if (type_out != 0) {
            (void)m.write_u32(type_out, D3D11_RESOURCE_DIMENSION_BUFFER);
        }
        m.set_return(0);
        return;
    }
    case 8:
        buffer->second.eviction_priority = static_cast<u32>(m.arg(1));
        m.set_return(0);
        return;
    case 9:
        m.set_return(buffer->second.eviction_priority);
        return;
    case 10:
        if (!write_buffer_desc(m.arg_ptr(1), buffer->second.desc)) {
            d3d11_diag("ID3D11Buffer::GetDesc invalid output");
        }
        m.set_return(0);
        return;
    default:
        unsupported(m, "ID3D11Buffer", slot);
        return;
    }
}

void D3D11Manager::impl_Texture2DMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto texture = m_textures.find(self);
    if (texture == m_textures.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        // One graph for every dimension and facet (texture_interface_for):
        // a Texture1D answers ID3D11Texture1D and not the 2D interfaces its
        // host storage happens to share (measured "G tex1d QI: T1D=ok
        // T2D=no").
        m.set_return(resource_query_interface(m, self));
        return;
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(texture->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, texture->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, texture->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, texture->second.private_data));
        return;
    case 7: {
        const GuestAddr type_out = m.arg_ptr(1);
        if (type_out != 0) {
            (void)m.write_u32(type_out, texture->second.volume
                ? D3D11_RESOURCE_DIMENSION_TEXTURE3D
                : texture->second.linear
                    ? D3D11_RESOURCE_DIMENSION_TEXTURE1D
                    : D3D11_RESOURCE_DIMENSION_TEXTURE2D);
        }
        m.set_return(0);
        return;
    }
    case 8:
        texture->second.eviction_priority = static_cast<u32>(m.arg(1));
        m.set_return(0);
        return;
    case 9:
        m.set_return(texture->second.eviction_priority);
        return;
    case 10:
        if (texture->second.volume) {
            const auto& d = texture->second.volume_desc;
            const std::array<u32, 9> words{
                d.width, d.height, d.depth, d.mip_levels, d.format, d.usage,
                d.bind_flags, d.cpu_access_flags, d.misc_flags};
            if (m.arg_ptr(1) != 0) {
                (void)m_memory.write_bytes(
                    m.arg_ptr(1), reinterpret_cast<const Byte*>(words.data()),
                    sizeof(words));
            }
        } else if (texture->second.linear) {
            // ID3D11Texture1D::GetDesc writes a D3D11_TEXTURE1D_DESC, which is
            // the 2D one without Height and SampleDesc. The resource is backed
            // by a height-1 2D texture, but a guest reading back the 2D layout
            // would take MipLevels out of the Height field.
            const auto& d = texture->second.desc;
            const std::array<u32, 8> words{
                d.width, d.mip_levels, d.array_size, d.format, d.usage,
                d.bind_flags, d.cpu_access_flags, d.misc_flags};
            if (m.arg_ptr(1) != 0) {
                (void)m_memory.write_bytes(
                    m.arg_ptr(1), reinterpret_cast<const Byte*>(words.data()),
                    sizeof(words));
            }
        } else if (!write_texture_desc(m.arg_ptr(1), texture->second.desc)) {
            d3d11_diag("ID3D11Texture2D::GetDesc invalid output");
        }
        m.set_return(0);
        return;
    case 11: {
        // ID3D11Texture2D1 / ID3D11Texture3D1::GetDesc1: the base descriptor
        // plus TextureLayout, the value the texture was created with (any
        // value -- measured, d3d11_texmap_probe [other]; UNDEFINED from the
        // base creators). ID3D11Texture1D has no slot 11.
        if (texture->second.linear) {
            unsupported(m, "ID3D11Texture1D", slot);
            return;
        }
        const GuestAddr out = m.arg_ptr(1);
        const bool volume = texture->second.volume;
        const u32 layout = texture->second.layout;
        impl_Texture2DMethod(m, 10);
        if (out != 0) {
            (void)m_memory.write_u32(out + (volume ? 36u : 44u), layout);
        }
        return;
    }
    default:
        unsupported(m, "ID3D11Texture2D", slot);
        return;
    }
}

bool* D3D11Manager::dxgi_resource_offered_flag(GuestAddr resource) {
    if (resource < 0x20) return nullptr;
    const GuestAddr owner = resource - 0x20;
    if (auto texture = m_textures.find(owner);
        texture != m_textures.end() &&
        texture->second.dxgi_resource_interface == resource) {
        return &texture->second.offered;
    }
    if (auto buffer = m_buffers.find(owner);
        buffer != m_buffers.end() &&
        buffer->second.dxgi_resource_interface == resource) {
        return &buffer->second.offered;
    }
    return nullptr;
}

std::optional<u32> D3D11Manager::dxgi_resource_residency(
    GuestAddr resource) const {
    // Learn DXGI_RESIDENCY: FULLY_RESIDENT (1) "located in video memory",
    // RESIDENT_IN_SHARED_MEMORY (2) "at least some ... in CPU memory".
    // Nothing is ever evicted (3). Under the discrete-class adapter profile
    // (.docs/contracts/dxgi.md) every backend allocation is in the dedicated
    // LOCAL pool, the one QueryVideoMemoryInfo counts; a resource with no
    // backend storage (a STAGING texture: CPU memory in the record's shadow)
    // is CPU memory.
    constexpr u32 kFullyResident = 1;
    constexpr u32 kResidentInSharedMemory = 2;
    // The object itself, or its IDXGIResource (+0x20), IDXGIKeyedMutex
    // (+0x28) or IDXGISurface (+0x30) facet (create_buffer,
    // create_texture_2d).
    const auto is_facet = [resource](GuestAddr owner, const auto& record) {
        return resource == owner ||
               resource == record.dxgi_resource_interface ||
               resource == record.keyed_mutex_interface ||
               resource == record.dxgi_surface_interface;
    };
    for (const GuestAddr offset : {GuestAddr{0}, GuestAddr{0x20},
                                   GuestAddr{0x28}, GuestAddr{0x30}}) {
        if (resource == 0 || resource < offset) break;
        const GuestAddr owner = resource - offset;
        if (auto texture = m_textures.find(owner);
            texture != m_textures.end() && is_facet(owner, texture->second)) {
            const bool gpu = texture->second.backend != nullptr ||
                             texture->second.backend_3d != nullptr;
            return gpu ? kFullyResident : kResidentInSharedMemory;
        }
        if (auto buffer = m_buffers.find(owner);
            buffer != m_buffers.end() && is_facet(owner, buffer->second)) {
            return buffer->second.backend != nullptr ? kFullyResident
                                                     : kResidentInSharedMemory;
        }
    }
    return std::nullopt;
}

namespace {

/// IDXGIResource::GetUsage's CPU-access field for a D3D11 usage (measured:
/// STAGING is DXGI_CPU_ACCESS_SCRATCH, DYNAMIC DXGI_CPU_ACCESS_DYNAMIC).
[[nodiscard]] u32 dxgi_cpu_access_for_usage(u32 usage) {
    constexpr u32 kCpuAccessDynamic = 1;
    constexpr u32 kCpuAccessScratch = 3;
    if (usage == D3D11_USAGE_STAGING) return kCpuAccessScratch;
    if (usage == D3D11_USAGE_DYNAMIC) return kCpuAccessDynamic;
    return 0;
}

constexpr u32 kDxgiUsageUnorderedAccess = 0x400u;
constexpr u32 kDxgiUsageReadOnly = 0x100u;
constexpr u32 kDxgiUsageDiscardOnPresent = 0x200u;

} // namespace

void D3D11Manager::buffer_dxgi_resource_method(Marshaler& m, usize slot,
                                               GuestAddr buffer_address) {
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
    // The buffer's own store (measured: facets share it).
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
    case 6:
    case 7:
        m.set_return(query_interface(m, buffer->second.device_owner));
        return;
    case 8:
        // GetSharedHandle: S_OK with a NULL handle for an unshared buffer;
        // a SHARED / KEYEDMUTEX one hands out the id of the memory every
        // opening maps (measured, d3d11_share_ers_probe; d3d11_sharing.cpp).
        m.set_return(get_shared_handle(m, buffer->second));
        return;
    case 9: {
        const GuestAddr usage_out = m.arg_ptr(1);
        u32 usage = dxgi_cpu_access_for_usage(buffer->second.desc.usage);
        if ((buffer->second.desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0) {
            usage |= DXGI_USAGE_SHADER_INPUT;
        }
        if ((buffer->second.desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) !=
            0) {
            usage |= kDxgiUsageUnorderedAccess;
        }
        if (usage_out == 0 || m.write_u32(usage_out, usage).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 10:
        buffer->second.eviction_priority = static_cast<u32>(m.arg(1));
        m.set_return(d3d11::S_OK);
        return;
    case 11: {
        const GuestAddr priority_out = m.arg_ptr(1);
        if (priority_out == 0 ||
            m.write_u32(priority_out, buffer->second.eviction_priority)
                .is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 12: {
        // CreateSubresourceSurface: a buffer has no surface subresources.
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_ptr(out, 0);
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    case 13: {
        // CreateSharedHandle: only SHARED_NTHANDLE resources (E_INVALIDARG).
        const GuestAddr handle_out = m.arg_ptr(4);
        if (handle_out != 0) (void)m.write_ptr(handle_out, 0);
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    default:
        unsupported(m, "IDXGIResource", slot);
        return;
    }
}

void D3D11Manager::impl_DxgiResourceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    if (self < 0x20) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    const GuestAddr texture_address = self - 0x20;
    if (auto buffer = m_buffers.find(texture_address);
        buffer != m_buffers.end() &&
        buffer->second.dxgi_resource_interface == self) {
        buffer_dxgi_resource_method(m, slot, texture_address);
        return;
    }
    auto texture = m_textures.find(texture_address);
    if (texture == m_textures.end() ||
        texture->second.dxgi_resource_interface != self) {
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
    // The texture's own store, interface entries included (measured: set
    // through ID3D11Texture2D, read through IDXGIResource).
    case 3:
        m.set_return(set_private_data(m, texture->second.private_data,
                                      texture_address));
        return;
    case 4:
        m.set_return(set_private_data_interface(
            m, texture->second.private_data, texture_address));
        return;
    case 5:
        m.set_return(get_private_data(m, texture->second.private_data,
                                      texture_address));
        return;
    case 6:
    case 7: {
        const u32 result = query_interface(m, texture->second.device_owner);
        m.set_return(result);
        return;
    }
    case 8:
        // IDXGIResource::GetSharedHandle (d3d11_sharing.cpp).
        m.set_return(get_shared_handle(m, texture->second));
        return;
    case 9: {
        // GetUsage (measured): the CPU-access field from the D3D11 usage,
        // SHADER_INPUT / RENDER_TARGET_OUTPUT / UNORDERED_ACCESS from the
        // bind flags, and for a swap-chain buffer BACK_BUFFER plus
        // DISCARD_ON_PRESENT (DISCARD chains) or READ_ONLY (buffers n > 0).
        const GuestAddr usage_out = m.arg_ptr(1);
        u32 usage = dxgi_cpu_access_for_usage(texture->second.desc.usage);
        if ((texture->second.desc.bind_flags &
             D3D11_BIND_SHADER_RESOURCE) != 0) {
            usage |= DXGI_USAGE_SHADER_INPUT;
        }
        if ((texture->second.desc.bind_flags &
             D3D11_BIND_RENDER_TARGET) != 0) {
            usage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
        }
        if ((texture->second.desc.bind_flags &
             D3D11_BIND_UNORDERED_ACCESS) != 0) {
            usage |= kDxgiUsageUnorderedAccess;
        }
        if (texture->second.swap_chain_owner != 0) {
            usage |= DXGI_USAGE_BACK_BUFFER;
            auto chain = m_swap_chains.find(texture->second.swap_chain_owner);
            if (chain != m_swap_chains.end() &&
                chain->second.desc.swap_effect == 0) {
                usage |= kDxgiUsageDiscardOnPresent;
            }
            if (texture->second.read_only_buffer) usage |= kDxgiUsageReadOnly;
        }
        if (usage_out == 0 || m.write_u32(usage_out, usage).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 10:
        texture->second.eviction_priority = static_cast<u32>(m.arg(1));
        m.set_return(d3d11::S_OK);
        return;
    case 11: {
        const GuestAddr priority_out = m.arg_ptr(1);
        if (priority_out == 0 ||
            m.write_u32(priority_out,
                        texture->second.eviction_priority).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 12: {
        // IDXGIResource1::CreateSubresourceSurface(index, ppSurface): an
        // IDXGISurface2 for one subresource of a 2D texture (measured: mip 1
        // of a 64x64 is a 32x32 surface whose GetResource names
        // subresource 1); an index past the last subresource is E_INVALIDARG.
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_ptr(out, 0);
        const auto& desc = texture->second.desc;
        const u32 subresources = desc.mip_levels * desc.array_size;
        if (out == 0 || texture->second.volume || texture->second.linear ||
            index >= subresources) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const GuestAddr surface = allocate_object(m_dxgi_surface_vtable);
        if (surface == 0) {
            m.set_return(d3d11::E_OUTOFMEMORY);
            return;
        }
        SubresourceSurface record{};
        record.texture = texture_address;
        record.subresource = index;
        m_subresource_surfaces.emplace(surface, std::move(record));
        ++texture->second.ref_count;
        (void)m.write_ptr(out, surface);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 13:
        // IDXGIResource1::CreateSharedHandle (d3d11_sharing.cpp).
        m.set_return(create_shared_handle(m, texture->second));
        return;
    default:
        unsupported(m, "IDXGIResource", slot);
        return;
    }
}

// ID3D11View::GetResource(ID3D11Resource**) returns void; the out-parameter
// receives the viewed resource with a reference added. retain_object covers
// buffers and textures alike -- a buffer view once answered nothing.
void D3D11Manager::write_view_resource(Marshaler& m, GuestAddr resource) {
    const GuestAddr out = m.arg_ptr(1);
    if (out != 0 && resource != 0 && retain_object(resource) != 0 &&
        m.write_ptr(out, resource).is_err()) {
        (void)release_object(resource);
    }
    m.set_return(0);
}

u32 D3D11Manager::srv_query_interface(Marshaler& m, GuestAddr view_address) {
    // Measured (a12 [qi] srv): besides the view interfaces, an SRV answers
    // IDXGIObject, IDXGIDeviceSubObject and IDXGISurface -- a runtime quirk,
    // on a facet that shares the view's count and identity (refmodel
    // [facet]). Not IDXGISurface1/2, not IDXGIResource.
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) return d3d11::E_POINTER;
    (void)m.write_ptr(object_out, 0);
    auto view = m_shader_resource_views.find(view_address);
    GuidBytes iid{};
    if (view == m_shader_resource_views.end() ||
        !read_guid(m.arg_ptr(1), iid)) {
        return d3d11::E_NOINTERFACE;
    }
    GuestAddr result = 0;
    if (guid_equal(iid, IID_IUNKNOWN) ||
        guid_equal(iid, IID_ID3D11DEVICECHILD) ||
        guid_equal(iid, IID_ID3D11VIEW) ||
        guid_equal(iid, IID_ID3D11SHADERRESOURCEVIEW) ||
        guid_equal(iid, IID_ID3D11SHADERRESOURCEVIEW1)) {
        result = view_address;
    } else if (guid_equal(iid, IID_IDXGIOBJECT) ||
               guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) ||
               guid_equal(iid, IID_IDXGISURFACE)) {
        result = view->second.dxgi_surface_interface;
    } else {
        // ID3D10View / ShaderResourceView(1) (d3d10_device.cpp).
        result = d3d10_interface_for(view_address, iid);
    }
    if (result == 0) return d3d11::E_NOINTERFACE;
    (void)retain_object(view_address);
    if (m.write_ptr(object_out, result).is_err()) {
        (void)release_object(view_address);
        return d3d11::E_POINTER;
    }
    return d3d11::S_OK;
}

void D3D11Manager::impl_ShaderResourceViewMethod(
    Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto view = m_shader_resource_views.find(self);
    if (view == m_shader_resource_views.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        m.set_return(srv_query_interface(m, self));
        return;
    // WP-R1 (com_direct): as for buffers.
    case 1:
        m.set_return(com_direct::active() ? retain_child(view->second)
                                          : retain_object(self));
        return;
    case 2:
        m.set_return(com_direct::active()
                         ? release_child(self, view->second,
                                         &D3D11Manager::
                                             release_shader_resource_view)
                         : release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(view->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, view->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, view->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, view->second.private_data));
        return;
    case 7:
        // ID3D11View::GetResource: the viewed resource, AddRef'd -- a buffer
        // as much as a texture (measured, d3d11_view_desc_probe).
        write_view_resource(m, view->second.buffer != 0 ? view->second.buffer
                                                        : view->second.texture);
        return;
    case 8: {
        // D3D11_SHADER_RESOURCE_VIEW_DESC: Format, ViewDimension, then a
        // 16-byte union the dimension selects. Windows zeroes what the shape
        // does not use and reports -1 counts resolved (measured,
        // d3d11_view_desc_probe).
        const GuestAddr desc_out = m.arg_ptr(1);
        const auto& v = view->second;
        std::array<u32, 6> words{v.format, v.view_dimension};
        switch (v.view_dimension) {
        case 1:   // BUFFER {FirstElement, NumElements}
        case 11:  // BUFFEREX {FirstElement, NumElements, Flags}
            words[2] = v.first_element;
            words[3] = v.num_elements;
            if (v.view_dimension == 11) words[4] = v.buffer_flags;
            break;
        case 6:  // TEXTURE2DMS: its one unused word, echoed (measured)
            words[2] = v.ms_unused_field;
            break;
        case 7:  // TEXTURE2DMSARRAY {FirstArraySlice, ArraySize}
            words[2] = v.first_array_slice;
            words[3] = v.array_size;
            break;
        case 3:   // TEXTURE1DARRAY
        case 5:   // TEXTURE2DARRAY {MostDetailedMip, MipLevels, First, Size}
        case 10:  // TEXTURECUBEARRAY {..., First2DArrayFace, NumCubes}
            words[4] = v.first_array_slice;
            words[5] = v.view_dimension == 10 ? v.array_size / 6u
                                              : v.array_size;
            [[fallthrough]];
        default:  // TEXTURE1D/2D/3D/CUBE {MostDetailedMip, MipLevels}
            words[2] = v.most_detailed_mip;
            words[3] = v.mip_levels;
            break;
        }
        if (desc_out != 0) {
            (void)m_memory.write_bytes(
                desc_out, reinterpret_cast<const Byte*>(words.data()),
                sizeof(words));
        }
        m.set_return(0);
        return;
    }
    case 9: {
        // ID3D11ShaderResourceView1::GetDesc1: the base descriptor, plus a PlaneSlice in the
        // Texture2D/Texture2DArray members that is always 0 here (the *1
        // creators refuse any other plane). Zero the whole 28-byte DESC1
        // first so the tail the base GetDesc does not reach reads as 0.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            for (u32 i = 0; i < 28u; i += 4) (void)m_memory.write_u32(out + i, 0);
        }
        impl_ShaderResourceViewMethod(m, 8);
        write_view_plane_slice(out, view->second.texture, view->second.format,
                               view->second.view_dimension, false, 16u, 24u);
        return;
    }
    default:
        unsupported(m, "ID3D11ShaderResourceView", slot);
        return;
    }
}

void D3D11Manager::write_view_plane_slice(GuestAddr desc_out,
                                          GuestAddr texture, u32 format,
                                          u32 view_dimension, bool uav,
                                          u32 offset_2d, u32 offset_2d_array) {
    // The PlaneSlice a GetDesc1 reports: the plane the view format names on
    // a planar video resource (measured, d3d11_nv12_probe [views]: an
    // R8G8 view of NV12 reports plane 1 however it was created), 0 on every
    // other resource. TEXTURE2D is 4, TEXTURE2DARRAY 5 for all three kinds.
    if (desc_out == 0 || (view_dimension != 4u && view_dimension != 5u)) {
        return;
    }
    const auto resource = m_textures.find(texture);
    if (resource == m_textures.end() ||
        !dxgi_format_is_planar(resource->second.desc.format)) {
        return;
    }
    const int plane =
        dxgi_video_view_plane(resource->second.desc.format, format, uav);
    if (plane > 0) {
        (void)m_memory.write_u32(
            desc_out + (view_dimension == 4u ? offset_2d : offset_2d_array),
            static_cast<u32>(plane));
    }
}

void D3D11Manager::impl_UnorderedAccessViewMethod(
    Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto view = m_unordered_access_views.find(self);
    if (view == m_unordered_access_views.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11VIEW) &&
             !guid_equal(iid, IID_ID3D11UNORDEREDACCESSVIEW) &&
             !guid_equal(iid, IID_ID3D11UNORDEREDACCESSVIEW1))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(view->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out, device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, view->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, view->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, view->second.private_data));
        return;
    case 7:
        write_view_resource(m, view->second.buffer != 0 ? view->second.buffer
                                                        : view->second.texture);
        return;
    case 8: {
        // D3D11_UNORDERED_ACCESS_VIEW_DESC: Format, ViewDimension, then a
        // 12-byte union, zeroed past the shape's fields (measured,
        // d3d11_view_desc_probe).
        const GuestAddr desc_out = m.arg_ptr(1);
        const auto& v = view->second;
        std::array<u32, 5> words{v.format, v.view_dimension, v.mip_slice};
        switch (v.view_dimension) {
        case 1:  // BUFFER {FirstElement, NumElements, Flags}
            words[2] = v.first_element;
            words[3] = v.num_elements;
            words[4] = v.buffer_flags;
            break;
        case 3:  // TEXTURE1DARRAY {MipSlice, FirstArraySlice, ArraySize}
        case 5:  // TEXTURE2DARRAY, the same three words
            words[3] = v.first_array_slice;
            words[4] = v.array_size;
            break;
        case 8:  // TEXTURE3D {MipSlice, FirstWSlice, WSize}
            words[3] = v.first_w_slice;
            words[4] = v.w_size;
            break;
        default:  // TEXTURE1D / TEXTURE2D {MipSlice}
            break;
        }
        if (desc_out != 0) {
            (void)m_memory.write_bytes(
                desc_out, reinterpret_cast<const Byte*>(words.data()),
                sizeof(words));
        }
        m.set_return(0);
        return;
    }
    case 9: {
        // ID3D11UnorderedAccessView1::GetDesc1: the base descriptor, plus a PlaneSlice in the
        // Texture2D/Texture2DArray members that is always 0 here (the *1
        // creators refuse any other plane). Zero the whole 24-byte DESC1
        // first so the tail the base GetDesc does not reach reads as 0.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            for (u32 i = 0; i < 24u; i += 4) (void)m_memory.write_u32(out + i, 0);
        }
        impl_UnorderedAccessViewMethod(m, 8);
        write_view_plane_slice(out, view->second.texture, view->second.format,
                               view->second.view_dimension, true, 12u, 20u);
        return;
    }
    default:
        unsupported(m, "ID3D11UnorderedAccessView", slot);
        return;
    }
}

void D3D11Manager::impl_RenderTargetViewMethod(
    Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto view = m_render_target_views.find(self);
    if (view == m_render_target_views.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11VIEW) &&
             !guid_equal(iid, IID_ID3D11RENDERTARGETVIEW) &&
             !guid_equal(iid, IID_ID3D11RENDERTARGETVIEW1))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(view->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, view->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, view->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, view->second.private_data));
        return;
    case 7:
        // GetResource: the application gets a reference to the resource.
        write_view_resource(m, view->second.buffer != 0 ? view->second.buffer
                                                        : view->second.texture);
        return;
    case 8: {
        // Learn ID3D11RenderTargetView::GetDesc → D3D11_RENDER_TARGET_VIEW_DESC:
        // Format, ViewDimension, then a 12-byte union, zeroed past the shape's
        // fields, with -1 counts resolved (measured, d3d11_view_desc_probe).
        const GuestAddr desc_out = m.arg_ptr(1);
        const auto& v = view->second;
        std::array<u32, 5> words{v.format, v.view_dimension, v.mip_slice};
        switch (v.view_dimension) {
        case D3D11_RTV_DIMENSION_BUFFER:  // {FirstElement, NumElements}
            words[2] = v.first_element;
            words[3] = v.num_elements;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMS:
            words[2] = 0;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
            words[2] = v.first_array_slice;
            words[3] = v.array_size;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        case D3D11_RTV_DIMENSION_TEXTURE3D:  // {MipSlice, FirstWSlice, WSize}
            words[3] = v.first_array_slice;
            words[4] = v.array_size;
            break;
        default:  // TEXTURE1D / TEXTURE2D {MipSlice}
            break;
        }
        if (desc_out != 0) {
            (void)m_memory.write_bytes(
                desc_out, reinterpret_cast<const Byte*>(words.data()),
                sizeof(words));
        }
        m.set_return(0);
        return;
    }
    case 9: {
        // ID3D11RenderTargetView1::GetDesc1: the base descriptor, plus a PlaneSlice in the
        // Texture2D/Texture2DArray members that is always 0 here (the *1
        // creators refuse any other plane). Zero the whole 24-byte DESC1
        // first so the tail the base GetDesc does not reach reads as 0.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            for (u32 i = 0; i < 24u; i += 4) (void)m_memory.write_u32(out + i, 0);
        }
        impl_RenderTargetViewMethod(m, 8);
        write_view_plane_slice(out, view->second.texture, view->second.format,
                               view->second.view_dimension, false, 12u, 20u);
        return;
    }
    default:
        unsupported(m, "ID3D11RenderTargetView", slot);
        return;
    }
}

void D3D11Manager::impl_DepthStencilViewMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto view = m_depth_stencil_views.find(self);
    if (view == m_depth_stencil_views.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11VIEW) &&
             !guid_equal(iid, IID_ID3D11DEPTHSTENCILVIEW))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(view->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, view->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, view->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, view->second.private_data));
        return;
    case 7:
        // GetResource: the application gets a reference to the resource.
        write_view_resource(m, view->second.texture);
        return;
    case 8: {
        // Learn ID3D11DepthStencilView::GetDesc → D3D11_DEPTH_STENCIL_VIEW_DESC:
        // Format, ViewDimension, Flags, then a 12-byte union the dimension
        // selects. Windows zeroes what the shape does not use and reports -1
        // resolved (measured, d3d11_dsv_probe).
        const GuestAddr desc_out = m.arg_ptr(1);
        const auto& v = view->second;
        std::array<u32, 6> words{v.format, v.view_dimension, v.flags};
        switch (v.view_dimension) {
        case 2:  // TEXTURE1DARRAY {MipSlice, FirstArraySlice, ArraySize}
        case 4:  // TEXTURE2DARRAY
            words[3] = v.mip_slice;
            words[4] = v.first_array_slice;
            words[5] = v.array_size;
            break;
        case 5:  // TEXTURE2DMS: nothing
            break;
        case 6:  // TEXTURE2DMSARRAY {FirstArraySlice, ArraySize}
            words[3] = v.first_array_slice;
            words[4] = v.array_size;
            break;
        default:  // TEXTURE1D / TEXTURE2D {MipSlice}
            words[3] = v.mip_slice;
            words[5] = v.union_tail;
            break;
        }
        if (desc_out != 0) {
            (void)m_memory.write_bytes(
                desc_out, reinterpret_cast<const Byte*>(words.data()),
                sizeof(words));
        }
        m.set_return(0);
        return;
    }
    default:
        unsupported(m, "ID3D11DepthStencilView", slot);
        return;
    }
}

u32 D3D11Manager::release_command_list(GuestAddr address) {
    auto list = m_command_lists.find(address);
    if (list == m_command_lists.end() || list->second.ref_count == 0) return 0;
    if (--list->second.ref_count > 0) return list->second.ref_count;
    // The last reference: the recording's references and memory, and the
    // object. (Its device reference went with the application's last one:
    // retain_object.)
    CommandList dead = std::move(list->second);
    m_command_lists.erase(list);
    if (dead.recording) release_recording(*dead.recording);
    free_object(address);
    return 0;
}

void D3D11Manager::impl_CommandListMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto list = m_command_lists.find(self);
    if (list == m_command_lists.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11COMMANDLIST))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(list->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, list->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, list->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, list->second.private_data));
        return;
    case 7:
        // GetContextFlags returns the flags the deferred context was created
        // with, which is what the guest passed to CreateDeferredContext.
        m.set_return(list->second.context_flags);
        return;
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

u32 D3D11Manager::release_fence(GuestAddr address) {
    auto fence = m_fences.find(address);
    if (fence == m_fences.end()) return 0;
    if (fence->second.ref_count == 0) return 0;
    if (--fence->second.ref_count > 0) return fence->second.ref_count;
    // Events waiting on this fence can no longer be reached.
    GpuEventSignaler::instance().cancel(fence->second.backend.get());
    m_fences.erase(fence);
    free_object(address);
    return 0;
}

void D3D11Manager::impl_FenceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto fence = m_fences.find(self);
    if (fence == m_fences.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11FENCE))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(fence->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, fence->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, fence->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, fence->second.private_data));
        return;
    case 7:
        // CreateSharedHandle. Only a fence created with
        // D3D11_FENCE_FLAG_SHARED has a handle to give, and CreateFence
        // refuses that flag (E_NOTIMPL) until shared timelines exist, so
        // every fence here is unshared: E_INVALIDARG with the handle left
        // untouched (measured).
        m.set_return(d3d11::E_INVALIDARG);
        return;
    case 8: {
        // GetCompletedValue returns UINT64 by value, so it occupies the full
        // 64-bit return register on x64 and the EDX:EAX pair on x86. The
        // marshaler's set_return writes the whole width.
        const u64 value = fence->second.backend
                              ? fence->second.backend->completed_value()
                              : 0ull;
        m.set_return_i64(value);
        return;
    }
    case 9: {
        // SetEventOnCompletion(UINT64 Value, HANDLE hEvent). Learn: when the
        // fence reaches Value the event is signalled; a null hEvent makes the
        // call block until it does.
        usize next = 0;
        const u64 value = read_arg_u64(m, m_is_64bit, 1, next);
        const GuestAddr event = m.arg_ptr(next);
        if (!fence->second.backend) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (event == 0) {
            // Blocking form. A bounded wait, because a fence value the guest
            // will never signal must not wedge the guest thread forever.
            constexpr u64 kTimeoutNs = 5ull * 1000000000ull;
            const bool reached =
                fence->second.backend->wait_cpu(value, kTimeoutNs);
            m.set_return(reached ? d3d11::S_OK : d3d11::E_FAIL);
            return;
        }
        // The event is set once the fence reaches Value, by the process-wide
        // GPU event bridge. Without the bridge wired there is no way to set a
        // guest event, and S_OK on an event that never fires would hang the
        // guest with no diagnostic -- so that case still refuses.
        auto& signaler = GpuEventSignaler::instance();
        if (!signaler.available()) {
            d3d11_diag("ID3D11Fence::SetEventOnCompletion with an event "
                       "handle -> E_NOTIMPL (no event bridge)");
            m.set_return(d3d11::E_NOTIMPL);
            return;
        }
        signaler.signal_when(fence->second.backend.get(), value, event);
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::impl_QueryMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto query = m_queries.find(self);
    if (query == m_queries.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11ASYNCHRONOUS) &&
             !guid_equal(iid, IID_ID3D11QUERY) &&
             !guid_equal(iid, IID_ID3D11QUERY1) &&
             // ID3D11Predicate derives from ID3D11Query and adds no methods, so
             // a predicate answers to both -- but a plain query must not claim
             // the predicate interface, or a guest would pass it to
             // SetPredication after a successful QueryInterface.
             !(query->second.predicate &&
               guid_equal(iid, IID_ID3D11PREDICATE)))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(query->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, query->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, query->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, query->second.private_data));
        return;
    case 7:
        // GetDataSize: the exact DataSize GetData takes for this type
        // (D3D11.3 20.3.1; measured for all 16 types).
        m.set_return(query_data_size(query->second.query_type));
        return;
    case 8: {
        const GuestAddr desc_out = m.arg_ptr(1);
        if (desc_out == 0 ||
            m_memory.write_u32(desc_out, query->second.query_type).is_err() ||
            m_memory.write_u32(desc_out + 4,
                               query->second.misc_flags).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        m.set_return(0);
        return;
    }
    case 9: {
        // ID3D11Query1::GetDesc1: {Query, MiscFlags, ContextType} -- measured.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            (void)m_memory.write_u32(out, query->second.query_type);
            (void)m_memory.write_u32(out + 4, query->second.misc_flags);
            (void)m_memory.write_u32(out + 8, query->second.context_type);
        }
        m.set_return(0);
        return;
    }
    default:
        if (slot < QUERY_METHOD_NAMES.size()) {
            d3d11_diag("ID3D11Query::%s -> E_NOTIMPL",
                       QUERY_METHOD_NAMES[slot]);
        }
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::impl_AnnotationMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    InterfaceType type{};
    if (find_device(self, &type) == nullptr ||
        type != InterfaceType::Annotation) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        m.set_return(query_interface(m, self));
        return;
    case 1:
        m.set_return(add_ref(self));
        return;
    case 2:
        m.set_return(release(self));
        return;
    // BeginEvent / EndEvent: -1, "not running under a Direct3D profiling
    // tool" (Learn; measured on the immediate and deferred facets). The
    // markers still reach the GPU command stream (annotation_marker).
    case 3: // BeginEvent(Name)
    case 4: // EndEvent()
    case 5: { // SetMarker(Name)
        const auto deferred = m_deferred_annotations.find(self);
        GuestAddr context = 0;
        if (deferred != m_deferred_annotations.end()) {
            context = deferred->second;
        } else if (const Device* device = find_device(self)) {
            context = device->context_interface;
        }
        if (context != 0) {
            annotation_marker(context,
                              slot == 3   ? DebugMarkerKind::Push
                              : slot == 4 ? DebugMarkerKind::Pop
                                          : DebugMarkerKind::Signpost,
                              slot == 4 ? 0 : m.arg_ptr(1), nullptr);
        }
        m.set_return(slot == 5 ? 0u : static_cast<u32>(-1));
        return;
    }
    case 6: // GetStatus
        m.set_return(0);
        return;
    default:
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
}

void D3D11Manager::impl_KeyedMutexMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    Texture2D* texture = nullptr;
    GuestAddr texture_address = 0;
    // The facet sits at object + 0x28 of a texture or a buffer.
    if (self >= 0x28) {
        if (auto found = m_textures.find(self - 0x28);
            found != m_textures.end() &&
            found->second.keyed_mutex_interface == self) {
            texture = &found->second;
            texture_address = found->first;
        }
    }
    if (texture == nullptr && self >= 0x28) {
        if (auto found = m_buffers.find(self - 0x28);
            found != m_buffers.end() &&
            found->second.keyed_mutex_interface == self) {
            buffer_keyed_mutex_method(m, slot, found->first);
            return;
        }
    }
    if (texture == nullptr) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        // The texture's graph: QI(IUnknown) of the facet is the texture
        // (COM identity; measured).
        m.set_return(resource_query_interface(m, texture_address));
        return;
    case 1:
        m.set_return(retain_object(texture_address));
        return;
    case 2:
        m.set_return(release_object(texture_address));
        return;
    case 3:
        m.set_return(set_private_data(m, texture->private_data,
                                      texture_address));
        return;
    case 4:
        m.set_return(set_private_data_interface(m, texture->private_data,
                                                texture_address));
        return;
    case 5:
        m.set_return(get_private_data(m, texture->private_data,
                                      texture_address));
        return;
    case 6:
    case 7: {
        m.set_return(query_interface(m, texture->device_owner));
        return;
    }
    case 8: {
        // AcquireSync(UINT64 Key, DWORD dwMilliseconds). The key is two
        // stack slots on x86. The state belongs to the shared resource, so
        // every opening of it -- on any device of any process -- excludes
        // every other (d3d11_sharing.cpp, shared_keyed_mutex.hpp).
        usize next = 0;
        const u64 key = read_arg_u64(m, m_is_64bit, 1, next);
        const u32 timeout = static_cast<u32>(m.arg(next));
        m.set_return(keyed_mutex_acquire(texture->keyed_mutex,
                                         texture->keyed_mutex_opening, key,
                                         timeout));
        return;
    }
    case 9: {
        // ReleaseSync(UINT64 Key): only the owner may release
        // (INVALID_CALL otherwise, measured); the next acquirer must ask for
        // Key.
        usize next = 0;
        const u64 key = read_arg_u64(m, m_is_64bit, 1, next);
        m.set_return(keyed_mutex_release(texture->keyed_mutex,
                                         texture->keyed_mutex_opening, key));
        return;
    }
    default:
        m.set_return(d3d11::S_OK);
        return;
    }
}

void D3D11Manager::buffer_keyed_mutex_method(Marshaler& m, usize slot,
                                             GuestAddr buffer_address) {
    // IDXGIKeyedMutex of a KEYEDMUTEX buffer (measured: a shared buffer
    // answers it and hands ownership across processes, d3d11_share_ers_probe).
    const GuestAddr self = m.arg_ptr(0);
    auto& buffer = m_buffers.find(buffer_address)->second;
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid)) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        GuestAddr result = 0;
        if (guid_equal(iid, IID_IUNKNOWN) ||
            guid_equal(iid, IID_IDXGIOBJECT) ||
            guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) ||
            guid_equal(iid, IID_IDXGIKEYEDMUTEX)) {
            result = self;
        } else {
            result = buffer_interface_for(buffer_address, iid);
        }
        if (result == 0) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        ++buffer.ref_count;
        (void)m.write_ptr(object_out, result);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(++buffer.ref_count);
        return;
    case 2:
        m.set_return(release_buffer(buffer_address));
        return;
    case 3:
        m.set_return(set_private_data(m, buffer.private_data));
        return;
    case 4:
        m.set_return(set_private_data_interface(m, buffer.private_data));
        return;
    case 5:
        m.set_return(get_private_data(m, buffer.private_data));
        return;
    case 6:
    case 7:
        m.set_return(query_interface(m, buffer.device_owner));
        return;
    case 8: {
        usize next = 0;
        const u64 key = read_arg_u64(m, m_is_64bit, 1, next);
        const u32 timeout = static_cast<u32>(m.arg(next));
        m.set_return(keyed_mutex_acquire(buffer.keyed_mutex,
                                         buffer.keyed_mutex_opening, key,
                                         timeout));
        return;
    }
    case 9: {
        usize next = 0;
        const u64 key = read_arg_u64(m, m_is_64bit, 1, next);
        m.set_return(keyed_mutex_release(buffer.keyed_mutex,
                                         buffer.keyed_mutex_opening, key));
        return;
    }
    default:
        m.set_return(d3d11::S_OK);
        return;
    }
}


} // namespace vitreo::winapi
