// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32_protocol.cpp
/// @brief WSAEnumProtocols and AcceptEx address helpers.
///
/// Closed cut from ws2_32.cpp — verified with plan_extraction.py.

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

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// WSAEnumProtocolsA/W — report the TCP/UDP AF_INET catalog entries
// -----------------------------------------------------------------------------
// Real ws2_32.dll exports these at ordinals 101 (A) and 102 (W). Ordinal 101
// is imported by ordinal. A missing ordinal installs a fail-thunk that returns
// 0xFFFFFFFF and breaks protocol enumeration.
static void impl_WSAEnumProtocols(Marshaler& m, Ws2Manager& mgr, bool unicode) {
    // int WSAEnumProtocols*(LPINT lpiProtocols,
    //                       LPWSAPROTOCOL_INFO* lpProtocolBuffer,
    //                       LPDWORD lpdwBufferLength)
    GuestAddr protocols_ptr = m.arg_ptr(0);
    GuestAddr buffer_ptr = m.arg_ptr(1);
    GuestAddr len_ptr = m.arg_ptr(2);

    VITREO_TRACE(
        "[ws2_32] WSAEnumProtocols%c(protos=0x%llx, buf=0x%llx, len=0x%llx)\n",
        unicode ? 'W' : 'A',
        static_cast<unsigned long long>(protocols_ptr),
        static_cast<unsigned long long>(buffer_ptr),
        static_cast<unsigned long long>(len_ptr));

    if (!len_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    // Optional protocol filter: NULL means "all". A non-null list that does
    // not contain IPPROTO_TCP or IPPROTO_UDP yields an empty result.
    bool want_tcp = true;
    bool want_udp = true;
    if (protocols_ptr) {
        want_tcp = false;
        want_udp = false;
        for (u32 i = 0; i < 64; ++i) {
            auto v = m.read_u32(protocols_ptr + i * 4);
            if (v.is_err()) break;
            i32 proto = static_cast<i32>(v.value());
            if (proto == 0) break; // Windows lists are zero-terminated in practice
            if (proto == IPPROTO_TCP) want_tcp = true;
            if (proto == IPPROTO_UDP) want_udp = true;
        }
    }

    // sizeof(WSAPROTOCOL_INFOA)=372, sizeof(WSAPROTOCOL_INFOW)=628 on x86.
    const u32 kInfoBytes = unicode ? 628u : 372u;
    u32 count = (want_tcp ? 1u : 0u) + (want_udp ? 1u : 0u);
    u32 need = count * kInfoBytes;

    auto len_res = m.read_u32(len_ptr);
    if (len_res.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    u32 have = len_res.value();

    if (count == 0) {
        (void)m.write_u32(len_ptr, 0);
        m.set_return(0);
        return;
    }

    if (!buffer_ptr || have < need) {
        (void)m.write_u32(len_ptr, need);
        mgr.set_last_error(WSAENOBUFS);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    auto write_entry = [&](GuestAddr base, i32 sock_type, i32 protocol,
                           const char* name) {
        for (u32 off = 0; off < kInfoBytes; off += 4)
            (void)m.write_u32(base + off, 0);
        // dwServiceFlags1: XP1_IFS_HANDLES | connection semantics
        if (sock_type == SOCK_STREAM) {
            // TCP: connection-oriented, guaranteed delivery/order, pseudo-stream
            (void)m.write_u32(base + 0, 0x20001u | 0x2u | 0x4u | 0x8u);
        } else {
            // UDP: connectionless, message-oriented
            (void)m.write_u32(base + 0, 0x20001u | 0x1u);
        }
        (void)m.write_u32(base + 72, 2);              // iVersion
        (void)m.write_u32(base + 76, AF_INET);        // iAddressFamily
        (void)m.write_u32(base + 80, 16);             // iMaxSockAddr
        (void)m.write_u32(base + 84, 16);             // iMinSockAddr
        (void)m.write_u32(base + 88, static_cast<u32>(sock_type));
        (void)m.write_u32(base + 92, static_cast<u32>(protocol));
        (void)m.write_u32(base + 100, 0);             // BIGENDIAN
        if (unicode) {
            (void)m.write_wide_string(base + 116, name);  // szProtocol
        } else {
            for (u32 i = 0; name[i] != '\0' && i < 255; ++i)
                (void)m.write_u8(base + 116 + i, static_cast<u8>(name[i]));
        }
    };
    u32 written = 0;
    if (want_tcp) {
        write_entry(buffer_ptr + written * kInfoBytes, SOCK_STREAM, IPPROTO_TCP,
                    "MSAFD Tcpip [TCP/IP]");
        ++written;
    }
    if (want_udp) {
        write_entry(buffer_ptr + written * kInfoBytes, SOCK_DGRAM, IPPROTO_UDP,
                    "MSAFD Tcpip [UDP/IP]");
        ++written;
    }

    (void)m.write_u32(len_ptr, need);
    m.set_return(written);
}

void ws2::impl_WSAEnumProtocolsA(Marshaler& m, Ws2Manager& mgr) {
    impl_WSAEnumProtocols(m, mgr, /*unicode=*/false);
}

void ws2::impl_WSAEnumProtocolsW(Marshaler& m, Ws2Manager& mgr) {
    impl_WSAEnumProtocols(m, mgr, /*unicode=*/true);
}

namespace {

void write_acceptex_addrs(GuestMemory& memory, GuestAddr output_buf,
                          u32 recv_len, u32 local_len, u32 remote_len,
                          const sockaddr_storage& local_ss,
                          const sockaddr_storage& remote_ss) {
    if (output_buf == 0) return;
    // Learn AcceptEx: [optional recv bytes][local sockaddr][remote sockaddr].
    Marshaler scratch(memory);
    if (local_len >= 16) {
        (void)write_win_sockaddr(scratch, output_buf + recv_len, local_len,
                                 local_ss);
    }
    if (remote_len >= 16) {
        (void)write_win_sockaddr(scratch, output_buf + recv_len + local_len,
                                 remote_len, remote_ss);
    }
}

} // namespace

// -----------------------------------------------------------------------------
// AcceptEx — Async accept with initial data receive
// -----------------------------------------------------------------------------
void ws2_AcceptEx(Marshaler& m, Ws2Manager& mgr) {
    // BOOL AcceptEx(SOCKET sListenSocket, SOCKET sAcceptSocket,
    //               PVOID lpOutputBuffer, DWORD dwReceiveDataLength,
    //               DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength,
    //               LPDWORD lpdwBytesReceived, LPOVERLAPPED lpOverlapped)
    // Contract: Learn mswsock!AcceptEx — with lpOverlapped, a would-block
    // listen completes later via IOCP/event; never fabricate success.
    u32 listen_socket = static_cast<u32>(m.arg(0));
    u32 accept_socket = static_cast<u32>(m.arg(1));
    GuestAddr output_buf = m.arg_ptr(2);
    u32 recv_len = static_cast<u32>(m.arg(3));
    u32 local_len = static_cast<u32>(m.arg(4));
    u32 remote_len = static_cast<u32>(m.arg(5));
    GuestAddr bytes_received_ptr = m.arg_ptr(6);
    GuestAddr overlapped = m.arg_ptr(7);

    ipc::ipc_log(
        "AcceptEx ENTER listen=0x%x accept=0x%x recv=%u local=%u remote=%u "
        "ov=0x%llx",
        listen_socket, accept_socket, recv_len, local_len, remote_len,
        static_cast<unsigned long long>(overlapped));
    VITREO_TRACE(
        "[ws2_32] AcceptEx(listen=0x%x, accept=0x%x, buf=0x%llx, recv=%u, "
        "local=%u, remote=%u, overlapped=0x%llx)\n",
        listen_socket, accept_socket, output_buf, recv_len, local_len, remote_len,
        overlapped);

    int listen_fd = mgr.get_native_fd(listen_socket);
    int accept_fd = mgr.get_native_fd(accept_socket);

    if (listen_fd < 0 || accept_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(0); // FALSE
        return;
    }

    GuestMemory* memory = mgr.guest_memory() ? mgr.guest_memory() : &m.memory();
    const bool is_64bit = m.cpu_is_64bit();

    auto finish_accepted = [&](int new_fd, const sockaddr_storage& remote_ss,
                               socklen_t remote_len_host, bool synchronous) {
        ::close(accept_fd);
        mgr.replace_native_fd(accept_socket, new_fd);

        sockaddr_storage local_ss{};
        socklen_t local_sslen = sizeof(local_ss);
        if (::getsockname(new_fd, reinterpret_cast<sockaddr*>(&local_ss),
                          &local_sslen) != 0) {
            local_ss = {};
        }

        if (memory) {
            write_acceptex_addrs(*memory, output_buf, recv_len, local_len,
                                 remote_len, local_ss, remote_ss);
        }
        if (bytes_received_ptr) {
            (void)m.write_u32(bytes_received_ptr, 0);
        }

        MappedSockaddr peer{};
        peer.ss = remote_ss;
        peer.len = remote_len_host;
        char addr_str[INET6_ADDRSTRLEN] = {};
        u16 port = 0;
        format_mapped_sockaddr(peer, addr_str, sizeof(addr_str), &port);
        ipc::ipc_log("AcceptEx listen=0x%x -> accept=0x%x peer=%s:%u sync=%d",
                     listen_socket, accept_socket, addr_str,
                     static_cast<unsigned>(port), synchronous ? 1 : 0);

        if (overlapped != 0 && memory) {
            mgr.notify_overlapped_completion(
                static_cast<Handle>(listen_socket), 0, overlapped, is_64bit,
                STATUS_SUCCESS, 0, synchronous);
        }
        mgr.set_last_error(0);
        m.set_return(1); // TRUE
    };

    // Prefer non-blocking accept so overlapped callers get ERROR_IO_PENDING
    // instead of parking a guest thread inside AcceptEx (Learn: AcceptEx is
    // designed for overlapped / IOCP use).
    const int old_flags = ::fcntl(listen_fd, F_GETFL, 0);
    if (old_flags >= 0) {
        (void)::fcntl(listen_fd, F_SETFL, old_flags | O_NONBLOCK);
    }

    // Learn AcceptEx: with dwReceiveDataLength > 0 the request "will not
    // complete until a connection is accepted and data is read", so it never
    // completes inline; the worker accepts and then waits for the first block.
    sockaddr_storage remote_ss{};
    socklen_t remote_sslen = sizeof(remote_ss);
    int new_fd = -1;
    int accept_errno = EAGAIN;
    if (recv_len == 0) {
        new_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&remote_ss),
                          &remote_sslen);
        accept_errno = errno;
    }

    if (old_flags >= 0) {
        (void)::fcntl(listen_fd, F_SETFL, old_flags);
    }

    if (new_fd >= 0) {
        if (overlapped != 0 && memory) {
            if (is_64bit) {
                (void)memory->write_u64(overlapped, STATUS_PENDING);
                (void)memory->write_u64(overlapped + 8, 0);
            } else {
                (void)memory->write_u32(overlapped, STATUS_PENDING);
                (void)memory->write_u32(overlapped + 4, 0);
            }
        }
        finish_accepted(new_fd, remote_ss, remote_sslen,
                        /*synchronous=*/true);
        return;
    }

    if (accept_errno != EAGAIN && accept_errno != EWOULDBLOCK) {
        const u32 wsa_err = Ws2Manager::errno_to_wsa(accept_errno);
        ipc::ipc_log("AcceptEx listen=0x%x accept=0x%x -> FAIL wsa=%u errno=%d",
                     listen_socket, accept_socket, wsa_err, accept_errno);
        mgr.set_last_error(wsa_err);
        m.set_return(0);
        return;
    }

    // Would block.
    if (overlapped == 0 || memory == nullptr) {
        mgr.set_last_error(WSAEWOULDBLOCK);
        m.set_return(0);
        return;
    }

    if (is_64bit) {
        (void)memory->write_u64(overlapped, STATUS_PENDING);
        (void)memory->write_u64(overlapped + 8, 0);
    } else {
        (void)memory->write_u32(overlapped, STATUS_PENDING);
        (void)memory->write_u32(overlapped + 4, 0);
    }

    {
        ReapedWorkers::Finished finished;
        std::lock_guard<std::mutex> lock(mgr.m_overlapped_socket_io_mutex);
        mgr.track_pending_socket_io(listen_socket, overlapped);
        mgr.m_overlapped_socket_io_workers.spawn(
            [&mgr, listen_socket, accept_socket, listen_fd, accept_fd, memory,
             overlapped, is_64bit, output_buf, recv_len, local_len, remote_len,
             bytes_received_ptr](std::stop_token stop_token) {
                auto still_active = [&]() -> bool {
                    std::lock_guard<std::mutex> lk(
                        mgr.m_overlapped_socket_io_mutex);
                    auto it = mgr.m_pending_socket_io.find(overlapped);
                    return it != mgr.m_pending_socket_io.end() &&
                           !it->second.cancel;
                };
                auto finish = [&](int new_fd, sockaddr_storage remote,
                                  socklen_t remote_host_len, u32 nt_status,
                                  u32 win_error, u32 received = 0) {
                    const auto pending =
                        mgr.consume_pending_socket_io(overlapped);
                    if (!pending.has_value() || *pending) {
                        if (new_fd >= 0) ::close(new_fd);
                        return;
                    }
                    if (win_error == 0 && new_fd >= 0) {
                        ::close(accept_fd);
                        mgr.replace_native_fd(accept_socket, new_fd);

                        sockaddr_storage local_ss{};
                        socklen_t local_sslen = sizeof(local_ss);
                        (void)::getsockname(
                            new_fd, reinterpret_cast<sockaddr*>(&local_ss),
                            &local_sslen);
                        if (memory) {
                            write_acceptex_addrs(*memory, output_buf, recv_len,
                                                 local_len, remote_len, local_ss,
                                                 remote);
                        }
                        if (bytes_received_ptr && memory) {
                            (void)memory->write_u32(bytes_received_ptr, received);
                        }

                        MappedSockaddr peer{};
                        peer.ss = remote;
                        peer.len = remote_host_len;
                        char addr_str[INET6_ADDRSTRLEN] = {};
                        u16 port = 0;
                        format_mapped_sockaddr(peer, addr_str, sizeof(addr_str),
                                               &port);
                        ipc::ipc_log(
                            "AcceptEx complete listen=0x%x -> accept=0x%x "
                            "peer=%s:%u",
                            listen_socket, accept_socket, addr_str,
                            static_cast<unsigned>(port));
                    } else if (new_fd >= 0) {
                        ::close(new_fd);
                    }
                    mgr.notify_overlapped_completion(
                        static_cast<Handle>(listen_socket), received, overlapped,
                        is_64bit, nt_status, win_error,
                        /*completed_synchronously=*/false);
                };

                for (;;) {
                    if (stop_token.stop_requested() || !still_active()) {
                        (void)mgr.consume_pending_socket_io(overlapped);
                        return;
                    }
                    struct pollfd pfd{};
                    pfd.fd = listen_fd;
                    pfd.events = POLLIN | POLLERR | POLLHUP;
                    const int pr = ::poll(&pfd, 1, 50);
                    if (pr < 0) {
                        if (errno == EINTR) continue;
                        finish(-1, {}, 0, 0xC0000001u,
                               Ws2Manager::errno_to_wsa(errno));
                        return;
                    }
                    if (pr == 0) continue;
                    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                        finish(-1, {}, 0, 0xC0000001u, WSAENOTSOCK);
                        return;
                    }

                    sockaddr_storage remote{};
                    socklen_t rlen = sizeof(remote);
                    const int new_fd = ::accept(
                        listen_fd, reinterpret_cast<sockaddr*>(&remote), &rlen);
                    if (new_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK ||
                            errno == EINTR) {
                            continue;
                        }
                        finish(-1, {}, 0, 0xC0000001u,
                               Ws2Manager::errno_to_wsa(errno));
                        return;
                    }
                    if (recv_len == 0) {
                        finish(new_fd, remote, rlen, STATUS_SUCCESS, 0);
                        return;
                    }
                    // Wait for the first block of data (or the peer's close,
                    // which completes with 0 bytes), read at most
                    // dwReceiveDataLength into the front of lpOutputBuffer.
                    for (;;) {
                        if (stop_token.stop_requested() || !still_active()) {
                            ::close(new_fd);
                            (void)mgr.consume_pending_socket_io(overlapped);
                            return;
                        }
                        struct pollfd dfd{};
                        dfd.fd = new_fd;
                        dfd.events = POLLIN | POLLHUP | POLLERR;
                        const int dr = ::poll(&dfd, 1, 50);
                        if (dr < 0 && errno == EINTR) continue;
                        if (dr == 0) continue;
                        std::vector<u8> first(recv_len);
                        const ssize_t got =
                            ::recv(new_fd, first.data(), recv_len, MSG_DONTWAIT);
                        if (got < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK ||
                                errno == EINTR) {
                                continue;
                            }
                            const u32 wsa = Ws2Manager::errno_to_wsa(errno);
                            ::close(new_fd);
                            finish(-1, {}, 0, 0xC0000001u, wsa);
                            return;
                        }
                        if (got > 0 && memory) {
                            (void)memory->write_bytes(output_buf, first.data(),
                                                      static_cast<usize>(got));
                        }
                        finish(new_fd, remote, rlen, STATUS_SUCCESS, 0,
                               static_cast<u32>(got));
                        return;
                    }
                }
            }, finished);
    }

    mgr.set_last_error(WSA_IO_PENDING);
    ipc::ipc_log("AcceptEx listen=0x%x accept=0x%x -> IO_PENDING ov=0x%llx",
                 listen_socket, accept_socket,
                 static_cast<unsigned long long>(overlapped));
    m.set_return(0); // FALSE + ERROR_IO_PENDING
}

// -----------------------------------------------------------------------------
// GetAcceptExSockaddrs — Extract addresses from AcceptEx buffer
// -----------------------------------------------------------------------------
void ws2_GetAcceptExSockaddrs(Marshaler& m, Ws2Manager& /*mgr*/) {
    // void GetAcceptExSockaddrs(PVOID lpOutputBuffer,
    //                           DWORD dwReceiveDataLength,
    //                           DWORD dwLocalAddressLength,
    //                           DWORD dwRemoteAddressLength,
    //                           LPSOCKADDR* LocalSockaddr,
    //                           LPINT LocalSockaddrLength,
    //                           LPSOCKADDR* RemoteSockaddr,
    //                           LPINT RemoteSockaddrLength)
    GuestAddr buf_ptr = m.arg_ptr(0);
    u32 recv_len = static_cast<u32>(m.arg(1));
    u32 local_len = static_cast<u32>(m.arg(2));
    u32 remote_len = static_cast<u32>(m.arg(3));
    GuestAddr local_out_ptr = m.arg_ptr(4);
    GuestAddr local_len_out_ptr = m.arg_ptr(5);
    GuestAddr remote_out_ptr = m.arg_ptr(6);
    GuestAddr remote_len_out_ptr = m.arg_ptr(7);

    VITREO_TRACE(
        "[ws2_32] GetAcceptExSockaddrs(buf=0x%llx, recv=%u, local=%u, remote=%u)\n",
        static_cast<unsigned long long>(buf_ptr), recv_len, local_len,
        remote_len);

    if (local_out_ptr && local_len_out_ptr && local_len >= 16) {
        const GuestAddr local_addr = buf_ptr + recv_len;
        if (m.cpu_is_64bit()) {
            (void)m.write_u64(local_out_ptr, local_addr);
        } else {
            (void)m.write_u32(local_out_ptr, static_cast<u32>(local_addr));
        }
        // The reported length is the address's own size: 16 for AF_INET,
        // 28 for AF_INET6 (ws2_misc_probe acceptex_v4 / acceptex_v6).
        const u16 fam = m.read_u16(local_addr).value_or(kWinAfInet);
        (void)m.write_u32(local_len_out_ptr,
                          fam == kWinAfInet6 ? static_cast<u32>(kWinSockaddrIn6Bytes) : 16u);
    }

    if (remote_out_ptr && remote_len_out_ptr && remote_len >= 16) {
        const GuestAddr remote_addr = buf_ptr + recv_len + local_len;
        if (m.cpu_is_64bit()) {
            (void)m.write_u64(remote_out_ptr, remote_addr);
        } else {
            (void)m.write_u32(remote_out_ptr, static_cast<u32>(remote_addr));
        }
        const u16 fam = m.read_u16(remote_addr).value_or(kWinAfInet);
        (void)m.write_u32(remote_len_out_ptr,
                          fam == kWinAfInet6 ? static_cast<u32>(kWinSockaddrIn6Bytes) : 16u);
    }

    // No return value (void function)
}

} // namespace vitreo::winapi
