// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_adapter.cpp
/// @brief IDXGIAdapter through IDXGIAdapter4 methods (descriptors,
///        video memory, events, CheckInterfaceSupport).
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

constexpr u32 DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0;
constexpr u32 DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1;
constexpr GuidBytes IID_IDXGIDEVICE{
    0xFA, 0x77, 0xEC, 0x54, 0x77, 0x13, 0xE6, 0x44,
    0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C};
/// d3d10.h / d3d10_1.h (tools/audit_guid_constants.py checks both).
constexpr GuidBytes IID_ID3D10DEVICE{
    0x0F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
constexpr GuidBytes IID_ID3D10DEVICE1{
    0x8F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};

/// User-mode driver version reported by IDXGIAdapter::CheckInterfaceSupport,
/// as the four 16-bit components of a LARGE_INTEGER.
///
/// This used to be 1.0.0.0 ("Vitreo's own contract version"). That is not a
/// neutral value: guests do not read it as "which Vitreo", they read it as a
/// DRIVER version and compare it against thresholds, so 1.0.0.0 sorts older
/// than every threshold that exists and pushes every such guest onto its
/// oldest-driver fallback path. Callers read 1.0.0.0 as the user-mode driver
/// version and compare it with blocklist entries of the form "vendor X,
/// driver version < Y -> block", so 1.0.0.0 matches all of them.
///
/// The default here sorts as current. It is a statement about capability, not
/// a claim to be someone else's driver: the vendor ID stays truthfully Apple.
/// The per-capsule knob is the display-class registry `DriverVersion` value;
/// VITREO_DXGI_DRIVER_VERSION="a.b.c.d" is the per-run override above it.
[[nodiscard]] u64 dxgi_umd_driver_version() {
    namespace identity = display_device_identity;
    if (const char* spec = std::getenv("VITREO_DXGI_DRIVER_VERSION")) {
        if (const auto parsed = identity::parse_driver_version(spec)) {
            return *parsed;
        }
    }
    return identity::adapter_identity().driver_version.value_or(
        identity::DEFAULT_DRIVER_VERSION);
}

} // namespace

bool DxgiManager::write_adapter_desc(Marshaler& m, GuestAddr address,
                                     u32 version) const {
    if (address == 0) return false;
    const usize pointer_size = m_is_64bit ? 8 : 4;
    // version 0: DXGI_ADAPTER_DESC, 1: +Flags, 2: +preemption fields
    const usize size = 272 + 3 * pointer_size + 8 +
        (version == 0 ? 0u : (version == 1 ? 4u : 12u));
    std::vector<u8> bytes(size, 0);
    if (m_memory.write_bytes(address, bytes.data(), bytes.size()).is_err()) {
        return false;
    }

    const usize description_length =
        std::min<usize>(m_adapter_info.description.size(), 127);
    for (usize index = 0; index < description_length; ++index) {
        const u16 character =
            static_cast<u8>(m_adapter_info.description[index]);
        if (m_memory.write_u16(address + index * sizeof(u16),
                               character).is_err()) {
            return false;
        }
    }
    if (m.write_u32(address + 256, m_adapter_info.vendor_id).is_err() ||
        m.write_u32(address + 260, m_adapter_info.device_id).is_err() ||
        m.write_u32(address + 264, m_adapter_info.subsystem_id).is_err() ||
        m.write_u32(address + 268, m_adapter_info.revision).is_err()) {
        return false;
    }

    GuestAddr offset = address + 272;
    const auto write_size_t = [&](u64 value) {
        const bool ok = m_is_64bit
            ? m_memory.write_u64(offset, value).is_ok()
            : m_memory.write_u32(offset, static_cast<u32>(
                  std::min<u64>(value, 0xFFFFFFFFull))).is_ok();
        offset += pointer_size;
        return ok;
    };
    if (!write_size_t(m_adapter_info.dedicated_video_memory) ||
        !write_size_t(m_adapter_info.dedicated_system_memory) ||
        !write_size_t(m_adapter_info.shared_system_memory)) {
        return false;
    }
    // Stable process-local LUID for the one adapter.
    if (m.write_u32(offset, ADAPTER_LUID_LOW).is_err() ||
        m.write_u32(offset + 4, ADAPTER_LUID_HIGH).is_err()) {
        return false;
    }
    offset += 8;
    if (version == 0) return true;
    if (m.write_u32(offset, 0).is_err()) return false; // Flags
    offset += 4;
    if (version == 1) return true;
    constexpr u32 DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY = 0;
    constexpr u32 DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY = 0;
    return m.write_u32(offset,
                       DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY).is_ok() &&
           m.write_u32(offset + 4,
                       DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY).is_ok();
}

void DxgiManager::log_adapter_desc(const char* api, u32 version) const {
    if (!live_diag::caps_log_enabled()) return;
    live_diag::caps::AdapterDesc desc;
    desc.api = api;
    desc.description = m_adapter_info.description;
    desc.vendor_id = m_adapter_info.vendor_id;
    desc.device_id = m_adapter_info.device_id;
    desc.subsystem_id = m_adapter_info.subsystem_id;
    desc.revision = m_adapter_info.revision;
    desc.dedicated_video_memory = m_adapter_info.dedicated_video_memory;
    desc.dedicated_system_memory = m_adapter_info.dedicated_system_memory;
    desc.shared_system_memory = m_adapter_info.shared_system_memory;
    desc.luid_low = ADAPTER_LUID_LOW;
    desc.luid_high = static_cast<i32>(ADAPTER_LUID_HIGH);
    // write_adapter_desc: version 1 adds Flags (0), version 2 the two
    // preemption granularities (DMA_BUFFER_BOUNDARY, 0).
    desc.has_flags = version >= 1;
    desc.has_preemption = version >= 2;
    live_diag::caps::log(live_diag::caps::format_adapter_desc(desc));
}

void DxgiManager::impl_GetDesc(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr description = m.arg_ptr(1);
    const auto object = m_objects.find(self);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Adapter ||
        !write_adapter_desc(m, description, 0)) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    VITREO_TRACE("[dxgi] IDXGIAdapter::GetDesc -> \"%s\"\n",
                 m_adapter_info.description.c_str());
    log_adapter_desc("IDXGIAdapter::GetDesc", 0);
    dxgi_diag("IDXGIAdapter::GetDesc -> \"%s\"",
              m_adapter_info.description.c_str());
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_GetDesc1(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr description = m.arg_ptr(1);
    const auto object = m_objects.find(self);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Adapter ||
        !write_adapter_desc(m, description, 1)) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    VITREO_TRACE("[dxgi] IDXGIAdapter1::GetDesc1 -> \"%s\"\n",
                 m_adapter_info.description.c_str());
    log_adapter_desc("IDXGIAdapter1::GetDesc1", 1);
    dxgi_diag("IDXGIAdapter1::GetDesc1 -> \"%s\"",
              m_adapter_info.description.c_str());
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_GetDesc2(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr description = m.arg_ptr(1);
    const auto object = m_objects.find(self);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Adapter ||
        !write_adapter_desc(m, description, 2)) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    log_adapter_desc("IDXGIAdapter2::GetDesc2", 2);
    dxgi_diag("IDXGIAdapter2::GetDesc2 -> \"%s\"",
              m_adapter_info.description.c_str());
    m.set_return(dxgi::S_OK);
}

bool DxgiManager::adapter_object(GuestAddr self) const {
    const auto object = m_objects.find(self);
    return object != m_objects.end() && object->second.type == ObjectType::Adapter;
}

u32 DxgiManager::register_adapter3_event(Marshaler& m,
                                         std::unordered_set<u32>& cookies,
                                         u32& next_cookie) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr event = m.arg_ptr(1);
    const GuestAddr cookie_out = m.arg_ptr(2);
    // Windows: a null event or null cookie pointer returns
    // DXGI_ERROR_INVALID_CALL and leaves *pdwCookie untouched.
    if (!adapter_object(self) || event == 0 || cookie_out == 0) {
        return dxgi::ERROR_INVALID_CALL;
    }
    const u32 cookie = next_cookie++;
    if (m.write_u32(cookie_out, cookie).is_err()) {
        --next_cookie;
        return dxgi::E_INVALIDARG;
    }
    cookies.insert(cookie);
    return dxgi::S_OK;
}

void DxgiManager::impl_RegisterHardwareContentProtectionTeardownStatusEvent(
    Marshaler& m) {
    const u32 hr = register_adapter3_event(m, m_hwcp_teardown_cookies,
                                           m_next_hwcp_teardown_cookie);
    if (hr == dxgi::S_OK) {
        dxgi_diag("RegisterHardwareContentProtectionTeardownStatusEvent -> "
                  "cookie issued (this host never tears HWCP down)");
    }
    m.set_return(hr);
}

void DxgiManager::impl_UnregisterHardwareContentProtectionTeardownStatus(
    Marshaler& m) {
    // Learn: void UnregisterHardwareContentProtectionTeardownStatus(DWORD).
    const GuestAddr self = m.arg_ptr(0);
    const u32 cookie = static_cast<u32>(m.arg(1));
    if (adapter_object(self)) {
        m_hwcp_teardown_cookies.erase(cookie);
    }
    m.set_return(0);
}

void DxgiManager::impl_QueryVideoMemoryInfo(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 node = static_cast<u32>(m.arg(1));
    const u32 segment = static_cast<u32>(m.arg(2));
    const GuestAddr info_out = m.arg_ptr(3);
    if (!adapter_object(self) || info_out == 0) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    if (node != 0 ||
        (segment != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
         segment != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    // Discrete-class profile (detect_adapter_info): LOCAL is the dedicated
    // pool, DXGI_ADAPTER_DESC::DedicatedVideoMemory, and every GPU resource
    // lives in it; NON_LOCAL, the system-memory pool behind
    // SharedSystemMemory (0), is all zeros -- the layout Windows measures
    // too. The live budget is what GPU admission would still grant, never
    // more than the working set reported as dedicated.
    u64 budget = (segment == DXGI_MEMORY_SEGMENT_GROUP_LOCAL)
                     ? m_adapter_info.dedicated_video_memory
                     : 0;
    u64 usage = 0;
    graphics::BackendVideoMemoryInfo live{};
    const auto* device = m_adapter_info_is_explicit
        ? nullptr : graphics::active_backend_device();
    if (segment == DXGI_MEMORY_SEGMENT_GROUP_LOCAL && device != nullptr &&
        device->query_video_memory(live)) {
        budget = live.budget;
        usage = live.current_usage;
    }
    // AvailableForReservation is not derivable from Windows beyond
    // <= Budget; it keeps half the budget.
    const u64 available = budget / 2;
    // DXGI_QUERY_VIDEO_MEMORY_INFO is four UINT64s on both bitnesses
    // (dxgi1_4.h). CurrentReservation stays 0 because
    // SetVideoMemoryReservation never succeeds (Windows; see below).
    if (m_memory.write_u64(info_out, budget).is_err() ||
        m_memory.write_u64(info_out + 8, usage).is_err() ||
        m_memory.write_u64(info_out + 16, available).is_err() ||
        m_memory.write_u64(info_out + 24, 0).is_err()) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    dxgi_diag("QueryVideoMemoryInfo node=%u seg=%u budget=%llu usage=%llu",
              node, segment, static_cast<unsigned long long>(budget),
              static_cast<unsigned long long>(usage));
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_SetVideoMemoryReservation(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 node = static_cast<u32>(m.arg(1));
    const u32 segment = static_cast<u32>(m.arg(2));
    u64 reservation = m.arg(3);
    if (!m_is_64bit) {
        reservation |= m.arg(4) << 32;
    }
    (void)reservation;
    // Windows (a UMA adapter, DedicatedVideoMemory=0): every reservation,
    // including 0 and 1, returns E_INVALIDARG. That stays the answer under
    // the discrete-class profile: Learn defines no success or failure rule,
    // and what a discrete adapter accepts has not been measured
    // (dxgi_adapter3_probe on a discrete GPU), so nothing is promised.
    if (!adapter_object(self) || node != 0 ||
        (segment != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
         segment != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    m.set_return(dxgi::E_INVALIDARG);
}

void DxgiManager::impl_RegisterVideoMemoryBudgetChangeNotificationEvent(
    Marshaler& m) {
    const u32 hr = register_adapter3_event(m, m_budget_change_cookies,
                                           m_next_budget_change_cookie);
    if (hr == dxgi::S_OK) {
        dxgi_diag("RegisterVideoMemoryBudgetChangeNotificationEvent -> cookie "
                  "issued");
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED dxgi!IDXGIAdapter3::"
                         "RegisterVideoMemoryBudgetChangeNotificationEvent: "
                         "the event is not signalled when the live LOCAL "
                         "budget changes\n");
        }
    }
    m.set_return(hr);
}

void DxgiManager::impl_UnregisterVideoMemoryBudgetChangeNotification(
    Marshaler& m) {
    // Learn: void UnregisterVideoMemoryBudgetChangeNotification(DWORD).
    const GuestAddr self = m.arg_ptr(0);
    const u32 cookie = static_cast<u32>(m.arg(1));
    if (adapter_object(self)) {
        m_budget_change_cookies.erase(cookie);
    }
    m.set_return(0);
}

void DxgiManager::impl_GetDesc3(Marshaler& m) {
    // DXGI_ADAPTER_DESC3 is DXGI_ADAPTER_DESC2 with Flags typed as
    // DXGI_ADAPTER_FLAG3. FLAG3_NONE is 0; this host is not remote, not
    // software, and does not advertise ACG or fence extras.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr description = m.arg_ptr(1);
    if (!adapter_object(self) || !write_adapter_desc(m, description, 2)) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    log_adapter_desc("IDXGIAdapter4::GetDesc3", 2);
    dxgi_diag("IDXGIAdapter4::GetDesc3 -> \"%s\"",
              m_adapter_info.description.c_str());
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_CheckInterfaceSupport(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid_address = m.arg_ptr(1);
    const GuestAddr version_out = m.arg_ptr(2);
    // An unsupported interface leaves *pUMDVersion untouched (measured).
    const auto adapter = m_objects.find(self);
    if (adapter == m_objects.end() ||
        adapter->second.type != ObjectType::Adapter) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // Learn CheckInterfaceSupport: IDXGIDevice and the Direct3D 10 device
    // interfaces are answerable (the page's own example asks for
    // ID3D10Device). D3D10 devices exist only while the D3D10 frontend is
    // enabled; otherwise the adapter supports no D3D10 device.
    const bool d3d10_device =
        (guid_equal(iid, IID_ID3D10DEVICE) ||
         guid_equal(iid, IID_ID3D10DEVICE1)) &&
        d3d10::d3d10_enabled();
    if (guid_equal(iid, IID_IDXGIDEVICE) || d3d10_device) {
        const u64 driver_version = dxgi_umd_driver_version();
        if (version_out != 0 &&
            m_memory.write_u64(version_out, driver_version).is_err()) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        dxgi_diag(
            "IDXGIAdapter::CheckInterfaceSupport "
            "iid=54ec77fa-1377-44e6-8c32-88fd5f44c84c -> S_OK");
        m.set_return(dxgi::S_OK);
        return;
    }
    // Windows documents DXGI_ERROR_UNSUPPORTED for unsupported interfaces,
    // including every Direct3D 11.x-or-newer interface. E_NOINTERFACE is not
    // the contract of this DXGI method.
    dxgi_diag(
        "IDXGIAdapter::CheckInterfaceSupport "
        "iid=%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x -> "
        "DXGI_ERROR_UNSUPPORTED",
        iid[3], iid[2], iid[1], iid[0], iid[5], iid[4], iid[7], iid[6],
        iid[8], iid[9], iid[10], iid[11], iid[12], iid[13], iid[14], iid[15]);
    m.set_return(dxgi::ERROR_UNSUPPORTED);
}

} // namespace vitreo::winapi
