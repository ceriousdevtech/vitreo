// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file hardware_description.hpp
/// @brief HKLM\HARDWARE\DESCRIPTION\System, as Windows publishes it at boot.
///
/// Windows rebuilds HKLM\HARDWARE at every boot from the firmware and the
/// processors, and never saves it. Vitreo rebuilds it from the host hardware
/// profile and CPUID at every process start (Registry::save skips it), so a
/// guest that reads it sees the same machine as every other surface.

namespace vitreo::capsule {
class Registry;
}

namespace vitreo::winapi {

/// Replaces HARDWARE\DESCRIPTION\System\CentralProcessor (one key per
/// logical processor) and System\BIOS (the SMBIOS identity), and sets the
/// System key's Identifier and SystemBiosVersion.
void publish_hardware_description(capsule::Registry& registry);

} // namespace vitreo::winapi
