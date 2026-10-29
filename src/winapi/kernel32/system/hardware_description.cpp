// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/kernel32/system/hardware_description.hpp"

#include "capsule/registry/registry.hpp"
#include "core/cpu/cpuid_synth.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

using capsule::RegType;
using capsule::RegValue;

/// RegDeleteKey only removes a key without subkeys, so remove bottom-up.
void delete_tree(capsule::Registry& registry, capsule::RegHandle parent, const std::wstring& name) {
    auto key = registry.open_key(parent, name, false);
    if (key.is_err()) return;
    if (auto children = registry.enum_keys(key.value()); children.is_ok()) {
        for (const std::wstring& child : children.value()) delete_tree(registry, key.value(), child);
    }
    registry.close_key(key.value());
    (void)registry.delete_key(parent, name);
}

std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

struct ProcessorIdentity {
    std::string vendor;
    std::string brand;
    u32 family = 0, model = 0, stepping = 0;
};

/// What CPUID reports (Intel SDM Vol. 2A: leaf 0 vendor, leaf 1 signature,
/// leaves 0x80000002-4 brand).
ProcessorIdentity identity_from_cpuid() {
    ProcessorIdentity id;
    u32 r[4]{};
    cpu::ops::synth_cpuid_on(0, 0, 0, true, r[0], r[1], r[2], r[3]);
    char vendor[13]{};
    std::memcpy(vendor + 0, &r[1], 4);
    std::memcpy(vendor + 4, &r[3], 4);
    std::memcpy(vendor + 8, &r[2], 4);
    id.vendor = vendor;
    cpu::ops::synth_cpuid_on(0, 1, 0, true, r[0], r[1], r[2], r[3]);
    const u32 base_family = (r[0] >> 8) & 0xF;
    id.family = base_family + (base_family == 0xF ? (r[0] >> 20) & 0xFF : 0);
    id.model = ((r[0] >> 4) & 0xF) |
               ((base_family == 0x6 || base_family == 0xF) ? ((r[0] >> 16) & 0xF) << 4 : 0);
    id.stepping = r[0] & 0xF;
    char brand[49]{};
    for (u32 i = 0; i < 3; ++i) {
        cpu::ops::synth_cpuid_on(0, 0x80000002 + i, 0, true, r[0], r[1], r[2], r[3]);
        std::memcpy(brand + 16 * i, r, 16);
    }
    id.brand = brand;
    while (!id.brand.empty() && id.brand.back() == ' ') id.brand.pop_back();
    return id;
}

} // namespace

void publish_hardware_description(capsule::Registry& registry) {
    auto system = registry.open_key(capsule::HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System", true);
    if (system.is_err()) return;
    delete_tree(registry, system.value(), L"CentralProcessor");

    // Measured on Windows 11 (tools/oracle_probe/processor_topology_probe
    // "reg"): one key per logical processor, ~MHz REG_DWORD, and
    // Identifier / ProcessorNameString / VendorIdentifier REG_SZ, with
    // Identifier "Intel64 Family <f> Model <m> Stepping <s>" in decimal.
    const ProcessorIdentity id = identity_from_cpuid();
    char identifier[96];
    std::snprintf(identifier, sizeof(identifier), "Intel64 Family %u Model %u Stepping %u", id.family, id.model,
                  id.stepping);
    const auto& topology = processor_topology::topology();
    const u32 tsc_mhz = static_cast<u32>(guest_tsc_hz() / 1'000'000);
    for (u32 p = 0; p < topology.count; ++p) {
        auto key = registry.open_key(system.value(), L"CentralProcessor\\" + std::to_wstring(p), true);
        if (key.is_err()) continue;
        const u32 mhz = p < topology.max_mhz.size() && topology.max_mhz[p] != 0 ? topology.max_mhz[p] : tsc_mhz;
        (void)registry.set_value(key.value(), L"~MHz", RegValue{RegType::Dword, {mhz}});
        (void)registry.set_value(key.value(), L"Identifier", RegValue{RegType::Sz, {widen(identifier)}});
        (void)registry.set_value(key.value(), L"ProcessorNameString", RegValue{RegType::Sz, {widen(id.brand)}});
        (void)registry.set_value(key.value(), L"VendorIdentifier", RegValue{RegType::Sz, {widen(id.vendor)}});
        // The kernel's KF_* feature word. Not derived from the CPU model yet:
        // kept at the value Vitreo always published.
        (void)registry.set_value(key.value(), L"FeatureSet", RegValue{RegType::Dword, {u32{0xFFFFFFFF}}});
        registry.close_key(key.value());
    }

    // System\BIOS mirrors the SMBIOS identity, and System carries the machine
    // Identifier and SystemBiosVersion. Names and types measured on Windows 11
    // (processor_topology_probe "reg"): the releases and EnclosureType are
    // REG_DWORD, an absent string is an empty REG_SZ, and no serial number is
    // published here.
    const virtual_firmware::Identity firmware = virtual_firmware::identity();
    delete_tree(registry, system.value(), L"BIOS");
    if (auto bios = registry.open_key(system.value(), L"BIOS", true); bios.is_ok()) {
        const auto dword = [&](const wchar_t* name, u32 value) {
            (void)registry.set_value(bios.value(), name, RegValue{RegType::Dword, {value}});
        };
        const auto text = [&](const wchar_t* name, const std::string& value) {
            (void)registry.set_value(bios.value(), name, RegValue{RegType::Sz, {widen(value)}});
        };
        dword(L"BiosMajorRelease", firmware.bios_major);
        dword(L"BiosMinorRelease", firmware.bios_minor);
        dword(L"ECFirmwareMajorRelease", firmware.ec_major);
        dword(L"ECFirmwareMinorRelease", firmware.ec_minor);
        dword(L"EnclosureType", firmware.enclosure_type);
        text(L"BaseBoardManufacturer", firmware.board_manufacturer);
        text(L"BaseBoardProduct", firmware.board_product);
        text(L"BaseBoardVersion", firmware.board_version);
        text(L"BIOSReleaseDate", firmware.bios_release_date);
        text(L"BIOSVendor", firmware.bios_vendor);
        text(L"BIOSVersion", firmware.bios_version);
        text(L"SystemFamily", firmware.system_family);
        text(L"SystemManufacturer", firmware.system_manufacturer);
        text(L"SystemProductName", firmware.system_product);
        text(L"SystemSKU", firmware.system_sku);
        text(L"SystemVersion", firmware.system_version);
        registry.close_key(bios.value());
    }
    (void)registry.set_value(system.value(), L"Identifier", RegValue{RegType::Sz, {std::wstring(L"AT/AT COMPATIBLE")}});
    // Windows lists the ACPI OEM identification, the SMBIOS BIOS version and
    // the vendor's release; Vitreo has no ACPI tables, so only the SMBIOS
    // version is published.
    std::vector<std::wstring> bios_versions;
    if (!firmware.bios_version.empty()) bios_versions.push_back(widen(firmware.bios_version));
    (void)registry.set_value(system.value(), L"SystemBiosVersion", RegValue{RegType::MultiSz, {bios_versions}});
    registry.close_key(system.value());
}

} // namespace vitreo::winapi
