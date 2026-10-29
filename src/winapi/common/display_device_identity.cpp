// SPDX-License-Identifier: LGPL-2.1-only

/// @file display_device_identity.cpp
/// @brief Per-capsule advertised display-adapter identity.

#include "winapi/common/display_device_identity.hpp"

#include "capsule/registry/registry.hpp"
#include "graphics/contracts/backend_device.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::winapi::display_device_identity {

namespace {

AdapterIdentity& mutable_identity() noexcept {
    static AdapterIdentity identity;
    return identity;
}

[[nodiscard]] std::wstring widen(std::string_view text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char character : text) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return wide;
}

/// Registry strings are stored wide. The identity values are ASCII by
/// contract (PCI identifiers, dotted version numbers, adapter names), so a
/// narrowing copy that drops non-ASCII is the honest translation rather than
/// a silent mojibake.
[[nodiscard]] std::string narrow(std::wstring_view text) {
    std::string narrow_text;
    narrow_text.reserve(text.size());
    for (const wchar_t character : text) {
        narrow_text.push_back(
            character < 0x80 ? static_cast<char>(character) : '?');
    }
    return narrow_text;
}

[[nodiscard]] std::optional<u32> read_dword(capsule::Registry& registry,
                                            capsule::RegHandle key,
                                            const wchar_t* name) {
    auto value = registry.query_value(key, name);
    if (value.is_err()) return std::nullopt;
    const auto& data = value.value().data;
    if (const auto* dword = std::get_if<u32>(&data)) return *dword;
    // A hand-edited hive may carry the number as a string. Accept it rather
    // than silently ignoring a value the user plainly meant.
    if (const auto* text = std::get_if<std::wstring>(&data)) {
        const std::string ascii = narrow(*text);
        char* end = nullptr;
        const unsigned long parsed =
            std::strtoul(ascii.c_str(), &end, ascii.rfind("0x", 0) == 0 ? 16 : 0);
        if (end != nullptr && end != ascii.c_str() && *end == '\0') {
            return static_cast<u32>(parsed);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> read_string(capsule::Registry& registry,
                                                     capsule::RegHandle key,
                                                     const wchar_t* name) {
    auto value = registry.query_value(key, name);
    if (value.is_err()) return std::nullopt;
    const auto* text = std::get_if<std::wstring>(&value.value().data);
    if (text == nullptr || text->empty()) return std::nullopt;
    return narrow(*text);
}

void write_string(capsule::Registry& registry, capsule::RegHandle key,
                  const wchar_t* name, std::string_view value) {
    (void)registry.set_value(key, name,
                             capsule::RegValue{capsule::RegType::Sz,
                                               widen(value)});
}

} // namespace

const AdapterIdentity& adapter_identity() noexcept {
    return mutable_identity();
}

void set_adapter_identity(AdapterIdentity identity) noexcept {
    mutable_identity() = std::move(identity);
}

std::string detected_adapter_name() {
    const auto* device = graphics::active_backend_device();
    return (device != nullptr && device->available())
        ? device->backend_device_name()
        : std::string{};
}

std::string resolved_adapter_description() {
    const auto& configured = adapter_identity();
    if (configured.description && !configured.description->empty()) {
        return *configured.description;
    }
    return detected_adapter_name();
}

std::optional<u64> parse_driver_version(std::string_view text) {
    std::array<u32, 4> parts{};
    const std::string owned(text);
    const char* cursor = owned.c_str();
    for (usize index = 0; index < parts.size(); ++index) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(cursor, &end, 10);
        if (end == cursor) return std::nullopt;
        parts[index] = static_cast<u32>(value & 0xFFFFu);
        if (index + 1 == parts.size()) {
            if (*end != '\0') return std::nullopt;
            break;
        }
        if (*end != '.') return std::nullopt;
        cursor = end + 1;
    }
    return (static_cast<u64>(parts[0]) << 48) |
           (static_cast<u64>(parts[1]) << 32) |
           (static_cast<u64>(parts[2]) << 16) |
           static_cast<u64>(parts[3]);
}

std::string format_driver_version(u64 packed) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu.%llu.%llu.%llu",
                  static_cast<unsigned long long>((packed >> 48) & 0xFFFFu),
                  static_cast<unsigned long long>((packed >> 32) & 0xFFFFu),
                  static_cast<unsigned long long>((packed >> 16) & 0xFFFFu),
                  static_cast<unsigned long long>(packed & 0xFFFFu));
    return std::string(buffer);
}

std::optional<std::pair<u32, u32>> parse_vendor_device_spec(const char* spec) {
    if (spec == nullptr) return std::nullopt;
    char* end = nullptr;
    const unsigned long vendor = std::strtoul(spec, &end, 16);
    if (end == nullptr || end == spec || *end != ':') return std::nullopt;
    const char* device_text = end + 1;
    char* device_end = nullptr;
    const unsigned long device = std::strtoul(device_text, &device_end, 16);
    if (device_end == device_text) return std::nullopt;
    return std::pair<u32, u32>{static_cast<u32>(vendor),
                               static_cast<u32>(device)};
}

void configure_adapter_identity_from_registry(capsule::Registry& registry) {
    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 ADAPTER_CLASS_KEY, true);
    if (key.is_err()) return;
    const capsule::RegHandle handle = key.value();

    AdapterIdentity identity;
    // Per-capsule configuration values for the identity reported to guests.
    identity.vendor_id = read_dword(registry, handle, L"VideoPciVendorID");
    identity.device_id = read_dword(registry, handle, L"VideoPciDeviceID");
    identity.subsystem_id = read_dword(registry, handle, L"VideoPciSubSystemID");
    identity.revision = read_dword(registry, handle, L"VideoPciRevisionID");
    identity.description = read_string(registry, handle, L"DriverDesc");
    if (const auto version = read_string(registry, handle, L"DriverVersion")) {
        identity.driver_version = parse_driver_version(*version);
    }

    set_adapter_identity(identity);

    // Publish the resolved identity so a guest that reads the registry
    // directly cannot see something different from what DXGI/D3D9/DDraw
    // report. These are informational mirrors and are never read back as
    // configuration, so republishing them cannot drift the configured value.
    const u64 version =
        identity.driver_version.value_or(DEFAULT_DRIVER_VERSION);
    write_string(registry, handle, L"DriverVersion",
                 format_driver_version(version));
    // The adapter-name mirrors describe the RESOLVED description on every
    // start. Publishing them only when the capsule configured a DriverDesc
    // left a value written by an earlier identity (an old experiment, another
    // host's GPU) in the hive forever, so a guest reading the registry saw a
    // different GPU than DXGI reports. The detected name goes only into these
    // mirrors -- never into DriverDesc, which is configuration input and would
    // turn a detected name into a configured one on the next start. With
    // nothing to describe a stale mirror is removed, as a driver that is not
    // installed has no HardwareInformation. REG_SZ, as the oracle shows.
    const std::string description = resolved_adapter_description();
    for (const wchar_t* mirror : {L"HardwareInformation.AdapterString",
                                  L"HardwareInformation.ChipType"}) {
        if (!description.empty()) {
            write_string(registry, handle, mirror, description);
        } else {
            (void)registry.delete_value(handle, mirror);
        }
    }
    if (identity.has_pci_identity()) {
        char matching[96];
        std::snprintf(matching, sizeof(matching),
                      "pci\\ven_%04x&dev_%04x&subsys_%08x&rev_%02x",
                      identity.vendor_id.value_or(0),
                      identity.device_id.value_or(0),
                      identity.subsystem_id.value_or(0),
                      identity.revision.value_or(0));
        write_string(registry, handle, L"MatchingDeviceId", matching);
    } else {
        // Root-enumerated, not PCI. Say so rather than inventing a PCI path.
        write_string(registry, handle, L"MatchingDeviceId",
                     "root\\vitreo_metal");
    }
    registry.close_key(handle);
}

} // namespace vitreo::winapi::display_device_identity
