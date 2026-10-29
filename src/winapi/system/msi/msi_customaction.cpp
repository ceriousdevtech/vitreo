// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_customaction.cpp
/// @brief Windows Installer engine area: custom actions of every type and
/// option (msi_customaction.hpp). DLL actions run in custom-action server
/// processes (msiexec.exe -Embedding) of the DLL's bitness, their MSIHANDLE
/// calls relayed to this engine (msi_remote.hpp); EXE actions are created
/// from the engine process through its CustomActionProcess.
///
/// Behaviour Learn leaves open is measured by tools/oracle_probe/
/// msi_customaction_{dll,exe}.c + msi_customaction_build/run.ps1 on Windows 11
/// (x64 and x86 packages, msi_customaction_x64/x86.expected):
///   * a DLL / EXE from the Binary table is written to
///     %WINDIR%\Installer\MSIxxxx.tmp (GetTempFileName "MSI"), loaded or run
///     from there and deleted afterwards;
///   * a DLL's return value: 0 success, 1626 "not called" (the sequence goes
///     on), 259 skips the rest of the sequence (the install succeeds), 1602
///     user exit, 1604 suspend, 1603 and every other value (1, 1234...)
///     failure; a missing entry point or unloadable DLL is error 1723 and a
///     failure; the Continue bit turns all of these into success;
///   * an EXE's non-zero exit code is error 1722 and a failure (Continue:
///     success); EXE actions run with %WINDIR%\system32 as their current
///     directory unless the type names one (34);
///   * an asynchronous action (Async without Continue too) never changes the
///     install's result: its return value is only collected at the end;
///   * Async on a rollback action, and the in-script bit on a type 51
///     property action, are error 2723 "unsupported type"; basic types 0 and
///     4 (and bits outside the type fields) are error 2717; a deferred action
///     sequenced outside InstallInitialize..InstallFinalize is error 2762;
///     each fails the action whatever its Continue bit;
///   * type 19 shows the Error-table message (or the literal text) with no
///     fields and fails;
///   * deferred actions see CustomActionData, ProductCode and UserSID; a
///     commit action sees no ProductCode and MsiGetLanguage 0;
///   * FirstSequence and ClientRepeat actions are skipped in the execute
///     sequence (the server side) after the UI sequence ran; ClientRepeat is
///     skipped there in every measured case; OncePerProcess runs (the UI ran
///     in the client process, not the server's).

#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_database.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_remote.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>

#include <unistd.h>

namespace vitreo::winapi::msi {

namespace {

// Windows Installer error numbers posted with the failing action (Learn
// "Windows Installer Error Messages").
constexpr i32 kErrScriptFailed = 1720;     ///< a script required for this install could not be run
constexpr i32 kErrProgramNotRun = 1721;    ///< a program required for this install could not be run
constexpr i32 kErrProgramFailed = 1722;    ///< a program run as part of the setup did not finish as expected
constexpr i32 kErrDllFailed = 1723;        ///< a DLL required for this install could not be run
constexpr i32 kErrBinaryMissing = 2721;    ///< custom action not found in Binary table stream
constexpr i32 kErrBadAction = 2717;        ///< bad action condition or error calling custom action
constexpr i32 kErrUnsupportedType = 2723;  ///< custom action specifies unsupported type
constexpr i32 kErrNotInScript = 2762;      ///< cannot schedule: not between InstallInitialize and InstallFinalize

constexpr u16 kMachineI386 = 0x014C;   ///< IMAGE_FILE_MACHINE_I386 (winnt.h)
constexpr u16 kMachineAmd64 = 0x8664;  ///< IMAGE_FILE_MACHINE_AMD64 (winnt.h)

std::atomic<CustomActionProcess*> g_process_host{nullptr};

/// One custom-action server process (msiexec.exe -Embedding): its
/// connection, bitness and security context.
struct CaServer {
    std::shared_ptr<RemoteChannel> channel;
    bool x64{false};
    bool elevated{false};
};

/// Asynchronous custom actions still running in this session.
struct PendingAsync {
    std::string action;
    u64 handle{0};          ///< process; 0 for an EXE nobody waits on
    u32 install_handle{0};  ///< the hInstall a DLL action holds
    std::shared_ptr<CaServer> server;  ///< the dedicated server a DLL action runs in
    std::u16string temp_path;
};

struct CustomActionState {
    std::mutex mutex;
    std::vector<PendingAsync> pending;
    /// Idle custom-action servers, reused by the session's later actions
    /// (Windows keeps one per bitness and context for an install).
    std::vector<std::shared_ptr<CaServer>> servers;
};

enum class Mode : u8 { Immediate, Scheduled, Rollback, Commit };

[[nodiscard]] std::u16string to_u16(const std::string& utf8) {
    const auto units = utf8_to_utf16(utf8);
    return std::u16string(units.begin(), units.end());
}

[[nodiscard]] std::string to_utf8(const std::u16string& text) {
    return utf16_to_utf8(reinterpret_cast<const u16*>(text.data()), static_cast<int>(text.size()));
}

/// What one run of a custom action needs, captured so a deferred action can
/// run after the immediate context is gone.
struct Run {
    Session* session{nullptr};
    Services* services{nullptr};
    ActionContext* immediate{nullptr};  ///< non-null only for immediate actions
    std::string action_name;
    u32 action_type{0};
    Mode mode{Mode::Immediate};
    std::string custom_action_data;     ///< deferred modes
    std::string source;
    std::string target;  ///< already formatted where the type formats it

    [[nodiscard]] u32 type() const { return action_type; }
    [[nodiscard]] const std::string& action() const { return action_name; }
    [[nodiscard]] bool cont() const { return (type() & kCaContinue) != 0; }
    [[nodiscard]] bool async() const { return (type() & kCaAsync) != 0; }

    /// MsiProcessMessage from the engine on behalf of this action.
    void post(u32 message, std::initializer_list<std::string> fields, std::string templ = {}) const {
        Record rec(static_cast<u32>(fields.size()));
        rec.fields[0].kind = templ.empty() ? Record::Kind::Null : Record::Kind::String;
        rec.fields[0].text = to_u16(templ);
        u32 i = 1;
        for (const std::string& f : fields) {
            rec.fields[i].kind = Record::Kind::String;
            rec.fields[i].text = to_u16(f);
            ++i;
        }
        // Field 1 of an engine error is the Error-table number, an integer.
        if (fields.size() != 0 && !fields.begin()->empty() && fields.begin()->size() < 10 &&
            fields.begin()->find_first_not_of("0123456789") == std::string::npos) {
            rec.fields[1].kind = Record::Kind::Integer;
            rec.fields[1].integer = std::stoi(*fields.begin());
            rec.fields[1].text.clear();
        }
        if (immediate != nullptr) {
            (void)immediate->message(message, rec);
        } else {
            ActionContext ctx(*session, *services);
            (void)ctx.message(message, rec);
        }
    }

    /// The action failed to run at all, or its code returned failure: the
    /// Continue bit makes either a success (measured: 1723 and 1722 are both
    /// "translated to success due to continue marking").
    [[nodiscard]] u32 failed() const { return cont() ? kActionSuccess : kActionFailure; }

    [[nodiscard]] CustomActionProcess* host() const { return process_custom_action_host(); }

    /// The engine's install-side view of this session: execute sequence and
    /// script run on the server side, the UI sequence on the client.
    [[nodiscard]] bool server_side() const {
        Engine* engine = session->engine();
        return engine != nullptr && (engine->options().server || engine->server_session() == session);
    }
};

[[nodiscard]] CustomActionState& state_of(Session& session) { return session.state<CustomActionState>(); }

/// A column of the CustomAction row for @p action ("" when absent or null).
[[nodiscard]] std::string custom_action_column(Session& session, const std::string& action, const char* column) {
    Package* package = session.package();
    const Table* table = package != nullptr ? package->table("CustomAction") : nullptr;
    if (table == nullptr) return {};
    const auto key = table->column_index("Action");
    const auto col = table->column_index(column);
    if (!key || !col) return {};
    for (const auto& row : table->rows) {
        if (*key < row.size() && row[*key] == action) return *col < row.size() ? row[*col] : std::string{};
    }
    return {};
}

/// The bytes of a Binary-table row (the stream "Binary.<Name>").
[[nodiscard]] const std::vector<u8>* binary_stream(Session& session, const std::string& name) {
    Package* package = session.package();
    return package != nullptr ? package->stream("Binary." + name) : nullptr;
}

/// IMAGE_FILE_HEADER.Machine of a PE image in memory (PE Format: e_lfanew at
/// 0x3C, "PE\0\0", then the machine).
[[nodiscard]] std::optional<u16> pe_machine(const std::vector<u8>& image) {
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return std::nullopt;
    const u32 nt = static_cast<u32>(image[0x3C]) | (static_cast<u32>(image[0x3D]) << 8) |
                   (static_cast<u32>(image[0x3E]) << 16) | (static_cast<u32>(image[0x3F]) << 24);
    if (nt > image.size() - 6 || image[nt] != 'P' || image[nt + 1] != 'E' || image[nt + 2] != 0 || image[nt + 3] != 0) {
        return std::nullopt;
    }
    return static_cast<u16>(image[nt + 4] | (image[nt + 5] << 8));
}

/// Where a custom action's Binary-table code is written: the server's
/// %WINDIR%\Installer for the execute sequence and the script (measured), the
/// client's %TEMP% for the UI sequence.
[[nodiscard]] std::u16string temp_directory(const Run& run) {
    if (run.mode == Mode::Immediate && !run.server_side()) return to_u16(run.session->property("TempFolder"));
    std::string windows = run.session->property("WindowsFolder");
    if (windows.empty()) return {};
    if (windows.back() != '\\') windows += '\\';
    return to_u16(windows + "Installer");
}

/// The current directory of an EXE action whose type names none: the
/// installer service's, %WINDIR%\system32 (measured).
[[nodiscard]] std::u16string service_directory(const Run& run) {
    std::string windows = run.session->property("WindowsFolder");
    if (windows.empty()) return {};
    if (windows.back() != '\\') windows += '\\';
    return to_u16(windows + "system32");
}

// ---- DLL actions (types 1, 17) -----------------------------------------------------

/// A fresh token naming one custom-action server's pipe.
std::u16string new_server_token() {
    static std::atomic<u32> counter{0};
    const u64 now = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%08X%08X%016llX", static_cast<unsigned>(::getpid()),
                  counter.fetch_add(1) + 1, static_cast<unsigned long long>(now));
    return to_u16(buf);
}

/// Start a custom-action server and wait for it to connect. Learn "64-Bit
/// Custom Actions": the server's bitness is the DLL's; "Custom Action
/// Security": deferred no-impersonate actions run in the elevated (system)
/// server, every other action in an impersonated one.
std::shared_ptr<CaServer> launch_server(const Run& run, bool x64, bool elevated) {
    CustomActionProcess* host = run.host();
    const std::u16string token = new_server_token();
    const std::string pipe_name = custom_action_server_pipe(to_utf8(token));
    auto listener = ipc::UnixPipe::create_server(pipe_name);
    if (!listener) return nullptr;
    const auto& launch = run.services->launch_custom_action_server;
    const bool started = launch ? launch(x64, elevated, token) : host->start_custom_action_server(x64, token);
    if (!started) return nullptr;
    // A guest process starts in seconds; a server that never connects failed
    // to start (the action then fails with 1723 as an unloadable DLL does).
    constexpr int kSliceMs = 50;
    constexpr int kConnectMs = 180000;
    std::shared_ptr<ipc::UnixPipe> pipe;
    for (int waited = 0; !pipe && waited < kConnectMs; waited += kSliceMs) {
        pipe = listener->accept(kSliceMs);
        if (!pipe && run.services->ui != nullptr) host->pump_messages();
    }
    if (!pipe) return nullptr;
    auto server = std::make_shared<CaServer>();
    server->channel = std::make_shared<RemoteChannel>(pipe);
    server->x64 = x64;
    server->elevated = elevated;
    auto hello = server->channel->receive(kConnectMs);
    if (!hello || hello->kind != Wire::Hello) return nullptr;
    WireReader r(hello->body);
    (void)r.u32_();                 // server pid
    const bool server_x64 = r.u8_() != 0;
    if (!r.ok() || server_x64 != x64) return nullptr;
    return server;
}

std::shared_ptr<CaServer> acquire_server(const Run& run, bool x64, bool elevated) {
    auto& st = state_of(*run.session);
    {
        std::lock_guard lock(st.mutex);
        for (auto it = st.servers.begin(); it != st.servers.end(); ++it) {
            if ((*it)->x64 == x64 && (*it)->elevated == elevated && !(*it)->channel->broken()) {
                auto server = *it;
                st.servers.erase(it);
                return server;
            }
        }
    }
    return launch_server(run, x64, elevated);
}

void release_server(Session& session, std::shared_ptr<CaServer> server) {
    if (!server || server->channel->broken()) return;
    auto& st = state_of(session);
    std::lock_guard lock(st.mutex);
    st.servers.push_back(std::move(server));
}

/// Outcome of one DLL action in its server (Wire::Done).
enum DoneStatus : u32 { kDoneRan = 0, kDoneLoadFailed = 1, kDoneNoEntry = 2, kDoneNoThread = 3 };

/// Serve @p server's relayed calls until the action it runs ends.
std::optional<std::pair<u32, u32>> wait_for_action(Session& session, Services& services, CaServer& server) {
    CustomActionProcess* host = process_custom_action_host();
    if (host == nullptr || services.manager == nullptr) return std::nullopt;
    RelayExecutor executor(*services.manager, *host);
    std::function<void()> idle;
    if (services.ui != nullptr) idle = [host] { host->pump_messages(); };
    auto done = serve_custom_action(*server.channel, executor, idle);
    executor.close_all();
    (void)session;
    if (!done) return std::nullopt;
    WireReader r(*done);
    const u32 status = r.u32_();
    const u32 rc = r.u32_();
    if (!r.ok()) return std::nullopt;
    return std::make_pair(status, rc);
}

u32 run_dll(const Run& run) {
    CustomActionProcess* host = run.host();
    const std::string& entry = run.target;
    if (host == nullptr) {
        log_unimplemented("CustomAction", "DLL custom action outside a custom-action host process");
        run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, run.source});
        return run.failed();
    }
    std::u16string path;
    std::u16string temp;
    std::optional<u16> machine;
    if ((run.type() & 0x30) == kCaBinaryData) {
        const std::vector<u8>* bytes = binary_stream(*run.session, run.source);
        if (bytes == nullptr) {
            run.post(kMsgError, {std::to_string(kErrBinaryMissing), run.action()});
            return kActionFailure;
        }
        machine = pe_machine(*bytes);
        if (!host->write_temp_file(*bytes, temp_directory(run), temp)) {
            run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, run.source});
            return run.failed();
        }
        path = temp;
    } else {
        // Type 17: the File-table file at its installed location.
        path = to_u16(run.session->format("[#" + run.source + "]"));
        machine = host->image_machine(path);
    }
    const auto cleanup = [&] {
        if (!temp.empty()) host->delete_file(temp);
    };
    const std::string library = to_utf8(path);
    if (!machine.has_value() || (*machine != kMachineAmd64 && *machine != kMachineI386)) {
        // Not an x86/x64 PE (or unreadable): LoadLibrary fails in any
        // custom-action server; Windows reports the DLL as not runnable.
        run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, library});
        cleanup();
        return run.failed();
    }
    const bool x64 = *machine == kMachineAmd64;
    const bool elevated = run.mode != Mode::Immediate && (run.type() & kCaNoImpersonate) != 0;

    // The hInstall: the engine's session object; inside a running ScriptOp
    // the engine hands out the restricted deferred handle carrying the
    // CustomActionData set below. The server gets a proxy for it.
    Engine* engine = run.session->engine();
    HandleTable* handles = run.services->manager != nullptr ? &run.services->manager->handles() : nullptr;
    if (engine == nullptr || handles == nullptr) {
        log_unimplemented("CustomAction", "DLL custom action on a session without an engine");
        run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, library});
        cleanup();
        return run.failed();
    }
    // An asynchronous action keeps its own server for as long as it runs.
    std::shared_ptr<CaServer> server = run.async() ? launch_server(run, x64, elevated) : acquire_server(run, x64, elevated);
    if (!server) {
        run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, library});
        cleanup();
        return run.failed();
    }
    if (run.mode != Mode::Immediate) engine->set_custom_action_data(run.custom_action_data);
    const u32 install = engine->open_install_handle();
    WireWriter w;
    w.str(run.action());
    w.u16str(path);
    w.str(entry);
    w.u32_(install);
    if (!server->channel->send(Wire::RunDll, w.data())) {
        (void)handles->close(install);
        run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, library});
        cleanup();
        return run.failed();
    }
    if (run.async()) {
        auto& st = state_of(*run.session);
        std::lock_guard lock(st.mutex);
        st.pending.push_back(PendingAsync{run.action(), 0, install, server, temp});
        return kActionSuccess;
    }
    const auto outcome = wait_for_action(*run.session, *run.services, *server);
    (void)handles->close(install);
    if (outcome) release_server(*run.session, server);
    cleanup();
    if (!outcome || outcome->first != kDoneRan) {
        // The library would not load, the entry point is missing, or the
        // server went away while the action ran.
        run.post(kMsgError, {std::to_string(kErrDllFailed), run.action(), entry, library});
        return run.failed();
    }
    if (run.cont()) return kActionSuccess;
    return dll_return_to_action_code(outcome->second);
}

// ---- EXE actions (types 2, 18, 34, 50) ---------------------------------------------

u32 run_exe(const Run& run) {
    CustomActionProcess* host = run.host();
    std::u16string application;
    std::u16string command_line;
    std::u16string directory = service_directory(run);
    std::u16string temp;
    std::string location;
    const u32 source_kind = run.type() & 0x30;
    if (host == nullptr) {
        log_unimplemented("CustomAction", "EXE custom action outside a custom-action host process");
        run.post(kMsgError, {std::to_string(kErrProgramNotRun), run.action(), run.source, run.target});
        return run.failed();
    }
    if (source_kind == kCaDirectory) {
        // Type 34: Target is the whole command line, Source the working
        // directory.
        directory = to_u16(run.session->format("[" + run.source + "]"));
        command_line = to_u16(run.target);
        location = run.target;
    } else {
        if (source_kind == kCaBinaryData) {
            const std::vector<u8>* bytes = binary_stream(*run.session, run.source);
            if (bytes == nullptr) {
                run.post(kMsgError, {std::to_string(kErrBinaryMissing), run.action()});
                return kActionFailure;
            }
            if (!host->write_temp_file(*bytes, temp_directory(run), temp)) {
                run.post(kMsgError, {std::to_string(kErrProgramNotRun), run.action(), run.source, run.target});
                return run.failed();
            }
            application = temp;
        } else if (source_kind == kCaSourceFile) {
            application = to_u16(run.session->format("[#" + run.source + "]"));  // type 18
        } else {
            application = to_u16(run.session->property(run.source));              // type 50
        }
        location = to_utf8(application);
        // Measured: "C:\WINDOWS\Installer\MSI95E.tmp" probe-arg exit=0
        command_line = u"\"" + application + u"\"";
        if (!run.target.empty()) command_line += u" " + to_u16(run.target);
    }
    const u64 process = host->start_process(application, command_line, directory);
    if (process == 0) {
        run.post(kMsgError, {std::to_string(kErrProgramNotRun), run.action(), location, run.target});
        if (!temp.empty()) host->delete_file(temp);
        return run.failed();
    }
    if (run.async()) {
        auto& st = state_of(*run.session);
        std::lock_guard lock(st.mutex);
        if (run.cont()) {
            // Async + Continue: not waited for; it may outlive the installer.
            host->close_handle(process);
            st.pending.push_back(PendingAsync{run.action(), 0, 0, 0, temp});
        } else {
            st.pending.push_back(PendingAsync{run.action(), process, 0, 0, temp});
        }
        return kActionSuccess;
    }
    const u32 exit_code = host->wait_and_close(process);
    if (!temp.empty()) host->delete_file(temp);
    if (exit_code == 0 || run.cont()) return kActionSuccess;
    run.post(kMsgError, {std::to_string(kErrProgramFailed), run.action(), location, run.target});
    return kActionFailure;
}

// ---- the other basic types ------------------------------------------------------------

u32 run_script(const Run& run) {
    // Types 5/6/21/22/37/38/53/54 need an Active Scripting engine (JScript /
    // VBScript) with the installer's Session object attached; this runtime
    // has none.
    log_unimplemented("CustomAction", (run.type() & 7) == kCaJScript ? "JScript custom action (no Active Scripting engine)"
                                                                     : "VBScript custom action (no Active Scripting engine)");
    run.post(kMsgError, {std::to_string(kErrScriptFailed), run.action(), run.target});
    return run.failed();
}

u32 run_nested_install(const Run& run) {
    // Types 7/23/39: a concurrent installation in a second engine session on
    // a thread of this process. Not implemented: nested engine sessions.
    log_unimplemented("CustomAction", "concurrent installation (type 7/23/39) needs a nested engine session");
    run.post(kMsgError, {std::to_string(kErrBadAction), run.action()});
    return run.failed();
}

u32 run_error(const Run& run) {
    // Type 19: an integer after formatting indexes the Error table; any
    // other text is the message itself. Measured: the message is posted as
    // the record's template with no fields, then the action fails.
    std::string message = run.target;
    if (!message.empty() && message.find_first_not_of("0123456789") == std::string::npos) {
        message.clear();
        Package* package = run.session->package();
        const Table* table = package != nullptr ? package->table("Error") : nullptr;
        if (table != nullptr) {
            const auto key = table->column_index("Error");
            const auto text = table->column_index("Message");
            for (const auto& row : table->rows) {
                if (key && text && *key < row.size() && row[*key] == run.target && *text < row.size()) {
                    message = row[*text];
                    break;
                }
            }
        }
        if (message.empty()) {
            run.post(kMsgError, {run.target});
            return kActionFailure;
        }
    }
    run.post(kMsgError, {}, message);
    return kActionFailure;
}

/// Run one action now, in @p run's mode.
u32 execute(const Run& run) {
    const u32 kind = run.type() & 7;
    const u32 source_kind = run.type() & 0x30;
    switch (kind) {
    case kCaDll:
        if (source_kind == kCaBinaryData || source_kind == kCaSourceFile) return run_dll(run);
        break;
    case kCaExe:
        return run_exe(run);
    case kCaJScript:
    case kCaVBScript:
        return run_script(run);
    case kCaInstall:
        return run_nested_install(run);
    case kCaTextData:
        if (source_kind == kCaSourceFile) return run_error(run);
        if (source_kind == kCaDirectory) {
            // Type 35: MsiSetTargetPath with the formatted Target.
            return set_target_path(*run.session, run.source, run.target) == 0 ? kActionSuccess : kActionFailure;
        }
        if (source_kind == kCaProperty) {
            run.session->set_property(run.source, run.target);  // type 51
            return kActionSuccess;
        }
        break;
    default:
        break;
    }
    run.post(kMsgInfo, {std::to_string(kErrBadAction), run.action()});
    return kActionFailure;
}

}  // namespace

void set_process_custom_action_host(CustomActionProcess* host) { g_process_host.store(host); }

CustomActionProcess* process_custom_action_host() { return g_process_host.load(); }

u32 dll_return_to_action_code(u32 returned) {
    switch (returned) {
    case kActionSuccess:
    case kActionSkipRemaining:
    case kActionUserExit:
    case kActionFailure:
    case kActionSuspend:
    case kActionNotCalled:
        return returned;
    default:
        return kActionFailure;  // measured: 1 and 1234 fail the install with 1603
    }
}

u32 finish_async_custom_actions(Session& session) {
    std::vector<PendingAsync> pending;
    std::vector<std::shared_ptr<CaServer>> servers;
    {
        auto& st = state_of(session);
        std::lock_guard lock(st.mutex);
        pending.swap(st.pending);
        servers.swap(st.servers);
    }
    CustomActionProcess* host = process_custom_action_host();
    Engine* engine = session.engine();
    for (PendingAsync& p : pending) {
        // Measured: the return value is collected (and logged) but never
        // changes the install's result, with or without Continue.
        if (p.server && engine != nullptr) {
            Services services = engine->services();
            (void)wait_for_action(session, services, *p.server);
            servers.push_back(p.server);
        }
        if (host != nullptr && p.handle != 0) (void)host->wait_and_close(p.handle);
        if (p.install_handle != 0 && engine != nullptr) (void)engine->manager().handles().close(p.install_handle);
        if (host != nullptr && !p.temp_path.empty()) host->delete_file(p.temp_path);
    }
    // The session's custom-action servers end with it.
    for (auto& server : servers) {
        (void)server->channel->send(Wire::Shutdown, {});
        server->channel->close();
    }
    return kActionSuccess;
}

std::string custom_action_server_pipe(const std::string& token) {
    return "\\\\.\\pipe\\Vitreo.MsiCustomActionServer." + token;
}

u32 run_custom_action_server(MsiManager& mgr, const std::string& token, CustomActionProcess& host) {
    set_custom_action_server_process(true);
    std::shared_ptr<ipc::UnixPipe> pipe;
    for (int attempt = 0; !pipe && attempt < 100; ++attempt) {
        pipe = ipc::UnixPipe::connect_client(custom_action_server_pipe(token));
        if (!pipe) ::usleep(20000);
    }
    if (!pipe) return kActionFailure;
    auto channel = std::make_shared<RemoteChannel>(pipe);
    {
        WireWriter w;
        w.u32_(static_cast<u32>(::getpid()));
        w.u8_(host.is_64bit() ? 1 : 0);
        if (!channel->send(Wire::Hello, w.data())) return kActionFailure;
    }
    for (;;) {
        auto frame = channel->receive();
        if (!frame || frame->kind == Wire::Shutdown) return 0;
        if (frame->kind != Wire::RunDll) return kActionFailure;
        WireReader r(frame->body);
        const std::string action = r.str();
        const std::u16string path = r.u16str();
        const std::string entry = r.str();
        const u32 install = r.u32_();
        (void)action;
        u32 status = kDoneRan;
        u32 rc = 0;
        // Learn "Custom Action Type 1": the DLL is loaded, the entry point
        // called with the session handle on its own thread, then unloaded.
        const u64 module = r.ok() ? host.load_library(path) : 0;
        const u64 fn = module != 0 ? host.get_proc_address(module, entry) : 0;
        if (module == 0) {
            status = kDoneLoadFailed;
        } else if (fn == 0) {
            status = kDoneNoEntry;
        } else {
            const u32 handle = add_remote_handle(mgr, channel, install);
            const u64 thread = host.start_dll_entry(fn, handle);
            if (thread == 0) status = kDoneNoThread;
            else rc = host.wait_and_close(thread);
            (void)mgr.handles().close(handle);
        }
        if (module != 0) host.free_library(module);
        WireWriter w;
        w.u32_(status);
        w.u32_(rc);
        if (!channel->send(Wire::Done, w.data())) return 0;
    }
}

u32 run_custom_action(ActionContext& ctx, const std::string& action, u32 type,
                      const std::string& source, const std::string& target) {
    Session& session = ctx.session();
    Run run;
    run.session = &session;
    run.services = &ctx.services();
    run.source = source;
    run.action_name = action;
    run.action_type = type;

    // Bits outside the documented Type fields (MsiDefs.h) are not a type.
    constexpr u32 kKnownBits = 0x7 | 0x30 | kCaContinue | kCaAsync | 0x300 | kCaInScript | kCaNoImpersonate |
                               kCa64BitScript | kCaHideTarget | kCaTSAware;
    const u32 kind = type & 7;
    if ((type & ~kKnownBits) != 0 || kind == 0 || kind == 4) {
        run.immediate = &ctx;
        run.post(kMsgInfo, {std::to_string(kErrBadAction), action});
        return kActionFailure;
    }

    // A restricted handle (MsiOpenPackageEx IGNOREMACHINESTATE) runs only the
    // property-setting types 19, 35 and 51 (Learn MsiOpenPackageEx); every
    // other action is ERROR_FUNCTION_NOT_CALLED there.
    if (session.engine() != nullptr && session.engine()->restricted() && kind != kCaTextData) {
        return kActionNotCalled;
    }

    // ExtendedType msidbCustomActionTypePatchUninstall: only while a patch
    // is being removed (MSIPATCHREMOVE names it).
    const std::string extended = custom_action_column(session, action, "ExtendedType");
    if (!extended.empty() && (std::stoll(extended) & kCaPatchUninstall) != 0 &&
        session.property("MSIPATCHREMOVE").empty()) {
        return kActionNotCalled;
    }

    const bool text = kind == kCaTextData;
    const bool in_script = (type & kCaInScript) != 0;
    const bool rollback = in_script && (type & 0x300) == kCaRollback;
    const bool commit = in_script && (type & 0x300) == kCaCommit;
    if ((text && in_script) || (rollback && (type & kCaAsync) != 0) ||
        (kind == kCaInstall && (in_script || (type & kCaAsync) != 0)) ||
        ((kind == kCaJScript || kind == kCaVBScript) && (type & kCaAsync) != 0)) {
        run.immediate = &ctx;
        run.post(kMsgError, {std::to_string(kErrUnsupportedType), action, "", ""});
        return kActionFailure;
    }

    // Targets are formatted where the type formats them: 19/35/51 always,
    // EXE command lines (2/18/34/50), not DLL entry points or script names.
    run.target = (kind == kCaDll || kind == kCaJScript || kind == kCaVBScript) ? target : session.format(target);

    if (!in_script) {
        run.immediate = &ctx;
        // Execution scheduling options (Learn "Custom Action Execution
        // Scheduling Options"): they act on the execute sequence, which runs
        // on the server side; none affects the UI sequence. The UI sequence
        // ran when the engine shows the wizard UI.
        const u32 schedule = type & 0x300;
        if (run.server_side()) {
            Engine* engine = session.engine();
            if (schedule == kCaFirstSequence && (engine->shows_wizard_ui() || run.services->client_ran_ui_sequence))
                return kActionNotCalled;
            if (schedule == kCaClientRepeat) return kActionNotCalled;
        }
        return execute(run);
    }

    // Deferred, rollback and commit actions go into the script with the
    // context they may observe (Learn "Obtaining Context Information for
    // Deferred Execution Custom Actions", measured).
    if (!ctx.scripting()) {
        run.immediate = &ctx;
        run.post(kMsgInfo, {std::to_string(kErrNotInScript), action});
        return kActionFailure;
    }
    // What the action may observe later is the engine's restricted handle
    // (ProductCode / UserSID / language per mode); the action's own share is
    // CustomActionData, the property named like it at scheduling time.
    run.mode = rollback ? Mode::Rollback : commit ? Mode::Commit : Mode::Scheduled;
    run.custom_action_data = session.property(action);
    // msidbCustomActionTypeNoImpersonate: a deferred DLL action runs in the
    // elevated (system-context) custom-action server, every other one in an
    // impersonated server the client starts (run_dll). TSAware only differs
    // on a Terminal Server.

    ScriptOp op;
    op.action = action;
    Run deferred = run;
    // The engine runs a rollback_only op's `run` during a rollback (its
    // result is ignored, measured "Error in rollback skipped") and a
    // commit_only op's after a successful script.
    op.rollback_only = rollback;
    op.commit_only = commit;
    op.run = [deferred] { return execute(deferred); };
    ctx.schedule(std::move(op));
    return kActionSuccess;
}

}  // namespace vitreo::winapi::msi
