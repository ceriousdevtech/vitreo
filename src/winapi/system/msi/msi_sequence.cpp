// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_sequence.cpp
/// @brief Windows Installer engine area: the Engine (sequencer, execution
/// script, action dispatch, messages, logging, built-in properties), the
/// engine's standard actions, and the install-session functions custom
/// actions call (MsiGetProperty, MsiDoAction, ...).
///
/// Contract: Microsoft Learn (sequence tables, "Using a Sequence Table",
/// "Custom Action Return Values", "Logging of Action Return Values", the
/// INSTALL/ExecuteAction/InstallInitialize/InstallFinalize/InstallExecute
/// action pages, "Obtaining Context Information for Deferred Execution Custom
/// Actions", "Restricted Public Properties", ALLUSERS, the session function
/// pages), MsiQuery.h/Msi.h, and tools/oracle_probe/msi_engine_session_probe.c
/// (msi_engine32/64.expected). See .docs/contracts/msi/engine.md.

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/pe/os_version.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vitreo::winapi::msi {

namespace {

constexpr u32 kErrSuccess = 0, kErrAccessDenied = 5, kErrInvalidHandle = 6, kErrInvalidParameter = 87,
              kErrMoreData = 234, kErrNoMoreItems = 259, kErrUserExit = 1602, kErrInstallFailure = 1603,
              kErrSuspend = 1604, kErrUnknownProduct = 1605, kErrAlreadyRunning = 1618,
              kErrPackageOpenFailed = 1619, kErrPackageInvalid = 1620, kErrNotCalled = 1626,
              kErrFunctionFailed = 1627, kErrInvalidCommandLine = 1639, kErrRebootRequired = 3010;

// INSTALLUILEVEL (Msi.h)
constexpr u32 kUiDefault = 1, kUiNone = 2, kUiBasic = 3, kUiReduced = 4, kUiFull = 5;

/// INSTALLLOGMODE_x = 1 << (INSTALLMESSAGE_x >> 24) (Msi.h).
constexpr u32 log_bit(u32 message) { return 1u << (message >> 24); }
constexpr u32 kLogPropertyDump = log_bit(kMsgProgress);
constexpr u32 kLogVerbose = log_bit(kMsgInitialize);
constexpr u32 kLogExtraDebug = log_bit(kMsgTerminate);

std::u16string to_u16(const std::string& s) {
    const std::vector<u16> v = utf8_to_utf16(s);
    return std::u16string(v.begin(), v.end());
}

std::string from_u16(const std::u16string& s) {
    return utf16_to_utf8(reinterpret_cast<const u16*>(s.data()), static_cast<int>(s.size()));
}

std::string acp_to_utf8(const std::string& acp) {
    if (acp.empty()) return {};
    const int n = StringConverter::multi_byte_to_wide_char(0, 0, acp.data(), static_cast<int>(acp.size()), nullptr, 0);
    std::vector<u16> w(static_cast<usize>(n > 0 ? n : 0));
    if (n > 0) StringConverter::multi_byte_to_wide_char(0, 0, acp.data(), static_cast<int>(acp.size()), w.data(), n);
    return utf16_to_utf8(w.data(), static_cast<int>(w.size()));
}

std::string two(int v) { char b[8]; std::snprintf(b, sizeof b, "%02d", v); return b; }

std::string clock_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return two(tm.tm_hour) + ":" + two(tm.tm_min) + ":" + two(tm.tm_sec);
}

std::string date_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return two(tm.tm_mon + 1) + "/" + two(tm.tm_mday) + "/" + std::to_string(tm.tm_year + 1900);
}

std::string with_slash(std::string s) {
    if (!s.empty() && s.back() != '\\') s.push_back('\\');
    return s;
}

/// The value a sequenced action's return code is logged as (Learn "Logging
/// of Action Return Values").
int log_return_value(u32 code) {
    switch (code) {
        case kErrNotCalled: return 0;
        case kErrSuccess: return 1;
        case kErrUserExit: return 2;
        case kErrInstallFailure: return 3;
        case kErrSuspend: return 4;
        case kErrNoMoreItems: return 5;
        default: return 3;
    }
}

// ---- Summary information (read side: the PIDs the engine needs) ----

struct SummaryInfo {
    std::string template_text;   // PID_TEMPLATE (7)
    std::string revision;        // PID_REVNUMBER (9): package code
    std::optional<i32> page_count;  // PID_PAGECOUNT (14): schema
    std::optional<i32> word_count;  // PID_WORDCOUNT (15): source type
};

/// MS-OLEPS property set, the "\005SummaryInformation" stream (Learn
/// "Summary Information Stream Property Set").
SummaryInfo read_summary(const Package& package) {
    SummaryInfo out;
    const std::vector<u8>* s = package.stream(std::string("\x05") + "SummaryInformation");
    if (s == nullptr) s = package.stream("SummaryInformation");
    if (s == nullptr || s->size() < 48) return out;
    auto u32_at = [&](usize o) -> u32 {
        if (o + 4 > s->size()) return 0;
        return static_cast<u32>((*s)[o]) | (static_cast<u32>((*s)[o + 1]) << 8) |
               (static_cast<u32>((*s)[o + 2]) << 16) | (static_cast<u32>((*s)[o + 3]) << 24);
    };
    const u32 section = u32_at(28 + 16);
    if (section + 8 > s->size()) return out;
    const u32 count = u32_at(section + 4);
    for (u32 k = 0; k < count && section + 8 + k * 8 + 8 <= s->size(); ++k) {
        const u32 pid = u32_at(section + 8 + k * 8);
        const usize at = section + u32_at(section + 12 + k * 8);
        const u32 type = u32_at(at);
        if (type == 30) {  // VT_LPSTR
            const u32 len = u32_at(at + 4);
            if (at + 8 + len > s->size()) continue;
            std::string text(reinterpret_cast<const char*>(s->data() + at + 8), len);
            while (!text.empty() && text.back() == '\0') text.pop_back();
            text = acp_to_utf8(text);
            if (pid == 7) out.template_text = text;
            else if (pid == 9) out.revision = text;
        } else if (type == 3 || type == 2) {  // VT_I4 / VT_I2
            i32 v = type == 3 ? static_cast<i32>(u32_at(at + 4)) : static_cast<i16>(u32_at(at + 4) & 0xFFFF);
            if (pid == 14) out.page_count = v;
            else if (pid == 15) out.word_count = v;
        }
    }
    return out;
}

std::vector<ExecuteHandoffHook>& handoff_hooks() {
    static std::vector<ExecuteHandoffHook> hooks;
    return hooks;
}

/// Open MsiOpenPackage sessions in this process (probe: a second
/// MsiOpenPackage while one is open is 1618, ERROR_INSTALL_ALREADY_RUNNING).
std::atomic<int> g_open_package_sessions{0};

const char* const kStandardActions[] = {
    // Learn "Standard Actions Reference".
    "ADMIN", "ADVERTISE", "AllocateRegistrySpace", "AppSearch", "BindImage", "CCPSearch",
    "CostFinalize", "CostInitialize", "CreateFolders", "CreateShortcuts", "DeleteServices",
    "DisableRollback", "DuplicateFiles", "ExecuteAction", "FileCost", "FindRelatedProducts",
    "ForceReboot", "INSTALL", "InstallAdminPackage", "InstallExecute", "InstallExecuteAgain",
    "InstallFiles", "InstallFinalize", "InstallInitialize", "InstallSFPCatalogFile",
    "InstallValidate", "IsolateComponents", "LaunchConditions", "MigrateFeatureStates",
    "MoveFiles", "MsiConfigureServices", "MsiPublishAssemblies", "MsiUnpublishAssemblies",
    "InstallODBC", "InstallServices", "PatchFiles", "ProcessComponents", "PublishComponents",
    "PublishFeatures", "PublishProduct", "RegisterClassInfo", "RegisterComPlus",
    "RegisterExtensionInfo", "RegisterFonts", "RegisterMIMEInfo", "RegisterProduct",
    "RegisterProgIdInfo", "RegisterTypeLibraries", "RegisterUser", "RemoveDuplicateFiles",
    "RemoveEnvironmentStrings", "RemoveExistingProducts", "RemoveFiles", "RemoveFolders",
    "RemoveIniValues", "RemoveODBC", "RemoveRegistryValues", "RemoveShortcuts", "ResolveSource",
    "RMCCPSearch", "ScheduleReboot", "SelfRegModules", "SelfUnregModules", "SEQUENCE",
    "SetODBCFolders", "StartServices", "StopServices", "UnpublishComponents", "UnpublishFeatures",
    "UnregisterClassInfo", "UnregisterComPlus", "UnregisterExtensionInfo", "UnregisterFonts",
    "UnregisterMIMEInfo", "UnregisterProgIdInfo", "UnregisterTypeLibraries", "ValidateProductID",
    "WriteEnvironmentStrings", "WriteIniValues", "WriteRegistryValues",
};

/// MsiOpenPackageEx(IGNOREMACHINESTATE): the standard actions a restricted
/// handle may run; all others are ERROR_FUNCTION_NOT_CALLED (Learn).
bool restricted_allowed(const std::string& a) {
    static const std::set<std::string> ok = {
        "ADMIN", "ADVERTISE", "INSTALL", "SEQUENCE", "AppSearch", "CCPSearch", "CostFinalize",
        "CostInitialize", "FileCost", "FindRelatedProducts", "IsolateComponents", "LaunchConditions",
        "MigrateFeatureStates", "ResolveSource", "RMCCPSearch", "ValidateProductID"};
    return ok.contains(a);
}

const std::set<std::string>& restricted_public_properties() {
    // Learn "Restricted Public Properties".
    static const std::set<std::string> list = {
        "ACTION", "AFTERREBOOT", "ALLUSERS", "EXECUTEACTION", "EXECUTEMODE", "FILEADDDEFAULT",
        "FILEADDLOCAL", "FILEADDSOURCE", "INSTALLLEVEL", "LIMITUI", "LOGACTION", "NOCOMPANYNAME",
        "NOUSERNAME", "MSIENFORCEUPGRADECOMPONENTRULES", "MSIINSTANCEGUID", "MSINEWINSTANCE",
        "MSIPATCHREMOVE", "PATCH", "PRIMARYFOLDER", "PROMPTROLLBACKCOST", "REBOOT", "REINSTALL",
        "REINSTALLMODE", "RESUME", "SEQUENCE", "SHORTFILENAMES", "TRANSFORMS", "TRANSFORMSATSOURCE"};
    return list;
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// Preselected: set when the command line carries a feature request
/// property (features agent's measurement on winoracle, msiexec and
/// MsiOpenPackage; MsiSetProperty of the same properties does not set it).
void apply_command_line(Session& s, const std::map<std::string, std::string>& command_line) {
    static const char* const requests[] = {
        "ADDLOCAL", "ADDSOURCE", "ADDDEFAULT", "REMOVE", "REINSTALL", "ADVERTISE", "COMPADDLOCAL",
        "COMPADDSOURCE", "COMPADDDEFAULT", "FILEADDLOCAL", "FILEADDSOURCE", "FILEADDDEFAULT"};
    bool preselected = false;
    for (const auto& [name, value] : command_line) {
        s.set_property(name, value);
        for (const char* r : requests) if (name == r && !value.empty()) preselected = true;
    }
    if (preselected) s.set_property("Preselected", "1");
}

}  // namespace

// ---------------------------------------------------------------------------
// Registries
// ---------------------------------------------------------------------------

void add_execute_handoff_hook(ExecuteHandoffHook hook) { handoff_hooks().push_back(std::move(hook)); }

bool is_standard_action(const std::string& name) {
    for (const char* a : kStandardActions) if (name == a) return true;
    return false;
}

const ActionRegistry& standard_actions() {
    static const ActionRegistry registry = [] {
        ActionRegistry r;
        register_engine_actions(r);
        register_feature_actions(r);
        register_file_actions(r);
        register_search_actions(r);
        register_registration_actions(r);
        return r;
    }();
    return registry;
}

// ---------------------------------------------------------------------------
// ActionContext (msi_core.hpp)
// ---------------------------------------------------------------------------

bool ActionContext::scripting() const {
    const Engine* e = m_session.engine();
    return e != nullptr && e->scripting();
}

void ActionContext::schedule(ScriptOp op) {
    Engine* e = m_session.engine();
    if (e == nullptr || !e->scripting()) {
        // A caller that did not check scripting() first: refusing is the
        // documented outcome of a system change outside the script.
        log_unimplemented("ActionContext::schedule", "op scheduled outside InstallInitialize..InstallFinalize: " + op.action);
        return;
    }
    e->schedule(std::move(op));
}

i32 ActionContext::message(u32 type, const Record& record) {
    Engine* e = m_session.engine();
    if (e != nullptr) return e->message(type, record);
    if (m_services.ui != nullptr) return m_services.ui->message(m_session, type, record);
    return 0;
}

// ---------------------------------------------------------------------------
// Machine state
// ---------------------------------------------------------------------------

MachineState default_machine_state(capsule::Registry* registry, const std::wstring& user_sid) {
    MachineState m;
    // Windows Installer's own processes declare Windows 8.1 compatibility:
    // probe p0-S VersionNT=603 WindowsBuild=9600 on Windows 11.
    m.os_major = 6;
    m.os_minor = 3;
    m.os_build = 9600;
    (void)registry;
    m.os_64bit = true;
    m.product_type = 1;
    // The capsule identity and layout: kernel32 EnvironmentManager defaults
    // (COMPUTERNAME, USERNAME, USERPROFILE, TEMP, ProgramData) and the
    // shell32 known-folder expansion (C:\Users\Default profile).
    m.computer_name = "VITREO";
    m.logon_user = "Default";
    m.user_sid = std::string(user_sid.begin(), user_sid.end());
    m.admin_user = true;   // capsule guests run with administrator rights
    m.elevated = true;
    m.windows_dir = "C:\\Windows";
    m.profile_dir = "C:\\Users\\Default";
    m.program_data = "C:\\ProgramData";
    m.temp_dir = "C:\\Users\\Default\\AppData\\Local\\Temp";
    m.user_lcid = 1033;
    m.system_lcid = 1033;
    m.current_directory = "C:\\Windows\\system32";
    return m;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine(MsiManager& manager, Options options, Services services)
    : m_manager(manager), m_options(std::move(options)), m_services(services) {
    m_machine = manager.machine_state();
}

Engine::~Engine() {
    if (!m_log_buffer.empty()) write_log_text({});
}

std::shared_ptr<Engine> Engine::open(MsiManager& manager, std::shared_ptr<Package> package, Options options,
                                     Services services, u32& error) {
    std::shared_ptr<Engine> engine(new Engine(manager, std::move(options), services));
    engine->m_client = std::make_unique<Session>();
    Session& s = *engine->m_client;
    if (!s.initialize(std::move(package))) {
        error = kErrPackageInvalid;
        return nullptr;
    }
    s.set_engine(engine.get());
    s.set_environment(engine->m_machine.environment);
    engine->m_session = &s;
    // The internal UI cannot show a wizard without a UI host; Windows does
    // the same without an interactive desktop (probe pass 1: /qr over ssh
    // ran no InstallUISequence). The run continues at basic UI.
    if (engine->m_services.ui == nullptr && (engine->m_options.ui_level & 0xF) >= kUiReduced) {
        engine->m_options.ui_level = (engine->m_options.ui_level & ~0xFu) | kUiBasic;
    }
    apply_command_line(s, engine->m_options.command_line);
    engine->set_builtin_properties(s, engine->m_options.server);
    engine->open_log();
    error = kErrSuccess;
    return engine;
}

bool Engine::shows_wizard_ui() const {
    const u32 base = m_options.ui_level & 0xF;
    return (base == kUiFull || base == kUiReduced || base == kUiDefault) && m_services.ui != nullptr;
}

void Engine::set_builtin_properties(Session& s, bool server_side) {
    const MachineState& m = m_machine;
    auto set = [&](const char* name, const std::string& value) { s.set_property(name, value); };
    auto set_if_unset = [&](const char* name, const std::string& value) {
        if (!s.has_property(name)) s.set_property(name, value);
    };

    // Operating system (Learn VersionNT, VersionNT64, WindowsBuild,
    // ServicePackLevel, MsiNTProductType, Intel, Msix64, MsiAMD64).
    const std::string version_nt = std::to_string(m.os_major * 100 + m.os_minor);
    set("VersionNT", version_nt);
    if (m.os_64bit) set("VersionNT64", version_nt);
    set("WindowsBuild", std::to_string(m.os_build));
    set("ServicePackLevel", std::to_string(m.service_pack_major));
    set("ServicePackLevelMinor", std::to_string(m.service_pack_minor));
    set("MsiNTProductType", std::to_string(m.product_type));
    if (m.processor_level != 0) {
        set("Intel", std::to_string(m.processor_level));
        if (m.os_64bit) {
            set("Msix64", std::to_string(m.processor_level));
            set("MsiAMD64", std::to_string(m.processor_level));
        }
    }
    set("VersionMsi", "5.00");

    Package* pkg = s.package();
    if (pkg != nullptr) {
        const SummaryInfo si = read_summary(*pkg);
        if (si.page_count.has_value()) set("VersionDatabase", std::to_string(*si.page_count));
        if (!si.revision.empty()) set("PackageCode", si.revision);
    }

    // Privileges.
    if (m.admin_user) { set("AdminUser", "1"); if (server_side) set("MsiTrueAdminUser", "1"); }
    if (m.elevated) { set("Privileged", "1"); set("MsiRunningElevated", "1"); }

    // Identity.
    set("UserSID", m.user_sid);
    set("LogonUser", m.logon_user);
    set("ComputerName", m.computer_name);
    set_if_unset("USERNAME", m.logon_user);
    set("UserLanguageID", std::to_string(m.user_lcid));
    set("SystemLanguageID", std::to_string(m.system_lcid));
    set("Date", date_text());
    set("Time", clock_text());
    if (m.physical_memory_mb) set("PhysicalMemory", std::to_string(*m.physical_memory_mb));
    if (m.virtual_memory_mb) set("VirtualMemory", std::to_string(*m.virtual_memory_mb));
    if (m.display) {
        set("ScreenX", std::to_string(m.display->screen_x));
        set("ScreenY", std::to_string(m.display->screen_y));
        set("ColorBits", std::to_string(m.display->color_bits));
        set("TextHeight", std::to_string(m.display->text_height));
        set("BorderSide", std::to_string(m.display->border_side));
        set("BorderTop", std::to_string(m.display->border_top));
        set("CaptionHeight", std::to_string(m.display->caption_height));
    }

    // Engine capabilities the probe session reports.
    set("OLEAdvtSupport", "1");
    set("ShellAdvtSupport", "1");
    set("RedirectedDllSupport", "2");
    if (server_side) { set("GPTSupport", "1"); set("TTCSupport", "1"); }

    // Package / session.
    set("OriginalDatabase", m_options.package_path);
    set("DATABASE", m_options.package_path);
    set_if_unset("ACTION", "INSTALL");
    set("UILevel", std::to_string(m_options.ui_level & 0xF));
    if (!m_options.log_file.empty()) set("MsiLogFileLocation", m_options.log_file);
    if (server_side) {
        if (m.process_id != 0) set("CLIENTPROCESSID", std::to_string(m.process_id));
        if (!m.current_directory.empty()) set("CURRENTDIRECTORY", m.current_directory);
    }
    const std::string product = s.property("ProductCode");
    const bool installed = !product.empty() && m_manager.is_product_registered(product);
    set("ProductState", installed ? "5" : "-1");   // INSTALLSTATE_DEFAULT / UNKNOWN
    if (installed) set("Installed", "1");

    // Installation context (Learn ALLUSERS): ALLUSERS=2 resolves to per-user
    // when MSIINSTALLPERUSER=1, else per-machine for an administrator.
    bool per_user_programs = false;
    if (s.property("ALLUSERS") == "2") {
        if (s.property("MSIINSTALLPERUSER") == "1" || !m.admin_user) {
            s.set_property("ALLUSERS", "");
            per_user_programs = s.property("MSIINSTALLPERUSER") == "1";
        } else {
            s.set_property("ALLUSERS", "1");
        }
    }
    const bool per_machine = !s.property("ALLUSERS").empty();

    // Folders: the capsule layout (shell32 known folders, kernel32
    // environment). Trailing backslash, as every folder property has.
    const std::string W = m.windows_dir;
    const std::string drive = W.size() >= 2 ? W.substr(0, 2) : std::string("C:");
    const std::string P = m.profile_dir;
    const std::string D = m.program_data;
    const std::string pub = P.substr(0, P.find_last_of('\\')) + "\\Public";
    const std::string roaming = P + "\\AppData\\Roaming";
    const std::string local = P + "\\AppData\\Local";
    const std::string user_menu = roaming + "\\Microsoft\\Windows\\Start Menu";
    const std::string common_menu = D + "\\Microsoft\\Windows\\Start Menu";
    const std::string menu = per_machine ? common_menu : user_menu;
    const std::string pf = m.os_64bit ? drive + "\\Program Files (x86)" : drive + "\\Program Files";
    if (per_user_programs) {
        set("ProgramFilesFolder", with_slash(local + "\\Programs"));
        set("CommonFilesFolder", with_slash(local + "\\Programs\\Common"));
        if (m.os_64bit) {
            set("ProgramFiles64Folder", with_slash(local + "\\Programs"));
            set("CommonFiles64Folder", with_slash(local + "\\Programs\\Common"));
        }
    } else {
        set("ProgramFilesFolder", with_slash(pf));
        set("CommonFilesFolder", with_slash(pf + "\\Common Files"));
        if (m.os_64bit) {
            set("ProgramFiles64Folder", with_slash(drive + "\\Program Files"));
            set("CommonFiles64Folder", with_slash(drive + "\\Program Files\\Common Files"));
        }
    }
    set("WindowsFolder", with_slash(W));
    set("WindowsVolume", with_slash(drive));
    set("SystemFolder", with_slash(W + (m.os_64bit ? "\\SysWOW64" : "\\System32")));
    if (m.os_64bit) set("System64Folder", with_slash(W + "\\System32"));
    set("FontsFolder", with_slash(W + "\\Fonts"));
    set("TempFolder", with_slash(m.temp_dir));
    set("CommonAppDataFolder", with_slash(D));
    set("AppDataFolder", with_slash(roaming));
    set("LocalAppDataFolder", with_slash(local));
    set("FavoritesFolder", with_slash(P + "\\Favorites"));
    set("NetHoodFolder", with_slash(roaming + "\\Microsoft\\Windows\\Network Shortcuts"));
    set("PersonalFolder", with_slash(P + "\\Documents"));
    set("PrintHoodFolder", with_slash(roaming + "\\Microsoft\\Windows\\Printer Shortcuts"));
    set("RecentFolder", with_slash(roaming + "\\Microsoft\\Windows\\Recent"));
    set("SendToFolder", with_slash(roaming + "\\Microsoft\\Windows\\SendTo"));
    set("MyPicturesFolder", with_slash(P + "\\Pictures"));
    set("DesktopFolder", with_slash(per_machine ? pub + "\\Desktop" : P + "\\Desktop"));
    set("StartMenuFolder", with_slash(menu));
    set("ProgramMenuFolder", with_slash(menu + "\\Programs"));
    set("StartupFolder", with_slash(menu + "\\Programs\\Startup"));
    set("AdminToolsFolder", with_slash(menu + "\\Programs\\Administrative Tools"));
    set("TemplateFolder", with_slash(per_machine ? D + "\\Microsoft\\Windows\\Templates"
                                                 : roaming + "\\Microsoft\\Windows\\Templates"));
    if (s.property("DISABLEROLLBACK") == "1") m_rollback_enabled = false;
}

// ---- logging ----

void Engine::open_log() {
    if (m_options.log_file.empty() || m_options.log_mode == 0) return;
    m_log_open = true;
    if (!m_options.log_append && m_services.vfs != nullptr) {
        auto h = m_services.vfs->open(m_options.log_file, capsule::AccessMode::Write, capsule::ShareMode::Read,
                                      capsule::CreationDisposition::CreateAlways);
        if (h.is_ok()) (void)m_services.vfs->close(h.value());
    }
    write_log_text("=== Vitreo Windows Installer log started: " + date_text() + " " + clock_text() + " ===\n");
}

void Engine::write_log_text(const std::string& text) {
    if (!m_log_open || m_services.vfs == nullptr) return;
    m_log_buffer += text;
    if (m_log_buffer.empty()) return;
    auto h = m_services.vfs->open(m_options.log_file, capsule::AccessMode::Write, capsule::ShareMode::Read,
                                  capsule::CreationDisposition::OpenAlways);
    if (h.is_err()) return;
    (void)m_services.vfs->seek(h.value(), 0, capsule::SeekOrigin::End);
    const auto* bytes = reinterpret_cast<const u8*>(m_log_buffer.data());
    (void)m_services.vfs->write(h.value(), std::span<const u8>(bytes, m_log_buffer.size()));
    (void)m_services.vfs->close(h.value());
    m_log_buffer.clear();
}

void Engine::log(u32 kind, const std::string& text) {
    if (!m_log_open) return;
    if ((m_options.log_mode & kind) == 0) return;
    const char side = (m_session != nullptr && m_session == m_server.get()) || m_options.server ? 's' : 'c';
    write_log_text(std::string("MSI (") + side + ") [" + clock_text() + "]: " + text + "\n");
}

std::string Engine::apply_hidden(const std::string& text) const {
    // MsiHiddenProperties: their values never reach the log (Learn).
    std::string out = text;
    if (m_session == nullptr) return out;
    for (const std::string& name : split_list(m_session->property("MsiHiddenProperties"))) {
        const std::string value = m_session->property(name);
        if (value.empty()) continue;
        usize at = 0;
        while ((at = out.find(value, at)) != std::string::npos) {
            out.replace(at, value.size(), "**********");
            at += 10;
        }
    }
    return out;
}

void Engine::log_property_dump() {
    if (!m_log_open || (m_options.log_mode & kLogPropertyDump) == 0) return;
    auto dump = [&](Session* s, char side) {
        if (s == nullptr) return;
        const auto hidden = split_list(s->property("MsiHiddenProperties"));
        for (const auto& [name, value] : s->properties()) {
            const bool hide = std::find(hidden.begin(), hidden.end(), name) != hidden.end();
            write_log_text(std::string("Property(") + side + "): " + name + " = " + (hide ? "**********" : value) + "\n");
        }
    };
    dump(m_server.get(), 'S');
    dump(m_client.get(), m_options.server ? 'S' : 'C');
}

// ---- messages ----

std::string Engine::error_template(i32 code) const {
    if (m_session != nullptr && m_session->package() != nullptr) {
        if (const Table* t = m_session->package()->table("Error")) {
            const auto key = t->column_index("Error");
            const auto msg = t->column_index("Message");
            if (key && msg) {
                const std::string want = std::to_string(code);
                for (const auto& row : t->rows) {
                    if (*key < row.size() && row[*key] == want && *msg < row.size()) return row[*msg];
                }
            }
        }
    }
    // The package carries no template for this error: Vitreo's own wording
    // naming the error and its arguments.
    return "Windows Installer error [1].{ Arguments: [2]}{, [3]}{, [4]}{, [5]}{, [6]}";
}

i32 Engine::message(u32 type, const Record& in) {
    const u32 kind = type & 0xFF000000u;
    Record record = in;
    const bool error_kind = kind == kMsgFatalExit || kind == kMsgError || kind == kMsgWarning || kind == kMsgUser ||
                            kind == kMsgOutOfDiskSpace;
    // Learn MsiProcessMessage: with field 0 unset, field 1 is the Error-table
    // number whose template formats the message.
    if (error_kind && (record.fields.empty() || record.fields[0].kind == Record::Kind::Null) &&
        record.count() >= 1 && record.fields[1].kind == Record::Kind::Integer) {
        record.fields[0].kind = Record::Kind::String;
        record.fields[0].text = to_u16(error_template(record.fields[1].integer));
    }
    const std::string text = (m_session != nullptr) ? m_session->format_record(record)
                                                    : Session::format_record_without_session(record);
    std::string prefix;
    if (kind == kMsgError || kind == kMsgFatalExit) prefix = "Error: ";
    else if (kind == kMsgWarning) prefix = "Warning: ";
    if (kind != kMsgActionStart && kind != kMsgProgress) log(log_bit(kind), prefix + apply_hidden(text));
    return route_ui(type, record);
}

/// MsiProcessMessage's UI half (msi_ui.hpp route_message): the external
/// record and string handlers a caller installed with MsiSetExternalUI(Record)
/// first, then the internal UI. The external handlers receive messages even
/// when no internal UI is shown (Learn "Monitoring an Installation Using
/// MsiSetExternalUI").
i32 Engine::route_ui(u32 type, const Record& record) {
    const RecordFormatter format = [this](const Record& r) {
        return m_session != nullptr ? m_session->format_record(r) : Session::format_record_without_session(r);
    };
    return route_message(m_manager, m_session != nullptr ? m_services.ui : nullptr, m_session, type, &record, format);
}

void Engine::action_start(const std::string& name) {
    Record r(3);
    r.fields[1].kind = Record::Kind::String;
    r.fields[1].text = to_u16(name);
    if (m_session != nullptr && m_session->package() != nullptr) {
        if (const Table* t = m_session->package()->table("ActionText")) {
            const auto a = t->column_index("Action");
            const auto d = t->column_index("Description");
            const auto tm = t->column_index("Template");
            for (const auto& row : t->rows) {
                if (!a || *a >= row.size() || row[*a] != name) continue;
                if (d && *d < row.size() && !row[*d].empty()) {
                    r.fields[2].kind = Record::Kind::String;
                    r.fields[2].text = to_u16(m_session->format(row[*d]));
                }
                if (tm && *tm < row.size() && !row[*tm].empty()) {
                    r.fields[3].kind = Record::Kind::String;
                    r.fields[3].text = to_u16(row[*tm]);
                }
            }
        }
    }
    const std::string description = r.fields[2].kind == Record::Kind::String ? from_u16(r.fields[2].text) : std::string{};
    log(log_bit(kMsgActionStart), "Action start " + clock_text() + ": " + name + "." + (description.empty() ? "" : " " + description));
    (void)route_ui(kMsgActionStart, r);
}

// ---- modes ----

bool Engine::get_mode(u32 mode) const {
    if (m_script_kind != 0) {
        // Deferred, rollback and commit actions see only these three (Learn).
        return mode == m_script_kind;
    }
    switch (mode) {
        case kRunAdmin: return m_session != nullptr && m_session->property("ACTION") == "ADMIN";
        case kRunAdvertise: return m_session != nullptr && m_session->property("ACTION") == "ADVERTISE";
        case kRunMaintenance: return m_session != nullptr && m_session->has_property("Installed");
        case kRunRollbackEnabled: return m_rollback_enabled;
        case kRunLogEnabled: return m_log_open;
        case kRunOperations: return m_scripting;
        case kRunRebootAtEnd: return m_reboot_at_end;
        case kRunRebootNow: return m_reboot_now;
        case kRunCabinet: return true;           // probe: true in an MsiOpenPackage session
        case kRunTargetShortNames: return m_session != nullptr && !m_session->property("SHORTFILENAMES").empty();
        case kRunZawEnabled: return true;        // probe: true (demand installation supported)
        default: return false;
    }
}

u32 Engine::set_mode(u32 mode, bool on) {
    if (mode == kRunRebootAtEnd) { m_reboot_at_end = on; return kErrSuccess; }
    if (mode == kRunRebootNow) { m_reboot_now = on; return kErrSuccess; }
    return kErrAccessDenied;  // probe: every other mode is 5
}

// ---- execution script ----

bool Engine::begin_script() {
    if (!m_options.allow_system_changes || m_scripting) return false;
    m_scripting = true;
    m_script_depth = m_sequence_depth;
    return true;
}

void Engine::schedule(ScriptOp op) {
    log(kLogVerbose, "Scheduling script operation: " + op.action);
    m_pending.push_back(std::move(op));
}

void Engine::disable_rollback() {
    m_rollback_enabled = false;
    m_rollback.clear();
    if (m_session != nullptr) m_session->set_property("RollbackDisabled", "1");
}

u32 Engine::execute_script(bool finalize) {
    std::vector<ScriptOp> ops;
    ops.swap(m_pending);
    for (ScriptOp& op : ops) {
        if (op.commit_only) { m_commit.push_back(std::move(op)); continue; }
        if (op.rollback_only) {
            if (m_rollback_enabled) m_rollback.push_back(std::move(op));
            continue;
        }
        log(kLogVerbose, "Executing op: " + op.action);
        m_script_kind = kRunScheduled;
        const u32 rc = op.run ? op.run() : kErrSuccess;
        m_script_kind = 0;
        m_custom_action_data.clear();
        if (op.rollback && m_rollback_enabled) {
            ScriptOp undo;
            undo.action = op.action;
            undo.rollback = op.rollback;
            m_rollback.push_back(std::move(undo));
        }
        if (rc != kErrSuccess && rc != kErrNoMoreItems) {
            log(log_bit(kMsgInfo), "Script operation " + op.action + " failed (" + std::to_string(rc) + "); rolling back");
            rollback();
            m_scripting = false;
            return rc == kErrUserExit ? kErrUserExit : kErrInstallFailure;
        }
    }
    if (finalize) {
        for (ScriptOp& op : m_commit) {
            m_script_kind = kRunCommit;
            if (op.run) (void)op.run();
            m_script_kind = 0;
            m_custom_action_data.clear();
        }
        m_commit.clear();
        m_rollback.clear();
        m_scripting = false;
    }
    return kErrSuccess;
}

void Engine::rollback() {
    for (auto it = m_rollback.rbegin(); it != m_rollback.rend(); ++it) {
        m_script_kind = kRunRollback;
        if (it->rollback_only && it->run) (void)it->run();
        else if (it->rollback) it->rollback();
        m_script_kind = 0;
        m_custom_action_data.clear();
    }
    m_rollback.clear();
    m_commit.clear();
    m_pending.clear();
}

u32 Engine::open_install_handle() {
    auto h = std::make_shared<InstallHandle>();
    h->engine = shared_from_this();
    if (m_script_kind != 0) {
        h->deferred = true;
        h->script_kind = m_script_kind;
        h->custom_action_data = m_custom_action_data;
    }
    return m_manager.handles().add(std::move(h));
}

// ---- dispatch ----

u32 Engine::do_action(const std::string& name) {
    if (name.empty()) return kErrInstallFailure;  // probe: MsiDoAction("") -> 1603
    return dispatch(name, false, std::string{});
}

u32 Engine::dispatch(const std::string& name, bool in_sequence, const std::string& table) {
    (void)table;
    if (m_options.restricted && !restricted_allowed(name)) {
        // A restricted handle runs dialogs, the listed standard actions and
        // property-setting custom actions only (Learn MsiOpenPackageEx); the
        // custom-action area enforces the CA types through Engine::restricted().
        if (is_standard_action(name)) return kErrNotCalled;
    }
    if (name == "INSTALL" || name == "ADMIN" || name == "ADVERTISE") return run_top_level(name);
    if (name == "SEQUENCE") {
        const std::string t = m_session->property("SEQUENCE");
        return t.empty() ? kErrSuccess : run_sequence(t);
    }
    if (name == "ExecuteAction") return execute_action();

    ActionContext ctx(*m_session, m_services);
    if (const ActionFn* fn = standard_actions().find(name)) {
        log(log_bit(kMsgInfo), "Doing action: " + name);
        action_start(name);
        const u32 rc = (*fn)(ctx);
        log(log_bit(kMsgInfo), "Action ended " + clock_text() + ": " + name + ". Return value " +
                                   std::to_string(log_return_value(rc)) + ".");
        return rc;
    }
    if (is_standard_action(name)) {
        log_unimplemented("action", name);
        log(log_bit(kMsgInfo), "Action " + name + " has no implementation");
        return kErrInstallFailure;
    }
    if (Package* pkg = m_session->package()) {
        if (const Table* ca = pkg->table("CustomAction")) {
            const auto a = ca->column_index("Action");
            const auto ty = ca->column_index("Type");
            const auto src = ca->column_index("Source");
            const auto tgt = ca->column_index("Target");
            if (a && ty) {
                for (const auto& row : ca->rows) {
                    if (*a >= row.size() || row[*a] != name) continue;
                    const u32 type = static_cast<u32>(std::strtoul(row[*ty].c_str(), nullptr, 10));
                    const std::string source = (src && *src < row.size()) ? row[*src] : std::string{};
                    const std::string target = (tgt && *tgt < row.size()) ? row[*tgt] : std::string{};
                    log(log_bit(kMsgInfo), "Doing action: " + name);
                    action_start(name);
                    const u32 rc = run_custom_action(ctx, name, type, source, target);
                    log(log_bit(kMsgInfo), "Action ended " + clock_text() + ": " + name + ". Return value " +
                                               std::to_string(log_return_value(rc)) + ".");
                    return rc;
                }
            }
        }
        if (const Table* dialogs = pkg->table("Dialog")) {
            const auto d = dialogs->column_index("Dialog");
            if (d) {
                for (const auto& row : dialogs->rows) {
                    if (*d >= row.size() || row[*d] != name) continue;
                    if (!shows_wizard_ui()) return kErrNotCalled;
                    log(log_bit(kMsgInfo), "Dialog: " + name);
                    return m_services.ui->run_dialog(*m_session, name);
                }
            }
        }
    }
    (void)in_sequence;
    log(log_bit(kMsgInfo), "Action not found: " + name);
    return kErrNotCalled;  // probe: MsiDoAction("NoSuchAction") -> 1626
}

u32 Engine::run_sequence(const std::string& table) {
    Package* pkg = m_session->package();
    const Table* t = pkg != nullptr ? pkg->table(table) : nullptr;
    if (t == nullptr) return kErrSuccess;  // probe: MsiSequence of a missing table -> 0
    const auto a = t->column_index("Action");
    const auto c = t->column_index("Condition");
    const auto q = t->column_index("Sequence");
    if (!a || !q) return kErrInstallFailure;

    struct Row { std::string action, condition; long seq; };
    std::vector<Row> rows;
    std::map<long, Row> terminal;
    for (const auto& row : t->rows) {
        if (*a >= row.size() || *q >= row.size() || row[*q].empty()) continue;  // null: never run
        Row r{row[*a], (c && *c < row.size()) ? row[*c] : std::string{}, std::strtol(row[*q].c_str(), nullptr, 10)};
        if (r.seq > 0) rows.push_back(r);
        else if (r.seq >= -4 && r.seq <= -1) terminal.emplace(r.seq, r);  // termination flags
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.seq < y.seq; });

    ++m_sequence_depth;
    const std::string saved = m_current_sequence;
    m_current_sequence = table;
    auto run_terminal = [&](long flag) {
        auto it = terminal.find(flag);
        if (it == terminal.end()) return;
        const auto ok = m_session->evaluate_condition(it->second.condition);
        if (ok.value_or(false)) (void)dispatch(it->second.action, true, table);
    };

    u32 result = kErrSuccess;
    for (const Row& r : rows) {
        const auto ok = m_session->evaluate_condition(r.condition);
        if (!ok.has_value()) {
            // Learn: invalid condition syntax terminates the sequence
            // (iesBadActionData); probe ProbeSeqBad -> 1603.
            log(log_bit(kMsgInfo), "Condition syntax error in " + table + " for " + r.action + ": " + r.condition);
            result = kErrInstallFailure;
            break;
        }
        if (!*ok) {
            log(log_bit(kMsgInfo), "Skipping action: " + r.action + " (condition is false)");
            continue;
        }
        const u32 rc = dispatch(r.action, true, table);
        if (rc == kErrSuccess || rc == kErrNotCalled) continue;
        if (rc == kErrNoMoreItems) { result = kErrSuccess; break; }   // skip the rest, not an error
        if (rc == kErrUserExit || rc == kErrSuspend) { result = rc; break; }
        result = kErrInstallFailure;
        break;
    }
    // The sequence that opened the script closes it: a failure rolls back
    // what already ran; a success with ops still pending runs them, as the
    // end of the INSTALL transaction.
    if (m_scripting && m_sequence_depth == m_script_depth) {
        if (result != kErrSuccess) {
            rollback();
            m_scripting = false;
        } else {
            result = execute_script(true);
        }
    }
    switch (result) {
        case kErrSuccess: run_terminal(-1); break;          // msiDoActionStatusSuccess
        case kErrUserExit: run_terminal(-2); break;
        case kErrInstallFailure: run_terminal(-3); break;
        case kErrSuspend: run_terminal(-4); break;
        default: break;
    }
    m_current_sequence = saved;
    --m_sequence_depth;
    return result;
}

u32 Engine::run_top_level(const std::string& action) {
    m_session->set_property("ACTION", action);
    if (!m_session->has_property("EXECUTEACTION")) m_session->set_property("EXECUTEACTION", action);
    log(log_bit(kMsgInfo), "Doing action: " + action);
    action_start(action);
    const char* ui_table = action == "INSTALL" ? "InstallUISequence" : action == "ADMIN" ? "AdminUISequence" : nullptr;
    u32 rc;
    // Learn: the UI sequence runs at full or reduced UI; the AdvtUISequence
    // table is never used. Otherwise the execute sequence runs directly.
    if (ui_table != nullptr && shows_wizard_ui() && m_session->package()->table(ui_table) != nullptr) {
        rc = run_sequence(ui_table);
    } else {
        rc = execute_action();
    }
    log(log_bit(kMsgInfo), "Action ended " + clock_text() + ": " + action + ". Return value " +
                               std::to_string(log_return_value(rc)) + ".");
    if (m_sequence_depth == 0) log_property_dump();
    write_log_text({});
    return rc;
}

u32 Engine::execute_action() {
    const std::string top = m_session->property("EXECUTEACTION").empty() ? m_session->property("ACTION")
                                                                          : m_session->property("EXECUTEACTION");
    const char* table = top == "ADMIN" ? "AdminExecuteSequence"
                      : top == "ADVERTISE" ? "AdvtExecuteSequence" : "InstallExecuteSequence";
    if (m_server != nullptr || m_options.server || m_session != m_client.get()) {
        // No client UI ran: this session is the one the execute sequence runs in.
        return run_sequence(table);
    }
    // Hand the install to a server session: package and command-line
    // properties, then the client's public properties the security rules
    // pass (Learn "Restricted Public Properties"); private properties stay in
    // the client.
    Session& client = *m_client;
    m_server = std::make_unique<Session>();
    Session& server = *m_server;
    if (!server.initialize(client.shared_package())) { m_server.reset(); return kErrInstallFailure; }
    server.set_engine(this);
    server.set_environment(m_machine.environment);
    apply_command_line(server, m_options.command_line);
    set_builtin_properties(server, true);
    const bool pass_all = client.property("AdminUser") == "1" || client.property("EnableUserControl") == "1";
    std::set<std::string> secure(restricted_public_properties());
    for (const std::string& p : split_list(client.property("SecureCustomProperties"))) secure.insert(p);
    bool restricted_any = false;
    for (const auto& [name, value] : client.properties()) {
        if (!Session::is_public_property(name)) continue;
        if (!pass_all && !secure.contains(name)) {
            if (server.property(name) != value) {
                restricted_any = true;
                log(log_bit(kMsgInfo), "Ignoring disallowed property " + name);
            }
            continue;
        }
        server.set_property(name, value);
    }
    if (restricted_any) server.set_property("RestrictedUserControl", "1");
    for (const ExecuteHandoffHook& hook : handoff_hooks()) hook(client, server);
    u32 rc;
    if (m_services.execute_in_service) {
        // The installer service runs the execute sequence with this server
        // session's properties (msi_service.hpp).
        log(log_bit(kMsgInfo), "Handing " + std::string(table) + " to the Windows Installer service");
        write_log_text({});
        rc = m_services.execute_in_service(server, table);
    } else {
        log(log_bit(kMsgInfo), "Switching to server session for " + std::string(table));
        m_session = &server;
        rc = run_sequence(table);
        if (log_bit(kMsgProgress) & m_options.log_mode) log_property_dump();
    }
    // Asynchronous custom actions of the server session end with it.
    (void)finish_async_custom_actions(server);
    m_session = &client;
    m_server.reset();
    return rc;
}

// ---------------------------------------------------------------------------
// The engine's standard actions
// ---------------------------------------------------------------------------

namespace {

u32 action_launch_conditions(ActionContext& ctx) {
    // Learn LaunchConditions: every row must be true, else its Description is
    // shown and the install ends.
    Session& s = ctx.session();
    const Table* t = s.package() != nullptr ? s.package()->table("LaunchCondition") : nullptr;
    if (t == nullptr) return kErrSuccess;
    const auto c = t->column_index("Condition");
    const auto d = t->column_index("Description");
    if (!c) return kErrInstallFailure;
    for (const auto& row : t->rows) {
        if (*c >= row.size()) continue;
        const auto ok = s.evaluate_condition(row[*c]);
        if (ok.has_value() && *ok) continue;
        Record r(0);
        r.fields[0].kind = Record::Kind::String;
        r.fields[0].text = to_u16((d && *d < row.size()) ? s.format(row[*d]) : std::string{});
        (void)ctx.message(kMsgError | 0x10 /* MB_ICONERROR */, r);
        return kErrInstallFailure;
    }
    return kErrSuccess;
}

u32 action_install_initialize(ActionContext& ctx) {
    // InstallInitialize begins the transaction. Only an install run may
    // change the system (probe: MsiDoAction of InstallInitialize from an
    // MsiOpenPackage session -> 1603).
    Engine* e = ctx.session().engine();
    if (e == nullptr || !e->begin_script()) return kErrInstallFailure;
    return kErrSuccess;
}

u32 action_install_execute(ActionContext& ctx) {
    Engine* e = ctx.session().engine();
    if (e == nullptr || !e->scripting()) return kErrInstallFailure;
    return e->execute_script(false);
}

u32 action_install_finalize(ActionContext& ctx) {
    Engine* e = ctx.session().engine();
    if (e == nullptr || !e->scripting()) return kErrInstallFailure;
    return e->execute_script(true);
}

u32 action_install_validate(ActionContext& ctx) {
    // Learn InstallValidate: volumes with costs must have the space; with too
    // little, PROMPTROLLBACKCOST decides (P prompt, D disable rollback, F fail).
    Session& s = ctx.session();
    s.set_property("CostingComplete", "1");
    if (s.property("OutOfDiskSpace") != "1") return kErrSuccess;
    const std::string mode = s.property("PROMPTROLLBACKCOST");
    if (mode == "D" && s.property("OutOfNoRbDiskSpace") != "1") {
        if (Engine* e = s.engine()) e->disable_rollback();
        return kErrSuccess;
    }
    Record r(0);
    r.fields[0].kind = Record::Kind::String;
    r.fields[0].text = to_u16("There is not enough disk space for this installation.");
    (void)ctx.message(kMsgOutOfDiskSpace, r);
    return kErrInstallFailure;
}

u32 action_disable_rollback(ActionContext& ctx) {
    if (Engine* e = ctx.session().engine()) e->disable_rollback();
    return kErrSuccess;
}

u32 action_schedule_reboot(ActionContext& ctx) {
    if (Engine* e = ctx.session().engine()) (void)e->set_mode(kRunRebootAtEnd, true);
    return kErrSuccess;
}

u32 action_force_reboot(ActionContext& ctx) {
    // REBOOT=ReallySuppress suppresses ForceReboot too (Learn REBOOT).
    if (ctx.session().property("REBOOT").rfind("R", 0) == 0) return kErrSuccess;
    log_unimplemented("ForceReboot", "a restart that resumes the install (RunOnce + suspended install) is not available in a capsule");
    return kErrInstallFailure;
}

u32 action_resolve_source(ActionContext& ctx) {
    // Learn ResolveSource: sets SourceDir (and SOURCEDIR) to the package's
    // source location.
    Session& s = ctx.session();
    std::string db = s.property("OriginalDatabase");
    const usize slash = db.find_last_of('\\');
    const std::string dir = slash == std::string::npos ? std::string{} : db.substr(0, slash + 1);
    if (dir.empty()) return kErrInstallFailure;
    s.set_property("SourceDir", dir);
    s.set_property("SOURCEDIR", dir);
    return kErrSuccess;
}

u32 action_validate_product_id(ActionContext& ctx) {
    // Learn ValidateProductID: sets ProductID from PIDKEY validated against
    // PIDTemplate (MaskedEdit syntax); does nothing when ProductID is set;
    // always succeeds. The visible segment is between < and >; > becomes a
    // dash; @ positions are random digits; # % digits; & ^ ? ` any character.
    Session& s = ctx.session();
    if (!s.property("ProductID").empty()) return kErrSuccess;
    const std::string key = s.property("PIDKEY");
    if (key.empty()) return kErrSuccess;
    const std::string tmpl = s.property("PIDTemplate");
    if (tmpl.empty()) { s.set_property("ProductID", key); return kErrSuccess; }
    const usize lt = tmpl.find('<');
    const usize gt = tmpl.find('>', lt == std::string::npos ? 0 : lt);
    const usize vis_begin = lt == std::string::npos ? 0 : lt + 1;
    const usize vis_end = gt == std::string::npos ? tmpl.size() : gt;
    const std::string visible = tmpl.substr(vis_begin, vis_end - vis_begin);
    auto editable = [](char c) { return c == '#' || c == '%' || c == '&' || c == '^' || c == '?' || c == '`'; };
    auto fits = [](char m, char c) {
        if (m == '#' || m == '%') return c >= '0' && c <= '9';
        return c != '\0';
    };
    // PIDKEY holds either the whole visible segment or only its editable
    // positions.
    std::string typed;
    usize editable_count = 0;
    for (const char c : visible) if (editable(c)) ++editable_count;
    if (key.size() == visible.size()) {
        for (usize i = 0; i < visible.size(); ++i) {
            if (editable(visible[i])) {
                if (!fits(visible[i], key[i])) return kErrSuccess;
                typed.push_back(key[i]);
            } else if (key[i] != visible[i]) {
                return kErrSuccess;
            }
        }
    } else if (key.size() == editable_count) {
        typed = key;
        usize k = 0;
        for (const char c : visible) if (editable(c) && !fits(c, typed[k++])) return kErrSuccess;
    } else {
        return kErrSuccess;
    }
    std::string id;
    usize k = 0;
    for (usize i = 0; i < tmpl.size(); ++i) {
        const char c = tmpl[i];
        if (i == lt) continue;
        if (i == gt) { id.push_back('-'); continue; }
        if (c == '@') { id.push_back(static_cast<char>('0' + std::rand() % 10)); continue; }
        if (i >= vis_begin && i < vis_end && editable(c)) { id.push_back(typed[k++]); continue; }
        id.push_back(c);
    }
    s.set_property("ProductID", id);
    return kErrSuccess;
}

u32 action_sfp_catalog(ActionContext&) {
    // Learn: InstallSFPCatalogFile installs Windows Me SFP catalogs; on the
    // NT family it has nothing to do.
    return kErrSuccess;
}

u32 action_install_admin_package(ActionContext& ctx) {
    Session& s = ctx.session();
    Engine* e = s.engine();
    if (e == nullptr || !ctx.scripting()) return kErrInstallFailure;
    const std::string source = s.property("OriginalDatabase");
    const std::string target_dir = with_slash(s.property("TARGETDIR"));
    const usize slash = source.find_last_of('\\');
    const std::string name = slash == std::string::npos ? source : source.substr(slash + 1);
    if (target_dir.size() < 2 || name.empty()) return kErrInstallFailure;
    MsiManager* mgr = &e->manager();
    const std::string target = target_dir + name;
    ScriptOp op;
    op.action = "InstallAdminPackage";
    op.run = [mgr, source, target]() -> u32 {
        auto bytes = mgr->engine_read_file(source);
        if (!bytes) return kErrInstallFailure;
        return mgr->engine_write_file(target, *bytes) ? kErrSuccess : kErrInstallFailure;
    };
    ctx.schedule(std::move(op));
    return kErrSuccess;
}

// Legacy behaviours, run through the sequencer until the owning areas'
// registrars replace them (a later ActionRegistry::add of the same name wins).

u32 action_legacy_install_files(ActionContext& ctx) {
    if (!ctx.scripting()) return kErrInstallFailure;
    Session* s = &ctx.session();
    MsiManager* mgr = &s->engine()->manager();
    ScriptOp op;
    op.action = "InstallFiles";
    op.run = [mgr, s]() -> u32 {
        s->resolve_directories();
        return mgr->engine_install_files(*s, s->property("OriginalDatabase"));
    };
    ctx.schedule(std::move(op));
    return kErrSuccess;
}

u32 action_legacy_write_registry(ActionContext& ctx) {
    if (!ctx.scripting()) return kErrInstallFailure;
    Session* s = &ctx.session();
    capsule::Registry* reg = ctx.services().registry;
    ScriptOp op;
    op.action = "WriteRegistryValues";
    op.run = [s, reg]() -> u32 {
        if (reg == nullptr) return kErrInstallFailure;
        return s->write_registry_values(*reg) ? kErrSuccess : kErrInstallFailure;
    };
    ctx.schedule(std::move(op));
    return kErrSuccess;
}

u32 action_legacy_register_product(ActionContext& ctx) {
    if (!ctx.scripting()) return kErrInstallFailure;
    Session* s = &ctx.session();
    MsiManager* mgr = &s->engine()->manager();
    ScriptOp op;
    op.action = "RegisterProduct";
    op.run = [mgr, s]() -> u32 {
        mgr->register_product(*s, s->property("OriginalDatabase"));
        return kErrSuccess;
    };
    ctx.schedule(std::move(op));
    return kErrSuccess;
}

}  // namespace

void register_engine_actions(ActionRegistry& r) {
    r.add("LaunchConditions", action_launch_conditions);
    r.add("InstallInitialize", action_install_initialize);
    r.add("InstallExecute", action_install_execute);
    r.add("InstallExecuteAgain", action_install_execute);
    r.add("InstallFinalize", action_install_finalize);
    r.add("InstallValidate", action_install_validate);
    r.add("DisableRollback", action_disable_rollback);
    r.add("ScheduleReboot", action_schedule_reboot);
    r.add("ForceReboot", action_force_reboot);
    r.add("ResolveSource", action_resolve_source);
    r.add("InstallSFPCatalogFile", action_sfp_catalog);
    r.add("ValidateProductID", action_validate_product_id);
    r.add("InstallAdminPackage", action_install_admin_package);
    r.add("InstallFiles", action_legacy_install_files);
    r.add("WriteRegistryValues", action_legacy_write_registry);
    r.add("RegisterProduct", action_legacy_register_product);
}

}  // namespace vitreo::winapi::msi

// ---------------------------------------------------------------------------
// Session functions (MsiQuery.h / Msi.h). A/W pairs share one
// implementation; A strings are CP_ACP and A counts are in bytes.
// ---------------------------------------------------------------------------

namespace vitreo::winapi::msi {

namespace {

struct SessionApi {
    MsiManager& mgr;

    std::shared_ptr<InstallHandle> install(u32 h) const { return mgr.handles().get<InstallHandle>(h); }

    static std::optional<std::string> read_str(Marshaler& m, GuestAddr p, bool wide) {
        if (p == 0) return std::nullopt;
        if (wide) {
            auto s = m.read_wide_string(p);
            if (s.is_err()) return std::nullopt;
            return s.value();
        }
        auto s = m.read_ansi_string(p);
        if (s.is_err()) return std::nullopt;
        return acp_to_utf8(s.value());
    }

    /// The MsiGetProperty buffer convention (probe "gp" lines): *pcch is the
    /// capacity including the terminator; on return the length without it.
    /// A null buffer reports the length with ERROR_SUCCESS; a short buffer is
    /// filled with what fits, terminated, and returns ERROR_MORE_DATA; a zero
    /// capacity writes nothing.
    static u32 write_out(Marshaler& m, GuestAddr buf, GuestAddr pcch, const std::string& value, bool wide,
                         bool null_buffer_ok = true) {
        if (pcch == 0) return kErrInvalidParameter;
        auto cap = m.read_u32(pcch);
        if (cap.is_err()) return kErrInvalidParameter;
        if (wide) {
            const std::vector<u16> units = utf8_to_utf16(value);
            const u32 need = static_cast<u32>(units.size());
            if (buf == 0) {
                (void)m.write_u32(pcch, need);
                return null_buffer_ok ? kErrSuccess : kErrInvalidParameter;
            }
            if (need + 1 > cap.value()) {
                if (cap.value() > 0) {
                    for (u32 k = 0; k + 1 < cap.value(); ++k) (void)m.write_u16(buf + k * 2, units[k]);
                    (void)m.write_u16(buf + (cap.value() - 1) * 2, 0);
                }
                (void)m.write_u32(pcch, need);
                return kErrMoreData;
            }
            for (u32 k = 0; k < need; ++k) (void)m.write_u16(buf + k * 2, units[k]);
            (void)m.write_u16(buf + need * 2, 0);
            (void)m.write_u32(pcch, need);
            return kErrSuccess;
        }
        const std::string bytes = utf8_to_acp(value);
        const u32 need = static_cast<u32>(bytes.size());
        if (buf == 0) {
            (void)m.write_u32(pcch, need);
            return null_buffer_ok ? kErrSuccess : kErrInvalidParameter;
        }
        if (need + 1 > cap.value()) {
            if (cap.value() > 0) {
                for (u32 k = 0; k + 1 < cap.value(); ++k) (void)m.write_u8(buf + k, static_cast<u8>(bytes[k]));
                (void)m.write_u8(buf + cap.value() - 1, 0);
            }
            (void)m.write_u32(pcch, need);
            return kErrMoreData;
        }
        for (u32 k = 0; k < need; ++k) (void)m.write_u8(buf + k, static_cast<u8>(bytes[k]));
        (void)m.write_u8(buf + need, 0);
        (void)m.write_u32(pcch, need);
        return kErrSuccess;
    }

    /// A deferred action's restricted property view (Learn).
    static std::string deferred_property(const InstallHandle& h, const std::string& name) {
        if (name == "CustomActionData") return h.custom_action_data;
        if (name == "UserSID") return h.engine->session().property("UserSID");
        if (name == "ProductCode" && h.script_kind != kRunCommit) return h.engine->session().property("ProductCode");
        return {};
    }

    void get_property(Marshaler& m, bool wide) {
        auto h = install(m.arg32(0));
        if (!h) { m.set_return(kErrInvalidHandle); return; }
        auto name = read_str(m, m.arg_ptr(1), wide);
        if (!name) { m.set_return(kErrInvalidParameter); return; }
        const std::string value = h->deferred ? deferred_property(*h, *name) : h->engine->session().property(*name);
        m.set_return(write_out(m, m.arg_ptr(2), m.arg_ptr(3), value, wide));
    }

    void set_property(Marshaler& m, bool wide) {
        auto h = install(m.arg32(0));
        if (!h) { m.set_return(kErrInvalidHandle); return; }
        auto name = read_str(m, m.arg_ptr(1), wide);
        if (!name) { m.set_return(kErrInvalidParameter); return; }
        if (name->empty() || h->deferred) { m.set_return(kErrFunctionFailed); return; }  // probe: "" -> 1627
        auto value = read_str(m, m.arg_ptr(2), wide);
        Engine& e = *h->engine;
        const std::string before = e.session().property(*name);
        const std::string after = value.value_or(std::string{});  // NULL value deletes (probe)
        e.session().set_property(*name, after);
        if (before != after) {
            e.log(kLogVerbose, "PROPERTY CHANGE: " + *name + " = '" + after + "'");
        }
        m.set_return(kErrSuccess);
    }

    void get_mode(Marshaler& m) {
        auto h = install(m.arg32(0));
        const u32 mode = m.arg32(1);
        if (!h) { m.set_return(0); return; }
        if (h->deferred) { m.set_return(mode == h->script_kind ? 1 : 0); return; }
        m.set_return(h->engine->get_mode(mode) ? 1 : 0);
    }

    void set_mode(Marshaler& m) {
        auto h = install(m.arg32(0));
        if (!h) { m.set_return(kErrInvalidHandle); return; }
        if (h->deferred) { m.set_return(kErrAccessDenied); return; }
        m.set_return(h->engine->set_mode(m.arg32(1), m.arg32(2) != 0));
    }

    void get_language(Marshaler& m) {
        auto h = install(m.arg32(0));
        if (!h) { m.set_return(kErrInvalidHandle); return; }  // probe: lang h0 -> 6
        if (h->script_kind == kRunCommit) { m.set_return(0); return; }
        m.set_return(static_cast<u32>(std::strtoul(h->engine->session().property("ProductLanguage").c_str(), nullptr, 10)) & 0xFFFF);
    }

    void evaluate_condition(Marshaler& m, bool wide) {
        auto h = install(m.arg32(0));
        if (!h || h->deferred) { m.set_return(static_cast<u32>(ConditionResult::Error)); return; }  // probe: h0 -> 3
        auto expr = read_str(m, m.arg_ptr(1), wide);
        if (!expr) { m.set_return(static_cast<u32>(ConditionResult::None)); return; }  // probe: NULL -> 2
        m.set_return(static_cast<u32>(h->engine->session().evaluate_condition_ex(*expr)));
    }

    void do_action(Marshaler& m, bool wide) {
        auto h = install(m.arg32(0));
        if (!h) { m.set_return(kErrInvalidHandle); return; }
        if (h->deferred) { m.set_return(kErrInvalidHandle); return; }
        auto name = read_str(m, m.arg_ptr(1), wide);
        std::shared_ptr<Engine> e = h->engine;
        if (!name) {
            // Learn: a null action is the upper-cased ACTION property, else INSTALL.
            std::string a = e->session().property("ACTION");
            for (char& c : a) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            name = a.empty() ? std::string("INSTALL") : a;
        }
        m.set_return(e->do_action(*name));
    }

    void sequence(Marshaler& m, bool wide) {
        auto h = install(m.arg32(0));
        if (!h) { m.set_return(kErrInvalidHandle); return; }
        if (h->deferred) { m.set_return(kErrInvalidHandle); return; }
        auto table = read_str(m, m.arg_ptr(1), wide);
        if (!table || m.arg32(2) != 0) { m.set_return(kErrInvalidParameter); return; }  // probe: mode 1 -> 87
        std::shared_ptr<Engine> e = h->engine;
        m.set_return(e->run_sequence(*table));
    }

    std::optional<Record> record(u32 h) const {
        if (auto r = mgr.handles().get<Record>(h)) return *r;
        return mgr.engine_legacy_record(h);
    }

    void process_message(Marshaler& m) {
        auto h = install(m.arg32(0));
        const u32 type = m.arg32(1);
        auto rec = record(m.arg32(2));
        if (!h || !rec) { m.set_return(static_cast<u32>(-1)); return; }  // probe: h0 / rec0 -> -1
        const u32 kind = type & 0xFF000000u;
        // The engine's own lifecycle messages and unknown kinds are not
        // accepted from a caller (probe: INITIALIZE/TERMINATE/SHOWDIALOG,
        // RMFILESINUSE, 0x10000000, 0x1A000000 -> -1).
        const bool accepted = kind == kMsgFatalExit || kind == kMsgError || kind == kMsgWarning || kind == kMsgUser ||
                              kind == kMsgInfo || kind == kMsgFilesInUse || kind == kMsgResolveSource ||
                              kind == kMsgOutOfDiskSpace || kind == kMsgActionStart || kind == kMsgActionData ||
                              kind == kMsgProgress || kind == kMsgCommonData;
        if (!accepted) { m.set_return(static_cast<u32>(-1)); return; }
        if (kind == kMsgCommonData) {
            // [1] 0 language / 1 caption / 2 cancel button (Learn); anything else is rejected.
            const bool ok = rec->count() >= 1 && rec->fields[1].kind == Record::Kind::Integer &&
                            rec->fields[1].integer >= 0 && rec->fields[1].integer <= 2;
            if (!ok) { m.set_return(static_cast<u32>(-1)); return; }
        }
        m.set_return(static_cast<u32>(h->engine->message(type, *rec)));
    }

    void format_record(Marshaler& m, bool wide) {
        const u32 hi = m.arg32(0);
        auto rec = record(m.arg32(1));
        if (!rec) { m.set_return(kErrInvalidHandle); return; }
        std::string text;
        if (hi == 0) {
            text = Session::format_record_without_session(*rec);
        } else {
            auto h = install(hi);
            if (!h) { m.set_return(kErrInvalidHandle); return; }
            if (h->deferred) {
                // Deferred: only CustomActionData and ProductCode (Learn).
                Session limited;
                limited.set_property("CustomActionData", h->custom_action_data);
                limited.set_property("ProductCode", deferred_property(*h, "ProductCode"));
                text = limited.format_record(*rec);
            } else {
                text = h->engine->session().format_record(*rec);
            }
        }
        m.set_return(write_out(m, m.arg_ptr(2), m.arg_ptr(3), text, wide));
    }

    void open_package(Marshaler& m, bool wide, bool ex) {
        const GuestAddr path_ptr = m.arg_ptr(0);
        const u32 options = ex ? m.arg32(1) : 0;
        const GuestAddr out = m.arg_ptr(ex ? 2 : 1);
        auto path = read_str(m, path_ptr, wide);
        if (!path || out == 0 || (options & ~1u) != 0) { m.set_return(kErrInvalidParameter); return; }
        u32 handle = 0;
        const u32 rc = open_package_path(*path, options != 0, handle);
        if (rc == kErrSuccess) (void)m.write_u32(out, handle);
        m.set_return(rc);
    }

    u32 open_package_path(const std::string& path, bool restricted, u32& handle) {
        if (g_open_package_sessions.load() > 0) return kErrAlreadyRunning;  // probe: open_missing with a session open
        if (!path.empty() && path[0] == '#') {
            // "#<database handle>": a database opened with MsiOpenDatabase.
            auto pkg = mgr.engine_database_package(static_cast<u32>(std::strtoul(path.c_str() + 1, nullptr, 10)));
            if (!pkg) return kErrInvalidHandle;  // probe: a non-database handle -> 6
            return open_from(pkg, path, restricted, handle);
        }
        auto bytes = mgr.engine_read_file(path);
        if (!bytes) return kErrPackageOpenFailed;
        auto pkg = Package::open(std::span<const u8>(bytes->data(), bytes->size()));
        if (!pkg) return kErrPackageInvalid;
        return open_from(std::make_shared<Package>(std::move(*pkg)), path, restricted, handle);
    }

    u32 open_from(std::shared_ptr<Package> pkg, const std::string& path, bool restricted, u32& handle) {
        Engine::Options o;
        o.package_path = path;
        o.ui_level = mgr.engine_ui_level();
        o.restricted = restricted;
        u32 error = 0;
        auto engine = Engine::open(mgr, std::move(pkg), o, mgr.engine_services(), error);
        if (!engine) return error;
        auto h = std::make_shared<InstallHandle>();
        h->engine = engine;
        // The session counts as open until its handle closes.
        ++g_open_package_sessions;
        std::shared_ptr<InstallHandle> counted(h.get(), [h](InstallHandle*) mutable { --g_open_package_sessions; h.reset(); });
        handle = mgr.handles().add(std::move(counted));
        return kErrSuccess;
    }

    void open_product(Marshaler& m, bool wide) {
        auto code = read_str(m, m.arg_ptr(0), wide);
        const GuestAddr out = m.arg_ptr(1);
        if (!code || out == 0 || code->size() != 38) { m.set_return(kErrInvalidParameter); return; }
        if (!mgr.is_product_registered(*code)) { m.set_return(kErrUnknownProduct); return; }
        const std::string local = mgr.product_property(*code, "LocalPackage");
        if (local.empty()) { m.set_return(kErrUnknownProduct); return; }
        u32 handle = 0;
        const u32 rc = open_package_path(local, false, handle);
        if (rc == kErrSuccess) (void)m.write_u32(out, handle);
        m.set_return(rc);
    }
};

}  // namespace

void register_session_api(MsiManager& mgr, HandlerMap& map) {
    auto api = std::make_shared<SessionApi>(SessionApi{mgr});
    for (const bool wide : {false, true}) {
        const char* sfx = wide ? "W" : "A";
        map[std::string("MsiGetProperty") + sfx] = [api, wide](Marshaler& m) { api->get_property(m, wide); };
        map[std::string("MsiSetProperty") + sfx] = [api, wide](Marshaler& m) { api->set_property(m, wide); };
        map[std::string("MsiEvaluateCondition") + sfx] = [api, wide](Marshaler& m) { api->evaluate_condition(m, wide); };
        map[std::string("MsiDoAction") + sfx] = [api, wide](Marshaler& m) { api->do_action(m, wide); };
        map[std::string("MsiSequence") + sfx] = [api, wide](Marshaler& m) { api->sequence(m, wide); };
        map[std::string("MsiFormatRecord") + sfx] = [api, wide](Marshaler& m) { api->format_record(m, wide); };
        map[std::string("MsiOpenPackage") + sfx] = [api, wide](Marshaler& m) { api->open_package(m, wide, false); };
        map[std::string("MsiOpenPackageEx") + sfx] = [api, wide](Marshaler& m) { api->open_package(m, wide, true); };
        map[std::string("MsiOpenProduct") + sfx] = [api, wide](Marshaler& m) { api->open_product(m, wide); };
    }
    map["MsiGetMode"] = [api](Marshaler& m) { api->get_mode(m); };
    map["MsiSetMode"] = [api](Marshaler& m) { api->set_mode(m); };
    map["MsiGetLanguage"] = [api](Marshaler& m) { api->get_language(m); };
    map["MsiProcessMessage"] = [api](Marshaler& m) { api->process_message(m); };
}

}  // namespace vitreo::winapi::msi
