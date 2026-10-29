// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_dxgi_device_methods.cpp
/// @brief The IDXGIDevice facet of the D3D11 device.
///
/// A caller reaches IDXGIDevice by QueryInterface on the device, and it
/// shares the device's storage. Split out of d3d11_device_methods.cpp.

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

void D3D11Manager::impl_DxgiDeviceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    Device* device = find_device(self);
    if (device == nullptr) {
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
    // IDXGIDevice shares the device's store, interface entries included
    // (measured: set through ID3D11Device, read through IDXGIDevice).
    case 3:
        m.set_return(set_private_data(m, device->private_data,
                                      device->device_interface));
        return;
    case 4:
        m.set_return(set_private_data_interface(m, device->private_data,
                                                device->device_interface));
        return;
    case 5:
        m.set_return(get_private_data(m, device->private_data,
                                      device->device_interface));
        return;
    case 6:
        m.set_return(m_dxgi.query_object_interface(
            device->adapter, m.arg_ptr(1), m.arg_ptr(2)));
        return;
    case 7: {
        const GuestAddr adapter_out = m.arg_ptr(1);
        if (adapter_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(adapter_out, 0);
        if (!m_dxgi.retain_adapter(device->adapter) ||
            m.write_ptr(adapter_out, device->adapter).is_err()) {
            m_dxgi.release_adapter(device->adapter);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 10: {
        const i32 priority = static_cast<i32>(m.arg(1));
        if (priority < -7 || priority > 7) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        device->gpu_thread_priority = priority;
        m.set_return(d3d11::S_OK);
        return;
    }
    case 11: {
        const GuestAddr priority_out = m.arg_ptr(1);
        if (priority_out == 0 ||
            m.write_u32(priority_out,
                        static_cast<u32>(device->gpu_thread_priority)).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 12: {
        // SetMaximumFrameLatency(MaxLatency): 0..16; 0 (Learn: "reset to the
        // default") is stored and read back as 0, and 17 is INVALID_CALL
        // (measured, dxgi_a11_contracts_probe [dxgidevice]).
        const u32 latency = static_cast<u32>(m.arg(1));
        if (latency > 16) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        device->maximum_frame_latency = latency;
        m.set_return(d3d11::S_OK);
        return;
    }
    case 13: {
        const GuestAddr latency_out = m.arg_ptr(1);
        if (latency_out == 0 ||
            m.write_u32(latency_out,
                        device->maximum_frame_latency).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 14:
    case 18: {
        // OfferResources(NumResources, ppResources, Priority) and
        // IDXGIDevice4::OfferResources1(..., Priority, Flags). Offering lets
        // the OS discard the contents under memory pressure; Vitreo never
        // discards, so a reclaim always finds them intact -- exactly what a
        // system without memory pressure reports. Measured: priority 0 (or
        // past HIGH) is E_INVALIDARG; Flags may only be ALLOW_DECOMMIT.
        const u32 count = static_cast<u32>(m.arg(1));
        const GuestAddr resources = m.arg_ptr(2);
        const u32 priority = static_cast<u32>(m.arg(3));
        const u32 flags = slot == 18 ? static_cast<u32>(m.arg(4)) : 0u;
        constexpr u32 kOfferPriorityLow = 1;
        constexpr u32 kOfferPriorityHigh = 3;
        constexpr u32 kOfferAllowDecommit = 0x1;
        if (priority < kOfferPriorityLow || priority > kOfferPriorityHigh ||
            (flags & ~kOfferAllowDecommit) != 0 ||
            (count != 0 && resources == 0)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<bool*> offered;
        for (u32 i = 0; i < count; ++i) {
            GuestAddr resource = 0;
            (void)read_pointer(resources + i * (m_is_64bit ? 8u : 4u),
                               resource);
            bool* flag = dxgi_resource_offered_flag(resource);
            if (flag == nullptr) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            offered.push_back(flag);
        }
        for (bool* flag : offered) *flag = true;
        m.set_return(d3d11::S_OK);
        return;
    }
    case 15:
    case 19: {
        // ReclaimResources(NumResources, ppResources, BOOL* pDiscarded) and
        // ReclaimResources1(..., DXGI_RECLAIM_RESOURCE_RESULTS* pResults). A
        // resource that is not offered is E_INVALIDARG (measured); every
        // reclaimed resource still has its contents (FALSE / RESULT_OK).
        const u32 count = static_cast<u32>(m.arg(1));
        const GuestAddr resources = m.arg_ptr(2);
        const GuestAddr results = m.arg_ptr(3);
        if (count != 0 && resources == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<bool*> reclaimed;
        for (u32 i = 0; i < count; ++i) {
            GuestAddr resource = 0;
            (void)read_pointer(resources + i * (m_is_64bit ? 8u : 4u),
                               resource);
            bool* flag = dxgi_resource_offered_flag(resource);
            if (flag == nullptr || !*flag) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            reclaimed.push_back(flag);
        }
        for (u32 i = 0; i < count; ++i) {
            *reclaimed[i] = false;
            if (results != 0) (void)m_memory.write_u32(results + i * 4u, 0);
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 16: {
        // EnqueueSetEvent(hEvent): flush, and set the event once everything
        // submitted so far has completed (Learn). NULL is E_INVALIDARG
        // (measured).
        const GuestAddr event = m.arg_ptr(1);
        if (event == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        auto& signaler = GpuEventSignaler::instance();
        if (!signaler.available()) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED dxgi!IDXGIDevice2::"
                             "EnqueueSetEvent: no event bridge -> E_FAIL\n");
            }
            m.set_return(d3d11::E_FAIL);
            return;
        }
        signaler.signal_after(m_backend.flush_completion(), event);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 8: {
        // CreateSurface(pDesc, NumSurfaces, Usage, pSharedResource, ppSurface):
        // a single-subresource 2D texture, returned as its IDXGISurface --
        // measured working on Windows.
        const GuestAddr desc_address = m.arg_ptr(1);
        const u32 count = static_cast<u32>(m.arg(2));
        const u32 usage = static_cast<u32>(m.arg(3));
        const GuestAddr shared = m.arg_ptr(4);
        const GuestAddr surface_out = m.arg_ptr(5);
        if (surface_out != 0) (void)m.write_ptr(surface_out, 0);
        if (desc_address == 0 || surface_out == 0 || count == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (shared != 0) {
            // Learn CreateSurface: pSharedResource is reserved (NULL).
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::array<u32, 5> words{};
        if (m_memory.read_bytes(desc_address, reinterpret_cast<Byte*>(words.data()),
                                sizeof(words)).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        Texture2DDesc desc{};
        desc.width = words[0];
        desc.height = words[1];
        desc.format = words[2];
        desc.sample_count = words[3] == 0 ? 1u : words[3];
        desc.sample_quality = words[4];
        // DXGI_USAGE: SHADER_INPUT 0x10, RENDER_TARGET_OUTPUT 0x20,
        // UNORDERED_ACCESS 0x400; the low bits are the CPU access mode.
        if ((usage & 0x10u) != 0) desc.bind_flags |= D3D11_BIND_SHADER_RESOURCE;
        if ((usage & 0x20u) != 0) desc.bind_flags |= D3D11_BIND_RENDER_TARGET;
        if ((usage & 0x400u) != 0) desc.bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
        switch (usage & 0xFu) {
        case 1:  // DXGI_CPU_ACCESS_DYNAMIC
            desc.usage = D3D11_USAGE_DYNAMIC;
            desc.cpu_access_flags = D3D11_CPU_ACCESS_WRITE;
            break;
        case 2:  // DXGI_CPU_ACCESS_READ_WRITE
        case 3:  // DXGI_CPU_ACCESS_SCRATCH
            desc.usage = D3D11_USAGE_STAGING;
            desc.cpu_access_flags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            desc.bind_flags = 0;
            break;
        default:
            desc.usage = D3D11_USAGE_DEFAULT;
            break;
        }
        const GuestAddr owner = find_device(self) != nullptr
            ? m_interfaces.find(self)->second.owner : 0;
        // ppSurface is an array of NumSurfaces pointers, each its own
        // surface (measured: two of two filled).
        const usize ptr = m_is_64bit ? 8u : 4u;
        std::vector<GuestAddr> made;
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr texture = create_texture_2d(owner, desc, nullptr, 0);
            auto created = m_textures.find(texture);
            if (texture == 0 || created == m_textures.end()) {
                for (const GuestAddr dead : made) (void)release_object(dead);
                for (u32 j = 0; j < count; ++j) {
                    (void)write_pointer(surface_out + j * ptr, 0);
                }
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            made.push_back(texture);
            (void)write_pointer(surface_out + i * ptr,
                                created->second.dxgi_surface_interface);
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 9: {
        // QueryResourceResidency(ppResources, pResidencyStatus, NumResources).
        // Nothing is ever evicted: every resource is resident. Where it is
        // resident follows the discrete-class adapter profile
        // (.docs/contracts/dxgi.md "Adapter profile policy"): a resource with
        // GPU storage is in the dedicated pool, FULLY_RESIDENT ("located in
        // video memory", Learn DXGI_RESIDENCY); one held only in CPU memory
        // (a STAGING texture) is RESIDENT_IN_SHARED_MEMORY
        // (dxgi_resource_residency). Windows, a UMA adapter, answers
        // RESIDENT_IN_SHARED_MEMORY for a DEFAULT texture
        // (dxgi_a11_contracts_probe); a discrete adapter keeps such a
        // texture in video memory. An entry that is not a resource of this
        // device's frontend is E_INVALIDARG, as Offer/ReclaimResources
        // answer, and no status is written.
        const GuestAddr resources = m.arg_ptr(1);
        const GuestAddr status = m.arg_ptr(2);
        const u32 count = static_cast<u32>(m.arg(3));
        if (resources == 0 || status == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<u32> residency;
        residency.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            GuestAddr resource = 0;
            (void)read_pointer(resources + i * (m_is_64bit ? 8u : 4u),
                               resource);
            const std::optional<u32> where = dxgi_resource_residency(resource);
            if (!where) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            residency.push_back(*where);
        }
        for (u32 i = 0; i < count; ++i) {
            (void)m_memory.write_u32(status + i * 4, residency[i]);
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 17:
        // IDXGIDevice3::Trim (void): nothing is held back to release.
        m.set_return(0);
        return;
    default:
        unsupported(m, "IDXGIDevice", slot);
        return;
    }
}

} // namespace vitreo::winapi
