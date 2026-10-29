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
#include "winapi/kernel32/filesystem/file.hpp"
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

namespace vitreo::winapi {

/// ConnectEx stub - performs synchronous connect and returns immediately
/// BOOL ConnectEx(SOCKET s, const sockaddr* name, int namelen, 
///                PVOID lpSendBuffer, DWORD dwSendDataLength, 
///                LPDWORD lpdwBytesSent, LPOVERLAPPED lpOverlapped)

/// DisconnectEx stub - performs graceful disconnect
/// BOOL DisconnectEx(SOCKET s, LPOVERLAPPED lpOverlapped, DWORD dwFlags, DWORD dwReserved)
void ws2::impl_DisconnectEx(Marshaler& m, Ws2Manager& mgr) {
    u32 guest_socket = static_cast<u32>(m.arg(0));
    const GuestAddr overlapped = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    // BOOL DisconnectEx(SOCKET s, LPOVERLAPPED, DWORD dwFlags, DWORD reserved)
    // TF_REUSE_SOCKET (0x02): after shutdown the socket must be reconnectable;
    // Darwin needs a fresh fd for that, so replace the host descriptor.
    constexpr u32 kTfReuseSocket = 0x02;

    VITREO_TRACE( "[ws2_32] DisconnectEx(socket=0x%x flags=0x%x)\n", guest_socket, flags);

    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(0);  // FALSE
        return;
    }

    ::shutdown(native_fd, SHUT_RDWR);
    if ((flags & kTfReuseSocket) != 0) {
        int domain = AF_INET, type = SOCK_STREAM, protocol = 0;
        sockaddr_storage ss{};
        socklen_t sl = sizeof(ss);
        if (::getsockname(native_fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0) {
            domain = ss.ss_family;
        }
        socklen_t tl = sizeof(type);
        (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &type, &tl);
        const int fresh = ::socket(domain, type, protocol);
        if (fresh >= 0) {
            mgr.replace_native_fd(guest_socket, fresh);
            ::close(native_fd);
        }
    }

    // Immediate completion (sync shutdown): post OVERLAPPED like WSAIoctl.
    if (overlapped != 0) {
        mgr.complete_socket_request(guest_socket, overlapped, m.cpu_is_64bit(),
                                    /*completion_routine=*/0, /*bytes=*/0,
                                    STATUS_SUCCESS, 0, 0,
                                    /*completed_synchronously=*/true);
    }

    VITREO_TRACE( "[ws2_32] DisconnectEx: success\n");
    m.set_return(1);  // TRUE
}

/// BOOL TransmitFile(SOCKET, HANDLE, DWORD nNumberOfBytesToWrite,
///   DWORD nNumberOfBytesPerSend, LPOVERLAPPED, LPTRANSMIT_FILE_BUFFERS, DWORD dwFlags)
/// Learn mswsock TransmitFile + ws2_transmit_probe.expected: head, file bytes
/// from the current pointer (pointer unchanged), tail, then TF_DISCONNECT/REUSE.
void ws2::impl_TransmitFile(Marshaler& m, Ws2Manager& mgr) {
    const u32 guest_socket = static_cast<u32>(m.arg(0));
    const u64 hfile = m.arg(1);
    const u32 nbytes = m.arg32(2);  // nNumberOfBytesToWrite (0 = rest of file)
    (void)m.arg32(3);  // nNumberOfBytesPerSend — advisory
    const GuestAddr overlapped = m.arg_ptr(4);
    const GuestAddr buffers = m.arg_ptr(5);
    const u32 flags = m.arg32(6);
    constexpr u32 kTfDisconnect = 0x01;
    constexpr u32 kTfReuseSocket = 0x02;
    const bool have_file = hfile != 0 && hfile != ~u64{0} && hfile != 0xFFFFFFFFull;

    int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(0);
        return;
    }
    if (overlapped != 0) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ws2_32!TransmitFile: overlapped\n");
        mgr.set_last_error(WSAEOPNOTSUPP);
        m.set_return(0);
        return;
    }

    auto send_all = [&](const u8* data, usize len) -> bool {
        usize off = 0;
        while (off < len) {
            const ssize_t n = ::send(fd, data + off, len - off, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
                return false;
            }
            if (n == 0) break;
            off += static_cast<usize>(n);
        }
        return true;
    };

    GuestAddr head_ptr = 0, tail_ptr = 0;
    u32 head_len = 0, tail_len = 0;
    if (buffers != 0) {
        const bool is64 = m.cpu_is_64bit();
        head_ptr = is64 ? m.read_u64(buffers).value_or(0)
                        : m.read_u32(buffers).value_or(0);
        head_len = m.read_u32(buffers + (is64 ? 8 : 4)).value_or(0);
        const GuestAddr tail_off = is64 ? 16 : 8;
        tail_ptr = is64 ? m.read_u64(buffers + tail_off).value_or(0)
                        : m.read_u32(buffers + tail_off).value_or(0);
        tail_len = m.read_u32(buffers + tail_off + (is64 ? 8 : 4)).value_or(0);
    }
    if (head_ptr != 0 && head_len != 0) {
        std::vector<u8> head(head_len);
        if (m.memory().read_bytes(head_ptr, head.data(), head_len).is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(0);
            return;
        }
        if (!send_all(head.data(), head.size())) {
            m.set_return(0);
            return;
        }
    }
    if (have_file) {
        FileManager* files = mgr.file_manager();
        if (!files) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ws2_32!TransmitFile: no FileManager\n");
            mgr.set_last_error(WSAEOPNOTSUPP);
            m.set_return(0);
            return;
        }
        std::vector<u8> file_bytes;
        if (!files->read_handle_bytes(static_cast<Handle>(hfile), file_bytes,
                                      nbytes)) {
            mgr.set_last_error(WSAEINVAL);
            m.set_return(0);
            return;
        }
        if (!file_bytes.empty() &&
            !send_all(file_bytes.data(), file_bytes.size())) {
            m.set_return(0);
            return;
        }
    }
    if (tail_ptr != 0 && tail_len != 0) {
        std::vector<u8> tail(tail_len);
        if (m.memory().read_bytes(tail_ptr, tail.data(), tail_len).is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(0);
            return;
        }
        if (!send_all(tail.data(), tail.size())) {
            m.set_return(0);
            return;
        }
    }

    // TF_REUSE implies TF_DISCONNECT (Learn). Do not wait for peer FIN.
    if ((flags & (kTfDisconnect | kTfReuseSocket)) != 0) {
        ::shutdown(fd, SHUT_RDWR);
        if ((flags & kTfReuseSocket) != 0) {
            int domain = AF_INET, type = SOCK_STREAM;
            sockaddr_storage ss{};
            socklen_t sl = sizeof(ss);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0)
                domain = ss.ss_family;
            socklen_t tl = sizeof(type);
            (void)::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl);
            const int fresh = ::socket(domain, type, 0);
            if (fresh >= 0) {
                mgr.replace_native_fd(guest_socket, fresh);
                ::close(fd);
            }
        }
    }
    m.set_return(1);
}

/// TransmitPackets — Learn mswsock + ws2_transmit_probe: TP_ELEMENT_MEMORY /
/// TP_ELEMENT_FILE (cLength 0 = rest from nFileOffset); TP_DISCONNECT/REUSE.
void ws2::impl_TransmitPackets(Marshaler& m, Ws2Manager& mgr) {
    const u32 guest_socket = static_cast<u32>(m.arg(0));
    const GuestAddr packet_array = m.arg_ptr(1);
    const u32 element_count = m.arg32(2);
    (void)m.arg32(3);  // nSendSize — advisory
    const GuestAddr overlapped = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    constexpr u32 kTpDisconnect = 0x01;
    constexpr u32 kTpReuseSocket = 0x02;
    constexpr u32 kTpElementMemory = 1;
    constexpr u32 kTpElementFile = 2;
    constexpr u32 kElementSize = 24;  // TRANSMIT_PACKETS_ELEMENT x86/x64

    int fd = mgr.get_native_fd(guest_socket);
    if (fd < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(0);
        return;
    }
    if (overlapped != 0) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED ws2_32!TransmitPackets: overlapped\n");
        mgr.set_last_error(WSAEOPNOTSUPP);
        m.set_return(0);
        return;
    }
    if (element_count != 0 && packet_array == 0) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(0);
        return;
    }

    auto send_all = [&](const u8* data, usize len) -> bool {
        usize off = 0;
        while (off < len) {
            const ssize_t n = ::send(fd, data + off, len - off, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
                return false;
            }
            if (n == 0) break;
            off += static_cast<usize>(n);
        }
        return true;
    };

    FileManager* files = mgr.file_manager();
    for (u32 i = 0; i < element_count; ++i) {
        const GuestAddr el = packet_array + static_cast<GuestAddr>(i) * kElementSize;
        const u32 el_flags = m.read_u32(el).value_or(0);
        const u32 c_length = m.read_u32(el + 4).value_or(0);
        if ((el_flags & kTpElementMemory) != 0) {
            const GuestAddr buf =
                m.cpu_is_64bit() ? m.read_u64(el + 8).value_or(0)
                                 : m.read_u32(el + 8).value_or(0);
            if (c_length != 0) {
                if (buf == 0) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(0);
                    return;
                }
                std::vector<u8> mem(c_length);
                if (m.memory().read_bytes(buf, mem.data(), c_length).is_err()) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(0);
                    return;
                }
                if (!send_all(mem.data(), mem.size())) {
                    m.set_return(0);
                    return;
                }
            }
        } else if ((el_flags & kTpElementFile) != 0) {
            if (!files) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED ws2_32!TransmitPackets: "
                             "no FileManager\n");
                mgr.set_last_error(WSAEOPNOTSUPP);
                m.set_return(0);
                return;
            }
            const u64 offset = m.read_u64(el + 8).value_or(0);
            const u64 hfile =
                m.cpu_is_64bit() ? m.read_u64(el + 16).value_or(0)
                                 : m.read_u32(el + 16).value_or(0);
            std::vector<u8> file_bytes;
            if (!files->read_handle_bytes(static_cast<Handle>(hfile), file_bytes,
                                          c_length, offset)) {
                mgr.set_last_error(WSAEINVAL);
                m.set_return(0);
                return;
            }
            if (!file_bytes.empty() &&
                !send_all(file_bytes.data(), file_bytes.size())) {
                m.set_return(0);
                return;
            }
        } else {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ws2_32!TransmitPackets: "
                         "element flags 0x%x\n",
                         el_flags);
            mgr.set_last_error(WSAEOPNOTSUPP);
            m.set_return(0);
            return;
        }
    }

    if ((flags & (kTpDisconnect | kTpReuseSocket)) != 0) {
        ::shutdown(fd, SHUT_RDWR);
        if ((flags & kTpReuseSocket) != 0) {
            int domain = AF_INET, type = SOCK_STREAM;
            sockaddr_storage ss{};
            socklen_t sl = sizeof(ss);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0)
                domain = ss.ss_family;
            socklen_t tl = sizeof(type);
            (void)::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl);
            const int fresh = ::socket(domain, type, 0);
            if (fresh >= 0) {
                mgr.replace_native_fd(guest_socket, fresh);
                ::close(fd);
            }
        }
    }
    m.set_return(1);
}

void ws2::impl_WSAAccept(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::InvalidSocket)) return;

    // SOCKET WSAAccept(SOCKET, sockaddr*, LPINT, LPCONDITIONPROC, DWORD_PTR)
    // Condition callbacks are not implemented; NULL condition → accept().
    GuestAddr cond = m.arg_ptr(3);
    (void)m.arg(4);
    if (cond != 0) {
        mgr.set_last_error(WSAEOPNOTSUPP);
        set_invalid_socket_return(m);
        ipc::ipc_log("WSAAccept condition callback not supported");
        return;
    }
    impl_accept(m, mgr);
}

// Windows WSAPOLLFD event bits (winsock2.h) — not POSIX poll values.
static constexpr u16 WIN_POLLRDNORM = 0x0100;
static constexpr u16 WIN_POLLRDBAND = 0x0200;
static constexpr u16 WIN_POLLIN = static_cast<u16>(WIN_POLLRDNORM | WIN_POLLRDBAND);
static constexpr u16 WIN_POLLWRNORM = 0x0010;
static constexpr u16 WIN_POLLOUT = WIN_POLLWRNORM;
static constexpr u16 WIN_POLLERR = 0x0001;
static constexpr u16 WIN_POLLHUP = 0x0002;
static constexpr u16 WIN_POLLNVAL = 0x0004;
static constexpr u16 WIN_POLLPRI = 0x0400;

void ws2::impl_WSAPoll(Marshaler& m, Ws2Manager& mgr) {
    // int WSAPoll(LPWSAPOLLFD fdArray, ULONG fds, INT timeout)
    // WSAPOLLFD (x86): SOCKET fd (4) + SHORT events (2) + SHORT revents (2)
    GuestAddr arr = m.arg_ptr(0);
    const u32 fds = static_cast<u32>(m.arg(1));
    const i32 timeout_ms = static_cast<i32>(m.arg(2));
    if (!arr || fds == 0 || fds > 1024) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    std::vector<pollfd> native(fds);
    bool any_valid = false;
    for (u32 i = 0; i < fds; ++i) {
        const GuestAddr e = arr + static_cast<GuestAddr>(i) * 8;
        const u32 sock = m.read_u32(e).value_or(0xFFFFFFFFu);
        const u16 events = m.read_u16(e + 4).value_or(0);
        (void)m.write_u16(e + 6, 0); // clear revents
        native[i].fd = -1;
        native[i].events = 0;
        native[i].revents = 0;
        if (static_cast<i32>(sock) < 0) {
            (void)m.write_u16(e + 6, WIN_POLLNVAL);
            continue;
        }
        if (events & WIN_POLLPRI) {
            mgr.set_last_error(WSAEINVAL);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        const int nfd = mgr.get_native_fd(sock);
        if (nfd < 0) {
            (void)m.write_u16(e + 6, WIN_POLLNVAL);
            continue;
        }
        any_valid = true;
        native[i].fd = nfd;
        short nev = 0;
        if (events & (WIN_POLLIN | WIN_POLLRDNORM | WIN_POLLRDBAND)) {
            nev = static_cast<short>(nev | POLLIN);
        }
        if (events & (WIN_POLLOUT | WIN_POLLWRNORM)) {
            nev = static_cast<short>(nev | POLLOUT);
        }
        native[i].events = nev;
    }
    if (!any_valid) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    const int rc = ::poll(native.data(), static_cast<nfds_t>(fds), timeout_ms);
    if (rc < 0) {
        mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (rc == 0) {
        m.set_return(0);
        return;
    }

    u32 ready = 0;
    for (u32 i = 0; i < fds; ++i) {
        const GuestAddr e = arr + static_cast<GuestAddr>(i) * 8;
        u16 revents = m.read_u16(e + 6).value_or(0);
        if (revents & WIN_POLLNVAL) {
            ++ready;
            continue;
        }
        if (native[i].fd < 0) continue;
        const short nr = native[i].revents;
        u16 wr = 0;
        if (nr & POLLIN) wr = static_cast<u16>(wr | WIN_POLLRDNORM);
        if (nr & POLLOUT) wr = static_cast<u16>(wr | WIN_POLLWRNORM);
        if (nr & POLLERR) wr = static_cast<u16>(wr | WIN_POLLERR);
        if (nr & POLLHUP) wr = static_cast<u16>(wr | WIN_POLLHUP);
        if (nr & POLLNVAL) wr = static_cast<u16>(wr | WIN_POLLNVAL);
        (void)m.write_u16(e + 6, wr);
        if (wr != 0) ++ready;
    }
    m.set_return(static_cast<u64>(ready));
}







} // namespace vitreo::winapi
