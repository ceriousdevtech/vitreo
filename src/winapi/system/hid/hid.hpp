// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file hid.hpp
/// @brief hid.dll exports.
///
/// Vitreo exposes no HID device interfaces, so no guest handle is a HID
/// device and no HIDP_PREPARSED_DATA is ever issued. Every entry point
/// therefore reports exactly what Windows reports for a non-HID handle or
/// preparsed data it did not produce (tools/oracle_probe/hid_probe.expected):
/// HidD_* -> FALSE with ERROR_INVALID_PARAMETER for an open handle (it is not
/// a HID device) or ERROR_INVALID_HANDLE for NULL, INVALID_HANDLE_VALUE or a
/// closed handle, outputs untouched;
/// HidP_* -> HIDP_STATUS_INVALID_PREPARSED_DATA; the Max*ListLength queries
/// -> 0. x86 stdcall byte counts come from the SDK import library
/// (libhid.a decorations).

#include "winapi/core/guest_com.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::hardware { class BluetoothHost; }

namespace vitreo::winapi {

class HandleTable;

/// @p handles is the process handle table used to tell an open handle from
/// a stale one.
[[nodiscard]] DllExports create_hid_thunks(HandleTable* handles = nullptr);

/// winusb.dll: every export of the real DLL (winusb.lib decorations for the
/// x86 byte counts). Vitreo has no USB stack, so no process ever holds a
/// WinUSB interface handle: WinUsb_Initialize fails ERROR_INVALID_HANDLE
/// with *InterfaceHandle = NULL and every per-interface call reports the
/// error Windows gives for a handle it never issued
/// (tools/oracle_probe/s7_winusb_probe.expected). The descriptor parsers are
/// pure and fully implemented.
[[nodiscard]] DllExports create_winusb_thunks();

/// bthprops.cpl: the classic Bluetooth radio/device API. The radio is the
/// host controller as reported by @p host (a real, closable kernel handle in
/// @p handles); the SDP record parsers are pure and fully implemented
/// (Bluetooth Core Vol 3 Part B 3 data elements;
/// tools/oracle_probe/s7_bluetooth_probe.expected). @p caller re-enters guest
/// code for BluetoothSdpEnumAttributes' callback.
[[nodiscard]] DllExports create_bthprops_thunks(hardware::BluetoothHost* host,
                                                HandleTable* handles,
                                                GuestFunctionCaller caller);

} // namespace vitreo::winapi
