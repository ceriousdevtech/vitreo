// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_interop.cpp
/// @brief Host-side DXGI interop the graphics frontends call (adapter
///        and output objects, retain/release, output lookup, QI).
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

bool DxgiManager::is_adapter_object(GuestAddr address) const {
    ApiLock lock(*this);
    const auto object = m_objects.find(address);
    return object != m_objects.end() &&
           object->second.type == ObjectType::Adapter;
}

GuestAddr DxgiManager::create_default_adapter() {
    ApiLock lock(*this);
    const GuestAddr factory = create_object(ObjectType::Factory);
    if (factory == 0) return 0;
    // The factory the D3D11 runtime creates for a device reports bit 31 from
    // GetCreationFlags (measured, d3d11_session_contracts_probe [dxgi.5]).
    m_objects.find(factory)->second.creation_flags = 0x80000000u;
    const GuestAddr adapter = create_adapter(factory);
    // Drop the creator's factory reference. The adapter keeps its parent
    // alive until the adapter itself is released.
    (void)release_object(factory);
    return adapter;
}

bool DxgiManager::retain_adapter(GuestAddr address) {
    ApiLock lock(*this);
    auto object = m_objects.find(address);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Adapter) {
        return false;
    }
    ++object->second.ref_count;
    return true;
}

void DxgiManager::release_adapter(GuestAddr address) {
    ApiLock lock(*this);
    if (is_adapter_object(address)) (void)release_object(address);
}

bool DxgiManager::retain_object(GuestAddr address) {
    ApiLock lock(*this);
    auto object = m_objects.find(address);
    if (object == m_objects.end() || object->second.ref_count == 0) {
        return false;
    }
    ++object->second.ref_count;
    return true;
}

void DxgiManager::release_dxgi_object(GuestAddr address) {
    ApiLock lock(*this);
    (void)release_object(address);
}

bool DxgiManager::is_output_object(GuestAddr address) const {
    ApiLock lock(*this);
    const auto object = m_objects.find(address);
    return object != m_objects.end() &&
           object->second.type == ObjectType::Output;
}

GuestAddr DxgiManager::parent_factory_of(GuestAddr adapter) const {
    ApiLock lock(*this);
    const auto object = m_objects.find(adapter);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Adapter) {
        return 0;
    }
    const auto parent = m_objects.find(object->second.parent_object);
    return parent != m_objects.end() &&
                   parent->second.type == ObjectType::Factory
               ? object->second.parent_object
               : 0;
}

std::vector<dxgi::OutputInfo> DxgiManager::output_infos() const {
    ApiLock lock(*this);
    const auto resolver = m_output_resolver;
    ApiUnlock unlocked(*this);
    return resolver ? resolver() : std::vector<dxgi::OutputInfo>{};
}

std::optional<dxgi::OutputInfo> DxgiManager::output_info_of(
    GuestAddr address) const {
    ApiLock lock(*this);
    dxgi::OutputInfo info;
    {
        const auto object = m_objects.find(address);
        if (object == m_objects.end() ||
            object->second.type != ObjectType::Output ||
            object->second.ref_count == 0) {
            return std::nullopt;
        }
        info = object->second.output_info;
    }
    // An output object describes the display as it is now: its desktop
    // coordinates follow a full-screen mode change made after it was
    // enumerated.
    for (const auto& live : output_infos()) {
        if (live.device_name == info.device_name) return live;
    }
    return info;
}

GuestAddr DxgiManager::create_output_named(GuestAddr adapter,
                                           const std::string& device_name) {
    ApiLock lock(*this);
    if (!is_adapter_object(adapter)) return 0;
    for (const auto& output : output_infos()) {
        if (output.device_name == device_name) {
            return create_output(adapter, output);
        }
    }
    return 0;
}

GuestAddr DxgiManager::create_containing_output(GuestAddr adapter, i32 left,
                                                i32 top, i32 right,
                                                i32 bottom) {
    ApiLock lock(*this);
    if (!is_adapter_object(adapter)) return 0;
    const std::vector<dxgi::OutputInfo> outputs = output_infos();
    i64 best_area = 0;
    const dxgi::OutputInfo* best = nullptr;
    for (const auto& output : outputs) {
        const i64 width = static_cast<i64>(std::min(right, output.right)) -
                          std::max(left, output.left);
        const i64 height = static_cast<i64>(std::min(bottom, output.bottom)) -
                           std::max(top, output.top);
        const i64 area = width > 0 && height > 0 ? width * height : 0;
        if (area > best_area) {
            best_area = area;
            best = &output;
        }
    }
    return best != nullptr ? create_output(adapter, *best) : 0;
}

u32 DxgiManager::query_object_interface(GuestAddr object_address,
                                        GuestAddr iid_address,
                                        GuestAddr object_out) {
    ApiLock lock(*this);
    if (object_out == 0) return dxgi::E_POINTER;
    (void)write_pointer(object_out, 0);
    auto object = m_objects.find(object_address);
    if (object == m_objects.end() ||
        !iid_is_supported(iid_address, object->second.type)) {
        return dxgi::E_NOINTERFACE;
    }
    ++object->second.ref_count;
    if (!write_pointer(object_out, object_address)) {
        --object->second.ref_count;
        return dxgi::E_POINTER;
    }
    return dxgi::S_OK;
}

} // namespace vitreo::winapi
