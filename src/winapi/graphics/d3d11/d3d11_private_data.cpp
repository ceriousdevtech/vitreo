// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_private_data.cpp
/// @brief Private data (Get/SetPrivateData[Interface]) and the guest COM
///        references it holds. Split out of d3d11.cpp as verbatim code motion.
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

u32 D3D11Manager::guest_reference(GuestAddr address, usize slot) {
    const auto caller = m_guest_call;
    DxgiManager::ApiUnlock unlocked(m_dxgi);
    return static_cast<u32>(call_guest_com_slot(
        m_memory, m_is_64bit, caller, address, slot));
}

D3D11Manager::PrivateInterface D3D11Manager::own_private_interface(GuestAddr address) {
    return PrivateInterface(new GuestAddr(address), [this](GuestAddr* owned) {
        const GuestAddr pointer = *owned;
        delete owned;
        if (pointer != 0 && !m_shutting_down)
            (void)guest_reference(pointer, COM_SLOT_RELEASE);
    });
}

void D3D11Manager::set_guest_function_caller(GuestFunctionCaller caller) {
    DxgiManager::ApiLock lock(m_dxgi);
    m_guest_call = std::move(caller);
}

void D3D11Manager::release_private_data_interfaces(GuestAddr address) {
    auto held = m_private_data_interfaces.find(address);
    if (held == m_private_data_interfaces.end()) return;
    // Copy out and erase BEFORE releasing: a guest Release may re-enter this
    // manager and set private data on another object, rehashing this map.
    std::vector<PrivateInterface> interfaces;
    interfaces.reserve(held->second.size());
    for (auto& [name, interface_ptr] : held->second) {
        (void)name;
        interfaces.push_back(std::move(interface_ptr));
    }
    m_private_data_interfaces.erase(held);
    interfaces.clear();
}

// Private data (ID3D11DeviceChild / ID3D11Device / IDXGIObject
// Get/SetPrivateData[Interface]) -- one contract, measured on every object
// and facet (d3d11_a12_object_model_probe [pd]):
//  - bytes and interfaces share one GUID namespace per object, and every
//    facet of the object shares it (a Texture2D, its IDXGIResource and its
//    IDXGISurface; the device and its IDXGIDevice) -- including interface
//    entries, which is why they are keyed by the object (`owner`), not by the
//    facet pointer the call came in on;
//  - Get of an absent GUID: DXGI_ERROR_NOT_FOUND with *pDataSize 0; a NULL
//    pData is a size query that SUCCEEDS with the size written; a too-small
//    buffer is DXGI_ERROR_MORE_DATA with the size written and the buffer
//    untouched; a NULL pDataSize is E_INVALIDARG;
//  - Set(size 0, NULL) deletes and answers S_FALSE when there was nothing to
//    delete; Set(size > 0, NULL) deletes too (Learn: "If pData is NULL,
//    DataSize must also be 0" -- the runtime treats it as the delete);
//    Set(0, non-NULL) stores an empty entry;
//  - SetPrivateDataInterface AddRefs, releases the interface it replaces,
//    and with NULL releases the old one but keeps a NULL entry (Get then
//    answers S_OK with a NULL pointer of pointer size).

namespace {

constexpr u32 kDxgiErrorNotFound = 0x887A0002u;
constexpr u32 kDxgiErrorMoreData = 0x887A0003u;
constexpr u32 kSFalse = 0x00000001u;

} // namespace

u32 D3D11Manager::set_private_data_interface(
    Marshaler& m, PrivateDataStore& private_data, GuestAddr owner) {
    // SetPrivateDataInterface(REFIID Name, const IUnknown *pUnknown).
    //
    // Learn: the object takes a reference on pUnknown and releases it when the
    // object is destroyed, when the same GUID is set again, or when pUnknown is
    // NULL. Those side effects land on an interface the guest implements, so
    // they go through its own vtable -- see winapi/core/guest_com.hpp, including
    // why a Release is allowed to re-enter this manager.
    if (owner == 0) owner = m.arg_ptr(0);
    GuidBytes name{};
    if (!read_guid(m.arg_ptr(1), name) || owner == 0) {
        return d3d11::E_INVALIDARG;
    }
    const GuestAddr interface_ptr = m.arg_ptr(2);
    if (!m_guest_call) {
        // Retaining the pointer without the AddRef would leave this object
        // holding a reference it never took, and reading freed guest memory
        // later.
        return d3d11::E_NOTIMPL;
    }
    // AddRef first: the new and old pointers may be the same, and releasing
    // first could destroy the object being installed.
    if (interface_ptr != 0 &&
        guest_reference(interface_ptr, COM_SLOT_ADD_REF) == 0) {
        return d3d11::E_INVALIDARG;
    }
    // Finish every mutation before any Release. A NULL interface stays as a
    // NULL entry (measured: Get then answers S_OK, pointer-sized).
    auto& held = m_private_data_interfaces[owner];
    PrivateInterface previous;
    if (const auto existing = held.find(name); existing != held.end()) {
        previous = std::move(existing->second);
    }
    held.insert_or_assign(name, own_private_interface(interface_ptr));
    // The two share one GUID namespace, so installing an interface drops any
    // bytes under the same GUID.
    private_data.erase(name);
    // Releasing last. When the same pointer is set twice this drops the
    // reference just taken, so the object keeps exactly one.
    previous.reset();
    return d3d11::S_OK;
}

u32 D3D11Manager::set_private_data(
    Marshaler& m, PrivateDataStore& private_data, GuestAddr owner) {
    if (owner == 0) owner = m.arg_ptr(0);
    GuidBytes name{};
    const GuestAddr name_address = m.arg_ptr(1);
    const u32 data_size = static_cast<u32>(m.arg(2));
    const GuestAddr data_address = m.arg_ptr(3);
    if (!read_guid(name_address, name)) return d3d11::E_INVALIDARG;
    // Learn: private data and private *interfaces* share one GUID namespace, so
    // setting or clearing by GUID drops whichever the GUID held.
    const auto drop_interface = [&]() {
        auto held = m_private_data_interfaces.find(owner);
        if (held == m_private_data_interfaces.end()) return false;
        const auto existing = held->second.find(name);
        if (existing == held->second.end()) return false;
        auto previous = std::move(existing->second);
        held->second.erase(existing);
        if (held->second.empty()) m_private_data_interfaces.erase(held);
        previous.reset();
        return true;
    };
    if (data_address == 0) {
        // A delete, whatever the size (measured: size 4 with NULL data is
        // S_OK and removes the entry). Nothing to delete is S_FALSE.
        const bool had_bytes = private_data.erase(name) != 0;
        const bool had_interface = drop_interface();
        return had_bytes || had_interface ? d3d11::S_OK : kSFalse;
    }
    std::vector<u8> bytes(data_size);
    if (data_size != 0 &&
        m_memory.read_bytes(data_address, bytes.data(), bytes.size())
            .is_err()) {
        return d3d11::E_INVALIDARG;
    }
    // Size 0 with data stores an empty entry (measured: Get answers S_OK
    // with size 0).
    private_data.insert_or_assign(name, std::move(bytes));
    (void)drop_interface();
    return d3d11::S_OK;
}

u32 D3D11Manager::get_private_data(
    Marshaler& m, PrivateDataStore& private_data, GuestAddr owner) {
    if (owner == 0) owner = m.arg_ptr(0);
    GuidBytes name{};
    const GuestAddr size_address = m.arg_ptr(2);
    const GuestAddr data_address = m.arg_ptr(3);
    if (!read_guid(m.arg_ptr(1), name) || size_address == 0) {
        return d3d11::E_INVALIDARG;
    }
    // Learn: GetPrivateData on a GUID holding an interface writes the pointer
    // and AddRefs it; the required size is one pointer.
    const auto held_by = m_private_data_interfaces.find(owner);
    if (held_by != m_private_data_interfaces.end()) {
        const auto held = held_by->second.find(name);
        if (held != held_by->second.end()) {
            const u32 pointer_size = m_is_64bit ? 8u : 4u;
            const auto capacity = m.read_u32(size_address);
            (void)m.write_u32(size_address, pointer_size);
            if (data_address == 0) return d3d11::S_OK;
            if (capacity.is_err() || capacity.value() < pointer_size) {
                return kDxgiErrorMoreData;
            }
            const auto lease = held->second;
            const GuestAddr interface_ptr = *lease;
            if (m.write_ptr(data_address, interface_ptr).is_err()) {
                return d3d11::E_INVALIDARG;
            }
            // The caller owns this reference. Taken after every write, since the
            // guest AddRef may re-enter.
            if (interface_ptr != 0) {
                (void)guest_reference(interface_ptr, COM_SLOT_ADD_REF);
            }
            return d3d11::S_OK;
        }
    }
    const auto value = private_data.find(name);
    if (value == private_data.end()) {
        (void)m.write_u32(size_address, 0);
        return kDxgiErrorNotFound;
    }
    const auto capacity = m.read_u32(size_address);
    const u32 required = static_cast<u32>(value->second.size());
    (void)m.write_u32(size_address, required);
    if (data_address == 0) return d3d11::S_OK;
    if (capacity.is_err() || capacity.value() < required) {
        return kDxgiErrorMoreData;
    }
    if (required != 0 &&
        m_memory.write_bytes(data_address, value->second.data(),
                             value->second.size()).is_err()) {
        return d3d11::E_INVALIDARG;
    }
    return d3d11::S_OK;
}

} // namespace vitreo::winapi
