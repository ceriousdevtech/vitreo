// SPDX-License-Identifier: LGPL-2.1-only
/// @file process_handle.cpp

#include "ipc/process_handle.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/sync/ntsync.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vitreo/diagnostics.hpp>

namespace vitreo::ipc {

namespace {

[[nodiscard]] u64 windows_filetime_now_100ns() {
    constexpr u64 kWindowsEpochOffset100ns = 116444736000000000ULL;
    return static_cast<u64>(
               std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count()) *
               10ULL +
           kWindowsEpochOffset100ns;
}

void cache_process_times_from_rusage(ProcessObject* obj) {
    if (obj == nullptr || obj->pid <= 0) return;
    rusage_info_v2 ri{};
    if (::proc_pid_rusage(obj->pid, RUSAGE_INFO_V2,
                          reinterpret_cast<rusage_info_t*>(&ri)) != 0) {
        return;
    }
    obj->kernel_filetime.store(ri.ri_system_time / 100ULL,
                               std::memory_order_relaxed);
    obj->user_filetime.store(ri.ri_user_time / 100ULL,
                             std::memory_order_relaxed);
    constexpr u64 kWindowsEpochOffset100ns = 116444736000000000ULL;
    static mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        if (tb.denom == 0) {
            tb.numer = 1;
            tb.denom = 1;
        }
        return tb;
    }();
    const u64 now_100ns = windows_filetime_now_100ns();
    const u64 now_abs = mach_absolute_time();
    u64 creation = now_100ns;
    if (ri.ri_proc_start_abstime != 0 && now_abs > ri.ri_proc_start_abstime) {
        const u64 elapsed_ns = (now_abs - ri.ri_proc_start_abstime) *
                               timebase.numer / timebase.denom;
        const u64 elapsed_100ns = elapsed_ns / 100ULL;
        creation = (elapsed_100ns < now_100ns) ? now_100ns - elapsed_100ns
                                               : now_100ns;
    }
    obj->creation_filetime.store(creation, std::memory_order_relaxed);
    obj->times_cached.store(true, std::memory_order_release);
    (void)kWindowsEpochOffset100ns;
}

} // namespace

void ProcessObject::close_windows_exit_fd() noexcept {
    if (windows_exit_read_fd >= 0) {
        (void)::close(windows_exit_read_fd);
        windows_exit_read_fd = -1;
    }
}

void suppress_sigpipe(int fd) noexcept {
    if (fd >= 0) (void)::fcntl(fd, F_SETNOSIGPIPE, 1);
}

void publish_windows_exit_code(int write_fd, u32 exit_code) noexcept {
    if (write_fd < 0) return;
    // The parent may have exited (or closed its end) first; the code is then
    // unobservable anyway, and dying of SIGPIPE here would replace this
    // process's own host status with a signal.
    suppress_sigpipe(write_fd);
    const u8 bytes[4] = {
        static_cast<u8>(exit_code & 0xFFu),
        static_cast<u8>((exit_code >> 8) & 0xFFu),
        static_cast<u8>((exit_code >> 16) & 0xFFu),
        static_cast<u8>((exit_code >> 24) & 0xFFu),
    };
    usize offset = 0;
    while (offset < sizeof(bytes)) {
        const ssize_t n = ::write(write_fd, bytes + offset, sizeof(bytes) - offset);
        if (n > 0) {
            offset += static_cast<usize>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    (void)::close(write_fd);
}

namespace {

[[nodiscard]] bool read_windows_exit_code(int fd, u32& out_code) {
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    u8 bytes[4] = {};
    usize offset = 0;
    // Child writes before exiting; NOTE_EXIT runs after exit, so the four
    // bytes should already be buffered. Brief retries cover scheduler races.
    for (int attempt = 0; attempt < 50 && offset < sizeof(bytes); ++attempt) {
        const ssize_t n =
            ::read(fd, bytes + offset, sizeof(bytes) - offset);
        if (n > 0) {
            offset += static_cast<usize>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        break; // EOF or hard error
    }
    if (offset != sizeof(bytes)) return false;
    out_code = static_cast<u32>(bytes[0]) |
               (static_cast<u32>(bytes[1]) << 8) |
               (static_cast<u32>(bytes[2]) << 16) |
               (static_cast<u32>(bytes[3]) << 24);
    return true;
}

/// Singleton background reaper. Registers each tracked pid on a shared kqueue
/// with EVFILT_PROC/NOTE_EXIT. This works for both ProcessSession-owned direct
/// children and non-child peers opened by pid. On exit it captures the status
/// (NOTE_EXITSTATUS) and signals the ntsync event; ProcessSession performs the
/// eventual waitpid for owned children.
class Reaper {
public:
    static Reaper& instance() {
        static Reaper r;
        return r;
    }

    ProcessObject* track(pid_t pid) {
        bool exited_now = false;
        ProcessObject* result = track_locked(pid, exited_now);
        if (exited_now) notify_observer(pid);
        return result;
    }

    void set_observer(ChildExitObserver observer) {
        std::lock_guard<std::mutex> lk(m_observer_mu);
        m_observer = std::move(observer);
    }

private:
    void notify_observer(pid_t pid) {
        std::lock_guard<std::mutex> lk(m_observer_mu);
        if (m_observer) m_observer(pid);
    }

    ProcessObject* track_locked(pid_t pid, bool& exited_now) {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_map.find(pid);
        if (it != m_map.end()) {
            // A reaped pid can be reused by a new process. The old object
            // stays alive for the handles that still refer to it; the new
            // process gets its own.
            if (!it->second->exited.load(std::memory_order_acquire)) {
                return it->second.get();
            }
            m_retired.push_back(std::move(it->second));
            m_map.erase(it);
        }

        auto obj = std::make_unique<ProcessObject>();
        obj->pid = pid;
        obj->exit_event =
            ntsync::global().create_event(/*manual_reset=*/true, /*initial_state=*/false);
        ProcessObject* raw = obj.get();
        m_map.emplace(pid, std::move(obj));

        ensure_thread();

        struct kevent ev;
        EV_SET(&ev, static_cast<uintptr_t>(pid), EVFILT_PROC,
               EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_EXIT | NOTE_EXITSTATUS, 0, raw);
        int rc = ::kevent(m_kq, &ev, 1, nullptr, 0, nullptr);
        if (rc < 0) {
            // pid already gone (ESRCH) or registration failed — treat as exited.
            mark_exited(raw, 0);
            exited_now = true;
            return raw;
        }
        // Race: pid may have exited between fork and EV_ADD. Confirm liveness.
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            mark_exited(raw, 0);
            exited_now = true;
        }
        return raw;
    }

public:
    ProcessObject* find(pid_t pid) {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_map.find(pid);
        return it == m_map.end() ? nullptr : it->second.get();
    }

private:
    Reaper() { m_kq = ::kqueue(); }

    void ensure_thread() {
        if (m_started) return;
        m_started = true;
        std::thread([this] { run(); }).detach();
    }

    void run() {
        struct kevent events[16];
        for (;;) {
            int n = ::kevent(m_kq, nullptr, 0, events, 16, nullptr);
            if (n < 0) {
                if (errno == EINTR) continue;
                return;  // kqueue broken; stop reaping
            }
            for (int i = 0; i < n; ++i) {
                if (events[i].filter != EVFILT_PROC) continue;
                if (!(events[i].fflags & NOTE_EXIT)) continue;
                auto* obj = static_cast<ProcessObject*>(events[i].udata);
                if (obj == nullptr) continue;
                u32 code = 0;
                if (events[i].fflags & NOTE_EXITSTATUS) {
                    int status = static_cast<int>(events[i].data);
                    code = WIFEXITED(status)
                               ? static_cast<u32>(WEXITSTATUS(status))
                               : 0xFFFFFFFFu;
                    ipc_log(
                        "process reaper pid=%d wait_status=0x%x exited=%d "
                        "signaled=%d signal=%d host_exit=0x%08x",
                        static_cast<int>(obj->pid), status,
                        WIFEXITED(status) ? 1 : 0,
                        WIFSIGNALED(status) ? 1 : 0,
                        WIFSIGNALED(status) ? WTERMSIG(status) : 0, code);
                } else {
                    ipc_log(
                        "process reaper pid=%d missing NOTE_EXITSTATUS",
                        static_cast<int>(obj->pid));
                }
                mark_exited(obj, code);
                notify_observer(obj->pid);
            }
        }
    }

    static void mark_exited(ProcessObject* obj, u32 code) {
        // Snapshot times while the pid may still be a zombie (Learn keeps
        // process-object times valid until the last handle closes).
        cache_process_times_from_rusage(obj);
        obj->exit_filetime.store(windows_filetime_now_100ns(),
                                 std::memory_order_release);

        // Claim the exit once, but publish `exited` only after exit_code is
        // stored: a GetExitCodeProcess racing this must see STILL_ACTIVE or
        // the final code, never a transient 0.
        bool was = obj->exit_claimed.exchange(true, std::memory_order_acq_rel);
        if (!was) {
            // Prefer the full Windows DWORD published by the child over the
            // host wait status (which only retains 8 bits).
            if (obj->windows_exit_read_fd >= 0) {
                u32 windows_code = 0;
                if (read_windows_exit_code(obj->windows_exit_read_fd,
                                           windows_code)) {
                    code = windows_code;
                }
                obj->close_windows_exit_fd();
            }
            const u32 observable_code =
                obj->termination_requested.load(std::memory_order_acquire)
                    ? obj->termination_exit_code.load(std::memory_order_acquire)
                    : code;
            obj->exit_code.store(observable_code, std::memory_order_release);
            obj->exited.store(true, std::memory_order_release);
            // Defer signaling the process wait object briefly so the parent's
            // completion-port path can observe pipe EOF and the child's
            // initialization-failure notice before the wait callback runs.
            // When the wait fires first, a browser host restarts the GPU
            // process with the same failing device flags; when the failure
            // notice wins, it relaunches with a software renderer.
            // GetExitCodeProcess sees the real code immediately; only
            // WFSO/RegisterWait is delayed.
            const u64 exit_event = obj->exit_event;
            const pid_t pid = obj->pid;
            std::thread([exit_event, pid, observable_code]() {
                unsigned delay_ms = 100;
                if (const char* env =
                        std::getenv("VITREO_PROCESS_EXIT_SIGNAL_DELAY_MS")) {
                    char* end = nullptr;
                    const unsigned long parsed =
                        std::strtoul(env, &end, 10);
                    if (end != env && parsed <= 500ul) {
                        delay_ms = static_cast<unsigned>(parsed);
                    }
                }
                if (delay_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(delay_ms));
                }
                if (exit_event != 0) {
                    ntsync::global().set_event(exit_event);
                }
                ipc_log(
                    "process exit_event signaled pid=%d code=0x%x delay_ms=%u",
                    static_cast<int>(pid), observable_code, delay_ms);
            }).detach();
            // When a child worker exits, dump this process's hostcall ring so
            // the parent's side of the cross-process handshake can be
            // inspected. Child dumps alone cannot show whether the parent
            // receiver ever ran.
            if (std::getenv("VITREO_DUMP_ON_EXIT") != nullptr ||
                std::getenv("VITREO_DUMP_ON_CHILD_EXIT") != nullptr) {
                char reason[96];
                std::snprintf(
                    reason, sizeof(reason),
                    "child-exit pid=%d code=0x%x",
                    static_cast<int>(obj->pid), observable_code);
                ipc_log("%s — dumping local hostcall ring", reason);
                vitreo::diagnostics::dump_hostcalls(reason);
            }
        }
    }

    int m_kq{-1};
    bool m_started{false};
    std::mutex m_mu;
    std::unordered_map<pid_t, std::unique_ptr<ProcessObject>> m_map;
    /// Objects of exited processes whose pid was reused; kept alive for the
    /// guest handles that still point at them.
    std::vector<std::unique_ptr<ProcessObject>> m_retired;
    std::mutex m_observer_mu;
    ChildExitObserver m_observer;
};

} // namespace

ProcessObject* track_process(pid_t pid) {
    if (pid <= 0) return nullptr;
    return Reaper::instance().track(pid);
}

void set_child_exit_observer(ChildExitObserver observer) {
    Reaper::instance().set_observer(std::move(observer));
}

ProcessObject* find_tracked_process(pid_t pid) {
    if (pid <= 0) return nullptr;
    return Reaper::instance().find(pid);
}

ProcessObject* create_completed_process(u32 exit_code) {
    // These have no pid, so the Reaper's pid-keyed map cannot own them. They
    // are few and live as long as the process, so a simple owning list is
    // enough — the pointer must outlive every handle referring to it.
    static std::mutex owner_mutex;
    static std::vector<std::unique_ptr<ProcessObject>> owned;

    auto obj = std::make_unique<ProcessObject>();
    obj->pid = 0;
    obj->exit_event =
        ntsync::global().create_event(/*manual_reset=*/true, /*initial_state=*/true);
    obj->exit_code.store(exit_code, std::memory_order_release);
    obj->exited.store(true, std::memory_order_release);
    const u64 now = windows_filetime_now_100ns();
    obj->creation_filetime.store(now, std::memory_order_relaxed);
    obj->exit_filetime.store(now, std::memory_order_relaxed);
    obj->times_cached.store(true, std::memory_order_release);

    ProcessObject* raw = obj.get();
    {
        std::lock_guard<std::mutex> lk(owner_mutex);
        owned.push_back(std::move(obj));
    }
    return raw;
}

bool terminate_process(ProcessObject& process, u32 exit_code) {
    if (process.pid <= 0 ||
        process.exited.load(std::memory_order_acquire)) {
        return false;
    }

    process.termination_exit_code.store(exit_code, std::memory_order_release);
    process.termination_requested.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "[process] terminate_process killer_pid=%d target_pid=%d "
                 "code=0x%x — SIGKILL\n",
                 static_cast<int>(::getpid()), static_cast<int>(process.pid),
                 exit_code);
    std::fflush(stderr);
    // Learn TerminateProcess: the target cannot prevent termination. SIGTERM
    // is intercepted by the Vitreo worker as a slow host shutdown, so the
    // process stays live while controllers poll QueryServiceStatus / WFSO.
    if (::kill(process.pid, SIGKILL) == 0) {
        return true;
    }

    process.termination_requested.store(false, std::memory_order_release);
    return false;
}

} // namespace vitreo::ipc
