// SPDX-License-Identifier: LGPL-2.1-only

/// @file refresh_socket_events.cpp
/// @brief The WSAEventSelect provider: each association's network event record.
///
/// Learn (WSAEventSelect, WSAEnumNetworkEvents): a network event is recorded
/// in the socket's internal record -- and the associated event object
/// signaled -- when it occurs while selected and enabled. Recording disables
/// it; WSAEnumNetworkEvents returns and clears the record and re-enables, and
/// so do the re-enabling functions (recv for FD_READ/FD_OOB, a send that
/// failed WSAEWOULDBLOCK for FD_WRITE, accept for FD_ACCEPT), which also let
/// a condition that still holds be recorded again. A condition that merely
/// persists is not an occurrence.
///
/// Measured where Learn is silent (tools/oracle_probe/ws2_event_record_probe,
/// x64 and x86, identical):
/// - FD_READ is recorded again when new data arrives, read or not; a socket
///   whose data simply stays unread records nothing more.
/// - FD_WRITE is owed once per connection (connect, accept; a datagram socket
///   from the start) and again after WSAEWOULDBLOCK; a later WSAEventSelect
///   does not re-record it.
/// - FD_CLOSE is recorded with data still unread (FD_READ|FD_CLOSE together);
///   an abortive close reports it with WSAECONNABORTED.
/// - FD_ACCEPT on a socket that is not listening is never recorded.
///
/// The host has no network event callbacks, so the provider observes each
/// socket (poll, FIONREAD, the listen backlog, the TCP state) and turns what
/// changed since the last observation into these occurrences. Observation
/// runs in WSAWaitForMultipleEvents, WSAEnumNetworkEvents, WSAEventSelect and
/// a 10 ms background pump (for waits through kernel32).

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <vector>

#include <sys/types.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <poll.h>
#include <unistd.h>

namespace vitreo::winapi {

namespace {

constexpr u32 kFdReadBit = 0;
constexpr u32 kFdWriteBit = 1;
constexpr u32 kFdOobBit = 2;
constexpr u32 kFdAcceptBit = 3;
constexpr u32 kFdConnectBit = 4;
constexpr u32 kFdCloseBit = 5;
constexpr u32 kWsaEConnAborted = 10053;

constexpr u32 fd_bit(u32 index) { return 1u << index; }

/// Connections waiting on a listening socket. poll only says whether there
/// is one; kqueue's EVFILT_READ reports the listen backlog in `data`.
u64 pending_connections(int native_fd) {
    const int kq = ::kqueue();
    if (kq < 0) return 0;
    struct kevent change{};
    EV_SET(&change, native_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    struct kevent result{};
    const struct timespec zero{0, 0};
    const int n = ::kevent(kq, &change, 1, &result, 1, &zero);
    ::close(kq);
    if (n <= 0 || (result.flags & EV_ERROR) != 0 || result.data <= 0) return 0;
    return static_cast<u64>(result.data);
}

enum class PeerClose { Open, Graceful, Reset };

/// Whether the peer has closed the circuit, with data still buffered or not
/// (poll and FIONREAD cannot tell a FIN behind unread data).
PeerClose peer_close(int native_fd) {
    char byte = 0;
    const ssize_t peeked =
        ::recv(native_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    const int peek_errno = errno;
    if (peeked < 0 && peek_errno == ECONNRESET) return PeerClose::Reset;
    tcp_connection_info info{};
    socklen_t len = sizeof(info);
    if (::getsockopt(native_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info,
                     &len) == 0) {
        switch (info.tcpi_state) {
        case TCPS_CLOSE_WAIT:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
            return PeerClose::Graceful;
        case TCPS_CLOSED:
            return peeked == 0 ? PeerClose::Graceful : PeerClose::Reset;
        default:
            break;
        }
    }
    return peeked == 0 ? PeerClose::Graceful : PeerClose::Open;
}

}  // namespace

Ws2Manager::EventSource& Ws2Manager::event_source_locked(u32 guest_socket,
                                                         int native_fd) {
    auto [it, created] = m_event_sources.try_emplace(guest_socket);
    EventSource& source = it->second;
    if (!created || native_fd < 0) return source;
    int type = 0;
    socklen_t type_len = sizeof(type);
    (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &type, &type_len);
    source.stream = type == SOCK_STREAM;
    if (!source.stream) {
        // A datagram socket can send from the start (ws2_event_record_probe
        // udp_select_enum: FD_WRITE on the first WSAEventSelect).
        source.write_owed = true;
    } else if (!m_pending_connect.contains(guest_socket)) {
        // Connected before the provider saw it (a blocking connect, a path
        // with no hook): its FD_WRITE has not been recorded yet.
        sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        if (::getpeername(native_fd, reinterpret_cast<sockaddr*>(&peer),
                          &peer_len) == 0) {
            source.connected = true;
            source.write_owed = true;
        }
    }
    return source;
}

u32 Ws2Manager::observe_socket_locked(u32 guest_socket, EventAssoc& assoc,
                                      bool selecting) {
    const int native_fd = get_native_fd(guest_socket);
    if (native_fd < 0) return 0;
    EventSource& source = event_source_locked(guest_socket, native_fd);

    pollfd pfd{native_fd, static_cast<short>(POLLIN | POLLOUT | POLLPRI), 0};
    const short revents = ::poll(&pfd, 1, 0) > 0 ? pfd.revents : 0;

    u32 newly = 0;
    const auto record = [&](u32 index, u32 error) {
        const u32 bit = fd_bit(index);
        if ((assoc.event_mask & bit) == 0 || (assoc.disabled & bit) != 0) {
            return;
        }
        assoc.recorded |= bit;
        assoc.disabled |= bit;
        assoc.errors[index] = error;
        newly |= bit;
    };

    // FD_ACCEPT: a listening socket's pending connection -- on arrival, at the
    // select, and after an accept that leaves one pending.
    if (m_listening.contains(guest_socket)) {
        const u64 backlog =
            (revents & POLLIN) != 0
                ? std::max<u64>(1, pending_connections(native_fd))
                : 0;
        if (backlog > 0 && (selecting || source.accept_reenabled ||
                            backlog > source.seen_backlog)) {
            record(kFdAcceptBit, 0);
        }
        source.seen_backlog = backlog;
        source.accept_reenabled = false;
        return newly;
    }

    // FD_CONNECT: once, when a nonblocking connect completes, with its error.
    // A failed connect never had a circuit: no FD_WRITE, no FD_CLOSE
    // (ws2_event_record_probe refused_enum: 0x10 err_connect=10061).
    if (m_pending_connect.contains(guest_socket)) {
        if ((revents & (POLLOUT | POLLERR | POLLHUP)) == 0) return newly;
        bool still_pending = false;
        const u32 error =
            latch_connect_error(guest_socket, native_fd, &still_pending);
        if (still_pending) return newly;
        m_pending_connect.erase(guest_socket);
        record(kFdConnectBit, error);
        if (error != 0) {
            source.close_seen = true;
            return newly;
        }
        source.connected = true;
        source.write_owed = true;
    }

    // FD_READ: data arrived, the select found data, or a recv-family call
    // re-enabled it with data still buffered.
    int available = 0;
    if ((revents & POLLIN) != 0 &&
        ::ioctl(native_fd, FIONREAD, &available) < 0) {
        available = 0;
    }
    const u64 readable = available > 0 ? static_cast<u64>(available) : 0;
    if (readable > 0 && (selecting || source.read_reenabled ||
                         readable > source.seen_readable)) {
        record(kFdReadBit, 0);
    }
    source.seen_readable = readable;
    // FD_OOB: urgent data, on the same triggers.
    const bool urgent = (revents & POLLPRI) != 0;
    if (urgent && (selecting || source.read_reenabled || !source.oob_seen)) {
        record(kFdOobBit, 0);
    }
    source.oob_seen = urgent;
    source.read_reenabled = false;

    // FD_WRITE: owed once the socket can send (connected, accepted, a
    // datagram socket), and again when space returns after WSAEWOULDBLOCK.
    // Not re-recorded by a later select (reselect_first_wait).
    if (source.send_blocked && (revents & POLLOUT) != 0) {
        source.send_blocked = false;
        source.write_owed = true;
    }
    if (source.write_owed && (revents & POLLOUT) != 0 &&
        (assoc.event_mask & fd_bit(kFdWriteBit)) != 0) {
        record(kFdWriteBit, 0);
        source.write_owed = false;
    }

    // FD_CLOSE: once per circuit when the peer closes, unread data or not;
    // and at a select made after it closed.
    if (source.stream && source.connected) {
        if (!source.close_seen &&
            (revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const PeerClose close = peer_close(native_fd);
            if (close != PeerClose::Open) {
                source.close_seen = true;
                source.close_error =
                    close == PeerClose::Reset ? kWsaEConnAborted : 0;
                record(kFdCloseBit, source.close_error);
            }
        } else if (selecting && source.close_seen) {
            record(kFdCloseBit, source.close_error);
        }
    }
    return newly;
}

void Ws2Manager::observe_and_signal_locked(u32 guest_socket,
                                           EventAssoc& assoc, bool selecting) {
    const u32 newly = observe_socket_locked(guest_socket, assoc, selecting);
    if (newly == 0 || m_sync_manager == nullptr) return;
    (void)m_sync_manager->set_event(static_cast<Handle>(assoc.event_handle));
    if (wsa_event_diag_enabled()) {
        std::fprintf(stderr,
                     "[ws2-event] record socket=0x%x event=0x%x mask=0x%x "
                     "new=0x%x record=0x%x%s\n",
                     guest_socket, assoc.event_handle, assoc.event_mask,
                     newly, assoc.recorded, selecting ? " (select)" : "");
    }
}

bool Ws2Manager::refresh_socket_events(std::span<const Handle> handles) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    bool any = false;
    for (auto& [guest_socket, assoc] : m_event_assocs) {
        if (std::find(handles.begin(), handles.end(),
                      static_cast<Handle>(assoc.event_handle)) ==
            handles.end()) {
            continue;
        }
        any = true;
        observe_and_signal_locked(guest_socket, assoc, /*selecting=*/false);
    }
    return any;
}

void Ws2Manager::refresh_all_socket_events() {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    for (auto& [guest_socket, assoc] : m_event_assocs) {
        observe_and_signal_locked(guest_socket, assoc, /*selecting=*/false);
    }
}

void Ws2Manager::select_network_events(u32 guest_socket, u32 event_handle,
                                       u32 event_mask) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    EventAssoc& assoc = m_event_assocs[guest_socket];
    assoc = EventAssoc{event_handle, event_mask};
    observe_and_signal_locked(guest_socket, assoc, /*selecting=*/true);
    ensure_event_pump_locked();
    m_event_pump_cv.notify_all();
}

void Ws2Manager::cancel_network_events(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_event_assocs.erase(guest_socket);
}

void Ws2Manager::take_network_events(u32 guest_socket, Handle reset_event,
                                     u32* events,
                                     std::array<u32, 10>* errors) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    *events = 0;
    auto it = m_event_assocs.find(guest_socket);
    if (it != m_event_assocs.end()) {
        observe_and_signal_locked(guest_socket, it->second,
                                  /*selecting=*/false);
        *events = it->second.recorded;
        *errors = it->second.errors;
        // Clearing the record re-enables recording: data that arrives next is
        // recorded with no recv in between (ws2_event_record_probe
        // loop_more_data_before_recv), while data that merely stays is not
        // (loop_drain_before_recv).
        it->second.recorded = 0;
        it->second.disabled = 0;
        it->second.errors = {};
    }
    // Learn WSAEnumNetworkEvents: hEventObject, when given, is reset with the
    // record; NULL leaves the associated event as it is (reset_after_enum_null:
    // signaled=1).
    if (reset_event != 0 && m_sync_manager != nullptr) {
        (void)m_sync_manager->reset_event(reset_event);
    }
}

void Ws2Manager::reenable_fd_read(u32 guest_socket, int native_fd) {
    (void)native_fd;
    std::lock_guard<std::mutex> lock(m_event_mutex);
    if (auto it = m_event_assocs.find(guest_socket); it != m_event_assocs.end()) {
        it->second.disabled &= ~(fd_bit(kFdReadBit) | fd_bit(kFdOobBit));
    }
    if (auto it = m_event_sources.find(guest_socket);
        it != m_event_sources.end()) {
        it->second.read_reenabled = true;
    }
}

void Ws2Manager::arm_fd_write(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    if (auto it = m_event_assocs.find(guest_socket); it != m_event_assocs.end()) {
        it->second.disabled &= ~fd_bit(kFdWriteBit);
    }
    if (auto it = m_event_sources.find(guest_socket);
        it != m_event_sources.end()) {
        it->second.send_blocked = true;
    }
}

void Ws2Manager::reenable_fd_accept(u32 listener) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    if (auto it = m_event_assocs.find(listener); it != m_event_assocs.end()) {
        it->second.disabled &= ~fd_bit(kFdAcceptBit);
    }
    if (auto it = m_event_sources.find(listener);
        it != m_event_sources.end()) {
        it->second.accept_reenabled = true;
    }
}

void Ws2Manager::note_stream_connected(u32 guest_socket) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_pending_connect.erase(guest_socket);
    if (auto it = m_event_sources.find(guest_socket);
        it != m_event_sources.end()) {
        it->second.connected = true;
        it->second.write_owed = true;
        it->second.close_seen = false;
    }
}

} // namespace vitreo::winapi
