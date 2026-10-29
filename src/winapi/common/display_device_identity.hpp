// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file display_device_identity.hpp
/// @brief Stable Windows-visible identity for Vitreo's Metal display adapter.

#include <vitreo/common.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vitreo::capsule {
class Registry;
}

namespace vitreo::winapi::display_device_identity {

// This is the identity of Vitreo's real Metal-backed virtual Windows adapter.
// Hardware properties such as the display name and memory size remain sourced
// from the host Metal device rather than from these identity strings.
inline constexpr char INSTANCE_ID_A[] = "ROOT\\VITREO_METAL\\0000";
inline constexpr char16_t INSTANCE_ID_W[] = u"ROOT\\VITREO_METAL\\0000";

// The root enumerator's device/hardware ID identifies the device type. The
// PnP manager combines it with the separate instance component ("0000") to
// form INSTANCE_ID_*; SPDRP_HARDWAREID must not return that combined ID.
inline constexpr char HARDWARE_ID_A[] = "ROOT\\VITREO_METAL";
inline constexpr char16_t HARDWARE_ID_W[] = u"ROOT\\VITREO_METAL";

inline constexpr char DEVICE_KEY_A[] =
    "\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\"
    "ROOT\\VITREO_METAL\\0000";
inline constexpr char16_t DEVICE_KEY_W[] =
    u"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\"
    u"ROOT\\VITREO_METAL\\0000";

// D3DKMT_ADAPTERADDRESS is part of the Windows adapter namespace rather than
// host hardware discovery. Apple Silicon's integrated GPU has no PCI B/D/F,
// so Vitreo assigns its single root-enumerated Metal adapter slot 0:0:0. This
// tuple must never be presented as an Apple PCI address or used to derive host
// hardware properties.
inline constexpr u32 VIRTUAL_BUS_NUMBER = 0;
inline constexpr u32 VIRTUAL_DEVICE_NUMBER = 0;
inline constexpr u32 VIRTUAL_FUNCTION_NUMBER = 0;

// =============================================================================
// Per-capsule advertised adapter identity
// =============================================================================

/// The display-class key that carries the per-capsule configuration, relative
/// to HKEY_LOCAL_MACHINE. `{4d36e968-e325-11ce-bfc1-08002be10318}` is the
/// Windows `Display` setup class GUID, and `\0000` is the first (only)
/// adapter instance under it. This is where a real Windows display driver
/// publishes `DriverDesc`, `DriverVersion` and its `HardwareInformation.*`
/// values, so it is where Vitreo reads its overrides from too.
inline constexpr wchar_t ADAPTER_CLASS_KEY[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
    L"{4d36e968-e325-11ce-bfc1-08002be10318}\\0000";

/// Advertised PCI identity of the display adapter.
///
/// Every field is optional and EMPTY BY DEFAULT. Empty means "this surface
/// keeps reporting whatever it truthfully detects", which for Vitreo is
/// Apple's vendor ID (0x106B) with no device ID and the real Metal device
/// name. A populated field is the *user* asserting a hardware identity for
/// one capsule; it is never Vitreo claiming a capability it does not have.
///
/// Guests routinely gate rendering paths on the PCI identity instead of on
/// capabilities (per-GPU databases, driver blocklists, vendor allowlists).
/// Apple's vendor ID cannot appear on real Windows hardware, so every such
/// gate takes its unknown-vendor fallback. Exposing the identity as a knob
/// lets users configure an explicit per-capsule identity.
struct AdapterIdentity {
    std::optional<u32> vendor_id;
    std::optional<u32> device_id;
    std::optional<u32> subsystem_id;
    std::optional<u32> revision;
    std::optional<std::string> description;
    /// Four 16-bit components packed big-end-first, as a Windows
    /// LARGE_INTEGER driver version.
    std::optional<u64> driver_version;

    /// True when the capsule asserts a PCI identity rather than leaving the
    /// truthful detected one in place.
    [[nodiscard]] bool has_pci_identity() const noexcept {
        return vendor_id.has_value() || device_id.has_value() ||
               subsystem_id.has_value() || revision.has_value();
    }
};

/// User-mode driver version reported when nothing is configured: 32.0.101.5972.
/// See `dxgi_umd_driver_version()` for why this is not 1.0.0.0.
inline constexpr u64 DEFAULT_DRIVER_VERSION =
    (32ull << 48) | (0ull << 32) | (101ull << 16) | 5972ull;

/// The identity configured for this process. Defaults to all-empty.
[[nodiscard]] const AdapterIdentity& adapter_identity() noexcept;

/// Install a configured identity. Called once during runtime start-up.
void set_adapter_identity(AdapterIdentity identity) noexcept;

/// The truthful name of the display adapter this host provides: the active
/// graphics backend's device name (the Metal device name, e.g. "Apple M4 Pro"),
/// or empty when no backend is installed or it is unavailable. This is what
/// IDXGIAdapter::GetDesc reports when the capsule configures no description.
[[nodiscard]] std::string detected_adapter_name();

/// The description of the adapter every surface that names it reports: the
/// capsule's configured `DriverDesc` when there is one, otherwise
/// detected_adapter_name(). Empty when there is neither -- no adapter to
/// describe.
[[nodiscard]] std::string resolved_adapter_description();

/// Read the per-capsule overrides out of `ADAPTER_CLASS_KEY`, install them,
/// and publish the resolved identity back as the informational
/// `HardwareInformation.*` / `MatchingDeviceId` / `DriverVersion` values a
/// guest can read directly. Absent values leave the corresponding surface at
/// its truthful default.
///
/// The `HardwareInformation.AdapterString` / `.ChipType` mirrors are rewritten
/// on every call from resolved_adapter_description(), so a value left in the
/// hive by an earlier identity never survives; with no adapter to describe
/// they are deleted. They are `REG_SZ`, as the oracle shows.
void configure_adapter_identity_from_registry(capsule::Registry& registry);

/// Parse "a.b.c.d" into the packed LARGE_INTEGER form. Returns empty when the
/// text is not exactly four decimal components.
[[nodiscard]] std::optional<u64> parse_driver_version(std::string_view text);

/// Format a packed driver version back as "a.b.c.d".
[[nodiscard]] std::string format_driver_version(u64 packed);

/// Parse "vendor:device" in hex, as used by the VITREO_*_ADAPTER_ID overrides.
/// Returns empty when the text is not of that form.
[[nodiscard]] std::optional<std::pair<u32, u32>>
parse_vendor_device_spec(const char* spec);

} // namespace vitreo::winapi::display_device_identity
