#pragma once
/// @file bluetooth_host.hpp
/// @brief Host Bluetooth radio contract.
///
/// Windows.Devices.Bluetooth reports what the machine's radio actually is:
/// its address, whether it is powered, and which roles it supports. Vitreo
/// answers those from the real host controller rather than inventing an
/// adapter, so a guest that enumerates Bluetooth sees this Mac's hardware —
/// including "no radio", which is a legitimate answer the guest must handle.
///
/// The interface keeps the WinRT layer free of any platform header; the
/// macOS backend lives behind make_platform_bluetooth_host().

#include <vitreo/common.hpp>

#include <memory>
#include <string>

namespace vitreo::hardware {

/// A snapshot of the default host radio.
struct BluetoothAdapterState {
    /// False when the machine has no Bluetooth controller at all.
    bool present = false;
    /// 48-bit BD_ADDR in the low bits, host byte order, as
    /// BluetoothAdapter.BluetoothAddress reports it.
    u64 address = 0;
    /// Radio powered on. A present-but-off radio is still an adapter.
    bool powered = false;
    bool classic_supported = false;
    bool low_energy_supported = false;
    /// Stable identifier for BluetoothAdapter.DeviceId.
    std::string device_id;
    std::string name;
};

class BluetoothHost {
public:
    virtual ~BluetoothHost() = default;

    /// Fills @p out with the default controller's current state.
    /// Returns false only when the platform query itself failed; a machine
    /// with no radio returns true with `present == false`.
    [[nodiscard]] virtual bool query_default_adapter(
        BluetoothAdapterState& out) = 0;
};

/// Builds the backend for this platform. Never null: a platform without an
/// implementation returns a host that reports no radio.
[[nodiscard]] std::unique_ptr<BluetoothHost> make_platform_bluetooth_host();

} // namespace vitreo::hardware
