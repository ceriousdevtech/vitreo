// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_object_methods.cpp
/// @brief The IUnknown and IDXGIObject methods every DXGI object shares
///        (QueryInterface, AddRef, Release, private data, GetParent).
///        Split out of dxgi.cpp as verbatim code motion.
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/dxgi/dxgi_internal.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/guest_com.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/common/virtual_display_modes.hpp"
#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/common/live_diag.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {

void DxgiManager::impl_QueryInterface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) {
        m.set_return(dxgi::E_POINTER);
        return;
    }
    (void)m.write_ptr(object_out, 0);
    auto object = m_objects.find(self);
    if (object == m_objects.end() ||
        !iid_is_supported(iid, object->second.type)) {
        GuidBytes requested{};
        if (read_guid(iid, requested)) {
            dxgi_diag(
                "IUnknown::QueryInterface self=0x%llx -> E_NOINTERFACE "
                "iid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned long long>(self),
                requested[3], requested[2], requested[1], requested[0],
                requested[5], requested[4], requested[7], requested[6],
                requested[8], requested[9], requested[10], requested[11],
                requested[12], requested[13], requested[14],
                requested[15]);
        } else {
            dxgi_diag(
                "IUnknown::QueryInterface self=0x%llx -> E_NOINTERFACE",
                static_cast<unsigned long long>(self));
        }
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }
    ++object->second.ref_count;
    if (m.write_ptr(object_out, self).is_err()) {
        --object->second.ref_count;
        m.set_return(dxgi::E_POINTER);
        return;
    }
    GuidBytes requested{};
    if (read_guid(iid, requested)) {
        dxgi_diag("IUnknown::QueryInterface self=0x%llx -> self "
                  "iid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long long>(self),
                  requested[3], requested[2], requested[1], requested[0],
                  requested[5], requested[4], requested[7], requested[6],
                  requested[8], requested[9], requested[10], requested[11],
                  requested[12], requested[13], requested[14],
                  requested[15]);
    } else {
        dxgi_diag("IUnknown::QueryInterface self=0x%llx -> self",
                  static_cast<unsigned long long>(self));
    }
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_AddRef(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    m.set_return(++object->second.ref_count);
}

void DxgiManager::impl_Release(Marshaler& m) {
    m.set_return(release_object(m.arg_ptr(0)));
}

void DxgiManager::impl_SetPrivateData(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr name_address = m.arg_ptr(1);
    const u32 data_size = static_cast<u32>(m.arg(2));
    const GuestAddr data_address = m.arg_ptr(3);
    auto object = m_objects.find(self);
    GuidBytes name{};
    if (object == m_objects.end() || !read_guid(name_address, name)) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // The private-data contract every D3D11 / DXGI object follows (measured,
    // d3d11_a12_object_model_probe [pd]; the d3d11 side is documented at
    // D3D11Manager::set_private_data_interface): NULL data deletes whatever
    // the size says, and deleting nothing is S_FALSE; size 0 with data
    // stores an empty entry.
    if (data_address == 0) {
        const bool had_bytes = object->second.private_data.erase(name) != 0;
        // The two share one GUID namespace, so clearing by GUID clears either.
        const bool had_interface =
            object->second.private_data_interfaces.contains(name);
        auto dropped = take_private_interface(self, name);
        dropped.reset();
        m.set_return(had_bytes || had_interface ? dxgi::S_OK : 1u);
        return;
    }
    std::vector<u8> bytes(data_size);
    if (data_size != 0 &&
        m_memory.read_bytes(data_address, bytes.data(), bytes.size())
            .is_err()) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    object->second.private_data.insert_or_assign(name, std::move(bytes));
    // Learn: setting data under a GUID that previously held an interface
    // releases that interface. Mutations are finished, so releasing is safe now.
    auto replaced = take_private_interface(self, name);
    replaced.reset();
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_SetPrivateDataInterface(Marshaler& m) {
    // SetPrivateDataInterface(REFIID Name, const IUnknown *pUnknown).
    //
    // Learn: the object takes a reference on pUnknown and releases it when the
    // object is destroyed, when the same GUID is set again, or when pUnknown is
    // NULL. Those are real COM side effects on an interface the guest
    // implements, so they go through the guest's own vtable.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr name_address = m.arg_ptr(1);
    const GuestAddr interface_ptr = m.arg_ptr(2);
    GuidBytes name{};
    auto object = m_objects.find(self);
    if (object == m_objects.end() || !read_guid(name_address, name)) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (!m_guest_call) {
        // Retaining the pointer without the AddRef would leave this object
        // holding a reference it never took, and reading freed guest memory
        // later. Refusing is the honest answer when the host has not wired the
        // call.
        m.set_return(dxgi::E_NOTIMPL);
        return;
    }
    // AddRef the new interface BEFORE dropping the old one: they may be the same
    // pointer, and releasing first could destroy the object being installed.
    if (interface_ptr != 0 &&
        guest_reference(interface_ptr, COM_SLOT_ADD_REF) == 0) {
        // A vtable that cannot be read, or an AddRef that reported no
        // references, is not an interface this object can hold.
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // Every mutation happens here, before any Release, because a guest Release
    // may re-enter this manager and invalidate `object`.
    auto previous = take_private_interface(self, name);
    object = m_objects.find(self);
    if (object == m_objects.end()) {
        // The AddRef above re-entered and destroyed this object. Undo the
        // reference rather than leak it.
        if (interface_ptr != 0) {
            (void)guest_reference(interface_ptr, COM_SLOT_RELEASE);
        }
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    object->second.private_data.erase(name);
    // A NULL interface stays as a NULL entry (measured: Get then answers
    // S_OK with a NULL pointer).
    object->second.private_data_interfaces.insert_or_assign(
        name, own_private_interface(interface_ptr));
    // Release the old interface last. When the same pointer is set twice this
    // drops the reference just taken, so the object keeps exactly one rather
    // than accumulating.
    previous.reset();
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_GetPrivateData(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr name_address = m.arg_ptr(1);
    const GuestAddr size_address = m.arg_ptr(2);
    const GuestAddr data_address = m.arg_ptr(3);
    auto object = m_objects.find(self);
    GuidBytes name{};
    if (object == m_objects.end() || !read_guid(name_address, name) ||
        size_address == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // Learn: GetPrivateData with a GUID that holds an interface writes the
    // interface pointer and AddRefs it, and the required size is a pointer.
    const auto held = object->second.private_data_interfaces.find(name);
    if (held != object->second.private_data_interfaces.end()) {
        const u32 pointer_size = m_is_64bit ? 8u : 4u;
        const auto capacity = m.read_u32(size_address);
        (void)m.write_u32(size_address, pointer_size);
        // A NULL pData is the size query, and it succeeds (measured).
        if (data_address == 0) {
            m.set_return(dxgi::S_OK);
            return;
        }
        if (capacity.is_err() || capacity.value() < pointer_size) {
            m.set_return(0x887A0003u);  // DXGI_ERROR_MORE_DATA
            return;
        }
        const auto lease = held->second;
        const GuestAddr interface_ptr = *lease;
        if (m.write_ptr(data_address, interface_ptr).is_err()) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        // The caller owns this reference. Taken last, after every write, since
        // the guest AddRef may re-enter.
        if (interface_ptr != 0) {
            (void)guest_reference(interface_ptr, COM_SLOT_ADD_REF);
        }
        m.set_return(dxgi::S_OK);
        return;
    }
    const auto value = object->second.private_data.find(name);
    if (value == object->second.private_data.end()) {
        (void)m.write_u32(size_address, 0);
        m.set_return(dxgi::ERROR_NOT_FOUND);
        return;
    }
    const auto capacity = m_memory.read_u32(size_address);
    const u32 required = static_cast<u32>(value->second.size());
    (void)m.write_u32(size_address, required);
    if (data_address == 0) {
        m.set_return(dxgi::S_OK);
        return;
    }
    if (capacity.is_err() || capacity.value() < required) {
        m.set_return(dxgi::ERROR_MORE_DATA);
        return;
    }
    if (required != 0 &&
        m_memory.write_bytes(data_address, value->second.data(),
                             value->second.size()).is_err()) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_GetParent(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr parent_out = m.arg_ptr(2);
    if (parent_out == 0) {
        m.set_return(dxgi::E_POINTER);
        return;
    }
    (void)m.write_ptr(parent_out, 0);
    auto object = m_objects.find(self);
    if (object == m_objects.end() || object->second.parent_object == 0) {
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }
    auto parent = m_objects.find(object->second.parent_object);
    if (parent == m_objects.end() ||
        !iid_is_supported(iid, parent->second.type)) {
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }
    ++parent->second.ref_count;
    if (m.write_ptr(parent_out, parent->first).is_err()) {
        --parent->second.ref_count;
        m.set_return(dxgi::E_POINTER);
        return;
    }
    m.set_return(dxgi::S_OK);
}

} // namespace vitreo::winapi
