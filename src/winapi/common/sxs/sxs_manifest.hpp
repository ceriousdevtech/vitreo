// SPDX-License-Identifier: LGPL-2.1-only
/// @file sxs_manifest.hpp
/// @brief Side-by-side assembly identities and manifest / configuration files.
///
/// Microsoft Learn: "Manifest Files Reference" (assembly, assemblyIdentity,
/// file, dependency, dependentAssembly, noInheritable), "Publisher
/// Configuration Files" (bindingRedirect), "Application Configuration Files"
/// (configuration/windows/assemblyBinding, publisherPolicy apply="no").
/// Elements are matched by namespace URI and local name: the assembly schema
/// is urn:schemas-microsoft-com:asm.v1 (v2/v3 carry the same elements in
/// later schema revisions). Anything outside those namespaces is ignored, as
/// unknown elements are.

#pragma once

#include <vitreo/common.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::sxs {

/// Major.Minor.Build.Revision, each 0-65535 (Learn: assemblyIdentity
/// "version": four-part version number).
struct Version {
    std::array<u16, 4> parts{};
    [[nodiscard]] static std::optional<Version> parse(std::string_view text);
    [[nodiscard]] std::string str() const;
    auto operator<=>(const Version&) const = default;
};

/// An assemblyIdentity. Empty strings are absent attributes. Comparisons of
/// name, type, processorArchitecture, publicKeyToken and language are
/// case-insensitive.
struct Identity {
    std::string type;               ///< "win32", "win32-policy", ...
    std::string name;
    std::optional<Version> version;
    std::string architecture;       ///< x86, amd64, arm64, msil, ia64, wow64, "*"
    std::string public_key_token;   ///< 16 hex digits, or empty (private assembly)
    std::string language;           ///< culture, "*" / empty = neutral
};

/// "x" == "X" in the identity sense.
[[nodiscard]] bool iequal(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] std::string to_lower(std::string_view s);

/// bindingRedirect oldVersion="a" or "a-b", newVersion="c".
struct BindingRedirect {
    Version old_low;
    Version old_high;
    Version new_version;
};

struct Dependency {
    Identity identity;
    bool optional = false;                    ///< dependency optional="yes"
    std::vector<BindingRedirect> redirects;   ///< publisher policy / app config only
    bool publisher_policy_disabled = false;   ///< app config: publisherPolicy apply="no"
};

struct Manifest {
    std::optional<Identity> identity;   ///< assembly/assemblyIdentity
    std::vector<std::string> files;     ///< assembly/file @name, in order
    std::vector<Dependency> dependencies;
    bool no_inheritable = false;
    /// Declares activation-context data other than DLL redirection
    /// (file/comClass, typelib, comInterfaceProxyStub, windowClass;
    /// comInterfaceExternalProxyStub, clrClass, clrSurrogate), which Vitreo's
    /// contexts do not carry yet (.docs/contracts/sxs.md).
    std::vector<std::string> unbuilt_sections;
};

/// Application configuration file (<exe>.config). Only the windows /
/// assemblyBinding part concerns side-by-side.
struct AppConfig {
    std::vector<Dependency> dependencies;     ///< redirects per dependent assembly
    bool publisher_policy_disabled = false;   ///< assemblyBinding-level apply="no"
};

/// Parse a manifest (UTF-8 with or without BOM, or UTF-16 with BOM).
/// nullopt: not well-formed XML or not an asm.v1 <assembly> document
/// (ERROR_SXS_MANIFEST_PARSE_ERROR / _FORMAT_ERROR to the caller).
[[nodiscard]] std::optional<Manifest> parse_manifest(std::span<const u8> bytes);

/// Parse an application configuration file. nullopt: not well-formed.
[[nodiscard]] std::optional<AppConfig> parse_app_config(std::span<const u8> bytes);

}  // namespace vitreo::winapi::sxs
