// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_wsa_ioctl_connectex.cpp
/// @brief WSAIoctl / ConnectEx
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
#include <ifaddrs.h>
#include <net/if.h>

namespace vitreo::winapi {

void ws2::impl_ConnectEx(Marshaler& m, Ws2Manager& mgr) {
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr addr_ptr = m.arg_ptr(1);
    int addrlen = static_cast<int>(m.arg(2));
    GuestAddr send_buffer_ptr = m.arg_ptr(3);
    u32 send_length = static_cast<u32>(m.arg(4));
    GuestAddr bytes_sent_ptr = m.arg_ptr(5);
    GuestAddr overlapped = m.arg_ptr(6);
    
    VITREO_TRACE( "[ws2_32] ConnectEx(socket=0x%x, addrlen=%d)\n", guest_socket, addrlen);
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(0);  // FALSE
        return;
    }

    // ConnectEx is always an overlapped API; lpOverlapped is mandatory.
    if (overlapped == 0 || addr_ptr == 0 ||
        (send_length != 0 && send_buffer_ptr == 0)) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(0);
        return;
    }

    GuestMemory* memory = mgr.guest_memory();
    if (!memory) memory = &m.memory();
    const bool is_64bit = m.cpu_is_64bit();
    const auto pending_write = is_64bit
        ? memory->write_u64(overlapped, STATUS_PENDING)
        : memory->write_u32(overlapped, STATUS_PENDING);
    const auto count_write = is_64bit
        ? memory->write_u64(overlapped + 8, 0)
        : memory->write_u32(overlapped + 4, 0);
    if (pending_write.is_err() || count_write.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(0);
        return;
    }
    if (bytes_sent_ptr != 0) (void)m.write_u32(bytes_sent_ptr, 0);

    std::vector<Byte> initial_data(send_length);
    if (send_length != 0 &&
        memory->read_bytes(send_buffer_ptr, initial_data.data(), send_length)
            .is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(0);
        return;
    }
    
    // Read address from guest (Windows sockaddr_in / sockaddr_in6 → host)
    MappedSockaddr mapped{};
    if (addr_ptr != 0) {
        if (!read_win_sockaddr_mapped(m, addr_ptr, addrlen, mapped)) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(0);
            return;
        }
    }

    char addr_str[INET6_ADDRSTRLEN] = {};
    u16 port = 0;
    format_mapped_sockaddr(mapped, addr_str, sizeof(addr_str), &port);
    ipc::ipc_log("ConnectEx socket=0x%x -> %s:%u",
                 guest_socket, addr_str, static_cast<unsigned>(port));
    
    // Make socket non-blocking for async-style behavior
    int old_flags = fcntl(native_fd, F_GETFL, 0);
    fcntl(native_fd, F_SETFL, old_flags | O_NONBLOCK);
    
    int rc = ::connect(native_fd, reinterpret_cast<struct sockaddr*>(&mapped.ss),
                       mapped.len);
    int err = errno;
    
    // Restore the caller's socket mode after initiating the host operation.
    fcntl(native_fd, F_SETFL, old_flags);

    auto complete_inline = [&](u32 bytes) {
        if (bytes_sent_ptr != 0) (void)memory->write_u32(bytes_sent_ptr, bytes);
        mgr.note_stream_connected(guest_socket);
        mgr.notify_overlapped_completion(
            static_cast<Handle>(guest_socket), bytes, overlapped, is_64bit,
            STATUS_SUCCESS, 0, /*completed_synchronously=*/true);
        mgr.set_last_error(0);
        m.set_return(1);
    };

    auto begin_pending = [&](bool connection_pending) {
        {
            ReapedWorkers::Finished finished;
            std::lock_guard<std::mutex> lock(mgr.m_overlapped_socket_io_mutex);
            mgr.track_pending_socket_io(guest_socket, overlapped);
            mgr.m_overlapped_socket_io_workers.spawn(
                [&mgr, guest_socket, native_fd, overlapped, is_64bit,
                 bytes_sent_ptr, memory, connection_pending,
                 initial_data = std::move(initial_data)](
                    std::stop_token stop_token) mutable {
                    auto still_active = [&]() -> bool {
                        std::lock_guard<std::mutex> lock(
                            mgr.m_overlapped_socket_io_mutex);
                        auto it = mgr.m_pending_socket_io.find(overlapped);
                        return it != mgr.m_pending_socket_io.end() &&
                               !it->second.cancel;
                    };
                    auto finish = [&](u32 bytes, u32 nt_status, u32 win_error) {
                        // MS-ERREF: a refused transport connection is
                        // STATUS_CONNECTION_REFUSED, not STATUS_UNSUCCESSFUL.
                        // OVERLAPPED.Internal is consumed directly by callers.
                        if (win_error == WSAECONNREFUSED) nt_status = 0xC0000236u;
                        const auto pending =
                            mgr.consume_pending_socket_io(overlapped);
                        if (!pending.has_value() || *pending) return;
                        if (bytes_sent_ptr != 0 && memory) {
                            (void)memory->write_u32(bytes_sent_ptr, bytes);
                        }
                        ipc::ipc_log(
                            "ConnectEx complete socket=0x%x ov=0x%llx bytes=%u error=%u",
                            guest_socket,
                            static_cast<unsigned long long>(overlapped), bytes,
                            win_error);
                        // Learn WSAEventSelect: FD_WRITE is recorded when a
                        // socket is connected with ConnectEx.
                        if (win_error == 0) mgr.note_stream_connected(guest_socket);
                        mgr.notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), bytes, overlapped,
                            is_64bit, nt_status, win_error,
                            /*completed_synchronously=*/false);
                    };

                    if (connection_pending) {
                        for (;;) {
                            if (stop_token.stop_requested() || !still_active()) {
                                (void)mgr.consume_pending_socket_io(overlapped);
                                return;
                            }
                            struct pollfd pfd{};
                            pfd.fd = native_fd;
                            pfd.events = POLLOUT | POLLERR | POLLHUP;
                            const int pr = ::poll(&pfd, 1, 50);
                            if (pr < 0) {
                                if (errno == EINTR) continue;
                                const u32 wsa = Ws2Manager::errno_to_wsa(errno);
                                finish(0, 0xC0000001u, wsa);
                                return;
                            }
                            if (pr == 0) continue;

                            int socket_error = 0;
                            socklen_t error_length = sizeof(socket_error);
                            if (::getsockopt(native_fd, SOL_SOCKET, SO_ERROR,
                                             &socket_error, &error_length) != 0) {
                                socket_error = errno;
                            }
                            if (socket_error != 0 ||
                                (pfd.revents & (POLLERR | POLLHUP))) {
                                if (socket_error == 0) socket_error = ECONNREFUSED;
                                finish(0, 0xC0000001u,
                                       Ws2Manager::errno_to_wsa(socket_error));
                                return;
                            }
                            break;
                        }
                    }

                    size_t sent = 0;
                    while (sent < initial_data.size()) {
                        if (stop_token.stop_requested() || !still_active()) {
                            (void)mgr.consume_pending_socket_io(overlapped);
                            return;
                        }
                        const ssize_t rc = ::send(
                            native_fd, initial_data.data() + sent,
                            initial_data.size() - sent, 0);
                        if (rc > 0) {
                            sent += static_cast<size_t>(rc);
                            continue;
                        }
                        if (rc < 0 && errno == EINTR) continue;
                        if (rc < 0 &&
                            (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            struct pollfd pfd{};
                            pfd.fd = native_fd;
                            pfd.events = POLLOUT | POLLERR | POLLHUP;
                            (void)::poll(&pfd, 1, 50);
                            continue;
                        }
                        const int send_error = rc < 0 ? errno : ECONNRESET;
                        finish(static_cast<u32>(sent), 0xC0000001u,
                               Ws2Manager::errno_to_wsa(send_error));
                        return;
                    }
                    finish(static_cast<u32>(sent), STATUS_SUCCESS, 0);
                }, finished);
        }
        mgr.set_last_error(WSA_IO_PENDING);
        ipc::ipc_log("ConnectEx %s:%u -> IO_PENDING ov=0x%llx",
                     addr_str, static_cast<unsigned>(port),
                     static_cast<unsigned long long>(overlapped));
        m.set_return(0);
    };
    
    if (rc == 0) {
        // A synchronous ConnectEx success includes all optional initial data.
        // If data is present, complete it through the pending path so a short
        // nonblocking send cannot be mistaken for full success.
        if (!initial_data.empty()) {
            begin_pending(/*connection_pending=*/false);
            return;
        }
        VITREO_TRACE( "[ws2_32] ConnectEx: immediate success\n");
        ipc::ipc_log("ConnectEx %s:%d -> TRUE", addr_str, static_cast<unsigned>(port));
        complete_inline(0);
        return;
    }
    
    if (err == EINPROGRESS || err == EWOULDBLOCK) {
        // Connection-oriented sockets commonly complete later.  Return the
        // native pending result immediately and deliver exactly one eventual
        // OVERLAPPED/IOCP completion.
        begin_pending(/*connection_pending=*/true);
        return;
    }
    
    // Real error
    u32 wsa = Ws2Manager::errno_to_wsa(err);
    mgr.set_last_error(wsa);
    VITREO_TRACE( "[ws2_32] ConnectEx: error %d\n", err);
    ipc::ipc_log("ConnectEx %s:%u -> FALSE wsa=%u errno=%d",
                 addr_str, static_cast<unsigned>(port), wsa, err);
    m.set_return(0);  // FALSE
}

namespace {

struct ExtensionGuid {
    u8 guid[16];
    const char* name;
};
// WSAID_* from mswsock.h (Windows SDK 10.0.26100), little-endian GUID bytes.
constexpr ExtensionGuid kExtensionGuids[] = {
    {{0xb9, 0x07, 0xa2, 0x25, 0xf3, 0xdd, 0x60, 0x46, 0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}, "ConnectEx"},
    {{0x11, 0x2e, 0xda, 0x7f, 0x30, 0x86, 0x6f, 0x43, 0xa0, 0x31, 0xf5, 0x36, 0xa6, 0xee, 0xc1, 0x57}, "DisconnectEx"},
    {{0xf1, 0x7d, 0x36, 0xb5, 0xac, 0xcb, 0xcf, 0x11, 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}, "AcceptEx"},
    {{0xf2, 0x7d, 0x36, 0xb5, 0xac, 0xcb, 0xcf, 0x11, 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}, "GetAcceptExSockaddrs"},
    {{0xf0, 0x7d, 0x36, 0xb5, 0xac, 0xcb, 0xcf, 0x11, 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}, "TransmitFile"},
    // WSAID_TRANSMITPACKETS {d9689da0-1f90-11d3-9971-00c04f68c876} (mswsock.h)
    {{0xa0, 0x9d, 0x68, 0xd9, 0x90, 0x1f, 0xd3, 0x11, 0x99, 0x71, 0x00, 0xc0, 0x4f, 0x68, 0xc8, 0x76}, "TransmitPackets"},
    {{0xc8, 0xd7, 0x89, 0xf6, 0x1f, 0x6f, 0x6b, 0x43, 0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22}, "WSARecvMsg"},
    {{0x12, 0xe7, 0x41, 0xa4, 0x4f, 0x75, 0xca, 0x43, 0x84, 0xa7, 0x0d, 0xee, 0x44, 0xcf, 0x60, 0x6d}, "WSASendMsg"},
};

/// Local addresses of one host family, loopback excluded (the address list
/// Winsock reports for SIO_ADDRESS_LIST_QUERY).
std::vector<sockaddr_storage> local_addresses(int host_family) {
    std::vector<sockaddr_storage> out;
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0) return out;
    for (const struct ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != host_family ||
            (i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        sockaddr_storage ss{};
        std::memcpy(&ss, i->ifa_addr,
                    host_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
        out.push_back(ss);
    }
    ::freeifaddrs(ifs);
    return out;
}

}  // namespace

void ws2::impl_WSAIoctl(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // int WSAIoctl(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer,
    //   DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer,
    //   LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped,
    //   LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
    //
    // Learn WSAIoctl / Winsock IOCTLs; everything below that Learn leaves open
    // (which errors, whether *lpcbBytesReturned is written on failure) is from
    // tools/oracle_probe/ws2_opts_probe.expected (wsaioctl_*, keepalive_*,
    // ext_*, bsp_*, udp_connreset_*, broadcast_*, routing_*, address_list_*,
    // iflist_*). ret=57005 in the probe means "left untouched".
    const u32 guest_socket = ws2_socket_arg(m, 0);
    const u32 code = static_cast<u32>(m.arg(1));
    const GuestAddr in_buf = m.arg_ptr(2);
    const u32 in_len = static_cast<u32>(m.arg(3));
    const GuestAddr out_buf = m.arg_ptr(4);
    const u32 out_len = static_cast<u32>(m.arg32(5));
    const GuestAddr ret_ptr = m.arg_ptr(6);
    const GuestAddr overlapped = m.arg_ptr(7);
    const GuestAddr routine = m.arg_ptr(8);
    const bool is64 = m.cpu_is_64bit();
    const u32 ptr_size = is64 ? 8u : 4u;

    VITREO_TRACE("[ws2_32] WSAIoctl(socket=0x%x, code=0x%08x, inlen=%u, outlen=%u)\n",
                 guest_socket, code, in_len, out_len);

    auto set_ret = [&](u32 v) { if (ret_ptr != 0) (void)m.write_u32(ret_ptr, v); };
    auto fail = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    auto fail_ret0 = [&](u32 err) { set_ret(0); fail(err); };
    auto succeed = [&](u32 bytes) {
        set_ret(bytes);
        if (overlapped != 0) {
            // ws2_ioctl_ov_probe.expected: immediate overlapped FIONREAD /
            // SIO_GET_EXTENSION returns 0, *lpcbBytesReturned=bytes, but
            // OVERLAPPED.InternalHigh stays 0 (GOR xfer=0) while hEvent is
            // signaled. Byte count lives only in *lpcbBytesReturned.
            mgr.complete_socket_request(guest_socket, overlapped, is64, routine,
                                        /*bytes=*/0, STATUS_SUCCESS, 0,
                                        /*flags=*/0,
                                        /*completed_synchronously=*/true);
        }
        m.set_return(0);
    };

    const int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) return fail(WSAENOTSOCK);
    int host_type = 0;
    {
        socklen_t tl = sizeof(host_type);
        (void)::getsockopt(fd, SOL_SOCKET, SO_TYPE, &host_type, &tl);
    }
    const bool stream = host_type == SOCK_STREAM;

    constexpr u32 kFionbio = 0x8004667E, kFionread = 0x4004667F, kSiocatmark = 0x40047307;
    constexpr u32 kGetExtension = 0xC8000006, kKeepaliveVals = 0x98000004;
    constexpr u32 kBspHandle = 0x4800001B, kBspHandleSelect = 0x4800001C,
                  kBspHandlePoll = 0x4800001D, kBaseHandle = 0x48000022;
    constexpr u32 kUdpConnreset = 0x9800000C, kLoopbackFastPath = 0x98000010;
    constexpr u32 kGetBroadcast = 0x48000005, kRoutingQuery = 0xC8000014,
                  kAddressListQuery = 0x48000016, kGetInterfaceList = 0x4004747F;

    switch (code) {
    case kFionbio: {
        if (in_buf == 0 || in_len < 4) return fail(WSAEFAULT);
        auto v = m.read_u32(in_buf);
        if (v.is_err()) return fail(WSAEFAULT);
        u32 value = v.value();
        if (const u32 err = ws2_unix_ioctl(mgr, guest_socket, fd, code, &value)) return fail(err);
        return succeed(0);
    }
    case kFionread:
    case kSiocatmark: {
        if (out_buf == 0 || out_len < 4) return fail(WSAEFAULT);
        u32 value = 0;
        if (const u32 err = ws2_unix_ioctl(mgr, guest_socket, fd, code, &value)) return fail(err);
        if (m.write_u32(out_buf, value).is_err()) return fail(WSAEFAULT);
        return succeed(4);
    }
    case kKeepaliveVals: {
        // struct tcp_keepalive { ULONG onoff; ULONG keepalivetime; ULONG
        // keepaliveinterval; } -- times in milliseconds. With onoff=0 the
        // times are not applied (keepidle_after_off stays 5).
        if (!stream) return fail_ret0(WSAEOPNOTSUPP);
        if (in_buf == 0 || in_len < 12) return fail_ret0(WSAEFAULT);
        u32 vals[3] = {};
        if (m.memory().read_bytes(in_buf, reinterpret_cast<Byte*>(vals), 12).is_err()) {
            return fail_ret0(WSAEFAULT);
        }
        const int on = vals[0] != 0 ? 1 : 0;
        if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) {
            return fail_ret0(Ws2Manager::errno_to_wsa(errno));
        }
        if (on) {
            const int idle = static_cast<int>(std::max<u32>(1, vals[1] / 1000));
            const int intvl = static_cast<int>(std::max<u32>(1, vals[2] / 1000));
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle)) != 0 ||
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) != 0) {
                return fail_ret0(Ws2Manager::errno_to_wsa(errno));
            }
        }
        return succeed(0);
    }
    case kGetExtension: {
        u8 guid[16] = {};
        if (in_buf == 0 || in_len < 16 || m.memory().read_bytes(in_buf, guid, 16).is_err()) {
            return fail(WSAEFAULT);
        }
        if (out_buf == 0 || out_len < ptr_size) return fail(WSAEFAULT);
        const char* name = nullptr;
        for (const auto& e : kExtensionGuids) {
            if (std::memcmp(e.guid, guid, 16) == 0) { name = e.name; break; }
        }
        if (name == nullptr) return fail(WSAEINVAL);
        const GuestAddr fn = mgr.get_extension_func(name);
        if (fn == 0) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ws2_32!WSAIoctl: extension %s "
                                 "has no thunk\n", name);
            return fail(WSAEINVAL);
        }
        const bool ok = is64 ? m.write_u64(out_buf, fn).is_ok()
                             : m.write_u32(out_buf, static_cast<u32>(fn)).is_ok();
        if (!ok) return fail(WSAEFAULT);
        ipc::ipc_log("WSAIoctl SIO_GET_EXTENSION %s -> 0x%llx", name,
                     static_cast<unsigned long long>(fn));
        return succeed(ptr_size);
    }
    case kBspHandle:
    case kBspHandleSelect:
    case kBspHandlePoll:
    case kBaseHandle: {
        if (code == kBspHandlePoll && overlapped != 0) return fail_ret0(WSAEOPNOTSUPP);
        if (out_buf == 0 || out_len < ptr_size) return fail(WSAEFAULT);
        // Vitreo has no layered providers: the base handle is the socket.
        const bool ok = is64 ? m.write_u64(out_buf, guest_socket).is_ok()
                             : m.write_u32(out_buf, guest_socket).is_ok();
        if (!ok) return fail(WSAEFAULT);
        return succeed(ptr_size);
    }
    case kUdpConnreset: {
        // Datagram only (udp_connreset_tcp 10022); the output count is left
        // untouched even on success (udp_connreset_udp ret=57005).
        if (host_type != SOCK_DGRAM) return fail(WSAEINVAL);
        u32 enable = 0;
        if (in_buf != 0 && in_len != 0) {
            u8 b[4] = {};
            (void)m.memory().read_bytes(in_buf, b, std::min<u32>(in_len, 4));
            enable = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<u32>(b[3]) << 24);
        }
        if (enable != 0) {
            // Darwin never reports ICMP port-unreachable on an unconnected UDP
            // socket, so the WSAECONNRESET reporting cannot be provided.
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ws2_32!WSAIoctl: "
                                 "SIO_UDP_CONNRESET TRUE (no host ICMP reporting)\n");
            return fail_ret0(WSAEOPNOTSUPP);
        }
        if (overlapped != 0) {
            mgr.complete_socket_request(guest_socket, overlapped, is64, routine, 0,
                                        STATUS_SUCCESS, 0, 0, true);
        }
        m.set_return(0);
        return;
    }
    case kLoopbackFastPath:
        // A loopback performance hint with no semantic effect (loopback_fast_path).
        return succeed(0);
    case kGetBroadcast: {
        if (host_type != SOCK_DGRAM) return fail(WSAEINVAL);
        if (out_buf == 0 || out_len < 16) return fail(WSAEFAULT);
        sockaddr_storage ss{};
        auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = INADDR_BROADCAST;
        if (write_win_sockaddr(m, out_buf, out_len, ss) == 0) return fail(WSAEFAULT);
        return succeed(16);
    }
    case kRoutingQuery: {
        MappedSockaddr dst{};
        if (!read_win_sockaddr_mapped(m, in_buf, static_cast<int>(in_len), dst)) {
            return fail(WSAEFAULT);
        }
        // The local address the host would route to `dst` from: connect a
        // throwaway datagram socket and ask for its name. Routes do not
        // depend on the port, but Darwin refuses to connect to port 0
        // (EADDRNOTAVAIL) while Windows answers a port-0 query
        // (ws2_opts_probe routing_iface_query: 127.0.0.1:0 -> 127.0.0.1),
        // so the lookup uses a nonzero port.
        if (dst.ss.ss_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(&dst.ss);
            if (sin->sin_port == 0) sin->sin_port = htons(9);
        } else if (dst.ss.ss_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(&dst.ss);
            if (sin6->sin6_port == 0) sin6->sin6_port = htons(9);
        }
        const int probe = ::socket(dst.ss.ss_family, SOCK_DGRAM, 0);
        if (probe < 0) return fail(Ws2Manager::errno_to_wsa(errno));
        sockaddr_storage local{};
        socklen_t ll = sizeof(local);
        const int crc = ::connect(probe, reinterpret_cast<sockaddr*>(&dst.ss), dst.len);
        const int cerr = errno;
        const int grc = crc == 0 ? ::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &ll) : -1;
        ::close(probe);
        if (crc != 0) return fail(Ws2Manager::errno_to_wsa(cerr));
        if (grc != 0) return fail(WSAENETUNREACH);
        if (local.ss_family == AF_INET) reinterpret_cast<sockaddr_in*>(&local)->sin_port = 0;
        else reinterpret_cast<sockaddr_in6*>(&local)->sin6_port = 0;
        const u32 need = local.ss_family == AF_INET6 ? static_cast<u32>(kWinSockaddrIn6Bytes) : 16u;
        if (out_buf == 0 || out_len < need) {
            set_ret(need);                          // routing_iface_query_short ret=16
            return fail(WSAEFAULT);
        }
        if (write_win_sockaddr(m, out_buf, out_len, local) == 0) return fail(WSAEFAULT);
        return succeed(need);
    }
    case kAddressListQuery: {
        // SOCKET_ADDRESS_LIST { INT iAddressCount; SOCKET_ADDRESS Address[]; }
        // SOCKET_ADDRESS { LPSOCKADDR lpSockaddr; INT iSockaddrLength; }
        // followed by the sockaddrs (address_list_query_short ret=40 on x64
        // for one IPv4 address: 8 + 16 + 16).
        sockaddr_storage own{};
        socklen_t ol = sizeof(own);
        (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&own), &ol);
        const int family = own.ss_family == AF_INET6 ? AF_INET6 : AF_INET;
        const auto addrs = local_addresses(family);
        const u32 sa_len = family == AF_INET6 ? static_cast<u32>(kWinSockaddrIn6Bytes) : 16u;
        const u32 head = ptr_size;                  // iAddressCount padded to pointer
        const u32 entry = 2 * ptr_size;
        const u32 need = head + static_cast<u32>(addrs.size()) * (entry + sa_len);
        if (out_buf == 0 || out_len < need) {
            set_ret(need);
            return fail(WSAEFAULT);
        }
        (void)m.write_u32(out_buf, static_cast<u32>(addrs.size()));
        if (is64) (void)m.write_u32(out_buf + 4, 0);
        GuestAddr sa_at = out_buf + head + static_cast<u32>(addrs.size()) * entry;
        for (usize i = 0; i < addrs.size(); ++i) {
            const GuestAddr e = out_buf + head + static_cast<u32>(i) * entry;
            if (is64) (void)m.write_u64(e, sa_at);
            else (void)m.write_u32(e, static_cast<u32>(sa_at));
            (void)m.write_u32(e + ptr_size, sa_len);
            if (is64) (void)m.write_u32(e + 12, 0);
            (void)write_win_sockaddr(m, sa_at, sa_len, addrs[i]);
            sa_at += sa_len;
        }
        return succeed(need);
    }
    case kGetInterfaceList: {
        // INTERFACE_INFO { ULONG iiFlags; sockaddr_gen iiAddress,
        // iiBroadcastAddress, iiNetmask; } = 4 + 3 * 24 = 76 bytes, IPv4 only.
        // Loopback reads flags=0x15 mask=255.0.0.0 bcast=0 (iflist_loopback).
        struct ifaddrs* ifs = nullptr;
        if (::getifaddrs(&ifs) != 0) return fail(Ws2Manager::errno_to_wsa(errno));
        std::vector<Byte> blob;
        for (const struct ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET ||
                (i->ifa_flags & IFF_UP) == 0) {
                continue;
            }
            Byte rec[76] = {};
            u32 flags = 0x1;                                    // IFF_UP
            if (i->ifa_flags & IFF_BROADCAST) flags |= 0x2;
            if (i->ifa_flags & IFF_LOOPBACK) flags |= 0x4;
            if (i->ifa_flags & IFF_POINTOPOINT) flags |= 0x8;
            if (i->ifa_flags & IFF_MULTICAST) flags |= 0x10;
            std::memcpy(rec, &flags, 4);
            auto put = [&](usize off, const sockaddr* sa) {
                const u16 fam = kWinAfInet;
                std::memcpy(rec + off, &fam, 2);
                if (sa != nullptr && sa->sa_family == AF_INET) {
                    std::memcpy(rec + off + 4,
                                &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, 4);
                }
            };
            put(4, i->ifa_addr);
            put(28, (i->ifa_flags & IFF_BROADCAST) ? i->ifa_broadaddr : nullptr);
            put(52, i->ifa_netmask);
            blob.insert(blob.end(), rec, rec + sizeof(rec));
        }
        ::freeifaddrs(ifs);
        if (out_buf == 0 || out_len < blob.size()) return fail_ret0(WSAEFAULT);
        if (!blob.empty() && m.memory().write_bytes(out_buf, blob.data(), blob.size()).is_err()) {
            return fail_ret0(WSAEFAULT);
        }
        return succeed(static_cast<u32>(blob.size()));
    }
    default:
        // Unknown or unsupported code: WSAEOPNOTSUPP with the count zeroed
        // (wsaioctl_unknown / wsaioctl_unknown_vendor: err=10045 ret=0).
        VITREO_TRACE("[ws2_32] WSAIoctl: unsupported code 0x%08x\n", code);
        return fail_ret0(WSAEOPNOTSUPP);
    }
}

} // namespace vitreo::winapi
