// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_dd7.cpp
/// @brief IDirectDraw7-only methods: GetSurfaceFromDC, RestoreAllSurfaces,
///        TestCooperativeLevel, GetDeviceIdentifier and mode testing.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/common/display_device_identity.hpp"

#include <algorithm>
#include <string>

namespace vitreo::winapi {

u32 DirectDrawManager::get_surface_from_dc(GuestAddr object, u32 hdc,
                                           GuestAddr out_surface) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (out_surface != 0) (void)m_memory->write_u32(out_surface, 0);
    VITREO_TRACE("[ddraw] GetSurfaceFromDC hdc=0x%x -> DDERR_NOTFOUND\n", hdc);
    // MAKE_DDHRESULT(499) — no surface owns this DC.
    return 0x887601F3u;
}

u32 DirectDrawManager::restore_all_surfaces(GuestAddr object) {
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    // Surfaces are host-backed and never enter a lost state.
    VITREO_TRACE("[ddraw] RestoreAllSurfaces -> DD_OK\n");
    return ddraw::DD_OK;
}

u32 DirectDrawManager::test_cooperative_level(GuestAddr object) {
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    // The guest never loses exclusive mode to another process here.
    return ddraw::DD_OK;
}

u32 DirectDrawManager::get_device_identifier(GuestAddr object,
                                             GuestAddr out_id, u32 flags) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (out_id == 0) return ddraw::E_INVALIDARG;
    (void)flags;
    // DDDEVICEIDENTIFIER2: char szDriver[512], szDescription[512],
    // LARGE_INTEGER liDriverVersion, DWORD dwVendorId, dwDeviceId,
    // dwSubSysId, dwRevision, GUID guidDeviceIdentifier, DWORD dwWHQLLevel.
    constexpr u32 kDriverOff = 0;
    constexpr u32 kDescOff = 512;
    constexpr u32 kVersionOff = 1024;
    constexpr u32 kStructSize = 1024 + 8 + 4 * 4 + 16 + 4;
    for (u32 off = 0; off < kStructSize; off += 4) {
        (void)m_memory->write_u32(out_id + off, 0);
    }
    constexpr u32 kVendorOff = 1032;
    constexpr u32 kDeviceOff = 1036;
    constexpr u32 kSubSysOff = 1040;
    constexpr u32 kRevisionOff = 1044;
    const char kDriver[] = "vitreo.dll";
    const std::string description = [] {
        // The same configured adapter identity DXGI and D3D9 report, so a
        // guest cannot see two different adapters through two APIs. Absent a
        // configured identity this is the unchanged default string.
        const auto& configured =
            display_device_identity::adapter_identity();
        return configured.description.value_or(
            std::string("Vitreo DirectDraw (Metal)"));
    }();
    (void)m_memory->write_bytes(out_id + kDriverOff,
                                reinterpret_cast<const u8*>(kDriver),
                                sizeof(kDriver));
    (void)m_memory->write_bytes(
        out_id + kDescOff, reinterpret_cast<const u8*>(description.c_str()),
        std::min<usize>(description.size() + 1, 512));
    {
        const auto& configured = display_device_identity::adapter_identity();
        if (configured.has_pci_identity()) {
            (void)m_memory->write_u32(out_id + kVendorOff,
                                      configured.vendor_id.value_or(0));
            (void)m_memory->write_u32(out_id + kDeviceOff,
                                      configured.device_id.value_or(0));
            (void)m_memory->write_u32(out_id + kSubSysOff,
                                      configured.subsystem_id.value_or(0));
            (void)m_memory->write_u32(out_id + kRevisionOff,
                                      configured.revision.value_or(0));
        }
        const u64 version = configured.driver_version.value_or(
            configured.has_pci_identity() || configured.description
                ? display_device_identity::DEFAULT_DRIVER_VERSION
                : 1ull);
        // A non-zero driver version keeps callers from treating this as a stub.
        (void)m_memory->write_u32(out_id + kVersionOff,
                                  static_cast<u32>(version & 0xFFFFFFFFull));
        (void)m_memory->write_u32(out_id + kVersionOff + 4,
                                  static_cast<u32>(version >> 32));
    }
    VITREO_TRACE("[ddraw] GetDeviceIdentifier -> DD_OK\n");
    return ddraw::DD_OK;
}

u32 DirectDrawManager::start_mode_test(GuestAddr object, GuestAddr modes,
                                       u32 num_modes, u32 flags) {
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    (void)modes; (void)num_modes; (void)flags;
    // Refresh-rate mode testing has no meaning against a windowed host
    // surface; DDERR_TESTFINISHED tells the caller there is nothing to run.
    return 0x887601C0u;  // MAKE_DDHRESULT(448) DDERR_TESTFINISHED
}

u32 DirectDrawManager::evaluate_mode(GuestAddr object, u32 flags,
                                     GuestAddr out_timeout) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    (void)flags;
    if (out_timeout != 0) (void)m_memory->write_u32(out_timeout, 0);
    return 0x887601C0u;  // DDERR_TESTFINISHED
}

} // namespace vitreo::winapi
