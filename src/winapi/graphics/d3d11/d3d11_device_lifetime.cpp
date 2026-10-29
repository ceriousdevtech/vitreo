// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_device_lifetime.cpp
/// @brief The device object: its facets, allocation and lookup, AddRef, Release
///        and destruction, and QueryInterface. Split out of d3d11.cpp as
///        verbatim code motion.
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

bool D3D11Manager::is_device_interface(GuestAddr address) const {
    const auto interface = m_interfaces.find(address);
    return interface != m_interfaces.end() &&
           (interface->second.type == InterfaceType::Device ||
            interface->second.type == InterfaceType::D3D10Device ||
            interface->second.type == InterfaceType::DxgiDevice);
}

bool D3D11Manager::retain_device_reference(GuestAddr address) {
    if (!is_device_interface(address)) return false;
    return add_ref(address) != 0;
}

void D3D11Manager::release_device_interface_reference(GuestAddr address) {
    (void)release(address);
}

GuestAddr D3D11Manager::allocate_device(GuestAddr adapter,
                                        u32 feature_level,
                                        u32 creation_flags) {
    if (!ensure_storage() ||
        m_next_object + 0x100 > m_region_base + STORAGE_SIZE) {
        if (adapter != 0) m_dxgi.release_adapter(adapter);
        return 0;
    }

    const GuestAddr owner = m_next_object;
    m_next_object += 0x100;
    const GuestAddr device_interface = owner;
    const GuestAddr context_interface = owner + 0x20;
    const GuestAddr dxgi_device_interface = owner + 0x40;
    const GuestAddr annotation_interface = owner + 0x60;
    const GuestAddr video_context_interface = owner + 0x80;
    // Learn (ID3D11Multithread; D3D11_CREATE_DEVICE_SINGLETHREADED): a
    // single-threaded device has no critical section to expose, and refuses
    // the interface (measured E_NOINTERFACE).
    const bool multithread =
        (creation_flags & d3d11::CREATE_DEVICE_SINGLETHREADED) == 0;
    const GuestAddr device_multithread_interface =
        multithread ? owner + 0xA0 : 0;
    const GuestAddr context_multithread_interface =
        multithread ? owner + 0xC0 : 0;
    // The ID3D10Device1 facet exists on every device; QueryInterface
    // answers it only once the device speaks D3D10 (Device::d3d10_*).
    const GuestAddr d3d10_device_interface =
        m_d3d10_ready ? owner + kD3D10DeviceFacetOffset : 0;
    if (!write_pointer(device_interface, m_device_vtable) ||
        !write_pointer(context_interface, m_context_vtable) ||
        !write_pointer(dxgi_device_interface, m_dxgi_device_vtable) ||
        !write_pointer(annotation_interface, m_annotation_vtable) ||
        !write_pointer(video_context_interface, m_video_context_vtable) ||
        (multithread &&
         (!write_pointer(device_multithread_interface,
                         m_multithread_vtable) ||
          !write_pointer(context_multithread_interface,
                         m_multithread_vtable))) ||
        (d3d10_device_interface != 0 &&
         !write_pointer(d3d10_device_interface, m_d3d10_device_vtable))) {
        if (adapter != 0) m_dxgi.release_adapter(adapter);
        return 0;
    }

    Device device;
    device.device_interface = device_interface;
    device.context_interface = context_interface;
    device.dxgi_device_interface = dxgi_device_interface;
    device.annotation_interface = annotation_interface;
    device.video_context_interface = video_context_interface;
    device.device_multithread_interface = device_multithread_interface;
    device.context_multithread_interface = context_multithread_interface;
    device.d3d10_device_interface = d3d10_device_interface;
    device.adapter = adapter;
    device.feature_level = feature_level;
    device.creation_flags = creation_flags;
    m_devices.emplace(owner, std::move(device));
    m_interfaces.emplace(device_interface,
                         Interface{owner, InterfaceType::Device});
    m_interfaces.emplace(context_interface,
                         Interface{owner, InterfaceType::Context});
    m_interfaces.emplace(dxgi_device_interface,
                         Interface{owner, InterfaceType::DxgiDevice});
    m_interfaces.emplace(annotation_interface,
                         Interface{owner, InterfaceType::Annotation});
    m_interfaces.emplace(video_context_interface,
                         Interface{owner, InterfaceType::VideoContext});
    if (d3d10_device_interface != 0) {
        m_interfaces.emplace(d3d10_device_interface,
                             Interface{owner, InterfaceType::D3D10Device});
    }
    if (multithread) {
        m_interfaces.emplace(
            device_multithread_interface,
            Interface{owner, InterfaceType::DeviceMultithread});
        m_interfaces.emplace(
            context_multithread_interface,
            Interface{owner, InterfaceType::ContextMultithread});
    }
    // Binding state belongs to the CONTEXT, not the device. It used to be keyed
    // by the device, which was harmless only because CreateDeferredContext
    // returned E_NOTIMPL and a device therefore had exactly one context. A
    // deferred context has its own bindings, so two contexts on one device must
    // not share a state entry.
    m_context_states.emplace(context_interface, ContextState{});
    return owner;
}

D3D11Manager::Device* D3D11Manager::find_device(
    GuestAddr interface_address, InterfaceType* type) {
    const auto interface = m_interfaces.find(interface_address);
    if (interface == m_interfaces.end()) return nullptr;
    auto device = m_devices.find(interface->second.owner);
    if (device == m_devices.end()) return nullptr;
    if (type != nullptr) *type = interface->second.type;
    return &device->second;
}

namespace {

/// The facets counted with, and QueryInterface'd as, the immediate context.
[[nodiscard]] constexpr bool is_context_facet(auto type) {
    using T = decltype(type);
    return type == T::Context || type == T::Annotation ||
           type == T::ContextMultithread || type == T::VideoContext;
}

} // namespace

u32 D3D11Manager::add_ref(GuestAddr interface_address) {
    // A deferred context (and its annotation facet) counts on its own.
    if (const GuestAddr deferred = deferred_context_of(interface_address)) {
        return deferred_add_ref(deferred);
    }
    InterfaceType type{};
    Device* device = find_device(interface_address, &type);
    if (device == nullptr) return 0;
    if (is_context_facet(type)) {
        // The immediate context is a device child like any other: while the
        // application holds it, it holds the device (measured, a12 refmodel
        // [ctx]: GetImmediateContext on a device made without
        // ppImmediateContext takes the device count 1 -> 2; releasing the
        // context takes it back to 1; a second context reference adds none).
        if (device->context_ref_count++ == 0) ++device->device_ref_count;
        return device->context_ref_count;
    }
    return ++device->device_ref_count;
}

u32 D3D11Manager::release(GuestAddr interface_address) {
    if (const GuestAddr deferred = deferred_context_of(interface_address)) {
        return deferred_release(deferred);
    }
    const auto interface = m_interfaces.find(interface_address);
    if (interface == m_interfaces.end()) return 0;
    const GuestAddr owner = interface->second.owner;
    Device* device = find_device(interface_address);
    if (device == nullptr) return 0;

    // A Release past zero is the application's bug; it must not take a
    // reference the runtime (or another facet) holds.
    u32 count = 0;
    if (is_context_facet(interface->second.type)) {
        if (device->context_ref_count == 0) return 0;
        count = --device->context_ref_count;
        if (count == 0 && device->device_ref_count != 0) {
            --device->device_ref_count;
        }
    } else {
        if (device->device_ref_count == 0) return 0;
        count = --device->device_ref_count;
    }
    destroy_if_unreferenced(owner);
    return count;
}

void D3D11Manager::destroy_if_unreferenced(GuestAddr owner) {
    auto device = m_devices.find(owner);
    if (device == m_devices.end() ||
        device->second.device_ref_count != 0 ||
        device->second.context_ref_count != 0) {
        return;
    }
    const Device dead = device->second;
    // Erases below: the context-record memo must not answer for them.
    invalidate_context_lookups();
    m_interfaces.erase(dead.device_interface);
    m_interfaces.erase(dead.context_interface);
    m_interfaces.erase(dead.dxgi_device_interface);
    m_interfaces.erase(dead.annotation_interface);
    m_interfaces.erase(dead.video_context_interface);
    m_interfaces.erase(dead.device_multithread_interface);
    m_interfaces.erase(dead.context_multithread_interface);
    m_interfaces.erase(dead.d3d10_device_interface);
    m_devices.erase(device);
    if (dead.adapter != 0) m_dxgi.release_adapter(dead.adapter);
    // The application holds no child of this device any more (each would
    // hold a device reference), so whatever is left is the runtime's own:
    // the immediate context's bindings and its active context state. Drop
    // those references, which destroys the objects only they kept alive.
    clear_context_state(dead.context_interface);
    if (auto active = m_active_context_state.find(dead.context_interface);
        active != m_active_context_state.end()) {
        const GuestAddr state = active->second;
        m_active_context_state.erase(active);
        if (state != 0) release_internal(state);
    }
    // (Again: the releases above can run guest code that calls back in.)
    invalidate_context_lookups();
    m_context_states.erase(dead.context_interface);
    // The device's state-object pools go with it: live objects stay valid
    // but are no longer found by description, and the runtime's own default
    // objects (StatePoolEntry), which only the device kept, are freed.
    // Freeing may release a private-data interface through the guest, which
    // can re-enter this manager, so the maps are settled first.
    std::vector<GuestAddr> unreferenced;
    const auto drop_pool = [this, owner, &unreferenced](auto& objects) {
        for (auto it = objects.begin(); it != objects.end();) {
            if (it->second.device_owner != owner) {
                ++it;
                continue;
            }
            m_state_pool.erase(it->second.pool.pool_key);
            it->second.pool = StatePoolEntry{};
            if (it->second.ref_count == 0) {
                unreferenced.push_back(it->first);
                it = objects.erase(it);
            } else {
                ++it;
            }
        }
    };
    drop_pool(m_blend_states);
    drop_pool(m_rasterizer_states);
    drop_pool(m_depth_stencil_states);
    drop_pool(m_sampler_states);
    ++m_sampler_erase_generation;  // drop_pool may have erased samplers
    ++m_sampler_set_generation;
    for (auto it = m_state_pool_counts.begin();
         it != m_state_pool_counts.end();) {
        it = it->first.first == owner ? m_state_pool_counts.erase(it)
                                      : std::next(it);
    }
    for (const GuestAddr address : unreferenced) free_object(address);
    // Learn (SetPrivateDataInterface): destroying the object releases the
    // interfaces it holds -- the device's and the immediate context's.
    release_private_data_interfaces(dead.device_interface);
    release_private_data_interfaces(dead.context_interface);
}

u32 D3D11Manager::query_interface(Marshaler& m,
                                  GuestAddr interface_address) {
    if (const GuestAddr deferred = deferred_context_of(interface_address)) {
        return deferred_query_interface(m, deferred);
    }
    InterfaceType source_type{};
    Device* device = find_device(interface_address, &source_type);
    const GuestAddr iid_address = m.arg_ptr(1);
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) return d3d11::E_POINTER;
    (void)m.write_ptr(object_out, 0);
    GuidBytes iid{};
    if (device == nullptr || !read_guid(iid_address, iid)) {
        return d3d11::E_NOINTERFACE;
    }

    // ID3D11Multithread and ID3D10Multithread are one facet per owner
    // (measured: the same pointer, QI(IUnknown) of it is the owner).
    const bool multithread = guid_equal(iid, IID_ID3D11MULTITHREAD) ||
                             guid_equal(iid, IID_ID3D10MULTITHREAD);
    GuestAddr result = 0;
    if (is_context_facet(source_type)) {
        if (guid_equal(iid, IID_IUNKNOWN) ||
            guid_equal(iid, IID_ID3D11DEVICECONTEXT) ||
            guid_equal(iid, IID_ID3D11DEVICECONTEXT1) ||
            guid_equal(iid, IID_ID3D11DEVICECONTEXT2) ||
            guid_equal(iid, IID_ID3D11DEVICECONTEXT3) ||
            guid_equal(iid, IID_ID3D11DEVICECONTEXT4) ||
            guid_equal(iid, IID_ID3D11DEVICECHILD)) {
            result = device->context_interface;
        } else if (guid_equal(iid, IID_ID3DUSERDEFINEDANNOTATION)) {
            result = device->annotation_interface;
        } else if (multithread) {
            result = device->context_multithread_interface;
        } else if (guid_equal(iid, IID_ID3D11VIDEOCONTEXT) ||
                   guid_equal(iid, IID_ID3D11VIDEOCONTEXT1) ||
                   guid_equal(iid, IID_ID3D11VIDEOCONTEXT2) ||
                   guid_equal(iid, IID_ID3D11VIDEOCONTEXT3)) {
            // Measured on Windows (d3d11_video_formats_probe [video];
            // a12 [qi]): the immediate context answers every
            // ID3D11VideoContext tier while the device answers no
            // ID3D11VideoDevice, with or without
            // D3D11_CREATE_DEVICE_VIDEO_SUPPORT.
            result = device->video_context_interface;
        }
    } else if (guid_equal(iid, IID_IUNKNOWN) ||
               guid_equal(iid, IID_ID3D11DEVICE) ||
               guid_equal(iid, IID_ID3D11DEVICE1) ||
               guid_equal(iid, IID_ID3D11DEVICE2) ||
               guid_equal(iid, IID_ID3D11DEVICE3) ||
               guid_equal(iid, IID_ID3D11DEVICE4) ||
               guid_equal(iid, IID_ID3D11DEVICE5)) {
        // COM identity requires every interface's IUnknown query to return
        // the same controlling unknown. Device2–5 share Device1's object;
        // extended methods return E_NOTIMPL until implemented.
        result = device->device_interface;
    } else if (guid_equal(iid, IID_IDXGIOBJECT) ||
               guid_equal(iid, IID_IDXGIDEVICE) ||
               guid_equal(iid, IID_IDXGIDEVICE1) ||
               guid_equal(iid, IID_IDXGIDEVICE2) ||
               guid_equal(iid, IID_IDXGIDEVICE3) ||
               guid_equal(iid, IID_IDXGIDEVICE4)) {
        // The device answers every IDXGIDevice tier and their base
        // IDXGIObject (measured); the DXGI facet's vtable is IDXGIDevice4's.
        result = device->dxgi_device_interface;
    } else if (multithread) {
        result = device->device_multithread_interface;
    } else if (guid_equal(iid, IID_ID3D10DEVICE) ||
               guid_equal(iid, IID_ID3D10DEVICE1)) {
        // One facet for both (measured: the same pointer), and only once
        // the device speaks D3D10: created by D3D10CreateDevice(1), or after
        // its first CreateDeviceContextState (measured, d3d10_contract_probe
        // [emu]).
        if (device->d3d10_interfaces) result = device->d3d10_device_interface;
    }
    if (result == 0) {
        d3d11_diag(
            "QueryInterface self=0x%llx "
            "iid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x -> E_NOINTERFACE",
            static_cast<unsigned long long>(interface_address),
            iid[3], iid[2], iid[1], iid[0], iid[5], iid[4],
            iid[7], iid[6], iid[8], iid[9], iid[10], iid[11],
            iid[12], iid[13], iid[14], iid[15]);
        return d3d11::E_NOINTERFACE;
    }
    (void)add_ref(result);
    if (m.write_ptr(object_out, result).is_err()) {
        (void)release(result);
        return d3d11::E_POINTER;
    }
    d3d11_diag("QueryInterface self=0x%llx -> 0x%llx",
               static_cast<unsigned long long>(interface_address),
               static_cast<unsigned long long>(result));
    return d3d11::S_OK;
}

} // namespace vitreo::winapi
