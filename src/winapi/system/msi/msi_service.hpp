#pragma once
/// @file msi_service.hpp
/// @brief The Windows Installer client/service split (Learn "Windows
/// Installer Service", "Installation Context", "Custom Action Security",
/// MsiInstallProduct, MsiSetInternalUI, MsiSetExternalUI(Record),
/// MsiEnableLog; .docs/contracts/msi/process.md).
///
/// A client (msiexec.exe /i, or any process calling MsiInstallProduct,
/// MsiConfigureProduct(Ex), MsiReinstallProduct, ...) opens the package,
/// runs the UI sequence itself when its UI level shows wizard dialogs, and
/// hands the rest to the installer service (msiexec.exe /V, the capsule's
/// "msiserver" service): the whole install at basic or no UI, the execute
/// sequence (with the server session's properties) after a wizard UI.
/// Every message the service's engine posts comes back to the client, which
/// routes it through its own external handlers and internal UI and returns
/// their answer (IDCANCEL ends the install). Impersonated custom-action
/// servers are started by the client (they run as the installing user);
/// elevated ones by the service.

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi_remote.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace vitreo::winapi { class MsiManager; }

namespace vitreo::winapi::msi {

class Package;

/// The installing user: the per-user facts the service's session takes from
/// its client (Learn: per-user folders and UserSID are the installing
/// user's, also in the service).
struct ClientIdentity {
    std::string user_sid;
    std::string logon_user;
    std::string profile_dir;
    std::string temp_dir;
    std::string current_directory;
    bool admin_user{false};
    bool elevated{false};
    u32 user_lcid{0};
    u32 process_id{0};
};

/// One request a client sends the service.
struct InstallRequest {
    enum class Kind : u32 {
        Install = 1,          ///< the whole top-level action (no wizard UI ran)
        ExecuteSequence = 2,  ///< the execute sequence after the client's UI sequence
    };
    Kind kind{Kind::Install};
    std::string package_path;  ///< absolute
    std::string action;        ///< INSTALL / ADVERTISE / ADMIN
    std::string table;         ///< ExecuteSequence: the sequence table
    /// Install: the command-line properties. ExecuteSequence: the server
    /// session's properties ExecuteAction built in the client.
    std::map<std::string, std::string> properties;
    u32 ui_level{1};           ///< INSTALLUILEVEL_* with modifiers
    /// INSTALLLOGMODE bits (1 << (INSTALLMESSAGE >> 24)) the client handles;
    /// other messages are answered 0 without a round trip.
    u32 message_filter{0};
    std::string log_file;
    u32 log_mode{0};
    bool log_append{false};
    bool log_flush{false};
    ClientIdentity identity;
};

struct InstallResult {
    u32 rc{1603};
    bool reboot_at_end{false};
    bool reboot_now{false};
};

[[nodiscard]] std::vector<u8> encode_install_request(const InstallRequest& request);
[[nodiscard]] std::optional<InstallRequest> decode_install_request(std::span<const u8> body);

/// What the client does for the service during an install.
struct InstallClient {
    /// A message for the client's UI (@p record null: none); its answer.
    std::function<i32(u32 type, const Record* record, const std::string& text)> message;
    /// Start an impersonated custom-action server (msiexec.exe -Embedding).
    std::function<bool(bool x64, const std::u16string& token)> launch_server;
    /// Runs while the client waits with nothing to answer (its UI pump).
    std::function<void()> idle;
};

/// ERROR_INSTALL_SERVICE_FAILURE: the service could not be accessed.
inline constexpr u32 kErrInstallServiceFailure = 1601;
/// ERROR_INSTALL_ALREADY_RUNNING (_MSIExecute held by another install).
inline constexpr u32 kErrInstallAlreadyRunning = 1618;

/// Client: send @p request on @p channel and serve the service's requests
/// until it answers. A broken connection is 1601.
InstallResult run_in_service(RemoteChannel& channel, const InstallRequest& request, const InstallClient& client);

/// Service: run one decoded request with @p channel as its client.
InstallResult serve_install_request(MsiManager& mgr, RemoteChannel& channel, const InstallRequest& request);

/// The pipe the service whose process id is @p pid listens on.
[[nodiscard]] std::string installer_service_pipe(u32 pid);

/// The service's main loop (msiexec /V's ServiceMain thread): listen on
/// installer_service_pipe(getpid()), call @p listening once clients can
/// connect, then run each client's install on this thread, one at a time
/// (a client arriving meanwhile gets 1618). @p bracket(true/false) runs
/// around each install (the _MSIExecute mutex). Returns when @p stop says so
/// or after @p idle_ms without a client.
void run_installer_service(MsiManager& mgr, const std::function<void()>& listening,
                           const std::function<bool()>& stop, const std::function<void(bool)>& bracket,
                           u32 idle_ms);

/// How a client reaches the service: start "msiserver" through the capsule
/// SCM if needed and connect. Null when there is no SCM in this process
/// (unit tests): the install then runs in this process.
using ServiceConnector = std::function<std::shared_ptr<RemoteChannel>(u32& error)>;
void set_installer_service_connector(MsiManager& mgr, ServiceConnector connector);
/// The SCM-based connector (msi_search.hpp set_service_control_manager).
[[nodiscard]] ServiceConnector scm_service_connector();

/// One client install (msiexec /i, MsiInstallProduct & co.).
struct InstallInvocation {
    std::string package_path;  ///< absolute
    std::shared_ptr<Package> package;
    std::string action{"INSTALL"};
    std::map<std::string, std::string> properties;
    u32 ui_level{1};
    std::string log_file;
    u32 log_mode{0};
    bool log_append{false};
    bool log_flush{false};
};
/// Run @p invocation as the client: through the service when this process
/// can reach one, else in this process. Returns the install's result code
/// (3010 when a reboot was scheduled and not performed).
u32 install_package(MsiManager& mgr, const InstallInvocation& invocation);

/// @p path made absolute against the client's current directory (the
/// service resolves nothing relative to its own).
[[nodiscard]] std::string absolute_package_path(MsiManager& mgr, const std::string& path);

/// MsiEnableLog's request as engine options (Msi.h INSTALLLOGATTRIBUTES_APPEND
/// = 1, _FLUSHEACHLINE = 2); empty when logging is off.
void apply_enable_log(MsiManager& mgr, InstallInvocation& invocation);

}  // namespace vitreo::winapi::msi
