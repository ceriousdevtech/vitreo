// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file begin_overlapped_send.cpp
/// @brief Ws2Manager::begin_overlapped_send
///
/// Microsoft Learn, WSASend: on an overlapped socket a send that cannot be
/// completed immediately returns SOCKET_ERROR with WSA_IO_PENDING, and the
/// operation finishes later by signalling the OVERLAPPED's hEvent (or posting
/// to the associated completion port). It does NOT return WSAEWOULDBLOCK --
/// that is the non-overlapped contract, and it tells the caller "nothing was
/// queued, do it yourself", which an overlapped caller is not written to do.
///
/// Vitreo used to fall through to WSAEWOULDBLOCK here, with a comment saying
/// there was no deferred-send worker yet. The visible cost: a content
/// downloader opens an HTTP/2 client, and its request send hits a full
/// socket buffer, so the fetch stalls until the caller gives up with a
/// connection timeout and the transfer never starts. Same shape as the
/// recorded WSARecv / WSARecvFrom overlapped fixes.

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>

namespace vitreo::winapi {

namespace {
constexpr u32 kSendStatusPending = 0x00000103u;
constexpr u32 kSendStatusSuccess = 0x00000000u;
constexpr u32 kSendStatusUnsuccessful = 0xC0000001u;
}  // namespace

bool Ws2Manager::begin_overlapped_send(
    u32 guest_socket, int native_fd, std::vector<char> remaining,
    u32 already_sent, int send_flags, GuestAddr overlapped, bool is_64bit,
    GuestAddr bytes_sent_ptr, Marshaler& m,
    GuestAddr completion_routine, u32 issuing_tid) {
    GuestMemory* memory = m_memory ? m_memory : &m.memory();
    if (!memory || overlapped == 0) {
        set_last_error(WSAEINVAL);
        return false;
    }

    // Windows leaves lpNumberOfBytesSent alone for a pending overlapped send;
    // the real count arrives with the completion.
    const auto pending_write = is_64bit
        ? memory->write_u64(overlapped, kSendStatusPending)
        : memory->write_u32(overlapped, kSendStatusPending);
    const auto count_write = is_64bit
        ? memory->write_u64(overlapped + 8, 0)
        : memory->write_u32(overlapped + 4, 0);
    if (pending_write.is_err() || count_write.is_err()) {
        set_last_error(WSAEFAULT);
        return false;
    }
    // Learn: leave *lpNumberOfBytesSent alone for a pending overlapped send.

    {
        ReapedWorkers::Finished finished;
        std::lock_guard<std::mutex> lock(m_overlapped_socket_io_mutex);
        track_pending_socket_io(guest_socket, overlapped, issuing_tid, completion_routine, is_64bit);
        m_overlapped_socket_io_workers.spawn(
            [this, guest_socket, native_fd, overlapped, is_64bit, send_flags,
             already_sent, completion_routine, issuing_tid,
             buf = std::move(remaining)](
                std::stop_token stop_token) mutable {
                auto still_active = [&]() -> bool {
                    std::lock_guard<std::mutex> lk(m_overlapped_socket_io_mutex);
                    auto it = m_pending_socket_io.find(overlapped);
                    if (it == m_pending_socket_io.end()) return false;
                    return !it->second.cancel;
                };
                usize off = 0;
                for (;;) {
                    if (stop_token.stop_requested() || !still_active()) {
                        (void)consume_pending_socket_io(overlapped);
                        return;
                    }
                    struct pollfd pfd{};
                    pfd.fd = native_fd;
                    pfd.events = POLLOUT | POLLERR | POLLHUP;
                    const int pr = ::poll(&pfd, 1, 50);
                    if (pr < 0) {
                        if (errno == EINTR) continue;
                        const auto pending = consume_pending_socket_io(overlapped);
                        if (!pending.has_value() || *pending) return;
                        notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), 0, overlapped,
                            is_64bit, kSendStatusUnsuccessful,
                            errno_to_wsa(errno), false, completion_routine, issuing_tid);
                        return;
                    }
                    if (pr == 0) continue;

                    if (get_native_fd(guest_socket) != native_fd) {
                        const auto pending = consume_pending_socket_io(overlapped);
                        if (!pending.has_value() || *pending) return;
                        notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), 0, overlapped,
                            is_64bit, STATUS_CANCELLED, WSA_OPERATION_ABORTED,
                            false, completion_routine, issuing_tid);
                        return;
                    }

                    const ssize_t n = ::send(
                        native_fd, buf.data() + off, buf.size() - off,
                        send_flags);
                    const int err = errno;
                    if (n < 0) {
                        if (err == EAGAIN || err == EWOULDBLOCK ||
                            err == EINTR) {
                            continue;
                        }
                        const auto pending = consume_pending_socket_io(overlapped);
                        if (!pending.has_value() || *pending) return;
                        ipc::ipc_log(
                            "WSASend complete socket=0x%x ov=0x%llx error=%u",
                            guest_socket,
                            static_cast<unsigned long long>(overlapped),
                            errno_to_wsa(err));
                        notify_overlapped_completion(
                            static_cast<Handle>(guest_socket), 0, overlapped,
                            is_64bit, kSendStatusUnsuccessful, errno_to_wsa(err),
                            false, completion_routine, issuing_tid);
                        return;
                    }
                    off += static_cast<usize>(n);
                    if (off < buf.size()) continue;  // partial: keep going

                    const auto pending = consume_pending_socket_io(overlapped);
                    if (!pending.has_value() || *pending) return;
                    const u32 total = already_sent +
                                      static_cast<u32>(buf.size());
                    ipc::ipc_log(
                        "WSASend complete socket=0x%x ov=0x%llx bytes=%u",
                        guest_socket,
                        static_cast<unsigned long long>(overlapped), total);
                    notify_overlapped_completion(
                        static_cast<Handle>(guest_socket), total, overlapped,
                        is_64bit, kSendStatusSuccess, 0, false, completion_routine, issuing_tid);
                    return;
                }
            }, finished);
    }
    ipc::ipc_log("WSASend socket=0x%x ov=0x%llx -> IO_PENDING (%zu deferred)",
                 guest_socket, static_cast<unsigned long long>(overlapped),
                 static_cast<usize>(0));
    set_last_error(WSA_IO_PENDING);
    return true;
}

} // namespace vitreo::winapi
