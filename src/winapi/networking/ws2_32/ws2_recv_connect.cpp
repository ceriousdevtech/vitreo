// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_recv_connect.cpp
/// @brief recv / connect thunks
///
/// Closed method cuts from ws2_32.cpp.

#include "winapi/networking/ws2_32/ws2_32.hpp"
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

[[nodiscard]] bool ws2_connect_diag_enabled() {
    static const bool enabled =
        std::getenv("VITREO_WS2_CONNECT_DIAG") != nullptr;
    return enabled;
}

void ws2_connect_diag(const char* address, u16 port, int native_result,
                      int native_error, u32 windows_error,
                      bool event_selected) {
    if (!ws2_connect_diag_enabled()) return;
    static std::atomic<unsigned> lines{0};
    const unsigned line = lines.fetch_add(1, std::memory_order_relaxed);
    if (line >= 256) return;
    std::fprintf(stderr,
                 "[ws2-connect] target=%s:%u rc=%d errno=%d wsa=%u "
                 "event_selected=%u\n",
                 address, static_cast<unsigned>(port), native_result,
                 native_error, windows_error, event_selected ? 1u : 0u);
}

} // namespace

void ws2::impl_connect(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    s_ws2_io_gen.fetch_add(1, std::memory_order_release);
    // connect(socket, addr, addrlen)
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr addr_ptr = m.arg_ptr(1);
    int addrlen = static_cast<int>(m.arg(2));
    u32 guest_tid = ThreadManager::get_current_thread_id_static();
    
    VITREO_TRACE( "[ws2_32] connect(tid=%u, socket=0x%x, addrlen=%d, caller_ret=0x%llx)\n",
                  guest_tid, guest_socket, addrlen,
                  (unsigned long long)guest_caller_ret(m));
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        VITREO_TRACE( "[ws2_32] connect: invalid socket -> WSAENOTSOCK\n");
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    // Address validation, measured (ws2_core_probe connect_short_namelen /
    // connect_bad_family / connect_on_listener / udp_connect_unspec,
    // ws2_corex_probe tcp*_connect_*): a name shorter than the socket's own
    // sockaddr is WSAEFAULT (an AF_INET6 socket given a 16-byte sockaddr_in
    // included); a family other than the socket's is WSAEAFNOSUPPORT
    // (AF_UNSPEC included -- it is not a disconnect on Windows); a listening
    // socket is WSAEINVAL.
    int sock_family = AF_INET;
    {
        sockaddr_storage local{};
        socklen_t ll = sizeof(local);
        if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local), &ll) == 0) {
            sock_family = local.ss_family;
        }
    }
    const int need_len = sock_family == AF_INET6 ? kWinSockaddrIn6Bytes
                         : sock_family == AF_UNIX ? 2
                                                  : 16;
    const u16 want_family = sock_family == AF_INET6 ? kWinAfInet6
                            : sock_family == AF_UNIX ? kWinAfUnix
                                                     : kWinAfInet;
    if (addr_ptr == 0 || addrlen < need_len) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    {
        const auto fam = m.read_u16(addr_ptr);
        if (fam.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        if (fam.value() != want_family) {
            mgr.set_last_error(WSAEAFNOSUPPORT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }
    if (mgr.is_listening(guest_socket)) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // Read Windows sockaddr_in / sockaddr_in6 from guest and convert to host.
    MappedSockaddr mapped{};
    if (!read_win_sockaddr_mapped(m, addr_ptr, addrlen, mapped)) {
        ipc::ipc_log("connect socket=0x%x addrlen=%d ptr=0x%llx -> WSAEFAULT caller=0x%llx",
                     guest_socket, addrlen,
                     (unsigned long long)addr_ptr,
                     (unsigned long long)guest_caller_ret(m));
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    // Log target address for debugging
    char addr_str[INET6_ADDRSTRLEN] = {};
    u16 port = 0;
    format_mapped_sockaddr(mapped, addr_str, sizeof(addr_str), &port);
    VITREO_TRACE("[ws2_32] connect: target=%s:%u\n",
                 addr_str, static_cast<unsigned>(port));
    // Always-on so a bring-up log shows whether a TCP connect follows
    // WSAStartup (VITREO_TRACE is usually off).
    ipc::ipc_log("connect socket=0x%x -> %s:%u tid=%u",
                 guest_socket, addr_str, static_cast<unsigned>(port), guest_tid);

    // Learn / ws2_core_probe: TCP INADDR_ANY or port 0 -> WSAEADDRNOTAVAIL;
    // UDP all-zero disconnects (AF_UNSPEC); UDP INADDR_ANY+port -> ADDRNOTAVAIL.
    {
        bool addr_any = false;
        if (mapped.ss.ss_family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(&mapped.ss);
            addr_any = (sin->sin_addr.s_addr == 0);
        } else if (mapped.ss.ss_family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&mapped.ss);
            const auto* b = reinterpret_cast<const u8*>(&sin6->sin6_addr);
            addr_any = true;
            for (int i = 0; i < 16; ++i) {
                if (b[i] != 0) { addr_any = false; break; }
            }
        }
        int host_type = 0;
        socklen_t tl = sizeof(host_type);
        (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
        if (host_type == SOCK_STREAM) {
            if (addr_any || port == 0) {
                mgr.set_last_error(WSAEADDRNOTAVAIL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        } else if (host_type == SOCK_DGRAM) {
            if (addr_any && port == 0) {
                // Learn connect: an all-zero address dissolves a datagram
                // association (udp_connect_zero / udp_getpeername_after_zero).
                // An unconnected socket gets WSAEADDRNOTAVAIL, after the
                // implicit bind (ws2_corex_probe udp4_connect_zero_unconnected
                // / udp4_getsockname_after_zero).
                sockaddr_storage peer{};
                socklen_t peer_len = sizeof(peer);
                if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer),
                                  &peer_len) != 0) {
                    sockaddr_storage local{};
                    socklen_t ll = sizeof(local);
                    if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local),
                                      &ll) == 0) {
                        u16 lport = 0;
                        if (local.ss_family == AF_INET) {
                            lport = reinterpret_cast<sockaddr_in*>(&local)->sin_port;
                        } else if (local.ss_family == AF_INET6) {
                            lport = reinterpret_cast<sockaddr_in6*>(&local)->sin6_port;
                        }
                        if (lport == 0) {
                            // mapped already holds the family's wildcard:0.
                            (void)::bind(native_fd,
                                         reinterpret_cast<sockaddr*>(&mapped.ss),
                                         mapped.len);
                        }
                    }
                    mgr.set_last_error(WSAEADDRNOTAVAIL);
                    m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                    return;
                }
                // Darwin dissolves on connect(AF_UNSPEC) with a full-length
                // sockaddr and reports EAFNOSUPPORT while doing so.
                sockaddr_in unspec{};
                unspec.sin_len = static_cast<u8>(sizeof(unspec));
                unspec.sin_family = AF_UNSPEC;
                (void)::connect(native_fd, reinterpret_cast<sockaddr*>(&unspec),
                                sizeof(unspec));
                m.set_return(0);
                return;
            }
            if (addr_any) {
                mgr.set_last_error(WSAEADDRNOTAVAIL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        }
    }

    // VITREO_TLS_REDIRECT_PORT=<port>: redirect all port-443 connects to
    // 127.0.0.1:<port>.  Used by the tls_intercept_server diagnostic tool.
    {
        const char* redir = std::getenv("VITREO_TLS_REDIRECT_PORT");
        if (redir && port == 443 && mapped.ss.ss_family == AF_INET) {
            int redir_port = std::atoi(redir);
            if (redir_port > 0 && redir_port <= 65535) {
                auto* sin = reinterpret_cast<sockaddr_in*>(&mapped.ss);
                char orig_ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &sin->sin_addr, orig_ip, sizeof(orig_ip));
                std::fprintf(stderr,
                    "[ws2_32] TLS-intercept: redirecting %s:443 -> 127.0.0.1:%d "
                    "(socket 0x%x)\n",
                    orig_ip, redir_port, guest_socket);
                sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sin->sin_port        = htons(static_cast<uint16_t>(redir_port));
                port = static_cast<u16>(redir_port);
            }
        }
    }

    int host_type_for_state = 0;
    bool was_unbound = false;
    {
        socklen_t tl = sizeof(host_type_for_state);
        (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type_for_state, &tl);
        sockaddr_storage local{};
        socklen_t ll = sizeof(local);
        if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local), &ll) == 0) {
            if (local.ss_family == AF_INET) {
                was_unbound = reinterpret_cast<sockaddr_in*>(&local)->sin_port == 0;
            } else if (local.ss_family == AF_INET6) {
                was_unbound = reinterpret_cast<sockaddr_in6*>(&local)->sin6_port == 0;
            }
        }
    }
    int rc = ::connect(native_fd, reinterpret_cast<sockaddr*>(&mapped.ss),
                       mapped.len);
    int connect_errno = errno;
    bool event_selected_connect = false;

    // Nonblocking connect contract (Learn connect): return SOCKET_ERROR /
    // WSAEWOULDBLOCK immediately; completion is observed via select /
    // WSAEventSelect / getsockopt(SO_ERROR). Optional VITREO_CONNECT_WAIT_MS
    // may still poll for diagnostics (0 = Windows behaviour, the default).
    if (rc < 0 && connect_errno == EINPROGRESS) {
        constexpr u32 kFdConnect = 0x10;
        const auto assoc = mgr.get_event_assoc(guest_socket);
        const bool awaits_fd_connect =
            assoc.has_value() && (assoc->event_mask & kFdConnect) != 0;
        event_selected_connect = awaits_fd_connect;

        const u64 wait_ms = env_u64_or("VITREO_CONNECT_WAIT_MS", 0);
        // Its completion records FD_CONNECT and, on success, FD_WRITE for
        // whatever is selected then (Learn WSAEventSelect).
        if (host_type_for_state == SOCK_STREAM) {
            mgr.mark_pending_connect(guest_socket);
        }
        if (awaits_fd_connect) {
            ipc::ipc_log(
                "connect %s:%u -> defer WSAEWOULDBLOCK (WSAEventSelect "
                "FD_CONNECT event=0x%x)",
                addr_str, static_cast<unsigned>(port),
                assoc->event_handle);
        }
        if (wait_ms > 0) {
            struct pollfd pfd{};
            pfd.fd = native_fd;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, static_cast<int>(wait_ms));
            if (pr > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
                int sockerr = 0;
                socklen_t len = sizeof(sockerr);
                if (::getsockopt(native_fd, SOL_SOCKET, SO_ERROR,
                                 &sockerr, &len) == 0) {
                    if (sockerr == 0 && !(pfd.revents & (POLLERR | POLLHUP))) {
                        VITREO_TRACE("[ws2_32] connect: completed via "
                                     "poll-wait (%llu ms budget)\n",
                                     (unsigned long long)wait_ms);
                        rc = 0;
                        connect_errno = 0;
                    } else {
                        if (sockerr == 0) sockerr = ECONNREFUSED;
                        VITREO_TRACE("[ws2_32] connect: poll completed with "
                                     "SO_ERROR=%d revents=0x%x\n",
                                     sockerr, pfd.revents);
                        connect_errno = sockerr;
                    }
                }
            } else if (pr == 0) {
                VITREO_TRACE("[ws2_32] connect: poll-wait %llu ms timed out, "
                             "returning WSAEWOULDBLOCK\n",
                             (unsigned long long)wait_ms);
            } else if (pr < 0) {
                VITREO_TRACE("[ws2_32] connect: poll-wait failed errno=%d\n",
                             errno);
            }
        }
    }

    if (rc < 0) {
        // On BSD, non-blocking connect returns EINPROGRESS.
        // On Windows, this maps to WSAEWOULDBLOCK (10035), not WSAEINPROGRESS.
        u32 wsa_err = (connect_errno == EINPROGRESS)
                          ? WSAEWOULDBLOCK
                          : Ws2Manager::errno_to_wsa(connect_errno);
        VITREO_TRACE("[ws2_32] connect: error=%d (wsa=%u)\n",
                     connect_errno, wsa_err);
        ipc::ipc_log("connect %s:%u -> SOCKET_ERROR wsa=%u errno=%d",
                     addr_str, static_cast<unsigned>(port), wsa_err, connect_errno);
        // Learn / ws2_core_probe: a pending nonblocking connect is reported via
        // getsockopt(SO_ERROR) as a sticky WSA code; a completed failure is
        // latched the same way (refused_SO_ERROR / refused_SO_ERROR_again).
        // A call rejected for the socket's state (a connect already in
        // progress or complete, a bad argument) is not a connection failure
        // and must not be latched: a second connect on a pending nonblocking
        // socket used to poison SO_ERROR/select with WSAEALREADY, so the
        // successful connect then looked failed (connect_nb_ok_select).
        const bool state_error =
            connect_errno == EALREADY || connect_errno == EISCONN ||
            connect_errno == EINVAL || connect_errno == EAFNOSUPPORT ||
            connect_errno == EOPNOTSUPP || connect_errno == EFAULT ||
            connect_errno == EBADF || connect_errno == EINTR;
        if (!state_error) {
            mgr.update_socket_state(guest_socket, [&](Ws2Manager::SocketState& s) {
                if (wsa_err == WSAEWOULDBLOCK) {
                    s.connect_in_progress = true;
                    if (was_unbound) s.bound_by_connect = true;
                } else {
                    s.connect_error = wsa_err;
                    s.connect_in_progress = false;
                }
            });
        }
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        ws2_connect_diag(addr_str, port, rc, connect_errno, wsa_err,
                         event_selected_connect);
    } else {
        VITREO_TRACE("[ws2_32] connect: success\n");
        ipc::ipc_log("connect %s:%u -> OK",
                     addr_str, static_cast<unsigned>(port));
        mgr.update_socket_state(guest_socket, [&](Ws2Manager::SocketState& s) {
            s.connect_error = 0;
            s.connect_in_progress = false;
            if (host_type_for_state == SOCK_STREAM) s.was_connected = true;
            if (was_unbound) s.bound_by_connect = true;
        });
        if (host_type_for_state == SOCK_STREAM) {
            mgr.note_stream_connected(guest_socket);
        }
        m.set_return(0);
        ws2_connect_diag(addr_str, port, rc, connect_errno, 0,
                         event_selected_connect);
    }
}

void ws2::impl_recv(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    s_ws2_io_gen.fetch_add(1, std::memory_order_release);
    // recv(socket, buf, len, flags) → bytes received or SOCKET_ERROR
    u32 guest_socket = static_cast<u32>(m.arg(0));
    const FdReadReenable reenable_fd_read_on_return(mgr, guest_socket);
    GuestAddr buf_ptr = m.arg_ptr(1);
    int len = static_cast<int>(m.arg(2));
    int flags = static_cast<int>(m.arg(3));
    if (ws2_trace_enabled()) {
        u32 guest_tid = ThreadManager::get_current_thread_id_static();
        VITREO_TRACE("[ws2_32] recv(tid=%u, socket=0x%x, len=%d, flags=%d)\n",
                     guest_tid, guest_socket, len, flags);
    }
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        VITREO_TRACE( "[ws2_32] recv: invalid socket -> WSAENOTSOCK\n");
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    // len is an int the provider treats as a buffer length: a negative one
    // faults before anything is received (ws2_core_probe tcp_recv_len_neg,
    // ws2_corex_probe data_len_neg: the pending data stays queued).
    if (len < 0) return fail(WSAEFAULT);

    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    const bool datagram = (host_type == SOCK_DGRAM || host_type == SOCK_SEQPACKET);
    if (datagram) {
        if (mgr.socket_state(guest_socket).shut_rd) return fail(WSAESHUTDOWN);
    } else if (const u32 gate = ws2_stream_recv_gate(mgr, guest_socket, native_fd)) {
        return fail(gate);
    }
    const int fl = ::fcntl(native_fd, F_GETFL, 0);
    const bool nonblocking = fl >= 0 && (fl & O_NONBLOCK) != 0;
    int native_flags = 0;
    if (const u32 ferr = check_recv_flags(flags, datagram, nonblocking, &native_flags)) {
        return fail(ferr);
    }

    // Message-oriented: recvmsg + MSG_TRUNC -> WSAEMSGSIZE (Learn / probe).
    if (datagram) {
        // A buffer the pending datagram cannot be copied to faults without
        // consuming it (ws2_corex_probe udp_null_buf, udp_after_null_buf).
        if (len > 0 && !ws2_guest_range_ok(m, buf_ptr, static_cast<u64>(len), true)) {
            int pending = 0;
            socklen_t pl = sizeof(pending);
            if (::getsockopt(native_fd, SOL_SOCKET, SO_NREAD, &pending, &pl) == 0 &&
                pending > 0 &&
                !ws2_guest_range_ok(m, buf_ptr,
                                    std::min<u64>(static_cast<u64>(len),
                                                  static_cast<u64>(pending)),
                                    true)) {
                return fail(WSAEFAULT);
            }
        }
        // No datagram exceeds 64 KiB: a larger request needs no larger host
        // buffer to see the whole datagram (and MSG_TRUNC).
        const size_t host_len = std::min<size_t>(static_cast<size_t>(len), 256u * 1024u);
        thread_local std::vector<char> host_buf;
        if (host_len > 0 && host_buf.size() < host_len) {
            host_buf.resize(host_len);
        }
        auto dg = recv_datagram_msg(
            native_fd, host_len > 0 ? host_buf.data() : nullptr,
            host_len, native_flags, false);
        if (dg.failed) {
            u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, dg.sys_errno, false);
            mgr.set_last_error(wsa_err);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        if (dg.copied > 0) {
            auto wr = m.memory().write_bytes(
                buf_ptr, reinterpret_cast<const Byte*>(host_buf.data()),
                static_cast<size_t>(dg.copied));
            if (wr.is_err()) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        }
        if (dg.truncated) {
            mgr.set_last_error(WSAEMSGSIZE);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        m.set_return(static_cast<u64>(static_cast<u32>(dg.copied)));
        return;
    }

    // A zero-length stream receive completes only when the socket is
    // readable (ws2_corex_probe nodata_len0: WSAEWOULDBLOCK when nothing is
    // queued); Darwin returns 0 at once, so ask with a one-byte peek.
    if (len == 0) {
        char probe_byte = 0;
        const ssize_t pr = ::recv(native_fd, &probe_byte, 1, native_flags | MSG_PEEK);
        if (pr < 0) {
            const u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, false);
            mgr.reenable_fd_read(guest_socket, native_fd);
            return fail(wsa_err);
        }
        m.set_return(0);
        return;
    }

    // Reuse a thread-local host buffer to avoid per-call heap allocations.
    // A stream receive may return less than requested, so a huge len needs
    // no huge host buffer.
    const size_t host_len = std::min<size_t>(static_cast<size_t>(len), 8u * 1024u * 1024u);
    thread_local std::vector<char> host_buf;
    if (host_buf.size() < host_len) {
        host_buf.resize(host_len);
    }
    
    const bool perf = ws2_perf_enabled();
    auto recv_start = std::chrono::steady_clock::time_point{};
    if (perf) recv_start = std::chrono::steady_clock::now();
    ssize_t rc = ::recv(native_fd, host_buf.data(), host_len, native_flags);
    if (perf) {
        s_ws2_perf_stats.recv_calls.fetch_add(1, std::memory_order_relaxed);
        s_ws2_perf_stats.recv_wait_ns.fetch_add(
            ns_between(recv_start, std::chrono::steady_clock::now()),
            std::memory_order_relaxed);
        if (rc > 0) {
            s_ws2_perf_stats.recv_bytes.fetch_add(static_cast<u64>(rc), std::memory_order_relaxed);
            maybe_emit_ws2_perf_live();
        }
    }
    if (rc <= 0) {
        diagnose_recv(guest_socket, rc, errno);
    }
    if (rc < 0) {
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, false);
        VITREO_TRACE("[ws2_32] recv -> SOCKET_ERROR (wsa=%u)\n", wsa_err);
        // TLS uses recv() after FD_READ (not WSARecv).
        // Always ipc-log errors; skip WOULDBLOCK spam unless tracing.
        if (wsa_err != WSAEWOULDBLOCK || ws2_trace_enabled()) {
            ipc::ipc_log(
                "recv socket=0x%x len=%d flags=0x%x -> SOCKET_ERROR wsa=%u errno=%d",
                guest_socket, len, flags, wsa_err, errno);
        }
        mgr.set_last_error(wsa_err);
        // Learn WSAEventSelect: any recv call, even one that fails, reenables
        // FD_READ. A caller that waits on the event and never calls
        // WSAEnumNetworkEvents (loopback HTTP) otherwise stays latched.
        mgr.reenable_fd_read(guest_socket, native_fd);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    } else if (rc == 0) {
        VITREO_TRACE("[ws2_32] recv -> 0 bytes (EOF)\n");
        ipc::ipc_log("recv socket=0x%x len=%d flags=0x%x -> 0 (EOF)", guest_socket,
                     len, flags);
        recv_sha1_finalize(guest_socket);
        mgr.reenable_fd_read(guest_socket, native_fd);
        m.set_return(0);
    } else {
        VITREO_TRACE("[ws2_32] recv -> %zd bytes\n", rc);
        // Build packet hex dumps only when tracing is enabled.
        if (ws2_trace_enabled() && rc > 0) {
            size_t dump_len = static_cast<size_t>(rc) < 32 ? static_cast<size_t>(rc) : 32;
            char hex[128] = {};
            for (size_t i = 0; i < dump_len; ++i) {
                snprintf(hex + i * 3, 4, "%02x ", static_cast<unsigned char>(host_buf[i]));
            }
            VITREO_TRACE("[ws2_32] recv data: %s\n", hex);
        }

        // Write received data to guest memory
        if (rc > 0) {
            auto res = m.memory().write_bytes(buf_ptr, 
                reinterpret_cast<const Byte*>(host_buf.data()), static_cast<size_t>(rc));
            if (res.is_err()) {
                // A peek leaves the data queued: plain WSAEFAULT. A real
                // receive has consumed it; AFD then aborts the connection
                // (ws2_core_probe tcp_recv_null_buf / tcp_recv_bad_ptr,
                // ws2_corex_probe data_null_64k / data_peer_*_after_abort).
                if ((native_flags & MSG_PEEK) != 0) return fail(WSAEFAULT);
                ws2_abort_stream(mgr, guest_socket, native_fd);
                return fail(WSAECONNABORTED);
            }
            // Streaming SHA-1 of the exact bytes the guest sees.
            recv_sha1_update(guest_socket, host_buf.data(), static_cast<size_t>(rc));

            // Verification mode: read guest memory back and byte-compare to
            // host_buf. If they ever differ, our guest-memory write path is
            // corrupting data on the way in — the symptom of a download that
            // fails its content-hash check.
            static const bool verify_mem = (std::getenv("VITREO_RECV_VERIFY") != nullptr);
            if (verify_mem) {
                std::vector<Byte> verify_buf(static_cast<size_t>(rc));
                auto vr = m.memory().read_bytes(buf_ptr, verify_buf.data(),
                                                static_cast<size_t>(rc));
                if (vr.is_err()) {
                    std::fprintf(stderr,
                        "[recv-verify] socket=0x%x buf=0x%llx len=%zd "
                        "read-back FAILED\n",
                        guest_socket,
                        static_cast<unsigned long long>(buf_ptr), rc);
                } else if (std::memcmp(verify_buf.data(), host_buf.data(),
                                       static_cast<size_t>(rc)) != 0) {
                    // Find first and last diff offset
                    size_t first_diff = 0;
                    while (first_diff < static_cast<size_t>(rc)
                           && verify_buf[first_diff]
                              == static_cast<Byte>(host_buf[first_diff])) {
                        ++first_diff;
                    }
                    size_t last_diff = static_cast<size_t>(rc) - 1;
                    while (last_diff > first_diff
                           && verify_buf[last_diff]
                              == static_cast<Byte>(host_buf[last_diff])) {
                        --last_diff;
                    }
                    std::fprintf(stderr,
                        "[recv-verify] CORRUPTION socket=0x%x buf=0x%llx len=%zd "
                        "first_diff=%zu last_diff=%zu host=%02x guest=%02x\n",
                        guest_socket,
                        static_cast<unsigned long long>(buf_ptr), rc,
                        first_diff, last_diff,
                        static_cast<unsigned>(static_cast<Byte>(host_buf[first_diff])),
                        static_cast<unsigned>(verify_buf[first_diff]));
                }
            }
        }
        m.set_return(static_cast<u64>(static_cast<u32>(rc)));
        track_recv(guest_socket, rc);
        mgr.reenable_fd_read(guest_socket, native_fd);
        ipc::ipc_log("recv socket=0x%x -> %zd bytes flags=0x%x", guest_socket, rc,
                     flags);
        if (rc > 0 && rc <= 4096 &&
            (host_buf[0] == 'G' || host_buf[0] == 'O' || host_buf[0] == 'P' ||
             host_buf[0] == 'H')) {
            std::string preview(host_buf.data(), static_cast<size_t>(rc));
            for (char& ch : preview) {
                if (ch == '\r' || ch == '\n') ch = '|';
                else if (static_cast<unsigned char>(ch) < 0x20) ch = '.';
            }
            if (preview.size() > 300) preview.resize(300);
            ipc::ipc_log("recv socket=0x%x payload %s", guest_socket,
                         preview.c_str());
        }

        // Optional TLS dump for deep debugging only.
        if (tls_dump_enabled() && rc > 100 && static_cast<unsigned char>(host_buf[0]) == 0x16) {
            FILE* fp = fopen("/tmp/vitreo_serverreply.bin", "wb");
            if (fp) {
                fwrite(host_buf.data(), 1, static_cast<size_t>(rc), fp);
                fclose(fp);
                VITREO_TRACE("[ws2_32] recv: dumped %zd bytes to /tmp/vitreo_serverreply.bin\n", rc);
            }
        }
    }
}

} // namespace vitreo::winapi
