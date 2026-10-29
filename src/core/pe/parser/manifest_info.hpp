// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include "core/pe/parser/pe_parser.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::pe {

enum class RequestedExecutionLevel {
    Unspecified,
    AsInvoker,
    HighestAvailable,
    RequireAdministrator,
};

/// Parse requestedExecutionLevel from raw RT_MANIFEST bytes.
[[nodiscard]] RequestedExecutionLevel parse_manifest_execution_level(
    std::span<const u8> manifest) noexcept;

/// Locate the first RT_MANIFEST in a PE file and parse its execution level.
[[nodiscard]] RequestedExecutionLevel pe_requested_execution_level(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept;

/// Return whether the manifest declares Windows 10 compatibility via the
/// Microsoft supportedOS GUID.
[[nodiscard]] bool parse_manifest_supports_windows_10(
    std::span<const u8> manifest) noexcept;

[[nodiscard]] bool pe_supports_windows_10(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept;

/// Return whether the manifest declares Windows 8.1 compatibility
/// ({1f676c76-80e1-4239-95bb-83d0f6d0da78}).
[[nodiscard]] bool parse_manifest_supports_windows_81(
    std::span<const u8> manifest) noexcept;

[[nodiscard]] bool pe_supports_windows_81(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept;

/// Return whether the manifest declares Windows 8 compatibility
/// ({4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}, Learn: "Targeting your
/// application for Windows").
[[nodiscard]] bool parse_manifest_supports_windows_8(
    std::span<const u8> manifest) noexcept;

[[nodiscard]] bool pe_supports_windows_8(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept;

/// Return private side-by-side assembly names declared by dependentAssembly
/// elements. Windows probes appdir\<assembly-name> for these assemblies.
[[nodiscard]] std::vector<std::string> parse_manifest_private_assemblies(
    std::span<const u8> manifest);

[[nodiscard]] std::vector<std::string> pe_private_assemblies(
    std::span<const u8> pe_data,
    const ParsedPe& pe);

/// True when the manifest depends on Microsoft.Windows.Common-Controls with
/// major version 6 (Learn: Enabling Visual Styles): the process default
/// activation context then gives windows comctl32 v6's control classes.
[[nodiscard]] bool parse_manifest_common_controls_v6(std::span<const u8> manifest);
[[nodiscard]] bool pe_common_controls_v6(std::span<const u8> pe_data, const ParsedPe& pe);

/// True when the fusion/app manifest declares
/// `<activeCodePage>UTF-8</activeCodePage>` (Windows 10 1903+), which makes
/// GetACP / CP_ACP resolve to 65001 for the process.
/// The DPI awareness a manifest declares, as the DPI_AWARENESS_CONTEXT
/// pseudo-handle value (-1 unaware, -2 system, -3 per-monitor, -4 per-monitor
/// v2, -5 unaware GDI-scaled), or 0 when it declares none. <dpiAwareness>
/// (a list; the first recognised entry wins) takes precedence over
/// <dpiAware> (Learn: Application manifests).
[[nodiscard]] i64 parse_manifest_dpi_awareness_context(
    std::span<const u8> manifest) noexcept;
[[nodiscard]] i64 pe_dpi_awareness_context(
    std::span<const u8> pe_data, const ParsedPe& pe) noexcept;

[[nodiscard]] bool parse_manifest_active_code_page_utf8(
    std::span<const u8> manifest) noexcept;

[[nodiscard]] bool pe_active_code_page_utf8(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept;

/// True when an unelevated launch must elevate (or fail with
/// ERROR_ELEVATION_REQUIRED) before guest code runs.
///
/// `requireAdministrator` always needs elevation. `highestAvailable` elevates
/// too, because Vitreo's user models the dominant Windows home configuration:
/// a *protected administrator* -- a member of the Administrators group whose
/// processes run filtered until UAC consent. For that user, Windows runs a
/// `highestAvailable` image with the FULL admin token after a UAC prompt
/// (Vitreo's authorization dialog is that prompt). Modelling a standard user
/// here sends every pre-UAC installer stub, which marks its outer stub
/// `highestAvailable` and then hard-checks TokenGroups for
/// BUILTIN\Administrators, into a "please login as an Administrator"
/// refusal. Explicit ShellExecute "runas" still elevates regardless of
/// manifest.
[[nodiscard]] constexpr bool requests_elevation(
    RequestedExecutionLevel level) noexcept {
    return level == RequestedExecutionLevel::RequireAdministrator ||
           level == RequestedExecutionLevel::HighestAvailable;
}

/// Return whether Windows' legacy installer detection would treat this image
/// as an elevation candidate. This heuristic applies only to 32-bit GUI
/// executables without a requestedExecutionLevel manifest; explicit manifests
/// always win.
[[nodiscard]] bool is_legacy_installer_candidate(
    RequestedExecutionLevel level,
    const ParsedPe& pe,
    std::string_view executable_path) noexcept;

} // namespace vitreo::pe
