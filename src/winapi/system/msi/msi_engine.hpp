#pragma once
/// @file msi_engine.hpp
/// @brief Windows Installer install sessions: properties, formatted strings,
///        conditions, directory resolution, and the Engine that runs a
///        session's sequences and execution script.
///
/// Contract sources: Microsoft Learn "Formatted", "Conditional Statement
/// Syntax", "Property Reference", "Using a Sequence Table", the sequence-table
/// pages, "Custom Action Return Values", "Logging of Action Return Values",
/// "Obtaining Context Information for Deferred Execution Custom Actions",
/// "Restricted Public Properties", MsiQuery.h / Msi.h (SDK 10.0.26100), and
/// tools/oracle_probe/msi_engine_session_probe.c (msi_engine32/64.expected)
/// for what Learn leaves open. Contract notes and gaps:
/// .docs/contracts/msi/engine.md.
///
/// ## How the other areas use the engine (the API in brief)
///
///   * An action is `u32 fn(ActionContext&)` registered by name in an
///     ActionRegistry (msi_core.hpp). The engine dispatches a sequence row or
///     an MsiDoAction name to: the top-level actions (INSTALL, ADMIN,
///     ADVERTISE, SEQUENCE), then the standard-action registry, then the
///     CustomAction table (run_custom_action), then the Dialog table
///     (Services::ui->run_dialog) when the internal UI shows dialogs.
///   * `ctx.session().engine()` is the running Engine. An action that changes
///     the system must check `ctx.scripting()`: outside InstallInitialize ..
///     InstallFinalize it returns kActionFailure (Windows: MsiDoAction of
///     InstallFiles from an MsiOpenPackage session is 1603). Inside, it does
///     its work by `ctx.schedule(ScriptOp{...})`; InstallExecute,
///     InstallExecuteAgain and InstallFinalize run the scheduled ops in order,
///     a failed op rolls back what ran (newest first), commit ops run after a
///     successful script.
///   * While a ScriptOp runs, `engine->script_context()` says whether it is
///     the install, rollback or commit script; a deferred custom action sets
///     its CustomActionData with `engine->set_custom_action_data()` before it
///     calls the guest and opens its restricted hInstall with
///     `engine->open_install_handle()` (MsiGetProperty then answers only
///     CustomActionData, ProductCode and UserSID, MsiGetMode only
///     SCHEDULED/ROLLBACK/COMMIT, per Learn).
///   * Feature/component/file state for the `$ ? & !` condition operands and
///     the `[#file] [!file] [$component]` format forms comes from the
///     resolvers in `session.item_states()`, which the features and files
///     areas install (CostInitialize). Unset resolvers mean "no state yet",
///     which Windows evaluates as a null operand (probe: `&F1 = -1` is false
///     and `&NoFeat = ""` is true before CostFinalize).
///   * `session.engine()->message(type, record)` is MsiProcessMessage: Error
///     table templates, logging, then the UI host.
///   * Add a hook with `add_execute_handoff_hook()` to carry client-side state
///     into the server session that runs the execute sequence (the features
///     area turns UI selections into ADDLOCAL/REMOVE/... there).

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi_package.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vitreo::capsule { class Registry; class Vfs; }
namespace vitreo::winapi { class MsiManager; }

namespace vitreo::winapi::msi {

class Engine;

// ---- Machine state behind the built-in properties ------------------------------

/// What the built-in properties describe (Learn "Property Reference",
/// measured set in msi_engine*.expected). The engine computes every
/// property from these values; nothing is copied from a Windows capture.
/// Filled by whoever runs the engine from Vitreo's own system state; an
/// optional field that is unset leaves its property unset (see engine.md).
struct MachineState {
    u32 os_major{10};
    u32 os_minor{0};
    u32 os_build{0};
    u32 service_pack_major{0};
    u32 service_pack_minor{0};
    u32 product_type{1};          ///< VER_NT_WORKSTATION
    bool os_64bit{true};
    u32 processor_level{0};       ///< SYSTEM_INFO.wProcessorLevel
    std::string computer_name;
    std::string logon_user;
    std::string user_sid;
    bool admin_user{false};
    bool elevated{false};
    std::string windows_dir;      ///< "C:\\Windows" (no trailing backslash)
    std::string profile_dir;      ///< "C:\\Users\\<user>"
    std::string program_data;     ///< "C:\\ProgramData"
    std::string temp_dir;         ///< the user's TEMP directory
    u32 user_lcid{0};
    u32 system_lcid{0};
    std::optional<u64> physical_memory_mb;
    std::optional<u64> virtual_memory_mb;
    struct Display { u32 screen_x, screen_y, color_bits, text_height, border_side, border_top, caption_height; };
    std::optional<Display> display;
    u32 process_id{0};
    std::string current_directory;
    /// %name lookups (environment variables are case-insensitive). Empty
    /// function: no environment is available, every %name is unset.
    std::function<std::optional<std::string>(const std::string&)> environment;
};

/// Vitreo's own system state for a capsule: the OS version the runtime
/// reports to Windows Installer (msiexec.exe declares Windows 8.1
/// compatibility, so it sees 6.3.9600 on Windows 10/11: probe p0-S
/// VersionNT=603 WindowsBuild=9600), the capsule user profile layout that
/// kernel32's environment and shell32's known folders use, and the capsule
/// identity.
[[nodiscard]] MachineState default_machine_state(capsule::Registry* registry, const std::wstring& user_sid);

// ---- Install session -------------------------------------------------------------

/// Resolvers for install state, installed by the features/files areas.
/// Every function may be empty; an empty function or nullopt is a null value.
struct ItemStateResolvers {
    std::function<std::optional<i32>(const std::string&)> component_action;     ///< $component
    std::function<std::optional<i32>(const std::string&)> component_installed;  ///< ?component
    std::function<std::optional<i32>(const std::string&)> feature_action;       ///< &feature
    std::function<std::optional<i32>(const std::string&)> feature_installed;    ///< !feature
    /// [#file] (short_path false) / [!file] (true): full path of a File-table key.
    std::function<std::optional<std::string>(const std::string& file, bool short_path)> file_path;
    /// [$component]: directory of a component per its action state.
    std::function<std::optional<std::string>(const std::string&)> component_directory;
    /// Session::directory_path: the features area's resolved target path of a
    /// Directory key once CostFinalize ran (it honours SetTargetPath).
    std::function<std::optional<std::string>(const std::string&)> directory_path;
};

/// MsiEvaluateCondition's MSICONDITION (MsiQuery.h).
enum class ConditionResult : u32 { False = 0, True = 1, None = 2, Error = 3 };

/// One install session: the package plus the property state built up as the
/// sequence runs.
class Session {
public:
    Session() = default;

    /// Seed properties from the package's Property table. Returns false when
    /// the package has no usable Property table. Built-in properties are the
    /// Engine's (Engine::open / set_builtin_properties).
    bool initialize(std::shared_ptr<Package> package);

    [[nodiscard]] Package* package() const { return m_package.get(); }
    [[nodiscard]] std::shared_ptr<Package> shared_package() const { return m_package; }

    /// The Engine running this session (null for a bare Session in tests).
    [[nodiscard]] Engine* engine() const { return m_engine; }
    void set_engine(Engine* engine) { m_engine = engine; }

    // -- properties --------------------------------------------------------

    /// Property lookup. An unset property is the empty string; setting a
    /// property to "" deletes it (probe: MsiSetProperty(NewP,"") then
    /// MsiGetProperty returns ""). Names are case-sensitive.
    [[nodiscard]] std::string property(const std::string& name) const;
    [[nodiscard]] bool has_property(const std::string& name) const;
    void set_property(const std::string& name, const std::string& value);

    [[nodiscard]] const std::map<std::string, std::string>& properties() const {
        return m_properties;
    }

    /// Learn "Public Properties": a public property name has no lowercase
    /// letters.
    [[nodiscard]] static bool is_public_property(const std::string& name);

    // -- formatted strings -------------------------------------------------

    /// Expand a Formatted string (Learn "Formatted"; probe fmt lines):
    /// [Property] (values are not re-formatted), nested [[Property]] resolved
    /// inside out, [%env], [\x], [~] (NUL), [#file] [!file] [$component],
    /// {..} blocks that vanish unless every reference in them resolved,
    /// {{..}} (log-only text, removed), unmatched brackets kept literally.
    [[nodiscard]] std::string format(const std::string& text) const;

    /// MsiFormatRecord with this session: [N] record fields, then the
    /// Formatted rules. A null field 0 formats as "1: v1 2: v2 ... ".
    [[nodiscard]] std::string format_record(const Record& record) const;

    /// MsiFormatRecord with no session (hInstall 0): record fields only,
    /// property references are left in the text.
    [[nodiscard]] static std::string format_record_without_session(const Record& record);

    /// WriteRegistryValues (legacy, run by the sequencer until the search
    /// area's implementation replaces it).
    bool write_registry_values(capsule::Registry& registry);

    // -- conditions --------------------------------------------------------

    /// Evaluate a conditional expression. Empty/blank is true (a sequence row
    /// with no condition runs). nullopt is a syntax error, which terminates
    /// a sequence with 1603 (probe: ProbeSeqBad).
    [[nodiscard]] std::optional<bool> evaluate_condition(const std::string& expression) const;
    /// MsiEvaluateCondition's four-way result.
    [[nodiscard]] ConditionResult evaluate_condition_ex(const std::string& expression) const;

    /// State resolvers the features/files areas install.
    [[nodiscard]] ItemStateResolvers& item_states() { return m_item_states; }
    [[nodiscard]] const ItemStateResolvers& item_states() const { return m_item_states; }

    /// %name for conditions and [%name]: the engine's environment.
    void set_environment(std::function<std::optional<std::string>(const std::string&)> lookup) {
        m_environment = std::move(lookup);
    }
    [[nodiscard]] std::optional<std::string> environment(const std::string& name) const;

    // -- directories -------------------------------------------------------

    /// Resolve every row of the Directory table to an absolute target path.
    void resolve_directories();

    /// Absolute target path for a Directory key, or empty if unknown.
    [[nodiscard]] std::string directory_path(const std::string& key) const;

    // -- per-area state ----------------------------------------------------

    /// Session-scoped state an engine area keeps (feature and component
    /// states, costs, the execution script, UI state...), created on first
    /// use. One instance of each T per session; areas never edit Session
    /// itself to add their state.
    template <typename T>
    T& state() {
        static const char tag{};
        auto& slot = m_area_state[&tag];
        if (!slot) slot = std::shared_ptr<void>(new T(), [](void* p) { delete static_cast<T*>(p); });
        return *static_cast<T*>(slot.get());
    }

private:
    std::string resolve_directory(const std::string& key, int depth);
    std::string format_impl(const std::string& text, const Record* record, bool with_session) const;

    std::shared_ptr<Package> m_package;
    Engine* m_engine{nullptr};
    std::map<const void*, std::shared_ptr<void>> m_area_state;
    std::map<std::string, std::string> m_properties;
    std::map<std::string, std::string> m_directories;
    std::map<std::string, std::pair<std::string, std::string>> m_directory_rows;
    ItemStateResolvers m_item_states;
    std::function<std::optional<std::string>(const std::string&)> m_environment;
};

// ---- msiexec command line -----------------------------------------------------------

/// What an msiexec command line asked for (Learn "Command-Line Options",
/// "Standard Installer Command-Line Options").
struct MsiexecCommand {
    enum class Mode {
        Install,        ///< /i, /package
        Uninstall,      ///< /x, /uninstall
        Repair,         ///< /f[pecdoamusv]
        Administrative, ///< /a
        Advertise,      ///< /j[u|m]
        Patch,          ///< /p, /update
        RegisterServer, ///< /y module
        UnregisterServer, ///< /z module
        Help,           ///< /? /h /help
        Unsupported,    ///< no mode given or an unknown switch
    };

    Mode mode = Mode::Unsupported;
    /// Package path, or a product code for /x and /f.
    std::string package;
    /// True for /quiet, /qn, /qb, /passive — no UI sequence runs.
    bool quiet = false;
    /// INSTALLUILEVEL_* (Msi.h) with modifiers; FULL when no /q is given.
    u32 ui_level = 5;
    /// PROPERTY=VALUE pairs given on the command line, in order.
    std::map<std::string, std::string> properties;
    /// /l: log file and INSTALLLOGMODE letters ("*v", "+", "!" kept raw).
    std::string log_file;
    std::string log_modes;
    /// /f letters (REINSTALLMODE), /j u|m, /t transforms, /g language.
    std::string repair_modes;
    char advertise_target = 0;
    std::string transforms;
    std::string language;
    /// /norestart /forcerestart /promptrestart
    enum class Restart { Default, No, Force, Prompt } restart = Restart::Default;
    /// The first switch the parser did not recognize (1639 when set).
    std::string invalid;
};

/// Parse an msiexec command line. The first token is the executable name and
/// is skipped. Returns nullopt only when the line is empty.
[[nodiscard]] std::optional<MsiexecCommand> parse_msiexec_command_line(const std::string& command_line);

/// Split a `DefaultDir` cell into its target name, dropping the source half
/// and preferring the long name. A `.` target means "no path segment".
[[nodiscard]] std::string default_dir_target(const std::string& default_dir);

// ---- The engine ------------------------------------------------------------------------

/// MSIRUNMODE (MsiQuery.h).
enum RunMode : u32 {
    kRunAdmin = 0, kRunAdvertise = 1, kRunMaintenance = 2, kRunRollbackEnabled = 3,
    kRunLogEnabled = 4, kRunOperations = 5, kRunRebootAtEnd = 6, kRunRebootNow = 7,
    kRunCabinet = 8, kRunSourceShortNames = 9, kRunTargetShortNames = 10,
    kRunWindows9x = 12, kRunZawEnabled = 13, kRunScheduled = 16, kRunRollback = 17,
    kRunCommit = 18,
};

/// The install-session object behind an MSIHANDLE from MsiOpenPackage(Ex),
/// MsiOpenProduct, or the hInstall a custom action receives.
struct InstallHandle {
    std::shared_ptr<Engine> engine;
    /// A deferred/rollback/commit custom action's handle: restricted context.
    bool deferred{false};
    u32 script_kind{0};            ///< kRunScheduled / kRunRollback / kRunCommit
    std::string custom_action_data;
};

/// Hook run when the client hands the install to the server session that
/// runs the execute sequence (ExecuteAction).
using ExecuteHandoffHook = std::function<void(Session& client, Session& server)>;
void add_execute_handoff_hook(ExecuteHandoffHook hook);

/// The standard actions Learn lists ("Standard Actions Reference"). A name
/// here with no registered implementation fails loudly when sequenced.
[[nodiscard]] bool is_standard_action(const std::string& name);

/// Every area's standard actions, registered once.
[[nodiscard]] const ActionRegistry& standard_actions();

class Engine : public std::enable_shared_from_this<Engine> {
public:
    struct Options {
        std::string package_path;      ///< OriginalDatabase / DATABASE
        u32 ui_level{5};               ///< INSTALLUILEVEL_* with modifiers
        bool restricted{false};        ///< MSIOPENPACKAGEFLAGS_IGNOREMACHINESTATE
        bool server{false};            ///< runs the execute sequence (msiexec /qn, ExecuteAction)
        /// An install run (msiexec, MsiInstallProduct) may change the system;
        /// an MsiOpenPackage session may not (probe: InstallInitialize -> 1603).
        bool allow_system_changes{false};
        std::map<std::string, std::string> command_line;  ///< PROPERTY=value
        std::string log_file;          ///< /l file ("" = no log)
        u32 log_mode{0};               ///< INSTALLLOGMODE_*
        bool log_append{false};
        bool log_flush{false};
    };

    /// Create a session for @p package (MsiOpenPackage, msiexec). Returns
    /// null with @p error set (1620 when the package has no Property table).
    static std::shared_ptr<Engine> open(MsiManager& manager, std::shared_ptr<Package> package,
                                        Options options, Services services, u32& error);
    ~Engine();

    [[nodiscard]] Session& session() { return *m_session; }
    [[nodiscard]] Services& services() { return m_services; }
    [[nodiscard]] MsiManager& manager() { return m_manager; }
    [[nodiscard]] const Options& options() const { return m_options; }
    [[nodiscard]] u32 ui_level() const { return m_options.ui_level; }
    /// True when the internal UI runs the UI sequence (full or reduced).
    [[nodiscard]] bool shows_wizard_ui() const;
    [[nodiscard]] bool restricted() const { return m_options.restricted; }

    /// MsiDoAction. An empty name is 1603; a name nothing defines is 1626
    /// (probe doaction lines).
    u32 do_action(const std::string& name);
    /// MsiSequence: rows ordered by Sequence; termination rows -1..-4.
    u32 run_sequence(const std::string& table);
    /// The top-level action of an install (INSTALL / ADMIN / ADVERTISE).
    u32 run_top_level(const std::string& action);

    // -- execution script --
    [[nodiscard]] bool scripting() const { return m_scripting; }
    /// InstallInitialize: open the script. False when this session may not
    /// change the system or a script is already open.
    bool begin_script();
    void schedule(ScriptOp op);
    /// Run the ops scheduled so far (InstallExecute / InstallFinalize).
    u32 execute_script(bool finalize);
    void disable_rollback();
    /// While a ScriptOp runs: kRunScheduled / kRunRollback / kRunCommit, else 0.
    [[nodiscard]] u32 script_kind() const { return m_script_kind; }
    void set_custom_action_data(std::string data) { m_custom_action_data = std::move(data); }
    [[nodiscard]] const std::string& custom_action_data() const { return m_custom_action_data; }

    /// A new hInstall for this session. Inside a running ScriptOp it is the
    /// restricted deferred handle carrying the current CustomActionData.
    u32 open_install_handle();

    // -- modes --
    [[nodiscard]] bool get_mode(u32 mode) const;
    /// MsiSetMode: only REBOOTATEND / REBOOTNOW are settable (probe: others 5).
    u32 set_mode(u32 mode, bool on);

    // -- messages and logging --
    /// MsiProcessMessage: returns the handler's IDOK/IDCANCEL/..., 0 when
    /// nothing handled it, -1 for a message type it does not accept.
    i32 message(u32 type, const Record& record);
    /// Log one line (no message processing) if the log is enabled for @p kind
    /// (an INSTALLLOGMODE bit).
    void log(u32 kind, const std::string& text);
    /// Write the property dump (Property(S)/(C) lines) to the log.
    void log_property_dump();
    /// Write out what the log buffered.
    void flush_log() { write_log_text({}); }

    /// Learn "INSTALLMESSAGE_ACTIONSTART": [1] action, [2] description, [3] template.
    void action_start(const std::string& name);

    /// Built-in properties (Learn "Property Reference"), from MachineState.
    void set_builtin_properties(Session& session, bool server_side);

    /// The server session ExecuteAction runs the execute sequence in (owns
    /// it while it runs).
    [[nodiscard]] Session* server_session() { return m_server.get(); }

private:
    Engine(MsiManager& manager, Options options, Services services);
    u32 dispatch(const std::string& name, bool in_sequence, const std::string& table);
    u32 execute_action();
    void rollback();
    std::string error_template(i32 code) const;
    std::string apply_hidden(const std::string& text) const;
    void open_log();
    void write_log_text(const std::string& text);

    /// The UI half of message(): external handlers, then the internal UI.
    i32 route_ui(u32 type, const Record& record);
    MsiManager& m_manager;
    Options m_options;
    Services m_services;
    std::unique_ptr<Session> m_client;   ///< the session MsiOpenPackage / the UI sequence sees
    Session* m_session{nullptr};         ///< the active session (client, or server during ExecuteAction)
    std::unique_ptr<Session> m_server;
    MachineState m_machine;

    // execution script
    bool m_scripting{false};
    bool m_rollback_enabled{true};
    std::vector<ScriptOp> m_pending;
    std::vector<ScriptOp> m_rollback;   ///< rollback ops of what ran, oldest first
    std::vector<ScriptOp> m_commit;
    u32 m_script_kind{0};
    std::string m_custom_action_data;

    bool m_reboot_at_end{false};
    bool m_reboot_now{false};
    int m_sequence_depth{0};
    int m_script_depth{0};              ///< sequence depth InstallInitialize ran at
    std::string m_current_sequence;

    std::recursive_mutex m_mutex;
    std::string m_log_buffer;
    bool m_log_open{false};
};

} // namespace vitreo::winapi::msi
