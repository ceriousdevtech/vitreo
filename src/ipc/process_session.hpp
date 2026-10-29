// SPDX-License-Identifier: LGPL-2.1-only
/// @file process_session.hpp
/// @brief Ownership boundary for the host workers in one `vitreo run` session.

#pragma once

#include <vitreo/common.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace vitreo::ipc {

/// Windows token context selected by a trusted runtime launch path.
///
/// User workers use the normal manifest/consent elevation flow. Service
/// workers are created by the capsule SCM and receive their configured service
/// account token non-interactively, as they do under Windows.
enum class WorkerTokenContext : u8 {
    User = 0,
    LocalSystemService = 1,
    RestrictedService = 2,
};

/// Complete launch context for a Windows child process.
struct WorkerLaunchSpec {
    std::string host_image_path;
    std::string guest_image_path;
    std::string guest_command_line;
    std::string guest_current_directory;
    std::string capsule_name;
    std::string inherited_pipes;
    /// "in,out" kernel handle values of the pseudoconsole this child's console
    /// is, when launched with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
    std::string pseudo_console_handles;
    std::string inherited_sync;
    std::string inherited_mappings;
    /// UTF-16LE hexadecimal StartService arguments carried separately from
    /// the process command line so the worker can deliver ServiceMain argv.
    std::vector<std::string> guest_service_arguments_utf16;
    /// Security.framework external authorization form. A worker only presents
    /// an elevated guest token after validating this credential.
    std::string elevation_credential;
    /// Trusted service-account context. ProcessSession conveys this through an
    /// inherited pipe capability; environment variables alone cannot select
    /// an elevated token.
    WorkerTokenContext token_context{WorkerTokenContext::User};

    /// The worker is not waited on, and receives no parent-death pipe.
    ///
    /// CreateProcess children are not in a job and are not terminated when
    /// their creator exits. `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` is the only
    /// creator-lifetime kill, and only for processes assigned to that job
    /// when its last handle closes. A detached worker is
    /// `CREATE_BREAKAWAY_FROM_JOB` or a service the SCM started for a
    /// requester: the session never blocks waiting for it.
    bool detached{false};

    /// Extra "NAME=VALUE" entries added to the child's environment on top of
    /// the parent's. Used to hand a launched process facts it must know before
    /// it runs any guest code — e.g. VITREO_SERVICE_NAME, which tells a service
    /// binary which service it was started as. Passing them here (via execve)
    /// rather than setenv() avoids mutating this multi-threaded process's own
    /// environment just to reach a child.
    std::vector<std::string> extra_env;

    /// Guest Win32 environment variables ("NAME=VALUE") applied inside the
    /// child Vitreo worker via repeated `--env` arguments. Used to honor
    /// CreateProcess lpEnvironment / parent-env inheritance so browser
    /// children see VK_ICD_FILENAMES and other vars the parent set.
    std::vector<std::string> guest_environment;

    /// Normally discovered from the current host process. Tests may provide a
    /// small executable that records its argv.
    std::string runtime_path;
};

/// Remove @p link_path only when it is still a symlink resolving to
/// @p expected_target.
///
/// Capsule-side application mappings are shared ground between a launch, its
/// children, and any concurrent launch. Verifying the target before unlinking
/// means a replaced mapping, or real capsule content that took the name, is
/// never destroyed by a departing session.
///
/// @return true when the expected symlink was removed.
bool remove_mapped_symlink(const std::string& link_path,
                           const std::string& expected_target);

/// Consume and validate the one-shot token-context capability inherited by a
/// supervised worker. The descriptor is always closed. Returns nullopt for a
/// malformed, forged, or already-consumed capability.
[[nodiscard]] std::optional<WorkerTokenContext>
consume_worker_token_context_capability(int fd);

/// Watch the parent-death descriptor inherited by a supervised worker.
///
/// The parent holds the sole write end of @p death_read_fd. When that end
/// closes — normal exit, `SIGKILL`, or a host crash — the worker keeps
/// running. CreateProcess does not terminate a child because its creator
/// exited. `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` terminates only processes
/// assigned to that job, when the job's last handle closes. This is a no-op
/// when @p death_read_fd is < 0.
///
/// The descriptor is marked close-on-exec so it is not inherited by this
/// worker's own launched children (they receive their own death pipe).
void install_parent_death_watch(int death_read_fd);

/// Owns the direct host children created by one Vitreo worker.
///
/// Each Windows process still receives an independent host process and guest
/// address space. The originating `vitreo run` invocation remains at the root
/// of that host process tree and does not return until all directly owned
/// workers have exited; descendants are handled recursively by their workers.
class ProcessSession {
public:
    /// Registers this session as the reaper's child-exit observer: a direct
    /// child is reaped (waitpid) and its parent-death pipe closed as soon as
    /// it exits, instead of staying <defunct> until this process ends.
    ProcessSession();
    ~ProcessSession();

    ProcessSession(const ProcessSession&) = delete;
    ProcessSession& operator=(const ProcessSession&) = delete;
    ProcessSession(ProcessSession&&) = delete;
    ProcessSession& operator=(ProcessSession&&) = delete;

    /// Snapshot the guest image and exec a directly owned Vitreo worker.
    /// Returns the real host pid, or -1 when the snapshot/fork/exec failed.
    [[nodiscard]] pid_t launch_worker(const WorkerLaunchSpec& spec);

    /// Reap every directly owned worker. Normal Windows parent exit semantics
    /// are preserved: this waits; it does not terminate a still-running child.
    ///
    /// @p grace false (a worker process that has a guest parent): Windows
    /// does not keep a process alive for its children, so already-exited
    /// children are reaped and live ones released without the 2 s wait a
    /// top-level run gives each of them.
    void wait_for_workers(bool grace = true);

    /// Reaper callback: reap @p pid if it is a worker of this session.
    void on_child_exit(pid_t pid);

    /// Keep a parent-owned capsule symlink alive until all directly owned
    /// workers have exited, then remove it. This is used for capsule app
    /// mappings that child processes still need for DLL loading and signature
    /// validation.
    ///
    /// The link is removed only when it is still a symlink pointing at
    /// @p expected_target, so a mapping that was replaced by another session or
    /// by real capsule content is never destroyed here.
    void defer_symlink_cleanup(std::string link_path,
                               std::string expected_target,
                               std::string lease_path = {},
                               int lease_fd = -1);

    [[nodiscard]] usize owned_worker_count() const;

private:
    struct DeferredSymlink {
        std::string link_path;
        std::string expected_target;
        std::string lease_path;
        int lease_fd{-1};
    };

    /// A directly owned worker and the write end of its parent-death pipe.
    /// This process holds @p death_fd open for the worker's lifetime; closing
    /// it (including implicitly, when this process dies) signals the worker to
    /// terminate. It is closed after the worker is reaped.
    struct OwnedWorker {
        pid_t pid{-1};
        int death_fd{-1};
        /// A detached worker (CREATE_BREAKAWAY_FROM_JOB / service) that this
        /// session must not block on and does not tear down. It is reaped
        /// non-blockingly if it has already exited, otherwise abandoned to be
        /// reparented on this process's own exit.
        bool detached{false};
    };

    mutable std::mutex m_mutex;
    std::vector<OwnedWorker> m_workers;
    std::vector<DeferredSymlink> m_deferred_symlinks;
};

} // namespace vitreo::ipc
