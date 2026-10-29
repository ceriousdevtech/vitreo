// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file scm.hpp
/// @brief advapi32 Service Control Manager — real services, not fake handles.
///
/// Vitreo runs Windows services as what they are: separate guest processes.
/// A controller process (an installer, an updater) creates a service, which
/// writes a real entry under
/// `HKLM\SYSTEM\CurrentControlSet\Services\<name>`, and starts it, which
/// launches a child Vitreo worker for the service's ImagePath. That child
/// discovers it is a service through the VITREO_SERVICE_NAME environment
/// variable, so `StartServiceCtrlDispatcher` can call the guest's ServiceMain
/// — and can honestly fail with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT when
/// the binary was run directly, exactly as Windows does.
///
/// Status flows the other way through a capsule-local status file per service
/// (`<capsule>/runtime/services/<name>.status`), written by SetServiceStatus in
/// the service process and read by QueryServiceStatus/Ex in any controller.
/// The pattern mirrors ipc/process_registry: a small versioned record plus a
/// liveness check on the recorded pid, so a service whose process died reports
/// SERVICE_STOPPED rather than a stale RUNNING.
///
/// Known limitations, deliberately visible rather than faked:
///   * ControlService(SERVICE_CONTROL_STOP) stops the service by terminating
///     its process; the guest's control handler does not receive the request
///     first. The status published to QueryServiceStatus/Ex is
///     SERVICE_STOPPED, which is the controller-visible outcome of a
///     completed stop. A service relying on graceful-shutdown work will
///     not run it.
///   * Access masks are recorded but not enforced — a capsule has one
///     principal. Handles are still typed and validated.
///   * Start types are recorded; nothing auto-starts services at boot, since a
///     capsule has no boot.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule {
class Registry;
class Vfs;
}

namespace vitreo::ipc {
class ProcessSession;
}

namespace vitreo::winapi {

class Marshaler;

/// Windows service constants (winsvc.h).
namespace service_constants {

// Service types.
constexpr u32 SERVICE_KERNEL_DRIVER       = 0x00000001;
constexpr u32 SERVICE_FILE_SYSTEM_DRIVER  = 0x00000002;
constexpr u32 SERVICE_WIN32_OWN_PROCESS   = 0x00000010;
constexpr u32 SERVICE_WIN32_SHARE_PROCESS = 0x00000020;
constexpr u32 SERVICE_INTERACTIVE_PROCESS = 0x00000100;

// Start types.
constexpr u32 SERVICE_BOOT_START   = 0;
constexpr u32 SERVICE_SYSTEM_START = 1;
constexpr u32 SERVICE_AUTO_START   = 2;
constexpr u32 SERVICE_DEMAND_START = 3;
constexpr u32 SERVICE_DISABLED     = 4;

// Current states.
constexpr u32 SERVICE_STOPPED          = 0x00000001;
constexpr u32 SERVICE_START_PENDING    = 0x00000002;
constexpr u32 SERVICE_STOP_PENDING     = 0x00000003;
constexpr u32 SERVICE_RUNNING          = 0x00000004;
constexpr u32 SERVICE_CONTINUE_PENDING = 0x00000005;
constexpr u32 SERVICE_PAUSE_PENDING    = 0x00000006;
constexpr u32 SERVICE_PAUSED           = 0x00000007;

// Controls.
constexpr u32 SERVICE_CONTROL_STOP        = 0x00000001;
constexpr u32 SERVICE_CONTROL_PAUSE       = 0x00000002;
constexpr u32 SERVICE_CONTROL_CONTINUE    = 0x00000003;
constexpr u32 SERVICE_CONTROL_INTERROGATE = 0x00000004;
constexpr u32 SERVICE_CONTROL_SHUTDOWN    = 0x00000005;

// Controls accepted.
constexpr u32 SERVICE_ACCEPT_STOP     = 0x00000001;
constexpr u32 SERVICE_ACCEPT_SHUTDOWN = 0x00000004;

// SC_STATUS_TYPE.
constexpr u32 SC_STATUS_PROCESS_INFO = 0;

/// Size of SERVICE_STATUS: seven DWORDs, identical on x86 and x64.
constexpr u32 SERVICE_STATUS_SIZE = 28;
/// Size of SERVICE_STATUS_PROCESS: SERVICE_STATUS + dwProcessId + dwServiceFlags.
constexpr u32 SERVICE_STATUS_PROCESS_SIZE = 36;

} // namespace service_constants

/// One service's persistent configuration, as stored in the registry.
struct ServiceConfig {
    std::wstring name;
    std::wstring display_name;
    std::wstring image_path;      ///< ImagePath, may include arguments.
    std::wstring object_name;     ///< ObjectName (account); "LocalSystem".
    u32 service_type{service_constants::SERVICE_WIN32_OWN_PROCESS};
    u32 start_type{service_constants::SERVICE_DEMAND_START};
    u32 error_control{0};
};

/// A service's live status, as SERVICE_STATUS_PROCESS models it.
struct ServiceStatus {
    u32 service_type{service_constants::SERVICE_WIN32_OWN_PROCESS};
    u32 current_state{service_constants::SERVICE_STOPPED};
    u32 controls_accepted{0};
    u32 win32_exit_code{0};
    u32 service_specific_exit_code{0};
    u32 check_point{0};
    u32 wait_hint{0};
    u32 process_id{0};
};

/// One action from SERVICE_FAILURE_ACTIONS.
struct ServiceFailureAction {
    u32 type{0};
    u32 delay_ms{0};
};

/// Optional service configuration captured from ChangeServiceConfig2.
///
/// A disengaged string means the caller supplied NULL and the stored value
/// must remain unchanged. A present empty string means delete the value.
/// A disengaged actions vector likewise preserves the existing reset period
/// and action array.
struct ServiceFailureActionsUpdate {
    u32 reset_period_seconds{0};
    std::optional<std::wstring> reboot_message;
    std::optional<std::wstring> command;
    std::optional<std::vector<ServiceFailureAction>> actions;
};

/// The capsule's Service Control Manager.
///
/// One instance lives in every Vitreo process. In a controller process it
/// drives the create/start/query/control/delete side; in a service process it
/// additionally holds the dispatcher state that backs
/// StartServiceCtrlDispatcher / RegisterServiceCtrlHandler / SetServiceStatus.
class ScmManager {
public:
    /// Run ServiceMain on a newly-created guest thread and block until that
    /// thread exits. StartServiceCtrlDispatcher itself remains on the calling
    /// thread as the service control dispatcher.
    using ServiceThreadCall = std::function<bool(
        GuestAddr, const std::vector<std::wstring>&, bool, bool)>;
    using KernelDriverStart =
        std::function<bool(const std::wstring&, const std::wstring&,
                           const std::string&, u32&)>;
    using KernelDriverStop = std::function<void(const std::wstring&)>;
    using KernelDriverRunning = std::function<bool(const std::wstring&)>;

    ScmManager(capsule::Registry& registry, capsule::Vfs& vfs);

    ScmManager(const ScmManager&) = delete;
    ScmManager& operator=(const ScmManager&) = delete;

    // ---- Wiring -------------------------------------------------------------

    /// Capsule identity: the host root (for status files) and the capsule name
    /// (passed to launched service workers).
    void set_capsule(std::string capsule_root, std::string capsule_name);

    /// Install the Windows services that are implemented by Vitreo itself.
    /// Call this after the capsule registry has been loaded so the entries are
    /// visible both through SCM APIs and direct registry queries.
    void install_builtin_services();

    /// Session that owns launched service processes. Without it StartService
    /// fails rather than pretending a service started.
    void set_process_session(ipc::ProcessSession* session) noexcept {
        m_process_session = session;
    }

    /// Native device boundary used for SERVICE_KERNEL_DRIVER images. Vitreo
    /// cannot execute NT kernel images as user processes.
    void set_kernel_driver_callbacks(KernelDriverStart start,
                                     KernelDriverStop stop,
                                     KernelDriverRunning running) {
        m_kernel_driver_start = std::move(start);
        m_kernel_driver_stop = std::move(stop);
        m_kernel_driver_running = std::move(running);
    }

    /// Bind the runtime boundary that creates and joins the ServiceMain guest
    /// thread.
    void set_service_thread_call(ServiceThreadCall call) {
        m_service_thread_call = std::move(call);
    }

    /// Runtime wait between status checks; false cancels on process shutdown.
    /// The runtime owns the blocking boundary, SCM owns reported service state.
    using ServiceDispatchWait = std::function<bool()>;
    void set_service_dispatch_wait(ServiceDispatchWait wait) {
        m_service_dispatch_wait = std::move(wait);
    }

    /// Decode the internal UTF-16LE transport used for StartService arguments.
    /// The decoded strings are delivered to ServiceMain after argv[0].
    [[nodiscard]] bool set_own_service_arguments_utf16(
        const std::vector<std::string>& encoded_arguments);

    /// Current directory inherited by launched service processes.
    void set_current_directory(std::string directory) {
        m_current_directory = std::move(directory);
    }

    /// True when this process was launched as a service (VITREO_SERVICE_NAME is
    /// set). StartServiceCtrlDispatcher must fail when this is false.
    [[nodiscard]] bool launched_as_service() const noexcept {
        return !m_own_service_name.empty();
    }

    /// The service name this process was launched as, empty if none.
    [[nodiscard]] const std::wstring& own_service_name() const noexcept {
        return m_own_service_name;
    }

    // ---- Controller side ----------------------------------------------------

    /// Create a service and persist it. Sets @p error and returns false when a
    /// service of that name already exists or the name is invalid.
    [[nodiscard]] bool create_service(const ServiceConfig& config, u32& error);

    /// True when the named service exists in the database.
    [[nodiscard]] bool service_exists(const std::wstring& name);

    /// Read a service's configuration, or nullopt when it does not exist.
    [[nodiscard]] std::optional<ServiceConfig> query_config(
        const std::wstring& name);

    /// Enumerate the persistent service database in case-insensitive name
    /// order, matching the stable ordering required by resume handles.
    [[nodiscard]] std::vector<ServiceConfig> enumerate_services();

    /// Update the mutable parts of a service's configuration. Fields left
    /// empty / set to SERVICE_NO_CHANGE by the caller are preserved.
    [[nodiscard]] bool change_config(const ServiceConfig& config, u32& error);

    /// Update optional configuration stored by ChangeServiceConfig2.
    [[nodiscard]] bool change_description(
        const std::wstring& name,
        const std::optional<std::wstring>& description,
        u32& error);
    [[nodiscard]] bool change_failure_actions(
        const std::wstring& name,
        const ServiceFailureActionsUpdate& update,
        u32& error);

    /// Read or replace the service's self-relative security descriptor. When
    /// none has been stored, query returns the capsule's default NULL-DACL
    /// descriptor.
    [[nodiscard]] std::optional<std::vector<u8>> query_security_descriptor(
        const std::wstring& name);
    [[nodiscard]] bool set_security_descriptor(
        const std::wstring& name, const std::vector<u8>& descriptor,
        u32& error);

    /// Remove a service from the database.
    [[nodiscard]] bool delete_service(const std::wstring& name, u32& error);

    /// Launch the service's process and wait until StartServiceCtrlDispatcher
    /// has created the ServiceMain thread, matching StartServiceW. @p args are
    /// the extra arguments from StartService, which Windows passes to
    /// ServiceMain after argv[0].
    [[nodiscard]] bool start_service(const std::wstring& name,
                                     const std::vector<std::wstring>& args,
                                     u32& error);

    /// Read a service's live status. A service whose process is gone reads back
    /// as SERVICE_STOPPED.
    [[nodiscard]] ServiceStatus query_status(const std::wstring& name);

    /// Deliver a control code. See the file header for the STOP limitation.
    [[nodiscard]] bool control_service(const std::wstring& name, u32 control,
                                       ServiceStatus& status_out, u32& error);

    // ---- Handle table -------------------------------------------------------

    /// Allocate an SCM-database handle.
    [[nodiscard]] u32 open_manager_handle();
    /// Allocate a handle bound to @p name.
    [[nodiscard]] u32 open_service_handle(const std::wstring& name);
    /// Resolve a service handle to its name, or nullopt when @p handle is not a
    /// live service handle (an SCM handle resolves to nullopt too).
    [[nodiscard]] std::optional<std::wstring> handle_service_name(u32 handle);

    /// Everything QueryServiceConfig2 reports, from the service's registry
    /// key (Description, FailureActions, DelayedAutostart,
    /// FailureActionsOnNonCrashFailures, ServiceSidType, RequiredPrivileges,
    /// PreshutdownTimeout, LaunchProtected).
    struct ServiceConfig2 {
        std::optional<std::wstring> description;
        u32 reset_period{0};
        std::wstring reboot_message;
        std::wstring command;
        std::vector<std::pair<u32, u32>> actions; ///< {type, delay}
        u32 delayed_auto_start{0};
        u32 failure_actions_flag{0};
        u32 service_sid_type{0};
        std::vector<std::wstring> required_privileges;
        u32 preshutdown_timeout{10000};
        u32 launch_protected{0};
    };
    [[nodiscard]] std::optional<ServiceConfig2> query_config2(const std::wstring& name);
    /// True when @p handle is a live SCM-database handle.
    [[nodiscard]] bool is_manager_handle(u32 handle);
    /// Release either kind of handle. False when it was not open.
    bool close_handle(u32 handle);

    // ---- Service side -------------------------------------------------------

    /// Run the guest's ServiceMain for this process's service, then remain
    /// connected until it explicitly reports SERVICE_STOPPED. @p entry_name is
    /// the name from the guest's SERVICE_TABLE_ENTRY (used for argv[0] when the
    /// process has no name of its own).
    [[nodiscard]] bool run_dispatcher(GuestAddr service_main,
                                      const std::wstring& entry_name,
                                      bool is_64bit, bool wide, u32& error);

    /// Register a control handler; returns the SERVICE_STATUS_HANDLE, or 0 when
    /// this process is not a service.
    [[nodiscard]] u32 register_control_handler(GuestAddr handler,
                                               GuestAddr context, bool is_ex);

    /// Publish a status update from the service process.
    [[nodiscard]] bool set_status(u32 status_handle, const ServiceStatus& status);

private:
    friend void register_scm_service_side_exports(ScmManager&, DllExports&);
    [[nodiscard]] bool begin_dispatcher(u32& error);
    [[nodiscard]] bool run_connected_dispatcher(GuestAddr service_main,
        const std::wstring& entry_name, bool is_64bit, bool wide, u32& error);
    /// True for a Windows service whose behavior is provided by the Vitreo
    /// runtime rather than a guest service executable.
    [[nodiscard]] static bool is_builtin_native_service(
        const std::wstring& name);

    /// Registry path of a service's key, relative to HKLM.
    [[nodiscard]] static std::wstring service_key_path(const std::wstring& name);
    /// Host path of a service's status file.
    [[nodiscard]] std::string status_file_path(const std::wstring& name) const;

    [[nodiscard]] bool write_status_file(const std::wstring& name,
                                         const ServiceStatus& status) const;
    [[nodiscard]] std::optional<ServiceStatus> read_status_file(
        const std::wstring& name) const;
    void remove_status_file(const std::wstring& name) const;

    capsule::Registry& m_registry;
    capsule::Vfs& m_vfs;
    ipc::ProcessSession* m_process_session{nullptr};
    ServiceThreadCall m_service_thread_call;
    ServiceDispatchWait m_service_dispatch_wait;
    KernelDriverStart m_kernel_driver_start;
    KernelDriverStop m_kernel_driver_stop;
    KernelDriverRunning m_kernel_driver_running;

    std::string m_capsule_root;
    std::string m_capsule_name{"default"};
    std::string m_current_directory;

    mutable std::mutex m_mutex;
    u32 m_next_handle{0x5C000001};
    /// Handle -> service name; an empty name marks an SCM-database handle.
    std::unordered_map<u32, std::wstring> m_handles;

    // Service-process state.
    std::wstring m_own_service_name;   ///< From VITREO_SERVICE_NAME.
    std::vector<std::wstring> m_own_service_arguments;
    u32 m_status_handle{0};
    GuestAddr m_handler_addr{0};
    GuestAddr m_handler_context{0};
    bool m_handler_is_ex{false};
    bool m_dispatcher_running{false};
};

/// Create the advapi32 Service Control Manager thunks.
DllExports create_advapi32_scm_thunks(ScmManager& scm);

} // namespace vitreo::winapi
