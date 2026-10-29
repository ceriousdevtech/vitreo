// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_service.cpp
/// @brief The Windows Installer client/service split (msi_service.hpp).

#include "winapi/system/msi/msi_service.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_search.hpp"
#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/advapi32/services/scm.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "ipc/unix_pipe.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <unistd.h>

namespace vitreo::winapi::msi {

namespace {

constexpr u32 kProtocolVersion = 1;
constexpr u32 kErrPackageOpenFailed = 1619;
constexpr u32 kErrPackageInvalid = 1620;
constexpr u32 kRebootRequired = 3010;  ///< ERROR_SUCCESS_REBOOT_REQUIRED

void write_identity(WireWriter& w, const ClientIdentity& id) {
    w.str(id.user_sid);
    w.str(id.logon_user);
    w.str(id.profile_dir);
    w.str(id.temp_dir);
    w.str(id.current_directory);
    w.u8_(id.admin_user ? 1 : 0);
    w.u8_(id.elevated ? 1 : 0);
    w.u32_(id.user_lcid);
    w.u32_(id.process_id);
}

ClientIdentity read_identity(WireReader& r) {
    ClientIdentity id;
    id.user_sid = r.str();
    id.logon_user = r.str();
    id.profile_dir = r.str();
    id.temp_dir = r.str();
    id.current_directory = r.str();
    id.admin_user = r.u8_() != 0;
    id.elevated = r.u8_() != 0;
    id.user_lcid = r.u32_();
    id.process_id = r.u32_();
    return id;
}

std::wstring to_wide(const std::string& utf8) {
    const std::vector<u16> units = utf8_to_utf16(utf8);
    std::wstring out;
    for (const u16 u : units) out.push_back(static_cast<wchar_t>(u));
    return out;
}

struct ConnectorRegistry {
    std::mutex mutex;
    std::unordered_map<MsiManager*, ServiceConnector> connectors;
};
ConnectorRegistry& connectors() {
    static ConnectorRegistry r;
    return r;
}

ServiceConnector connector_of(MsiManager& mgr) {
    auto& r = connectors();
    std::lock_guard lock(r.mutex);
    const auto it = r.connectors.find(&mgr);
    return it == r.connectors.end() ? ServiceConnector{} : it->second;
}

/// The service's per-install client UI relay: a message the client handles
/// goes there, the rest is answered 0 here.
i32 relay_message(RemoteChannel& channel, u32 filter, u32 type, const Record* record, const std::string& text) {
    if ((filter & log_mode_bit(type)) == 0) return 0;
    WireWriter w;
    w.u32_(type);
    w.u8_(record != nullptr ? 1 : 0);
    if (record != nullptr) w.record(*record);
    w.str(text);
    if (!channel.send(Wire::UiMessage, w.data())) return 0;
    auto reply = channel.receive();
    if (!reply || reply->kind != Wire::UiReply) return 0;
    WireReader r(reply->body);
    return static_cast<i32>(r.u32_());
}

}  // namespace

// ---- Request codec ------------------------------------------------------------------

std::vector<u8> encode_install_request(const InstallRequest& q) {
    WireWriter w;
    w.u32_(kProtocolVersion);
    w.u32_(static_cast<u32>(q.kind));
    w.str(q.package_path);
    w.str(q.action);
    w.str(q.table);
    w.string_map(q.properties);
    w.u32_(q.ui_level);
    w.u32_(q.message_filter);
    w.str(q.log_file);
    w.u32_(q.log_mode);
    w.u8_(q.log_append ? 1 : 0);
    w.u8_(q.log_flush ? 1 : 0);
    write_identity(w, q.identity);
    return std::move(w.data());
}

std::optional<InstallRequest> decode_install_request(std::span<const u8> body) {
    WireReader r(body);
    if (r.u32_() != kProtocolVersion) return std::nullopt;
    InstallRequest q;
    const u32 kind = r.u32_();
    if (kind != static_cast<u32>(InstallRequest::Kind::Install) &&
        kind != static_cast<u32>(InstallRequest::Kind::ExecuteSequence)) {
        return std::nullopt;
    }
    q.kind = static_cast<InstallRequest::Kind>(kind);
    q.package_path = r.str();
    q.action = r.str();
    q.table = r.str();
    q.properties = r.string_map();
    q.ui_level = r.u32_();
    q.message_filter = r.u32_();
    q.log_file = r.str();
    q.log_mode = r.u32_();
    q.log_append = r.u8_() != 0;
    q.log_flush = r.u8_() != 0;
    q.identity = read_identity(r);
    if (!r.done()) return std::nullopt;
    return q;
}

// ---- Client ----------------------------------------------------------------------------

InstallResult run_in_service(RemoteChannel& channel, const InstallRequest& request, const InstallClient& client) {
    InstallResult broken;
    broken.rc = kErrInstallServiceFailure;
    if (!channel.send(Wire::InstallRequest, encode_install_request(request))) return broken;
    for (;;) {
        auto frame = channel.receive(client.idle ? 50 : -1);
        if (!frame) {
            if (channel.timed_out()) {
                client.idle();
                continue;
            }
            return broken;
        }
        WireReader r(frame->body);
        switch (frame->kind) {
            case Wire::UiMessage: {
                const u32 type = r.u32_();
                std::optional<Record> record;
                if (r.u8_() != 0) record = r.record();
                const std::string text = r.str();
                const i32 answer = client.message ? client.message(type, record ? &*record : nullptr, text) : 0;
                WireWriter w;
                w.u32_(static_cast<u32>(answer));
                if (!channel.send(Wire::UiReply, w.data())) return broken;
                break;
            }
            case Wire::LaunchServer: {
                const bool x64 = r.u8_() != 0;
                const std::u16string token = r.u16str();
                const bool ok = r.ok() && client.launch_server && client.launch_server(x64, token);
                WireWriter w;
                w.u8_(ok ? 1 : 0);
                if (!channel.send(Wire::LaunchReply, w.data())) return broken;
                break;
            }
            case Wire::InstallDone: {
                InstallResult result;
                result.rc = r.u32_();
                result.reboot_at_end = r.u8_() != 0;
                result.reboot_now = r.u8_() != 0;
                return r.ok() ? result : broken;
            }
            default:
                return broken;
        }
    }
}

// ---- Service ---------------------------------------------------------------------------

InstallResult serve_install_request(MsiManager& mgr, RemoteChannel& channel, const InstallRequest& q) {
    InstallResult result;
    // The installing user's facts (Learn "Installation Context": per-user
    // folders, UserSID and LogonUser are the client's); the rest of the
    // machine state, and %environment lookups, are this service's.
    MachineState machine = mgr.machine_state();
    machine.user_sid = q.identity.user_sid;
    machine.logon_user = q.identity.logon_user;
    if (!q.identity.profile_dir.empty()) machine.profile_dir = q.identity.profile_dir;
    if (!q.identity.temp_dir.empty()) machine.temp_dir = q.identity.temp_dir;
    if (!q.identity.current_directory.empty()) machine.current_directory = q.identity.current_directory;
    machine.admin_user = q.identity.admin_user;
    machine.elevated = q.identity.elevated;
    if (q.identity.user_lcid != 0) machine.user_lcid = q.identity.user_lcid;
    mgr.set_machine_state(machine);
    if (!q.identity.user_sid.empty()) mgr.set_user_sid(to_wide(q.identity.user_sid));

    auto bytes = mgr.engine_read_file(q.package_path);
    if (!bytes) {
        result.rc = kErrPackageOpenFailed;
        return result;
    }
    auto opened = Package::open(std::span<const u8>(bytes->data(), bytes->size()));
    if (!opened) {
        result.rc = kErrPackageInvalid;
        return result;
    }

    Engine::Options o;
    o.package_path = q.package_path;
    o.ui_level = q.ui_level;
    o.allow_system_changes = true;
    o.server = true;
    if (q.kind == InstallRequest::Kind::Install) o.command_line = q.properties;
    o.log_file = q.log_file;
    o.log_mode = q.log_mode;
    o.log_append = q.log_append;
    o.log_flush = q.log_flush;

    Services services = mgr.engine_services();
    services.ui = nullptr;  // the client presents the UI
    services.client_ran_ui_sequence = q.kind == InstallRequest::Kind::ExecuteSequence;
    CustomActionProcess* host = process_custom_action_host();
    services.launch_custom_action_server = [&channel, host](bool x64, bool elevated, const std::u16string& token) {
        if (elevated) return host != nullptr && host->start_custom_action_server(x64, token);
        // Impersonated: the client starts it, as the installing user.
        WireWriter w;
        w.u8_(x64 ? 1 : 0);
        w.u16str(token);
        if (!channel.send(Wire::LaunchServer, w.data())) return false;
        auto reply = channel.receive();
        return reply && reply->kind == Wire::LaunchReply && !reply->body.empty() && reply->body[0] == 1;
    };
    const u32 filter = q.message_filter;
    set_client_ui_relay(mgr, [&channel, filter](u32 type, const Record* record, const std::string& text) {
        return relay_message(channel, filter, type, record, text);
    });

    u32 error = 0;
    auto engine = Engine::open(mgr, std::make_shared<Package>(std::move(*opened)), o, services, error);
    if (!engine) {
        set_client_ui_relay(mgr, nullptr);
        result.rc = error;
        return result;
    }
    if (q.kind == InstallRequest::Kind::Install) {
        result.rc = engine->run_top_level(q.action.empty() ? std::string("INSTALL") : q.action);
        (void)finish_async_custom_actions(engine->session());
    } else {
        // The server session ExecuteAction built in the client.
        Session& session = engine->session();
        for (const auto& [name, value] : q.properties) session.set_property(name, value);
        engine->log(log_mode_bit(kMsgInfo), "Switching to server session for " + q.table);
        result.rc = engine->run_sequence(q.table);
        engine->log_property_dump();
        (void)finish_async_custom_actions(session);
        engine->flush_log();
    }
    result.reboot_at_end = engine->get_mode(kRunRebootAtEnd);
    result.reboot_now = engine->get_mode(kRunRebootNow);
    // A reboot the install asked for and did not perform (Learn "Error
    // Codes": ERROR_SUCCESS_REBOOT_REQUIRED).
    if (q.kind == InstallRequest::Kind::Install && result.rc == 0 && (result.reboot_at_end || result.reboot_now)) {
        result.rc = kRebootRequired;
    }
    set_client_ui_relay(mgr, nullptr);
    return result;
}

std::string installer_service_pipe(u32 pid) { return "\\\\.\\pipe\\Vitreo.MsiServer." + std::to_string(pid); }

void run_installer_service(MsiManager& mgr, const std::function<void()>& listening,
                           const std::function<bool()>& stop, const std::function<void(bool)>& bracket,
                           u32 idle_ms) {
    auto listener = ipc::UnixPipe::create_server(installer_service_pipe(static_cast<u32>(::getpid())));
    if (!listener) return;
    struct Queue {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::pair<std::shared_ptr<RemoteChannel>, InstallRequest>> items;
        bool busy{false};
        bool done{false};
    } queue;
    if (listening) listening();
    // Clients connect on a host thread; installs run on this (guest) thread.
    std::thread acceptor([&] {
        for (;;) {
            {
                std::lock_guard lock(queue.mutex);
                if (queue.done) return;
            }
            auto pipe = listener->accept(200);
            if (!pipe) continue;
            auto channel = std::make_shared<RemoteChannel>(pipe);
            auto frame = channel->receive(30000);
            std::optional<InstallRequest> request;
            if (frame && frame->kind == Wire::InstallRequest) request = decode_install_request(frame->body);
            if (!request) {
                WireWriter w;
                w.u32_(kErrInstallServiceFailure);
                w.u8_(0);
                w.u8_(0);
                (void)channel->send(Wire::InstallDone, w.data());
                continue;
            }
            std::lock_guard lock(queue.mutex);
            if (queue.busy || !queue.items.empty()) {
                // Learn "_MSIExecute Mutex": one installation at a time.
                WireWriter w;
                w.u32_(kErrInstallAlreadyRunning);
                w.u8_(0);
                w.u8_(0);
                (void)channel->send(Wire::InstallDone, w.data());
                continue;
            }
            queue.items.emplace_back(channel, std::move(*request));
            queue.cv.notify_one();
        }
    });
    auto last_activity = std::chrono::steady_clock::now();
    for (;;) {
        std::shared_ptr<RemoteChannel> channel;
        InstallRequest request;
        {
            std::unique_lock lock(queue.mutex);
            queue.cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !queue.items.empty(); });
            if (queue.items.empty()) {
                const auto idle = std::chrono::steady_clock::now() - last_activity;
                if ((stop && stop()) || idle > std::chrono::milliseconds(idle_ms)) {
                    queue.done = true;
                    break;
                }
                continue;
            }
            channel = queue.items.front().first;
            request = std::move(queue.items.front().second);
            queue.items.pop_front();
            queue.busy = true;
        }
        if (bracket) bracket(true);
        const InstallResult result = serve_install_request(mgr, *channel, request);
        if (bracket) bracket(false);
        WireWriter w;
        w.u32_(result.rc);
        w.u8_(result.reboot_at_end ? 1 : 0);
        w.u8_(result.reboot_now ? 1 : 0);
        (void)channel->send(Wire::InstallDone, w.data());
        {
            std::lock_guard lock(queue.mutex);
            queue.busy = false;
        }
        last_activity = std::chrono::steady_clock::now();
    }
    acceptor.join();
}

void set_installer_service_connector(MsiManager& mgr, ServiceConnector connector) {
    auto& r = connectors();
    std::lock_guard lock(r.mutex);
    if (connector) r.connectors[&mgr] = std::move(connector);
    else r.connectors.erase(&mgr);
}

ServiceConnector scm_service_connector() {
    return [](u32& error) -> std::shared_ptr<RemoteChannel> {
        using namespace service_constants;
        error = kErrInstallServiceFailure;
        ScmManager* scm = service_control_manager();
        if (scm == nullptr) return nullptr;
        // Learn "Windows Installer Service": the client starts the service
        // on demand (msiserver, SERVICE_DEMAND_START).
        const std::wstring name = L"msiserver";
        ServiceStatus status = scm->query_status(name);
        if (status.current_state == SERVICE_STOPPED) {
            u32 start_error = 0;
            constexpr u32 kAlreadyRunning = 1056;  // ERROR_SERVICE_ALREADY_RUNNING
            if (!scm->start_service(name, {}, start_error) && start_error != kAlreadyRunning) {
                std::fprintf(stderr, "[msi] the Windows Installer service did not start (error %u)\n", start_error);
                return nullptr;
            }
        }
        for (int waited = 0; waited < 120000; waited += 50) {
            status = scm->query_status(name);
            if (status.current_state == SERVICE_RUNNING && status.process_id != 0) break;
            if (status.current_state == SERVICE_STOPPED && waited > 5000) return nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (status.current_state != SERVICE_RUNNING || status.process_id == 0) return nullptr;
        const std::string pipe_name = installer_service_pipe(status.process_id);
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (auto pipe = ipc::UnixPipe::connect_client(pipe_name)) {
                error = 0;
                return std::make_shared<RemoteChannel>(pipe);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return nullptr;
    };
}

// ---- The client install ----------------------------------------------------------------

std::string absolute_package_path(MsiManager& mgr, const std::string& path) {
    const bool absolute = (path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
                          (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/'));
    if (absolute || path.empty()) return path;
    std::string dir = mgr.machine_state().current_directory;
    if (dir.empty()) return path;
    if (path[0] == '\\' || path[0] == '/') return dir.substr(0, 2) + path;  // drive-relative root
    if (dir.back() != '\\') dir += '\\';
    return dir + path;
}

void apply_enable_log(MsiManager& mgr, InstallInvocation& inv) {
    const LogRequest log = ui_process_state(mgr).log;
    if (!inv.log_file.empty() || !log.enabled || log.path.empty()) return;
    inv.log_file = log.path;
    inv.log_mode = log.mode;
    constexpr u32 kAppend = 1;      // INSTALLLOGATTRIBUTES_APPEND (Msi.h)
    constexpr u32 kFlushEachLine = 2;  // INSTALLLOGATTRIBUTES_FLUSHEACHLINE
    inv.log_append = (log.attributes & kAppend) != 0;
    inv.log_flush = (log.attributes & kFlushEachLine) != 0;
}

u32 install_package(MsiManager& mgr, const InstallInvocation& inv) {
    Services services = mgr.engine_services();
    const u32 base = inv.ui_level & 0xF;
    // INSTALLUILEVEL_DEFAULT (1), _REDUCED (4), _FULL (5) show the wizard when
    // this process has an internal UI.
    const bool wizard = services.ui != nullptr && (base == 4 || base == 5 || base == 1);
    Engine::Options o;
    o.package_path = inv.package_path;
    o.ui_level = inv.ui_level;
    o.allow_system_changes = true;
    o.command_line = inv.properties;
    o.log_file = inv.log_file;
    o.log_mode = inv.log_mode;
    o.log_append = inv.log_append;
    o.log_flush = inv.log_flush;

    const ServiceConnector connector = connector_of(mgr);
    if (!connector) {
        // No installer service is reachable from this process (a manager
        // with no runtime around it): the engine runs here.
        o.server = !wizard;
        u32 error = 0;
        auto engine = Engine::open(mgr, inv.package, o, services, error);
        if (!engine) return error;
        u32 rc = engine->run_top_level(inv.action);
        (void)finish_async_custom_actions(engine->session());
        if (rc == 0 && (engine->get_mode(kRunRebootAtEnd) || engine->get_mode(kRunRebootNow))) rc = kRebootRequired;
        return rc;
    }

    InstallRequest request;
    request.package_path = inv.package_path;
    request.action = inv.action;
    request.ui_level = inv.ui_level;
    request.log_file = inv.log_file;
    request.log_mode = inv.log_mode;
    request.log_append = inv.log_append;
    request.log_flush = inv.log_flush;
    {
        const UiProcessState ui = ui_process_state(mgr);
        u32 filter = 0;
        if (ui.record_handler.function != 0) filter |= ui.record_handler.filter;
        if (ui.string_handler.function != 0) filter |= ui.string_handler.filter;
        constexpr u32 kUiNone = 2;  // INSTALLUILEVEL_NONE
        if (services.ui != nullptr && base != kUiNone) filter = 0xFFFFFFFFu;
        request.message_filter = filter;
        const MachineState machine = mgr.machine_state();
        request.identity.user_sid = machine.user_sid;
        request.identity.logon_user = machine.logon_user;
        request.identity.profile_dir = machine.profile_dir;
        request.identity.temp_dir = machine.temp_dir;
        request.identity.current_directory = machine.current_directory;
        request.identity.admin_user = machine.admin_user;
        request.identity.elevated = machine.elevated;
        request.identity.user_lcid = machine.user_lcid;
        request.identity.process_id = machine.process_id;
    }

    auto engine_slot = std::make_shared<std::shared_ptr<Engine>>();
    CustomActionProcess* host = process_custom_action_host();
    InstallClient client;
    UiHost* internal = services.ui;
    client.message = [&mgr, internal, engine_slot](u32 type, const Record* record, const std::string& text) -> i32 {
        Session* session = *engine_slot ? &(*engine_slot)->session() : nullptr;
        const RecordFormatter format = [text](const Record&) { return text; };
        return route_message(mgr, internal, session, type, record, format);
    };
    client.launch_server = [host](bool x64, const std::u16string& token) {
        return host != nullptr && host->start_custom_action_server(x64, token);
    };
    if (internal != nullptr && host != nullptr) client.idle = [host] { host->pump_messages(); };

    if (wizard) {
        // The UI sequence runs here; ExecuteAction hands the execute
        // sequence to the service with the server session's properties.
        o.server = false;
        services.execute_in_service = [connector, request, client, engine_slot](Session& server,
                                                                              const std::string& table) -> u32 {
            u32 error = 0;
            auto channel = connector(error);
            if (!channel) return kErrInstallServiceFailure;
            InstallRequest q = request;
            q.kind = InstallRequest::Kind::ExecuteSequence;
            q.table = table;
            q.properties = server.properties();
            q.log_append = true;  // the client's half of the log is already there
            const InstallResult result = run_in_service(*channel, q, client);
            if (*engine_slot) {
                if (result.reboot_at_end) (void)(*engine_slot)->set_mode(kRunRebootAtEnd, true);
                if (result.reboot_now) (void)(*engine_slot)->set_mode(kRunRebootNow, true);
            }
            return result.rc;
        };
        u32 error = 0;
        auto engine = Engine::open(mgr, inv.package, o, services, error);
        if (!engine) return error;
        *engine_slot = engine;
        u32 rc = engine->run_top_level(inv.action);
        (void)finish_async_custom_actions(engine->session());
        if (rc == 0 && (engine->get_mode(kRunRebootAtEnd) || engine->get_mode(kRunRebootNow))) rc = kRebootRequired;
        *engine_slot = nullptr;
        return rc;
    }

    // No wizard: the whole install runs in the service. The client keeps a
    // session for its internal UI (basic progress) and validates the package.
    o.server = false;
    o.allow_system_changes = false;
    o.log_file.clear();
    o.log_mode = 0;
    u32 error = 0;
    auto engine = Engine::open(mgr, inv.package, o, services, error);
    if (!engine) return error;
    *engine_slot = engine;
    request.kind = InstallRequest::Kind::Install;
    request.properties = inv.properties;
    auto channel = connector(error);
    if (!channel) {
        *engine_slot = nullptr;
        return kErrInstallServiceFailure;
    }
    const InstallResult result = run_in_service(*channel, request, client);
    *engine_slot = nullptr;
    return result.rc;
}

}  // namespace vitreo::winapi::msi
