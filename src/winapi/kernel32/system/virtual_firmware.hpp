#pragma once

/// @file virtual_firmware.hpp
/// @brief The machine's SMBIOS firmware tables, built from the host.
///
/// An SMBIOS 3.6.0 structure table (DMTF DSP0134 3.6.0) describing the real
/// Mac: firmware, system, board, enclosure, processor, caches and memory, all
/// from platform::host_hardware(), the guest CPUID and the processor topology.
/// GetSystemFirmwareTable('RSMB') returns it, and the legacy BIOS window at
/// 000F0000h carries its _SM3_ and _SM_ entry points (what a physical-memory
/// scan or a hardware tool's driver reads). HARDWARE\DESCRIPTION\System\BIOS
/// publishes the same identity (hardware_description.cpp).

#include <vitreo/common.hpp>

#include <span>
#include <string>

namespace vitreo::winapi::virtual_firmware {

constexpr u64 kBiosBase = 0x000F0000;
constexpr usize kBiosSize = 0x00010000;

/// The SMBIOS version the table implements.
constexpr u8 kSmbiosMajor = 3;
constexpr u8 kSmbiosMinor = 6;
constexpr u8 kSmbiosDocrev = 0;

/// The identity the firmware tables describe.
struct Identity {
    std::string bios_vendor;
    std::string bios_version;
    std::string bios_release_date;   ///< mm/dd/yyyy, empty when unknown
    u8 bios_major = 0xFF;            ///< 0FFh: not expressible (see .cpp)
    u8 bios_minor = 0xFF;
    u8 ec_major = 0xFF;              ///< 0FFh: no field-upgradeable EC firmware reported
    u8 ec_minor = 0xFF;
    std::string system_manufacturer;
    std::string system_product;
    std::string system_version;
    std::string system_serial;
    std::string system_sku;
    std::string system_family;
    std::string board_manufacturer;
    std::string board_product;
    std::string board_version;
    u8 enclosure_type = 0x02;        ///< SMBIOS enclosure type, 02h Unknown
};

/// The identity of the host, as the firmware tables publish it.
[[nodiscard]] Identity identity();

/// The structure table (no header), rebuilt when a test installs a different
/// host profile.
[[nodiscard]] std::span<const u8> smbios_table();

/// The 64 KiB legacy BIOS window at kBiosBase: the _SM3_ (at 0) and _SM_ (at
/// 20h) entry points and the structure table they point to.
[[nodiscard]] std::span<const u8> bios_window();

} // namespace vitreo::winapi::virtual_firmware
