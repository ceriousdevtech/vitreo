// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_process_control.cpp
/// @brief ScmManager service process lifecycle: StartService, status liveness
///        and ControlService.

#include "winapi/advapi32/services/scm_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "ipc/process_session.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>

#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vitreo::winapi {

// kernel32/process/process_ext.cpp: copy a capsule file (generated system
// images included) to a host temporary executable.
Result<std::string, capsule::VfsError> materialize_guest_executable(capsule::Vfs& vfs,
                                                                    const std::string& guest_path);

using namespace scm_detail;
using namespace service_constants;

namespace {

// Learn StartServiceW: before returning success the SCM sets START_PENDING
// with WaitHint 2 seconds. StartService itself returns when the service
// control dispatcher has created the ServiceMain thread, and fails with
// ERROR_SERVICE_REQUEST_TIMEOUT if the process started but never called
// StartServiceCtrlDispatcher. The same 30-second bound is the documented
// StartService block timeout when the SCM is busy handling a control.
constexpr auto kStartServiceDispatcherTimeout = std::chrono::seconds(30);
constexpr auto kStartServiceDispatcherPoll = std::chrono::milliseconds(10);

bool process_is_live(pid_t pid) {
    if (pid <= 0) return false;
    // Darwin: kill(pid, 0) succeeds for zombies, while proc_pidinfo returns
    // ESRCH. The Windows SCM watches the service process handle and reports
    // SERVICE_STOPPED when that process exits, even if it never called
    // SetServiceStatus. A detached StartService worker that ExitProcess's
    // before RUNNING becomes a zombie of this host until reaped, and must
    // not keep QueryServiceStatus at START_PENDING.
    proc_bsdinfo info {};
    const int n = ::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info,
                                 static_cast<int>(sizeof(info)));
    if (n == static_cast<int>(sizeof(info))) {
        return info.pbi_status != SZOMB;
    }
    if (::kill(pid, 0) == 0) return false;
    return errno == EPERM;
}

/// Split an ImagePath into the executable and its argument tail, honouring the
/// quoted form ("C:\path with spaces\svc.exe" -k arg) that services use.
void split_image_path(const std::wstring& image_path, std::wstring& exe_out,
                      std::wstring& args_out) {
    exe_out.clear();
    args_out.clear();
    if (image_path.empty()) return;

    if (image_path.front() == L'"') {
        const auto end = image_path.find(L'"', 1);
        if (end == std::wstring::npos) {
            exe_out = image_path.substr(1);
            return;
        }
        exe_out = image_path.substr(1, end - 1);
        args_out = image_path.substr(end + 1);
    } else {
        const auto space = image_path.find(L' ');
        if (space == std::wstring::npos) {
            exe_out = image_path;
            return;
        }
        exe_out = image_path.substr(0, space);
        args_out = image_path.substr(space);
    }

    while (!args_out.empty() && args_out.front() == L' ') args_out.erase(0, 1);
}

} // namespace

// ---- Start / status / control ----------------------------------------------

bool ScmManager::start_service(const std::wstring& name,
                               const std::vector<std::wstring>& args,
                               u32& error) {
    auto config = query_config(name);
    if (!config) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }
    if (config->start_type == SERVICE_DISABLED) {
        error = error_codes::ERROR_SERVICE_DISABLED;
        return false;
    }

    const ServiceStatus current = query_status(name);
    if (current.current_state != SERVICE_STOPPED) {
        error = error_codes::ERROR_SERVICE_ALREADY_RUNNING;
        return false;
    }

    // Learn StartServiceW: success means the SCM received notification that
    // this start created the ServiceMain thread. A leftover handshake from a
    // previous start (or from ServiceMain returning) must not satisfy that.
    remove_dispatcher_handshake(status_file_path(name));

    if ((config->service_type & SERVICE_KERNEL_DRIVER) != 0) {
        // A driver's ImagePath names one image file for the I/O manager; it
        // is not a command line, so a space does not end it. CPU-Z registers
        // "C:\ProgramData\CPUID Software\cpu-z\<name>" unquoted, and the
        // command-line split handed the provider "C:\ProgramData\CPUID",
        // a directory (ERROR_BAD_FORMAT). No process is created, so no
        // process session is needed either.
        std::wstring image = config->image_path;
        if (image.size() >= 2 && image.front() == L'"' && image.back() == L'"') {
            image = image.substr(1, image.size() - 2);
        }
        const std::string guest_image = to_narrow(image);
        auto host_image = m_vfs.resolve_host_path(guest_image);
        if (image.empty() || host_image.is_err()) {
            VITREO_TRACE("[scm] StartService kernel driver '%s': image '%s' not "
                         "found\n", to_narrow(name).c_str(), guest_image.c_str());
            error = error_codes::ERROR_FILE_NOT_FOUND;
            return false;
        }
        if (!m_kernel_driver_start) {
            VITREO_TRACE("[scm] StartService kernel driver '%s': no native "
                         "device provider\n", to_narrow(name).c_str());
            error = error_codes::ERROR_CALL_NOT_IMPLEMENTED;
            return false;
        }
        if (!m_kernel_driver_start(name, config->image_path,
                                   host_image.value(), error)) {
            VITREO_TRACE("[scm] StartService kernel driver '%s': provider "
                         "rejected image '%s' host='%s' error=%u\n",
                         to_narrow(name).c_str(), guest_image.c_str(),
                         host_image.value().c_str(), error);
            return false;
        }
        ServiceStatus running;
        running.service_type = config->service_type;
        running.current_state = SERVICE_RUNNING;
        running.controls_accepted = SERVICE_ACCEPT_STOP |
                                    SERVICE_ACCEPT_SHUTDOWN;
        (void)write_status_file(name, running);
        VITREO_TRACE("[scm] StartService kernel driver '%s' -> native device "
                     "provider RUNNING\n", to_narrow(name).c_str());
        error = error_codes::ERROR_SUCCESS;
        return true;
    }

    if (m_process_session == nullptr) {
        // No session owner means we cannot create the process. Say so instead
        // of reporting a service that is not running as started.
        VITREO_TRACE("[scm] StartService '%s' rejected: no ProcessSession\n",
                     to_narrow(name).c_str());
        error = error_codes::ERROR_SERVICE_NO_THREAD;
        return false;
    }

    std::wstring exe;
    std::wstring image_args;
    split_image_path(config->image_path, exe, image_args);
    if (exe.empty()) {
        error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }

    const std::string guest_exe = to_narrow(exe);
    // The Service Control Manager is a native (64-bit) process: an ImagePath
    // is never WOW64-redirected, whatever the bitness of the process that
    // asked for the start (Learn "File System Redirector").
    struct NativeView {
        bool saved = capsule::Vfs::thread_redirection_disabled();
        NativeView() { capsule::Vfs::set_thread_redirection_disabled(true); }
        ~NativeView() { capsule::Vfs::set_thread_redirection_disabled(saved); }
    } native_view;
    auto host_path = m_vfs.resolve_host_path(guest_exe);
    if (host_path.is_err()) {
        VITREO_TRACE("[scm] StartService '%s': image '%s' not found\n",
                     to_narrow(name).c_str(), guest_exe.c_str());
        error = error_codes::ERROR_FILE_NOT_FOUND;
        return false;
    }
    // An image the capsule serves without a host file (a runtime-provided
    // System32 program such as msiexec.exe) is launched from a copy, as
    // CreateProcess does (process_ext.cpp resolve_guest_executable).
    std::string materialized;
    if (::access(host_path.value().c_str(), R_OK) != 0) {
        auto copy = materialize_guest_executable(m_vfs, guest_exe);
        if (copy.is_err()) {
            VITREO_TRACE("[scm] StartService '%s': image '%s' not found\n",
                         to_narrow(name).c_str(), guest_exe.c_str());
            error = error_codes::ERROR_FILE_NOT_FOUND;
            return false;
        }
        materialized = copy.value();
        host_path = Result<std::string, capsule::VfsError>::ok(materialized);
    }

    // Windows passes ServiceMain argv[0] = service name, then the arguments
    // supplied to StartService. The ImagePath's own tail is part of the process
    // command line, not of argv to ServiceMain.
    std::string command_line = "\"" + guest_exe + "\"";
    if (!image_args.empty()) command_line += " " + to_narrow(image_args);

    ipc::WorkerLaunchSpec spec;
    spec.host_image_path = host_path.value();
    spec.guest_image_path = guest_exe;
    spec.guest_command_line = command_line;
    spec.guest_current_directory = m_current_directory;
    spec.capsule_name = m_capsule_name;
    for (const auto& arg : args) {
        spec.guest_service_arguments_utf16.push_back(
            encode_utf16_hex(arg));
    }
    spec.extra_env.push_back("VITREO_SERVICE_NAME=" + to_narrow(name));
    const std::wstring account = to_lower(
        config->object_name.empty() ? std::wstring(L"LocalSystem")
                                    : config->object_name);
    spec.token_context =
        account == L"localsystem" ||
                account == L"nt authority\\system" ||
                account == L".\\localsystem"
            ? ipc::WorkerTokenContext::LocalSystemService
            : ipc::WorkerTokenContext::RestrictedService;
    // A Windows service is owned by the SCM, not by whoever called
    // StartService. It must survive the requester's exit, so it breaks away
    // from the session's kill-on-close ownership and is never death-watched.
    spec.detached = true;

    const pid_t pid = m_process_session->launch_worker(spec);
    if (!materialized.empty()) (void)::unlink(materialized.c_str());  // the worker runs its own snapshot
    if (pid <= 0) {
        VITREO_TRACE("[scm] StartService '%s': launch failed\n",
                     to_narrow(name).c_str());
        remove_status_file(name);
        // Learn StartServiceW: ERROR_SERVICE_NO_THREAD if a thread could not
        // be created. ERROR_SERVICE_REQUEST_TIMEOUT is reserved for a process
        // that started but never called StartServiceCtrlDispatcher.
        error = error_codes::ERROR_SERVICE_NO_THREAD;
        return false;
    }

    VITREO_TRACE("[scm] StartService '%s' account='%s' token=%s "
                 "-> pid %d ('%s'), waiting for dispatcher\n",
                 to_narrow(name).c_str(),
                 to_narrow(config->object_name).c_str(),
                 spec.token_context ==
                         ipc::WorkerTokenContext::LocalSystemService
                     ? "LocalSystem"
                     : "restricted-service",
                 static_cast<int>(pid),
                 command_line.c_str());

    // Learn StartServiceW: do not return success until the service control
    // dispatcher has created the ServiceMain thread. Publishing START_PENDING
    // before that handshake makes QueryServiceStatus look waitable while the
    // process may already have exited without connecting.
    const auto deadline =
        std::chrono::steady_clock::now() + kStartServiceDispatcherTimeout;
    const std::string status_path = status_file_path(name);
    for (;;) {
        const bool connected = dispatcher_handshake_exists(status_path);
        const bool live = process_is_live(pid);
        if (connected) {
            error = error_codes::ERROR_SUCCESS;
            return true;
        }
        if (!live) {
            int wait_status = 0;
            (void)::waitpid(pid, &wait_status, WNOHANG);
            VITREO_TRACE("[scm] StartService '%s': pid %d exited before "
                         "StartServiceCtrlDispatcher\n",
                         to_narrow(name).c_str(), static_cast<int>(pid));
            ServiceStatus dead;
            dead.service_type = config->service_type;
            dead.current_state = SERVICE_STOPPED;
            dead.process_id = 0;
            (void)write_status_file(name, dead);
            remove_dispatcher_handshake(status_path);
            error = error_codes::ERROR_SERVICE_REQUEST_TIMEOUT;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            VITREO_TRACE("[scm] StartService '%s': pid %d did not call "
                         "StartServiceCtrlDispatcher\n",
                         to_narrow(name).c_str(), static_cast<int>(pid));
            error = error_codes::ERROR_SERVICE_REQUEST_TIMEOUT;
            return false;
        }
        std::this_thread::sleep_for(kStartServiceDispatcherPoll);
    }
}

ServiceStatus ScmManager::query_status(const std::wstring& name) {
    if (is_builtin_native_service(name) && service_exists(name)) {
        // The native elevation broker is available for the lifetime of the
        // runtime. Reporting RUNNING is a real capability statement: a later
        // ShellExecute("runas") is gated by the guest-elevation consent dialog
        // and launches no elevated guest unless the user authorizes it.
        ServiceStatus status;
        status.service_type = SERVICE_WIN32_SHARE_PROCESS;
        status.current_state = SERVICE_RUNNING;
        status.process_id = static_cast<u32>(::getpid());
        return status;
    }

    ServiceStatus status;
    if (auto config = query_config(name)) {
        status.service_type = config->service_type;
        if ((config->service_type & SERVICE_KERNEL_DRIVER) != 0 &&
            m_kernel_driver_running) {
            // Kernel-device providers live in this Vitreo process. A RUNNING
            // record left by an earlier process cannot make the current
            // FileManager accept \\.\device opens, so the provider itself is
            // the authoritative live-state source.
            if (m_kernel_driver_running(name)) {
                status.current_state = SERVICE_RUNNING;
                status.controls_accepted = SERVICE_ACCEPT_STOP |
                                           SERVICE_ACCEPT_SHUTDOWN;
            }
            return status;
        }
    }

    auto stored = read_status_file(name);
    if (!stored) return status;  // Never started: SERVICE_STOPPED.

    // Windows SCM watches the service process. START_PENDING/RUNNING with no
    // live pid (including pid 0 from a crash before CreateProcess returned)
    // is STOPPED; otherwise StartService returns ERROR_SERVICE_ALREADY_RUNNING
    // and never relaunches.
    if (stored->current_state != SERVICE_STOPPED &&
        !process_is_live(static_cast<pid_t>(stored->process_id))) {
        VITREO_TRACE("[scm] service '%s' pid %u is gone -> SERVICE_STOPPED\n",
                     to_narrow(name).c_str(), stored->process_id);
        if (stored->process_id != 0) {
            int wait_status = 0;
            (void)::waitpid(static_cast<pid_t>(stored->process_id), &wait_status,
                            WNOHANG);
        }
        ServiceStatus dead = *stored;
        dead.current_state = SERVICE_STOPPED;
        dead.controls_accepted = 0;
        dead.process_id = 0;
        (void)write_status_file(name, dead);
        remove_dispatcher_handshake(status_file_path(name));
        return dead;
    }
    return *stored;
}

bool ScmManager::control_service(const std::wstring& name, u32 control,
                                 ServiceStatus& status_out, u32& error) {
    if (!service_exists(name)) {
        error = error_codes::ERROR_SERVICE_DOES_NOT_EXIST;
        return false;
    }

    ServiceStatus status = query_status(name);
    if (status.current_state == SERVICE_STOPPED) {
        status_out = status;
        error = error_codes::ERROR_SERVICE_NOT_ACTIVE;
        return false;
    }

    switch (control) {
        case SERVICE_CONTROL_INTERROGATE:
            // The current status IS the answer; nothing to deliver.
            status_out = status;
            error = error_codes::ERROR_SUCCESS;
            return true;

        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN: {
            if (status.process_id == 0) {
                if ((status.service_type & SERVICE_KERNEL_DRIVER) != 0 &&
                    m_kernel_driver_stop) {
                    m_kernel_driver_stop(name);
                    status.current_state = SERVICE_STOPPED;
                    status.controls_accepted = 0;
                    status.wait_hint = 0;
                    (void)write_status_file(name, status);
                    status_out = status;
                    error = error_codes::ERROR_SUCCESS;
                    VITREO_TRACE("[scm] ControlService STOP kernel driver "
                                 "'%s' -> native provider STOPPED\n",
                                 to_narrow(name).c_str());
                    return true;
                }
                status_out = status;
                error = error_codes::ERROR_SERVICE_NOT_ACTIVE;
                return false;
            }
            // See the header: the process is terminated rather than asked to
            // stop. SIGTERM is intercepted by the Vitreo worker as a host
            // graceful-shutdown of the whole guest, so the process stays
            // live while controllers poll QueryServiceStatusEx and then
            // OpenProcess the SERVICE_STATUS_PROCESS pid. Learn
            // OpenProcess: a pid of 0 (or a pid with no process object)
            // fails with ERROR_INVALID_PARAMETER. SIGKILL actually ends the
            // worker; reporting SERVICE_STOPPED is the controller-visible
            // outcome of a completed stop.
            VITREO_TRACE("[scm] ControlService STOP '%s' -> terminating pid %u"
                         " (handler not invoked; see scm.hpp)\n",
                         to_narrow(name).c_str(), status.process_id);
            (void)::kill(static_cast<pid_t>(status.process_id), SIGKILL);

            status.current_state = SERVICE_STOPPED;
            status.controls_accepted = 0;
            status.process_id = 0;
            status.wait_hint = 0;
            (void)write_status_file(name, status);
            status_out = status;
            error = error_codes::ERROR_SUCCESS;
            return true;
        }

        default:
            // Pause/continue and user-defined controls need the guest's
            // handler, which we cannot reach cross-process. Fail honestly.
            VITREO_TRACE("[scm] ControlService '%s' control %u unsupported\n",
                         to_narrow(name).c_str(), control);
            status_out = status;
            error = error_codes::ERROR_INVALID_SERVICE_CONTROL;
            return false;
    }
}

} // namespace vitreo::winapi
