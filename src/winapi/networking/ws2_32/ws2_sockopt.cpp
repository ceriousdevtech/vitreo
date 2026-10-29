// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_sockopt.cpp
/// @brief getsockopt / setsockopt / ioctlsocket
///
/// Closed method cuts from ws2_32.cpp.

// IPV6_RECVPKTINFO / IPV6_RECVHOPLIMIT / IPV6_TCLASS are the RFC 3542 names.
#define __APPLE_USE_RFC_3542 1
#include <tuple>
#include <mutex>
#include <set>
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
#include <optional>

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
#include <ifaddrs.h>
#include <net/if.h>

namespace vitreo::winapi {

// FIONBIO / FIONREAD / SIOCATMARK, shared by ioctlsocket and WSAIoctl.
// Learn ioctlsocket + ws2_opts_probe.expected:
//  - FIONBIO=0 while a WSAEventSelect association with a nonzero event mask
//    exists fails WSAEINVAL (FIONBIO0_after_eventselect); FIONBIO=1 is fine;
//  - a NULL argp fails WSAEFAULT for all three (FIONBIO_null ...);
//  - SIOCATMARK reports TRUE when no out-of-band data is waiting
//    (SIOCATMARK_none v=1, SIOCATMARK_oob v=0);
//  - FIONREAD on a datagram socket counts every queued byte (FIONREAD_udp 30
//    for datagrams of 10 and 20).
u32 ws2_unix_ioctl(Ws2Manager& mgr, u32 guest_socket, int fd, u32 cmd, u32* inout) {
    constexpr u32 kFionbio = 0x8004667E, kFionread = 0x4004667F, kSiocatmark = 0x40047307;
    if (cmd == kFionbio) {
        if (*inout == 0) {
            const auto assoc = mgr.get_event_assoc(guest_socket);
            if (assoc.has_value() && assoc->event_mask != 0) return WSAEINVAL;
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) return Ws2Manager::errno_to_wsa(errno);
        const int want = *inout != 0 ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        if (::fcntl(fd, F_SETFL, want) < 0) return Ws2Manager::errno_to_wsa(errno);
        return 0;
    }
    if (cmd == kFionread) {
        int avail = 0;
        if (::ioctl(fd, FIONREAD, &avail) < 0) return Ws2Manager::errno_to_wsa(errno);
        *inout = static_cast<u32>(avail);
        return 0;
    }
    if (cmd == kSiocatmark) {
        int inline_oob = 0;
        socklen_t len = sizeof(inline_oob);
        (void)::getsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &inline_oob, &len);
        bool oob_waiting = false;
        if (inline_oob == 0) {
            char c = 0;
            oob_waiting = ::recv(fd, &c, 1, MSG_OOB | MSG_PEEK | MSG_DONTWAIT) == 1;
        } else {
            // Inline OOB: the only observable host state is "the next byte is
            // the mark", which is when the urgent byte is still unread.
            int at_mark = 0;
            (void)::ioctl(fd, SIOCATMARK, &at_mark);
            oob_waiting = at_mark != 0;
        }
        *inout = oob_waiting ? 0u : 1u;
        return 0;
    }
    return WSAEOPNOTSUPP;
}

void ws2::impl_ioctlsocket(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int ioctlsocket(SOCKET s, long cmd, u_long* argp) -- u_long is 32-bit on
    // both bitnesses. An unknown command fails WSAEOPNOTSUPP (unknown_cmd).
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const u32 cmd = static_cast<u32>(m.arg(1));
    const GuestAddr argp = m.arg_ptr(2);
    auto fail_with = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    const int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) return fail_with(WSAENOTSOCK);
    constexpr u32 kFionbio = 0x8004667E, kFionread = 0x4004667F, kSiocatmark = 0x40047307;
    if (cmd != kFionbio && cmd != kFionread && cmd != kSiocatmark) {
        return fail_with(WSAEOPNOTSUPP);
    }
    if (argp == 0) return fail_with(WSAEFAULT);
    u32 value = 0;
    if (cmd == kFionbio) {
        auto r = m.read_u32(argp);
        if (r.is_err()) return fail_with(WSAEFAULT);
        value = r.value();
    }
    if (const u32 err = ws2_unix_ioctl(mgr, guest_socket, fd, cmd, &value)) return fail_with(err);
    if (cmd != kFionbio && m.write_u32(argp, value).is_err()) return fail_with(WSAEFAULT);
    VITREO_TRACE("[ws2_32] ioctlsocket(0x%x, cmd=0x%x) -> 0 (value=%u)\n",
                 guest_socket, cmd, value);
    m.set_return(0);
}

// -----------------------------------------------------------------------------
// getsockopt / setsockopt
// -----------------------------------------------------------------------------
//
// Contract: Learn getsockopt / setsockopt and the SOL_SOCKET, IPPROTO_IP,
// IPPROTO_IPV6 and IPPROTO_TCP option pages, with everything Learn leaves open
// measured in tools/oracle_probe/ws2_opts_probe.expected:
//  - every option has a fixed guest width: 4 (DWORD/int), 1 (BOOLEAN: e.g.
//    SO_KEEPALIVE, SO_DONTROUTE, TCP_NODELAY -- "len=1") or a structure;
//    getsockopt writes that many bytes and sets *optlen to it; a larger
//    buffer is accepted, a smaller one gets the first *optlen bytes and fails
//    WSAEFAULT (SO_RCVBUF_len2) -- except IPPROTO_IP ints, which accept a
//    1-byte buffer (IP_TTL_len1), and IPPROTO_IPV6 ints, which fail WSAEFAULT
//    and zero *optlen (tcp6_IPV6_V6ONLY_len1);
//  - BOOL options read back as 0/1, never a host option bit;
//  - an option the level does not define -> WSAENOPROTOOPT; an unknown level,
//    or an option of the wrong protocol (TCP option on UDP, multicast on TCP)
//    -> WSAEINVAL;
//  - a successful getsockopt sets WSAGetLastError to 0 (SO_ERROR_clears_wsaerr,
//    SO_TYPE_keeps_wsaerr);
//  - option numbers are Windows numbers and are translated, never passed to
//    Darwin raw.
// Host defaults that differ from Windows (IP_TTL 64, TCP_KEEPCNT 8,
// TCP_KEEPINTVL 75) are set on the host socket at creation, so the reported
// value is also the one in effect (apply_windows_socket_defaults).

namespace {

constexpr int kWinSolSocket = 0xFFFF;
constexpr int kWinIpprotoIp = 0;
constexpr int kWinIpprotoIpv6 = 41;
constexpr int kWinIpprotoTcp = 6;
constexpr int kWinIpprotoUdp = 17;

constexpr u32 key(int level, int opt) {
    return (static_cast<u32>(level) << 16) | (static_cast<u32>(opt) & 0xFFFFu);
}

struct HostSocketInfo {
    int type = 0;       // host SOCK_*
    int family = 0;     // host AF_*
    bool connected = false;
    bool bound = false;
};

HostSocketInfo host_info(int fd) {
    HostSocketInfo info;
    socklen_t len = sizeof(info.type);
    (void)::getsockopt(fd, SOL_SOCKET, SO_TYPE, &info.type, &len);
    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0) {
        info.family = ss.ss_family;
        if (ss.ss_family == AF_INET) {
            info.bound = reinterpret_cast<sockaddr_in*>(&ss)->sin_port != 0;
        } else if (ss.ss_family == AF_INET6) {
            info.bound = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port != 0;
        }
    }
    sockaddr_storage peer{};
    socklen_t pl = sizeof(peer);
    info.connected = ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0;
    return info;
}

int host_int(int fd, int level, int opt, bool* ok) {
    int v = 0;
    socklen_t len = sizeof(v);
    unsigned char raw[16] = {};
    len = sizeof(raw);
    *ok = ::getsockopt(fd, level, opt, raw, &len) == 0;
    if (!*ok) return 0;
    if (len == 1) return raw[0];
    std::memcpy(&v, raw, std::min<socklen_t>(len, sizeof(v)));
    return v;
}

bool set_host_int(int fd, int level, int opt, int v) {
    return ::setsockopt(fd, level, opt, &v, sizeof(v)) == 0;
}

/// Winsock's SO_ERROR for a socket: the sticky connect failure if one was
/// observed (it is NOT cleared by reading -- ws2_core_probe refused_SO_ERROR /
/// refused_SO_ERROR_again both return WSAECONNREFUSED), else the host's
/// pending error translated to a WSA code (never a raw Darwin errno).
u32 current_socket_error(Ws2Manager& mgr, u32 guest_socket, int fd) {
    const auto st = mgr.socket_state(guest_socket);
    if (st.connect_error != 0) return st.connect_error;
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err == 0) return 0;
    // macOS reports EINPROGRESS until a nonblocking connect finishes.
    // Windows SO_ERROR stays 0 for that whole interval.
    if (st.connect_in_progress && err == EINPROGRESS) return 0;
    const u32 wsa = Ws2Manager::errno_to_wsa(err);
    // A nonblocking connect latches the failure for the life of the socket
    // (Windows keeps SO_ERROR sticky across getsockopt reads).
    if (st.connect_in_progress) {
        mgr.update_socket_state(guest_socket, [&](Ws2Manager::SocketState& s) {
            s.connect_error = wsa;
            s.connect_in_progress = false;
        });
    }
    return wsa;
}

/// WSAPROTOCOL_INFO{A,W} for a socket, values from ws2_core_probe
/// SO_PROTOCOL_INFOW* (MSAFD Tcpip provider).
void write_protocol_info(Marshaler& m, GuestAddr base, bool unicode, int win_af,
                         int host_type) {
    const u32 size = unicode ? 628u : 372u;
    std::vector<Byte> zero(size, 0);
    (void)m.memory().write_bytes(base, zero.data(), size);
    const bool stream = host_type == SOCK_STREAM;
    const bool v6 = win_af == kWinAfInet6;
    (void)m.write_u32(base + 0, stream ? 0x20066u : 0x20609u);  // dwServiceFlags1
    (void)m.write_u32(base + 16, 0x8u);                          // dwProviderFlags
    (void)m.write_u32(base + 40, 1);                             // ChainLen
    (void)m.write_u32(base + 72, 2);                             // iVersion
    (void)m.write_u32(base + 76, static_cast<u32>(win_af));
    (void)m.write_u32(base + 80, v6 ? 28u : 16u);                // iMaxSockAddr
    (void)m.write_u32(base + 84, v6 ? 28u : 16u);                // iMinSockAddr
    (void)m.write_u32(base + 88, stream ? 1u : 2u);
    (void)m.write_u32(base + 92, stream ? 6u : 17u);
    (void)m.write_u32(base + 108, stream ? 0u : 0xFFF7u);        // dwMessageSize
    std::string name = std::string("MSAFD Tcpip [") + (stream ? "TCP" : "UDP") +
                       (v6 ? "/IPv6]" : "/IP]");
    if (unicode) {
        (void)m.write_wide_string(base + 116, name);
    } else {
        (void)m.write_ansi_string(base + 116, name);
    }
}

enum class Width : u8 { Dword, Boolean };

struct OptResult {
    u32 error = 0;          // WSA error, 0 on success
    std::vector<Byte> bytes;
    u32 protocol_info = 0;  // nonzero: WSAPROTOCOL_INFO{A,W} of this size
    bool bsp_state = false; // SO_BSP_STATE: CSADDR_INFO + sockaddrs
};

OptResult ok_value(u32 v, Width w) {
    OptResult r;
    if (w == Width::Boolean) {
        r.bytes.push_back(static_cast<Byte>(v != 0 ? 1 : 0));
    } else {
        r.bytes.resize(4);
        std::memcpy(r.bytes.data(), &v, 4);
    }
    return r;
}
OptResult fail(u32 e) { OptResult r; r.error = e; return r; }

u32 emulated_get(const Ws2Manager::SocketState& st, u32 k, u32 dflt) {
    auto it = st.emulated.find(k);
    return it == st.emulated.end() ? dflt : it->second;
}

/// Emulated options: guest-visible state Windows keeps and Darwin has no
/// equivalent for. Values are ws2_opts_probe defaults.
struct EmulatedOpt {
    int level;
    int opt;
    Width width;
    u32 dflt;
    bool stream_only;
    bool dgram_only;
};
constexpr EmulatedOpt kEmulated[] = {
    {kWinSolSocket, 0x0001, Width::Dword, 0, false, false},    // SO_DEBUG
    {kWinSolSocket, 0x3002, Width::Dword, 0, false, false},    // SO_CONDITIONAL_ACCEPT
    {kWinSolSocket, 0x3003, Width::Boolean, 0, false, false},  // SO_PAUSE_ACCEPT
    {kWinSolSocket, 0x3005, Width::Dword, 0, false, false},    // SO_RANDOMIZE_PORT
    {kWinSolSocket, 0x3006, Width::Boolean, 0, false, false},  // SO_PORT_SCALABILITY
    {kWinIpprotoIp, 39, Width::Dword, 0, false, false},        // IP_TCLASS
    {kWinIpprotoIp, 51, Width::Dword, 0, false, false},        // IP_PKTINFO_EX
    {kWinIpprotoIp, 75, Width::Dword, 0, false, false},        // IP_RECVERR
    {kWinIpprotoIp, 76, Width::Dword, 0xFFFFFFFFu, false, false},  // IP_USER_MTU
    {kWinIpprotoIpv6, 23, Width::Dword, 10, false, false},     // IPV6_PROTECTION_LEVEL
    {kWinIpprotoIpv6, 75, Width::Dword, 0, false, false},      // IPV6_RECVERR
    {kWinIpprotoIpv6, 76, Width::Dword, 0xFFFFFFFFu, false, false},  // IPV6_USER_MTU
    {kWinIpprotoTcp, 2, Width::Boolean, 0, true, false},       // TCP_EXPEDITED_1122
    {kWinIpprotoTcp, 6, Width::Boolean, 0, true, false},       // TCP_STDURG
    {kWinIpprotoTcp, 7, Width::Boolean, 0, true, false},       // TCP_NOURG
    {kWinIpprotoTcp, 9, Width::Boolean, 0, true, false},       // TCP_NOSYNRETRIES
    {kWinIpprotoTcp, 10, Width::Boolean, 0, true, false},      // TCP_TIMESTAMPS
    {kWinIpprotoTcp, 12, Width::Dword, 0, true, false},        // TCP_CONGESTION_ALGORITHM
    {kWinIpprotoTcp, 13, Width::Boolean, 0, true, false},      // TCP_DELAY_FIN_ACK
    {kWinIpprotoTcp, 18, Width::Boolean, 0, true, false},      // TCP_FAIL_CONNECT_ON_ICMP_ERROR
    // TCP_FASTOPEN: Windows accepts it on any TCP socket (ws2_corex_probe
    // tfo_set_dword / tfo_set_listener) and reads it back as a BOOLEAN. Fast
    // Open only changes whether ConnectEx data may ride in a SYN when the
    // server already issued a cookie; a connection that does not use it is
    // exactly what Windows does on a first contact or without server
    // support, so recording the option is the complete observable contract.
    {kWinIpprotoTcp, 15, Width::Boolean, 0, true, false},      // TCP_FASTOPEN
    {kWinIpprotoUdp, 1, Width::Boolean, 0, false, true},       // UDP_NOCHECKSUM
};

/// Host-backed options: Windows (level, optname) -> Darwin (level, optname).
struct HostOpt {
    int level;
    int opt;
    int host_level;
    int host_opt;
    Width width;
    bool is_bool;
    bool stream_only;
    bool dgram_only;
};
// The WSARecvMsg receive-control options (IP_PKTINFO, IP_RECVTOS, ...) are
// not here: they are per-socket state translated by ws2_msg_control.cpp.
const HostOpt kHostOpts[] = {
    {kWinSolSocket, 0x0008, SOL_SOCKET, SO_KEEPALIVE, Width::Boolean, true, false, false},
    {kWinSolSocket, 0x0010, SOL_SOCKET, SO_DONTROUTE, Width::Boolean, true, false, false},
    {kWinSolSocket, 0x0020, SOL_SOCKET, SO_BROADCAST, Width::Dword, true, false, true},
    {kWinSolSocket, 0x0100, SOL_SOCKET, SO_OOBINLINE, Width::Dword, true, false, false},
    {kWinSolSocket, 0x0002, SOL_SOCKET, SO_ACCEPTCONN, Width::Dword, true, true, false},
    {kWinIpprotoIp, 3, IPPROTO_IP, IP_TOS, Width::Dword, false, false, false},
    {kWinIpprotoIp, 4, IPPROTO_IP, IP_TTL, Width::Dword, false, false, false},
    {kWinIpprotoIp, 9, IPPROTO_IP, IP_MULTICAST_IF, Width::Dword, false, false, true},
    {kWinIpprotoIp, 10, IPPROTO_IP, IP_MULTICAST_TTL, Width::Dword, false, false, true},
    {kWinIpprotoIp, 11, IPPROTO_IP, IP_MULTICAST_LOOP, Width::Dword, true, false, true},
    {kWinIpprotoIpv6, 4, IPPROTO_IPV6, IPV6_UNICAST_HOPS, Width::Dword, false, false, false},
    {kWinIpprotoIpv6, 9, IPPROTO_IPV6, IPV6_MULTICAST_IF, Width::Dword, false, false, true},
    {kWinIpprotoIpv6, 10, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, Width::Dword, false, false, true},
    {kWinIpprotoIpv6, 11, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, Width::Dword, true, false, true},
    {kWinIpprotoIpv6, 14, IPPROTO_IPV6, IPV6_DONTFRAG, Width::Dword, true, false, false},
    {kWinIpprotoIpv6, 39, IPPROTO_IPV6, IPV6_TCLASS, Width::Dword, false, false, false},
    {kWinIpprotoTcp, 0x0001, IPPROTO_TCP, TCP_NODELAY, Width::Boolean, true, true, false},
    {kWinIpprotoTcp, 3, IPPROTO_TCP, TCP_KEEPALIVE, Width::Dword, false, true, false},
    {kWinIpprotoTcp, 16, IPPROTO_TCP, TCP_KEEPCNT, Width::Dword, false, true, false},
    {kWinIpprotoTcp, 17, IPPROTO_TCP, TCP_KEEPINTVL, Width::Dword, false, true, false},
};

bool level_known(int level) {
    return level == kWinSolSocket || level == kWinIpprotoIp ||
           level == kWinIpprotoIpv6 || level == kWinIpprotoTcp ||
           level == kWinIpprotoUdp;
}

/// Protocol applicability. Returns 0 or the WSA error for a wrong-protocol use.
u32 check_protocol(int level, bool stream_only, bool dgram_only,
                   const HostSocketInfo& info) {
    const bool stream = info.type == SOCK_STREAM;
    if (level == kWinIpprotoTcp && !stream) return WSAEINVAL;       // udp_TCP_NODELAY
    if (level == kWinIpprotoUdp && info.type != SOCK_DGRAM) return WSAEINVAL;
    if (stream_only && !stream) {
        return level == kWinSolSocket ? WSAENOPROTOOPT : WSAEINVAL; // udp_SO_ACCEPTCONN
    }
    if (dgram_only && stream) {
        return level == kWinSolSocket ? WSAENOPROTOOPT : WSAEINVAL; // tcp_SO_BROADCAST
    }
    return 0;
}

/// Path MTU of a connected socket: the MTU of the interface its local address
/// is on (IP_MTU / IPV6_MTU).
int path_mtu(int fd) {
    sockaddr_storage local{};
    socklen_t ll = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &ll) != 0) return 0;
    struct ifaddrs* ifs = nullptr;
    int mtu = 0;
    if (::getifaddrs(&ifs) != 0) return 0;
    for (const struct ifaddrs* i = ifs; i != nullptr && mtu == 0; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != local.ss_family) continue;
        bool same = false;
        if (local.ss_family == AF_INET) {
            same = reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr ==
                   reinterpret_cast<const sockaddr_in*>(&local)->sin_addr.s_addr;
        } else {
            same = std::memcmp(&reinterpret_cast<const sockaddr_in6*>(i->ifa_addr)->sin6_addr,
                               &reinterpret_cast<const sockaddr_in6*>(&local)->sin6_addr,
                               16) == 0;
        }
        if (!same) continue;
        // Windows' loopback pseudo-interface reports the IP maximum
        // (ws2_opts_probe conn_IP_MTU, ws2_corex_probe acc4_IP_MTU /
        // conn6_IPV6_MTU / udpconn_IP_MTU: 0xffff); Darwin's lo0 is 16384.
        if ((i->ifa_flags & IFF_LOOPBACK) != 0) {
            mtu = 0xFFFF;
            break;
        }
        struct ifreq req{};
        std::strncpy(req.ifr_name, i->ifa_name, sizeof(req.ifr_name) - 1);
        if (::ioctl(fd, SIOCGIFMTU, &req) == 0) mtu = req.ifr_mtu;
    }
    ::freeifaddrs(ifs);
    return mtu;
}

/// True when the connection's local address is a loopback address.
bool local_is_loopback(int fd) {
    sockaddr_storage local{};
    socklen_t ll = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &ll) != 0) return false;
    if (local.ss_family == AF_INET) {
        return (ntohl(reinterpret_cast<const sockaddr_in*>(&local)->sin_addr.s_addr) >> 24) == 127;
    }
    if (local.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6*>(&local)->sin6_addr;
        if (IN6_IS_ADDR_LOOPBACK(&a)) return true;
        return IN6_IS_ADDR_V4MAPPED(&a) && a.s6_addr[12] == 127;
    }
    return false;
}

/// TCP_MAXSEG of a connected socket as Windows reports it: the negotiated
/// MSS without TCP options (Windows does not use timestamps by default).
/// Over loopback that is the 65535-byte IP maximum less the IPv4/IPv6 and
/// TCP headers (ws2_opts_probe conn_TCP_MAXSEG 0xffd7, ws2_corex_probe
/// conn6_TCP_MAXSEG 0xffc3); elsewhere the host's segment size plus the
/// timestamp option it reserves.
std::optional<u32> windows_tcp_maxseg(int fd, int family) {
    if (local_is_loopback(fd)) {
        sockaddr_storage peer{};
        socklen_t pl = sizeof(peer);
        const bool v4_path =
            family == AF_INET ||
            (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0 &&
             peer.ss_family == AF_INET6 &&
             IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr));
        return v4_path ? 0xFFFFu - 40u : 0xFFFFu - 60u;
    }
    bool ok = false;
    int v = host_int(fd, IPPROTO_TCP, TCP_MAXSEG, &ok);
    if (!ok) return std::nullopt;
    struct tcp_connection_info ci{};
    socklen_t cl = sizeof(ci);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &ci, &cl) == 0 &&
        (ci.tcpi_options & TCPCI_OPT_TIMESTAMPS) != 0) {
        v += 12;  // TCPOLEN_TSTAMP_APPA
    }
    return static_cast<u32>(v);
}

OptResult get_option(Ws2Manager& mgr, u32 s, int fd, int level, int opt) {
    if (!level_known(level)) return fail(WSAEINVAL);
    const HostSocketInfo info = host_info(fd);
    const bool stream = info.type == SOCK_STREAM;
    const auto st = mgr.socket_state(s);
    // WSARecvMsg control options read back as a normalised BOOL
    // (ws2_msg_probe x_v4sock_get_IP_RECVTOS: set 5, get 1).
    if (const u32 bit = ws2_recv_ctl_bit(level, opt); bit != 0) {
        return ok_value((st.recv_ctl & bit) != 0 ? 1u : 0u, Width::Dword);
    }
    for (const auto& e : kEmulated) {
        if (e.level != level || e.opt != opt) continue;
        if (u32 err = check_protocol(level, e.stream_only, e.dgram_only, info)) return fail(err);
        return ok_value(emulated_get(st, key(level, opt), e.dflt), e.width);
    }
    // SO_ACCEPTCONN: Darwin's getsockopt does not answer it; the listen()
    // record does (ws2_opts_probe tcp/listen/udp_SO_ACCEPTCONN).
    if (level == kWinSolSocket && opt == 0x0002) {
        if (!stream) return fail(WSAENOPROTOOPT);
        return ok_value(mgr.is_listening(s) ? 1u : 0u, Width::Dword);
    }
    if (level == kWinSolSocket && opt == 0x1009) {           // SO_BSP_STATE
        OptResult r;
        r.bsp_state = true;
        return r;
    }
    // IPv4-level options on an AF_INET6 socket describe its IPv4-mapped
    // traffic. Darwin refuses IPPROTO_IP on AF_INET6 sockets, so Winsock's
    // values are kept here (ws2_opts_probe tcp6_IP_TTL, ws2_corex_probe
    // only6_* / dual6_*: TTL 128, TOS 0, multicast TTL 1 on a datagram).
    if (level == kWinIpprotoIp && info.family == AF_INET6) {
        switch (opt) {
        case 4: return ok_value(emulated_get(st, key(level, opt), 128), Width::Dword);
        case 3: return ok_value(0, Width::Dword);
        case 10:
            if (stream) return fail(WSAEINVAL);
            return ok_value(1, Width::Dword);
        default: break;
        }
    }
    for (const auto& h : kHostOpts) {
        if (h.level != level || h.opt != opt) continue;
        if (u32 err = check_protocol(level, h.stream_only, h.dgram_only, info)) return fail(err);
        bool ok = false;
        int v = host_int(fd, h.host_level, h.host_opt, &ok);
        if (!ok) return fail(Ws2Manager::errno_to_wsa(errno));
        if (h.is_bool) v = v != 0 ? 1 : 0;
        return ok_value(static_cast<u32>(v), h.width);
    }
    if (level == kWinSolSocket) {
        switch (opt) {
        case 0x0004: return ok_value(st.reuseaddr ? 1 : 0, Width::Dword);
        case ~0x0004: return ok_value(st.exclusiveaddruse ? 1 : 0, Width::Dword);
        case 0x0080:                                        // SO_LINGER
        case ~0x0080: {                                     // SO_DONTLINGER
            if (!stream) return fail(WSAENOPROTOOPT);
            if (opt == 0x0080) {
                return ok_value(static_cast<u32>(st.linger_onoff) |
                                    (static_cast<u32>(st.linger_seconds) << 16),
                                Width::Dword);
            }
            return ok_value(st.linger_onoff == 0 ? 1 : 0, Width::Dword);
        }
        case 0x1001: return ok_value(st.sndbuf, Width::Dword);
        case 0x1002: return ok_value(st.rcvbuf, Width::Dword);
        case 0x1005: return ok_value(st.sndtimeo_ms, Width::Dword);
        case 0x1006: return ok_value(st.rcvtimeo_ms, Width::Dword);
        case 0x1007: return ok_value(current_socket_error(mgr, s, fd), Width::Dword);
        case 0x1008: return ok_value(stream ? 1u : info.type == SOCK_DGRAM ? 2u : 3u,
                                     Width::Dword);
        case 0x2001: case 0x2002: return ok_value(0, Width::Dword);  // group id/priority
        case 0x2003:                                        // SO_MAX_MSG_SIZE
            return ok_value(stream ? 0xFFFFFFFFu : 0xFFE3u, Width::Dword);
        case 0x2004: case 0x2005: {                         // SO_PROTOCOL_INFOA/W
            OptResult r;
            r.protocol_info = opt == 0x2005 ? 628u : 372u;
            return r;
        }
        case 0x3004: return ok_value(1, Width::Dword);      // SO_COMPARTMENT_ID
        case 0x3007: return ok_value(0, Width::Boolean);    // SO_REUSE_UNICASTPORT
        case 0x3008:                                        // SO_REUSE_MULTICASTPORT
            return fail(stream ? WSAENOPROTOOPT : WSAEINVAL);
        case 0x700C: {                                      // SO_CONNECT_TIME
            if (!info.connected) return ok_value(0xFFFFFFFFu, Width::Dword);
            i64 t0 = st.connect_time_ms;
            const i64 now = static_cast<i64>(now_steady_ns() / 1000000ull);
            if (t0 < 0) {
                mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) {
                    if (x.connect_time_ms < 0) x.connect_time_ms = now;
                });
                t0 = now;
            }
            return ok_value(static_cast<u32>((now - t0) / 1000), Width::Dword);
        }
        case 0x7008: return ok_value(0, Width::Dword);      // SO_OPENTYPE
        case 0x7009: case 0x700A:                           // SO_MAXDG / SO_MAXPATHDG
            return ok_value(stream ? 0xFFFFFFFFu : 0xFFE3u, Width::Dword);
        default: return fail(WSAENOPROTOOPT);
        }
    }
    if (level == kWinIpprotoIp) {
        switch (opt) {
        case 1: return OptResult{};                         // IP_OPTIONS: none set
        case 2: return fail(WSAEINVAL);                     // IP_HDRINCL: raw only
        case 14: {                                          // IP_DONTFRAGMENT
            if (stream) return ok_value(1, Width::Dword);   // tcp_IP_DONTFRAGMENT
            bool ok = false;
            const int v = host_int(fd, IPPROTO_IP, IP_DONTFRAG, &ok);
            return ok ? ok_value(v != 0, Width::Dword) : fail(Ws2Manager::errno_to_wsa(errno));
        }
        case 22: return ok_value(emulated_get(st, key(level, opt), 1), Width::Dword);
        case 31: {                                          // IP_UNICAST_IF (net order)
            bool ok = false;
            const int v = host_int(fd, IPPROTO_IP, IP_BOUND_IF, &ok);
            return ok_value(htonl(static_cast<u32>(ok ? v : 0)), Width::Dword);
        }
        case 71: return ok_value(0, Width::Dword);          // IP_MTU_DISCOVER not set
        case 73: {                                          // IP_MTU
            if (!info.connected) return fail(WSAENOTCONN);
            return ok_value(static_cast<u32>(path_mtu(fd)), Width::Dword);
        }
        default: return fail(WSAENOPROTOOPT);
        }
    }
    if (level == kWinIpprotoIpv6) {
        switch (opt) {
        case 27: {                                          // IPV6_V6ONLY
            // udp6_IPV6_V6ONLY len=1, tcp6_IPV6_V6ONLY len=4; an AF_INET
            // socket reports TRUE (udp4_IPV6_V6ONLY v=01 len=1).
            bool ok = false;
            const int v = info.family == AF_INET6
                              ? host_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, &ok) : 1;
            return ok_value(v != 0 ? 1 : 0, stream ? Width::Dword : Width::Boolean);
        }
        case 31: {                                          // IPV6_UNICAST_IF (host order)
            bool ok = false;
            const int v = host_int(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &ok);
            return ok_value(static_cast<u32>(ok ? v : 0), Width::Dword);
        }
        case 71: return ok_value(0, Width::Dword);          // IPV6_MTU_DISCOVER
        case 72: {                                          // IPV6_MTU
            if (!info.connected) return fail(WSAENOTCONN);
            return ok_value(static_cast<u32>(path_mtu(fd)), Width::Dword);
        }
        default: return fail(WSAENOPROTOOPT);
        }
    }
    if (level == kWinIpprotoTcp) {
        if (!stream) return fail(WSAEINVAL);
        switch (opt) {
        case 4: {                                           // TCP_MAXSEG
            if (!info.connected) return fail(WSAENOPROTOOPT);
            const auto v = windows_tcp_maxseg(fd, info.family);
            return v ? ok_value(*v, Width::Dword) : fail(WSAENOPROTOOPT);
        }
        case 5: return ok_value(emulated_get(st, key(level, 14), 0) / 1000, Width::Dword);
        case 14: return ok_value(emulated_get(st, key(level, 14), 0), Width::Dword);
        case 8: {                                           // TCP_ATMARK
            int at = 0;
            (void)::ioctl(fd, SIOCATMARK, &at);
            return ok_value(at != 0, Width::Boolean);
        }
        case 19: return fail(WSAEFAULT);                    // TCP_ICMP_ERROR_INFO: struct
        default: return fail(WSAENOPROTOOPT);
        }
    }
    return fail(WSAENOPROTOOPT);
}


/// Options Windows accepts but whose effect Vitreo cannot provide: enabling one
/// fails loudly rather than pretending (brief: no fake success).
bool unimplemented_enable(int level, int opt) {
    // IP_PKTINFO_EX / IPV6_PKTINFO_EX carry a SCOPE_ID whose value for
    // non-loopback addresses has not been measured; IP_RECVERR has no host
    // equivalent at all.
    return (level == kWinIpprotoIp && (opt == 51 || opt == 75)) ||
           (level == kWinIpprotoIpv6 && (opt == 51 || opt == 75));
}

u32 set_option(Ws2Manager& mgr, u32 s, int fd, int level, int opt,
               const std::vector<Byte>& val, bool is64) {
    if (!level_known(level)) return WSAEINVAL;
    const HostSocketInfo info = host_info(fd);
    const bool stream = info.type == SOCK_STREAM;
    const int len = static_cast<int>(val.size());
    auto as_u32 = [&](u32* out) -> bool {
        if (len < 4) return false;
        std::memcpy(out, val.data(), 4);
        return true;
    };
    // BOOLEAN-width options take a single byte (SO_KEEPALIVE_len1); DWORD
    // ones need four (SO_RCVBUF_len2 / SO_RCVTIMEO_len2 -> WSAEFAULT).
    auto as_value = [&](Width w, u32* out) -> bool {
        if (w == Width::Boolean && len >= 1 && len < 4) {
            *out = val[0];
            return true;
        }
        return as_u32(out);
    };
    auto store = [&](int lv, int op, u32 v) {
        mgr.update_socket_state(s, [&](Ws2Manager::SocketState& st) {
            st.emulated[key(lv, op)] = v;
        });
    };
    auto host_set = [&](int hl, int ho, int v) -> u32 {
        return set_host_int(fd, hl, ho, v) ? 0u : Ws2Manager::errno_to_wsa(errno);
    };

    // WSARecvMsg control options (IP_PKTINFO, IP_RECVTOS, IP_RECVECN, ...).
    // Measured by ws2_msg_probe x_*: stream sockets refuse them (WSAEINVAL),
    // an IPv6 socket in V6ONLY mode refuses the IP-level ones, an IPv4 socket
    // accepts (and stores) the IPv6-level ones, and a short optlen is WSAEFAULT.
    if (const u32 bit = ws2_recv_ctl_bit(level, opt); bit != 0) {
        u32 v = 0;
        if (!as_u32(&v)) return WSAEFAULT;
        if (stream) return WSAEINVAL;
        if (level == kWinIpprotoIp && info.family == AF_INET6) {
            int v6only = 0;
            socklen_t vl = sizeof(v6only);
            if (::getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &vl) == 0 &&
                v6only != 0) {
                return WSAEINVAL;
            }
        }
        // Darwin reports no hop limit for v4-mapped datagrams on an AF_INET6
        // socket (IP_RECVTTL is refused there, IPV6_RECVHOPLIMIT stays silent),
        // so IP_HOPLIMIT on a dual-stack socket cannot deliver its message.
        if (bit == ws2ctl::kV4Hoplimit && info.family == AF_INET6 && v != 0) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED ws2_32!setsockopt: IP_HOPLIMIT on a "
                             "dual-stack AF_INET6 socket (no host TTL for v4-mapped data)\n");
            }
            return WSAENOPROTOOPT;
        }
        const u32 mask = mgr.socket_state(s).recv_ctl;
        const u32 next = v != 0 ? (mask | bit) : (mask & ~bit);
        if (const u32 err = ws2_apply_recv_ctl_host(fd, info.family, next)) return err;
        mgr.update_socket_state(s, [next](Ws2Manager::SocketState& st) {
            st.recv_ctl = next;
        });
        return 0;
    }
    // Checked before the store-only table: enabling one of these would
    // otherwise be stored and silently never delivered.
    if (unimplemented_enable(level, opt)) {
        u32 v = 0;
        if (!as_u32(&v)) return WSAEFAULT;
        if (v != 0) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ws2_32!setsockopt: level=%d optname=%d "
                         "enabled (no host equivalent for its ancillary data)\n",
                         level, opt);
            return WSAENOPROTOOPT;
        }
        store(level, opt, 0);
        return 0;
    }
    for (const auto& e : kEmulated) {
        if (e.level != level || e.opt != opt) continue;
        if (u32 err = check_protocol(level, e.stream_only, e.dgram_only, info)) return err;
        u32 v = 0;
        if (!as_value(e.width, &v)) return WSAEFAULT;
        store(level, opt, e.width == Width::Boolean || opt == 0x0001 || opt == 0x3002 ||
                              opt == 0x3005
                              ? (v != 0 ? 1u : 0u) : v);
        return 0;
    }
    // IP_TTL on an AF_INET6 socket (ws2_corex_probe only6_set_IP_TTL /
    // dual6_set_IP_TTL): refused in V6ONLY mode, where no IPv4 traffic
    // exists; on a dual-stack socket it sets the TTL of IPv4-mapped traffic,
    // which Darwin gives no way to set on an AF_INET6 socket.
    if (level == kWinIpprotoIp && opt == 4 && info.family == AF_INET6) {
        u32 v = 0;
        if (!as_u32(&v)) return WSAEFAULT;
        int v6only = 1;
        socklen_t vl = sizeof(v6only);
        (void)::getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &vl);
        if (v6only != 0) return WSAEINVAL;
        if (v > 255) return WSAEINVAL;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ws2_32!setsockopt: IP_TTL on a dual-stack "
                         "AF_INET6 socket (no host control of IPv4-mapped TTL)\n");
        }
        return WSAENOPROTOOPT;
    }
    for (const auto& h : kHostOpts) {
        if (h.level != level || h.opt != opt) continue;
        if (u32 err = check_protocol(level, h.stream_only, h.dgram_only, info)) return err;
        if (level == kWinSolSocket && opt == 0x0002) return WSAENOPROTOOPT;  // SO_ACCEPTCONN
        u32 v = 0;
        if (!as_value(h.width, &v)) return WSAEFAULT;
        if (level == kWinIpprotoIp && (opt == 4 || opt == 10) && v > 255) return WSAEINVAL;
        if (level == kWinIpprotoTcp && opt == 16 && v > 255) return WSAEINVAL;  // KEEPCNT
        int hv = static_cast<int>(v);
        if (h.is_bool) hv = v != 0 ? 1 : 0;
        if (level == kWinIpprotoIp && (opt == 10 || opt == 11)) {
            const u_char b = static_cast<u_char>(hv);   // Darwin takes a u_char
            return ::setsockopt(fd, IPPROTO_IP, h.host_opt, &b, sizeof(b)) == 0
                       ? 0u : Ws2Manager::errno_to_wsa(errno);
        }
        return host_set(h.host_level, h.host_opt, hv);
    }
    if (level == kWinSolSocket) {
        u32 v = 0;
        switch (opt) {
        case 0x0004: {                                      // SO_REUSEADDR
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            const auto st = mgr.socket_state(s);
            if (v != 0 && st.exclusiveaddruse) return WSAEINVAL;
            // Windows SO_REUSEADDR permits duplicate binds (rebind first=1
            // second=1 rc=0); Darwin needs SO_REUSEPORT for that.
            if (u32 e = host_set(SOL_SOCKET, SO_REUSEADDR, v != 0)) return e;
            if (u32 e = host_set(SOL_SOCKET, SO_REUSEPORT, v != 0)) return e;
            mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) { x.reuseaddr = v != 0; });
            return 0;
        }
        case ~0x0004: {                                     // SO_EXCLUSIVEADDRUSE
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            const auto st = mgr.socket_state(s);
            if (v != 0 && st.reuseaddr) return WSAEINVAL;   // set_SO_EXCL_after_reuse
            if (v != 0) {
                (void)host_set(SOL_SOCKET, SO_REUSEADDR, 0);
                (void)host_set(SOL_SOCKET, SO_REUSEPORT, 0);
            }
            mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) {
                x.exclusiveaddruse = v != 0;
            });
            return 0;
        }
        case 0x0080: {                                      // SO_LINGER {u_short, u_short}
            if (!stream) return WSAENOPROTOOPT;
            if (len < 4) return WSAEFAULT;
            u16 onoff = 0, secs = 0;
            std::memcpy(&onoff, val.data(), 2);
            std::memcpy(&secs, val.data() + 2, 2);
            // Darwin SO_LINGER counts ticks; SO_LINGER_SEC counts seconds.
            struct linger l{onoff != 0 ? 1 : 0, secs};
            if (::setsockopt(fd, SOL_SOCKET, SO_LINGER_SEC, &l, sizeof(l)) != 0) {
                return Ws2Manager::errno_to_wsa(errno);
            }
            mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) {
                x.linger_onoff = onoff;
                x.linger_seconds = secs;
            });
            return 0;
        }
        case ~0x0080: {                                     // SO_DONTLINGER
            if (!stream) return WSAENOPROTOOPT;
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            const auto st = mgr.socket_state(s);
            const u16 onoff = v != 0 ? 0 : 1;               // keeps l_linger
            struct linger l{onoff, st.linger_seconds};
            if (::setsockopt(fd, SOL_SOCKET, SO_LINGER_SEC, &l, sizeof(l)) != 0) {
                return Ws2Manager::errno_to_wsa(errno);
            }
            mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) { x.linger_onoff = onoff; });
            return 0;
        }
        case 0x1001: case 0x1002: {                         // SO_SNDBUF / SO_RCVBUF
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            // SO_SNDBUF=0 disables transport buffering; Darwin rejects 0 and
            // Vitreo's send paths already hold the guest data until sent.
            if (v != 0) {
                if (u32 e = host_set(SOL_SOCKET, opt == 0x1001 ? SO_SNDBUF : SO_RCVBUF,
                                     static_cast<int>(std::min<u32>(v, 0x7FFFFFFF)))) {
                    return e;
                }
            }
            mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) {
                (opt == 0x1001 ? x.sndbuf : x.rcvbuf) = v;
            });
            return 0;
        }
        case 0x1005: case 0x1006: {                         // SO_SNDTIMEO / SO_RCVTIMEO
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            // Learn: DWORD milliseconds. Darwin takes a timeval and rejects the
            // set (EINVAL) on a socket that already has a sticky connect error;
            // Windows still accepts the timeout (ws2_opts_probe). Store the
            // guest value always; apply to the host best-effort.
            struct timeval tv{};
            tv.tv_sec = static_cast<long>(v / 1000);
            tv.tv_usec = static_cast<int>((v % 1000) * 1000);
            (void)::setsockopt(fd, SOL_SOCKET, opt == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO,
                               &tv, sizeof(tv));
            mgr.update_socket_state(s, [&](Ws2Manager::SocketState& x) {
                (opt == 0x1005 ? x.sndtimeo_ms : x.rcvtimeo_ms) = v;
            });
            return 0;
        }
        case 0x1007: return 0;                              // SO_ERROR: accepted, no effect
        case 0x3007: return len >= 1 ? 0u : WSAEFAULT;      // SO_REUSE_UNICASTPORT
        case 0x3008:                                        // SO_REUSE_MULTICASTPORT
            if (stream) return WSAENOPROTOOPT;
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            return host_set(SOL_SOCKET, SO_REUSEPORT, v != 0);
        case 0x700B: {                                      // SO_UPDATE_ACCEPT_CONTEXT
            // optval is the listening SOCKET (pointer-width).
            const int want = is64 ? 8 : 4;
            if (len < want) return WSAEFAULT;
            u64 listen_sock = 0;
            std::memcpy(&listen_sock, val.data(), static_cast<usize>(want));
            if ((listen_sock >> 32) != 0 ||
                mgr.get_native_fd(static_cast<u32>(listen_sock)) < 0) {
                return WSAENOTSOCK;
            }
            mgr.inherit_listener_state(static_cast<u32>(listen_sock), s);
            return 0;
        }
        case 0x7010:                                        // SO_UPDATE_CONNECT_CONTEXT
            return info.connected ? 0u : WSAENOTCONN;
        default: return WSAENOPROTOOPT;
        }
    }
    if (level == kWinIpprotoIp) {
        u32 v = 0;
        switch (opt) {
        case 2: return WSAEINVAL;                           // IP_HDRINCL: raw only
        case 12: case 13: {                                 // IP_ADD/DROP_MEMBERSHIP
            if (stream) return WSAEINVAL;
            if (len < 8) return WSAEFAULT;
            return ::setsockopt(fd, IPPROTO_IP, opt == 12 ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                                val.data(), 8) == 0 ? 0u : Ws2Manager::errno_to_wsa(errno);
        }
        case 15: case 16: case 17: case 18: {               // source-specific multicast
            if (stream) return WSAEINVAL;
            if (len < 12) return WSAEFAULT;
            static constexpr int kHost[] = {IP_ADD_SOURCE_MEMBERSHIP, IP_DROP_SOURCE_MEMBERSHIP,
                                            IP_BLOCK_SOURCE, IP_UNBLOCK_SOURCE};
            return ::setsockopt(fd, IPPROTO_IP, kHost[opt - 15], val.data(), 12) == 0
                       ? 0u : Ws2Manager::errno_to_wsa(errno);
        }
        case 14: {                                          // IP_DONTFRAGMENT
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            if (stream) return 0;                           // TCP always sets DF
            return host_set(IPPROTO_IP, IP_DONTFRAG, v != 0);
        }
        case 22: {                                          // IP_RECEIVE_BROADCAST
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            if (v == 0) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ws2_32!setsockopt: "
                                     "IP_RECEIVE_BROADCAST=FALSE (host always delivers)\n");
                return WSAENOPROTOOPT;
            }
            store(level, opt, 1);
            return 0;
        }
        case 31:                                            // IP_UNICAST_IF (net order)
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            return host_set(IPPROTO_IP, IP_BOUND_IF, static_cast<int>(ntohl(v)));
        case 71: return WSAEINVAL;                          // IP_MTU_DISCOVER (measured)
        default: return WSAENOPROTOOPT;
        }
    }
    if (level == kWinIpprotoIpv6) {
        u32 v = 0;
        switch (opt) {
        case 12: case 13: {                                 // IPV6_ADD/DROP_MEMBERSHIP
            if (stream) return WSAEINVAL;
            if (len < 20) return WSAEFAULT;
            return ::setsockopt(fd, IPPROTO_IPV6, opt == 12 ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                                val.data(), 20) == 0 ? 0u : Ws2Manager::errno_to_wsa(errno);
        }
        case 27:                                            // IPV6_V6ONLY
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            if (info.family != AF_INET6) return WSAEINVAL;
            if (info.bound) return WSAEINVAL;               // set_IPV6_V6ONLY_after_bind
            return host_set(IPPROTO_IPV6, IPV6_V6ONLY, v != 0);
        case 31:                                            // IPV6_UNICAST_IF (host order)
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            return host_set(IPPROTO_IPV6, IPV6_BOUND_IF, static_cast<int>(v));
        case 71: return WSAEINVAL;                          // IPV6_MTU_DISCOVER
        default: return WSAENOPROTOOPT;
        }
    }
    if (level == kWinIpprotoTcp) {
        if (!stream) return WSAEINVAL;
        u32 v = 0;
        switch (opt) {
        case 4: return WSAENOPROTOOPT;                      // TCP_MAXSEG is read-only
        case 5: case 14: {                                  // TCP_MAXRT (s) / TCP_MAXRTMS (ms)
            if (!as_value(Width::Dword, &v)) return WSAEFAULT;
            const u32 ms = opt == 5 ? v * 1000u : v;
            if (u32 e = host_set(IPPROTO_TCP, TCP_RXT_CONNDROPTIME,
                                 static_cast<int>((ms + 999) / 1000))) {
                return e;
            }
            store(level, 14, ms);
            return 0;
        }
        default: return WSAENOPROTOOPT;
        }
    }
    return WSAENOPROTOOPT;
}

}  // namespace

bool ws2_write_socket_protocol_info(Marshaler& m, GuestAddr base, bool unicode, int fd) {
    const HostSocketInfo info = host_info(fd);
    if (info.type != SOCK_STREAM && info.type != SOCK_DGRAM) return false;
    write_protocol_info(m, base, unicode, info.family == AF_INET6 ? kWinAfInet6 : kWinAfInet,
                        info.type);
    return true;
}

u32 ws2_io_error(Ws2Manager& mgr, u32 guest_socket, int fd, int err, bool is_send) {
    if (err == EAGAIN || err == EWOULDBLOCK) {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0 && (fl & O_NONBLOCK) == 0) {
            const auto st = mgr.socket_state(guest_socket);
            if ((is_send ? st.sndtimeo_ms : st.rcvtimeo_ms) != 0) return WSAETIMEDOUT;
        }
    }
    return Ws2Manager::errno_to_wsa(err);
}

void apply_windows_socket_defaults(int fd, int host_af, int host_type) {
    // Windows defaults that the host socket would otherwise not share
    // (ws2_opts_probe: tcp_IP_TTL 0x80, tcp6_IPV6_UNICAST_HOPS 0x80,
    // tcp_TCP_KEEPCNT 10, tcp_TCP_KEEPINTVL 1).
    if (host_af == AF_INET) (void)set_host_int(fd, IPPROTO_IP, IP_TTL, 128);
    if (host_af == AF_INET6) (void)set_host_int(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, 128);
    if (host_type == SOCK_STREAM && (host_af == AF_INET || host_af == AF_INET6)) {
        (void)set_host_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 10);
        (void)set_host_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, 1);
    }
}

void ws2::impl_getsockopt(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int getsockopt(SOCKET s, int level, int optname, char* optval, int* optlen)
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const int level = static_cast<int>(static_cast<u32>(m.arg(1)));
    const int optname = static_cast<int>(static_cast<u32>(m.arg(2)));
    const GuestAddr optval_ptr = m.arg_ptr(3);
    const GuestAddr optlen_ptr = m.arg_ptr(4);
    auto fail_with = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    const int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) return fail_with(WSAENOTSOCK);
    if (optval_ptr == 0 || optlen_ptr == 0) return fail_with(WSAEFAULT);
    const auto optlen_r = m.read_u32(optlen_ptr);
    if (optlen_r.is_err()) return fail_with(WSAEFAULT);
    const int optlen = static_cast<int>(optlen_r.value());

    OptResult r = get_option(mgr, guest_socket, fd, level, optname);
    if (r.error != 0) return fail_with(r.error);

    if (r.bsp_state) {
        // SO_BSP_STATE (ws2_corex_probe bsp_*): a CSADDR_INFO followed by the
        // local and remote sockaddrs it points at; an unbound / unconnected
        // side is NULL with length 0; *optlen is left as passed. A socket
        // bound implicitly by connect reports the wildcard address it was
        // bound to (bsp_tcp_connected l_addr=0x0). Too small a buffer for the
        // struct and both addresses is WSAEFAULT (bsp_exact_struct).
        const bool is64 = m.cpu_is_64bit();
        const u32 csaddr = is64 ? 40u : 24u;
        const u32 psz = is64 ? 8u : 4u;
        const HostSocketInfo hi = host_info(fd);
        const u32 sa_len = hi.family == AF_INET6 ? static_cast<u32>(kWinSockaddrIn6Bytes) : 16u;
        if (optlen < 0 || static_cast<u32>(optlen) < csaddr + 2 * sa_len) {
            return fail_with(WSAEFAULT);
        }
        std::vector<Byte> zero(csaddr, 0);
        if (m.memory().write_bytes(optval_ptr, zero.data(), zero.size()).is_err()) {
            return fail_with(WSAEFAULT);
        }
        auto put_ptr = [&](GuestAddr at, GuestAddr v) {
            if (is64) (void)m.write_u64(at, v);
            else (void)m.write_u32(at, static_cast<u32>(v));
        };
        GuestAddr cursor = optval_ptr + csaddr;
        const auto st = mgr.socket_state(guest_socket);
        if (hi.bound) {
            sockaddr_storage local{};
            socklen_t ll = sizeof(local);
            (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &ll);
            if (st.bound_by_connect) {
                if (local.ss_family == AF_INET) {
                    reinterpret_cast<sockaddr_in*>(&local)->sin_addr.s_addr = 0;
                } else if (local.ss_family == AF_INET6) {
                    std::memset(&reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr, 0, 16);
                }
            }
            const u32 n = write_win_sockaddr(m, cursor, sa_len, local);
            if (n != 0) {
                put_ptr(optval_ptr, cursor);
                (void)m.write_u32(optval_ptr + psz, n);
                cursor += n;
            }
        }
        if (hi.connected) {
            sockaddr_storage peer{};
            socklen_t pl = sizeof(peer);
            (void)::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl);
            const u32 n = write_win_sockaddr(m, cursor, sa_len, peer);
            if (n != 0) {
                put_ptr(optval_ptr + 2 * psz, cursor);
                (void)m.write_u32(optval_ptr + 3 * psz, n);
            }
        }
        const bool tcp = hi.type == SOCK_STREAM;
        (void)m.write_u32(optval_ptr + 4 * psz, tcp ? 1u : 2u);
        (void)m.write_u32(optval_ptr + 4 * psz + 4, tcp ? 6u : 17u);
        mgr.set_last_error(0);
        m.set_return(0);
        return;
    }
    if (r.protocol_info != 0) {
        if (optlen < static_cast<int>(r.protocol_info)) return fail_with(WSAEFAULT);
        const HostSocketInfo info = host_info(fd);
        write_protocol_info(m, optval_ptr, r.protocol_info == 628,
                            info.family == AF_INET6 ? kWinAfInet6 : kWinAfInet, info.type);
        (void)m.write_u32(optlen_ptr, r.protocol_info);
        mgr.set_last_error(0);
        m.set_return(0);
        return;
    }
    const int width = static_cast<int>(r.bytes.size());
    if (optlen < width) {
        if (level == kWinIpprotoIp && width == 4 && optlen >= 1) {
            // IP_TTL_len1: rc=0 len=1 b0=0x80
            if (m.memory().write_bytes(optval_ptr, r.bytes.data(), 1).is_err()) {
                return fail_with(WSAEFAULT);
            }
            (void)m.write_u32(optlen_ptr, 1);
            mgr.set_last_error(0);
            m.set_return(0);
            return;
        }
        if (level == kWinIpprotoIpv6) {
            (void)m.write_u32(optlen_ptr, 0);               // tcp6_IPV6_V6ONLY_len1
        } else if (optlen > 0) {
            (void)m.memory().write_bytes(optval_ptr, r.bytes.data(),
                                         static_cast<usize>(optlen));
        }
        return fail_with(WSAEFAULT);
    }
    if (width == 4 && optlen > 4) {
        // SO_RCVBUF_len8: the bytes past the DWORD are zeroed.
        std::vector<Byte> zero(static_cast<usize>(optlen) - 4, 0);
        (void)m.memory().write_bytes(optval_ptr + 4, zero.data(), zero.size());
    }
    if (width > 0 &&
        m.memory().write_bytes(optval_ptr, r.bytes.data(), static_cast<usize>(width)).is_err()) {
        return fail_with(WSAEFAULT);
    }
    (void)m.write_u32(optlen_ptr, static_cast<u32>(width));
    mgr.set_last_error(0);
    m.set_return(0);
}

void ws2::impl_setsockopt(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int setsockopt(SOCKET s, int level, int optname, const char* optval, int optlen)
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const int level = static_cast<int>(static_cast<u32>(m.arg(1)));
    const int optname = static_cast<int>(static_cast<u32>(m.arg(2)));
    const GuestAddr optval_ptr = m.arg_ptr(3);
    const int optlen = static_cast<int>(m.arg32(4));
    auto fail_with = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    const int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) return fail_with(WSAENOTSOCK);
    if (optval_ptr == 0 || optlen <= 0) return fail_with(WSAEFAULT);
    std::vector<Byte> val(static_cast<usize>(std::min(optlen, 4096)));
    if (m.memory().read_bytes(optval_ptr, val.data(), val.size()).is_err()) {
        return fail_with(WSAEFAULT);
    }
    const u32 err = set_option(mgr, guest_socket, fd, level, optname, val, m.cpu_is_64bit());
    VITREO_TRACE("[ws2_32] setsockopt(socket=0x%x level=%d opt=0x%x len=%d) -> %u\n",
                 guest_socket, level, optname, optlen, err);
    if (err != 0) return fail_with(err);
    m.set_return(0);
}

} // namespace vitreo::winapi
