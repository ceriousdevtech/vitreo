#pragma once
/// @file msi_assembly.hpp
/// @brief Windows Installer assemblies (Learn: "Assemblies", "MsiAssembly
/// Table", "MsiAssemblyName Table", "Installation of Assemblies to the Global
/// Assembly Cache", "Rollback of Assemblies in the Global Assembly Cache",
/// "MsiPublishAssemblies Action", "MsiUnpublishAssemblies Action",
/// "Installation of Win32 Assemblies", MsiProvideAssembly).
///
/// Measured on Windows 11 (tools/oracle_probe/msi_assembly_*, contract notes
/// in .docs/contracts/msi/registration.md "Assemblies"):
///   * A global .NET assembly's files go to the assembly cache, not to the
///     component's directory: .NET 4 metadata ->
///     %WINDIR%\Microsoft.NET\assembly\GAC_MSIL|GAC_32|GAC_64\<name>\
///     v4.0_<version>_<culture>_<token>\, .NET 2 ->
///     %WINDIR%\assembly\GAC_MSIL|GAC_32|GAC_64\<name>\<version>_<culture>_<token>\
///     (.NET 1.x: ...\assembly\GAC\), culture empty when neutral. InstallFiles
///     stages them under %WINDIR%\assembly\tmp\<8 chars>\; they are committed
///     when the script succeeds (a deferred action does not see them, a commit
///     action does) and removed, on uninstall, at commit too, once no product
///     publishes the assembly any more.
///   * MsiPublishAssemblies writes <Installer root>\Assemblies\Global (.NET) /
///     Win32Assemblies\Global, or ...\<app key file path with '\' -> '|'> for
///     a private assembly: value "<name>,<Attr>=\"<value>\",..." (the
///     MsiAssemblyName rows in table order, names as authored) = MULTI_SZ of
///     descriptors, the newest product first.
///   * ProcessComponents registers '<' (.NET) or '>' (Win32), the key file's
///     name when it is not the manifest, '\' and that identity as the key path
///     of a global assembly component.
///
/// Not available: global Win32 (WinSxS) assemblies. Windows 11 stores them
/// under WinSxS\Fusion\<arch>_<name>_<token>_none_<hash>\..., the hash being
/// an undocumented identity hash, and no Vitreo loader consumes WinSxS; the
/// install fails with 1603 + UNIMPLEMENTED where one would be installed.

#include "winapi/system/msi/msi_core.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vitreo::capsule {
class Vfs;
class Registry;
}  // namespace vitreo::capsule

namespace vitreo::winapi::msi {

class Session;

/// One MsiAssembly row with its MsiAssemblyName rows.
struct PackageAssembly {
    std::string component;    ///< Component_
    std::string feature;      ///< Feature_
    std::string manifest;     ///< File_Manifest (File key)
    std::string application;  ///< File_Application (File key); empty = global
    bool win32{false};        ///< Attributes 1 (NULL / 0 = .NET)
    std::string name;         ///< the "name" row's value
    /// The other MsiAssemblyName rows in table order, names as authored.
    std::vector<std::pair<std::string, std::string>> attributes;

    [[nodiscard]] bool global() const { return application.empty(); }
    /// "<name>,<Name>=\"<value>\",..." (the registry value name, the
    /// AssemblyPublish op's AssemblyName and the key path's identity).
    [[nodiscard]] std::string identity() const;
};

[[nodiscard]] std::vector<PackageAssembly> package_assemblies(Session& session);
[[nodiscard]] std::optional<PackageAssembly> package_assembly(Session& session, const std::string& component);

/// The key path ProcessComponents registers for a global assembly component
/// whose KeyPath is the File key @p key_file.
[[nodiscard]] std::string assembly_key_path(Session& session, const PackageAssembly& assembly,
                                            const std::string& key_file);
/// '<' / '>' key paths (registered for global assemblies).
[[nodiscard]] bool is_assembly_key_path(std::string_view key_path);

// ---- names --------------------------------------------------------------------------

/// A parsed assembly name: the simple name and its attributes (names
/// lower-cased for .NET, as written for Win32).
struct AssemblyName {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    [[nodiscard]] const std::string* find(std::string_view attribute) const;
};
/// The .NET display-name form MsiProvideAssembly accepts (measured): commas
/// between `Name=value` pairs, optional double quotes and blanks, attribute
/// names case-insensitive; nullopt (ERROR_BAD_CONFIGURATION) for an empty
/// name, an empty item (a trailing ','), a pair without '=', a duplicate
/// attribute or an empty Version.
[[nodiscard]] std::optional<AssemblyName> parse_net_assembly_name(std::string_view text);
/// The Win32 identity form (measured): `name,attr="value",...`, attribute
/// names case-sensitive from type, version, processorArchitecture,
/// publicKeyToken, language, versionScope; values double-quoted; a trailing
/// ',' allowed.
[[nodiscard]] std::optional<AssemblyName> parse_win32_assembly_name(std::string_view text);
/// .NET partial match: every Version / Culture / PublicKeyToken /
/// ProcessorArchitecture the request names must be present and equal
/// (case-insensitive; Culture "" is neutral; a partial Version compares the
/// parts it gives); other attributes are ignored.
[[nodiscard]] bool net_name_matches(const AssemblyName& request, const AssemblyName& registered);
/// Win32: the same attribute set with equal values (case-insensitive).
[[nodiscard]] bool win32_name_matches(const AssemblyName& request, const AssemblyName& registered);

// ---- the assembly cache --------------------------------------------------------------

/// What fusion places a .NET assembly by: the metadata version string
/// (ECMA-335 II.24.2.1) and the processor architecture of the image.
struct ClrImage {
    std::string runtime;       ///< e.g. "v4.0.30319"
    std::string architecture;  ///< "MSIL", "x86", "AMD64" or "IA64"
};
/// nullopt when @p image is not a CLR image this cache layout covers.
[[nodiscard]] std::optional<ClrImage> read_clr_image(std::span<const u8> image);
/// The cache directory (trailing '\') of the assembly @p identity names.
[[nodiscard]] std::string gac_directory(const std::string& windows_dir, const ClrImage& image,
                                        const AssemblyName& identity);
/// The installed file @p file_name of a global .NET assembly, spelled as
/// MsiProvideAssembly / MsiGetComponentPath report it
/// ("<windows>\Microsoft.Net\assembly\..." or "<windows>\assembly\...").
[[nodiscard]] std::optional<std::string> find_global_assembly_file(capsule::Vfs& vfs, const std::string& windows_dir,
                                                                   const AssemblyName& identity,
                                                                   const std::string& file_name);
/// The file a '<' / '>' key path stands for; nullopt when the assembly is
/// not in the cache (and for '>' WinSxS key paths, logged as UNIMPLEMENTED).
[[nodiscard]] std::optional<std::string> resolve_assembly_key_path(capsule::Vfs& vfs, const std::string& windows_dir,
                                                                   std::string_view key_path, const char* api);

// ---- InstallFiles / RemoveFiles ----------------------------------------------------------

/// Where InstallFiles puts the files of @p component.
struct AssemblyInstall {
    enum class Kind : u8 { NotGlobal, Stage, AlreadyCached, Unsupported };
    Kind kind{Kind::NotGlobal};
    std::string staging;  ///< Stage: %WINDIR%\assembly\tmp\<8 chars>\ (trailing '\')
};
/// Decide once per component per InstallFiles run.
[[nodiscard]] AssemblyInstall plan_assembly_install(Session& session, capsule::Vfs* vfs, const std::string& component);
/// Schedule, after InstallFiles' copy op, the deferred check of the staged
/// manifest (rolls the install back when it is not an assembly the cache
/// takes) and the commit op that moves the staged files into the cache.
void schedule_assembly_commit(ActionContext& ctx, const std::string& component, const std::string& staging);
/// RemoveFiles: true when @p component is a global assembly (its files are
/// not in the component's directory); schedules the commit-time removal from
/// the cache for when no product publishes it any more.
[[nodiscard]] bool schedule_assembly_removal(ActionContext& ctx, const std::string& component);

/// MsiPublishAssemblies / MsiUnpublishAssemblies.
void register_assembly_actions(ActionRegistry& r);

}  // namespace vitreo::winapi::msi
