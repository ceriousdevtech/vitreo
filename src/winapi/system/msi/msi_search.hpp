#pragma once
/// @file msi_search.hpp
/// @brief Windows Installer search area: AppSearch / CCPSearch / RMCCPSearch
/// (every locator table and type), the registry, .ini, environment, ODBC and
/// service standard actions, and AllocateRegistrySpace.
///
/// Searches run immediately and only set properties. Every other action here
/// changes the system, so it runs inside the execution script: the action
/// formats its rows when it is sequenced (as Windows resolves properties
/// when it generates the script) and schedules one ScriptOp per row whose
/// rollback restores what the op found.
///
/// Contract sources: Microsoft Learn (AppSearch, CCPSearch, RMCCPSearch,
/// WriteRegistryValues, RemoveRegistryValues, WriteIniValues,
/// RemoveIniValues, WriteEnvironmentStrings, RemoveEnvironmentStrings,
/// InstallODBC, RemoveODBC, SetODBCFolders, InstallServices,
/// StartServices, StopServices, DeleteServices, MsiConfigureServices and
/// AllocateRegistrySpace actions; the AppSearch, Signature, CompLocator,
/// RegLocator, IniLocator, DrLocator, CCPSearch, Registry, RemoveRegistry,
/// IniFile, RemoveIniFile, Environment, ServiceInstall, ServiceControl,
/// MsiServiceConfig, MsiServiceConfigFailureActions and ODBC* tables),
/// MsiDefs.h, and the winoracle probes tools/oracle_probe/msi_search_probe.c
/// (msi_search32/64.expected) and msi_search_install_probe.c
/// (msi_search_install_*.expected) for what Learn leaves open or states
/// differently. Contract notes and gaps: .docs/contracts/msi/search.md.

#include <vitreo/common.hpp>
#include "capsule/registry/hive.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::capsule { class Registry; class Vfs; }
namespace vitreo::winapi { class ScmManager; }

namespace vitreo::winapi::msi {

class Session;
class ActionContext;
struct Services;
struct ScriptOp;

// ---- locator constants (MsiDefs.h) ------------------------------------------------

enum : u32 {
    kLocatorTypeDirectory = 0x000,  ///< msidbLocatorTypeDirectory
    kLocatorTypeFileName = 0x001,   ///< msidbLocatorTypeFileName
    kLocatorTypeRawValue = 0x002,   ///< msidbLocatorTypeRawValue
    kLocatorType64bit = 0x010,      ///< msidbLocatorType64bit
};

// ---- registry value encodings -----------------------------------------------------

/// What AppSearch sets a property to for a RegLocator raw-value search
/// (msidbLocatorTypeRawValue), measured on Windows 11 (msi_search_probe):
///   REG_SZ          the text; a leading '#' is escaped as "##"
///   REG_EXPAND_SZ   the text with %variables% expanded, no prefix
///   REG_DWORD       "#" + the value as a signed 32-bit decimal ("#-2")
///   REG_BINARY      "#x" + two uppercase hex digits per byte
///   REG_MULTI_SZ    "\0" before every string, then a final "\0"
///   other types     the data bytes read as UTF-16 code units
/// nullopt when the value yields no property (an empty REG_SZ or
/// REG_BINARY, the property stays unset). @p expand resolves one %NAME%.
[[nodiscard]] std::optional<std::string> format_raw_registry_value(
    const capsule::RegValue& value,
    const std::function<std::optional<std::string>(const std::string&)>& expand);

/// A Registry-table Value (already Formatted, so "[~]" is a NUL) as the
/// registry value WriteRegistryValues stores (Learn "Registry Table",
/// msi_search_install_probe). Multi-string values carry how they combine
/// with an existing value.
struct EncodedRegistryValue {
    enum class MultiMode : u8 { Replace, Append, Prepend };
    capsule::RegValue value;
    MultiMode multi{MultiMode::Replace};
};
/// nullopt when the value cannot be stored (Windows fails the install with
/// error 1406 for these: a lone "#", a "#" number that is not a decimal
/// DWORD, a "#x" value that is not hex).
[[nodiscard]] std::optional<EncodedRegistryValue> encode_registry_value(const std::string& formatted);

/// Combine a REG_MULTI_SZ write with the value already stored (append and
/// prepend remove an existing occurrence of each written string first).
[[nodiscard]] std::vector<std::wstring> combine_multi_sz(const std::vector<std::wstring>& existing,
                                                         const std::vector<std::wstring>& written,
                                                         EncodedRegistryValue::MultiMode mode);

// ---- searches ---------------------------------------------------------------------

/// Resolve one signature the way AppSearch does: CompLocator, RegLocator,
/// IniLocator, then DrLocator; the first location found wins. A directory
/// result ends with '\'; a file result is the full path of the file, spelled
/// with the Signature table's FileName. nullopt when not found.
[[nodiscard]] std::optional<std::string> resolve_signature(Session& session, Services& services,
                                                           const std::string& signature);

/// Signature-table filters of one row against a file's facts (exposed for
/// the tests; Learn "Signature Table" + probe s_* lines).
struct SignatureFilter {
    std::string file_name;               ///< long name (short|long split off)
    std::string short_name;              ///< empty when FileName had no '|'
    std::optional<u64> min_version, max_version;
    std::optional<u64> min_size, max_size;
    std::optional<u32> min_date, max_date;  ///< packed DOS date<<16 | time
    std::vector<u16> languages;          ///< empty = Languages null
};
struct FileFacts {
    u64 size{0};
    u32 local_dos_time{0};               ///< last write, local time, packed
    std::optional<u64> version;          ///< unversioned files compare as 0
    std::vector<u16> languages;
};
[[nodiscard]] bool signature_matches(const SignatureFilter& filter, const FileFacts& file);

// ---- .ini files -------------------------------------------------------------------

/// A .ini file as text lines, edited the way the profile APIs do (Learn
/// WritePrivateProfileString; IniFile/RemoveIniFile tables).
class IniDocument {
public:
    static IniDocument parse(const std::string& text);
    [[nodiscard]] std::string serialize() const;
    /// GetPrivateProfileString of one key: trimmed, one pair of quotes
    /// stripped. nullopt when the section or key is absent.
    [[nodiscard]] std::optional<std::string> get(const std::string& section, const std::string& key) const;
    /// Create or replace the key (the section is appended when absent).
    void set(const std::string& section, const std::string& key, const std::string& value);
    /// Delete the key; a section left without keys is deleted too.
    bool remove(const std::string& section, const std::string& key);

private:
    struct Line {
        std::string text;
    };
    [[nodiscard]] std::optional<usize> find_section(const std::string& section) const;
    [[nodiscard]] std::optional<usize> find_key(usize section_line, const std::string& key) const;
    std::vector<Line> m_lines;
};

// ---- services ---------------------------------------------------------------------

/// The process's Service Control Manager for StartServices/StopServices (the
/// runtime binds it). Without one the actions use a manager over the
/// session's registry, which can create, configure and delete services but
/// fails a start or stop with the SCM's own error.
void set_service_control_manager(ScmManager* scm);
/// The process's Service Control Manager (null when none was set).
[[nodiscard]] ScmManager* service_control_manager();

// ---- the execution script ---------------------------------------------------------

/// Run one of this area's actions with its ScriptOps collected into @p ops
/// instead of the engine's script, and without the InstallInitialize ..
/// InstallFinalize check: the ops it would schedule, in order, for a caller
/// that runs them itself (the tests drive each op's run and rollback).
u32 collect_script_ops(ActionContext& ctx, const std::function<u32(ActionContext&)>& action,
                       std::vector<ScriptOp>& ops);

// ---- actions (registered by register_search_actions) ------------------------------
u32 action_app_search(ActionContext& ctx);
u32 action_ccp_search(ActionContext& ctx);
u32 action_rmccp_search(ActionContext& ctx);
u32 action_write_registry_values(ActionContext& ctx);
u32 action_remove_registry_values(ActionContext& ctx);
u32 action_write_ini_values(ActionContext& ctx);
u32 action_remove_ini_values(ActionContext& ctx);
u32 action_write_environment_strings(ActionContext& ctx);
u32 action_remove_environment_strings(ActionContext& ctx);
u32 action_install_odbc(ActionContext& ctx);
u32 action_remove_odbc(ActionContext& ctx);
u32 action_set_odbc_folders(ActionContext& ctx);
u32 action_install_services(ActionContext& ctx);
u32 action_start_services(ActionContext& ctx);
u32 action_stop_services(ActionContext& ctx);
u32 action_delete_services(ActionContext& ctx);
u32 action_msi_configure_services(ActionContext& ctx);
u32 action_allocate_registry_space(ActionContext& ctx);

}  // namespace vitreo::winapi::msi
