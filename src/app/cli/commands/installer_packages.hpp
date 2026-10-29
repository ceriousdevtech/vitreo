// SPDX-License-Identifier: LGPL-2.1-only
/// @file installer_packages.hpp
/// @brief Windows installer packages Finder can hand to `vitreo run`.

#pragma once

#include <string_view>

namespace vitreo::cli {

/// How opening a path should be treated.
///
/// Windows Installer file extensions are documented at
/// https://learn.microsoft.com/en-us/windows/win32/msi/windows-installer-file-extensions
/// Explorer opens `.msi` with `msiexec /i` and `.msp` with `msiexec /p`
/// (https://learn.microsoft.com/en-us/windows/win32/msi/command-line-options).
/// `.mst` and `.msm` are not programs. MSIX, AppX, and `.msu` are separate
/// installers (App Installer and `wusa.exe`), not Windows Installer packages.
enum class InstallerPackageAction {
    None,          ///< Not an installer package; run it as a PE.
    Install,       ///< `.msi` — `msiexec /i`.
    Patch,         ///< `.msp` — `msiexec /p`.
    Unsupported,   ///< A recognized installer type this runtime does not run.
};

struct InstallerPackagePlan {
    InstallerPackageAction action = InstallerPackageAction::None;
    /// Static explanation when @c action is Unsupported. Empty otherwise.
    const char* detail = "";
};

[[nodiscard]] InstallerPackagePlan classify_installer_package(
    std::string_view path);

} // namespace vitreo::cli
