// SPDX-License-Identifier: LGPL-2.1-only

/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include <poll.h>
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/event.h>
#include <sys/file.h>
#include <unistd.h>

/// @file filemanager_construction.cpp
/// @brief FileManager Construction
///
/// Closed cut from file.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {
constexpr uintptr_t kPipeWriteWakeIdent = 1;
}  // namespace

// =============================================================================
// FileManager Construction
// =============================================================================

FileManager::FileManager(capsule::Vfs& vfs, HandleTable& handles)
    : m_vfs(vfs)
    , m_handles(handles) {}

FileManager::FileManager(capsule::Vfs& vfs, HandleTable& handles, GuestMemory& memory)
    : m_vfs(vfs)
    , m_handles(handles)
    , m_memory(&memory) {}

FileManager::~FileManager() {
    shutdown_overlapped_pipe_io();
}

u64 FileManager::async_object_id(Handle handle) const noexcept {
    if (m_handles.get_type(handle) == HandleType::Pipe) {
        const auto endpoint =
            m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe);
        return static_cast<u64>(
            reinterpret_cast<std::uintptr_t>(endpoint.get()));
    }
    if (m_handles.get_type(handle) == HandleType::File) {
        const capsule::FileHandle vfs_handle = vfs_handle_for(handle);
        return vfs_handle == capsule::INVALID_FILE_HANDLE
            ? 0
            : static_cast<u64>(vfs_handle);
    }
    return 0;
}

void FileManager::trace_overlapped(OverlappedIoTraceEvent event) const {
    if (!m_overlapped_trace) return;
    if (event.object_id == 0) event.object_id = async_object_id(event.handle);
    if (event.stage == OverlappedIoTraceStage::BufferFilled &&
        m_memory != nullptr && event.buffer != 0 &&
        event.transferred_bytes != 0) {
        const usize preview_size = std::min<usize>(
            event.transferred_bytes, event.preview.size());
        if (m_memory->read_bytes(
                event.buffer, reinterpret_cast<Byte*>(event.preview.data()),
                preview_size).is_ok()) {
            event.preview_size = static_cast<u8>(preview_size);
        }

        // Ping frame name hash. Scan the whole bounded pipe transfer, not
        // only the log preview.
        constexpr std::array<u8, 4> kPingHash{0xc3, 0xfe, 0xb3, 0x39};
        constexpr usize kMaxProbe = 64 * 1024;
        const usize probe_size =
            std::min<usize>(event.transferred_bytes, kMaxProbe);
        std::vector<u8> probe(probe_size);
        if (m_memory->read_bytes(
                event.buffer, reinterpret_cast<Byte*>(probe.data()),
                probe.size()).is_ok()) {
            event.contains_ping =
                std::search(probe.begin(), probe.end(),
                            kPingHash.begin(), kPingHash.end()) != probe.end();
        }
    }
    m_overlapped_trace(event);
}

void FileManager::shutdown_overlapped_pipe_io() {
    // Join workers *outside* m_overlapped_pipe_io_mutex. Workers call
    // consume_pending_pipe_io / pending_cancelled which take that same lock;
    // joining while holding it deadlocks utility-process teardown (network
    // and storage TerminateProcess → set_file_manager(nullptr) → hang
    // forever, so the host pid stays alive and the parent never restarts
    // the service).
    std::jthread write_service;
    std::jthread completion_worker;
    std::jthread read_worker;
    {
        std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
        for (auto& pending : m_pending_pipe_io) {
            pending.cancelled = true;
        }
        if (m_pipe_write_service.joinable()) {
            m_pipe_write_service.request_stop();
        }
        write_service = std::move(m_pipe_write_service);
        if (m_overlapped_pipe_completion_worker.joinable()) {
            m_overlapped_pipe_completion_worker.request_stop();
        }
        if (m_overlapped_pipe_read_worker.joinable()) {
            m_overlapped_pipe_read_worker.request_stop();
        }
        m_overlapped_pipe_completion_cv.notify_all();
        completion_worker = std::move(m_overlapped_pipe_completion_worker);
        read_worker = std::move(m_overlapped_pipe_read_worker);
    }
    write_service = {};
    // Writes queued after the service stopped (or with no service at all)
    // are aborted here; the service aborts everything it still held.
    std::vector<ArmedPipeWriter> stranded;
    {
        std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
        stranded = m_armed_pipe_writers;
    }
    for (const auto& writer : stranded) {
        drain_pipe_writer(writer, /*abort_all=*/true);
    }
    completion_worker = {};
    read_worker = {};
    {
        std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
        m_armed_pipe_writers.clear();
        if (m_pipe_write_kq >= 0) {
            (void)::close(m_pipe_write_kq);
            m_pipe_write_kq = -1;
        }
        m_pending_pipe_io.clear();
        m_pending_pipe_read_tasks.clear();
        m_delayed_pipe_completions.clear();
    }
    for (int& fd : m_pipe_read_wake) {
        if (fd >= 0) (void)::close(fd);
        fd = -1;
    }
}

usize FileManager::overlapped_pipe_io_worker_count() {
    std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
    return (m_pipe_write_service.joinable() ? 1u : 0u) +
           (m_overlapped_pipe_read_worker.joinable() ? 1u : 0u);
}

bool FileManager::ensure_pipe_write_service() {
    if (m_pipe_write_kq < 0) {
        const int kq = ::kqueue();
        if (kq < 0) return false;
        (void)::fcntl(kq, F_SETFD, FD_CLOEXEC);
        struct kevent wake {};
        EV_SET(&wake, kPipeWriteWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0,
               0, nullptr);
        if (::kevent(kq, &wake, 1, nullptr, 0, nullptr) != 0) {
            (void)::close(kq);
            return false;
        }
        m_pipe_write_kq = kq;
    }
    if (!m_pipe_write_service.joinable()) {
        m_pipe_write_service = std::jthread(
            [this](std::stop_token stop) { run_pipe_write_service(stop); });
    }
    return true;
}

bool FileManager::arm_pipe_writer(int fd) const noexcept {
    struct kevent ev {};
    EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_ADD | EV_ONESHOT,
           0, 0, nullptr);
    return ::kevent(m_pipe_write_kq, &ev, 1, nullptr, 0, nullptr) == 0;
}

void FileManager::wake_pipe_write_service() const noexcept {
    if (m_pipe_write_kq < 0) return;
    struct kevent ev {};
    EV_SET(&ev, kPipeWriteWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(m_pipe_write_kq, &ev, 1, nullptr, 0, nullptr);
}

void FileManager::run_pipe_write_service(std::stop_token stop) {
    std::stop_callback wake(stop, [this] { wake_pipe_write_service(); });
    const int kq = m_pipe_write_kq;
    std::array<struct kevent, 64> events{};
    while (!stop.stop_requested()) {
        const int n = ::kevent(kq, nullptr, 0, events.data(),
                               static_cast<int>(events.size()), nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr,
                         "[vitreo] pipe write service: kevent failed (%s); "
                         "aborting pended overlapped pipe writes\n",
                         std::strerror(errno));
            break;
        }
        // A wake (new cancellation or shutdown) revisits every armed
        // endpoint; readiness revisits only the endpoints on that descriptor.
        bool sweep = false;
        std::vector<uintptr_t> ready;
        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_USER) {
                sweep = true;
            } else {
                ready.push_back(events[i].ident);
            }
        }
        if (stop.stop_requested()) break;
        std::vector<ArmedPipeWriter> work;
        {
            std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
            for (const auto& writer : m_armed_pipe_writers) {
                const auto fd = static_cast<uintptr_t>(writer.io->fd());
                if (sweep || std::find(ready.begin(), ready.end(), fd) !=
                                 ready.end()) {
                    work.push_back(writer);
                }
            }
        }
        for (const auto& writer : work) {
            drain_pipe_writer(writer, /*abort_all=*/false);
        }
    }
    std::vector<ArmedPipeWriter> remaining;
    {
        std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
        remaining = m_armed_pipe_writers;
    }
    for (const auto& writer : remaining) {
        drain_pipe_writer(writer, /*abort_all=*/true);
    }
}

void FileManager::drain_pipe_writer(const ArmedPipeWriter& writer,
                                    bool abort_all) {
    constexpr u32 STATUS_PIPE_BROKEN = 0xC000014B;
    enum class Outcome : u8 { Sent, Broken, Aborted };
    struct Done {
        Handle handle;
        GuestAddr overlapped;
        u32 bytes;
        bool is_64bit;
        Outcome outcome;
    };
    std::vector<Done> done;
    PipeEndpoint& endpoint = *writer.endpoint;
    {
        std::lock_guard<std::mutex> write_lock(endpoint.write_io_mutex);
        auto& queue = endpoint.queued_writes;
        auto finish = [&](const QueuedPipeWrite& w, Outcome outcome) {
            done.push_back(Done{w.handle, w.overlapped, w.bytes_to_write,
                                w.is_64bit, outcome});
        };
        {
            // A cancelled request that has not reached the transport leaves
            // the queue at once wherever it is; the head aborts even part
            // sent (Microsoft Learn CancelIoEx: completes with
            // ERROR_OPERATION_ABORTED unless it already finished).
            std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
            for (auto it = queue.begin(); it != queue.end();) {
                bool cancelled = abort_all;
                for (const auto& pending : m_pending_pipe_io) {
                    if (pending.overlapped == it->overlapped &&
                        pending.cancelled) {
                        cancelled = true;
                        break;
                    }
                }
                if (cancelled && (it == queue.begin() || it->offset == 0)) {
                    finish(*it, Outcome::Aborted);
                    it = queue.erase(it);
                } else {
                    ++it;
                }
            }
        }
        bool broken = false;
        while (!queue.empty()) {
            QueuedPipeWrite& w = queue.front();
            if (!broken && w.offset < w.wire.size()) {
                const isize transferred = writer.io->write_nonblocking(
                    w.wire.data() + w.offset, w.wire.size() - w.offset);
                if (transferred > 0) {
                    w.offset += static_cast<usize>(transferred);
                    continue;
                }
                if (transferred == 0) break;  // EWOULDBLOCK: wait for space
                broken = true;
            }
            finish(w, broken ? Outcome::Broken : Outcome::Sent);
            queue.pop_front();
        }
        bool armed = !queue.empty() && arm_pipe_writer(writer.io->fd());
        if (!queue.empty() && !armed) {
            // The descriptor can no longer be watched: the transport is gone.
            for (const auto& w : queue) finish(w, Outcome::Broken);
            queue.clear();
        }
        if (queue.empty()) {
            std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
            std::erase_if(m_armed_pipe_writers, [&](const ArmedPipeWriter& a) {
                return a.endpoint.get() == &endpoint;
            });
        }
    }
    for (const Done& d : done) {
        const auto pending = consume_pending_pipe_io(d.overlapped);
        if (!pending.has_value()) continue;
        // A cancel that loses the race to a fully sent frame completes
        // normally (Microsoft Learn CancelIoEx).
        if (d.outcome == Outcome::Aborted ||
            (d.outcome == Outcome::Broken && *pending)) {
            notify_pipe_io_aborted(d.handle, d.overlapped, d.is_64bit);
        } else if (d.outcome == Outcome::Broken) {
            pipe_diag("overlapped-write-failed", d.handle, d.bytes, -1,
                      d.overlapped);
            notify_overlapped_completion(
                d.handle, 0, d.overlapped, d.is_64bit, STATUS_PIPE_BROKEN,
                error_codes::ERROR_BROKEN_PIPE);
        } else {
            pipe_diag("overlapped-write-complete", d.handle, d.bytes,
                      static_cast<isize>(d.bytes), d.overlapped);
            notify_overlapped_completion(d.handle, d.bytes, d.overlapped,
                                         d.is_64bit);
        }
    }
}

void FileManager::enqueue_delayed_pipe_completion(
    Handle handle, u32 bytes_transferred, GuestAddr overlapped, bool is_64bit,
    u32 nt_status, u32 win_error) {
    // WriteFile/ReadFile with FILE_FLAG_OVERLAPPED must return ERROR_IO_PENDING
    // and complete off the calling thread (Microsoft Learn WriteFile). ChannelWin
    // AddRefs only after the hostcall returns, so delay the IOCP packet slightly.
    constexpr auto kHandoffDelay = std::chrono::microseconds(100);
    m_delayed_pipe_completions.push_back(DelayedPipeCompletion{
        .handle = handle,
        .bytes_transferred = bytes_transferred,
        .overlapped = overlapped,
        .is_64bit = is_64bit,
        .nt_status = nt_status,
        .win_error = win_error,
        .ready_at = std::chrono::steady_clock::now() + kHandoffDelay,
    });
    if (!m_overlapped_pipe_completion_worker.joinable()) {
        m_overlapped_pipe_completion_worker = std::jthread(
            [this](std::stop_token stop) {
                run_overlapped_pipe_completion_worker(stop);
            });
    }
    m_overlapped_pipe_completion_cv.notify_one();
}

void FileManager::deliver_delayed_pipe_completion(
    const DelayedPipeCompletion& completion, bool force_abort) {
    const auto pending = consume_pending_pipe_io(completion.overlapped);
    if (!pending.has_value()) {
        return;
    }
    if (force_abort || *pending) {
        notify_pipe_io_aborted(
            completion.handle, completion.overlapped, completion.is_64bit);
        return;
    }
    pipe_diag("overlapped-complete", completion.handle,
              completion.bytes_transferred,
              static_cast<isize>(completion.bytes_transferred),
              completion.overlapped);
    notify_overlapped_completion(
        completion.handle, completion.bytes_transferred,
        completion.overlapped, completion.is_64bit, completion.nt_status,
        completion.win_error);
}

void FileManager::run_overlapped_pipe_completion_worker(std::stop_token stop) {
    std::stop_callback wake(stop, [this] {
        m_overlapped_pipe_completion_cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(m_overlapped_pipe_io_mutex);
    for (;;) {
        if (m_delayed_pipe_completions.empty()) {
            if (stop.stop_requested()) {
                return;
            }
            m_overlapped_pipe_completion_cv.wait(lock, [&] {
                return stop.stop_requested() ||
                       !m_delayed_pipe_completions.empty();
            });
            continue;
        }
        if (stop.stop_requested()) {
            std::vector<DelayedPipeCompletion> drain;
            drain.swap(m_delayed_pipe_completions);
            lock.unlock();
            for (const auto& completion : drain) {
                deliver_delayed_pipe_completion(completion, /*force_abort=*/true);
            }
            return;
        }
        auto ready_at = m_delayed_pipe_completions.front().ready_at;
        for (const auto& completion : m_delayed_pipe_completions) {
            if (completion.ready_at < ready_at) {
                ready_at = completion.ready_at;
            }
        }
        if (m_overlapped_pipe_completion_cv.wait_until(lock, ready_at, [&] {
                return stop.stop_requested();
            })) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        std::vector<DelayedPipeCompletion> ready;
        std::vector<DelayedPipeCompletion> rest;
        ready.reserve(m_delayed_pipe_completions.size());
        for (auto& completion : m_delayed_pipe_completions) {
            if (completion.ready_at <= now) {
                ready.push_back(std::move(completion));
            } else {
                rest.push_back(std::move(completion));
            }
        }
        m_delayed_pipe_completions.swap(rest);
        lock.unlock();
        for (const auto& completion : ready) {
            deliver_delayed_pipe_completion(completion, /*force_abort=*/false);
        }
        lock.lock();
    }
}

void FileManager::wake_pipe_read_worker() noexcept {
    if (m_pipe_read_wake[1] < 0) return;
    const char byte = 1;
    ssize_t n = 0;
    do {
        n = ::write(m_pipe_read_wake[1], &byte, 1);
    } while (n < 0 && errno == EINTR);
}

void FileManager::run_overlapped_pipe_read_worker(std::stop_token stop) {
    std::stop_callback wake(stop, [this] {
        m_overlapped_pipe_completion_cv.notify_all();
        wake_pipe_read_worker();
    });
    std::unique_lock<std::mutex> lock(m_overlapped_pipe_io_mutex);
    for (;;) {
        if (m_pending_pipe_read_tasks.empty()) {
            if (stop.stop_requested()) return;
            m_overlapped_pipe_completion_cv.wait(lock, [&] {
                return stop.stop_requested() ||
                       !m_pending_pipe_read_tasks.empty();
            });
            continue;
        }

        std::vector<PipeReadTask> tasks;
        tasks.swap(m_pending_pipe_read_tasks);
        lock.unlock();
        std::vector<PipeReadTask> pending;
        pending.reserve(tasks.size());
        for (auto& task : tasks) {
            if (!task.run(stop)) {
                pending.push_back(std::move(task));
            }
        }
        lock.lock();
        for (auto& task : pending) {
            m_pending_pipe_read_tasks.push_back(std::move(task));
        }
        if (stop.stop_requested()) {
            if (m_pending_pipe_read_tasks.empty()) return;
            continue;  // the tasks abort themselves on the next pass
        }

        // Sleep until a pending read's descriptor is readable (data, EOF or
        // hangup), a task is added or cancelled (wake pipe), or a 1 s safety
        // net passes. This used to re-run every task every millisecond, a
        // 1 kHz wakeup for as long as any overlapped pipe read was pending —
        // which every long-lived overlapped pipe keeps up for its whole life.
        std::vector<struct pollfd> fds;
        fds.reserve(m_pending_pipe_read_tasks.size() + 1);
        if (m_pipe_read_wake[0] >= 0) {
            fds.push_back({m_pipe_read_wake[0], POLLIN, 0});
        }
        for (const auto& task : m_pending_pipe_read_tasks) {
            if (task.fd >= 0) fds.push_back({task.fd, POLLIN, 0});
        }
        lock.unlock();
        int ready = 0;
        do {
            ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 1000);
        } while (ready < 0 && errno == EINTR);
        if (m_pipe_read_wake[0] >= 0) {
            char drain[64];
            while (::read(m_pipe_read_wake[0], drain, sizeof(drain)) > 0) {
            }
        }
        lock.lock();
    }
}

// Caller holds m_overlapped_pipe_io_mutex.
void FileManager::track_pending_pipe_io(
    Handle handle, GuestAddr overlapped, bool is_64bit) {
    m_pending_pipe_io.push_back(PendingPipeIo{
        .handle = handle,
        .overlapped = overlapped,
        .issuer_tid = ThreadManager::get_current_thread_id_static(),
        .is_64bit = is_64bit,
        .cancelled = false,
    });
}

std::optional<bool> FileManager::consume_pending_pipe_io(GuestAddr overlapped) {
    std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
    for (auto it = m_pending_pipe_io.begin(); it != m_pending_pipe_io.end();
         ++it) {
        if (it->overlapped != overlapped) continue;
        const bool cancelled = it->cancelled;
        m_pending_pipe_io.erase(it);
        return cancelled;
    }
    return std::nullopt;
}

void FileManager::notify_pipe_io_aborted(
    Handle handle, GuestAddr overlapped, bool is_64bit) const {
    constexpr u32 STATUS_CANCELLED = 0xC0000120;
    pipe_diag("overlapped-aborted", handle, 0, -1, overlapped);
    notify_overlapped_completion(
        handle, 0, overlapped, is_64bit, STATUS_CANCELLED,
        error_codes::ERROR_OPERATION_ABORTED);
}

usize FileManager::cancel_overlapped_pipe_io(
    Handle handle, GuestAddr overlapped, bool only_calling_thread) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    usize marked = 0;
    std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
    for (auto& pending : m_pending_pipe_io) {
        if (pending.handle != handle) continue;
        if (overlapped != 0 && pending.overlapped != overlapped) continue;
        if (only_calling_thread && pending.issuer_tid != tid) continue;
        if (pending.cancelled) continue;
        pending.cancelled = true;
        ++marked;
    }
    // A cancelled request completes on its service's next pass; wake both.
    if (marked != 0) {
        wake_pipe_read_worker();
        wake_pipe_write_service();
    }
    return marked;
}

usize FileManager::cancel_pipe_handle_io(Handle handle, GuestAddr overlapped,
                                         bool only_calling_thread) {
    usize marked = cancel_overlapped_pipe_io(handle, overlapped, only_calling_thread);
    marked += cancel_named_pipe_connect(
        m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe), handle,
        overlapped, only_calling_thread);
    return marked;
}

FileManager::SyncIoCancelFlag FileManager::begin_sync_io(u32 tid) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(m_sync_io_mutex);
    m_sync_io_by_tid[tid] = flag;
    return flag;
}

void FileManager::end_sync_io(u32 tid, const SyncIoCancelFlag& flag) {
    std::lock_guard<std::mutex> lock(m_sync_io_mutex);
    auto it = m_sync_io_by_tid.find(tid);
    if (it != m_sync_io_by_tid.end() && it->second == flag) {
        m_sync_io_by_tid.erase(it);
    }
}

usize FileManager::cancel_synchronous_io(u32 tid) {
    std::lock_guard<std::mutex> lock(m_sync_io_mutex);
    auto it = m_sync_io_by_tid.find(tid);
    if (it == m_sync_io_by_tid.end() || !it->second) return 0;
    it->second->store(true, std::memory_order_release);
    return 1;
}

OverlappedPipeIoResult FileManager::begin_overlapped_pipe_read(
    Handle handle, GuestAddr buffer_addr, u32 bytes_to_read,
    GuestAddr overlapped, bool is_64bit, u32& bytes_read) {
    constexpr u32 STATUS_PENDING = 0x00000103;
    constexpr u32 STATUS_BUFFER_OVERFLOW = 0x80000005;
    constexpr u32 STATUS_PIPE_BROKEN = 0xC000014B;
    constexpr u32 STATUS_ACCESS_DENIED = 0xC0000022;

    bytes_read = 0;
    if (m_memory == nullptr || overlapped == 0 ||
        m_handles.get_type(handle) != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return OverlappedPipeIoResult::Failed;
    }
    auto endpoint = m_handles.get_shared<PipeEndpoint>(
        handle, HandleType::Pipe);
    auto io = pipe_io(endpoint);
    if (!endpoint || !endpoint->readable || !io) {
        set_last_error(endpoint && endpoint->readable
            ? unconnected_pipe_error(endpoint) : error_codes::ERROR_INVALID_HANDLE);
        return OverlappedPipeIoResult::Failed;
    }
    const u64 object_id = static_cast<u64>(
        reinterpret_cast<std::uintptr_t>(endpoint.get()));
    if (pipe_diag_enabled()) {
        std::fprintf(stderr, "[pipe-endpoint pid=%d] read h=0x%x fd=%d peer=%d ov=0x%llx\n",
                     static_cast<int>(::getpid()), handle, io->fd(), io->peer_pid(),
                     static_cast<unsigned long long>(overlapped));
    }
    trace_overlapped({
        .stage = OverlappedIoTraceStage::Submitted,
        .handle = handle,
        .object_id = object_id,
        .overlapped = overlapped,
        .buffer = buffer_addr,
        .requested_bytes = bytes_to_read,
        .is_64bit = is_64bit,
    });

    // If data is already available, Windows completes the overlapped read
    // synchronously: ReadFile returns TRUE and reports the transferred byte
    // count. The caller still receives an IOCP packet unless it enabled
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS.
    // The rest of a message a short read left behind is buffered on the
    // endpoint, not in the socket: it is available at once (s4_overlapped_probe
    // remainder r=1 n=6); peeking only the socket left the read pending forever.
    usize buffered_message = 0;
    {
        std::lock_guard<std::mutex> read_lock(endpoint->read_io_mutex);
        if (endpoint->pending_message_loaded) {
            buffered_message = endpoint->pending_message.size() -
                               endpoint->pending_message_offset;
        }
    }
    const isize available = buffered_message != 0
        ? static_cast<isize>(buffered_message) : io->peek_available();
    pipe_diag("overlapped-read-peek", handle, bytes_to_read, available,
              overlapped);
    if (available < 0) {
        trace_overlapped({
            .stage = OverlappedIoTraceStage::Failed,
            .handle = handle,
            .object_id = object_id,
            .overlapped = overlapped,
            .buffer = buffer_addr,
            .requested_bytes = bytes_to_read,
            .win32_error = error_codes::ERROR_BROKEN_PIPE,
            .is_64bit = is_64bit,
        });
        set_last_error(error_codes::ERROR_BROKEN_PIPE);
        return OverlappedPipeIoResult::Failed;
    }
    // A zero-byte pipe read is also a readiness operation: on an empty
    // connected pipe it must pend until a write or disconnection arrives.
    if (available > 0) {
        trace_overlapped({
            .stage = OverlappedIoTraceStage::DataAvailable,
            .handle = handle,
            .object_id = object_id,
            .overlapped = overlapped,
            .buffer = buffer_addr,
            .requested_bytes = bytes_to_read,
            .transferred_bytes = static_cast<u32>(available),
            .is_64bit = is_64bit,
        });
        if (!read_file(handle, buffer_addr, bytes_to_read, 0, &bytes_read)) {
            if (get_last_error() == error_codes::ERROR_MORE_DATA) {
                // A message longer than the buffer: the read completes with
                // STATUS_BUFFER_OVERFLOW and the partial count, which reaches
                // hEvent and the port like any completion (not a success, so
                // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS does not apply); the
                // rest stays readable (s4_overlapped_probe more_data).
                constexpr u32 STATUS_BUFFER_OVERFLOW = 0x80000005;
                notify_overlapped_completion(
                    handle, bytes_read, overlapped, is_64bit,
                    STATUS_BUFFER_OVERFLOW, error_codes::ERROR_MORE_DATA,
                    /*completed_synchronously=*/false);
                set_last_error(error_codes::ERROR_MORE_DATA);
                return OverlappedPipeIoResult::Failed;
            }
            trace_overlapped({
                .stage = OverlappedIoTraceStage::Failed,
                .handle = handle,
                .object_id = object_id,
                .overlapped = overlapped,
                .buffer = buffer_addr,
                .requested_bytes = bytes_to_read,
                .transferred_bytes = bytes_read,
                .win32_error = get_last_error(),
                .is_64bit = is_64bit,
            });
            return OverlappedPipeIoResult::Failed;
        }
        const u32 transferred = bytes_read;
        trace_overlapped({
            .stage = OverlappedIoTraceStage::BufferFilled,
            .handle = handle,
            .object_id = object_id,
            .overlapped = overlapped,
            .buffer = buffer_addr,
            .requested_bytes = bytes_to_read,
            .transferred_bytes = transferred,
            .is_64bit = is_64bit,
        });
        pipe_diag("overlapped-read-sync", handle, bytes_to_read,
                  static_cast<isize>(transferred), overlapped);
        return OverlappedPipeIoResult::Completed;
    }

    const auto pending_write = is_64bit
        ? m_memory->write_u64(overlapped, STATUS_PENDING)
        : m_memory->write_u32(overlapped, STATUS_PENDING);
    const auto count_write = is_64bit
        ? m_memory->write_u64(overlapped + 8, 0)
        : m_memory->write_u32(overlapped + 4, 0);
    if (pending_write.is_err() || count_write.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return OverlappedPipeIoResult::Failed;
    }

    std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
    track_pending_pipe_io(handle, overlapped, is_64bit);
    const int wait_fd = io->fd();
    m_pending_pipe_read_tasks.push_back(PipeReadTask{wait_fd,
        [this, io = std::move(io), handle, object_id, buffer_addr,
         bytes_to_read, overlapped, is_64bit](std::stop_token stop_token) {
            auto pending_cancelled = [&]() {
                std::lock_guard<std::mutex> pending_lock(
                    m_overlapped_pipe_io_mutex);
                for (const auto& pending : m_pending_pipe_io) {
                    if (pending.overlapped == overlapped && pending.cancelled) {
                        return true;
                    }
                }
                return false;
            };
            if (!stop_token.stop_requested() && !pending_cancelled() &&
                !io->wait_readable(0)) {
                return false;
            }
            const auto pending = consume_pending_pipe_io(overlapped);
            if (!pending.has_value()) return true;
            if (stop_token.stop_requested() || *pending) {
                notify_pipe_io_aborted(handle, overlapped, is_64bit);
                return true;
            }

            trace_overlapped({
                .stage = OverlappedIoTraceStage::DataAvailable,
                .handle = handle,
                .object_id = object_id,
                .overlapped = overlapped,
                .buffer = buffer_addr,
                .requested_bytes = bytes_to_read,
                .is_64bit = is_64bit,
            });
            u32 transferred = 0;
            const bool empty_disconnect =
                bytes_to_read == 0 && io->peek_available() < 0;
            const bool ok = !empty_disconnect && read_file(
                handle, buffer_addr, bytes_to_read, 0, &transferred);
            const u32 error = empty_disconnect
                ? error_codes::ERROR_BROKEN_PIPE : get_last_error();
            pipe_diag("overlapped-read-complete", handle, bytes_to_read,
                      ok ? static_cast<isize>(transferred) : -1, overlapped);
            if (ok || error == error_codes::ERROR_MORE_DATA) {
                trace_overlapped({
                    .stage = OverlappedIoTraceStage::BufferFilled,
                    .handle = handle,
                    .object_id = object_id,
                    .overlapped = overlapped,
                    .buffer = buffer_addr,
                    .requested_bytes = bytes_to_read,
                    .transferred_bytes = transferred,
                    .win32_error = error,
                    .is_64bit = is_64bit,
                });
            } else {
                trace_overlapped({
                    .stage = OverlappedIoTraceStage::Failed,
                    .handle = handle,
                    .object_id = object_id,
                    .overlapped = overlapped,
                    .buffer = buffer_addr,
                    .requested_bytes = bytes_to_read,
                    .win32_error = error,
                    .is_64bit = is_64bit,
                });
            }
            if (!ok && error == error_codes::ERROR_MORE_DATA) {
                notify_overlapped_completion(
                    handle, transferred, overlapped, is_64bit,
                    STATUS_BUFFER_OVERFLOW, error);
                return true;
            }
            if (!ok) {
                const bool no_access =
                    error == error_codes::ERROR_NOACCESS;
                notify_overlapped_completion(
                    handle, 0, overlapped, is_64bit,
                    no_access ? STATUS_ACCESS_DENIED : STATUS_PIPE_BROKEN,
                    no_access ? error_codes::ERROR_NOACCESS
                              : error_codes::ERROR_BROKEN_PIPE);
                return true;
            }
            notify_overlapped_completion(
                handle, transferred, overlapped, is_64bit);
            return true;
        }});
    if (!m_overlapped_pipe_read_worker.joinable()) {
        if (m_pipe_read_wake[0] < 0 && ::pipe(m_pipe_read_wake) == 0) {
            for (const int fd : m_pipe_read_wake) {
                (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        }
        m_overlapped_pipe_read_worker = std::jthread(
            [this](std::stop_token stop) {
                run_overlapped_pipe_read_worker(stop);
            });
    }
    m_overlapped_pipe_completion_cv.notify_one();
    wake_pipe_read_worker();

    set_last_error(error_codes::ERROR_IO_PENDING);
    pipe_diag("overlapped-read-pending", handle, bytes_to_read, 0,
              overlapped);
    return OverlappedPipeIoResult::Pending;
}

OverlappedPipeIoResult FileManager::begin_overlapped_pipe_write(
    Handle handle, GuestAddr buffer_addr, u32 bytes_to_write,
    GuestAddr bytes_written_addr, GuestAddr overlapped, bool is_64bit,
    u32& bytes_written) {
    constexpr u32 STATUS_PENDING = 0x00000103;
    constexpr u32 STATUS_PIPE_BROKEN = 0xC000014B;

    bytes_written = 0;
    if (m_memory == nullptr || overlapped == 0 ||
        m_handles.get_type(handle) != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return OverlappedPipeIoResult::Failed;
    }
    auto endpoint = m_handles.get_shared<PipeEndpoint>(
        handle, HandleType::Pipe);
    auto io = pipe_io(endpoint);
    if (!endpoint || !endpoint->writable || !io) {
        set_last_error(endpoint && endpoint->writable
            ? unconnected_pipe_error(endpoint) : error_codes::ERROR_INVALID_HANDLE);
        return OverlappedPipeIoResult::Failed;
    }

    // The request exists from here on: its completion port route is
    // recorded and its hEvent reset now, as for reads (kernel32_ext trace
    // hook), so a CloseHandle before completion still reaches the port.
    trace_overlapped({
        .stage = OverlappedIoTraceStage::Submitted,
        .handle = handle,
        .object_id = static_cast<u64>(
            reinterpret_cast<std::uintptr_t>(endpoint.get())),
        .overlapped = overlapped,
        .buffer = buffer_addr,
        .requested_bytes = bytes_to_write,
        .is_64bit = is_64bit,
    });

    std::vector<u8> payload(bytes_to_write);
    if (pipe_diag_enabled()) {
        std::fprintf(stderr, "[pipe-endpoint pid=%d] write h=0x%x fd=%d peer=%d ov=0x%llx\n",
                     static_cast<int>(::getpid()), handle, io->fd(), io->peer_pid(),
                     static_cast<unsigned long long>(overlapped));
    }
    if (bytes_to_write != 0) {
        const auto read = m_memory->read_bytes(
            buffer_addr, reinterpret_cast<Byte*>(payload.data()),
            bytes_to_write);
        if (read.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return OverlappedPipeIoResult::Failed;
        }
        pipe_diag_payload("write", handle, payload.data(), payload.size());
    }

    // An OVERLAPPED caller owns its input buffer until completion, so snapshot
    // the payload before a potentially pending write. If the peer accepts the
    // complete frame inline, Windows reports synchronous success; the caller
    // still receives an IOCP packet unless it enabled
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS.
    std::vector<u8> wire;
    if (endpoint->pipe_type == PipeDataMode::Message) {
        wire.reserve(static_cast<usize>(bytes_to_write) + 4u);
        wire.push_back(static_cast<u8>(bytes_to_write & 0xFFu));
        wire.push_back(static_cast<u8>((bytes_to_write >> 8u) & 0xFFu));
        wire.push_back(static_cast<u8>((bytes_to_write >> 16u) & 0xFFu));
        wire.push_back(static_cast<u8>((bytes_to_write >> 24u) & 0xFFu));
    } else {
        wire.reserve(bytes_to_write);
    }
    wire.insert(wire.end(), payload.begin(), payload.end());

    bool delivered_inline = false;
    // Serialize submission, never transport backpressure. A write is sent
    // inline only when no pended write is queued ahead of it; otherwise its
    // unsent tail joins the endpoint's FIFO, which the write service drains
    // as the peer frees space. WriteFile(FILE_FLAG_OVERLAPPED) then returns
    // ERROR_IO_PENDING without waiting on the transport.
    std::unique_lock<std::mutex> write_lock(endpoint->write_io_mutex);
    if (endpoint->queued_writes.empty()) {
        usize offset = 0;
        while (offset < wire.size()) {
            const isize transferred = io->write_nonblocking(
                wire.data() + offset, wire.size() - offset);
            if (transferred > 0) {
                offset += static_cast<usize>(transferred);
                continue;
            }
            if (transferred < 0) {
                pipe_diag("overlapped-write-failed", handle, bytes_to_write, -1,
                          overlapped);
                set_last_error(error_codes::ERROR_BROKEN_PIPE);
                return OverlappedPipeIoResult::Failed;
            }
            break;  // EWOULDBLOCK — finish asynchronously.
        }
        if (offset == wire.size()) {
            delivered_inline = true;
            wire.clear();
        } else if (offset > 0) {
            // Partial progress: keep the unsent tail for the service.
            wire.erase(wire.begin(),
                       wire.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }

    if (delivered_inline) {
        bytes_written = bytes_to_write;
        pipe_diag("overlapped-write-sync", handle, bytes_to_write,
                  static_cast<isize>(bytes_to_write), overlapped);
        return OverlappedPipeIoResult::Completed;
    }

    const auto pending_write = is_64bit
        ? m_memory->write_u64(overlapped, STATUS_PENDING)
        : m_memory->write_u32(overlapped, STATUS_PENDING);
    const auto count_write = is_64bit
        ? m_memory->write_u64(overlapped + 8, 0)
        : m_memory->write_u32(overlapped + 4, 0);
    if (pending_write.is_err() || count_write.is_err() ||
        (bytes_written_addr != 0 &&
         m_memory->write_u32(bytes_written_addr, 0).is_err())) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return OverlappedPipeIoResult::Failed;
    }

    {
        std::lock_guard<std::mutex> lock(m_overlapped_pipe_io_mutex);
        if (!ensure_pipe_write_service()) {
            set_last_error(error_codes::ERROR_NO_SYSTEM_RESOURCES);
            return OverlappedPipeIoResult::Failed;
        }
        track_pending_pipe_io(handle, overlapped, is_64bit);
        const bool first = endpoint->queued_writes.empty();
        endpoint->queued_writes.push_back(QueuedPipeWrite{
            .handle = handle,
            .overlapped = overlapped,
            .bytes_to_write = bytes_to_write,
            .is_64bit = is_64bit,
            .wire = std::move(wire),
        });
        if (first) {
            m_armed_pipe_writers.push_back(ArmedPipeWriter{endpoint, io});
            // An unwatchable descriptor is failed by the service's drain.
            if (!arm_pipe_writer(io->fd())) wake_pipe_write_service();
        }
    }
    write_lock.unlock();

    set_last_error(error_codes::ERROR_IO_PENDING);
    pipe_diag("overlapped-write-pending", handle, bytes_to_write, 0,
              overlapped);
    return OverlappedPipeIoResult::Pending;
}

} // namespace vitreo::winapi
