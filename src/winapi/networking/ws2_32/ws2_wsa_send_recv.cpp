// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_wsa_send_recv.cpp
/// @brief WSASend / WSARecv family
///
/// Closed method cuts from ws2_32_overlapped.cpp.

#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

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

namespace vitreo::winapi {

namespace {

GuestAddr read_guest_pointer(Marshaler& m, GuestAddr address, bool is64) {
    if (is64) {
        const auto value = m.read_u64(address);
        return value.is_ok() ? static_cast<GuestAddr>(value.value()) : 0;
    }
    const auto value = m.read_u32(address);
    return value.is_ok() ? static_cast<GuestAddr>(value.value()) : 0;
}

} // namespace

namespace {

/// Marker-gated census for the overlapped UDP entry points. Datagram-relay
/// probes go through WSASendTo/WSARecvFrom, not sendto/recvfrom, so
/// instrumenting only the latter showed zero traffic on 29 live UDP sockets
/// and proved nothing. touch /tmp/vitreo-net-stats to enable.
/// Last IPv4 peer seen on each direction, so the census can say *who* the
/// guest is exchanging datagrams with. "Sends and receives" is not the same
/// as "reaches the relays", and only the peer address distinguishes them.
inline std::atomic<unsigned> g_last_send_ip{0};
inline std::atomic<unsigned> g_last_send_port{0};
inline std::atomic<unsigned> g_last_recv_ip{0};
inline std::atomic<unsigned> g_last_recv_port{0};

inline std::atomic<unsigned> g_send_null_dest{0};
inline std::atomic<unsigned> g_send_af{0};
inline std::atomic<unsigned> g_recv_af{0};

void note_udp_peer(bool is_send, const struct sockaddr* addr) {
    // Record the shape even when it is not IPv4: "peer 0.0.0.0" is ambiguous
    // between a null destination (a connected socket) and a family this code
    // does not decode, and those mean very different things.
    if (addr == nullptr) {
        if (is_send) g_send_null_dest.fetch_add(1);
        return;
    }
    (is_send ? g_send_af : g_recv_af).store(addr->sa_family);
    if (addr->sa_family != AF_INET) return;
    const auto* in4 = reinterpret_cast<const struct sockaddr_in*>(addr);
    const unsigned ip = ntohl(in4->sin_addr.s_addr);
    const unsigned port = ntohs(in4->sin_port);
    if (is_send) {
        g_last_send_ip.store(ip);
        g_last_send_port.store(port);
    } else {
        g_last_recv_ip.store(ip);
        g_last_recv_port.store(port);
    }
}

void wsa_udp_census(const char* what, long long rc, unsigned wsa_error) {
    static const bool enabled = ::access("/tmp/vitreo-net-stats", F_OK) == 0;
    if (!enabled) return;
    static std::atomic<unsigned long long> sends{0};
    static std::atomic<unsigned long long> send_errors{0};
    static std::atomic<unsigned long long> recvs{0};
    static std::atomic<unsigned long long> recv_errors{0};
    static std::atomic<unsigned long long> total{0};
    static std::atomic<unsigned> last_error{0};
    const bool is_send = std::strstr(what, "Send") != nullptr;
    // Count the Msg variants separately: lumping them under the To labels
    // hid which entry point the guest actually uses, which is the whole
    // question when only one of them is implemented.
    static std::atomic<unsigned long long> msg_sends{0};
    static std::atomic<unsigned long long> msg_recvs{0};
    if (std::strstr(what, "Msg") != nullptr) {
        (is_send ? msg_sends : msg_recvs).fetch_add(1);
    }
    if (rc < 0) {
        (is_send ? send_errors : recv_errors).fetch_add(1);
        last_error.store(wsa_error);
    } else {
        (is_send ? sends : recvs).fetch_add(1);
    }
    const unsigned long long seen = total.fetch_add(1) + 1;
    // Also write on the first call: a relay probe may be a handful of
    // packets, and a modulo-only trigger then produces no file at all —
    // indistinguishable from "the entry point is never called".
    if (seen != 1ull && (seen % 100ull) != 0ull) return;
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/vitreo-udp-%d.log",
                  static_cast<int>(::getpid()));
    if (std::FILE* file = std::fopen(path, "w")) {
        const unsigned sip = g_last_send_ip.load();
        const unsigned rip = g_last_recv_ip.load();
        std::fprintf(file,
                     "WSASendTo_ok=%llu WSASendTo_err=%llu "
                     "WSARecvFrom_ok=%llu WSARecvFrom_err=%llu "
                     "last_wsa_error=%u last_send_peer=%u.%u.%u.%u:%u "
                     "last_recv_peer=%u.%u.%u.%u:%u send_null_dest=%u "
                     "send_af=%u recv_af=%u WSASendMsg=%llu "
                     "WSARecvMsg=%llu\n",
                     sends.load(), send_errors.load(), recvs.load(),
                     recv_errors.load(), last_error.load(),
                     (sip >> 24) & 0xFFu, (sip >> 16) & 0xFFu,
                     (sip >> 8) & 0xFFu, sip & 0xFFu,
                     g_last_send_port.load(),
                     (rip >> 24) & 0xFFu, (rip >> 16) & 0xFFu,
                     (rip >> 8) & 0xFFu, rip & 0xFFu,
                     g_last_recv_port.load(), g_send_null_dest.load(),
                     g_send_af.load(), g_recv_af.load(), msg_sends.load(),
                     msg_recvs.load());
        (void)std::fclose(file);
    }
}

} // namespace

void ws2::impl_WSAGetOverlappedResult(Marshaler& m, Ws2Manager& mgr) {
    // BOOL WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped,
    //                             LPDWORD lpcbTransfer, BOOL fWait,
    //                             LPDWORD lpdwFlags)
    //
    // Learn WSAGetOverlappedResult; the rest is ws2_ovres_probe-measured:
    // - an invalid socket fails WSAENOTSOCK even for a completed OVERLAPPED
    //   (gor_bad_socket, gor_after_close), outputs untouched;
    // - Internal == STATUS_PENDING with fWait FALSE fails WSA_IO_INCOMPLETE,
    //   outputs untouched (gor_pending_nowait);
    // - otherwise *lpcbTransfer = InternalHigh whatever the outcome
    //   (map: xfer=7 for every status), and the status is mapped to a Winsock
    //   error (map:), *lpdwFlags written only on success.
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) {
        m.set_return(0);
        return;
    }
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const GuestAddr overlapped = m.arg_ptr(1);
    const GuestAddr transfer_ptr = m.arg_ptr(2);
    const bool wait = m.arg32(3) != 0;
    const GuestAddr flags_ptr = m.arg_ptr(4);
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(0);
    };
    if (mgr.get_native_fd(guest_socket) < 0) return fail(WSAENOTSOCK);
    if (overlapped == 0 || transfer_ptr == 0 || flags_ptr == 0) return fail(WSAEFAULT);

    const bool is64 = m.cpu_is_64bit();
    const GuestAddr high_off = is64 ? 8u : 4u;
    const GuestAddr event_off = is64 ? 24u : 16u;
    auto read_status = [&]() -> std::optional<u32> {
        // NTSTATUS lives in the low 32 bits of the ULONG_PTR Internal field.
        const auto r = m.read_u32(overlapped);
        if (r.is_err()) return std::nullopt;
        return r.value();
    };
    std::optional<u32> status = read_status();
    if (!status.has_value()) return fail(WSAEFAULT);
    if (*status == STATUS_PENDING) {
        if (!wait) return fail(996);  // WSA_IO_INCOMPLETE
        // fWait: block until the operation completes. The provider waits on
        // hEvent when there is one; the completion also stores the final
        // status before it signals, so re-reading Internal is authoritative.
        const u64 ev = is64 ? m.read_u64(overlapped + event_off).value_or(0)
                            : m.read_u32(overlapped + event_off).value_or(0);
        const Handle event = static_cast<Handle>(ev & ~u64{1});
        while (status.has_value() && *status == STATUS_PENDING) {
            if (event != 0) {
                const Handle h[1] = {event};
                (void)mgr.wait_for_multiple_events(std::span<const Handle>(h, 1),
                                                   false, 50, false);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            status = read_status();
        }
        if (!status.has_value()) return fail(WSAEFAULT);
    }
    const u32 transferred = m.read_u32(overlapped + high_off).value_or(0);
    (void)m.write_u32(transfer_ptr, transferred);
    if (static_cast<i32>(*status) >= 0) {
        (void)m.write_u32(flags_ptr, 0);
        mgr.set_last_error(ws2_ntstatus_to_wsa_error(*status));
        m.set_return(1);
        return;
    }
    fail(ws2_ntstatus_to_wsa_error(*status));
}

void ws2::impl_WSARecvMsg(Marshaler& m, Ws2Manager& mgr) {
    // INT WSARecvMsg(SOCKET s, LPWSAMSG lpMsg, LPDWORD lpdwNumberOfBytesRecd,
    //                LPWSAOVERLAPPED lpOverlapped,
    //                LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
    //
    // Learn (LPFN_WSARECVMSG): datagram and raw sockets only; WSAMSG.dwFlags
    // is MSG_PEEK on input and MSG_TRUNC / MSG_CTRUNC / MSG_BCAST / MSG_MCAST
    // on output; WSAMSG.Control receives the ancillary data the socket asked
    // for. Everything Learn leaves open is ws2_msg_probe-measured.
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const FdReadReenable reenable_fd_read_on_return(mgr, guest_socket);
    const GuestAddr msg_ptr = m.arg_ptr(1);
    const GuestAddr bytes_ptr = m.arg_ptr(2);
    const GuestAddr overlapped = m.arg_ptr(3);
    const GuestAddr completion_routine = m.arg_ptr(4);
    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };

    const int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) return fail(WSAENOTSOCK);
    if (msg_ptr == 0) return fail(WSAEFAULT);  // v4_recv_null_msg
    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    if (host_type == SOCK_STREAM) return fail(WSAEINVAL);  // recv_tcp
    if (overlapped == 0 && bytes_ptr == 0) return fail(WSAEFAULT);

    const bool is64 = m.cpu_is_64bit();
    const u32 name_len_off = is64 ? 8u : 4u;
    const u32 buffers_off = is64 ? 16u : 8u;
    const u32 count_off = is64 ? 24u : 12u;
    const u32 control_len_off = is64 ? 32u : 16u;
    const u32 control_ptr_off = is64 ? 40u : 20u;
    const u32 flags_off = is64 ? 48u : 24u;
    const u32 wsabuf_stride = is64 ? 16u : 8u;
    const u32 wsabuf_ptr_off = is64 ? 8u : 4u;

    const GuestAddr name_ptr = read_guest_pointer(m, msg_ptr, is64);
    const GuestAddr buffers_ptr =
        read_guest_pointer(m, msg_ptr + buffers_off, is64);
    const GuestAddr control_ptr =
        read_guest_pointer(m, msg_ptr + control_ptr_off, is64);
    const auto count_r = m.read_u32(msg_ptr + count_off);
    const auto control_len_r = m.read_u32(msg_ptr + control_len_off);
    const auto flags_r = m.read_u32(msg_ptr + flags_off);
    if (count_r.is_err() || control_len_r.is_err() || flags_r.is_err() ||
        (count_r.value() != 0 && buffers_ptr == 0) || count_r.value() > 1024) {
        return fail(WSAEFAULT);
    }

    std::vector<std::pair<GuestAddr, u32>> guest_bufs;
    guest_bufs.reserve(count_r.value());
    size_t total_len = 0;
    for (u32 i = 0; i < count_r.value(); ++i) {
        const GuestAddr wsabuf = buffers_ptr + i * wsabuf_stride;
        const auto len_r = m.read_u32(wsabuf);
        if (len_r.is_err()) return fail(WSAEFAULT);
        const GuestAddr data_ptr =
            read_guest_pointer(m, wsabuf + wsabuf_ptr_off, is64);
        if (len_r.value() != 0 && data_ptr == 0) return fail(WSAEFAULT);
        guest_bufs.emplace_back(data_ptr, len_r.value());
        total_len += len_r.value();
    }

    const int recv_flags = (flags_r.value() & 0x2u) != 0 ? MSG_PEEK : 0;
    if (overlapped != 0) {
        bool immediate = false;
        const bool ok = mgr.begin_overlapped_recv(
            guest_socket, native_fd, std::move(guest_bufs), recv_flags,
            overlapped, is64, bytes_ptr, m, name_ptr,
            msg_ptr + name_len_off, control_ptr, control_len_r.value(),
            msg_ptr + control_len_off, msg_ptr + flags_off,
            completion_routine, issuing_tid, &immediate);
        // Learn WSAEventSelect: a receive call re-enables FD_READ.
        mgr.reenable_fd_read(guest_socket, native_fd);
        m.set_return(ok && immediate ? 0 : static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    std::vector<char> data(total_len != 0 ? total_len : 1u);
    std::vector<struct iovec> iov(guest_bufs.size());
    size_t offset = 0;
    for (size_t i = 0; i < guest_bufs.size(); ++i) {
        iov[i].iov_base = data.data() + offset;
        iov[i].iov_len = guest_bufs[i].second;
        offset += guest_bufs[i].second;
    }
    struct sockaddr_storage source{};
    std::vector<char> control(512u);
    struct msghdr native_msg{};
    native_msg.msg_iov = iov.data();
    native_msg.msg_iovlen = static_cast<int>(iov.size());
    native_msg.msg_name = &source;
    native_msg.msg_namelen = sizeof(source);
    native_msg.msg_control = control.data();
    native_msg.msg_controllen = static_cast<socklen_t>(control.size());

    const ssize_t received = ::recvmsg(native_fd, &native_msg, recv_flags);
    const int recv_errno = errno;
    mgr.reenable_fd_read(guest_socket, native_fd);
    if (received < 0) {
        const u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, recv_errno, false);
        wsa_udp_census("WSARecvMsg", -1, wsa_err);
        return fail(wsa_err);
    }
    note_udp_peer(false,
                  static_cast<const struct sockaddr*>(native_msg.msg_name));
    wsa_udp_census("WSARecvMsg", received, 0);

    ssize_t remaining = received;
    size_t host_offset = 0;
    for (const auto& [guest_ptr, length] : guest_bufs) {
        if (remaining <= 0) break;
        const size_t copied = static_cast<size_t>(
            std::min(static_cast<ssize_t>(length), remaining));
        if (copied != 0) {
            (void)m.memory().write_bytes(
                guest_ptr,
                reinterpret_cast<const Byte*>(data.data() + host_offset),
                copied);
        }
        host_offset += copied;
        remaining -= static_cast<ssize_t>(copied);
    }
    if (name_ptr != 0) {
        const auto capacity = m.read_u32(msg_ptr + name_len_off);
        if (capacity.is_ok()) {
            const u32 written = write_win_sockaddr(
                m.memory(), name_ptr, capacity.value(), source);
            if (written != 0) (void)m.write_u32(msg_ptr + name_len_off, written);
        }
    }

    sockaddr_storage local{};
    socklen_t ll = sizeof(local);
    const int host_family =
        ::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local), &ll) == 0
            ? local.ss_family : AF_UNSPEC;
    const std::vector<u8> image = ws2_build_recv_control(
        native_msg, mgr.socket_state(guest_socket).recv_ctl, is64, host_family);
    const Ws2ControlPublish pub = ws2_publish_recv_control(
        m.memory(), is64, control_ptr, control_len_r.value(), image);
    u32 out_flags = 0;
    if ((native_msg.msg_flags & MSG_TRUNC) != 0) out_flags |= 0x0100;  // MSG_TRUNC
    if (pub.truncated) out_flags |= 0x0200;                              // MSG_CTRUNC
    (void)m.write_u32(msg_ptr + control_len_off, pub.control_len);
    (void)m.write_u32(msg_ptr + flags_off, out_flags);
    (void)m.write_u32(bytes_ptr, static_cast<u32>(received));
    VITREO_TRACE("[ws2_32] WSARecvMsg(socket=0x%x) -> %zd bytes flags=0x%x\n",
                 guest_socket, received, out_flags);
    // A truncated datagram or control image still delivers what fit, and
    // fails with WSAEMSGSIZE (ws2_msg_probe v4_recv_trunc / v4_recv_ctl_small).
    if (out_flags != 0) return fail(WSAEMSGSIZE);
    m.set_return(0);
}

void ws2::impl_WSARecv(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    //         LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    //         LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE)
    // → 0 on success, SOCKET_ERROR on failure / pending
    u32 guest_socket = static_cast<u32>(m.arg(0));
    const FdReadReenable reenable_fd_read_on_return(mgr, guest_socket);
    GuestAddr buffers_ptr = m.arg_ptr(1);
    u32 buffer_count = static_cast<u32>(m.arg(2));
    GuestAddr bytes_recvd_ptr = m.arg_ptr(3);
    GuestAddr flags_ptr = m.arg_ptr(4);
    GuestAddr overlapped = m.arg_ptr(5);
    GuestAddr completion_routine = m.arg_ptr(6);
    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        VITREO_TRACE("[ws2_32] WSARecv(socket=0x%x) -> WSAENOTSOCK\n", guest_socket);
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    u32 flags = 0;
    if (flags_ptr) {
        auto f = m.read_u32(flags_ptr);
        if (f) flags = f.value();
    }
    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    const bool datagram =
        (host_type == SOCK_DGRAM || host_type == SOCK_SEQPACKET);
    const int fl = ::fcntl(native_fd, F_GETFL, 0);
    const bool nonblocking = fl >= 0 && (fl & O_NONBLOCK) != 0;
    int recv_flags = 0;
    if (const u32 ferr = check_recv_flags(static_cast<int>(flags), datagram,
                                          nonblocking, &recv_flags)) {
        mgr.set_last_error(ferr);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    // Darwin reports an oversize datagram with MSG_TRUNC in msg_flags on its
    // own. MSG_TRUNC must NOT be passed as an input flag: Darwin copies the
    // input flags into msg_flags, so every datagram then looked truncated and
    // every datagram WSARecv failed WSAEMSGSIZE.

    // WSABUF layout: { ULONG len; CHAR* buf; }
    //   x86: 8 bytes — len@0 (u32), buf@4 (u32)
    //   x64: 16 bytes — len@0 (u32), 4B pad, buf@8 (u64)
    const bool is64 = m.cpu_is_64bit();
    const u32 wsabuf_stride = is64 ? 16u : 8u;
    const u32 wsabuf_ptr_off = is64 ? 8u : 4u;
    auto read_wsabuf_ptr = [&](GuestAddr base) -> GuestAddr {
        if (is64) {
            auto r = m.read_u64(base + wsabuf_ptr_off);
            return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
        }
        auto r = m.read_u32(base + wsabuf_ptr_off);
        return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
    };

    std::vector<std::pair<GuestAddr, u32>> guest_bufs;
    guest_bufs.reserve(buffer_count);
    std::vector<struct iovec> iovecs(buffer_count);
    std::vector<std::vector<char>> host_bufs(buffer_count);
    size_t total_len = 0;

    for (u32 i = 0; i < buffer_count; ++i) {
        GuestAddr wsabuf_addr = buffers_ptr + i * wsabuf_stride;
        auto len_r = m.read_u32(wsabuf_addr);
        if (!len_r) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        u32 buf_len = len_r.value();
        GuestAddr buf_ptr = read_wsabuf_ptr(wsabuf_addr);
        if (buf_len != 0 && buf_ptr == 0) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        guest_bufs.emplace_back(buf_ptr, buf_len);
        host_bufs[i].resize(buf_len);
        iovecs[i].iov_base = host_bufs[i].data();
        iovecs[i].iov_len = buf_len;
        total_len += buf_len;
    }

    // Overlapped path: callers associate SOCKETs with an IOCP and wait on
    // GetQueuedCompletionStatus. Returning WSAEWOULDBLOCK without posting a
    // packet stalls TLS after ClientHello / second flight.
    if (overlapped != 0) {
        bool immediate = false;
        if (!mgr.begin_overlapped_recv(
                guest_socket, native_fd, std::move(guest_bufs), recv_flags,
                overlapped, is64, bytes_recvd_ptr, m, 0, 0, 0, 0, 0, 0,
                completion_routine, issuing_tid, &immediate)) {
            mgr.reenable_fd_read(guest_socket, native_fd);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        mgr.reenable_fd_read(guest_socket, native_fd);
        if (immediate) {
            m.set_return(0);
            return;
        }
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    VITREO_TRACE("[ws2_32] WSARecv(socket=0x%x, bufs=%u, total=%zu, flags=0x%x)\n",
                 guest_socket, buffer_count, total_len, flags);

    struct msghdr msg = {};
    msg.msg_iov = iovecs.data();
    msg.msg_iovlen = static_cast<int>(buffer_count);

    ssize_t rc = ::recvmsg(native_fd, &msg, recv_flags);
    const int recv_errno = errno;
    // Learn WSAEventSelect: WSARecv reenables FD_READ/FD_OOB, even when it
    // fails; still-buffered data records FD_READ again.
    mgr.reenable_fd_read(guest_socket, native_fd);
    errno = recv_errno;
    if (rc <= 0) {
        diagnose_recv(guest_socket, rc, errno);
    }
    if (rc < 0) {
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, false);
        VITREO_TRACE("[ws2_32] WSARecv -> error %u (errno=%d)\n", wsa_err, errno);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    ssize_t remaining = rc;
    for (u32 i = 0; i < buffer_count && remaining > 0; ++i) {
        GuestAddr buf_ptr = guest_bufs[i].first;
        u32 buf_len = guest_bufs[i].second;
        if (!buf_ptr) break;
        size_t to_copy = static_cast<size_t>(
            std::min(static_cast<ssize_t>(buf_len), remaining));
        if (to_copy > 0) {
            m.memory().write_bytes(
                buf_ptr,
                reinterpret_cast<const Byte*>(host_bufs[i].data()), to_copy);
        }
        remaining -= static_cast<ssize_t>(to_copy);
    }

    if (bytes_recvd_ptr) {
        m.write_u32(bytes_recvd_ptr, static_cast<u32>(rc));
    }

    // Learn WSARecv (message-oriented): fill buffers, set MSG_PARTIAL in
    // *lpFlags, return SOCKET_ERROR / WSAEMSGSIZE when the datagram was
    // truncated.
    constexpr u32 kMsgPartial = 0x8000u;
    const bool truncated =
        datagram && ((msg.msg_flags & MSG_TRUNC) != 0 || total_len == 0);
    if (flags_ptr) {
        u32 out_flags = flags & ~(kMsgPartial);
        if (truncated) out_flags |= kMsgPartial;
        (void)m.write_u32(flags_ptr, out_flags);
    }
    if (truncated) {
        mgr.set_last_error(WSAEMSGSIZE);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    VITREO_TRACE("[ws2_32] WSARecv -> %zd bytes\n", rc);
    m.set_return(0); // success
}

void ws2::impl_WSARecvFrom(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    //             LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    //             struct sockaddr* lpFrom, LPINT lpFromlen,
    //             LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
    u32 guest_socket = static_cast<u32>(m.arg(0));
    const FdReadReenable reenable_fd_read_on_return(mgr, guest_socket);
    GuestAddr buffers_ptr = m.arg_ptr(1);
    u32 buffer_count = static_cast<u32>(m.arg(2));
    GuestAddr bytes_recvd_ptr = m.arg_ptr(3);
    GuestAddr flags_ptr = m.arg_ptr(4);
    GuestAddr from_ptr = m.arg_ptr(5);
    GuestAddr fromlen_ptr = m.arg_ptr(6);
    GuestAddr overlapped = m.arg_ptr(7);
    GuestAddr completion_routine = m.arg_ptr(8);
    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    int host_type = 0;
    socklen_t host_type_len = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &host_type_len);
    const bool datagram = host_type != SOCK_STREAM;
    // A datagram receive checks lpFromlen before taking anything off the
    // queue: too small for the socket's address is WSAEFAULT with the
    // datagram left in place (ws2_ovres_probe recvfrom_short_fromlen /
    // recvfrom_short_fromlen_left).
    if (datagram && from_ptr != 0) {
        sockaddr_storage local{};
        socklen_t ll = sizeof(local);
        const bool v6 = ::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local), &ll) == 0 &&
                        local.ss_family == AF_INET6;
        bool cap_ok = false;
        i32 cap = 0;
        if (fromlen_ptr != 0) {
            const auto c = m.read_u32(fromlen_ptr);
            cap_ok = c.is_ok();
            if (cap_ok) cap = static_cast<i32>(c.value());
        }
        if (!cap_ok || cap < (v6 ? kWinSockaddrIn6Bytes : 16)) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    // Read flags
    u32 recv_flags = 0;
    if (flags_ptr) {
        auto fr = m.read_u32(flags_ptr);
        if (fr.is_ok()) recv_flags = fr.value();
    }
    int native_flags = 0;
    if (recv_flags & 0x1) native_flags |= MSG_OOB;
    if (recv_flags & 0x2) native_flags |= MSG_PEEK;

    // WSABUF layout differs by bitness (see WSARecv).
    const bool is64 = m.cpu_is_64bit();
    const u32 wsabuf_stride = is64 ? 16u : 8u;
    const u32 wsabuf_ptr_off = is64 ? 8u : 4u;
    auto read_wsabuf_ptr = [&](GuestAddr base) -> GuestAddr {
        if (is64) {
            auto r = m.read_u64(base + wsabuf_ptr_off);
            return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
        } else {
            auto r = m.read_u32(base + wsabuf_ptr_off);
            return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
        }
    };
    std::vector<struct iovec> iovecs(buffer_count);
    std::vector<std::vector<char>> host_bufs(buffer_count);
    size_t total_len = 0;

    for (u32 i = 0; i < buffer_count; ++i) {
        GuestAddr wsabuf_addr = buffers_ptr + i * wsabuf_stride;
        auto len_r = m.read_u32(wsabuf_addr);
        GuestAddr buf_ptr_chk = read_wsabuf_ptr(wsabuf_addr);
        if (!len_r || !buf_ptr_chk) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        u32 buf_len = len_r.value();
        host_bufs[i].resize(buf_len);
        iovecs[i].iov_base = host_bufs[i].data();
        iovecs[i].iov_len = buf_len;
        total_len += buf_len;
    }

    // Overlapped path. WSARecvFrom takes lpOverlapped as its EIGHTH argument
    // (WSARecv's is the sixth), and it used to be read by nobody: every
    // overlapped datagram receive fell through to a synchronous recvmsg on a
    // non-blocking socket, returned WSAEWOULDBLOCK, and left the caller
    // waiting on an event no one would ever signal. Measured as a 1:1
    // WSARecvFrom/WSAGetLastError spin — 214,807,109 pairs plus 366,353,493
    // WaitForSingleObject calls — while the connection never established.
    // Same shape as the recorded WSARecv fix.
    if (overlapped != 0) {
        std::vector<std::pair<GuestAddr, u32>> guest_bufs;
        guest_bufs.reserve(buffer_count);
        for (u32 i = 0; i < buffer_count; ++i) {
            const GuestAddr wsabuf_addr = buffers_ptr + i * wsabuf_stride;
            const auto len_r = m.read_u32(wsabuf_addr);
            const GuestAddr buf_ptr = read_wsabuf_ptr(wsabuf_addr);
            if (!len_r || !buf_ptr) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            guest_bufs.emplace_back(buf_ptr, len_r.value());
        }
        bool immediate = false;
        if (!mgr.begin_overlapped_recv(
                guest_socket, native_fd, std::move(guest_bufs), native_flags,
                overlapped, is64, bytes_recvd_ptr, m, from_ptr, fromlen_ptr,
                0, 0, 0, 0, completion_routine, issuing_tid, &immediate)) {
            mgr.reenable_fd_read(guest_socket, native_fd);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        mgr.reenable_fd_read(guest_socket, native_fd);
        m.set_return(immediate ? 0 : static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    VITREO_TRACE("[ws2_32] WSARecvFrom(socket=0x%x, bufs=%u, total=%zu)\n",
                 guest_socket, buffer_count, total_len);

    struct sockaddr_storage ss{};
    struct msghdr msg = {};
    msg.msg_iov = iovecs.data();
    msg.msg_iovlen = static_cast<int>(buffer_count);
    if (from_ptr) {
        msg.msg_name = &ss;
        msg.msg_namelen = sizeof(ss);
    }

    ssize_t rc = ::recvmsg(native_fd, &msg, native_flags);
    const int recv_errno = errno;
    // Learn WSAEventSelect: WSARecvFrom reenables FD_READ/FD_OOB.
    mgr.reenable_fd_read(guest_socket, native_fd);
    errno = recv_errno;
    if (rc < 0) {
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, false);
        VITREO_TRACE("[ws2_32] WSARecvFrom -> error %u\n", wsa_err);
        wsa_udp_census("WSARecvFrom", -1, wsa_err);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // Write received data to guest buffers
    ssize_t remaining = rc;
    for (u32 i = 0; i < buffer_count && remaining > 0; ++i) {
        GuestAddr wsabuf_addr = buffers_ptr + i * wsabuf_stride;
        auto len_r = m.read_u32(wsabuf_addr);
        GuestAddr buf_ptr = read_wsabuf_ptr(wsabuf_addr);
        if (!len_r || !buf_ptr) break;
        u32 buf_len = len_r.value();
        size_t to_copy = static_cast<size_t>(std::min(static_cast<ssize_t>(buf_len), remaining));
        if (to_copy > 0) {
            m.memory().write_bytes(buf_ptr,
                reinterpret_cast<const Byte*>(host_bufs[i].data()), to_copy);
        }
        remaining -= static_cast<ssize_t>(to_copy);
    }

    // Write source address (Windows sockaddr layout)
    if (from_ptr && fromlen_ptr) {
        u32 guest_fromlen = 0;
        auto nr = m.read_u32(fromlen_ptr);
        if (nr.is_ok()) guest_fromlen = nr.value();
        if (guest_fromlen > 0) {
            u32 written = write_win_sockaddr(m, from_ptr, guest_fromlen, ss);
            if (written != 0) {
                (void)m.write_u32(fromlen_ptr, written);
            }
        }
    }

    if (bytes_recvd_ptr) {
        m.write_u32(bytes_recvd_ptr, static_cast<u32>(rc));
    }

    VITREO_TRACE("[ws2_32] WSARecvFrom -> %zd bytes\n", rc);
    note_udp_peer(false, reinterpret_cast<const struct sockaddr*>(&ss));
    wsa_udp_census("WSARecvFrom", rc, 0);
    // A datagram longer than the buffers delivers its first part and fails
    // with WSAEMSGSIZE (ws2_ovres_probe recvfrom_trunc_sync: got=4 err=10040).
    if (datagram && (msg.msg_flags & MSG_TRUNC) != 0) {
        mgr.set_last_error(WSAEMSGSIZE);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    m.set_return(0);
}

void ws2::impl_WSASend(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    //         LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
    //         LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE)
    // → 0 on success, SOCKET_ERROR on failure / pending
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr buffers_ptr = m.arg_ptr(1);
    u32 buffer_count = static_cast<u32>(m.arg(2));
    GuestAddr bytes_sent_ptr = m.arg_ptr(3);
    u32 flags = static_cast<u32>(m.arg(4));
    GuestAddr overlapped = m.arg_ptr(5);
    GuestAddr completion_routine = m.arg_ptr(6);
    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        VITREO_TRACE("[ws2_32] WSASend(socket=0x%x) -> WSAENOTSOCK\n", guest_socket);
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    int send_flags = 0;
    if (const u32 ferr = check_send_flags(static_cast<int>(flags), &send_flags)) {
        mgr.set_last_error(ferr);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    const bool is64 = m.cpu_is_64bit();
    const u32 wsabuf_stride = is64 ? 16u : 8u;
    const u32 wsabuf_ptr_off = is64 ? 8u : 4u;
    // OVERLAPPED.hEvent: x86 @16, x64 @24; InternalHigh: x86 @4, x64 @8
    const u32 ov_event_off = is64 ? 24u : 16u;
    const u32 ov_high_off = is64 ? 8u : 4u;
    auto read_wsabuf_ptr = [&](GuestAddr base) -> GuestAddr {
        if (is64) {
            auto r = m.read_u64(base + wsabuf_ptr_off);
            return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
        }
        auto r = m.read_u32(base + wsabuf_ptr_off);
        return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
    };
    std::vector<struct iovec> iovecs(buffer_count);
    std::vector<std::vector<char>> host_bufs(buffer_count);
    size_t total_len = 0;

    for (u32 i = 0; i < buffer_count; ++i) {
        GuestAddr wsabuf_addr = buffers_ptr + i * wsabuf_stride;
        auto len_r = m.read_u32(wsabuf_addr);
        GuestAddr buf_ptr = read_wsabuf_ptr(wsabuf_addr);
        if (!len_r || !buf_ptr) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        u32 buf_len = len_r.value();
        host_bufs[i].resize(buf_len);
        auto res = m.memory().read_bytes(
            buf_ptr, reinterpret_cast<Byte*>(host_bufs[i].data()), buf_len);
        if (res.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        iovecs[i].iov_base = host_bufs[i].data();
        iovecs[i].iov_len = buf_len;
        total_len += buf_len;
    }

    VITREO_TRACE(
        "[ws2_32] WSASend(socket=0x%x, bufs=%u, total=%zu, flags=0x%x ov=0x%llx)\n",
        guest_socket, buffer_count, total_len, flags,
        static_cast<unsigned long long>(overlapped));

    // The terminal-flight ring is the only view into what the guest executed
    // just before it gave up on a handshake, and it is useless if turning it on
    // also turns on whole-process tracing (which changes the timing it is
    // measuring). Give it its own switch.
    static const bool s_tls_ring =
        std::getenv("VITREO_TLS_RING") != nullptr;
    if (s_tls_ring && !host_bufs.empty()) {
        const auto& first = host_bufs.front();
        const bool terminal = first.size() >= 11 &&
            static_cast<unsigned char>(first[0]) == 0x14 &&
            static_cast<unsigned char>(first[1]) == 0x03 &&
            static_cast<unsigned char>(first[3]) == 0x00 &&
            static_cast<unsigned char>(first[4]) == 0x01 &&
            static_cast<unsigned char>(first[5]) == 0x01 &&
            static_cast<unsigned char>(first[6]) == 0x17;
        if (terminal && t_thread_trace.active) {
            constexpr usize kRingBlocks = 4096;
            const usize total = t_thread_trace.idx;
            const usize count = std::min(total, kRingBlocks);
            std::fprintf(stderr,
                "[tls-ring] terminal flight; %zu blocks executed in this "
                "callback, dumping %zu:\n", total, count);
            for (usize i = total - count; i < total; ++i) {
                std::fprintf(stderr, "[tls-ring] %zu 0x%08x\n", i,
                    static_cast<unsigned>(
                        t_thread_trace.rips[i % THREAD_TRACE_SIZE]));
            }
            std::fprintf(stderr, "[tls-ring] end\n");
        }
    }
    if (ws2_trace_enabled()) {
        constexpr size_t kDumpLimit = 128;
        char hex[kDumpLimit * 3 + 1] = {};
        size_t dumped = 0;
        for (const auto& buffer : host_bufs) {
            for (char byte : buffer) {
                if (dumped == kDumpLimit) break;
                std::snprintf(hex + dumped * 3, 4, "%02x ",
                              static_cast<unsigned char>(byte));
                ++dumped;
            }
            if (dumped == kDumpLimit) break;
        }
        VITREO_TRACE("[ws2_32] WSASend data: %s\n", hex);

        const auto& first = host_bufs.front();
        const bool tls13_terminal_flight = first.size() >= 11 &&
            static_cast<unsigned char>(first[0]) == 0x14 &&
            static_cast<unsigned char>(first[1]) == 0x03 &&
            static_cast<unsigned char>(first[3]) == 0x00 &&
            static_cast<unsigned char>(first[4]) == 0x01 &&
            static_cast<unsigned char>(first[5]) == 0x01 &&
            static_cast<unsigned char>(first[6]) == 0x17;
        if (tls13_terminal_flight && t_thread_trace.active) {
            constexpr usize kTerminalTraceBlocks = 4096;
            const usize total = t_thread_trace.idx;
            const usize count = std::min(total, kTerminalTraceBlocks);
            const usize start = total - count;
            VITREO_TRACE(
                "[ws2_32] WSASend TLS terminal thread trace ring (%zu blocks):\n",
                count);
            for (usize i = start; i < total; ++i) {
                VITREO_TRACE("  [%zu] 0x%08x\n", i,
                    static_cast<unsigned>(
                        t_thread_trace.rips[i % THREAD_TRACE_SIZE]));
            }
            VITREO_TRACE(
                "[ws2_32] End WSASend TLS terminal thread trace ring\n");
        }
    }

    struct msghdr msg = {};
    msg.msg_iov = iovecs.data();
    msg.msg_iovlen = static_cast<int>(buffer_count);

    ssize_t rc = ::sendmsg(native_fd, &msg, send_flags);
    if (rc < 0) {
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, true);
        VITREO_TRACE("[ws2_32] WSASend -> error %u (errno=%d)\n", wsa_err, errno);
        if (wsa_err == WSAEWOULDBLOCK) {
            mgr.arm_fd_write(guest_socket);
            // Microsoft Learn, WSASend: an overlapped send that cannot finish
            // now returns WSA_IO_PENDING and completes later. WSAEWOULDBLOCK
            // is the NON-overlapped answer and tells the caller nothing was
            // queued, which an overlapped caller will not act on. The transfer
            // then stalls until the caller gives up with a connection timeout.
            if (overlapped != 0) {
                std::vector<char> queued;
                queued.reserve(total_len);
                for (u32 i = 0; i < buffer_count; ++i) {
                    const char* src =
                        static_cast<const char*>(iovecs[i].iov_base);
                    queued.insert(queued.end(), src,
                                  src + iovecs[i].iov_len);
                }
                if (mgr.begin_overlapped_send(
                        guest_socket, native_fd, std::move(queued), 0,
                        send_flags, overlapped, is64, bytes_sent_ptr, m,
                        completion_routine, issuing_tid)) {
                    m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                    return;
                }
            }
        }
        ipc::ipc_log("WSASend socket=0x%x -> SOCKET_ERROR wsa=%u",
                     guest_socket, wsa_err);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // Overlapped + short send on a non-blocking socket: queue the remainder
    // (Learn / already_sent path of begin_overlapped_send). A send that goes
    // pending leaves *lpNumberOfBytesSent alone (Learn WSASend: the count is
    // only updated when the operation completes immediately).
    if (overlapped != 0 && static_cast<size_t>(rc) < total_len) {
        std::vector<char> queued;
        queued.reserve(total_len - static_cast<size_t>(rc));
        size_t skip = static_cast<size_t>(rc);
        for (u32 i = 0; i < buffer_count; ++i) {
            const char* src = static_cast<const char*>(iovecs[i].iov_base);
            size_t len = iovecs[i].iov_len;
            if (skip >= len) {
                skip -= len;
                continue;
            }
            queued.insert(queued.end(), src + skip, src + len);
            skip = 0;
        }
        if (mgr.begin_overlapped_send(
                guest_socket, native_fd, std::move(queued),
                static_cast<u32>(rc), send_flags, overlapped, is64,
                bytes_sent_ptr, m, completion_routine, issuing_tid)) {
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }
    if (bytes_sent_ptr) {
        m.write_u32(bytes_sent_ptr, static_cast<u32>(rc));
    }

    // Sync success with OVERLAPPED: Learn signals hEvent only when
    // lpCompletionRoutine is NULL; otherwise the routine is queued as an APC
    // and hEvent is application context. Callers reset hEvent and depend on
    // the event path for IOCP-less overlapped sends.
    if (overlapped != 0) {
        if (completion_routine != 0) {
            mgr.notify_overlapped_completion(
                static_cast<Handle>(guest_socket), static_cast<u32>(rc),
                overlapped, is64, STATUS_SUCCESS, 0,
                /*completed_synchronously=*/true, completion_routine,
                issuing_tid, 0);
        } else {
            GuestMemory& mem = m.memory();
            if (is64) {
                (void)mem.write_u64(overlapped, STATUS_SUCCESS);
                (void)mem.write_u64(overlapped + ov_high_off,
                                    static_cast<u64>(rc));
            } else {
                (void)mem.write_u32(overlapped, STATUS_SUCCESS);
                (void)mem.write_u32(overlapped + ov_high_off,
                                    static_cast<u32>(rc));
            }
            Handle hevent = 0;
            if (is64) {
                hevent = static_cast<Handle>(
                    mem.read_u64(overlapped + ov_event_off).value_or(0));
            } else {
                hevent = static_cast<Handle>(
                    mem.read_u32(overlapped + ov_event_off).value_or(0));
            }
            if (hevent != 0 && mgr.sync_manager()) {
                (void)mgr.sync_manager()->set_event(hevent & ~Handle{1});
            }
            mgr.notify_overlapped_completion(
                static_cast<Handle>(guest_socket), static_cast<u32>(rc),
                overlapped, is64, STATUS_SUCCESS, 0,
                /*completed_synchronously=*/true);
        }
    }

    ipc::ipc_log("WSASend socket=0x%x -> %zd bytes%s",
                 guest_socket, rc, overlapped ? " (ov)" : "");
    if (rc > 0 && rc <= 512 && !host_bufs.empty() &&
        !host_bufs.front().empty() &&
        static_cast<unsigned char>(host_bufs.front().front()) < 0x80) {
        std::string preview;
        preview.reserve(static_cast<size_t>(rc));
        for (const auto& buffer : host_bufs) {
            preview.append(buffer.data(), buffer.size());
            if (preview.size() >= static_cast<size_t>(rc)) break;
        }
        preview.resize(static_cast<size_t>(rc));
        for (char& ch : preview) {
            if (ch == '\r' || ch == '\n') ch = '|';
            else if (static_cast<unsigned char>(ch) < 0x20) ch = '.';
        }
        ipc::ipc_log("WSASend socket=0x%x payload %s", guest_socket,
                     preview.c_str());
    }
    VITREO_TRACE("[ws2_32] WSASend -> %zd bytes\n", rc);
    m.set_return(0);
}

namespace {

/// An overlapped datagram send (WSASendTo / WSASendMsg) that the host
/// finished inline. With an event or IOCP the call returns 0 with the
/// OVERLAPPED already complete and hEvent already signalled; with a
/// completion routine it reports WSA_IO_PENDING and the routine runs on the
/// next alertable wait (ws2_ovres_probe sendto_ov / sendto_routine,
/// ws2_msg_probe ovi_sendmsg_event / ovi_sendmsg_routine).
void finish_inline_datagram_send(Marshaler& m, Ws2Manager& mgr, u32 guest_socket,
                                 GuestAddr overlapped, GuestAddr routine,
                                 u32 issuing_tid, GuestAddr bytes_ptr, u32 bytes,
                                 bool datagram = true) {
    const bool is64 = m.cpu_is_64bit();
    if (routine != 0 && !datagram) {
        // A stream send with a routine returns 0 (rt_send_immediate).
        if (bytes_ptr != 0) (void)m.write_u32(bytes_ptr, bytes);
        mgr.notify_overlapped_completion(static_cast<Handle>(guest_socket), bytes,
                                         overlapped, is64, STATUS_SUCCESS, 0,
                                         true, routine, issuing_tid);
        m.set_return(0);
        return;
    }
    if (routine != 0) {
        mgr.notify_overlapped_completion(static_cast<Handle>(guest_socket), bytes,
                                         overlapped, is64, STATUS_SUCCESS, 0,
                                         false, routine, issuing_tid);
        mgr.set_last_error(WSA_IO_PENDING);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (bytes_ptr != 0) (void)m.write_u32(bytes_ptr, bytes);
    mgr.notify_overlapped_completion(static_cast<Handle>(guest_socket), bytes,
                                     overlapped, is64, STATUS_SUCCESS, 0,
                                     /*completed_synchronously=*/true);
    m.set_return(0);
}

} // namespace

void ws2::impl_WSASendMsg(Marshaler& m, Ws2Manager& mgr) {
    // INT WSASendMsg(SOCKET Handle, LPWSAMSG lpMsg, DWORD dwFlags,
    //                LPDWORD lpNumberOfBytesSent, LPWSAOVERLAPPED lpOverlapped,
    //                LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
    //
    // Learn (WSASendMsg): datagram and raw sockets only; lpMsg->Control may
    // carry IN_PKTINFO / IN6_PKTINFO (source address) and, on current Windows,
    // IP_ECN / IPV6_ECN; lpNumberOfBytesSent may be NULL only with an
    // OVERLAPPED. What Learn leaves open is ws2_msg_probe-measured.
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const GuestAddr msg_ptr = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const GuestAddr bytes_sent_ptr = m.arg_ptr(3);
    const GuestAddr overlapped = m.arg_ptr(4);
    const GuestAddr completion_routine = m.arg_ptr(5);
    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };

    const int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) return fail(WSAENOTSOCK);          // send_bad_socket
    if (msg_ptr == 0) return fail(WSAEFAULT);             // v4_send_null_msg
    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    if (host_type == SOCK_STREAM) return fail(WSAEINVAL);  // send_tcp
    // MSG_OOB / MSG_DONTROUTE / MSG_PARTIAL are accepted on a datagram socket
    // (v4_send_flag_oob / _dontroute / _partial); anything else is
    // WSAEOPNOTSUPP (v4_send_flag_bogus). OOB has no datagram meaning.
    int native_flags = 0;
    if (const u32 ferr = check_send_flags(static_cast<int>(flags), &native_flags)) {
        return fail(ferr);
    }
    native_flags &= ~MSG_OOB;

    const bool is64 = m.cpu_is_64bit();
    const u32 name_len_off = is64 ? 8u : 4u;
    const u32 buffers_off = is64 ? 16u : 8u;
    const u32 count_off = is64 ? 24u : 12u;
    const u32 control_len_off = is64 ? 32u : 16u;
    const u32 control_ptr_off = is64 ? 40u : 20u;
    const u32 wsabuf_stride = is64 ? 16u : 8u;
    const u32 wsabuf_ptr_off = is64 ? 8u : 4u;

    const GuestAddr name_ptr = read_guest_pointer(m, msg_ptr, is64);
    const GuestAddr buffers_ptr =
        read_guest_pointer(m, msg_ptr + buffers_off, is64);
    const GuestAddr control_ptr =
        read_guest_pointer(m, msg_ptr + control_ptr_off, is64);
    const auto name_len_r = m.read_u32(msg_ptr + name_len_off);
    const auto count_r = m.read_u32(msg_ptr + count_off);
    const auto control_len_r = m.read_u32(msg_ptr + control_len_off);
    if (count_r.is_err() || name_len_r.is_err() || control_len_r.is_err() ||
        (count_r.value() != 0 && buffers_ptr == 0) || count_r.value() > 1024) {
        return fail(WSAEFAULT);
    }

    // Gather the scattered buffers into one contiguous payload. A datagram is
    // one packet regardless of how many WSABUFs describe it.
    std::vector<char> payload;
    for (u32 i = 0; i < count_r.value(); ++i) {
        const GuestAddr entry = buffers_ptr + i * wsabuf_stride;
        const auto len_r = m.read_u32(entry);
        const GuestAddr buf_ptr =
            read_guest_pointer(m, entry + wsabuf_ptr_off, is64);
        if (len_r.is_err() || (len_r.value() != 0 && buf_ptr == 0)) {
            return fail(WSAEFAULT);
        }
        const usize offset = payload.size();
        payload.resize(offset + len_r.value());
        if (len_r.value() != 0 &&
            m.memory().read_bytes(
                buf_ptr, reinterpret_cast<Byte*>(payload.data() + offset),
                len_r.value()).is_err()) {
            return fail(WSAEFAULT);
        }
    }

    // Destination: a short or unreadable name is WSAEFAULT (v4_send_shortname);
    // no name on an unconnected socket is WSAEINVAL (v4_send_noname_unconnected).
    MappedSockaddr mapped{};
    struct sockaddr* dest_addr = nullptr;
    socklen_t dest_len = 0;
    if (name_ptr != 0 && name_len_r.value() != 0) {
        if (!read_win_sockaddr_mapped(m, name_ptr,
                                      static_cast<int>(name_len_r.value()),
                                      mapped)) {
            return fail(WSAEFAULT);
        }
        dest_addr = reinterpret_cast<struct sockaddr*>(&mapped.ss);
        dest_len = mapped.len;
    } else {
        sockaddr_storage peer{};
        socklen_t pl = sizeof(peer);
        if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer), &pl) != 0) {
            return fail(WSAEINVAL);
        }
    }

    sockaddr_storage local{};
    socklen_t ll = sizeof(local);
    const int host_family =
        ::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local), &ll) == 0
            ? local.ss_family : AF_UNSPEC;
    Ws2SendControl control;
    if (const u32 cerr = ws2_translate_send_control(
            m.memory(), is64, control_ptr, control_len_r.value(), native_fd,
            host_family, dest_addr, &control)) {
        return fail(cerr);
    }

    // UDP_SEND_MSG_SIZE splits the payload into datagrams of that size
    // (ws2_msg_probe uso_recv: 10 bytes by 4 -> 4 4 2).
    const usize segment = control.segment != 0 ? control.segment : payload.size();
    usize sent_total = 0;
    usize offset = 0;
    do {
        const usize chunk = std::min(segment, payload.size() - offset);
        struct iovec iov {};
        iov.iov_base = payload.data() + offset;
        iov.iov_len = chunk;
        struct msghdr msg {};
        msg.msg_name = dest_addr;
        msg.msg_namelen = dest_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (!control.native.empty()) {
            msg.msg_control = control.native.data();
            msg.msg_controllen = static_cast<socklen_t>(control.native.size());
        }
        const ssize_t rc = ::sendmsg(native_fd, &msg, native_flags);
        if (rc < 0) {
            const u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, true);
            if (wsa_err == WSAEWOULDBLOCK) mgr.arm_fd_write(guest_socket);
            wsa_udp_census("WSASendMsg", -1, wsa_err);
            return fail(wsa_err);
        }
        sent_total += static_cast<usize>(rc);
        offset += chunk;
    } while (offset < payload.size());
    note_udp_peer(true, dest_addr);
    wsa_udp_census("WSASendMsg", static_cast<long long>(sent_total), 0);

    if (overlapped != 0) {
        finish_inline_datagram_send(m, mgr, guest_socket, overlapped,
                                    completion_routine, issuing_tid,
                                    bytes_sent_ptr, static_cast<u32>(sent_total));
        return;
    }
    // Without an OVERLAPPED the count pointer is required, but Windows has
    // already sent the datagram when it reports the fault
    // (v4_send_null_count / v4_send_null_count_arrived).
    if (bytes_sent_ptr == 0) return fail(WSAEFAULT);
    (void)m.write_u32(bytes_sent_ptr, static_cast<u32>(sent_total));
    m.set_return(0);
}

void ws2::impl_WSASendTo(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    //           LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
    //           const struct sockaddr* lpTo, int iTolen,
    //           LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
    u32 guest_socket = ws2_socket_arg(m, 0);
    GuestAddr buffers_ptr = m.arg_ptr(1);
    u32 buffer_count = m.arg32(2);
    GuestAddr bytes_sent_ptr = m.arg_ptr(3);
    u32 flags = m.arg32(4);
    GuestAddr to_ptr = m.arg_ptr(5);
    int tolen = static_cast<int>(m.arg32(6));
    const GuestAddr overlapped = m.arg_ptr(7);
    const GuestAddr completion_routine = m.arg_ptr(8);
    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    int host_type = 0;
    socklen_t host_type_len = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &host_type_len);
    const bool datagram = host_type != SOCK_STREAM;

    // Unknown flags, and MSG_OOB on a datagram socket, are WSAEOPNOTSUPP
    // (ws2_ovres_probe sendto_flag_bogus / sendto_flag_oob).
    int send_flags = 0;
    if (const u32 ferr = check_send_flags(static_cast<int>(flags), &send_flags)) {
        mgr.set_last_error(ferr);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (datagram && (flags & 0x1u) != 0) {
        mgr.set_last_error(WSAEOPNOTSUPP);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // WSABUF layout differs by bitness (see WSARecv).
    const bool is64 = m.cpu_is_64bit();
    const u32 wsabuf_stride = is64 ? 16u : 8u;
    const u32 wsabuf_ptr_off = is64 ? 8u : 4u;
    auto read_wsabuf_ptr = [&](GuestAddr base) -> GuestAddr {
        if (is64) {
            auto r = m.read_u64(base + wsabuf_ptr_off);
            return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
        } else {
            auto r = m.read_u32(base + wsabuf_ptr_off);
            return r.is_ok() ? static_cast<GuestAddr>(r.value()) : 0;
        }
    };
    std::vector<struct iovec> iovecs(buffer_count);
    std::vector<std::vector<char>> host_bufs(buffer_count);
    size_t total_len = 0;

    for (u32 i = 0; i < buffer_count; ++i) {
        GuestAddr wsabuf_addr = buffers_ptr + i * wsabuf_stride;
        auto len_r = m.read_u32(wsabuf_addr);
        GuestAddr buf_ptr = read_wsabuf_ptr(wsabuf_addr);
        if (!len_r || !buf_ptr) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        u32 buf_len = len_r.value();
        host_bufs[i].resize(buf_len);
        auto res = m.memory().read_bytes(buf_ptr,
            reinterpret_cast<Byte*>(host_bufs[i].data()), buf_len);
        if (res.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        iovecs[i].iov_base = host_bufs[i].data();
        iovecs[i].iov_len = buf_len;
        total_len += buf_len;
    }

    VITREO_TRACE("[ws2_32] WSASendTo(socket=0x%x, bufs=%u, total=%zu)\n",
                 guest_socket, buffer_count, total_len);

    MappedSockaddr mapped{};
    struct msghdr msg = {};
    msg.msg_iov = iovecs.data();
    msg.msg_iovlen = static_cast<int>(buffer_count);
    // A stream socket ignores lpTo (Learn WSASendTo). On a datagram socket a
    // short or unreadable address is WSAEFAULT (sendto_shortname); no address
    // on an unconnected socket is left to the host, which reports ENOTCONN ->
    // WSAENOTCONN (sendto_noname).
    if (datagram && to_ptr && tolen > 0) {
        if (!read_win_sockaddr_mapped(m, to_ptr, tolen, mapped)) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        msg.msg_name = &mapped.ss;
        msg.msg_namelen = mapped.len;
    }
    if (datagram && msg.msg_name == nullptr) {
        sockaddr_storage peer{};
        socklen_t pl = sizeof(peer);
        if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer), &pl) != 0) {
            mgr.set_last_error(WSAENOTCONN);  // sendto_noname
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    ssize_t rc = ::sendmsg(native_fd, &msg, send_flags);
    if (rc < 0) {
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, true);
        VITREO_TRACE("[ws2_32] WSASendTo -> error %u\n", wsa_err);
        wsa_udp_census("WSASendTo", -1, wsa_err);
        if (wsa_err == WSAEWOULDBLOCK) {
            mgr.arm_fd_write(guest_socket);
        }
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    VITREO_TRACE("[ws2_32] WSASendTo -> %zd bytes\n", rc);
    note_udp_peer(true, static_cast<const struct sockaddr*>(msg.msg_name));
    wsa_udp_census("WSASendTo", rc, 0);

    if (overlapped != 0) {
        // Learn WSASendTo: an overlapped send that completes immediately still
        // completes the OVERLAPPED (hEvent / IOCP / completion routine).
        finish_inline_datagram_send(m, mgr, guest_socket, overlapped,
                                    completion_routine, issuing_tid,
                                    bytes_sent_ptr, static_cast<u32>(rc), datagram);
        return;
    }
    // Non-overlapped: the count pointer is required, and Windows reports the
    // fault after the datagram has gone (sendto_nullcount_sync, udp_drained).
    if (bytes_sent_ptr == 0) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    m.write_u32(bytes_sent_ptr, static_cast<u32>(rc));
    m.set_return(0);
}

} // namespace vitreo::winapi
