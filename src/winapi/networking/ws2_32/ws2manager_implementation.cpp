// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32.cpp
/// @brief Implementation of ws2_32.dll API stubs (Winsock 2)

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/memory/heap.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

// BSD socket headers
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/// @file ws2manager_implementation.cpp
/// @brief Ws2Manager Implementation
///
/// Closed cut from ws2_32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Ws2Manager Implementation
// =============================================================================

Ws2Manager::Ws2Manager() = default;

Ws2Manager::~Ws2Manager() {
    shutdown_overlapped_socket_io();
    stop_event_pump();
}

// Caller holds m_overlapped_socket_io_mutex (this and the three stream-FIFO
// helpers below are the table operations the locked callers compose).
void Ws2Manager::track_pending_socket_io(u32 guest_socket, GuestAddr overlapped,
                                         u32 issuing_tid,
                                         GuestAddr completion_routine,
                                         bool is_64bit) {
    m_pending_socket_io[overlapped] = PendingSocketIo{
        guest_socket, overlapped, issuing_tid, completion_routine, is_64bit,
        false};
}

// Caller holds m_overlapped_socket_io_mutex.
void Ws2Manager::enqueue_stream_recv(u32 guest_socket, GuestAddr overlapped) {
    m_stream_recv_fifo[guest_socket].push_back(overlapped);
}

// Caller holds m_overlapped_socket_io_mutex.
void Ws2Manager::dequeue_stream_recv(u32 guest_socket, GuestAddr overlapped) {
    auto it = m_stream_recv_fifo.find(guest_socket);
    if (it == m_stream_recv_fifo.end()) return;
    auto& q = it->second;
    for (auto qi = q.begin(); qi != q.end(); ++qi) {
        if (*qi == overlapped) {
            q.erase(qi);
            break;
        }
    }
    if (q.empty()) m_stream_recv_fifo.erase(it);
}

// Caller holds m_overlapped_socket_io_mutex.
bool Ws2Manager::is_stream_recv_head(u32 guest_socket,
                                     GuestAddr overlapped) const {
    auto it = m_stream_recv_fifo.find(guest_socket);
    if (it == m_stream_recv_fifo.end() || it->second.empty()) return false;
    return it->second.front() == overlapped;
}

void Ws2Manager::notify_overlapped_completion(
    Handle handle, u32 bytes_transferred, GuestAddr overlapped, bool is_64bit,
    u32 nt_status, u32 win_error, bool completed_synchronously,
    GuestAddr completion_routine, u32 issuing_tid,
    u32 completion_flags) const {
    if (overlapped == 0) return;

    // A failed operation leaves the specific NTSTATUS in Internal, the one
    // WSAGetOverlappedResult maps back to the Winsock error; the generic
    // STATUS_UNSUCCESSFUL would read back as WSAEINVAL (ws2_ovres_probe map:).
    if (win_error != 0 && (nt_status == STATUS_SUCCESS || nt_status == 0xC0000001u)) {
        nt_status = ws2_wsa_error_to_ntstatus(win_error);
    }

    if (m_memory != nullptr) {
        const GuestAddr high = overlapped + (is_64bit ? 8u : 4u);
        if (is_64bit) {
            (void)m_memory->write_u64(overlapped, nt_status);
            (void)m_memory->write_u64(high, bytes_transferred);
        } else {
            (void)m_memory->write_u32(overlapped, nt_status);
            (void)m_memory->write_u32(high, bytes_transferred);
        }
    }

    // A request issued through NtDeviceIoControlFile carries its own Event,
    // ApcRoutine and ApcContext instead of an OVERLAPPED's (the status block
    // written above has the same layout).
    {
        std::unique_lock<std::mutex> lock(m_nt_route_mutex);
        if (auto route = m_nt_routes.find(overlapped); route != m_nt_routes.end()) {
            const NtRequestRoute r = route->second;
            if (r.issuing) {
                route->second.completed = true;
                route->second.status = nt_status;
            } else {
                m_nt_routes.erase(route);
            }
            lock.unlock();
            if (m_nt_completion) {
                m_nt_completion(handle, r.apc_context, r.event, r.apc_routine, overlapped, r.issuing_tid,
                                bytes_transferred, nt_status, win_error, r.issuing || completed_synchronously);
            }
            return;
        }
    }

    // Learn WSARecv/WSASend: a non-NULL lpCompletionRoutine means hEvent is
    // application context only — queue the APC and never SetEvent / IOCP.
    if (completion_routine != 0) {
        if (m_wsa_completion_queue) {
            m_wsa_completion_queue(issuing_tid, completion_routine, win_error,
                                   bytes_transferred, overlapped,
                                   completion_flags);
        } else {
            static std::atomic<bool> once{false};
            if (!once.exchange(true)) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED ws2_32!WSARecv/WSASend: "
                    "lpCompletionRoutine (APC queue not wired)\n");
            }
        }
        return;
    }

    if (m_overlapped_completion) {
        m_overlapped_completion(handle, bytes_transferred, overlapped, is_64bit,
                                nt_status, win_error, completed_synchronously);
    } else if (m_sync_manager != nullptr && m_memory != nullptr) {
        // No IOCP callback (unit tests): still signal OVERLAPPED.hEvent.
        const GuestAddr event_off = is_64bit ? 24u : 16u;
        const u64 ev = is_64bit
                           ? m_memory->read_u64(overlapped + event_off).value_or(0)
                           : m_memory->read_u32(overlapped + event_off).value_or(0);
        if (ev != 0) {
            (void)m_sync_manager->set_event(static_cast<Handle>(ev & ~u64{1}));
        }
    }
}

void Ws2Manager::complete_socket_request(
    u32 guest_socket, GuestAddr overlapped, bool is_64bit,
    GuestAddr completion_routine, u32 bytes_transferred, u32 nt_status,
    u32 win_error, u32 flags, bool completed_synchronously) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    notify_overlapped_completion(
        static_cast<Handle>(guest_socket), bytes_transferred, overlapped,
        is_64bit, nt_status, win_error, completed_synchronously,
        completion_routine, tid, flags);
}

std::optional<bool> Ws2Manager::consume_pending_socket_io(GuestAddr overlapped) {
    std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
    return consume_pending_socket_io_locked(overlapped);
}

std::optional<bool> Ws2Manager::consume_pending_socket_io_locked(GuestAddr overlapped) {
    auto it = m_pending_socket_io.find(overlapped);
    if (it == m_pending_socket_io.end()) return std::nullopt;
    const bool cancelled = it->second.cancel;
    const u32 guest = it->second.guest_socket;
    m_pending_socket_io.erase(it);
    // Drop from stream-recv FIFO if present (order gate for SOCK_STREAM).
    auto fit = m_stream_recv_fifo.find(guest);
    if (fit != m_stream_recv_fifo.end()) {
        auto& q = fit->second;
        for (auto qi = q.begin(); qi != q.end(); ++qi) {
            if (*qi == overlapped) {
                q.erase(qi);
                break;
            }
        }
        if (q.empty()) m_stream_recv_fifo.erase(fit);
    }
    return cancelled;
}

void Ws2Manager::cancel_pending_socket_io_for_socket(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
    for (auto& [ov, pending] : m_pending_socket_io) {
        (void)ov;
        if (pending.guest_socket == guest_socket) pending.cancel = true;
    }
}

usize Ws2Manager::cancel_overlapped_socket_io(u32 guest_socket,
                                              GuestAddr overlapped,
                                              bool only_calling_thread,
                                              u32 calling_tid) {
    // CancelIo / CancelIoEx: the cancelled operations COMPLETE, with
    // STATUS_CANCELLED in Internal and WSA_OPERATION_ABORTED from
    // WSAGetOverlappedResult, signalling hEvent / posting to the port / queueing
    // the routine like any completion (ws2_ovres_probe gor_cancelled). Taking
    // them out of the pending table under the lock is what keeps a worker
    // from also completing them: the recv worker receives only while its
    // entry is still present, under the same lock.
    std::vector<PendingSocketIo> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        for (auto it = m_pending_socket_io.begin(); it != m_pending_socket_io.end();) {
            const PendingSocketIo& pending = it->second;
            if (pending.guest_socket != guest_socket ||
                (overlapped != 0 && it->first != overlapped) ||
                (only_calling_thread && pending.issuing_tid != 0 &&
                 pending.issuing_tid != calling_tid)) {
                ++it;
                continue;
            }
            cancelled.push_back(pending);
            const GuestAddr ov = it->first;
            ++it;
            (void)consume_pending_socket_io_locked(ov);
        }
    }
    for (const auto& pending : cancelled) {
        notify_overlapped_completion(
            static_cast<Handle>(pending.guest_socket), 0, pending.overlapped,
            pending.is_64bit, STATUS_CANCELLED, WSA_OPERATION_ABORTED,
            /*completed_synchronously=*/false, pending.completion_routine,
            pending.issuing_tid, 0);
    }
    return cancelled.size();
}

void Ws2Manager::shutdown_overlapped_socket_io() {
    ReapedWorkers::Finished workers;
    {
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        for (auto& [ov, pending] : m_pending_socket_io) {
            (void)ov;
            pending.cancel = true;
        }
        m_overlapped_socket_io_workers.take_all(workers);
    }
    // Join outside the mutex — workers call consume_pending_socket_io.
    workers.clear();
    {
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        m_pending_socket_io.clear();
    }
}


GuestAddr Ws2Manager::get_extension_func(const std::string& name) const {
    if (!m_thunk_registry) {
        return 0;
    }
    auto result = m_thunk_registry->get_thunk_address("ws2_32.dll", name);
    if (result.is_ok()) {
        return result.value();
    }
    return 0;
}

int Ws2Manager::wsa_startup(u16 version_requested) {
    // Learn WSAStartup / WSADATA: DLL supports 1.0–2.2; wHighVersion is always
    // 2.2; wVersion is min(requested, 2.2) when the request is at least 1.0.
    const u8 major = static_cast<u8>(version_requested & 0xFF);
    const u8 minor = static_cast<u8>((version_requested >> 8) & 0xFF);
    if (major < 1) {
        return static_cast<int>(WSAVERNOTSUPPORTED);
    }
    (void)minor;
    m_startup_count.fetch_add(1, std::memory_order_acq_rel);
    return 0;
}

int Ws2Manager::wsa_cleanup() {
    // Learn: unmatched WSACleanup -> SOCKET_ERROR / WSANOTINITIALISED.
    u32 prev = m_startup_count.load(std::memory_order_acquire);
    for (;;) {
        if (prev == 0) {
            set_last_error(WSANOTINITIALISED);
            return SOCKET_ERROR_VAL;
        }
        if (m_startup_count.compare_exchange_weak(
                prev, prev - 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return 0;
        }
    }
}

u32 Ws2Manager::alloc_async_task() {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    u32 h = m_next_async_handle.fetch_add(1, std::memory_order_relaxed);
    if (h == 0) h = m_next_async_handle.fetch_add(1, std::memory_order_relaxed);
    m_async_tasks[h] = AsyncTask{};
    return h;
}

bool Ws2Manager::complete_async_task(u32 handle) {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    auto it = m_async_tasks.find(handle);
    if (it == m_async_tasks.end()) return false;
    if (it->second.cancelled) {
        m_async_tasks.erase(it);
        return false;
    }
    it->second.completed = true;
    m_async_tasks.erase(it);
    return true;
}

u32 Ws2Manager::cancel_async_task(u32 handle) {
    // Learn WSACancelAsyncRequest: unknown/completed -> WSAEINVAL;
    // success clears the pending operation (no completion message).
    std::lock_guard<std::mutex> lock(m_async_mutex);
    auto it = m_async_tasks.find(handle);
    if (it == m_async_tasks.end() || it->second.completed) {
        return WSAEINVAL;
    }
    it->second.cancelled = true;
    m_async_tasks.erase(it);
    return 0;
}

bool Ws2Manager::async_task_active(u32 handle) const {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    auto it = m_async_tasks.find(handle);
    return it != m_async_tasks.end() && !it->second.cancelled &&
           !it->second.completed;
}

int Ws2Manager::get_hostname_w(char* hostname, size_t hostname_len) {
    if (::gethostname(hostname, hostname_len) == 0) {
        return 0;
    }
    return -1;
}

namespace {
void prevent_host_socket_inheritance(int fd) {
    // A host descriptor is not an inherited Windows SOCKET. Child runtimes
    // need an explicit guest-handle transfer to use one. Leaving raw fds open
    // across exec otherwise lets unrelated children retain listeners and
    // connected peers after their owning process closes them.
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) return; // No live descriptor can leak in this case.
    int result;
    do {
        result = ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        std::fprintf(stderr, "[ws2_32] cannot mark socket fd=%d close-on-exec: errno=%d\n",
                     fd, errno);
    }
}
} // namespace

u32 Ws2Manager::alloc_socket(int native_fd) {
    // Windows reports a failed send through SOCKET_ERROR/WSAGetLastError.
    // macOS otherwise terminates the host process with SIGPIPE.
    suppress_sigpipe(native_fd);
    prevent_host_socket_inheritance(native_fd);
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    u32 guest = m_next_socket++;
    m_socket_map[guest] = native_fd;
    return guest;
}

int Ws2Manager::get_native_fd(u32 guest_socket) const {
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    auto it = m_socket_map.find(guest_socket);
    if (it != m_socket_map.end()) {
        return it->second;
    }
    return -1;
}

bool Ws2Manager::close_socket(u32 guest_socket, bool reset) {
    // Learn closesocket: pending overlapped ops are cancelled and completed
    // with WSA_OPERATION_ABORTED (IOCP / event / completion routine).
    // Drop the guest→fd mapping before notifying so overlapped workers that
    // re-check get_native_fd cannot I/O on a reused host fd number.
    int native_fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        auto it = m_socket_map.find(guest_socket);
        if (it == m_socket_map.end()) {
            return false;
        }
        native_fd = it->second;
        m_socket_map.erase(it);
        m_socket_state.erase(guest_socket);
    }
    // closesocket completes what is still pending: on a connected stream with
    // STATUS_CONNECTION_ABORTED (WSAECONNABORTED to a completion routine), on a
    // datagram socket with STATUS_CANCELLED (ws2_ovres_probe closed_under_recv,
    // close_tcp_recv_routine, close_udp_recv).
    bool connected_stream = false;
    if (native_fd >= 0) {
        int type = 0;
        socklen_t tl = sizeof(type);
        sockaddr_storage peer{};
        socklen_t pl = sizeof(peer);
        connected_stream =
            ::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 &&
            type == SOCK_STREAM &&
            ::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0;
    }
    // WSACleanup resets connections (ws2_corex_probe cleanup_pending_internal:
    // 0xC000020D, STATUS_CONNECTION_RESET).
    const u32 abort_error = !connected_stream ? WSA_OPERATION_ABORTED
                            : reset          ? WSAECONNRESET
                                             : WSAECONNABORTED;
    if (reset && connected_stream) {
        struct linger l{1, 0};
        (void)::setsockopt(native_fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    }
    std::vector<PendingSocketIo> aborted;
    {
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        for (auto it = m_pending_socket_io.begin();
             it != m_pending_socket_io.end();) {
            if (it->second.guest_socket != guest_socket) {
                ++it;
                continue;
            }
            aborted.push_back(it->second);
            it = m_pending_socket_io.erase(it);
        }
        m_stream_recv_fifo.erase(guest_socket);
    }
    for (const auto& pending : aborted) {
        notify_overlapped_completion(
            static_cast<Handle>(guest_socket), 0, pending.overlapped,
            pending.is_64bit, ws2_wsa_error_to_ntstatus(abort_error), abort_error,
            /*completed_synchronously=*/false, pending.completion_routine,
            pending.issuing_tid, 0);
    }
    {
        std::lock_guard<std::mutex> lock(m_event_mutex);
        m_event_assocs.erase(guest_socket);
        m_event_sources.erase(guest_socket);
        m_pending_connect.erase(guest_socket);
        m_listening.erase(guest_socket);
    }
    if (native_fd >= 0) {
        ::close(native_fd);
    }
    return true;
}

Ws2Manager::SocketState Ws2Manager::socket_state(u32 guest_socket) const {
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    auto it = m_socket_state.find(guest_socket);
    return it == m_socket_state.end() ? SocketState{} : it->second;
}

void Ws2Manager::inherit_listener_state(u32 listener, u32 accepted) {
    {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        if (!m_socket_map.contains(accepted)) return;
        auto it = m_socket_state.find(listener);
        SocketState copy = it == m_socket_state.end() ? SocketState{} : it->second;
        copy.connect_error = 0;
        copy.connect_in_progress = false;
        copy.shut_rd = copy.shut_wr = false;
        copy.connect_time_ms = static_cast<i64>(now_steady_ns() / 1000000ull);
        copy.was_connected = true;
        copy.aborted = false;
        copy.bound_by_connect = false;
        m_socket_state[accepted] = std::move(copy);
    }
    // Learn accept: accepted socket inherits WSAEventSelect / WSAAsyncSelect
    // registration from the listener (ws2_core_probe accepted_event / enum)
    // with an empty record; being connected, it owes FD_WRITE
    // (ws2_event_record_probe accepted_socket_enum: 0x2).
    std::lock_guard<std::mutex> elock(m_event_mutex);
    auto eit = m_event_assocs.find(listener);
    if (eit == m_event_assocs.end()) return;
    EventAssoc& assoc = m_event_assocs[accepted];
    assoc = EventAssoc{eit->second.event_handle, eit->second.event_mask};
    observe_and_signal_locked(accepted, assoc, /*selecting=*/false);
    ensure_event_pump_locked();
    m_event_pump_cv.notify_all();
}

void Ws2Manager::update_socket_state(
    u32 guest_socket, const std::function<void(SocketState&)>& fn) {
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    if (!m_socket_map.contains(guest_socket)) return;
    fn(m_socket_state[guest_socket]);
}

void Ws2Manager::replace_native_fd(u32 guest_socket, int new_fd) {
    suppress_sigpipe(new_fd);
    prevent_host_socket_inheritance(new_fd);
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    m_socket_map[guest_socket] = new_fd;
}

std::vector<u32> Ws2Manager::socket_handles() const {
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    std::vector<u32> out;
    out.reserve(m_socket_map.size());
    for (const auto& [guest, fd] : m_socket_map) {
        (void)fd;
        out.push_back(guest);
    }
    return out;
}

u32 Ws2Manager::register_duplicated_socket(u32 guest_socket, int dup_fd) {
    prevent_host_socket_inheritance(dup_fd);
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    auto it = m_socket_state.find(guest_socket);
    DuplicatedSocket d;
    d.fd = dup_fd;
    if (it != m_socket_state.end()) d.state = it->second;
    const u32 token = m_next_dup_token++;
    m_duplicated[token] = std::move(d);
    return token;
}

int Ws2Manager::take_duplicated_socket(u32 token, SocketState* state) {
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    auto it = m_duplicated.find(token);
    if (it == m_duplicated.end()) return -1;
    const int fd = ::dup(it->second.fd);
    if (fd >= 0 && state != nullptr) *state = it->second.state;
    return fd;
}

void Ws2Manager::clear_duplicated_sockets() {
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    for (auto& [token, d] : m_duplicated) {
        (void)token;
        if (d.fd >= 0) ::close(d.fd);
    }
    m_duplicated.clear();
}

void Ws2Manager::shutdown_all_sockets() {
    shutdown_overlapped_socket_io();
    stop_event_pump();
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    for (auto& [guest, native_fd] : m_socket_map) {
        ::shutdown(native_fd, SHUT_RDWR);
    }
}

u32 Ws2Manager::errno_to_wsa(int err) {
    switch (err) {
    case 0:           return 0;
    case EINTR:       return WSAEINTR;
    case EBADF:       return WSAEBADF;
    case EACCES:      return WSAEACCES;
    case EFAULT:      return WSAEFAULT;
    case EINVAL:      return WSAEINVAL;
    case EMFILE:      return WSAEMFILE;
#ifdef ENFILE
    case ENFILE:      return WSAEMFILE;
#endif
#ifdef ENOMEM
    case ENOMEM:      return WSAENOBUFS;
#endif
    case EAGAIN:      return WSAEWOULDBLOCK;  // EWOULDBLOCK == EAGAIN on most systems
    case EINPROGRESS: return WSAEINPROGRESS;
    case EALREADY:    return WSAEALREADY;
    case ENOTSOCK:    return WSAENOTSOCK;
    case EDESTADDRREQ:return WSAEDESTADDRREQ;
    case EMSGSIZE:    return WSAEMSGSIZE;
    case EPROTOTYPE:  return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
#endif
    case EOPNOTSUPP:  return WSAEOPNOTSUPP;
    case EAFNOSUPPORT:return WSAEAFNOSUPPORT;
    case EADDRINUSE:  return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN:    return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET:   return WSAENETRESET;
    case ECONNABORTED:return WSAECONNABORTED;
    case ECONNRESET:  return WSAECONNRESET;
    case EPIPE:       return WSAECONNRESET;
    case ENOBUFS:     return WSAENOBUFS;
    case EISCONN:     return WSAEISCONN;
    case ENOTCONN:    return WSAENOTCONN;
    case ETIMEDOUT:   return WSAETIMEDOUT;
    case ECONNREFUSED:return WSAECONNREFUSED;
    case EHOSTUNREACH:return WSAEHOSTUNREACH;
    // Darwin keeps the 4.4BSD number 45 for ENOTSUP (EOPNOTSUPP is 102);
    // winsock.h defines WSAEOPNOTSUPP as the same BSD 45 (ws2_opts_probe
    // tcp_TCP_FASTOPEN once leaked a raw err=45).
    case ENOTSUP:     return WSAEOPNOTSUPP;
    case EPERM:       return WSAEACCES;
    case ESHUTDOWN:   return WSAESHUTDOWN;
    case EHOSTDOWN:   return 10064;  // WSAEHOSTDOWN
    case ELOOP:       return 10062;  // WSAELOOP
    case ENAMETOOLONG:return 10063;  // WSAENAMETOOLONG
    case ETOOMANYREFS:return 10059;  // WSAETOOMANYREFS
    case EPFNOSUPPORT:return 10046;  // WSAEPFNOSUPPORT
    case ENOTEMPTY:   return 10066;  // WSAENOTEMPTY
    case EIO:         return WSAENETDOWN;
    // Never hand a raw host errno to a guest that compares WSA codes.
    default:          return WSAEINVAL;
    }
}

bool Ws2Manager::peer_shut_down(int native_fd) {
    char byte = 0;
    return ::recv(native_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

u32 Ws2Manager::latch_connect_error(u32 guest_socket, int native_fd,
                                    bool* still_pending) {
    if (still_pending) *still_pending = false;
    const SocketState st = socket_state(guest_socket);
    if (st.connect_error != 0) return st.connect_error;
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(native_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
        err == 0) {
        return 0;
    }
    if (err == EINPROGRESS) {
        if (still_pending) *still_pending = true;
        return 0;
    }
    const u32 wsa = errno_to_wsa(err);
    update_socket_state(guest_socket, [&](SocketState& s) {
        s.connect_error = wsa;
        s.connect_in_progress = false;
    });
    return wsa;
}

void Ws2Manager::remove_event_assoc_by_socket(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_event_assocs.erase(guest_socket);
}

void Ws2Manager::remove_event_assoc_by_event(u32 event_handle) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    std::erase_if(m_event_assocs, [event_handle](const auto& entry) {
        return entry.second.event_handle == event_handle;
    });
}

std::optional<Ws2Manager::EventAssoc>
Ws2Manager::get_event_assoc(u32 guest_socket) const {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    auto it = m_event_assocs.find(guest_socket);
    if (it == m_event_assocs.end()) return std::nullopt;
    return it->second;
}

void Ws2Manager::mark_listening(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_listening.insert(guest_socket);
}

bool Ws2Manager::is_listening(u32 guest_socket) const {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    return m_listening.contains(guest_socket);
}

void Ws2Manager::mark_pending_connect(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_pending_connect.insert(guest_socket);
    if (auto it = m_event_sources.find(guest_socket);
        it != m_event_sources.end()) {
        it->second.connected = false;
        it->second.write_owed = false;
        it->second.close_seen = false;
    }
    ensure_event_pump_locked();
    m_event_pump_cv.notify_all();
}

bool Ws2Manager::has_pending_connect(u32 guest_socket) const {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    return m_pending_connect.contains(guest_socket);
}

void Ws2Manager::ensure_event_pump_locked() {
    if (m_event_pump_started || m_sync_manager == nullptr) {
        return;
    }
    m_event_pump_stop.store(false, std::memory_order_release);
    m_event_pump_started = true;
    m_event_pump_thread = std::thread([this] { event_pump_loop(); });
}

void Ws2Manager::stop_event_pump() {
    std::thread joinable;
    {
        std::lock_guard<std::mutex> lock(m_event_mutex);
        if (!m_event_pump_started) {
            return;
        }
        m_event_pump_stop.store(true, std::memory_order_release);
        m_event_pump_cv.notify_all();
        joinable = std::move(m_event_pump_thread);
        m_event_pump_started = false;
    }
    if (joinable.joinable()) {
        joinable.join();
    }
}

void Ws2Manager::event_pump_loop() {
    constexpr auto kSlice = std::chrono::milliseconds(10);
    while (!m_event_pump_stop.load(std::memory_order_acquire)) {
        refresh_all_socket_events();
        std::unique_lock<std::mutex> lock(m_event_pump_mutex);
        m_event_pump_cv.wait_for(lock, kSlice, [this] {
            return m_event_pump_stop.load(std::memory_order_acquire);
        });
    }
}




GuestAddr Ws2Manager::guest_alloc(u32 size) {
    if (m_heap == nullptr || size == 0) return 0;
    constexpr u32 kHeapZeroMemory = 0x00000008;
    return static_cast<GuestAddr>(
        m_heap->heap_alloc(m_heap->get_process_heap(), kHeapZeroMemory, size));
}

void Ws2Manager::guest_free(GuestAddr addr) {
    if (m_heap == nullptr || addr == 0) return;
    (void)m_heap->heap_free(m_heap->get_process_heap(), 0, addr);
}

GuestAddr Ws2Manager::thread_db_block(DbBlock kind, u32 size) {
    if (m_heap == nullptr) {
        // Unit tests without a guest heap: the legacy scratch arena.
        scratch_reset();
        return scratch_alloc(size);
    }
    const u64 slot_key =
        (static_cast<u64>(ThreadManager::get_current_thread_id_static()) << 8) |
        static_cast<u64>(kind);
    std::lock_guard<std::mutex> lock(m_db_block_mutex);
    DbBlockSlot& slot = m_db_blocks[slot_key];
    if (slot.size < size) {
        guest_free(slot.addr);
        const u32 want = std::max<u32>(size, 512);
        slot.addr = guest_alloc(want);
        slot.size = slot.addr != 0 ? want : 0;
    }
    return slot.addr;
}

GuestAddr Ws2Manager::scratch_alloc(u32 size) noexcept {
    // Align to 4 bytes
    size = (size + 3) & ~3u;
    
    if (m_scratch_offset + size > WS2_HOSTENT_SCRATCH_SIZE) {
        return 0; // Out of space
    }
    
    GuestAddr addr = WS2_SCRATCH_BASE + m_scratch_offset;
    m_scratch_offset += size;
    return addr;
}

GuestAddr Ws2Manager::addrinfo_alloc(u32 size) noexcept {
    size = (size + 7u) & ~7u;
    // Learn freeaddrinfo: each result chain is independently heap-allocated.
    // Prefer the guest process heap so keeping one live result never exhausts
    // a fixed bump arena (ws2 getaddrinfo HIGH).
    if (m_heap != nullptr) {
        const GuestAddr addr = guest_alloc(size);
        if (addr == 0) return 0;
        std::lock_guard<std::mutex> lock(m_addrinfo_mutex);
        m_addrinfo_live.emplace(addr, size);
        return addr;
    }
    std::lock_guard<std::mutex> lock(m_addrinfo_mutex);
    if (m_addrinfo_live.empty()) {
        m_addrinfo_offset = 0;
    }
    if (m_addrinfo_offset + size > WS2_ADDRINFO_SIZE) {
        return 0;
    }
    GuestAddr addr = WS2_ADDRINFO_BASE + m_addrinfo_offset;
    m_addrinfo_offset += size;
    m_addrinfo_live.emplace(addr, size);
    return addr;
}

void Ws2Manager::addrinfo_free(GuestAddr root) noexcept {
    if (!root) return;
    bool from_heap = false;
    {
        std::lock_guard<std::mutex> lock(m_addrinfo_mutex);
        auto it = m_addrinfo_live.find(root);
        if (it == m_addrinfo_live.end()) return;
        m_addrinfo_live.erase(it);
        from_heap = m_heap != nullptr;
        if (!from_heap && m_addrinfo_live.empty()) {
            m_addrinfo_offset = 0;
        }
    }
    if (from_heap) guest_free(root);
}

} // namespace vitreo::winapi
