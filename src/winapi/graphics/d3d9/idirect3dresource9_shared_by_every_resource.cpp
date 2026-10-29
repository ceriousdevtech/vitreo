// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_resources.cpp
/// @brief IDirect3DSwapChain9, IDirect3DSurface9, IDirect3DTexture9,
///        IDirect3DVertexBuffer9, and IDirect3DIndexBuffer9.
///
/// Locking is the contract that matters most here: D3D9 hands the application
/// a raw pointer into resource memory. Vitreo maps guest-visible memory for
/// the resource's whole lifetime and pushes the locked range into the shared
/// backend on unlock, so pixels and vertices the guest writes are the ones the
/// GPU receives.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/// @file idirect3dresource9_shared_by_every_resource.cpp
/// @brief IDirect3DResource9, shared by every resource interface at slots 3..10
///
/// Closed cut from d3d9_resources.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {

/// Guest IUnknown* width for D3D9 (x86 frontend; objects are u32 handles).
constexpr u32 kGuestIUnknownPtrSize = 4;

[[nodiscard]] u32 read_iunknown_ptr(const std::vector<u8>& bytes) {
    if (bytes.size() < kGuestIUnknownPtrSize) return 0;
    u32 punk = 0;
    std::memcpy(&punk, bytes.data(), sizeof(punk));
    return punk;
}

} // namespace

// =============================================================================
// IDirect3DResource9, shared by every resource interface at slots 3..10
// =============================================================================

u32 D3D9Manager::retain_private_iunknown(u32 punk) {
    if (punk == 0) return 0;
    // Vitreo-managed D3D9 objects stay on the local fast path.
    if (find_any(punk) != nullptr) return add_ref(punk);
    if (!m_guest_iunknown_addref) return 0;
    return m_guest_iunknown_addref(punk);
}

void D3D9Manager::release_private_iunknown(u32 punk) {
    if (punk == 0) return;
    if (find_any(punk) != nullptr) {
        (void)release(punk);
        return;
    }
    if (m_guest_iunknown_release) m_guest_iunknown_release(punk);
}

void D3D9Manager::release_private_data_iunknown(
    const ComObject::PrivateDataEntry& data) {
    if ((data.flags & D3DSPD_IUNKNOWN) == 0) return;
    release_private_iunknown(read_iunknown_ptr(data.bytes));
}

void D3D9Manager::clear_private_data(ComObject& entry, u32 self) {
    std::vector<u32> to_release;
    to_release.reserve(entry.private_data.size());
    for (const auto& [guid, data] : entry.private_data) {
        (void)guid;
        if ((data.flags & D3DSPD_IUNKNOWN) == 0) continue;
        const u32 punk = read_iunknown_ptr(data.bytes);
        // Skip self: forced destroy_object can run while this entry still holds
        // a ref on the dying object; releasing it would re-enter destroy.
        if (punk != 0 && punk != self) to_release.push_back(punk);
    }
    entry.private_data.clear();
    for (u32 punk : to_release) {
        release_private_iunknown(punk);
    }
}

void D3D9Manager::dispatch_private_data(Marshaler& m, usize slot,
                                        ComObject& entry) {
    std::array<u8, 16> guid{};
    if (!read_guid(m.arg_ptr(1), guid)) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    switch (slot) {
    case 4: { // SetPrivateData(REFGUID, const void*, DWORD, DWORD)
        const GuestAddr data_addr = m.arg_ptr(2);
        const u32 size = static_cast<u32>(m.arg(3));
        const u32 flags = static_cast<u32>(m.arg(4));
        const bool as_iunknown = (flags & D3DSPD_IUNKNOWN) != 0;

        if (as_iunknown) {
            // Learn: SizeOfData must be sizeof(IUnknown*); pData holds an
            // IUnknown*. Direct3D AddRefs on set and Releases on replace,
            // FreePrivateData, or resource destroy. Vitreo ComObjects use the
            // local fast path; other guest pointers use the optional callback
            // wired to ComCore (no D3D9→ole32 link dependency).
            if (size != kGuestIUnknownPtrSize || data_addr == 0 ||
                m_memory == nullptr) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            std::vector<u8> bytes(size);
            if (m_memory->read_bytes(data_addr, bytes.data(), bytes.size())
                    .is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const u32 punk = read_iunknown_ptr(bytes);
            if (retain_private_iunknown(punk) == 0) {
                VITREO_TRACE(
                    "[d3d9] SetPrivateData D3DSPD_IUNKNOWN punk=0x%08x "
                    "retain failed -> INVALIDCALL\n",
                    punk);
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const auto prior = entry.private_data.find(guid);
            if (prior != entry.private_data.end()) {
                release_private_data_iunknown(prior->second);
            }
            entry.private_data.insert_or_assign(
                guid, ComObject::PrivateDataEntry{D3DSPD_IUNKNOWN,
                                                  std::move(bytes)});
            m.set_return(D3D_OK);
            return;
        }

        if (size != 0 && (data_addr == 0 || m_memory == nullptr)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::vector<u8> bytes(size);
        if (size != 0 &&
            m_memory->read_bytes(data_addr, bytes.data(), bytes.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto prior = entry.private_data.find(guid);
        if (prior != entry.private_data.end()) {
            release_private_data_iunknown(prior->second);
        }
        entry.private_data.insert_or_assign(
            guid, ComObject::PrivateDataEntry{0, std::move(bytes)});
        m.set_return(D3D_OK);
        return;
    }
    case 5: { // GetPrivateData(REFGUID, void*, DWORD*)
        const GuestAddr data_addr = m.arg_ptr(2);
        const GuestAddr size_addr = m.arg_ptr(3);
        if (size_addr == 0 || m_memory == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto found = entry.private_data.find(guid);
        if (found == entry.private_data.end()) {
            m.set_return(D3DERR_NOTFOUND_D3D);
            return;
        }
        const u32 required = static_cast<u32>(found->second.bytes.size());
        const auto capacity = m.read_u32(size_addr);
        if (m.write_u32(size_addr, required).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Learn: null pData or undersized buffer → write required size and
        // return D3DERR_MOREDATA.
        if (data_addr == 0 || capacity.is_err() ||
            capacity.value() < required) {
            m.set_return(D3DERR_MOREDATA);
            return;
        }
        if (required != 0 &&
            m_memory
                ->write_bytes(data_addr, found->second.bytes.data(),
                              found->second.bytes.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Learn: IUnknown private data is AddRef'd before return; caller
        // Releases when done with the pointer.
        if ((found->second.flags & D3DSPD_IUNKNOWN) != 0) {
            const u32 punk = read_iunknown_ptr(found->second.bytes);
            if (retain_private_iunknown(punk) == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    case 6: { // FreePrivateData(REFGUID)
        const auto found = entry.private_data.find(guid);
        if (found == entry.private_data.end()) {
            m.set_return(D3DERR_NOTFOUND_D3D);
            return;
        }
        ComObject::PrivateDataEntry doomed = std::move(found->second);
        entry.private_data.erase(found);
        release_private_data_iunknown(doomed);
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
}

bool D3D9Manager::dispatch_resource_base(Marshaler& m, usize slot, u32 self,
                                         u32 resource_type) {
    ComObject* entry = find_any(self);
    if (entry == nullptr) return false;

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return true;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return true;
        }
        m.set_return(D3D_OK);
        return true;
    }
    case 4: // SetPrivateData
    case 5: // GetPrivateData
    case 6: // FreePrivateData
        dispatch_private_data(m, slot, *entry);
        return true;
    case 7: { // SetPriority (returns the previous priority)
        const u32 priority = static_cast<u32>(m.arg(1));
        u32* slot_value = nullptr;
        if (entry->texture) slot_value = &entry->texture->priority;
        if (entry->buffer) slot_value = &entry->buffer->priority;
        if (slot_value == nullptr) {
            m.set_return(0);
            return true;
        }
        const u32 previous = *slot_value;
        *slot_value = priority;
        m.set_return(previous);
        return true;
    }
    case 8: { // GetPriority
        if (entry->texture) {
            m.set_return(entry->texture->priority);
            return true;
        }
        if (entry->buffer) {
            m.set_return(entry->buffer->priority);
            return true;
        }
        m.set_return(0);
        return true;
    }
    case 9: // PreLoad (void return)
        // Every managed resource Vitreo holds already lives in shared-core
        // storage, so there is nothing to promote.
        m.set_return(0);
        return true;
    case 10: // GetType
        m.set_return(resource_type);
        return true;
    default:
        return false;
    }
}

} // namespace vitreo::winapi
