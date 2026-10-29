// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32_overlapped.cpp
/// @brief The WSA layer: overlapped I/O, WSAPoll, and the extension functions.
///
/// WSASend / WSARecv and their From variants, WSAIoctl, WSAPoll, WSAConnect and
/// WSAAccept, plus ConnectEx and DisconnectEx — which a guest does not link
/// against but retrieves at runtime through
/// WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER), which is why they live beside
/// the ioctl that hands them out.
///
/// These are not the BSD calls with extra arguments. Each takes a WSABUF array
/// rather than one buffer, and each can complete asynchronously through an
/// OVERLAPPED whose Internal field carries an NTSTATUS. That difference is the
/// file boundary: ws2_32.cpp is the synchronous socket, this is the Windows
/// completion model over it.
///
/// Split out of ws2_32.cpp. The WSAPOLLFD event bits came with WSAPoll because
/// they exist only for it — and they are Windows values, not the POSIX poll
/// constants they resemble.

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
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <CommonCrypto/CommonDigest.h>
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

/// @file wsaconnect_connect_optional_send_recv.cpp
/// @brief WSAConnect — Connect with optional send/recv data
///
/// Closed cut from ws2_32_overlapped.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// WSAConnect — Connect with optional send/recv data
// -----------------------------------------------------------------------------
void ws2::impl_WSAConnect(Marshaler& m, Ws2Manager& mgr) {
    // int WSAConnect(SOCKET s, const struct sockaddr* name, int namelen,
    //                LPWSABUF lpCallerData, LPWSABUF lpCalleeData,
    //                LPQOS lpSQOS, LPQOS lpGQOS)
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr addr_ptr = m.arg_ptr(1);
    int addrlen = static_cast<int>(m.arg(2));
    // args 3-6: optional data/QOS - ignored
    
    VITREO_TRACE("[ws2_32] WSAConnect(socket=0x%x, addrlen=%d)\n", guest_socket, addrlen);
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    MappedSockaddr mapped{};
    if (!read_win_sockaddr_mapped(m, addr_ptr, addrlen, mapped)) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    char addr_str[INET6_ADDRSTRLEN] = {};
    u16 port = 0;
    format_mapped_sockaddr(mapped, addr_str, sizeof(addr_str), &port);
    ipc::ipc_log("WSAConnect socket=0x%x -> %s:%u",
                 guest_socket, addr_str, static_cast<unsigned>(port));
    
    int rc = ::connect(native_fd, reinterpret_cast<sockaddr*>(&mapped.ss),
                       mapped.len);
    const int connect_errno = errno;
    int host_type = 0;
    socklen_t type_len = sizeof(host_type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &host_type, &type_len);
    // Learn WSAEventSelect: a WSAConnect connection records FD_CONNECT when
    // it completes and owes FD_WRITE, as connect does.
    if (host_type == SOCK_STREAM) {
        if (rc == 0) {
            mgr.note_stream_connected(guest_socket);
        } else if (connect_errno == EINPROGRESS) {
            mgr.mark_pending_connect(guest_socket);
        }
    }
    errno = connect_errno;
    if (rc < 0) {
        u32 wsa_err = (errno == EINPROGRESS) ? WSAEWOULDBLOCK : Ws2Manager::errno_to_wsa(errno);
        mgr.set_last_error(wsa_err);
        ipc::ipc_log("WSAConnect %s:%u -> SOCKET_ERROR wsa=%u",
                     addr_str, static_cast<unsigned>(port), wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    } else {
        ipc::ipc_log("WSAConnect %s:%u -> OK",
                     addr_str, static_cast<unsigned>(port));
        m.set_return(0);
    }
}

} // namespace vitreo::winapi
