// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_socket_lifecycle.cpp
/// @brief socket lifecycle and name/datagram helpers
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
#include <csignal>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <optional>
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

void ws2::impl_WSAStartup(Marshaler& m, Ws2Manager& mgr) {
    // int WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData)
    // Learn: returns the error code directly (not via WSAGetLastError).
    //
    // ws2_core_probe WSAStartup (x64 and x86): the WSADATA is filled even when
    // the version is refused (wVersion = wHighVersion = 2.2 then);
    // iMaxSockets / iMaxUdpDg are 32767 / 65467 for a 1.x negotiation and 0
    // otherwise; lpVendorInfo is never written; the two strings are written
    // with their terminator and the rest of each array is left as it was
    // (desc_tail=204 on a 0xCC-filled struct). NULL lpWSAData -> WSAEFAULT
    // with no startup counted (cleanup_after_null).
    const u16 version_requested = static_cast<u16>(m.arg(0));
    const GuestAddr data_ptr = m.arg_ptr(1);
    if (data_ptr == 0) {
        m.set_return(static_cast<u64>(WSAEFAULT));
        return;
    }

    const u8 major = static_cast<u8>(version_requested & 0xFF);
    const u8 minor = static_cast<u8>((version_requested >> 8) & 0xFF);
    // Negotiate wVersion: min(requested, 2.2) when major >= 1.
    u16 negotiated = 0x0202;
    if (major == 1) {
        negotiated = (minor == 0) ? 0x0001 : 0x0101;  // 1.0 or 1.1
    } else if (major == 2) {
        if (minor == 0) negotiated = 0x0002;
        else if (minor == 1) negotiated = 0x0102;
        else negotiated = 0x0202;  // 2.2+ request -> 2.2
    }
    const bool refused = major < 1;

    // WSADATA (winsock2.h): x64 = wVersion@0 wHighVersion@2 iMaxSockets@4
    // iMaxUdpDg@6 lpVendorInfo@8 szDescription@16[257] szSystemStatus@273[129];
    // x86 = wVersion@0 wHighVersion@2 szDescription@4[257]
    // szSystemStatus@261[129] iMaxSockets@390 iMaxUdpDg@392 lpVendorInfo@396.
    const bool is64 = m.cpu_is_64bit();
    const u32 desc_off = is64 ? 16u : 4u;
    const u32 status_off = desc_off + 257u;
    const u32 max_sockets_off = is64 ? 4u : 390u;
    const u32 max_udp_off = is64 ? 6u : 392u;
    const bool v1 = !refused && (negotiated & 0xFF) == 1;
    static constexpr char kDesc[] = "WinSock 2.0";
    static constexpr char kStatus[] = "Running";
    const bool wrote =
        m.write_u16(data_ptr, refused ? 0x0202 : negotiated).is_ok() &&
        m.write_u16(data_ptr + 2, 0x0202).is_ok() &&
        m.write_u16(data_ptr + max_sockets_off, v1 ? 32767 : 0).is_ok() &&
        m.write_u16(data_ptr + max_udp_off, v1 ? 65467 : 0).is_ok() &&
        m.memory()
            .write_bytes(data_ptr + desc_off, reinterpret_cast<const Byte*>(kDesc),
                         sizeof(kDesc))
            .is_ok() &&
        m.memory()
            .write_bytes(data_ptr + status_off,
                         reinterpret_cast<const Byte*>(kStatus), sizeof(kStatus))
            .is_ok();
    if (!wrote) {
        m.set_return(static_cast<u64>(WSAEFAULT));
        return;
    }
    if (refused) {
        m.set_return(static_cast<u64>(WSAVERNOTSUPPORTED));
        return;
    }

    const int rc = mgr.wsa_startup(version_requested);
    if (rc != 0) {
        m.set_return(static_cast<u64>(rc));
        return;
    }

    ipc::ipc_log("WSAStartup version=0x%04x negotiated=0x%04x data=0x%llx",
                 version_requested, negotiated,
                 static_cast<unsigned long long>(data_ptr));
    m.set_return(0);
}

void ws2::impl_WSACleanup(Marshaler& m, Ws2Manager& mgr) {
    // int WSACleanup(void)
    // Learn WSACleanup: sockets open at the final cleanup are reset and
    // deallocated as if closesocket were called (ws2_core_probe
    // stale_closesocket_after_restart -> WSAENOTSOCK; ws2_corex_probe
    // cleanup_pending_*: a pending overlapped receive on a connected stream
    // completes with STATUS_CONNECTION_RESET and signals its event).
    const int rc = mgr.wsa_cleanup();
    if (rc == 0 && !mgr.winsock_ready()) {
        for (const u32 s : mgr.socket_handles()) close_guest_socket(mgr, s, /*reset=*/true);
        mgr.clear_duplicated_sockets();
    }
    m.set_return(rc == 0 ? 0u : static_cast<u64>(SOCKET_ERROR_VAL));
}

void ws2::impl_bind(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // bind(socket, addr, addrlen)
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr addr_ptr = m.arg_ptr(1);
    int addrlen = static_cast<int>(m.arg(2));
    
    VITREO_TRACE( "[ws2_32] bind(socket=0x%x, addrlen=%d)\n", guest_socket, addrlen);
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        VITREO_TRACE( "[ws2_32] bind: invalid socket -> WSAENOTSOCK\n");
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    // Read Windows sockaddr_in / sockaddr_in6 and convert to host layout.
    MappedSockaddr mapped{};
    if (!read_win_sockaddr_mapped(m, addr_ptr, addrlen, mapped)) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    char addr_str[INET6_ADDRSTRLEN] = {};
    u16 port = 0;
    format_mapped_sockaddr(mapped, addr_str, sizeof(addr_str), &port);

    // Windows address-sharing rules the host does not share (ws2_opts_probe
    // rebind / rebind_any, measured for UDP and TCP alike):
    //  - a wildcard bind on a port another socket holds for a specific
    //    address succeeds unless the new socket is SO_EXCLUSIVEADDRUSE;
    //    Darwin needs SO_REUSEADDR on the new socket for that, and with it
    //    still refuses an exact duplicate, as Windows does;
    //  - SO_REUSEADDR on the new socket against a holder that did not set it
    //    is WSAEACCES (Windows' socket-security check), not WSAEADDRINUSE.
    const auto st = mgr.socket_state(guest_socket);
    bool wildcard = false;
    if (mapped.ss.ss_family == AF_INET) {
        wildcard = reinterpret_cast<const sockaddr_in*>(&mapped.ss)->sin_addr.s_addr == 0;
    } else if (mapped.ss.ss_family == AF_INET6) {
        wildcard = IN6_IS_ADDR_UNSPECIFIED(
            &reinterpret_cast<const sockaddr_in6*>(&mapped.ss)->sin6_addr);
    }
    //  - a SPECIFIC bind on a port another socket holds for the wildcard
    //    address succeeds whatever the new socket's options, unless the
    //    wildcard holder is SO_EXCLUSIVEADDRUSE: then WSAEACCES
    //    (ws2_bind_specific_after_wildcard_probe.expected). Darwin refuses it
    //    unless the new socket has SO_REUSEADDR, and knows no exclusive
    //    holders, so the exclusive case is checked here against this
    //    process's sockets (another process's exclusive holder is not seen).
    if (!wildcard && port != 0) {
        int new_type = 0;
        socklen_t type_len = sizeof(new_type);
        (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &new_type, &type_len);
        for (const u32 other : mgr.socket_handles()) {
            if (other == guest_socket || !mgr.socket_state(other).exclusiveaddruse) continue;
            const int other_fd = mgr.get_native_fd(other);
            if (other_fd < 0) continue;
            sockaddr_storage held{};
            socklen_t held_len = sizeof(held);
            int other_type = 0;
            socklen_t other_type_len = sizeof(other_type);
            if (::getsockname(other_fd, reinterpret_cast<sockaddr*>(&held), &held_len) != 0 ||
                ::getsockopt(other_fd, SOL_SOCKET, SO_TYPE, &other_type, &other_type_len) != 0 ||
                other_type != new_type || held.ss_family != mapped.ss.ss_family) {
                continue;
            }
            bool held_wildcard = false;
            u16 held_port = 0;
            if (held.ss_family == AF_INET) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(&held);
                held_wildcard = in->sin_addr.s_addr == 0;
                held_port = ntohs(in->sin_port);
            } else if (held.ss_family == AF_INET6) {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&held);
                held_wildcard = IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr);
                held_port = ntohs(in6->sin6_port);
            }
            if (held_wildcard && held_port == port) {
                ipc::ipc_log("bind socket=0x%x -> %s:%u FAIL wsa=%u (exclusive wildcard holder 0x%x)",
                             guest_socket, addr_str, static_cast<unsigned>(port),
                             static_cast<unsigned>(WSAEACCES), other);
                mgr.set_last_error(WSAEACCES);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        }
    }
    // An exclusive NEW socket still shares with a holder of the other address
    // kind in the specific-after-wildcard direction (measured), but not in the
    // wildcard-after-specific one (ws2_opts_probe rebind_any second=2).
    const bool temp_reuse = port != 0 && !st.reuseaddr && (!wildcard || !st.exclusiveaddruse);
    if (temp_reuse) {
        const int on = 1;
        (void)::setsockopt(native_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }
    int rc = ::bind(native_fd, reinterpret_cast<sockaddr*>(&mapped.ss),
                    mapped.len);
    const int bind_errno = errno;
    if (temp_reuse) {
        const int off = 0;
        (void)::setsockopt(native_fd, SOL_SOCKET, SO_REUSEADDR, &off, sizeof(off));
    }
    errno = bind_errno;
    if (rc < 0) {
        u32 wsa_err = Ws2Manager::errno_to_wsa(errno);
        if (bind_errno == EADDRINUSE && st.reuseaddr) wsa_err = WSAEACCES;
        ipc::ipc_log("bind socket=0x%x -> %s:%u FAIL wsa=%u errno=%d",
                     guest_socket, addr_str, static_cast<unsigned>(port),
                     wsa_err, errno);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    } else {
        ipc::ipc_log("bind socket=0x%x -> %s:%u OK",
                     guest_socket, addr_str, static_cast<unsigned>(port));
        mgr.update_socket_state(guest_socket, [](Ws2Manager::SocketState& x) {
            x.bound_by_connect = false;
        });
        m.set_return(0);
    }
}

bool close_guest_socket(Ws2Manager& mgr, u32 guest_socket, bool reset) {
    if (!mgr.close_socket(guest_socket, reset)) return false;
    dump_socket_stats(guest_socket);
    recv_sha1_finalize(guest_socket);
    return true;
}

void ws2::impl_closesocket(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    s_ws2_io_gen.fetch_add(1, std::memory_order_release);
    // closesocket(socket) → 0 on success
    u32 guest_socket = static_cast<u32>(m.arg(0));

    VITREO_TRACE( "[ws2_32] closesocket(socket=0x%x, caller_ret=0x%llx)\n",
                 guest_socket, (unsigned long long)guest_caller_ret(m));
    
    // Reject handles outside our allocated socket range (starting at 0x10000)
    if (guest_socket < 0x10000) {
        VITREO_TRACE( "[ws2_32] closesocket: handle 0x%x not a valid socket -> WSAENOTSOCK\n", guest_socket);
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    if (close_guest_socket(mgr, guest_socket)) {
        VITREO_TRACE( "[ws2_32] closesocket: success\n");
        m.set_return(0);
    } else {
        VITREO_TRACE( "[ws2_32] closesocket: invalid socket -> WSAENOTSOCK\n");
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    }
}

void ws2::impl_getpeername(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int getpeername(SOCKET s, struct sockaddr* name, int* namelen)
    // ws2_core_probe: short/null name|namelen -> WSAEFAULT (10014), namelen
    // unchanged; unconnected -> WSAENOTCONN.
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr name_ptr = m.arg_ptr(1);
    GuestAddr namelen_ptr = m.arg_ptr(2);

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    if (!name_ptr || !namelen_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    auto nr = m.read_u32(namelen_ptr);
    if (nr.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const u32 guest_namelen = nr.value();

    struct sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    int rc = ::getpeername(native_fd, reinterpret_cast<struct sockaddr*>(&ss),
                           &sslen);
    if (rc < 0) {
        const int err = errno;
        // An aborted connection keeps its peer (ws2_corex_probe
        // data_getpeername_after_abort: rc=0).
        const auto st = mgr.socket_state(guest_socket);
        if (st.aborted && !st.aborted_peer.empty() &&
            st.aborted_peer.size() <= sizeof(ss)) {
            std::memcpy(&ss, st.aborted_peer.data(), st.aborted_peer.size());
        } else {
            mgr.set_last_error(err == EINVAL ? WSAENOTCONN : Ws2Manager::errno_to_wsa(err));
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    u32 need = 0;
    if (ss.ss_family == AF_INET) {
        need = 16;
    } else if (ss.ss_family == AF_INET6) {
        need = static_cast<u32>(kWinSockaddrIn6Bytes);
    } else if (ss.ss_family == AF_UNIX) {
        need = 2;  // full size checked in write_win_sockaddr
    } else {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (guest_namelen < need) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    u32 written = write_win_sockaddr(m, name_ptr, guest_namelen, ss);
    if (written == 0) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    (void)m.write_u32(namelen_ptr, written);

    VITREO_TRACE("[ws2_32] getpeername(socket=0x%x) -> success\n", guest_socket);
    m.set_return(0);
}

void ws2::impl_getsockname(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int getsockname(SOCKET s, struct sockaddr* name, int* namelen)
    // ws2_core_probe: short/null -> WSAEFAULT (10014); unbound -> WSAEINVAL.
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr name_ptr = m.arg_ptr(1);
    GuestAddr namelen_ptr = m.arg_ptr(2);

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    if (!name_ptr || !namelen_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    auto nr = m.read_u32(namelen_ptr);
    if (nr.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const u32 guest_namelen = nr.value();

    struct sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    int rc = ::getsockname(native_fd, reinterpret_cast<struct sockaddr*>(&ss),
                           &sslen);
    if (rc < 0) {
        mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // ws2_core_probe getsockname_unbound: unbound -> WSAEINVAL (Darwin reports
    // 0.0.0.0:0 successfully).
    u16 port = 0;
    if (ss.ss_family == AF_INET) {
        port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        port = ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    }
    if (port == 0 && (ss.ss_family == AF_INET || ss.ss_family == AF_INET6)) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    u32 need = 0;
    if (ss.ss_family == AF_INET) {
        need = 16;
    } else if (ss.ss_family == AF_INET6) {
        need = static_cast<u32>(kWinSockaddrIn6Bytes);
    } else if (ss.ss_family == AF_UNIX) {
        need = 2;
    } else {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (guest_namelen < need) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    u32 written = write_win_sockaddr(m, name_ptr, guest_namelen, ss);
    if (written == 0) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    (void)m.write_u32(namelen_ptr, written);

    if (ss.ss_family == AF_INET) {
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(&ss);
        VITREO_TRACE("[ws2_32] getsockname(socket=0x%x) -> %s:%u\n",
                     guest_socket, inet_ntoa(sin->sin_addr),
                     static_cast<unsigned>(ntohs(sin->sin_port)));
    } else {
        VITREO_TRACE("[ws2_32] getsockname(socket=0x%x) -> family=%u\n",
                     guest_socket, static_cast<unsigned>(ss.ss_family));
    }
    m.set_return(0);
}

void ws2::impl_listen(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int listen(SOCKET s, int backlog)
    // ws2_core_probe: listen_unbound -> WSAEINVAL; listen_udp -> WSAEOPNOTSUPP;
    // listen_connected -> WSAEISCONN; unbound Darwin auto-binds (rejected here).
    u32 guest_socket = static_cast<u32>(m.arg(0));
    int backlog = static_cast<int>(m.arg(1));
    
    VITREO_TRACE("[ws2_32] listen(socket=0x%x, backlog=%d)\n", guest_socket, backlog);
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    if (host_type != SOCK_STREAM && host_type != SOCK_SEQPACKET) {
        mgr.set_last_error(WSAEOPNOTSUPP);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer),
                      &peer_len) == 0) {
        mgr.set_last_error(WSAEISCONN);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local),
                      &local_len) != 0) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    u16 port = 0;
    if (local.ss_family == AF_INET) {
        port = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
    } else if (local.ss_family == AF_INET6) {
        port = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
    }
    if (port == 0) {
        // Not bound with bind() — Learn WSAEINVAL (Darwin would auto-bind).
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    // Windows SOMAXCONN is 0x7fffffff; SOMAXCONN_HINT(N) is negative N.
    // Map huge/negative hints into a host-reasonable backlog.
    if (backlog < 0) {
        // SOMAXCONN_HINT(N): use |N| clamped to a Windows-like range.
        const int hint = -backlog;
        backlog = std::min(std::max(hint, 200), 65535);
    } else if (backlog > 512) {
        backlog = 512;
    }
    
    int rc = ::listen(native_fd, backlog);
    if (rc < 0) {
        u32 wsa_err = Ws2Manager::errno_to_wsa(errno);
        VITREO_TRACE("[ws2_32] listen: error=%d (wsa=%u)\n", errno, wsa_err);
        ipc::ipc_log("listen socket=0x%x backlog=%d -> FAIL wsa=%u errno=%d",
                     guest_socket, backlog, wsa_err, errno);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    } else {
        VITREO_TRACE("[ws2_32] listen: success\n");
        char addr_str[INET6_ADDRSTRLEN] = {};
        u16 listen_port = 0;
        MappedSockaddr mapped{};
        mapped.ss = local;
        mapped.len = local_len;
        format_mapped_sockaddr(mapped, addr_str, sizeof(addr_str), &listen_port);
        ipc::ipc_log("listen socket=0x%x backlog=%d -> OK %s:%u",
                     guest_socket, backlog, addr_str,
                     static_cast<unsigned>(listen_port));
        mgr.mark_listening(guest_socket);
        m.set_return(0);
    }
}

// -----------------------------------------------------------------------------
void ws2::impl_accept(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::InvalidSocket)) return;

    s_ws2_io_gen.fetch_add(1, std::memory_order_release);
    // SOCKET accept(SOCKET s, struct sockaddr* addr, int* addrlen)
    // Learn / ws2_core_probe: if addr is non-NULL and *addrlen is too small
    // (or zero), fail WSAEFAULT WITHOUT consuming the pending connection
    // (accept_short_addrlen then accept_after_short succeeds).
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr addr_ptr = m.arg_ptr(1);
    GuestAddr addrlen_ptr = m.arg_ptr(2);

    VITREO_TRACE("[ws2_32] accept(socket=0x%x, addr=0x%llx, addrlen=0x%llx)\n",
                 guest_socket, static_cast<unsigned long long>(addr_ptr),
                 static_cast<unsigned long long>(addrlen_ptr));

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        set_invalid_socket_return(m);
        return;
    }

    u32 min_addrlen = 16;
    {
        struct sockaddr_storage local{};
        socklen_t llen = sizeof(local);
        if (::getsockname(native_fd, reinterpret_cast<struct sockaddr*>(&local),
                          &llen) == 0) {
            if (local.ss_family == AF_INET6) {
                min_addrlen = static_cast<u32>(kWinSockaddrIn6Bytes);
            } else if (local.ss_family == AF_UNIX) {
                min_addrlen = 2;
            }
        }
    }

    if (addr_ptr != 0 && addrlen_ptr != 0) {
        const auto addrlen_res = m.read_u32(addrlen_ptr);
        if (addrlen_res.is_err() || addrlen_res.value() < min_addrlen) {
            mgr.set_last_error(WSAEFAULT);
            set_invalid_socket_return(m);
            return;
        }
    }

    struct sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);

    int new_fd = ::accept(native_fd, reinterpret_cast<struct sockaddr*>(&ss), &sslen);
    // Learn WSAEventSelect: accept and WSAAccept re-enable FD_ACCEPT, whatever
    // they return; a connection still pending is recorded again.
    const int accept_errno = errno;
    mgr.reenable_fd_accept(guest_socket);
    errno = accept_errno;
    if (new_fd < 0) {
        u32 wsa_err = Ws2Manager::errno_to_wsa(errno);
        VITREO_TRACE("[ws2_32] accept: error=%d (wsa=%u)\n", errno, wsa_err);
        // Would-block accepts are hot on async servers; log other failures.
        if (wsa_err != WSAEWOULDBLOCK) {
            ipc::ipc_log("accept listen=0x%x -> FAIL wsa=%u errno=%d",
                         guest_socket, wsa_err, errno);
        }
        mgr.set_last_error(wsa_err);
        set_invalid_socket_return(m);
        return;
    }

    // Allocate a guest socket handle for the new connection
    u32 new_guest = mgr.alloc_socket(new_fd);
    mgr.inherit_listener_state(guest_socket, new_guest);

    // Write back the peer address if requested (Windows sockaddr layout).
    // NULL addr or NULL addrlen skips the write (accept_null_addr / _null_len).
    if (addr_ptr != 0 && addrlen_ptr != 0) {
        auto addrlen_res = m.read_u32(addrlen_ptr);
        u32 guest_addrlen = addrlen_res.is_ok() ? addrlen_res.value() : 0;
        if (guest_addrlen > 0) {
            u32 written = write_win_sockaddr(m, addr_ptr, guest_addrlen, ss);
            if (written != 0) {
                (void)m.write_u32(addrlen_ptr, written);
            }
        }
    }

    {
        MappedSockaddr peer{};
        peer.ss = ss;
        peer.len = sslen;
        char addr_str[INET6_ADDRSTRLEN] = {};
        u16 port = 0;
        format_mapped_sockaddr(peer, addr_str, sizeof(addr_str), &port);
        ipc::ipc_log("accept listen=0x%x -> 0x%x peer=%s:%u",
                     guest_socket, new_guest, addr_str,
                     static_cast<unsigned>(port));
    }

    VITREO_TRACE("[ws2_32] accept: success, new socket=0x%x (native=%d)\n", new_guest, new_fd);
    m.set_return(static_cast<u64>(new_guest));
}

void ws2::impl_shutdown(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int shutdown(SOCKET s, int how)
    // how: SD_RECEIVE=0, SD_SEND=1, SD_BOTH=2
    u32 guest_socket = static_cast<u32>(m.arg(0));
    int how = static_cast<int>(m.arg(1));

    VITREO_TRACE("[ws2_32] shutdown(socket=0x%x, how=%d)\n", guest_socket, how);

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // Map Windows constants to POSIX constants
    int native_how;
    switch (how) {
        case SD_RECEIVE: native_how = SHUT_RD;   break;
        case SD_SEND:    native_how = SHUT_WR;   break;
        case SD_BOTH:    native_how = SHUT_RDWR; break;
        default:
            mgr.set_last_error(WSAEINVAL);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
    }

    // Measured (ws2_core_probe shutdown_* / udp_shutdown_*, ws2_corex_probe
    // tcp_shut_* / listener_shutdown / udp_bound_shut_*):
    //  - a datagram socket accepts every direction, connected or not, and
    //    only records it (later sends/receives fail WSAESHUTDOWN);
    //  - a stream that is or has been connected accepts any direction any
    //    number of times -- after the peer closed, after an abort, again
    //    after SD_BOTH -- though Darwin refuses a repeated host shutdown;
    //  - a stream that never connected (unconnected, listening) is
    //    WSAENOTCONN.
    // The direction is latched BEFORE the host shutdown: SHUT_RD makes the
    // host socket read as EOF at once, and a pending overlapped receive must
    // see the latch rather than that EOF (ws2_ovres_probe
    // shutdown_rd_under_recv: the receive stays pending).
    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    const bool stream = host_type == SOCK_STREAM;
    const auto before = mgr.socket_state(guest_socket);
    bool connected = false;
    if (stream) {
        sockaddr_storage peer{};
        socklen_t pl = sizeof(peer);
        connected = ::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0;
        if (!connected && !before.was_connected && !before.aborted &&
            !before.shut_rd && !before.shut_wr) {
            mgr.set_last_error(WSAENOTCONN);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }
    mgr.update_socket_state(guest_socket, [how, connected](Ws2Manager::SocketState& st) {
        if (how == SD_RECEIVE || how == SD_BOTH) st.shut_rd = true;
        if (how == SD_SEND || how == SD_BOTH) st.shut_wr = true;
        if (connected) st.was_connected = true;
    });
    if (stream && connected && !before.aborted) {
        if (::shutdown(native_fd, native_how) < 0 && errno != ENOTCONN &&
            errno != EINVAL) {
            const u32 wsa_err = Ws2Manager::errno_to_wsa(errno);
            mgr.update_socket_state(guest_socket, [&before](Ws2Manager::SocketState& st) {
                st.shut_rd = before.shut_rd;
                st.shut_wr = before.shut_wr;
            });
            VITREO_TRACE("[ws2_32] shutdown -> error %u\n", wsa_err);
            mgr.set_last_error(wsa_err);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }
    // Learn: subsequent recv/send after SD_RECEIVE/SD_SEND fail WSAESHUTDOWN.
    VITREO_TRACE("[ws2_32] shutdown -> success\n");
    m.set_return(0);
}

void ws2::impl_WSASendDisconnect(Marshaler& m, Ws2Manager& mgr) {
    // int WSASendDisconnect(SOCKET s, LPWSABUF lpOutboundDisconnectData)
    // Learn: equivalent to shutdown(SD_SEND); native TCP/IP does not support
    // disconnect data (XP1_DISCONNECT_DATA) — non-empty out data -> WSAEOPNOTSUPP.
    const u32 guest_socket = static_cast<u32>(m.arg(0));
    const GuestAddr data_ptr = m.arg_ptr(1);
    const int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (data_ptr != 0) {
        const bool is64 = m.cpu_is_64bit();
        const auto len_r = m.read_u32(data_ptr);
        if (len_r.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        if (len_r.value() != 0) {
            mgr.set_last_error(WSAEOPNOTSUPP);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        (void)is64;
    }
    if (::shutdown(native_fd, SHUT_WR) < 0) {
        mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    mgr.update_socket_state(guest_socket, [](Ws2Manager::SocketState& st) {
        st.shut_wr = true;
    });
    m.set_return(0);
}

namespace {

/// Marker-gated UDP send/receive census, written per pid. Relay probes are
/// `sendto`/`recvfrom` on unconnected sockets, and the existing ws2 perf
/// counters cover neither, so a failure to reach any relay had no evidence
/// either way. touch /tmp/vitreo-net-stats to enable.
void udp_census(const char* what, long long rc, unsigned wsa_error) {
    static const bool enabled = ::access("/tmp/vitreo-net-stats", F_OK) == 0;
    if (!enabled) return;
    static std::atomic<unsigned long long> sends{0};
    static std::atomic<unsigned long long> send_errors{0};
    static std::atomic<unsigned long long> recvs{0};
    static std::atomic<unsigned long long> recv_errors{0};
    static std::atomic<unsigned long long> total{0};
    const bool is_send = what[0] == 's';
    if (rc < 0) {
        (is_send ? send_errors : recv_errors).fetch_add(1);
    } else {
        (is_send ? sends : recvs).fetch_add(1);
    }
    const unsigned long long seen = total.fetch_add(1) + 1;
    // Also write on the first call: a relay probe may be a handful of
    // packets, and a modulo-only trigger then produces no file at all —
    // indistinguishable from "the entry point is never called".
    if (seen != 1ull && (seen % 200ull) != 0ull) return;
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/vitreo-udp-%d.log",
                  static_cast<int>(::getpid()));
    if (std::FILE* file = std::fopen(path, "w")) {
        std::fprintf(file,
                     "sendto_ok=%llu sendto_err=%llu recvfrom_ok=%llu "
                     "recvfrom_err=%llu last=%s rc=%lld wsa=%u\n",
                     sends.load(), send_errors.load(), recvs.load(),
                     recv_errors.load(), what, rc, wsa_error);
        (void)std::fclose(file);
    }
}

} // namespace

void ws2::impl_recvfrom(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int recvfrom(SOCKET s, char* buf, int len, int flags,
    //              struct sockaddr* from, int* fromlen)
    u32 guest_socket = static_cast<u32>(m.arg(0));
    const FdReadReenable reenable_fd_read_on_return(mgr, guest_socket);
    GuestAddr buf_ptr = m.arg_ptr(1);
    int len = static_cast<int>(m.arg(2));
    int flags = static_cast<int>(m.arg(3));
    GuestAddr from_ptr = m.arg_ptr(4);
    GuestAddr fromlen_ptr = m.arg_ptr(5);

    VITREO_TRACE("[ws2_32] recvfrom(socket=0x%x, len=%d, flags=%d)\n",
                 guest_socket, len, flags);

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    // Negative len -> WSAEFAULT (ws2_corex_probe udp_len_neg). One of from /
    // fromlen without the other is WSAEFAULT before anything is received
    // (ws2_core_probe recvfrom_null_from: the datagram stays queued, which
    // the following udp_recv_len0 / udp_recv_after_len0 pair shows;
    // ws2_corex_probe udp_recvfrom_null_fromlen).
    if (len < 0) return fail(WSAEFAULT);
    if ((from_ptr == 0) != (fromlen_ptr == 0)) return fail(WSAEFAULT);

    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    const bool datagram = (host_type == SOCK_DGRAM || host_type == SOCK_SEQPACKET);
    if (datagram) {
        if (mgr.socket_state(guest_socket).shut_rd) return fail(WSAESHUTDOWN);
    } else if (const u32 gate = ws2_stream_recv_gate(mgr, guest_socket, native_fd)) {
        return fail(gate);
    }

    // Learn / ws2_core_probe recvfrom_short_fromlen: WSAEFAULT without
    // consuming the datagram when *fromlen is too small for the address.
    u32 min_from = 16;
    {
        struct sockaddr_storage local{};
        socklen_t llen = sizeof(local);
        if (::getsockname(native_fd, reinterpret_cast<struct sockaddr*>(&local),
                          &llen) == 0) {
            if (local.ss_family == AF_INET6) {
                min_from = static_cast<u32>(kWinSockaddrIn6Bytes);
            } else if (local.ss_family == AF_UNIX) {
                min_from = 2;
            }
        }
    }
    if (from_ptr != 0 && fromlen_ptr != 0) {
        const auto fl = m.read_u32(fromlen_ptr);
        if (fl.is_err() || fl.value() < min_from) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    const int fl = ::fcntl(native_fd, F_GETFL, 0);
    const bool nonblocking = fl >= 0 && (fl & O_NONBLOCK) != 0;
    int native_rf_flags = 0;
    if (const u32 ferr = check_recv_flags(flags, datagram, nonblocking, &native_rf_flags)) {
        return fail(ferr);
    }

    // Datagrams never exceed 64 KiB; a stream receive may return less than
    // asked. Neither needs a host buffer the size of a huge len.
    const size_t host_len = std::min<size_t>(
        static_cast<size_t>(len), datagram ? 256u * 1024u : 8u * 1024u * 1024u);
    // A buffer the pending datagram cannot be copied to faults without
    // consuming it (ws2_corex_probe udp_null_buf).
    if (datagram && len > 0 &&
        !ws2_guest_range_ok(m, buf_ptr, static_cast<u64>(len), true)) {
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
    std::vector<char> host_buf(host_len > 0 ? host_len : 1);
    struct sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    ssize_t rc = 0;
    bool truncated = false;

    if (datagram) {
        auto dg = recv_datagram_msg(
            native_fd, host_len > 0 ? host_buf.data() : nullptr, host_len,
            native_rf_flags, from_ptr != 0);
        if (dg.failed) {
            u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, dg.sys_errno, false);
            udp_census("recvfrom", -1, wsa_err);
            mgr.set_last_error(wsa_err);
            mgr.reenable_fd_read(guest_socket, native_fd);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        rc = dg.copied;
        truncated = dg.truncated;
        ss = dg.from;
        sslen = dg.from_len;
    } else {
        rc = ::recvfrom(native_fd, host_buf.data(), host_len,
                        native_rf_flags,
                        from_ptr ? reinterpret_cast<struct sockaddr*>(&ss) : nullptr,
                        from_ptr ? &sslen : nullptr);
        if (rc < 0) {
            u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, false);
            VITREO_TRACE("[ws2_32] recvfrom -> SOCKET_ERROR (wsa=%u)\n", wsa_err);
            udp_census("recvfrom", -1, wsa_err);
            mgr.set_last_error(wsa_err);
            mgr.reenable_fd_read(guest_socket, native_fd);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    // Write received data to guest memory
    if (rc > 0) {
        auto res = m.memory().write_bytes(buf_ptr,
            reinterpret_cast<const Byte*>(host_buf.data()), static_cast<size_t>(rc));
        if (res.is_err()) {
            // Consumed stream data that cannot be delivered aborts the
            // connection, as in recv (ws2_corex_probe data_null_*).
            if (!datagram && (native_rf_flags & MSG_PEEK) == 0) {
                ws2_abort_stream(mgr, guest_socket, native_fd);
                return fail(WSAECONNABORTED);
            }
            return fail(WSAEFAULT);
        }
    }

    // Write source address to guest (Windows sockaddr_in / sockaddr_in6)
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

    if (truncated) {
        mgr.set_last_error(WSAEMSGSIZE);
        udp_census("recvfrom", -1, WSAEMSGSIZE);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    VITREO_TRACE("[ws2_32] recvfrom -> %zd bytes\n", rc);
    udp_census("recvfrom", rc, 0);
    mgr.reenable_fd_read(guest_socket, native_fd);
    m.set_return(static_cast<u64>(static_cast<u32>(rc)));
}

void ws2::impl_sendto(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int sendto(SOCKET s, const char* buf, int len, int flags,
    //            const struct sockaddr* to, int tolen)
    // Learn / ws2_core_probe: on SOCK_STREAM, to/tolen are ignored (tcp_sendto_*);
    // on a connected datagram socket, to overrides the peer for that datagram.
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr buf_ptr = m.arg_ptr(1);
    int len = static_cast<int>(m.arg(2));
    int flags = static_cast<int>(m.arg(3));
    GuestAddr to_ptr = m.arg_ptr(4);
    int tolen = static_cast<int>(m.arg32(5));

    VITREO_TRACE("[ws2_32] sendto(socket=0x%x, len=%d, flags=%d, tolen=%d)\n",
                 guest_socket, len, flags, tolen);

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    // Negative len -> WSAEFAULT (ws2_corex_probe udp_sendto_len_neg).
    if (len < 0) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    {
        const auto st = mgr.socket_state(guest_socket);
        const u32 gate = st.aborted ? WSAECONNABORTED
                         : (st.connect_error != 0 && !st.connect_in_progress)
                             ? WSAENOTCONN
                             : st.shut_wr ? WSAESHUTDOWN : 0u;
        if (gate != 0) {
            mgr.set_last_error(gate);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    int host_type = 0;
    socklen_t tl = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);

    // Connection-oriented: sendto == send (to/tolen ignored).
    if (host_type == SOCK_STREAM || host_type == SOCK_SEQPACKET) {
        std::vector<char> host_buf(static_cast<size_t>(len > 0 ? len : 0));
        if (len > 0) {
            auto res = m.memory().read_bytes(
                buf_ptr, reinterpret_cast<Byte*>(host_buf.data()),
                static_cast<size_t>(len));
            if (res.is_err()) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        }
        int native_st_flags = 0;
        if (const u32 ferr = check_send_flags(flags, &native_st_flags)) {
            mgr.set_last_error(ferr);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        ssize_t rc = ::send(native_fd, host_buf.data(), static_cast<size_t>(len),
                            native_st_flags);
        if (rc < 0) {
            u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, errno, true);
            mgr.set_last_error(wsa_err);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        } else {
            m.set_return(static_cast<u64>(static_cast<u32>(rc)));
        }
        return;
    }

    // Read data from guest memory
    std::vector<char> host_buf(static_cast<size_t>(len > 0 ? len : 0));
    if (len > 0) {
        auto res = m.memory().read_bytes(buf_ptr,
            reinterpret_cast<Byte*>(host_buf.data()), static_cast<size_t>(len));
        if (res.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }

    // Datagram destination validation (ws2_core_probe udp_sendto_*):
    // null to -> WSAENOTCONN when unconnected; short/bad ptr -> WSAEFAULT;
    // bad family / AF mismatch -> WSAEAFNOSUPPORT; INADDR_ANY -> WSAEADDRNOTAVAIL.
    MappedSockaddr mapped{};
    struct sockaddr* dest_addr = nullptr;
    socklen_t dest_len = 0;
    if (!to_ptr) {
        sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer),
                          &peer_len) != 0) {
            mgr.set_last_error(WSAENOTCONN);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        // Connected + null to: equivalent to send().
    } else {
        if (tolen < 2) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        auto fam_r = m.read_u16(to_ptr);
        if (fam_r.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        const u16 win_fam = fam_r.value();
        if (win_fam == kWinAfInet) {
            if (tolen < 16) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        } else if (win_fam == kWinAfInet6) {
            if (tolen < kWinSockaddrIn6Bytes) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        } else {
            mgr.set_last_error(WSAEAFNOSUPPORT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        if (!read_win_sockaddr_mapped(m, to_ptr, tolen, mapped)) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        // Socket family vs destination family (udp4_sendto_v6_addr -> 10047).
        // Unbound Darwin sockets still report AF via getsockname.
        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&local),
                          &local_len) == 0 &&
            local.ss_family != mapped.ss.ss_family) {
            mgr.set_last_error(WSAEAFNOSUPPORT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        // Windows sends a datagram to port 0 (ws2_core_probe
        // udp_sendto_port0: rc=1; ws2_corex_probe udp_recv_after_port0: the
        // ICMP port-unreachable then resets the next receive). Darwin's UDP
        // refuses destination port 0 outright (EADDRNOTAVAIL), so the
        // datagram cannot be put on the wire: fail loudly with the
        // documented sendto error rather than report a send that never
        // happened.
        {
            const u16 dport = mapped.ss.ss_family == AF_INET6
                ? reinterpret_cast<const sockaddr_in6*>(&mapped.ss)->sin6_port
                : reinterpret_cast<const sockaddr_in*>(&mapped.ss)->sin_port;
            if (dport == 0) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED ws2_32!sendto: datagram to "
                                 "destination port 0 (the host UDP stack cannot send it)\n");
                }
                mgr.set_last_error(WSAEADDRNOTAVAIL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        }
        if (mapped.ss.ss_family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(&mapped.ss);
            if (sin->sin_addr.s_addr == 0) {
                mgr.set_last_error(WSAEADDRNOTAVAIL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        } else if (mapped.ss.ss_family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&mapped.ss);
            const auto* b = reinterpret_cast<const u8*>(&sin6->sin6_addr);
            bool any = true;
            for (int i = 0; i < 16; ++i) {
                if (b[i] != 0) { any = false; break; }
            }
            if (any) {
                mgr.set_last_error(WSAEADDRNOTAVAIL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
        }
        dest_addr = reinterpret_cast<struct sockaddr*>(&mapped.ss);
        dest_len = mapped.len;
    }

    // Connected UDP + destination: Darwin returns EISCONN if still associated.
    // Learn: `to` overrides for that datagram only. Dissociate, sendto, restore.
    // On Darwin, connect(AF_UNSPEC) returns EAFNOSUPPORT/EINVAL but still clears
    // the peer (getpeername -> ENOTCONN); ignore the connect return code.
    sockaddr_storage saved_peer{};
    socklen_t saved_len = 0;
    bool restore_peer = false;
    if (dest_addr != nullptr) {
        sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer),
                          &peer_len) == 0) {
            saved_peer = peer;
            saved_len = peer_len;
            sockaddr_in unspec{};
            unspec.sin_family = AF_UNSPEC;
#if defined(__APPLE__)
            unspec.sin_len = static_cast<u8>(sizeof(unspec));
#endif
            (void)::connect(native_fd, reinterpret_cast<sockaddr*>(&unspec),
                            sizeof(unspec));
            restore_peer = true;
        }
    }

    int native_st_flags = 0;
    if (const u32 ferr = check_send_flags(flags, &native_st_flags)) {
        mgr.set_last_error(ferr);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    ssize_t rc = ::sendto(native_fd, host_buf.data(), static_cast<size_t>(len),
                          native_st_flags, dest_addr, dest_len);
    const int send_errno = errno;
    if (restore_peer) {
        (void)::connect(native_fd, reinterpret_cast<sockaddr*>(&saved_peer),
                        saved_len);
    }
    if (rc < 0) {
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, send_errno, true);
        VITREO_TRACE("[ws2_32] sendto -> SOCKET_ERROR (wsa=%u)\n", wsa_err);
        udp_census("sendto", -1, wsa_err);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    } else {
        VITREO_TRACE("[ws2_32] sendto -> %zd bytes\n", rc);
        udp_census("sendto", rc, 0);
        m.set_return(static_cast<u64>(static_cast<u32>(rc)));
    }
}

namespace {

/// Vitreo's Winsock provider catalog, in the order Windows 11 enumerates the
/// MSAFD Tcpip entries (IPv6 before IPv4: ws2_corex_probe sock af=0 picks
/// iaf=23), plus the AF_UNIX stream provider. A nonzero protocol must fall in
/// [protocol, protocol + max_offset]; 0 matches any entry.
struct CatalogEntry {
    int win_af;
    int type;
    int protocol;
    int max_offset;
};
constexpr CatalogEntry kCatalog[] = {
    {kWinAfInet6, SOCK_STREAM, IPPROTO_TCP, 0},
    {kWinAfInet6, SOCK_DGRAM, IPPROTO_UDP, 0},
    {kWinAfInet6, SOCK_RAW, 0, 255},
    {kWinAfInet, SOCK_STREAM, IPPROTO_TCP, 0},
    {kWinAfInet, SOCK_DGRAM, IPPROTO_UDP, 0},
    {kWinAfInet, SOCK_RAW, 0, 255},
    {kWinAfUnix, SOCK_STREAM, 0, 0},
};

/// WSASocket provider selection, measured over the full (af, type,
/// protocol) matrix in ws2_corex_probe sock:*: AF_UNSPEC needs a protocol
/// (WSAEINVAL); no entry for the family -> WSAEAFNOSUPPORT; none of the
/// family's entries has the type -> WSAESOCKTNOSUPPORT; otherwise
/// WSAEPROTONOSUPPORT. The AF_UNIX provider reports a wrong type as
/// WSAEAFNOSUPPORT (ws2_core_probe socket_af_unix_dgram).
const CatalogEntry* select_provider(int af, int type, int protocol, u32* error) {
    if (af == kWinAfUnspec && protocol == 0) {
        *error = WSAEINVAL;
        return nullptr;
    }
    bool af_seen = false;
    bool type_seen = false;
    for (const auto& e : kCatalog) {
        if (af != kWinAfUnspec && e.win_af != af) continue;
        af_seen = true;
        if (type != 0 && e.type != type) continue;
        type_seen = true;
        if (protocol != 0 &&
            (protocol < e.protocol || protocol > e.protocol + e.max_offset)) {
            continue;
        }
        return &e;
    }
    if (!af_seen || (af == kWinAfUnix && !type_seen)) {
        *error = WSAEAFNOSUPPORT;
    } else {
        *error = type_seen ? WSAEPROTONOSUPPORT : WSAESOCKTNOSUPPORT;
    }
    return nullptr;
}

}  // namespace

void create_guest_socket(Marshaler& m, Ws2Manager& mgr, int af, int type,
                        int protocol) {
    // Address families are NOT the same numbers on both sides: Windows
    // AF_INET6 is 23 while macOS uses 30 (AF_INET 2 and AF_UNSPEC 0 /
    // AF_UNIX 1 coincide). Passing an unsupported Windows family through
    // (e.g. AF_BTH=32) would hit Darwin AF_SYSTEM and succeed incorrectly.
    const int guest_af = af;
    u32 select_error = 0;
    const CatalogEntry* provider = select_provider(af, type, protocol, &select_error);
    if (provider == nullptr) {
        mgr.set_last_error(select_error);
        set_invalid_socket_return(m);
        return;
    }
    af = provider->win_af == kWinAfInet6 ? AF_INET6
         : provider->win_af == kWinAfUnix ? AF_UNIX
                                          : AF_INET;
    type = provider->type;
    // A protocol-0 request takes the entry's protocol; a raw socket keeps
    // the caller's IP protocol number (Learn socket, SOCK_RAW).
    if (protocol == 0 && provider->type != SOCK_RAW) protocol = provider->protocol;

    int native_fd = ::socket(af, type, protocol);
    if (native_fd < 0) {
        mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
        VITREO_TRACE( "[ws2_32] socket(af=%d, type=%d, proto=%d) -> INVALID (errno=%d)\n",
                     af, type, protocol, errno);
        ipc::ipc_log("socket af=%d type=%d proto=%d -> INVALID errno=%d caller=0x%llx",
                     guest_af, type, protocol, errno,
                     (unsigned long long)guest_caller_ret(m));
        set_invalid_socket_return(m);
    } else {
        if (af == AF_INET6) {
            const int v6only = 1;
            (void)::setsockopt(native_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
                               sizeof(v6only));
        }
        apply_windows_socket_defaults(native_fd, af, type);
        u32 guest_socket = mgr.alloc_socket(native_fd);
        VITREO_TRACE( "[ws2_32] socket(af=%d, type=%d, proto=%d) -> guest=0x%x (native=%d) caller_ret=0x%llx\n",
                     af, type, protocol, guest_socket, native_fd,
                     (unsigned long long)guest_caller_ret(m));
        ipc::ipc_log("socket af=%d type=%d proto=%d -> 0x%x caller=0x%llx",
                     guest_af, type, protocol, guest_socket,
                     (unsigned long long)guest_caller_ret(m));
        m.set_return(static_cast<u64>(guest_socket));
    }
}

void ws2::impl_socket(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::InvalidSocket)) return;

    // socket(af, type, protocol) → SOCKET or INVALID_SOCKET
    create_guest_socket(m, mgr, static_cast<int>(m.arg(0)),
                        static_cast<int>(m.arg(1)),
                        static_cast<int>(m.arg(2)));
}

void ws2::impl_WSASocketW(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::InvalidSocket)) return;

    // WSASocket{A,W}(af, type, protocol, lpProtocolInfo, g, dwFlags)
    // Flags / FROM_PROTOCOL_INFO from Learn + ws2_core_probe.expected.
    int af = static_cast<int>(m.arg(0));
    int type = static_cast<int>(m.arg(1));
    int protocol = static_cast<int>(m.arg(2));
    const GuestAddr info = m.arg_ptr(3);
    (void)m.arg(4);  // GROUP g — oracle WSASocketW_group1: accepted
    const u32 flags = m.arg32(5);

    constexpr u32 kWsaFlagOverlapped = 0x01;
    constexpr u32 kWsaFlagMpCRoot = 0x02;
    constexpr u32 kWsaFlagMpCLeaf = 0x04;
    constexpr u32 kWsaFlagMpDRoot = 0x08;
    constexpr u32 kWsaFlagMpDLeaf = 0x10;
    constexpr u32 kWsaFlagAccessSysSec = 0x40;
    constexpr u32 kWsaFlagNoInherit = 0x80;
    // WSA_FLAG_REGISTERED_IO (winsock2.h 0x100) only marks the socket as
    // usable with RIO; Windows accepts it (ws2_misc_probe
    // wsasocket_registered_io). Vitreo has no RIO table, so a caller asking
    // SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER for it still gets the
    // WSAIoctl failure it falls back on.
    constexpr u32 kWsaFlagRegisteredIo = 0x100;
    constexpr u32 kKnownFlags =
        kWsaFlagOverlapped | kWsaFlagMpCRoot | kWsaFlagMpCLeaf |
        kWsaFlagMpDRoot | kWsaFlagMpDLeaf | kWsaFlagAccessSysSec |
        kWsaFlagNoInherit | kWsaFlagRegisteredIo;
    constexpr u32 kMultipoint =
        kWsaFlagMpCRoot | kWsaFlagMpCLeaf | kWsaFlagMpDRoot | kWsaFlagMpDLeaf;

    // Oracle: unknown bits and multipoint TCP -> WSAEPROTOTYPE (10041).
    if ((flags & ~kKnownFlags) != 0 || (flags & kMultipoint) != 0) {
        mgr.set_last_error(WSAEPROTOTYPE);
        set_invalid_socket_return(m);
        return;
    }
    (void)kWsaFlagNoInherit;
    (void)kWsaFlagAccessSysSec;

    constexpr int kFromProtocolInfo = -1;
    if (info != 0) {
        // WSAPROTOCOL_INFO.dwProviderReserved (offset 112 in both the A and W
        // layouts) is zero in catalog / SO_PROTOCOL_INFO entries and carries
        // the duplicate token from WSADuplicateSocket. An unknown nonzero
        // value is WSAEINVAL (ws2_corex_probe from_info_forged_reserved);
        // a known one may be used any number of times (dup_second_use).
        const auto reserved = m.read_u32(info + 112);
        if (reserved.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            set_invalid_socket_return(m);
            return;
        }
        if (reserved.value() != 0) {
            Ws2Manager::SocketState st;
            const int fd = mgr.take_duplicated_socket(reserved.value(), &st);
            if (fd < 0) {
                mgr.set_last_error(WSAEINVAL);
                set_invalid_socket_return(m);
                return;
            }
            const u32 guest = mgr.alloc_socket(fd);
            mgr.update_socket_state(guest, [&st](Ws2Manager::SocketState& x) { x = st; });
            ipc::ipc_log("WSASocket FROM_PROTOCOL_INFO duplicate token=0x%x -> 0x%x",
                         reserved.value(), guest);
            m.set_return(static_cast<u64>(guest));
            return;
        }
        // WSAPROTOCOL_INFO{A,W}: iAddressFamily@76, iSocketType@88, iProtocol@92
        // (write_protocol_info / probe SO_PROTOCOL_INFOW).
        auto take = [&](int cur, u32 off) -> std::optional<int> {
            if (cur != kFromProtocolInfo) return cur;
            const auto r = m.read_u32(info + off);
            if (r.is_err()) return std::nullopt;
            return static_cast<int>(r.value());
        };
        const auto af_o = take(af, 76);
        const auto type_o = take(type, 88);
        const auto proto_o = take(protocol, 92);
        if (!af_o || !type_o || !proto_o) {
            mgr.set_last_error(WSAEFAULT);
            set_invalid_socket_return(m);
            return;
        }
        af = *af_o;
        type = *type_o;
        protocol = *proto_o;
    } else if (af == kFromProtocolInfo || type == kFromProtocolInfo ||
               protocol == kFromProtocolInfo) {
        // Oracle WSASocketW_all_minus1: WSAESOCKTNOSUPPORT (10044).
        mgr.set_last_error(WSAESOCKTNOSUPPORT);
        set_invalid_socket_return(m);
        return;
    }

    create_guest_socket(m, mgr, af, type, protocol);
}

void ws2::impl_WSADuplicateSocket(Marshaler& m, Ws2Manager& mgr, bool unicode) {
    // int WSADuplicateSocket{A,W}(SOCKET s, DWORD dwProcessId,
    //                             LPWSAPROTOCOL_INFO{A,W} lpProtocolInfo)
    // Learn: the returned WSAPROTOCOL_INFO is passed to the target process,
    // which creates its descriptor with WSASocket(FROM_PROTOCOL_INFO...).
    // Measured (ws2_core_probe WSADuplicateSocketW_*, ws2_corex_probe dup_*):
    // the blob matches SO_PROTOCOL_INFO except for a nonzero
    // dwProviderReserved; the duplicate outlives the source's closesocket;
    // a nonexistent process is WSAEINVAL; NULL lpProtocolInfo is WSAEFAULT.
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const u32 pid = m.arg32(1);
    const GuestAddr info = m.arg_ptr(2);
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    const int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) return fail(WSAENOTSOCK);
    const u32 info_bytes = unicode ? 628u : 372u;
    if (info == 0 || !ws2_guest_range_ok(m, info, info_bytes, true)) return fail(WSAEFAULT);
    // GetCurrentProcessId is the host pid.
    if (pid != static_cast<u32>(::getpid())) {
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            return fail(WSAEINVAL);
        }
        // Another live process: its runtime would have to receive the host
        // descriptor, and no socket transfer exists between Vitreo processes.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ws2_32!WSADuplicateSocket%c: target is "
                         "another process (pid %u); only this process is supported\n",
                         unicode ? 'W' : 'A', pid);
        }
        return fail(WSAEINVAL);
    }
    const int dup_fd = ::dup(fd);
    if (dup_fd < 0) return fail(errno == EMFILE ? WSAEMFILE : WSAENOBUFS);
    if (!ws2_write_socket_protocol_info(m, info, unicode, fd)) {
        ::close(dup_fd);
        return fail(WSAEFAULT);
    }
    const u32 token = mgr.register_duplicated_socket(guest_socket, dup_fd);
    (void)m.write_u32(info + 112, token);  // dwProviderReserved
    m.set_return(0);
}

} // namespace vitreo::winapi
