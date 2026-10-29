// SPDX-License-Identifier: LGPL-2.1-only
/// @file process_session.cpp

#include "ipc/process_session.hpp"
#include "ipc/macos_app_identity.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>
#include <fcntl.h>
#include <limits.h>
#include <libproc.h>
#include <string_view>
#include <unordered_set>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;

namespace vitreo::ipc {

namespace {

bool copy_file(const std::string& source, const std::string& destination) {
    int src_fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) return false;

    int dst_fd = ::open(destination.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    if (dst_fd < 0) {
        ::close(src_fd);
        return false;
    }

    char buffer[64 * 1024];
    bool ok = true;
    while (ok) {
        const ssize_t read_size = ::read(src_fd, buffer, sizeof(buffer));
        if (read_size == 0) break;
        if (read_size < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }

        ssize_t offset = 0;
        while (offset < read_size) {
            const ssize_t written = ::write(
                dst_fd, buffer + offset,
                static_cast<size_t>(read_size - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            offset += written;
        }
    }

    ::close(src_fd);
    ::close(dst_fd);
    return ok;
}

std::string make_snapshot_path() {
    const char* temp_dir = ::getenv("TMPDIR");
    if (temp_dir == nullptr || temp_dir[0] == '\0') temp_dir = "/tmp";

    char path[PATH_MAX];
    std::snprintf(
        path, sizeof(path), "%s/vitreo_launch_%d_%lld.exe", temp_dir,
        static_cast<int>(::getpid()),
        static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    return path;
}

std::string current_runtime_path() {
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (::proc_pidpath(::getpid(), path, sizeof(path)) <= 0) return {};
    return path;
}

std::string hex_encode(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0F]);
    }
    return encoded;
}

constexpr std::array<u8, 8> token_context_payload(
    WorkerTokenContext context) {
    return {
        'V', 'T', 'K', '1',
        static_cast<u8>(context),
        0xA7, 0x3C, 0x5E,
    };
}

bool inherits_process_handle_for_pid(std::string_view descriptors,
                                     pid_t pid) {
    usize offset = 0;
    while (offset < descriptors.size()) {
        const usize end = descriptors.find(';', offset);
        const std::string_view item = descriptors.substr(
            offset, end == std::string_view::npos
                        ? descriptors.size() - offset
                        : end - offset);
        if (item.starts_with("P,")) {
            const usize pid_separator = item.find(',', 2);
            if (pid_separator != std::string_view::npos) {
                i64 inherited_pid = -1;
                const std::string_view pid_text =
                    item.substr(pid_separator + 1);
                const auto parsed = std::from_chars(
                    pid_text.data(), pid_text.data() + pid_text.size(),
                    inherited_pid);
                if (parsed.ec == std::errc{} &&
                    parsed.ptr == pid_text.data() + pid_text.size() &&
                    inherited_pid == static_cast<i64>(pid)) {
                    return true;
                }
            }
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return false;
}

} // namespace

std::optional<WorkerTokenContext>
consume_worker_token_context_capability(int fd) {
    if (fd < 3) return std::nullopt;

    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISFIFO(status.st_mode)) {
        (void)::close(fd);
        return std::nullopt;
    }

    std::array<u8, 8> payload{};
    usize offset = 0;
    while (offset < payload.size()) {
        const ssize_t count =
            ::read(fd, payload.data() + offset, payload.size() - offset);
        if (count > 0) {
            offset += static_cast<usize>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        (void)::close(fd);
        return std::nullopt;
    }
    (void)::close(fd);

    const auto context =
        static_cast<WorkerTokenContext>(payload[4]);
    if (context != WorkerTokenContext::LocalSystemService &&
        context != WorkerTokenContext::RestrictedService) {
        return std::nullopt;
    }
    return payload == token_context_payload(context)
               ? std::optional<WorkerTokenContext>{context}
               : std::nullopt;
}

/// Written by a parent that is shutting down through its normal path to tell a
/// still-running worker it is being handed over rather than torn down. EOF
/// without it still means the parent died abnormally, which is the case the
/// pipe exists to catch.
constexpr char kWorkerReleaseByte = 'K';

void install_parent_death_watch(int death_read_fd) {
    if (death_read_fd < 0) return;

    // Do not let this worker's own launched children inherit the descriptor;
    // each child receives its own parent-death pipe from launch_worker().
    const int fd_flags = ::fcntl(death_read_fd, F_GETFD);
    if (fd_flags >= 0) {
        (void)::fcntl(death_read_fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }

    std::thread watcher([death_read_fd] {
        // Block until the parent closes the sole write end. A parent that exits
        // for any reason — clean, SIGKILL, or a crash — closes it, yielding EOF
        // (read() == 0). A stray byte is not expected but is treated the same:
        // the parent is the only writer and never writes here.
        char scratch = 0;
        ssize_t got = 0;
        do {
            got = ::read(death_read_fd, &scratch, 1);
        } while (got < 0 && errno == EINTR);

        // CreateProcess does not associate the child with a job. The child
        // keeps running when its creator exits, including an abnormal exit
        // that closes this pipe without the release byte. Termination is
        // only JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: the last handle of a job
        // that has that limit (Kernel32ExtManager::on_job_handle_close /
        // begin_shutdown). Killing here made every installer and helper die
        // with its launcher.
        (void)scratch;
        (void)got;
        ipc_log("creator closed parent-death pipe fd=%d; worker pid=%d continues",
                death_read_fd, static_cast<int>(::getpid()));
        std::fprintf(stderr,
                     "[process_session] creator closed parent-death pipe "
                     "fd=%d pid=%d — worker continues\n",
                     death_read_fd, static_cast<int>(::getpid()));
        std::fflush(stderr);
        (void)::close(death_read_fd);
    });
    watcher.detach();
    std::fprintf(stderr,
                 "[process_session] parent-death watch installed fd=%d pid=%d\n",
                 death_read_fd, static_cast<int>(::getpid()));
    std::fflush(stderr);
}

pid_t ProcessSession::launch_worker(const WorkerLaunchSpec& spec) {
    if (spec.host_image_path.empty() || spec.guest_image_path.empty()) {
        return -1;
    }

    const std::string runtime_path =
        spec.runtime_path.empty() ? current_runtime_path() : spec.runtime_path;
    if (runtime_path.empty()) return -1;

    // CreateProcess returns immediately and the child runs independently.
    // A child that inherits a real handle to its creator can explicitly wait
    // for that process object to become signaled. Keeping such a child under
    // this creator's recursive wait ownership reverses that dependency:
    // creator waits for child while child waits for creator termination.
    // Let it outlive the creator just like CREATE_BREAKAWAY_FROM_JOB workers.
    const bool observes_creator_process = inherits_process_handle_for_pid(
        spec.inherited_sync, ::getpid());
    const bool detached = spec.detached || observes_creator_process;
    if (observes_creator_process && !spec.detached) {
        VITREO_TRACE(
            "[process_session] worker inherits creator process handle; "
            "using independent lifetime\n");
    }

    const std::string snapshot_path = make_snapshot_path();
    if (!copy_file(spec.host_image_path, snapshot_path)) {
        (void)::unlink(snapshot_path.c_str());
        return -1;
    }

    // Parent-death pipe: the worker inherits the read end and self-terminates
    // when this process closes the write end — which the OS does on any exit,
    // including a crash or SIGKILL that never runs wait_for_workers(). Without
    // it, an abnormal death of the owning process orphans the whole worker tree
    // to launchd, leaking its committed memory. The write end is retained here
    // (see OwnedWorker::death_fd) and closed after the worker is reaped.
    //
    // A detached worker (CREATE_BREAKAWAY_FROM_JOB / service) must outlive its
    // launcher, so it gets no death pipe: close(-1) below is a harmless no-op.
    int death_pipe[2] = {-1, -1};
    if (!detached && ::pipe(death_pipe) != 0) {
        (void)::unlink(snapshot_path.c_str());
        return -1;
    }
    // wait_for_workers writes the release byte after the worker may already
    // have exited (a Windows child routinely ends before its creator); that
    // must be EPIPE, not a SIGPIPE that kills this process before it
    // publishes its own exit code.
    suppress_sigpipe(death_pipe[1]);

    // Windows ExitProcess codes are a full DWORD; host wait status only keeps
    // 8 bits. The child writes the DWORD to this pipe before exiting.
    int exit_code_pipe[2] = {-1, -1};
    if (::pipe(exit_code_pipe) != 0) {
        ::close(death_pipe[0]);
        ::close(death_pipe[1]);
        (void)::unlink(snapshot_path.c_str());
        return -1;
    }

    int token_context_fd = -1;
    if (spec.token_context != WorkerTokenContext::User) {
        int capability_pipe[2] = {-1, -1};
        if (::pipe(capability_pipe) != 0) {
            ::close(death_pipe[0]);
            ::close(death_pipe[1]);
            ::close(exit_code_pipe[0]);
            ::close(exit_code_pipe[1]);
            (void)::unlink(snapshot_path.c_str());
            return -1;
        }
        const auto payload = token_context_payload(spec.token_context);
        const ssize_t written =
            ::write(capability_pipe[1], payload.data(), payload.size());
        (void)::close(capability_pipe[1]);
        if (written != static_cast<ssize_t>(payload.size())) {
            (void)::close(capability_pipe[0]);
            ::close(death_pipe[0]);
            ::close(death_pipe[1]);
            ::close(exit_code_pipe[0]);
            ::close(exit_code_pipe[1]);
            (void)::unlink(snapshot_path.c_str());
            return -1;
        }
        token_context_fd = capability_pipe[0];
        const int flags = ::fcntl(token_context_fd, F_GETFL);
        if (flags >= 0) {
            (void)::fcntl(token_context_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    // Keep argument storage alive until posix_spawn consumes it.
    std::vector<const char*> argv;
    const std::string elevation_credential =
        hex_encode(spec.elevation_credential);
    const std::string token_context_fd_text =
        token_context_fd >= 0 ? std::to_string(token_context_fd)
                              : std::string{};
    const std::string death_fd_text =
        death_pipe[0] >= 0 ? std::to_string(death_pipe[0]) : std::string{};
    const std::string exit_code_fd_text =
        std::to_string(exit_code_pipe[1]);
    const std::string guest_parent_pid_text = std::to_string(::getpid());
    argv.push_back(runtime_path.c_str());
    argv.push_back("run");
    argv.push_back("--relaunched-as");
    argv.push_back(spec.guest_image_path.c_str());
    if (!spec.guest_command_line.empty()) {
        argv.push_back("--guest-cmdline");
        argv.push_back(spec.guest_command_line.c_str());
    }
    if (!spec.guest_current_directory.empty()) {
        argv.push_back("--guest-cwd");
        argv.push_back(spec.guest_current_directory.c_str());
    }
    if (!spec.capsule_name.empty()) {
        argv.push_back("--capsule");
        argv.push_back(spec.capsule_name.c_str());
    }
    if (!spec.inherited_pipes.empty()) {
        argv.push_back("--inherited-pipes");
        argv.push_back(spec.inherited_pipes.c_str());
    }
    if (!spec.pseudo_console_handles.empty()) {
        argv.push_back("--conpty-handles");
        argv.push_back(spec.pseudo_console_handles.c_str());
    }
    if (!spec.inherited_sync.empty()) {
        argv.push_back("--inherited-sync");
        argv.push_back(spec.inherited_sync.c_str());
    }
    if (!spec.inherited_mappings.empty()) {
        argv.push_back("--inherited-mappings");
        argv.push_back(spec.inherited_mappings.c_str());
    }
    for (const auto& argument : spec.guest_service_arguments_utf16) {
        argv.push_back("--guest-service-arg-utf16");
        argv.push_back(argument.c_str());
    }
    if (!elevation_credential.empty()) {
        argv.push_back("--guest-elevation-credential");
        argv.push_back(elevation_credential.c_str());
    }
    if (!token_context_fd_text.empty()) {
        argv.push_back("--guest-token-context-fd");
        argv.push_back(token_context_fd_text.c_str());
    }
    // Capture the Windows creator identity before spawning. A detached or
    // short-lived creator can exit while the child is still initializing, at
    // which point host getppid() changes even though the Windows relationship
    // represented by PROCESS_BASIC_INFORMATION does not.
    argv.push_back("--guest-parent-pid");
    argv.push_back(guest_parent_pid_text.c_str());
    // The worker watches this descriptor and self-terminates when this process
    // (its owner) closes the write end, i.e. exits for any reason. Omitted for
    // detached workers, which must outlive their launcher.
    if (!death_fd_text.empty()) {
        argv.push_back("--parent-death-fd");
        argv.push_back(death_fd_text.c_str());
    }
    argv.push_back("--guest-exit-code-fd");
    argv.push_back(exit_code_fd_text.c_str());
    // Guest Win32 env must outlive argv pointers. Store locally before push.
    std::vector<std::string> guest_env_storage = spec.guest_environment;
    for (const auto& entry : guest_env_storage) {
        if (entry.find('=') == std::string::npos) {
            continue;
        }
        argv.push_back("--env");
        argv.push_back(entry.c_str());
    }
    argv.push_back(snapshot_path.c_str());
    argv.push_back(nullptr);

    // Every CreateProcess descendant belongs to the originating macOS
    // application presentation. The worker remains a full, independent guest
    // process, but AppKit must not publish a separate Dock tile for it.
    std::vector<std::string> launch_environment = spec.extra_env;
    std::erase_if(launch_environment, [](const std::string& entry) {
        return entry.starts_with("VITREO_MACOS_APP_SESSION=");
    });
    launch_environment.emplace_back("VITREO_MACOS_APP_SESSION=" +
                                    macos_app_identity());
    const auto role_entry = std::find_if(
        launch_environment.begin(), launch_environment.end(),
        [](const std::string& entry) {
            return entry.starts_with("VITREO_MACOS_PROCESS_ROLE=");
        });
    if (role_entry == launch_environment.end()) {
        launch_environment.emplace_back(
            "VITREO_MACOS_PROCESS_ROLE=child");
    } else {
        *role_entry = "VITREO_MACOS_PROCESS_ROLE=child";
    }

    // Extra environment entries replace inherited entries with the same name;
    // duplicate names in envp have implementation-defined lookup order and could otherwise make a
    // service inherit its parent's VITREO_SERVICE_NAME.
    std::vector<std::string> environment;
    std::vector<const char*> envp;
    if (!launch_environment.empty()) {
        std::unordered_set<std::string> override_names;
        for (const auto& entry : launch_environment) {
            const usize separator = entry.find('=');
            if (separator == 0 || separator == std::string::npos) {
                ::close(death_pipe[0]);
                ::close(death_pipe[1]);
                ::close(exit_code_pipe[0]);
                ::close(exit_code_pipe[1]);
                if (token_context_fd >= 0) {
                    (void)::close(token_context_fd);
                }
                (void)::unlink(snapshot_path.c_str());
                return -1;
            }
            override_names.insert(entry.substr(0, separator));
        }

        for (char** e = ::environ; e != nullptr && *e != nullptr; ++e) {
            const char* separator = std::strchr(*e, '=');
            const std::string name =
                separator == nullptr
                    ? std::string(*e)
                    : std::string(*e, static_cast<usize>(separator - *e));
            // A service's identity is never inherited: a process a service
            // creates is an ordinary process (Learn "Service Programs"), not
            // a second instance of the service.
            if (!override_names.contains(name) && name != "VITREO_SERVICE_NAME") {
                environment.emplace_back(*e);
            }
        }
        for (const auto& entry : launch_environment) {
            environment.push_back(entry);
        }

        envp.reserve(environment.size() + 1);
        for (const auto& entry : environment) {
            envp.push_back(entry.c_str());
        }
        envp.push_back(nullptr);
    }

    // Darwin fork freezes allocator activity in every parent thread while
    // cloning the address space. Spawn directly into the worker image instead.
    // File actions preserve the same child-only closures as fork/exec.
    posix_spawn_file_actions_t actions;
    int spawn_error = ::posix_spawn_file_actions_init(&actions);
    const bool actions_initialized = spawn_error == 0;
    if (spawn_error == 0 && death_pipe[1] >= 0) {
        spawn_error = ::posix_spawn_file_actions_addclose(&actions, death_pipe[1]);
    }
    if (spawn_error == 0) {
        spawn_error = ::posix_spawn_file_actions_addclose(&actions, exit_code_pipe[0]);
    }
    pid_t child = -1;
    if (spawn_error == 0) {
        spawn_error = ::posix_spawn(
            &child, runtime_path.c_str(), &actions, nullptr,
            const_cast<char* const*>(argv.data()),
            envp.empty() ? ::environ : const_cast<char* const*>(envp.data()));
    }
    if (actions_initialized) (void)::posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        ::close(death_pipe[0]);
        ::close(death_pipe[1]);
        ::close(exit_code_pipe[0]);
        ::close(exit_code_pipe[1]);
        if (token_context_fd >= 0) (void)::close(token_context_fd);
        (void)::unlink(snapshot_path.c_str());
        errno = spawn_error;
        return -1;
    }

    if (token_context_fd >= 0) {
        (void)::close(token_context_fd);
    }
    // Retain the write end of the parent-death pipe for the worker's lifetime.
    // Mark it close-on-exec so a subsequently launched sibling worker does not
    // inherit a copy; otherwise this worker's death pipe would stay "open" from
    // the sibling and never signal EOF when this process dies. Detached workers
    // have no death pipe (death_pipe == {-1, -1}); close(-1) is a no-op.
    ::close(death_pipe[0]);
    if (death_pipe[1] >= 0) {
        const int flags = ::fcntl(death_pipe[1], F_GETFD);
        if (flags >= 0) {
            (void)::fcntl(death_pipe[1], F_SETFD, flags | FD_CLOEXEC);
        }
    }
    // Parent keeps the Windows exit-code read end on the ProcessObject.
    ::close(exit_code_pipe[1]);
    if (exit_code_pipe[0] >= 0) {
        const int flags = ::fcntl(exit_code_pipe[0], F_GETFD);
        if (flags >= 0) {
            (void)::fcntl(exit_code_pipe[0], F_SETFD, flags | FD_CLOEXEC);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workers.push_back(OwnedWorker{child, death_pipe[1], detached});
    }
    if (auto* process = track_process(child)) {
        process->attach_windows_exit_fd(exit_code_pipe[0]);
        exit_code_pipe[0] = -1; // ownership transferred
    } else {
        ::close(exit_code_pipe[0]);
    }
    VITREO_TRACE(
        "[process_session] owns pid=%d image='%s' capsule='%s' cwd='%s'\n",
        static_cast<int>(child), spec.guest_image_path.c_str(),
        spec.capsule_name.c_str(), spec.guest_current_directory.c_str());
    ipc_log("session owns worker pid=%d image='%s' capsule='%s' cwd='%s'",
            static_cast<int>(child), spec.guest_image_path.c_str(),
            spec.capsule_name.c_str(), spec.guest_current_directory.c_str());
    return child;
}

ProcessSession::ProcessSession() {
    set_child_exit_observer([this](pid_t pid) { on_child_exit(pid); });
}

ProcessSession::~ProcessSession() {
    set_child_exit_observer(nullptr);
}

void ProcessSession::on_child_exit(pid_t pid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_workers.begin(); it != m_workers.end(); ++it) {
        if (it->pid != pid) continue;
        int status = 0;
        pid_t reaped = 0;
        do {
            reaped = ::waitpid(pid, &status, WNOHANG);
        } while (reaped < 0 && errno == EINTR);
        // 0: NOTE_EXIT raced ahead of the zombie becoming waitable; the
        // worker stays listed and wait_for_workers reaps it later.
        if (reaped == 0) return;
        if (it->death_fd >= 0) (void)::close(it->death_fd);
        ipc_log("session reaped exited worker pid=%d status=0x%x",
                static_cast<int>(pid), status);
        m_workers.erase(it);
        return;
    }
}

void ProcessSession::wait_for_workers(bool grace) {
    std::vector<OwnedWorker> workers;
    std::vector<DeferredSymlink> deferred_symlinks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        workers.swap(m_workers);
        deferred_symlinks.swap(m_deferred_symlinks);
    }

    // Tell every worker we are leaving normally before waiting on any of them.
    // A worker that finishes on its own is still reaped below; one that is
    // still running when this session ends keeps running, which is what
    // Windows does with a child whose creator exited.
    for (const OwnedWorker& worker : workers) {
        if (worker.detached || worker.death_fd < 0) continue;
        const char release = kWorkerReleaseByte;
        ssize_t wrote = 0;
        do {
            wrote = ::write(worker.death_fd, &release, 1);
        } while (wrote < 0 && errno == EINTR);
    }

    // A worker still running when this session ends keeps resolving paths
    // through the deferred mappings -- its own image, sibling DLLs, and the
    // image another process verifies after opening it by PID. Windows keeps
    // a file reachable for as long as it exists, not for as long as the
    // process that first ran it.
    bool workers_left_running = false;
    for (const OwnedWorker& worker : workers) {
        int status = 0;
        if (worker.detached) {
            // A detached worker (CREATE_BREAKAWAY_FROM_JOB / service) must
            // outlive its launcher. Never block on it: reap it only if it has
            // already exited, otherwise abandon it to be reparented to launchd
            // on this process's own exit. It holds no death pipe.
            const pid_t reaped = ::waitpid(worker.pid, &status, WNOHANG);
            if (reaped != worker.pid) workers_left_running = true;
            VITREO_TRACE(
                "[process_session] detached worker pid=%d %s\n",
                static_cast<int>(worker.pid),
                reaped == worker.pid ? "already exited (reaped)" : "left running");
            continue;
        }
        VITREO_TRACE("[process_session] waiting for pid=%d\n",
                     static_cast<int>(worker.pid));

        // Give the worker a grace period to exit on its own, then TELL it we
        // are leaving before blocking on it.
        //
        // Closing the parent-death write end is what wakes the worker's watch
        // thread (it sits in read() on the read end). Doing that only *after*
        // an unbounded waitpid() deadlocks whenever the worker is waiting to be
        // dismissed rather than finishing on its own: parent waits for the
        // child to exit, child waits to be told to exit, neither moves.
        // A 64-bit installer helper is exactly that shape: it answers COM
        // calls until its launcher dismisses it, and waiting on it first hung
        // the title installer at its status page.
        //
        // The grace period keeps the good case intact: a worker that exits by
        // itself is reaped normally and never sees a SIGKILL, so it is not cut
        // off mid-write.
        bool exited = false;
        if (!grace) {
            // A worker process with a guest parent: Windows does not keep a
            // process alive for its children, so reap what has already
            // exited and release the rest without waiting (a 2 s grace per
            // live child made every child exit late to its own parent).
            const pid_t reaped = ::waitpid(worker.pid, &status, WNOHANG);
            exited = reaped == worker.pid || (reaped < 0 && errno == ECHILD);
        } else {
            constexpr int kGraceMs = 2000;
            constexpr int kPollMs = 20;
            for (int waited = 0; waited < kGraceMs; waited += kPollMs) {
                const pid_t reaped = ::waitpid(worker.pid, &status, WNOHANG);
                if (reaped == worker.pid) { exited = true; break; }
                if (reaped < 0 && errno != EINTR) { exited = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            }
        }
        if (!exited) {
            VITREO_TRACE(
                "[process_session] pid=%d still running after grace; "
                "releasing parent-death pipe\n",
                static_cast<int>(worker.pid));
            if (worker.death_fd >= 0) {
                (void)::close(worker.death_fd);
            }
            // Released above: reap it if it has already finished, otherwise
            // leave it running instead of blocking on a process that is now
            // independent of this one.
            if (::waitpid(worker.pid, &status, WNOHANG) != worker.pid) {
                workers_left_running = true;
            }
        } else if (worker.death_fd >= 0) {
            // Already gone; closing just reclaims the descriptor.
            (void)::close(worker.death_fd);
        }
        VITREO_TRACE("[process_session] reaped pid=%d status=0x%x\n",
                     static_cast<int>(worker.pid), status);
        ipc_log("session reaped worker pid=%d", static_cast<int>(worker.pid));
    }

    for (const auto& mapping : deferred_symlinks) {
        if (workers_left_running) {
            // Leave the link and its lease file. The lease descriptor was
            // inherited by every worker (it is not close-on-exec), so its
            // flock stays held until the last of them exits; after that a
            // later launch reclaims the unleased link (external_app_mapping).
            VITREO_TRACE(
                "[process_session] workers still running; keeping mapping "
                "link='%s'\n",
                mapping.link_path.c_str());
            if (mapping.lease_fd >= 0) (void)::close(mapping.lease_fd);
            continue;
        }
        if (remove_mapped_symlink(mapping.link_path, mapping.expected_target)) {
            VITREO_TRACE(
                "[process_session] removed deferred mapping link='%s' "
                "target='%s'\n",
                mapping.link_path.c_str(), mapping.expected_target.c_str());
        }
        if (!mapping.lease_path.empty()) {
            (void)::unlink(mapping.lease_path.c_str());
        }
        if (mapping.lease_fd >= 0) (void)::close(mapping.lease_fd);
    }
}

void ProcessSession::defer_symlink_cleanup(std::string link_path,
                                           std::string expected_target,
                                           std::string lease_path,
                                           int lease_fd) {
    if (link_path.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(
        m_deferred_symlinks.begin(), m_deferred_symlinks.end(),
        [&link_path](const DeferredSymlink& entry) {
            return entry.link_path == link_path;
        });
    if (existing == m_deferred_symlinks.end()) {
        m_deferred_symlinks.push_back(DeferredSymlink{
            std::move(link_path), std::move(expected_target),
            std::move(lease_path), lease_fd});
    } else if (lease_fd >= 0) {
        (void)::close(lease_fd);
    }
}

bool remove_mapped_symlink(const std::string& link_path,
                           const std::string& expected_target) {
    namespace fs = std::filesystem;
    if (link_path.empty()) return false;

    std::error_code ec;
    const auto link_status = fs::symlink_status(link_path, ec);
    if (ec || !fs::exists(link_status)) return false;
    if (!fs::is_symlink(link_status)) {
        VITREO_TRACE(
            "[process_session] mapping '%s' is no longer a symlink; "
            "leaving it\n",
            link_path.c_str());
        return false;
    }

    const fs::path current = fs::read_symlink(link_path, ec);
    if (ec || current.string() != expected_target) {
        VITREO_TRACE(
            "[process_session] mapping '%s' now points at '%s' "
            "(expected '%s'); leaving it\n",
            link_path.c_str(), ec ? "<unreadable>" : current.c_str(),
            expected_target.c_str());
        return false;
    }

    fs::remove(link_path, ec);
    if (ec) {
        VITREO_TRACE(
            "[process_session] failed to remove mapping '%s': %s\n",
            link_path.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

usize ProcessSession::owned_worker_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workers.size();
}

} // namespace vitreo::ipc
