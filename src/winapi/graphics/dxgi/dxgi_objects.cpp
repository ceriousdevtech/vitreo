// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_objects.cpp
/// @brief The DXGI guest object table (vtable region, factory, adapter
///        and output objects, release) and the IID support checks.
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
namespace {

constexpr usize STORAGE_SIZE = 0x20000;
constexpr u32 FACTORY_VTABLE_SLOTS = 32; // IDXGIFactory7, through UnregisterAdaptersChangedEvent
constexpr u32 ADAPTER_VTABLE_SLOTS = 19; // IDXGIAdapter4::GetDesc3

constexpr GuidBytes IID_IUNKNOWN{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr GuidBytes IID_IDXGIOBJECT{
    0xB8, 0x2F, 0xC2, 0xAE, 0xF3, 0x76, 0x39, 0x46,
    0x9B, 0xE0, 0x28, 0xEB, 0x43, 0xA6, 0x7A, 0x2E};
constexpr GuidBytes IID_IDXGIFACTORY{
    0xEC, 0x66, 0x71, 0x7B, 0xC7, 0x21, 0xAE, 0x44,
    0xB2, 0x1A, 0xC9, 0xAE, 0x32, 0x1A, 0xE3, 0x69};
constexpr GuidBytes IID_IDXGIFACTORY1{
    0x78, 0xAE, 0x0A, 0x77, 0x6F, 0xF2, 0xBA, 0x4D,
    0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87};
constexpr GuidBytes IID_IDXGIFACTORY2{
    0x1C, 0x3A, 0xC8, 0x50, 0x72, 0xE0, 0x48, 0x4C,
    0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0};
/// IDXGIFactory3 {25483823-cd46-4c7d-86ca-47aa95b837bd}
constexpr GuidBytes IID_IDXGIFACTORY3{
    0x23, 0x38, 0x48, 0x25, 0x46, 0xCD, 0x7D, 0x4C,
    0x86, 0xCA, 0x47, 0xAA, 0x95, 0xB8, 0x37, 0xBD};
/// IDXGIFactory4 {1bc6ea02-ef36-464f-bf0c-21ca39e5168a}
constexpr GuidBytes IID_IDXGIFACTORY4{
    0x02, 0xEA, 0xC6, 0x1B, 0x36, 0xEF, 0x4F, 0x46,
    0xBF, 0x0C, 0x21, 0xCA, 0x39, 0xE5, 0x16, 0x8A};
/// IDXGIFactory5 {7632e1f5-ee65-4dca-87fd-84cd75f8838d}
constexpr GuidBytes IID_IDXGIFACTORY5{
    0xF5, 0xE1, 0x32, 0x76, 0x65, 0xEE, 0xCA, 0x4D,
    0x87, 0xFD, 0x84, 0xCD, 0x75, 0xF8, 0x83, 0x8D};
/// IDXGIFactory6 {c1b6694f-ff09-44a9-b03c-77900a0a1d17}
constexpr GuidBytes IID_IDXGIFACTORY6{
    0x4F, 0x69, 0xB6, 0xC1, 0x09, 0xFF, 0xA9, 0x44,
    0xB0, 0x3C, 0x77, 0x90, 0x0A, 0x0A, 0x1D, 0x17};
/// IDXGIFactory7 {a4966eed-76db-44da-84c1-ee9a7afb20a8}
constexpr GuidBytes IID_IDXGIFACTORY7{
    0xED, 0x6E, 0x96, 0xA4, 0xDB, 0x76, 0xDA, 0x44,
    0x84, 0xC1, 0xEE, 0x9A, 0x7A, 0xFB, 0x20, 0xA8};
constexpr GuidBytes IID_IDXGIADAPTER{
    0xE1, 0xE7, 0x11, 0x24, 0xAC, 0x12, 0xCF, 0x4C,
    0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0};
constexpr GuidBytes IID_IDXGIADAPTER1{
    0x61, 0x8F, 0x03, 0x29, 0x39, 0x38, 0x26, 0x46,
    0x91, 0xFD, 0x08, 0x68, 0x79, 0x01, 0x1A, 0x05};
constexpr GuidBytes IID_IDXGIADAPTER2{
    0x0A, 0xAE, 0xA1, 0x0A, 0x0E, 0xFA, 0x84, 0x4B,
    0x86, 0x44, 0xE0, 0x5F, 0xF8, 0xE5, 0xAC, 0xB5};
/// dxgi1_4.h DEFINE_GUID(IID_IDXGIAdapter3, 0x645967A4, 0x1392, 0x4310, ...)
constexpr GuidBytes IID_IDXGIADAPTER3{
    0xA4, 0x67, 0x59, 0x64, 0x92, 0x13, 0x10, 0x43,
    0xA7, 0x98, 0x80, 0x53, 0xCE, 0x3E, 0x93, 0xFD};
/// dxgi1_6.h DEFINE_GUID(IID_IDXGIAdapter4, 0x3c8d99d1, 0x4fbf, 0x4181, ...)
constexpr GuidBytes IID_IDXGIADAPTER4{
    0xD1, 0x99, 0x8D, 0x3C, 0xBF, 0x4F, 0x81, 0x41,
    0xA8, 0x2C, 0xAF, 0x66, 0xBF, 0x7B, 0xD2, 0x4E};
constexpr GuidBytes IID_IDXGIOUTPUT{
    0xDB, 0xEE, 0x02, 0xAE, 0x35, 0xC7, 0x90, 0x46,
    0x8D, 0x52, 0x5A, 0x8D, 0xC2, 0x02, 0x13, 0xAA};
constexpr GuidBytes IID_IDXGIOUTPUT1{
    0xA8, 0xDE, 0xCD, 0x00, 0x9B, 0x93, 0x83, 0x4B,
    0xA3, 0x40, 0xA6, 0x85, 0x22, 0x66, 0x66, 0xCC};
constexpr GuidBytes IID_IDXGIOUTPUT2{
    0xD1, 0x39, 0x5E, 0x59, 0x24, 0x27, 0x63, 0x46,
    0x99, 0xB1, 0xDA, 0x96, 0x9D, 0xE2, 0x83, 0x64};
constexpr GuidBytes IID_IDXGIOUTPUT3{
    0x01, 0xB3, 0x6B, 0x8A, 0x7E, 0x7E, 0xF4, 0x41,
    0xA8, 0xE0, 0x5B, 0x32, 0xF7, 0xF9, 0x9B, 0x18};
constexpr GuidBytes IID_IDXGIOUTPUT4{
    0x35, 0xCA, 0x7D, 0xDC, 0x96, 0x21, 0x4D, 0x41,
    0x9F, 0x53, 0x61, 0x78, 0x84, 0x03, 0x2A, 0x60};
constexpr GuidBytes IID_IDXGIOUTPUT5{
    0x24, 0x74, 0xA0, 0x80, 0x52, 0xAB, 0xEB, 0x42,
    0x83, 0x3C, 0x0C, 0x42, 0xFD, 0x28, 0x2D, 0x98};
constexpr GuidBytes IID_IDXGIOUTPUT6{
    0xE8, 0x46, 0x83, 0x06, 0xEC, 0xAA, 0x84, 0x4B,
    0xAD, 0xD7, 0x13, 0x7F, 0x51, 0x3F, 0x77, 0xA1};

} // namespace

bool DxgiManager::write_pointer(GuestAddr address, GuestAddr value) {
    if (m_is_64bit) return m_memory.write_u64(address, value).is_ok();
    return m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

bool DxgiManager::read_guid(GuestAddr address, GuidBytes& guid) const {
    return address != 0 &&
        m_memory.read_bytes(address, guid.data(), guid.size()).is_ok();
}

bool DxgiManager::iid_is_supported(GuestAddr iid_address,
                                   ObjectType type) const {
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) return false;
    if (guid_equal(iid, IID_IUNKNOWN) || guid_equal(iid, IID_IDXGIOBJECT)) {
        return true;
    }
    if (type == ObjectType::Factory) {
        // The factory vtable is built to IDXGIFactory7's length, so every
        // version up to it is answerable. A guest that cannot obtain
        // IDXGIFactory6 falls back to EnumAdapters1 and never asks for a GPU
        // preference; one that can, uses its intended path.
        return guid_equal(iid, IID_IDXGIFACTORY) ||
               guid_equal(iid, IID_IDXGIFACTORY1) ||
               guid_equal(iid, IID_IDXGIFACTORY2) ||
               guid_equal(iid, IID_IDXGIFACTORY3) ||
               guid_equal(iid, IID_IDXGIFACTORY4) ||
               guid_equal(iid, IID_IDXGIFACTORY5) ||
               guid_equal(iid, IID_IDXGIFACTORY6) ||
               guid_equal(iid, IID_IDXGIFACTORY7);
    }
    if (type == ObjectType::Adapter) {
        return guid_equal(iid, IID_IDXGIADAPTER) ||
               guid_equal(iid, IID_IDXGIADAPTER1) ||
               guid_equal(iid, IID_IDXGIADAPTER2) ||
               guid_equal(iid, IID_IDXGIADAPTER3) ||
               guid_equal(iid, IID_IDXGIADAPTER4);
    }
    return guid_equal(iid, IID_IDXGIOUTPUT) ||
           guid_equal(iid, IID_IDXGIOUTPUT1) ||
           guid_equal(iid, IID_IDXGIOUTPUT2) ||
           guid_equal(iid, IID_IDXGIOUTPUT3) ||
           guid_equal(iid, IID_IDXGIOUTPUT4) ||
           guid_equal(iid, IID_IDXGIOUTPUT5) ||
           guid_equal(iid, IID_IDXGIOUTPUT6);
}

bool DxgiManager::iid_is_factory1(GuestAddr iid_address) const {
    GuidBytes iid{};
    return read_guid(iid_address, iid) && guid_equal(iid, IID_IDXGIFACTORY1);
}

bool DxgiManager::iid_is_factory2(GuestAddr iid_address) const {
    GuidBytes iid{};
    return read_guid(iid_address, iid) && guid_equal(iid, IID_IDXGIFACTORY2);
}

bool DxgiManager::ensure_storage() {
    if (m_region_base != 0) return true;
    for (GuestAddr minimum = 0x62000000ull; minimum != 0;) {
        const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0) return false;
        if (m_memory.map_region(base, STORAGE_SIZE,
                                MemoryProtection::ReadWrite).is_ok()) {
            m_region_base = base;
            m_factory_vtable = base;
            m_adapter_vtable = base + 0x400;
            m_output_vtable = base + 0x800;
            m_next_object = base + 0x1000;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;

    const usize pointer_size = m_is_64bit ? 8 : 4;
    const std::array<GuestAddr, FACTORY_VTABLE_SLOTS> factory_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.set_private_data,
        m_method_thunks.set_private_data_interface,
        m_method_thunks.get_private_data,
        m_method_thunks.get_parent,
        m_method_thunks.enum_adapters,
        m_method_thunks.make_window_association,
        m_method_thunks.get_window_association,
        m_method_thunks.create_swap_chain,
        m_method_thunks.create_software_adapter,
        m_method_thunks.enum_adapters1,
        m_method_thunks.is_current,
        m_method_thunks.is_windowed_stereo_enabled,
        m_method_thunks.create_swap_chain_for_hwnd,
        m_method_thunks.create_swap_chain_for_core_window,
        m_method_thunks.get_shared_resource_adapter_luid,
        m_method_thunks.register_stereo_status_window,
        m_method_thunks.register_stereo_status_event,
        m_method_thunks.unregister_stereo_status,
        m_method_thunks.register_occlusion_status_window,
        m_method_thunks.register_occlusion_status_event,
        m_method_thunks.unregister_occlusion_status,
        m_method_thunks.create_swap_chain_for_composition,
        m_method_thunks.get_creation_flags,
        m_method_thunks.enum_adapter_by_luid,
        m_method_thunks.enum_warp_adapter,
        m_method_thunks.factory_check_feature_support,
        m_method_thunks.enum_adapter_by_gpu_preference,
        m_method_thunks.register_adapters_changed_event,
        m_method_thunks.unregister_adapters_changed_event,
    };
    const std::array<GuestAddr, ADAPTER_VTABLE_SLOTS> adapter_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.set_private_data,
        m_method_thunks.set_private_data_interface,
        m_method_thunks.get_private_data,
        m_method_thunks.get_parent,
        m_method_thunks.enum_outputs,
        m_method_thunks.get_desc,
        m_method_thunks.check_interface_support,
        m_method_thunks.get_desc1,
        m_method_thunks.get_desc2,
        m_method_thunks.register_hwcp_teardown_event,
        m_method_thunks.unregister_hwcp_teardown,
        m_method_thunks.query_video_memory_info,
        m_method_thunks.set_video_memory_reservation,
        m_method_thunks.register_budget_change_event,
        m_method_thunks.unregister_budget_change,
        m_method_thunks.get_desc3,
    };
    for (usize index = 0; index < factory_methods.size(); ++index) {
        if (factory_methods[index] == 0 ||
            !write_pointer(m_factory_vtable + index * pointer_size,
                           factory_methods[index])) {
            return false;
        }
    }
    for (usize index = 0; index < adapter_methods.size(); ++index) {
        if (adapter_methods[index] == 0 ||
            !write_pointer(m_adapter_vtable + index * pointer_size,
                           adapter_methods[index])) {
            return false;
        }
    }
    for (usize index = 0; index < m_method_thunks.output.size(); ++index) {
        if (m_method_thunks.output[index] == 0) continue;
        if (!write_pointer(m_output_vtable + index * pointer_size,
                           m_method_thunks.output[index])) {
            return false;
        }
    }
    return true;
}

GuestAddr DxgiManager::create_object(ObjectType type,
                                     GuestAddr parent_factory) {
    if (!ensure_storage()) return 0;
    GuestAddr object_address = 0;
    if (!m_free_objects.empty()) {
        object_address = m_free_objects.back();
        m_free_objects.pop_back();
    } else {
        if (m_next_object + 0x40 > m_region_base + STORAGE_SIZE) return 0;
        object_address = m_next_object;
        m_next_object += 0x40;
    }
    const GuestAddr vtable = type == ObjectType::Factory
        ? m_factory_vtable
        : (type == ObjectType::Adapter ? m_adapter_vtable : m_output_vtable);
    if (!write_pointer(object_address, vtable)) {
        m_free_objects.push_back(object_address);
        return 0;
    }
    Object object{};
    object.type = type;
    object.parent_object = parent_factory;
    m_objects.emplace(object_address, std::move(object));
    return object_address;
}

GuestAddr DxgiManager::create_factory_for_iid(GuestAddr iid_address) {
    if (!iid_is_supported(iid_address, ObjectType::Factory)) return 0;
    return create_object(ObjectType::Factory);
}

GuestAddr DxgiManager::create_adapter(GuestAddr parent_factory) {
    auto parent = m_objects.find(parent_factory);
    if (parent == m_objects.end() ||
        parent->second.type != ObjectType::Factory) {
        return 0;
    }
    ++parent->second.ref_count;
    const GuestAddr adapter =
        create_object(ObjectType::Adapter, parent_factory);
    if (adapter == 0) --parent->second.ref_count;
    return adapter;
}

GuestAddr DxgiManager::create_output(
    GuestAddr parent_adapter, const dxgi::OutputInfo& info) {
    if (std::any_of(m_method_thunks.output.begin(),
                    m_method_thunks.output.end(),
                    [](GuestAddr thunk) { return thunk == 0; })) {
        return 0;
    }
    auto parent = m_objects.find(parent_adapter);
    if (parent == m_objects.end() ||
        parent->second.type != ObjectType::Adapter) {
        return 0;
    }
    ++parent->second.ref_count;
    const GuestAddr output = create_object(ObjectType::Output, parent_adapter);
    if (output == 0) {
        --parent->second.ref_count;
        return 0;
    }
    m_objects.find(output)->second.output_info = info;
    return output;
}

DxgiManager::PrivateInterface DxgiManager::take_private_interface(GuestAddr object_address,
                                              const GuidBytes& name) {
    auto object = m_objects.find(object_address);
    if (object == m_objects.end()) return {};
    auto held = object->second.private_data_interfaces.find(name);
    if (held == object->second.private_data_interfaces.end()) return {};
    auto previous = std::move(held->second);
    object->second.private_data_interfaces.erase(held);
    return previous;
}

u32 DxgiManager::release_object(GuestAddr address) {
    auto object = m_objects.find(address);
    if (object == m_objects.end() || object->second.ref_count == 0) return 0;
    const u32 count = --object->second.ref_count;
    if (count != 0) return count;

    const GuestAddr parent = object->second.parent_object;
    // Learn (SetPrivateDataInterface): the object releases the interface when it
    // is destroyed. Collect them and erase the record FIRST -- a guest Release
    // can re-enter this manager, and `object` would be a dangling node by then.
    std::vector<PrivateInterface> held;
    held.reserve(object->second.private_data_interfaces.size());
    for (auto& [name, interface_ptr] :
         object->second.private_data_interfaces) {
        (void)name;
        held.push_back(std::move(interface_ptr));
    }
    m_objects.erase(object);
    m_free_objects.push_back(address);
    held.clear();
    if (parent != 0) (void)release_object(parent);
    return 0;
}

} // namespace vitreo::winapi
