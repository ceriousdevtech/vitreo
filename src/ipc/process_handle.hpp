// SPDX-License-Identifier: LGPL-2.1-only
/// @file process_handle.hpp
/// @brief Track a real host child process by pid and expose its liveness as an
///        ntsync event, so a guest HANDLE to a process supports
///        WaitForSingleObject / GetExitCodeProcess across the forked Vitreo
///        host processes that back guest processes (Stage 1 of cross-process IPC).

#pragma once

#include <functional>

#include <vitreo/common.hpp>

#include <atomic>
#include <sys/types.h>

namespace vitreo::ipc {

/// Guest-visible startup facts which a parent may query for a process it
/// created. These describe the independent guest address space, not host
/// pointers in the worker process.
struct GuestProcessMetadata {
    bool is_64bit{false};
    u64 peb_address{0};
    u64 image_base{0};
    bool remote_memory_available{false};
};

/// Liveness state for a tracked host process. Stable for the lifetime of the
/// host process (the reaper never frees these — child-process count is tiny).
struct ProcessObject {
    pid_t            pid{0};
    u64              exit_event{0};            ///< ntsync manual-reset event id
    std::atomic<bool> exited{false};
    /// Set by the first exit notification; `exited` follows once exit_code
    /// holds the final value.
    std::atomic<bool> exit_claimed{false};
    std::atomic<u32> exit_code{0};
    std::atomic<bool> termination_requested{false};
    std::atomic<u32>  termination_exit_code{0};
    /// Parent read end of the Windows ExitProcess-code pipe. Host wait status
    /// only carries 8 bits; the child writes the full DWORD before exiting.
    int windows_exit_read_fd{-1};

    /// Cached FILETIME (100ns since 1601) for GetProcessTimes after the host
    /// pid is reaped and proc_pid_rusage no longer works. exit_filetime is 0
    /// while the process is still running (Learn).
    std::atomic<u64> creation_filetime{0};
    std::atomic<u64> exit_filetime{0};
    std::atomic<u64> kernel_filetime{0};
    std::atomic<u64> user_filetime{0};
    std::atomic<bool> times_cached{false};

    /// WaitForInputIdle (Learn): once a process has been observed idle, later
    /// WaitForInputIdle calls return immediately without waiting again.
    std::atomic<bool> input_idle_latched{false};

    void attach_windows_exit_fd(int read_fd) noexcept {
        if (windows_exit_read_fd >= 0 && windows_exit_read_fd != read_fd) {
            // Replaced; close the previous descriptor.
            // Defined out-of-line to avoid pulling unistd into every TU.
            close_windows_exit_fd();
        }
        windows_exit_read_fd = read_fd;
    }

    void close_windows_exit_fd() noexcept;

    void publish_guest_metadata(const GuestProcessMetadata& metadata) noexcept {
        guest_is_64bit.store(metadata.is_64bit, std::memory_order_relaxed);
        guest_peb_address.store(metadata.peb_address, std::memory_order_relaxed);
        guest_image_base.store(metadata.image_base, std::memory_order_relaxed);
        guest_remote_memory_available.store(
            metadata.remote_memory_available, std::memory_order_relaxed);
        guest_metadata_ready.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool guest_metadata(
        GuestProcessMetadata& metadata) const noexcept {
        if (!guest_metadata_ready.load(std::memory_order_acquire)) return false;
        metadata.is_64bit =
            guest_is_64bit.load(std::memory_order_relaxed);
        metadata.peb_address =
            guest_peb_address.load(std::memory_order_relaxed);
        metadata.image_base =
            guest_image_base.load(std::memory_order_relaxed);
        metadata.remote_memory_available =
            guest_remote_memory_available.load(std::memory_order_relaxed);
        return metadata.peb_address != 0 && metadata.image_base != 0;
    }

private:
    std::atomic<bool> guest_metadata_ready{false};
    std::atomic<bool> guest_is_64bit{false};
    std::atomic<u64> guest_peb_address{0};
    std::atomic<u64> guest_image_base{0};
    std::atomic<bool> guest_remote_memory_available{false};
};

/// Begin tracking @p pid via a background kqueue/NOTE_EXIT reaper. Returns a
/// stable pointer whose `exit_event` fires (and `exit_code` is set) when the
/// process exits. Idempotent per pid. Returns nullptr only if @p pid <= 0.
[[nodiscard]] ProcessObject* track_process(pid_t pid);

/// Non-creating lookup for a pid already passed to @ref track_process.
/// Returns the stable ProcessObject even after the host process has exited
/// (Windows keeps the process object alive while handles remain open).
[[nodiscard]] ProcessObject* find_tracked_process(pid_t pid);

/// Called on the reaper thread once a tracked pid has exited and its exit
/// code and times are recorded, so the owner of a direct child can reap it
/// (waitpid) and drop its per-child resources right away. Windows keeps no
/// zombie: the process object lives on only through open handles, which the
/// ProcessObject provides. One observer (the process's ProcessSession);
/// pass nullptr to clear it.
using ChildExitObserver = std::function<void(pid_t)>;
void set_child_exit_observer(ChildExitObserver observer);

/// Create a process object that has *already* exited with @p exit_code, with
/// no host process behind it. Used for system executables the runtime
/// implements itself rather than loading as a PE: the caller still gets a
/// waitable handle carrying a real exit code, which is all a parent that does
/// CreateProcess/WaitForSingleObject/GetExitCodeProcess can observe.
///
/// The exit event is created already signaled — a process handle whose event
/// never fires makes the parent's wait fail outright rather than return.
[[nodiscard]] ProcessObject* create_completed_process(u32 exit_code);

/// Forcefully terminate a tracked peer/child process.
///
/// Vitreo guest processes are backed by host processes, so SIGKILL is the
/// transport: Learn TerminateProcess cannot be intercepted, and SIGTERM is
/// handled as a slow host shutdown. The requested Windows exit code is
/// retained on the ProcessObject and published when the host process actually
/// exits; callers therefore observe the code supplied to TerminateProcess
/// rather than the host's signal-derived status.
[[nodiscard]] bool terminate_process(ProcessObject& process, u32 exit_code);

/// Child-side: write the guest ExitProcess / termination DWORD to the inherited
/// write end so the parent reaper can publish it via GetExitCodeProcess.
void publish_windows_exit_code(int write_fd, u32 exit_code) noexcept;

/// Make a write to `fd` whose reader has gone fail with EPIPE instead of
/// raising SIGPIPE. Every host control pipe between Vitreo processes needs
/// this: its reader is a peer process that may already have exited, and
/// SIGPIPE's default action kills the WRITER - which then reports "killed by
/// a signal" (Windows exit code 0xFFFFFFFF) to its own parent instead of the
/// guest's real ExitProcess code.
void suppress_sigpipe(int fd) noexcept;

} // namespace vitreo::ipc
