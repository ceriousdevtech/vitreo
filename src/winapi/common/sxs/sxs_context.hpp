// SPDX-License-Identifier: LGPL-2.1-only
/// @file sxs_context.hpp
/// @brief The side-by-side assembly store and activation-context generation.
///
/// Microsoft Learn: "Assembly Searching Sequence", "Publisher Configuration
/// Files", "Application Configuration Files", "Private Assemblies", "Shared
/// Assemblies", "Activation Contexts", "Dynamic-link library redirection".
///
/// The store layout follows what Windows Installer measurably produces for a
/// global Win32 assembly (.docs/contracts/msi/registration.md "Assemblies"):
/// %WINDIR%\WinSxS\Fusion\<arch>_<name>_<token>_<language|none>_<hash>\
/// <major.minor>\<version>\, holding the assembly's files and its manifest
/// renamed <arch>_<name>_<token>_<version>_<language|none>_<hash>.manifest.
/// Microsoft does not document the hashes; Vitreo keeps the visible shape and
/// uses its own deterministic hash (.docs/contracts/sxs.md). No Windows API
/// takes or returns these names.

#pragma once

#include "winapi/common/sxs/sxs_manifest.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi::sxs {

// winerror.h (Windows SDK 10.0.26100.0).
inline constexpr u32 kErrorSxsSectionNotFound = 14000;
inline constexpr u32 kErrorSxsCantGenActCtx = 14001;
inline constexpr u32 kErrorSxsAssemblyNotFound = 14003;
inline constexpr u32 kErrorSxsManifestFormatError = 14004;
inline constexpr u32 kErrorSxsManifestParseError = 14005;
inline constexpr u32 kErrorSxsKeyNotFound = 14007;
inline constexpr u32 kErrorSxsVersionConflict = 14008;
inline constexpr u32 kErrorSxsProcessDefaultAlreadySet = 14011;
inline constexpr u32 kErrorSxsDuplicateDllName = 14021;
inline constexpr u32 kErrorSxsPolicyParseError = 14029;

/// The store manifest's base name for a complete identity (version,
/// processorArchitecture and type required).
[[nodiscard]] std::string store_key(const Identity& identity);

struct StoreEntry {
    Identity identity;
    std::string manifest_path;   ///< <directory>\<store_key>.manifest
    std::string directory;       ///< WinSxS\Fusion\<family>\<major.minor>\<version>
    Manifest manifest;
};

/// The installed assembly with exactly @p identity, if any.
[[nodiscard]] std::optional<StoreEntry> store_find(capsule::Vfs& vfs, const std::string& windows_dir,
                                                   const Identity& identity);

/// Publisher policy (Learn "Publisher Configuration Files"): the version the
/// highest installed policy.<major>.<minor>.<name> redirects @p reference's
/// version to, or nullopt when no policy covers it.
[[nodiscard]] std::optional<Version> store_publisher_policy(capsule::Vfs& vfs, const std::string& windows_dir,
                                                            const Identity& reference);

/// Install an assembly (or a win32-policy assembly) into the store.
/// @p files maps each <file name> of the manifest to its source path; a
/// non-empty @p catalog (the assembly's signature catalog) is kept beside
/// the manifest under the same base name with ".cat".
/// Returns 0 or a Win32 error (ERROR_SXS_MANIFEST_PARSE_ERROR,
/// ERROR_SXS_MISSING_ASSEMBLY_IDENTITY_ATTRIBUTE as _FORMAT_ERROR, a missing
/// listed file as ERROR_FILE_NOT_FOUND, a write failure as
/// ERROR_ACCESS_DENIED).
[[nodiscard]] u32 store_install(capsule::Vfs& vfs, const std::string& windows_dir,
                                std::span<const u8> manifest_bytes,
                                const std::map<std::string, std::string>& files,
                                const std::string& catalog = {});

/// Remove an installed assembly; false when it was not installed.
bool store_remove(capsule::Vfs& vfs, const std::string& windows_dir, const Identity& identity);

/// One assembly of a generated activation context.
struct ResolvedAssembly {
    std::optional<Identity> identity;  ///< absent for an identity-less root manifest
    std::string manifest_path;         ///< the manifest (or PE) it was read from
    std::string directory;             ///< where its <file> entries live
    std::vector<std::string> files;
    bool shared = false;               ///< found in the store
    /// An operating-system assembly whose files are Vitreo's built-in
    /// modules (no file on disk): its names appear in the context's
    /// sections, but the loader is not redirected to a path for them.
    bool builtin = false;
};

/// Microsoft's publicKeyToken for the assemblies Windows itself ships.
inline constexpr std::string_view kWindowsPublicKeyToken = "6595b64144ccf1df";

/// A generated activation context: the assembly roster (index 0 is the
/// root manifest) and its DLL redirection section.
struct ContextData {
    std::vector<ResolvedAssembly> assemblies;
    /// lower-cased <file name> -> {full path, roster index}; built-in
    /// assemblies' files are not here (they load as built-in modules).
    std::map<std::string, std::pair<std::string, usize>> dll_redirection;

    /// The redirected path for a DLL base name (case-insensitive), if any.
    [[nodiscard]] std::optional<std::string> redirect_dll(std::string_view base_name) const;
};

struct ContextRequest {
    std::span<const u8> manifest;     ///< the root manifest's bytes
    std::string manifest_path;        ///< reported for the root assembly
    std::string root_directory;       ///< directory of the root's own <file>s
    std::string application_dir;      ///< private probing base
    std::string architecture;         ///< "x86" / "amd64": resolves "*"
    std::string windows_dir = "C:\\Windows";
    std::optional<AppConfig> config;  ///< <application>.config, when present
};

struct ContextResult {
    std::optional<ContextData> data;
    u32 error = 0;            ///< Win32 error when data is absent
    std::string detail;       ///< which assembly failed, for the log
};

/// Generate an activation context from a root manifest following Learn's
/// assembly searching sequence: application configuration redirects, then
/// publisher policy (unless disabled), then the shared store, then private
/// probing in the application directory; dependencies of every found
/// assembly resolve the same way.
[[nodiscard]] ContextResult build_context(capsule::Vfs& vfs, const ContextRequest& request);

}  // namespace vitreo::winapi::sxs
