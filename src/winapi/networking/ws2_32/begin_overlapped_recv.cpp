/// @file ws2_32.cpp
/// @brief Implementation of ws2_32.dll API stubs (Winsock 2)

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"

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

/// @file begin_overlapped_recv.cpp
/// @brief Ws2Manager::begin_overlapped_recv
///
/// Closed method cut from ws2_32.cpp.

namespace vitreo::winapi {

bool Ws2Manager::begin_overlapped_recv(
    u32 guest_socket, int native_fd,
    std::vector<std::pair<GuestAddr, u32>> guest_bufs,
    int recv_flags, GuestAddr overlapped, bool is_64bit,
    GuestAddr bytes_recvd_ptr, Marshaler& m,
    GuestAddr from_ptr, GuestAddr fromlen_ptr,
    GuestAddr control_ptr, u32 control_capacity,
    GuestAddr control_len_ptr, GuestAddr msg_flags_ptr,
    GuestAddr completion_routine, u32 issuing_tid,
    bool* completed_immediately, bool write_pending_status) {
    if (completed_immediately) *completed_immediately = false;
    GuestMemory* memory = m_memory ? m_memory : &m.memory();
    if (!memory || overlapped == 0) {
        set_last_error(WSAEINVAL);
        return false;
    }

    // An OVERLAPPED reads STATUS_PENDING while the request is outstanding;
    // a raw NT request leaves its IO_STATUS_BLOCK untouched until it
    // completes (afd_recv_probe blocking_waits: iosb unchanged).
    if (write_pending_status) {
        const auto pending_write = is_64bit
            ? memory->write_u64(overlapped, STATUS_PENDING)
            : memory->write_u32(overlapped, STATUS_PENDING);
        const auto count_write = is_64bit
            ? memory->write_u64(overlapped + 8, 0)
            : memory->write_u32(overlapped + 4, 0);
        if (pending_write.is_err() || count_write.is_err()) {
            set_last_error(WSAEFAULT);
            return false;
        }
    }

    size_t total_len = 0;
    for (const auto& [ptr, len] : guest_bufs) {
        (void)ptr;
        total_len += len;
    }

    // Windows: a zero-length WSARecv completes when the socket is readable
    // without consuming bytes. recvmsg with empty iovecs returns 0 immediately
    // on macOS and would spin an IOCP read loop.
    if (total_len == 0) {
        // Pending: leave *lpNumberOfBytesRecvd alone (Learn).

        // If already readable (or closed), complete after the hostcall returns.
        struct pollfd probe{};
        probe.fd = native_fd;
        probe.events = POLLIN | POLLHUP | POLLERR;
        const int ready_now = ::poll(&probe, 1, 0);
        {
            ReapedWorkers::Finished finished;
            std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
            track_pending_socket_io(guest_socket, overlapped, issuing_tid, completion_routine, is_64bit);
            if (ready_now > 0) {
                m_overlapped_socket_io_workers.spawn(
                    [this, guest_socket, overlapped, is_64bit, completion_routine, issuing_tid](
                        std::stop_token stop_token) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        const auto pending = consume_pending_socket_io(overlapped);
                        if (!pending.has_value()) return;
                        if (stop_token.stop_requested() || *pending) return;
                        notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), 0, overlapped,
                            is_64bit, STATUS_SUCCESS, 0, false, completion_routine, issuing_tid);
                    }, finished);
            } else {
                m_overlapped_socket_io_workers.spawn(
                    [this, guest_socket, native_fd, overlapped, is_64bit, completion_routine, issuing_tid](
                        std::stop_token stop_token) {
                        for (;;) {
                            if (stop_token.stop_requested()) {
                                (void)consume_pending_socket_io(overlapped);
                                return;
                            }
                            {
                                std::lock_guard<std::mutex> lk(
                                    m_overlapped_socket_io_mutex);
                                auto it = m_pending_socket_io.find(overlapped);
                                if (it == m_pending_socket_io.end() ||
                                    it->second.cancel) {
                                    if (it != m_pending_socket_io.end())
                                        m_pending_socket_io.erase(it);
                                    return;
                                }
                            }
                            struct pollfd pfd{};
                            pfd.fd = native_fd;
                            pfd.events = POLLIN | POLLHUP | POLLERR;
                            const int pr = ::poll(&pfd, 1, 50);
                            if (pr < 0) {
                                if (errno == EINTR) continue;
                                const auto pending =
                                    consume_pending_socket_io(overlapped);
                                if (!pending.has_value() || *pending) return;
                                notify_overlapped_completion(
                                    static_cast<Handle>(guest_socket), 0,
                                    overlapped, is_64bit, 0xC0000001u,
                                    errno_to_wsa(errno), false, completion_routine, issuing_tid);
                                return;
                            }
                            if (pr == 0) continue;
                            const auto pending =
                                consume_pending_socket_io(overlapped);
                            if (!pending.has_value() || *pending) return;
                            notify_overlapped_completion(
                                static_cast<Handle>(guest_socket), 0, overlapped,
                                is_64bit, STATUS_SUCCESS, 0, false, completion_routine, issuing_tid);
                            return;
                        }
                    }, finished);
            }
        }
        ipc::ipc_log(
            "WSARecv socket=0x%x ov=0x%llx total=0 -> IO_PENDING (readiness)",
            guest_socket, static_cast<unsigned long long>(overlapped));
        set_last_error(WSA_IO_PENDING);
        return true;
    }

    // WSARecvFrom's lpFrom/lpFromlen. Windows fills these when the receive
    // COMPLETES, so the deferred worker has to do it too, not just the
    // immediate path (Microsoft Learn, WSARecvFrom).
    auto publish_from = [memory, from_ptr, fromlen_ptr](
        const sockaddr_storage& ss) {
        if (!memory || from_ptr == 0 || fromlen_ptr == 0) return;
        const auto cap = memory->read_u32(fromlen_ptr);
        if (cap.is_err() || cap.value() == 0) return;
        const u32 written =
            write_win_sockaddr(*memory, from_ptr, cap.value(), ss);
        if (written != 0) (void)memory->write_u32(fromlen_ptr, written);
    };

    // WSARecvMsg's WSAMSG.Control / dwFlags, published when the receive
    // COMPLETES (ws2_msg_control.cpp). Returns the Windows MSG_* output flags:
    // MSG_TRUNC for a datagram longer than the buffers, MSG_CTRUNC when the
    // control image did not fit -- either one fails the receive with
    // WSAEMSGSIZE (ws2_msg_probe v4_recv_trunc / v4_recv_ctl_small).
    int host_family = AF_UNSPEC;
    {
        sockaddr_storage local{};
        socklen_t ll = sizeof(local);
        if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local), &ll) == 0) {
            host_family = local.ss_family;
        }
    }
    const u32 recv_ctl_mask = socket_state(guest_socket).recv_ctl;
    auto publish_control = [memory, control_ptr, control_capacity,
                            control_len_ptr, msg_flags_ptr, is_64bit,
                            recv_ctl_mask, host_family](
        const struct msghdr& native_msg) -> u32 {
        u32 windows_flags = 0;
        if ((native_msg.msg_flags & MSG_TRUNC) != 0) windows_flags |= 0x0100;
        if (!memory || control_len_ptr == 0) return windows_flags;
        const std::vector<u8> image = ws2_build_recv_control(
            native_msg, recv_ctl_mask, is_64bit, host_family);
        const Ws2ControlPublish pub = ws2_publish_recv_control(
            *memory, is_64bit, control_ptr, control_capacity, image);
        if (pub.truncated) windows_flags |= 0x0200;
        (void)memory->write_u32(control_len_ptr, pub.control_len);
        if (msg_flags_ptr != 0) (void)memory->write_u32(msg_flags_ptr, windows_flags);
        return windows_flags;
    };

    auto copy_host_to_guest = [memory](
        const std::vector<std::pair<GuestAddr, u32>>& bufs,
        const char* host, ssize_t rc) {
        if (rc <= 0 || !host) return;
        ssize_t rem = rc;
        size_t off = 0;
        for (const auto& [ptr, len] : bufs) {
            if (rem <= 0 || !ptr) break;
            const size_t n = static_cast<size_t>(
                std::min(static_cast<ssize_t>(len), rem));
            (void)memory->write_bytes(
                ptr, reinterpret_cast<const Byte*>(host + off), n);
            off += n;
            rem -= static_cast<ssize_t>(n);
        }
    };

    // Non-blocking probe.
    const int old_fl = ::fcntl(native_fd, F_GETFL, 0);
    ::fcntl(native_fd, F_SETFL, old_fl | O_NONBLOCK);

    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    const bool stream_ordered = (host_type == SOCK_STREAM);

    std::vector<char> host(total_len ? total_len : 1);
    std::vector<struct iovec> iov(guest_bufs.size());
    {
        size_t off = 0;
        for (size_t i = 0; i < guest_bufs.size(); ++i) {
            iov[i].iov_base = host.data() + off;
            iov[i].iov_len = guest_bufs[i].second;
            off += guest_bufs[i].second;
        }
    }
    struct sockaddr_storage probe_ss{};
    std::vector<char> probe_control(control_len_ptr != 0 ? 512u : 0u);
    struct msghdr msg = {};
    msg.msg_iov = iov.data();
    msg.msg_iovlen = static_cast<int>(iov.size());
    if (from_ptr != 0) {
        msg.msg_name = &probe_ss;
        msg.msg_namelen = sizeof(probe_ss);
    }
    if (!probe_control.empty()) {
        msg.msg_control = probe_control.data();
        msg.msg_controllen = probe_control.size();
    }

    // Stream: enqueue before any recv so earlier pending ops stay ahead.
    {
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        track_pending_socket_io(guest_socket, overlapped, issuing_tid,
                                completion_routine, is_64bit);
        if (stream_ordered) {
            enqueue_stream_recv(guest_socket, overlapped);
        }
    }

    // Stream with an earlier pending recv must not steal bytes — skip probe.
    bool skip_probe = false;
    if (stream_ordered) {
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        skip_probe = !is_stream_recv_head(guest_socket, overlapped);
    }

    ssize_t rc = -1;
    int recv_errno = EAGAIN;
    if (!skip_probe) {
        rc = ::recvmsg(native_fd, &msg, recv_flags);
        recv_errno = errno;
    }
    ::fcntl(native_fd, F_SETFL, old_fl);

    if (rc >= 0) {
        copy_host_to_guest(guest_bufs, host.data(), rc);
        publish_from(probe_ss);
        const u32 out_flags = publish_control(msg);
        const bool datagram_truncated =
            !stream_ordered && (out_flags & 0x0300u) != 0;
        const u32 transferred = static_cast<u32>(rc);
        (void)consume_pending_socket_io(overlapped);
        if (datagram_truncated) {
            // A datagram that did not fit completes with STATUS_BUFFER_OVERFLOW
            // and the call reports WSA_IO_PENDING, even though the OVERLAPPED
            // is already complete and hEvent signalled when it returns
            // (ws2_ovres_probe recvfrom_trunc_immediate).
            notify_overlapped_completion(
                static_cast<Handle>(guest_socket), transferred, overlapped,
                is_64bit, ws2_wsa_error_to_ntstatus(WSAEMSGSIZE), WSAEMSGSIZE,
                false, completion_routine, issuing_tid);
            set_last_error(WSA_IO_PENDING);
            return true;
        }
        // Learn: immediate completion returns 0 and updates
        // *lpNumberOfBytesRecvd; Internal/InternalHigh are already final and
        // hEvent already signalled when the call returns (ws2_ovres_probe
        // recv_immediate: internal=0 internalhigh=3 sig=1), and a completion
        // routine is queued as an APC (rt_recv_immediate: rc=0).
        if (bytes_recvd_ptr) (void)m.write_u32(bytes_recvd_ptr, transferred);
        ipc::ipc_log(
            "WSARecv socket=0x%x ov=0x%llx -> immediate=%u",
            guest_socket, static_cast<unsigned long long>(overlapped),
            transferred);
        notify_overlapped_completion(
            static_cast<Handle>(guest_socket), transferred, overlapped,
            is_64bit, STATUS_SUCCESS, 0, /*completed_synchronously=*/true,
            completion_routine, issuing_tid);
        set_last_error(0);
        if (completed_immediately) *completed_immediately = true;
        return true;
    }

    if (!skip_probe && recv_errno != EAGAIN && recv_errno != EWOULDBLOCK) {
        diagnose_recv(guest_socket, rc, recv_errno);
        (void)consume_pending_socket_io(overlapped);
        set_last_error(errno_to_wsa(recv_errno));
        return false;
    }

    // Pending: Learn leaves *lpNumberOfBytesRecvd alone.
    {
        ReapedWorkers::Finished finished;
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        // Already tracked; spawn waiter.
        m_overlapped_socket_io_workers.spawn(
            [this, guest_socket, native_fd, overlapped, is_64bit, recv_flags,
             memory, guest_bufs = std::move(guest_bufs), total_len,
             publish_from, publish_control, from_ptr,
             control_len_ptr, completion_routine, issuing_tid,
             stream_ordered](std::stop_token stop_token) mutable {
                auto still_active = [&]() -> bool {
                    std::lock_guard<std::mutex> lk(m_overlapped_socket_io_mutex);
                    auto it = m_pending_socket_io.find(overlapped);
                    if (it == m_pending_socket_io.end()) return false;
                    return !it->second.cancel;
                };
                for (;;) {
                    if (stop_token.stop_requested() || !still_active()) {
                        (void)consume_pending_socket_io(overlapped);
                        return;
                    }
                    // Stream FIFO: only the oldest pending recv may call recvmsg.
                    if (stream_ordered) {
                        bool head = false;
                        {
                            std::lock_guard<std::mutex> lk(
                                m_overlapped_socket_io_mutex);
                            head = is_stream_recv_head(guest_socket, overlapped);
                        }
                        if (!head) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(1));
                            continue;
                        }
                    }
                    // After shutdown(SD_RECEIVE) a pending receive stays
                    // pending (ws2_ovres_probe shutdown_rd_under_recv); the
                    // host's SHUT_RD reads as EOF and must not complete it.
                    if (socket_state(guest_socket).shut_rd) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        continue;
                    }
                    struct pollfd pfd{};
                    pfd.fd = native_fd;
                    pfd.events = POLLIN | POLLHUP | POLLERR;
                    const int pr = ::poll(&pfd, 1, 50);
                    if (pr < 0) {
                        if (errno == EINTR) continue;
                        const auto pending = consume_pending_socket_io(overlapped);
                        if (!pending.has_value() || *pending) return;
                        const u32 wsa = errno_to_wsa(errno);
                        notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), 0, overlapped,
                            is_64bit, 0xC0000001u, wsa, false, completion_routine, issuing_tid);
                        return;
                    }
                    if (pr == 0) continue;

                    std::vector<char> buf(total_len ? total_len : 1);
                    std::vector<struct iovec> wait_iov(guest_bufs.size());
                    size_t off = 0;
                    for (size_t i = 0; i < guest_bufs.size(); ++i) {
                        wait_iov[i].iov_base = buf.data() + off;
                        wait_iov[i].iov_len = guest_bufs[i].second;
                        off += guest_bufs[i].second;
                    }
                    struct sockaddr_storage wait_ss{};
                    std::vector<char> wait_control(control_len_ptr != 0 ? 512u : 0u);
                    struct msghdr wait_msg = {};
                    wait_msg.msg_iov = wait_iov.data();
                    wait_msg.msg_iovlen = static_cast<int>(wait_iov.size());
                    if (from_ptr != 0) {
                        wait_msg.msg_name = &wait_ss;
                        wait_msg.msg_namelen = sizeof(wait_ss);
                    }
                    if (!wait_control.empty()) {
                        wait_msg.msg_control = wait_control.data();
                        wait_msg.msg_controllen = wait_control.size();
                    }
                    // Receive and retire the request atomically with respect to
                    // CancelIoEx / closesocket, which complete a request by
                    // taking it out of the pending table under this lock: bytes
                    // are taken off the socket only for a request that is
                    // still pending, and once taken the request is ours to
                    // complete.
                    ssize_t got = -1;
                    int err = 0;
                    {
                        std::lock_guard<std::mutex> lk(m_overlapped_socket_io_mutex);
                        auto it = m_pending_socket_io.find(overlapped);
                        if (it == m_pending_socket_io.end()) return;
                        if (it->second.cancel) {
                            m_pending_socket_io.erase(it);
                            return;
                        }
                        // Stream FIFO: a newer request must not steal bytes.
                        if (stream_ordered &&
                            !is_stream_recv_head(guest_socket, overlapped)) {
                            continue;
                        }
                        // closesocket may have closed this guest and the host
                        // fd number been reused: never I/O on a stale fd; the
                        // close completes the request (audit MEDIUM).
                        if (get_native_fd(guest_socket) != native_fd) return;
                        // shutdown(SD_RECEIVE) raced the poll: stay pending.
                        if (socket_state(guest_socket).shut_rd) continue;
                        got = ::recvmsg(native_fd, &wait_msg, recv_flags | MSG_DONTWAIT);
                        err = errno;
                        if (got < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
                            continue;  // spurious readiness
                        }
                        (void)consume_pending_socket_io_locked(overlapped);
                    }
                    if (got < 0) {
                        const u32 wsa = errno_to_wsa(err);
                        ipc::ipc_log(
                            "WSARecv complete socket=0x%x ov=0x%llx error=%u",
                            guest_socket,
                            static_cast<unsigned long long>(overlapped), wsa);
                        notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), 0, overlapped,
                            is_64bit, 0xC0000001u, wsa, false, completion_routine, issuing_tid);
                        return;
                    }
                    publish_from(wait_ss);
                    const u32 out_flags = publish_control(wait_msg);
                    if (memory && got > 0) {
                        ssize_t rem = got;
                        size_t host_off = 0;
                        for (const auto& [ptr, len] : guest_bufs) {
                            if (rem <= 0 || !ptr) break;
                            const size_t n = static_cast<size_t>(std::min(
                                static_cast<ssize_t>(len), rem));
                            (void)memory->write_bytes(
                                ptr,
                                reinterpret_cast<const Byte*>(buf.data() +
                                                              host_off),
                                n);
                            host_off += n;
                            rem -= static_cast<ssize_t>(n);
                        }
                    }
                    ipc::ipc_log(
                        "WSARecv complete socket=0x%x ov=0x%llx bytes=%zd",
                        guest_socket,
                        static_cast<unsigned long long>(overlapped), got);
                    // A truncated datagram completes with STATUS_BUFFER_OVERFLOW
                    // and the truncated count (ws2_ovres_probe gor_trunc,
                    // ws2_msg_probe ovp_recvmsg_trunc_done).
                    const bool truncated =
                        !stream_ordered && (out_flags & 0x0300u) != 0;
                    notify_overlapped_completion(
                        static_cast<Handle>(guest_socket),
                        static_cast<u32>(got), overlapped, is_64bit,
                        truncated ? ws2_wsa_error_to_ntstatus(WSAEMSGSIZE)
                                  : STATUS_SUCCESS,
                        truncated ? WSAEMSGSIZE : 0, false, completion_routine,
                        issuing_tid);
                    return;
                }
            }, finished);
    }
    ipc::ipc_log(
        "WSARecv socket=0x%x ov=0x%llx total=%zu -> IO_PENDING (wait)",
        guest_socket, static_cast<unsigned long long>(overlapped), total_len);
    set_last_error(WSA_IO_PENDING);
    return true;
}

} // namespace vitreo::winapi
