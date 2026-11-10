// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// Whether this process has created a Direct3D device on the adapter.
///
/// IDXGIAdapter3::SetVideoMemoryReservation depends on it. Measured on an
/// NVIDIA RTX 4080 SUPER (tools/oracle_probe/dxgi_reservation_probe.c):
/// every reservation, 0 included, is E_INVALIDARG until a D3D11 or D3D12
/// device has been created on the adapter, and once one has, reservations
/// keep succeeding after that device is released. Vitreo exposes one
/// adapter, so one process-wide flag carries the rule.

#include <atomic>

namespace vitreo::winapi::graphics_common {

inline std::atomic<bool> g_adapter_has_had_device{false};

/// Called by D3D11 and D3D12 device creation on success.
inline void note_adapter_device_created() {
    g_adapter_has_had_device.store(true, std::memory_order_release);
}

/// Tests only: a test binary creates devices in many tests of one process.
inline void reset_adapter_device_presence_for_testing() {
    g_adapter_has_had_device.store(false, std::memory_order_release);
}

inline bool adapter_has_had_device() {
    return g_adapter_has_had_device.load(std::memory_order_acquire);
}

}  // namespace vitreo::winapi::graphics_common
