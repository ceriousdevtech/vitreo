#pragma once
/// @file msi_core.hpp
/// @brief The Windows Installer engine's shared framework: MSIHANDLEs, the
/// msi.dll export registrars, the action registry the sequencer dispatches
/// through, the execution script, and the host services actions use.
///
/// Every msi.dll export (295, msi_export_table.inc, generated from the real
/// DLLs) is registered; an area file supplies the handlers of its family
/// through a registrar. An export no registrar supplies fails with
/// ERROR_CALL_NOT_IMPLEMENTED and a one-time UNIMPLEMENTED line.
///
/// Contract sources: Msi.h / MsiQuery.h / MsiDefs.h (Windows SDK 10.0.26100),
/// the Microsoft Learn "Windows Installer" reference (standard actions,
/// custom action types, database tables, Formatted/Condition syntax, the
/// installer functions), and tools/oracle_probe/msi_* probes for what Learn
/// leaves open.

#include <vitreo/common.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule {
class Registry;
class Vfs;
}  // namespace vitreo::capsule

namespace vitreo::winapi {
class Marshaler;
class MsiManager;
}  // namespace vitreo::winapi

namespace vitreo::winapi::msi {

class Session;

// ---- MSIHANDLE ---------------------------------------------------------------

/// MSIHANDLEs: one 32-bit space for every kind (database, view, record,
/// summary info, install session), on both bitnesses (MSIHANDLE is
/// `unsigned long`). MsiCloseHandle closes any of them. Objects are shared,
/// so a view keeps its database alive after the database handle closes, as
/// Windows does. Thread-safe: custom actions call in from guest threads.
class HandleTable {
    using TypeTag = const void*;
    template <typename T>
    static TypeTag tag() { static const char t{}; return &t; }

public:
    template <typename T>
    u32 add(std::shared_ptr<T> object) {
        std::lock_guard lock(m_mutex);
        u32 h = m_next++;
        if (m_next == 0) m_next = 1;
        m_objects[h] = Entry{tag<T>(), std::move(object), current_thread_key()};
        return h;
    }
    /// The object behind @p h when it is a T, else null.
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> get(u32 h) const {
        std::lock_guard lock(m_mutex);
        const auto it = m_objects.find(h);
        if (it == m_objects.end() || it->second.type != tag<T>()) return {};
        return std::static_pointer_cast<T>(it->second.object);
    }
    [[nodiscard]] bool contains(u32 h) const {
        std::lock_guard lock(m_mutex);
        return m_objects.contains(h);
    }
    /// MsiCloseHandle: false for an unknown handle.
    bool close(u32 h) {
        std::lock_guard lock(m_mutex);
        return m_objects.erase(h) != 0;
    }
    /// The calling thread's handles whose object is a T.
    template <typename T>
    [[nodiscard]] std::vector<u32> of_current_thread() const {
        std::lock_guard lock(m_mutex);
        const u64 key = current_thread_key();
        std::vector<u32> out;
        for (const auto& [h, e] : m_objects)
            if (e.thread == key && e.type == tag<T>()) out.push_back(h);
        return out;
    }
    /// MsiCloseAllHandles: closes the calling thread's handles and returns
    /// how many were open (Learn: "returns 0 if all handles are closed").
    u32 close_all_of_current_thread() {
        std::lock_guard lock(m_mutex);
        const u64 key = current_thread_key();
        u32 n = 0;
        for (auto it = m_objects.begin(); it != m_objects.end();) {
            if (it->second.thread == key) { it = m_objects.erase(it); ++n; }
            else ++it;
        }
        return n;
    }

private:
    struct Entry {
        TypeTag type{nullptr};
        std::shared_ptr<void> object;
        u64 thread{0};
    };
    static u64 current_thread_key();
    mutable std::mutex m_mutex;
    std::unordered_map<u32, Entry> m_objects;
    u32 m_next{1};
};

// ---- msi.dll exports ------------------------------------------------------------

using Handler = std::function<void(Marshaler&)>;
using HandlerMap = std::unordered_map<std::string, Handler>;

/// One-time `[vitreo] UNIMPLEMENTED msi!<api>: <what>` line.
void log_unimplemented(std::string_view api, std::string_view what);

// One registrar per area (msi_<area>.cpp); later ones replace earlier ones
// for the same export name.
void register_legacy(MsiManager& mgr, HandlerMap& map);        ///< msi.cpp (pre-engine surface)
void register_database_api(MsiManager& mgr, HandlerMap& map);  ///< database, view, record, summary info, transforms, import/export
void register_session_api(MsiManager& mgr, HandlerMap& map);   ///< install-session functions custom actions call
void register_feature_api(MsiManager& mgr, HandlerMap& map);   ///< feature/component/directory/costing session functions
void register_product_api(MsiManager& mgr, HandlerMap& map);   ///< installed product/feature/component/patch queries, source lists
void register_product_ops_api(MsiManager& mgr, HandlerMap& map); ///< install/configure/reinstall/advertise/patch operations, file/descriptor utilities, transactions
void register_ui_api(MsiManager& mgr, HandlerMap& map);        ///< internal/external UI, logging

// ---- Actions ---------------------------------------------------------------------

/// Codes an action (standard or custom) returns to the sequencer (Learn:
/// "Custom Action Return Values", "Using a Sequence Table").
enum ActionCode : u32 {
    kActionSuccess = 0,          ///< ERROR_SUCCESS
    kActionSkipRemaining = 259,  ///< ERROR_NO_MORE_ITEMS: skip the rest of the sequence
    kActionUserExit = 1602,      ///< ERROR_INSTALL_USEREXIT
    kActionFailure = 1603,       ///< ERROR_INSTALL_FAILURE
    kActionSuspend = 1604,       ///< ERROR_INSTALL_SUSPEND
    kActionNotCalled = 1626,     ///< ERROR_FUNCTION_NOT_CALLED
};

/// INSTALLMESSAGE (Msi.h): the high byte of a message type.
enum InstallMessage : u32 {
    kMsgFatalExit = 0x00000000, kMsgError = 0x01000000, kMsgWarning = 0x02000000,
    kMsgUser = 0x03000000, kMsgInfo = 0x04000000, kMsgFilesInUse = 0x05000000,
    kMsgResolveSource = 0x06000000, kMsgOutOfDiskSpace = 0x07000000,
    kMsgActionStart = 0x08000000, kMsgActionData = 0x09000000,
    kMsgProgress = 0x0A000000, kMsgCommonData = 0x0B000000,
    kMsgInitialize = 0x0C000000, kMsgTerminate = 0x0D000000,
    kMsgShowDialog = 0x0E000000, kMsgPerformance = 0x0F000000,
    kMsgRmFilesInUse = 0x19000000, kMsgInstallStart = 0x1A000000,
    kMsgInstallEnd = 0x1B000000,
};

/// A record as the engine passes it around (MsiCreateRecord's object). Field
/// 0 is the format template. The database area owns the full record API.
struct Record {
    enum class Kind : u8 { Null, Integer, String, Stream };
    struct Field {
        Kind kind{Kind::Null};
        i32 integer{0};
        std::u16string text;
        std::vector<u8> stream;
        usize stream_pos{0};
    };
    std::vector<Field> fields;  ///< fields[0] .. fields[count]
    explicit Record(u32 count = 0) : fields(count + 1) {}
    [[nodiscard]] u32 count() const { return static_cast<u32>(fields.size() - 1); }
};

/// One deferred operation of the execution script (Learn: "Execution
/// Scripts"). Actions sequenced between InstallInitialize and
/// InstallFinalize do their work by scheduling ops; InstallFinalize runs
/// them in order, and on failure runs the rollback ops of what already ran,
/// newest first. Commit ops run after a successful script.
struct ScriptOp {
    std::string action;                      ///< ActionStart name for logs/UI
    std::function<u32()> run;                ///< returns an ActionCode
    std::function<void()> rollback;          ///< may be empty
    bool commit_only{false};                 ///< a commit custom action
    bool rollback_only{false};               ///< a rollback custom action
};

/// What actions use beyond the session itself. Filled by whoever runs the
/// engine (the msiexec process, or a test).
struct Services {
    capsule::Vfs* vfs{nullptr};
    capsule::Registry* registry{nullptr};
    MsiManager* manager{nullptr};
    class CustomActionHost* custom_actions{nullptr};
    class UiHost* ui{nullptr};
    bool guest_64bit{true};
    /// Starts a custom-action server (msiexec.exe -Embedding @p token) of
    /// @p x64's bitness; @p elevated for deferred no-impersonate actions,
    /// which run in the system context (Learn "Custom Action Security").
    /// Empty: this process's custom-action host starts every server. The
    /// installer service routes impersonated servers to its client.
    std::function<bool(bool x64, bool elevated, const std::u16string& token)> launch_custom_action_server;
    /// The client ran the UI sequence before handing the execute sequence to
    /// this engine (FirstSequence actions are skipped then).
    bool client_ran_ui_sequence{false};
    /// Run the execute sequence in the installer service (a client engine
    /// whose UI sequence ran here): @p server is the session ExecuteAction
    /// built. Empty: run it in this process.
    std::function<u32(Session& server, const std::string& table)> execute_in_service;
};

class ActionContext {
public:
    ActionContext(Session& session, Services& services) : m_session(session), m_services(services) {}
    [[nodiscard]] Session& session() { return m_session; }
    [[nodiscard]] Services& services() { return m_services; }
    /// True while the sequencer is between InstallInitialize and
    /// InstallFinalize: system changes must be scheduled, not done.
    [[nodiscard]] bool scripting() const;
    void schedule(ScriptOp op);
    /// MsiProcessMessage from inside the engine: to the UI / external
    /// handler / log. Returns the handler's IDOK/IDCANCEL/... or 0.
    i32 message(u32 type, const Record& record);

private:
    Session& m_session;
    Services& m_services;
};

using ActionFn = std::function<u32(ActionContext&)>;

/// Standard actions by name. The sequencer looks an action up here first,
/// then in the CustomAction table, then (UI sequence) in the Dialog table.
class ActionRegistry {
public:
    void add(std::string name, ActionFn fn) { m_actions[std::move(name)] = std::move(fn); }
    [[nodiscard]] const ActionFn* find(const std::string& name) const {
        const auto it = m_actions.find(name);
        return it == m_actions.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const std::unordered_map<std::string, ActionFn>& all() const { return m_actions; }

private:
    std::unordered_map<std::string, ActionFn> m_actions;
};

// One action registrar per area. The engine calls all of them once.
void register_engine_actions(ActionRegistry& r);        ///< LaunchConditions, InstallInitialize/Finalize, InstallValidate, ScheduleReboot, ForceReboot, ...
void register_feature_actions(ActionRegistry& r);       ///< CostInitialize, FileCost, CostFinalize, IsolateComponents, MigrateFeatureStates, ...
void register_file_actions(ActionRegistry& r);          ///< InstallFiles, RemoveFiles, MoveFiles, DuplicateFiles, CreateFolders, ...
void register_search_actions(ActionRegistry& r);        ///< AppSearch, CCPSearch, RMCCPSearch, registry/ini/env/ODBC/service actions
void register_registration_actions(ActionRegistry& r);  ///< COM/shell/shortcut/font registration, product publication, upgrades

/// Run one CustomAction-table row (msi_customaction.cpp): every type and
/// option bit; deferred/rollback/commit types schedule themselves.
u32 run_custom_action(ActionContext& ctx, const std::string& action, u32 type,
                      const std::string& source, const std::string& target);

// ---- Host interfaces ---------------------------------------------------------------

/// The guest process the engine runs in (msiexec.exe, a runtime system
/// program): what custom actions need from it. Implemented by the
/// custom-action area.
class CustomActionHost {
public:
    virtual ~CustomActionHost() = default;
    /// Bitness of this process; a custom action of the other bitness needs a
    /// separate custom-action server.
    [[nodiscard]] virtual bool is_64bit() const = 0;
    /// Write @p bytes to a temporary file and LoadLibraryW it. 0 on failure.
    virtual u64 load_library_from_bytes(const std::vector<u8>& bytes, std::u16string& temp_path) = 0;
    virtual u64 load_library(const std::u16string& path) = 0;
    virtual u64 get_proc_address(u64 module, const std::string& name) = 0;
    virtual void free_library(u64 module) = 0;
    /// Call `UINT __stdcall entry(MSIHANDLE hInstall)` on a guest thread.
    virtual u32 call_dll_entry(u64 entry, u32 install_handle) = 0;
    /// CreateProcessW + optional wait; returns the exit code (or 0 when not
    /// waiting) and false when the process could not be created.
    virtual bool run_process(const std::u16string& application, const std::u16string& command_line,
                             const std::u16string& directory, bool wait, u32& exit_code) = 0;
};

/// The installer's internal UI (msi_ui.cpp), presented with native windows.
class UiHost {
public:
    virtual ~UiHost() = default;
    /// The UI level in effect (INSTALLUILEVEL_*).
    [[nodiscard]] virtual u32 level() const = 0;
    /// Show a Dialog-table dialog modally (a UI-sequence action that names a
    /// dialog). Returns the ActionCode the dialog's EndDialog produced.
    virtual u32 run_dialog(Session& session, const std::string& dialog) = 0;
    /// A message for the UI (progress, action start/data, errors).
    virtual i32 message(Session& session, u32 type, const Record& record) = 0;
};

}  // namespace vitreo::winapi::msi
