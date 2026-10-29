// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32_registration.cpp
/// @brief Export table for ws2_32.
///
/// Composition only: maps export names and ordinals to the handlers in
/// ws2_32.cpp and records each one's stdcall byte count. Winsock is the DLL
/// where ordinals matter most — the BSD socket calls are reached by ordinal 1
/// through 23 by design, and a guest that resolves them that way gets whatever
/// this table says ordinal 3 is.
///
/// Split out of ws2_32.cpp, which was 6,077 lines with a sixth of that being
/// this table.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh — which for this file also
/// checks the ordinals, not just the names.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export, each
/// naming a handler and its stdcall byte count. Plan section 4.1 exempts these
/// because length here tracks how many Winsock exports Vitreo implements, not
/// how much logic one file carries.

#include <array>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "ipc/ipc_gate.hpp"

#include <vitreo/common.hpp>

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace vitreo::winapi {

using namespace ws2;

namespace {

struct NamespaceLookup {
    u32 name_space{};
    bool yielded{};
};

struct NamespaceLookupTable {
    std::mutex mutex;
    u64 next_handle{0x4C530001u};
    std::unordered_map<u64, NamespaceLookup> lookups;
};

std::string active_host_network_name() {
    struct ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) return {};
    std::string result;
    for (const struct ifaddrs* current = interfaces;
         current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_name == nullptr) continue;
        const unsigned flags = current->ifa_flags;
        if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) continue;
        const int family = current->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        result = current->ifa_name;
        break;
    }
    ::freeifaddrs(interfaces);
    return result;
}

} // namespace

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_ws2_thunks(Ws2Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(60);
    const auto namespace_lookups = std::make_shared<NamespaceLookupTable>();

    // ---- Standard BSD socket ordinals (matching real Windows ws2_32.dll) ----

    // ord 1: accept (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "accept", 1,
        [&mgr](Marshaler& m) { impl_accept(m, mgr); },
        12
    });

    // ord 2: bind (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "bind", 2,
        [&mgr](Marshaler& m) { impl_bind(m, mgr); },
        12
    });

    // ord 3: closesocket (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "closesocket", 3,
        [&mgr](Marshaler& m) { impl_closesocket(m, mgr); },
        4
    });

    // ord 4: connect (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "connect", 4,
        [&mgr](Marshaler& m) { impl_connect(m, mgr); },
        12
    });

    // ord 5: getpeername (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "getpeername", 5,
        [&mgr](Marshaler& m) { impl_getpeername(m, mgr); },
        12
    });

    // ord 6: getsockname (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "getsockname", 6,
        [&mgr](Marshaler& m) { impl_getsockname(m, mgr); },
        12
    });

    // ord 7: getsockopt (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "getsockopt", 7,
        [&mgr](Marshaler& m) { impl_getsockopt(m, mgr); },
        20
    });

    // ord 8: htonl (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "htonl", 8,
        [&mgr](Marshaler& m) { impl_htonl(m, mgr); },
        4
    });

    // ord 9: htons (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "htons", 9,
        [&mgr](Marshaler& m) { impl_htons(m, mgr); },
        4
    });

    // ord 10: ioctlsocket (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "ioctlsocket", 10,
        [&mgr](Marshaler& m) { impl_ioctlsocket(m, mgr); },
        12
    });

    // ord 11: inet_addr (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "inet_addr", 11,
        [&mgr](Marshaler& m) { impl_inet_addr(m, mgr); },
        4
    });

    // ord 12: inet_ntoa (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "inet_ntoa", 12,
        [&mgr](Marshaler& m) { impl_inet_ntoa(m, mgr); },
        4
    });

    // ord 13: listen (stdcall 8)
    exports.entries.push_back(ExportEntry{
        "listen", 13,
        [&mgr](Marshaler& m) { impl_listen(m, mgr); },
        8
    });

    // ord 14: ntohl (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "ntohl", 14,
        [&mgr](Marshaler& m) { impl_ntohl(m, mgr); },
        4
    });

    // ord 15: ntohs (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "ntohs", 15,
        [&mgr](Marshaler& m) { impl_ntohs(m, mgr); },
        4
    });

    // ord 16: recv (stdcall 16)
    exports.entries.push_back(ExportEntry{
        "recv", 16,
        [&mgr](Marshaler& m) { impl_recv(m, mgr); },
        16
    });

    // ord 18: select (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "select", 18,
        [&mgr](Marshaler& m) { impl_select(m, mgr); },
        20
    });

    // ord 17: recvfrom (stdcall 24)
    exports.entries.push_back(ExportEntry{
        "recvfrom", 17,
        [&mgr](Marshaler& m) { impl_recvfrom(m, mgr); },
        24
    });

    // ord 19: send (stdcall 16)
    exports.entries.push_back(ExportEntry{
        "send", 19,
        [&mgr](Marshaler& m) { impl_send(m, mgr); },
        16
    });

    // ord 20: sendto (stdcall 24)
    exports.entries.push_back(ExportEntry{
        "sendto", 20,
        [&mgr](Marshaler& m) { impl_sendto(m, mgr); },
        24
    });

    // ord 21: setsockopt (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "setsockopt", 21,
        [&mgr](Marshaler& m) { impl_setsockopt(m, mgr); },
        20
    });

    // ord 22: shutdown (stdcall 8)
    exports.entries.push_back(ExportEntry{
        "shutdown", 22,
        [&mgr](Marshaler& m) { impl_shutdown(m, mgr); },
        8
    });

    // ord 23: socket (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "socket", 23,
        [&mgr](Marshaler& m) { impl_socket(m, mgr); },
        12
    });

    // ---- Higher ordinals (gethostby*, getprotoby*, etc.) ----

    // ord 52: gethostbyname (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "gethostbyname", 52,
        [&mgr](Marshaler& m) { impl_gethostbyname(m, mgr); },
        4
    });

    // ord 53: getprotobyname (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "getprotobyname", 53,
        [&mgr](Marshaler& m) { impl_getprotobyname(m, mgr); },
        4
    });

    // ord 54: getprotobynumber (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "getprotobynumber", 54,
        [&mgr](Marshaler& m) { impl_getprotobynumber(m, mgr); },
        4
    });

    // ord 56: getservbyport (stdcall 8)
    exports.entries.push_back(ExportEntry{
        "getservbyport", 56,
        [&mgr](Marshaler& m) { impl_getservbyport(m, mgr); },
        8
    });

    // ord 57: gethostname (stdcall 8)
    exports.entries.push_back(ExportEntry{
        "gethostname", 57,
        [&mgr](Marshaler& m) { impl_gethostname(m, mgr); },
        8
    });

    // ---- Winsock2 higher ordinals ----

    // ord 111: WSAGetLastError (stdcall 0)
    exports.entries.push_back(ExportEntry{
        "WSAGetLastError", 111,
        [&mgr](Marshaler& m) { impl_WSAGetLastError(m, mgr); },
        0
    });

    // ord 112: WSASetLastError (stdcall 4)
    exports.entries.push_back(ExportEntry{
        "WSASetLastError", 112,
        [&mgr](Marshaler& m) { impl_WSASetLastError(m, mgr); },
        4
    });

    // ord 115: WSAStartup (stdcall 8)
    exports.entries.push_back(ExportEntry{
        "WSAStartup", 115,
        [&mgr](Marshaler& m) { impl_WSAStartup(m, mgr); },
        8
    });

    // Ordinal 116: WSACleanup (stdcall 0)
    exports.entries.push_back(ExportEntry{
        "WSACleanup", 116,
        [&mgr](Marshaler& m) { impl_WSACleanup(m, mgr); },
        0
    });

    // dumpbin ws2_32: WSAAsyncGetHostByName @103 stdcall 20; Cancel @108 @4.
    // Imported by name.
    exports.entries.push_back(ExportEntry{
        "WSAAsyncGetHostByName", 103,
        [&mgr](Marshaler& m) { impl_WSAAsyncGetHostByName(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "WSACancelAsyncRequest", 108,
        [&mgr](Marshaler& m) { impl_WSACancelAsyncRequest(m, mgr); },
        4
    });

    // Imported by name.
    exports.entries.push_back(ExportEntry{
        "WSANtohs", 0,
        [&mgr](Marshaler& m) { impl_WSANtohs(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "WSAHtons", 0,
        [&mgr](Marshaler& m) { impl_WSAHtons(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "WSANtohl", 0,
        [&mgr](Marshaler& m) { impl_WSANtohl(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "WSAHtonl", 0,
        [&mgr](Marshaler& m) { impl_WSAHtonl(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "WSAAccept", 0,
        [&mgr](Marshaler& m) { impl_WSAAccept(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "WSAPoll", 0,
        [&mgr](Marshaler& m) { impl_WSAPoll(m, mgr); },
        12
    });

    // ord 151: __WSAFDIsSet (stdcall 8)
    // int __WSAFDIsSet(SOCKET fd, fd_set* set)
    // Returns non-zero if fd is in the fd_set, 0 otherwise
    exports.entries.push_back(ExportEntry{
        "__WSAFDIsSet", 151,
        [](Marshaler& m) {
            GuestAddr set_ptr = m.arg_ptr(1);
            if (!set_ptr) {
                m.set_return(0);
                return;
            }
            auto count_res = m.read_u32(set_ptr);
            if (count_res.is_err()) {
                m.set_return(0);
                return;
            }
            // FD_SETSIZE is a caller compile-time choice (Mono uses 1024).
            // Cap only to bound guest memory walks.
            u32 count = count_res.value();
            constexpr u32 kMaxFdSet = 65536u;
            if (count > kMaxFdSet) count = kMaxFdSet;
            const bool is64 = m.cpu_is_64bit();
            const u32 arr_off = is64 ? 8u : 4u;
            const u32 stride  = is64 ? 8u : 4u;
            const u64 sock = is64 ? m.arg(0) : (m.arg(0) & 0xFFFFFFFFull);
            for (u32 i = 0; i < count; ++i) {
                u64 entry = 0;
                if (is64) {
                    auto r = m.read_u64(set_ptr + arr_off + i * stride);
                    if (r.is_err()) continue;
                    entry = r.value();
                } else {
                    auto r = m.read_u32(set_ptr + arr_off + i * stride);
                    if (r.is_err()) continue;
                    entry = r.value();
                }
                if (entry == sock) {
                    VITREO_TRACE("[ws2_32] __WSAFDIsSet(0x%llx) -> found\n",
                                 static_cast<unsigned long long>(sock));
                    m.set_return(1);
                    return;
                }
            }
            VITREO_TRACE("[ws2_32] __WSAFDIsSet(0x%llx) -> not found\n",
                         static_cast<unsigned long long>(sock));
            m.set_return(0);
        },
        8
    });

    // ---- Named exports ----

    // GetHostNameW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetHostNameW", 0,
        [&mgr](Marshaler& m) { impl_GetHostNameW(m, mgr); },
        8
    });

    // WSAIoctl (9 args, stdcall 36)
    exports.entries.push_back(ExportEntry{
        "WSAIoctl", 0,
        [&mgr](Marshaler& m) { impl_WSAIoctl(m, mgr); },
        36
    });

    // WSARecv (7 args, stdcall 28)
    exports.entries.push_back(ExportEntry{
        "WSARecv", 0,
        [&mgr](Marshaler& m) { impl_WSARecv(m, mgr); },
        28
    });

    // WSARecvFrom (9 args, stdcall 36)
    exports.entries.push_back(ExportEntry{
        "WSARecvFrom", 0,
        [&mgr](Marshaler& m) { impl_WSARecvFrom(m, mgr); },
        36
    });

    // WSARecvMsg is a provider extension discovered through
    // WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER).
    exports.entries.push_back(ExportEntry{
        "WSARecvMsg", 0,
        [&mgr](Marshaler& m) { impl_WSARecvMsg(m, mgr); },
        20
    });

    // WSASend (7 args, stdcall 28)
    exports.entries.push_back(ExportEntry{
        "WSASend", 0,
        [&mgr](Marshaler& m) { impl_WSASend(m, mgr); },
        28
    });

    // dumpbin: WSASendDisconnect @93 stdcall 8 (imported by name).
    exports.entries.push_back(ExportEntry{
        "WSASendDisconnect", 93,
        [&mgr](Marshaler& m) { impl_WSASendDisconnect(m, mgr); },
        8
    });

    // WSASendTo (9 args, stdcall 36)
    exports.entries.push_back(ExportEntry{
        "WSASendTo", 0,
        [&mgr](Marshaler& m) { impl_WSASendTo(m, mgr); },
        36
    });

    // INT WSASendMsg(SOCKET, LPWSAMSG, DWORD, LPDWORD, LPWSAOVERLAPPED,
    //                LPWSAOVERLAPPED_COMPLETION_ROUTINE)
    // https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasendmsg
    exports.entries.push_back(ExportEntry{
        "WSASendMsg", 0,
        [&mgr](Marshaler& m) { impl_WSASendMsg(m, mgr); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "WSASocketA", 0,
        [&mgr](Marshaler& m) { impl_WSASocketA(m, mgr); },
        24
    });

    // freeaddrinfo (1 arg, stdcall 4)
    exports.entries.push_back(ExportEntry{
        "freeaddrinfo", 0,
        [&mgr](Marshaler& m) { impl_freeaddrinfo(m, mgr); },
        4
    });

    // getaddrinfo (4 args, stdcall 16)
    exports.entries.push_back(ExportEntry{
        "getaddrinfo", 0,
        [&mgr](Marshaler& m) { impl_getaddrinfo(m, mgr); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "GetAddrInfoW", 0,
        [&mgr](Marshaler& m) { impl_GetAddrInfoW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "FreeAddrInfoW", 0,
        [&mgr](Marshaler& m) { impl_FreeAddrInfoW(m, mgr); },
        4
    });

    // GetAddrInfoExW (10 args). DNS uses the ExW path. A missing export
    // fail-thunked to 0xFFFFFFFF blocks HTTPS name resolution.
    exports.entries.push_back(ExportEntry{
        "GetAddrInfoExW", 0,
        [&mgr](Marshaler& m) { impl_GetAddrInfoExW(m, mgr); },
        40
    });
    exports.entries.push_back(ExportEntry{
        "FreeAddrInfoExW", 0,
        [&mgr](Marshaler& m) { impl_FreeAddrInfoExW(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FreeAddrInfoEx", 0,
        [&mgr](Marshaler& m) { impl_FreeAddrInfoExW(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetAddrInfoExCancel", 0,
        [&mgr](Marshaler& m) { impl_GetAddrInfoExCancel(m, mgr); },
        4
    });

    // ConnectEx (7 args, stdcall 28) - extension function
    exports.entries.push_back(ExportEntry{
        "ConnectEx", 0,
        [&mgr](Marshaler& m) { impl_ConnectEx(m, mgr); },
        28
    });

    // DisconnectEx (4 args, stdcall 16) - extension function
    exports.entries.push_back(ExportEntry{
        "DisconnectEx", 0,
        [&mgr](Marshaler& m) { impl_DisconnectEx(m, mgr); },
        16
    });

    // TransmitFile (7 args, stdcall 28) — mswsock extension via WSAIoctl
    exports.entries.push_back(ExportEntry{
        "TransmitFile", 0,
        [&mgr](Marshaler& m) { impl_TransmitFile(m, mgr); },
        28
    });

    // TransmitPackets (6 args, stdcall 24) — mswsock extension via WSAIoctl
    exports.entries.push_back(ExportEntry{
        "TransmitPackets", 0,
        [&mgr](Marshaler& m) { impl_TransmitPackets(m, mgr); },
        24
    });

    // WSACreateEvent (0 args, stdcall 0)
    exports.entries.push_back(ExportEntry{
        "WSACreateEvent", 0,
        [&mgr](Marshaler& m) {
            // WSAEVENT WSACreateEvent(void)
            // Returns a manual-reset event, initially non-signaled
            SyncManager* sync = mgr.sync_manager();
            if (!sync) {
                mgr.set_last_error(WSASYSCALLFAILURE);
                m.set_return(0); // WSA_INVALID_EVENT = NULL
                return;
            }
            Handle h = sync->create_event(true, false, "");
            VITREO_TRACE("[ws2_32] WSACreateEvent -> handle=0x%x\n", h);
            if (wsa_event_diag_enabled()) {
                std::fprintf(stderr,
                             "[ws2-event] tid=%u create event=0x%x\n",
                             ThreadManager::get_current_thread_id_static(), h);
            }
            m.set_return(static_cast<u64>(h));
        },
        0
    });

    // WSACloseEvent (1 arg, stdcall 4)
    exports.entries.push_back(ExportEntry{
        "WSACloseEvent", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { mgr.set_last_error(6); m.set_return(0); return; }
            // BOOL WSACloseEvent(WSAEVENT hEvent)
            Handle h = static_cast<Handle>(m.arg(0));
            VITREO_TRACE("[ws2_32] WSACloseEvent(handle=0x%x)\n", h);
            // Clean up any socket event associations referencing this event
            mgr.remove_event_assoc_by_event(static_cast<u32>(h));
            // Close the event in SyncManager if available. An unknown or
            // already-closed handle fails WSA_INVALID_HANDLE (6)
            // (ws2_misc_probe close_bogus / close_twice).
            SyncManager* sync = mgr.sync_manager();
            if (sync == nullptr || !sync->close_handle(h)) {
                mgr.set_last_error(6);
                m.set_return(0);
                return;
            }
            m.set_return(1); // TRUE
        },
        4
    });

    // WSAEventSelect (3 args, stdcall 12)
    exports.entries.push_back(ExportEntry{
        "WSAEventSelect", 0,
        [&mgr](Marshaler& m) {
            // int WSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents)
            u32 guest_socket = static_cast<u32>(m.arg(0));
            Handle event_handle = static_cast<Handle>(m.arg(1));
            u32 network_events = static_cast<u32>(m.arg(2));
            VITREO_TRACE("[ws2_32] WSAEventSelect(socket=0x%x, event=0x%x, events=0x%x)\n",
                         guest_socket, event_handle, network_events);
            ipc::ipc_log(
                "WSAEventSelect socket=0x%x event=0x%x mask=0x%x",
                guest_socket, event_handle, network_events);

            int native_fd = mgr.get_native_fd(guest_socket);
            if (native_fd < 0) {
                mgr.set_last_error(WSAENOTSOCK);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            // Learn WSAEventSelect: lNetworkEvents 0 cancels the association
            // and ignores hEventObject; a NULL event with events selected is
            // WSAEINVAL (ws2_event_record_probe select_null_event).
            if (network_events == 0) {
                mgr.cancel_network_events(guest_socket);
                m.set_return(0);
                return;
            }
            if (event_handle == 0) {
                mgr.set_last_error(WSAEINVAL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            // WSAEventSelect makes the socket nonblocking.
            int flags_val = ::fcntl(native_fd, F_GETFL, 0);
            if (flags_val >= 0) {
                ::fcntl(native_fd, F_SETFL, flags_val | O_NONBLOCK);
            }
            if (wsa_event_diag_enabled()) {
                std::fprintf(
                    stderr,
                    "[ws2-event] tid=%u associate socket=0x%x event=0x%x "
                    "mask=0x%x\n",
                    ThreadManager::get_current_thread_id_static(), guest_socket,
                    event_handle, network_events);
            }
            // Replaces any previous association, clears the record, and
            // records what already holds.
            mgr.select_network_events(guest_socket,
                                      static_cast<u32>(event_handle),
                                      network_events);

            m.set_return(0); // Success
        },
        12
    });

    // WSAEnumNetworkEvents (3 args, stdcall 12)
    exports.entries.push_back(ExportEntry{
        "WSAEnumNetworkEvents", 0,
        [&mgr](Marshaler& m) {
            // int WSAEnumNetworkEvents(SOCKET s, WSAEVENT hEventObject,
            //                         LPWSANETWORKEVENTS lpNetworkEvents)
            u32 guest_socket = static_cast<u32>(m.arg(0));
            Handle event_handle = static_cast<Handle>(m.arg(1));
            GuestAddr events_ptr = m.arg_ptr(2);
            VITREO_TRACE("[ws2_32] WSAEnumNetworkEvents(socket=0x%x, event=0x%x)\n",
                         guest_socket, event_handle);
            // Win32 validates the socket handle before anything else; an
            // INVALID_SOCKET (0) yields SOCKET_ERROR + WSAENOTSOCK.
            if (guest_socket == 0 || mgr.get_native_fd(guest_socket) < 0) {
                mgr.set_last_error(WSAENOTSOCK);  // ws2_misc_probe enum_bad_socket
                m.set_return(0xFFFFFFFFu);
                return;
            }
            // A NULL lpNetworkEvents fails WSAEINVAL before anything is
            // cleared or reset (ws2_misc_probe enum_null_out: event_still_set=1),
            // and an hEventObject that is not an event fails WSAENOTSOCK
            // (enum_bad_event).
            if (events_ptr == 0) {
                mgr.set_last_error(WSAEINVAL);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            // An hEventObject that is not an event fails WSAENOTSOCK before
            // the record is touched (ws2_misc_probe enum_bad_event).
            if (event_handle != 0 && mgr.sync_manager() != nullptr &&
                !mgr.sync_manager()->reset_event(event_handle)) {
                mgr.set_last_error(WSAENOTSOCK);
                m.set_return(0xFFFFFFFFu);
                return;
            }

            // Learn WSAEnumNetworkEvents: the record is returned and cleared,
            // and hEventObject (when not NULL) reset, as one step; NULL leaves
            // the associated event signaled (ws2_event_record_probe
            // reset_after_enum_null). Only lNetworkEvents and the iErrorCode
            // slots of reported events are written; the others keep what the
            // caller had (ws2_misc_probe enum_ok: err0=0xcccccccc).
            u32 reported_events = 0;
            std::array<u32, 10> errors{};
            mgr.take_network_events(guest_socket, event_handle,
                                    &reported_events, &errors);
            (void)m.write_u32(events_ptr, reported_events);
            for (u32 bit = 0; bit < 10; ++bit) {
                if ((reported_events & (1u << bit)) == 0) continue;
                (void)m.write_u32(events_ptr + 4 + bit * 4, errors[bit]);
            }
            if (reported_events != 0) {
                ipc::ipc_log(
                    "WSAEnumNetworkEvents socket=0x%x event=0x%x -> "
                    "events=0x%x",
                    guest_socket, event_handle, reported_events);
            }
            VITREO_TRACE("[ws2_32] WSAEnumNetworkEvents -> events=0x%x\n", reported_events);

            m.set_return(0); // Success
        },
        12
    });

    // ord 51: gethostbyaddr (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "gethostbyaddr", 51,
        [&mgr](Marshaler& m) { impl_gethostbyaddr(m, mgr); },
        12
    });

    // ord 55: getservbyname (stdcall 8)
    exports.entries.push_back(ExportEntry{
        "getservbyname", 55,
        [&mgr](Marshaler& m) { impl_getservbyname(m, mgr); },
        8
    });

    // ord 507: inet_ntop (stdcall 16)
    // NOTE: Previously registered at ordinal 12 which collided with inet_ntoa.
    // Real ws2_32.dll exports inet_ntop at ordinal 507.
    exports.entries.push_back(ExportEntry{
        "inet_ntop", 507,
        [&mgr](Marshaler& m) { impl_inet_ntop(m, mgr); },
        16
    });

    // InetNtopW: the wide form of inet_ntop. Learn (InetNtopW): identical
    // conversion, but the destination is wide characters and the length is a
    // character count, not a byte count. Formats through the narrow path and
    // widens — every character an address can produce is ASCII.
    exports.entries.push_back(ExportEntry{
        "InetNtopW", 0,
        [&mgr](Marshaler& m) {
            const u32 family = static_cast<u32>(m.arg(0));
            const GuestAddr addr_ptr = m.arg_ptr(1);
            const GuestAddr dst = m.arg_ptr(2);
            const usize dst_chars = static_cast<usize>(m.arg(3));
            constexpr u32 kAfInet = 2;
            constexpr u32 kAfInet6 = 23;  // Windows value
            if (addr_ptr == 0 || dst == 0 || dst_chars == 0 ||
                (family != kAfInet && family != kAfInet6)) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(0);
                return;
            }
            char text[INET6_ADDRSTRLEN] = {};
            if (family == kAfInet) {
                u32 raw = 0;
                if (m.memory().read_bytes(
                        addr_ptr, reinterpret_cast<Byte*>(&raw),
                        sizeof(raw)).is_err()) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(0);
                    return;
                }
                struct in_addr a {};
                a.s_addr = raw;
                if (::inet_ntop(AF_INET, &a, text, sizeof(text)) == nullptr) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(0);
                    return;
                }
            } else {
                std::array<u8, 16> raw{};
                if (m.memory().read_bytes(
                        addr_ptr, reinterpret_cast<Byte*>(raw.data()),
                        raw.size()).is_err()) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(0);
                    return;
                }
                struct in6_addr a {};
                std::memcpy(&a, raw.data(), raw.size());
                if (::inet_ntop(AF_INET6, &a, text, sizeof(text)) == nullptr) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(0);
                    return;
                }
            }
            const usize len = std::strlen(text);
            if (len + 1 > dst_chars) {
                mgr.set_last_error(WSAEINVAL);
                m.set_return(0);
                return;
            }
            for (usize i = 0; i <= len; ++i) {
                (void)m.write_u16(dst + static_cast<GuestAddr>(i * 2),
                                  static_cast<u16>(text[i]));
            }
            m.set_return(static_cast<u64>(dst));
        },
        16
    });

    // ord 508: inet_pton (stdcall 12)
    // NOTE: Previously registered at ordinal 13 which collided with listen.
    // Real ws2_32.dll exports inet_pton at ordinal 508.
    exports.entries.push_back(ExportEntry{
        "inet_pton", 508,
        [&mgr](Marshaler& m) { impl_inet_pton(m, mgr); },
        12
    });

    // WSAAddressToStringA (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "WSAAddressToStringA", 0,
        [&mgr](Marshaler& m) { impl_WSAAddressToStringA(m, mgr); },
        20
    });

    // WSAStringToAddressA (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "WSAStringToAddressA", 0,
        [&mgr](Marshaler& m) { impl_WSAStringToAddressA(m, mgr); },
        20
    });

    // WSAAddressToStringW (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "WSAAddressToStringW", 0,
        [&mgr](Marshaler& m) { impl_WSAAddressToStringW(m, mgr); },
        20
    });

    // WSAStringToAddressW (stdcall 20)
    exports.entries.push_back(ExportEntry{
        "WSAStringToAddressW", 0,
        [&mgr](Marshaler& m) { impl_WSAStringToAddressW(m, mgr); },
        20
    });

    // WSAEnumProtocolsA/W — Windows ordinals 101/102 (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "WSAEnumProtocolsA", 101,
        [&mgr](Marshaler& m) { impl_WSAEnumProtocolsA(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "WSAEnumProtocolsW", 102,
        [&mgr](Marshaler& m) { impl_WSAEnumProtocolsW(m, mgr); },
        12
    });

    // WSAConnect (stdcall 28)
    exports.entries.push_back(ExportEntry{
        "WSAConnect", 0,
        [&mgr](Marshaler& m) { impl_WSAConnect(m, mgr); },
        28
    });

    // AcceptEx (stdcall 32) — extension function retrieved via WSAIoctl
    exports.entries.push_back(ExportEntry{
        "AcceptEx", 0,
        [&mgr](Marshaler& m) { ws2_AcceptEx(m, mgr); },
        32
    });

    // GetAcceptExSockaddrs (stdcall 32) — extension function
    exports.entries.push_back(ExportEntry{
        "GetAcceptExSockaddrs", 0,
        [&mgr](Marshaler& m) { ws2_GetAcceptExSockaddrs(m, mgr); },
        32
    });

    // getnameinfo(sa, salen, host, hostlen, serv, servlen, flags) (stdcall 28)
    exports.entries.push_back(ExportEntry{
        "getnameinfo", 0,
        [&mgr](Marshaler& m) { impl_getnameinfo(m, mgr); },
        28
    });
    // GetNameInfoW(sa, salen, host, hostlen, serv, servlen, flags) (stdcall 28)
    exports.entries.push_back(ExportEntry{
        "GetNameInfoW", 0,
        [&mgr](Marshaler& m) { impl_GetNameInfoW(m, mgr); },
        28
    });

    // ------------------------------------------------------------------------
    // Wave 4: additional WS2_32 stub APIs
    // ------------------------------------------------------------------------
    constexpr u32 WSA_E_NO_MORE_LOCAL = 10110;
    // WSADuplicateSocket{A,W}(s, dwProcessId, lpProtocolInfo) (stdcall 12)
    exports.entries.push_back(ExportEntry{
        "WSADuplicateSocketW", 0,
        [&mgr](Marshaler& m) { impl_WSADuplicateSocket(m, mgr, true); }, 12 });
    exports.entries.push_back(ExportEntry{
        "WSADuplicateSocketA", 0,
        [&mgr](Marshaler& m) { impl_WSADuplicateSocket(m, mgr, false); }, 12 });
    exports.entries.push_back(ExportEntry{
        "WSAEnumNameSpaceProvidersW", 0,
        [](Marshaler& m) {
            GuestAddr len_ptr = m.arg_ptr(0);
            (void)m.arg_ptr(1);
            if (len_ptr) (void)m.write_u32(len_ptr, 0);
            m.set_return(0);
        }, 8 });
    exports.entries.push_back(ExportEntry{
        "WSAGetOverlappedResult", 0,
        [&mgr](Marshaler& m) { impl_WSAGetOverlappedResult(m, mgr); }, 20 });
    exports.entries.push_back(ExportEntry{
        "WSALookupServiceBeginW", 0,
        [&mgr, namespace_lookups](Marshaler& m) {
            const GuestAddr restrictions = m.arg_ptr(0);
            (void)m.arg(1);
            GuestAddr lookup = m.arg_ptr(2);
            if (restrictions == 0 || lookup == 0) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            const u32 namespace_offset = m.cpu_is_64bit() ? 40u : 20u;
            const auto name_space = m.read_u32(restrictions + namespace_offset);
            if (name_space.is_err()) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            u64 handle = 0;
            {
                std::lock_guard lock(namespace_lookups->mutex);
                handle = namespace_lookups->next_handle++;
                namespace_lookups->lookups.emplace(
                    handle, NamespaceLookup{name_space.value(), false});
            }
            if (m.write_ptr(lookup, handle).is_err()) {
                std::lock_guard lock(namespace_lookups->mutex);
                namespace_lookups->lookups.erase(handle);
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            ipc::ipc_log(
                "WSALookupServiceBeginW namespace=%u -> 0x%llx",
                name_space.value(), static_cast<unsigned long long>(handle));
            m.set_return(0);
        }, 12 });
    exports.entries.push_back(ExportEntry{
        "WSALookupServiceNextW", 0,
        [&mgr, namespace_lookups](Marshaler& m) {
            const u64 handle = m.arg(0);
            (void)m.arg(1);
            const GuestAddr length_ptr = m.arg_ptr(2);
            const GuestAddr results = m.arg_ptr(3);
            constexpr u32 NS_NLA = 15;

            std::lock_guard lock(namespace_lookups->mutex);
            const auto found = namespace_lookups->lookups.find(handle);
            if (found == namespace_lookups->lookups.end()) {
                mgr.set_last_error(6u); // WSA_INVALID_HANDLE
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            const std::string interface_name = active_host_network_name();
            if (found->second.name_space != NS_NLA || found->second.yielded ||
                interface_name.empty()) {
                mgr.set_last_error(WSA_E_NO_MORE_LOCAL);
                ipc::ipc_log(
                    "WSALookupServiceNextW handle=0x%llx namespace=%u "
                    "interface='%s' -> WSA_E_NO_MORE",
                    static_cast<unsigned long long>(handle),
                    found->second.name_space, interface_name.c_str());
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            if (length_ptr == 0 || results == 0) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }

            const bool is64 = m.cpu_is_64bit();
            const u32 query_size = is64 ? 120u : 60u;
            const u32 namespace_offset = is64 ? 40u : 20u;
            const u32 name_pointer_offset = is64 ? 8u : 4u;
            const u32 name_bytes = static_cast<u32>(
                (interface_name.size() + 1u) * sizeof(char16_t));
            const u32 required = query_size + name_bytes;
            const auto capacity = m.read_u32(length_ptr);
            if (capacity.is_err()) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            if (capacity.value() < required) {
                (void)m.write_u32(length_ptr, required);
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }

            std::vector<Byte> zeroes(required, 0);
            if (m.memory().write_bytes(results, zeroes.data(), zeroes.size()).is_err() ||
                m.write_u32(results, query_size).is_err() ||
                m.write_ptr(results + name_pointer_offset,
                            results + query_size).is_err() ||
                m.write_u32(results + namespace_offset, NS_NLA).is_err()) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            for (u32 i = 0; i < interface_name.size(); ++i) {
                if (m.write_u16(results + query_size + i * 2u,
                                static_cast<u16>(
                                    static_cast<unsigned char>(interface_name[i])))
                        .is_err()) {
                    mgr.set_last_error(WSAEFAULT);
                    m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                    return;
                }
            }
            if (m.write_u16(results + query_size +
                                static_cast<u32>(interface_name.size()) * 2u,
                            0)
                    .is_err() ||
                m.write_u32(length_ptr, required).is_err()) {
                mgr.set_last_error(WSAEFAULT);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            found->second.yielded = true;
            VITREO_TRACE(
                "[ws2_32] WSALookupServiceNextW(handle=0x%llx) -> "
                "network '%s'\n",
                static_cast<unsigned long long>(handle), interface_name.c_str());
            m.set_return(0);
        }, 16 });
    exports.entries.push_back(ExportEntry{
        "WSALookupServiceEnd", 0,
        [&mgr, namespace_lookups](Marshaler& m) {
            const u64 handle = m.arg(0);
            std::lock_guard lock(namespace_lookups->mutex);
            if (namespace_lookups->lookups.erase(handle) == 0) {
                mgr.set_last_error(6u); // WSA_INVALID_HANDLE
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            m.set_return(0);
        }, 4 });
    exports.entries.push_back(ExportEntry{
        "WSAResetEvent", 0,
        [&mgr](Marshaler& m) {
            u32 evt = static_cast<u32>(m.arg(0));
            SyncManager* sync = mgr.sync_manager();
            if (sync && sync->reset_event(static_cast<Handle>(evt))) {
                if (wsa_event_diag_enabled()) {
                    std::fprintf(stderr,
                                 "[ws2-event] tid=%u reset event=0x%x\n",
                                 ThreadManager::get_current_thread_id_static(),
                                 evt);
                }
                m.set_return(1);
            } else {
                mgr.set_last_error(6);  // WSA_INVALID_HANDLE (ws2_misc_probe reset_bogus)
                m.set_return(0);
            }
        }, 4 });
    exports.entries.push_back(ExportEntry{
        "WSASetEvent", 0,
        [&mgr](Marshaler& m) {
            u32 evt = static_cast<u32>(m.arg(0));
            SyncManager* sync = mgr.sync_manager();
            if (sync && sync->set_event(static_cast<Handle>(evt))) {
                if (wsa_event_diag_enabled()) {
                    std::fprintf(stderr,
                                 "[ws2-event] tid=%u set event=0x%x\n",
                                 ThreadManager::get_current_thread_id_static(),
                                 evt);
                }
                m.set_return(1);
            } else {
                mgr.set_last_error(6);  // WSA_INVALID_HANDLE (ws2_misc_probe set_bogus)
                m.set_return(0);
            }
        }, 4 });
    exports.entries.push_back(ExportEntry{
        "WSASetServiceW", 0,
        [&mgr](Marshaler& m) {
            // INT WSASetServiceW(LPWSAQUERYSETW, WSAESETSERVICEOP, DWORD).
            // Vitreo has no namespace provider to register a service with;
            // returning success here reported a registration that never
            // happened. Fail with a documented WSASetService error instead.
            (void)m.arg_ptr(0); (void)m.arg(1); (void)m.arg(2);
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ws2_32!WSASetServiceW: "
                                     "no namespace provider accepts registrations\n");
            }
            mgr.set_last_error(WSAEINVAL);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        }, 12 });
    exports.entries.push_back(ExportEntry{
        "WSASocketW", 0,
        [&mgr](Marshaler& m) { impl_WSASocketW(m, mgr); },
        24 });
    exports.entries.push_back(ExportEntry{
        "WSAWaitForMultipleEvents", 0,
        [&mgr](Marshaler& m) {
            const u32 count = static_cast<u32>(m.arg(0));
            const GuestAddr handles_ptr = m.arg_ptr(1);
            const bool wait_all = m.arg32(2) != 0;
            u32 timeout = static_cast<u32>(m.arg(3));
            // BOOL on x64 stack slot (index 4): use arg32.
            const bool alertable = m.arg32(4) != 0;

            if (count == 0 || count > 64 || handles_ptr == 0) {
                mgr.set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(WAIT_FAILED);
                return;
            }

            const u32 handle_stride = m.cpu_is_64bit() ? 8u : 4u;
            std::vector<Handle> handles(count);
            for (u32 i = 0; i < count; ++i) {
                const GuestAddr address =
                    handles_ptr + static_cast<GuestAddr>(i) * handle_stride;
                const auto value =
                    m.cpu_is_64bit()
                        ? m.memory().read_u64(address)
                        : m.memory().read_u32(address).map(
                              [](u32 handle) {
                                  return static_cast<u64>(handle);
                              });
                if (!value.is_ok()) {
                    mgr.set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(WAIT_FAILED);
                    return;
                }
                handles[i] = static_cast<Handle>(value.value());
            }

            bool log_this_wait = false;
            if (wsa_event_diag_enabled()) {
                static std::atomic<u32> wait_log_count{0};
                const u32 log_index =
                    wait_log_count.fetch_add(1, std::memory_order_relaxed);
                if (log_index < 64) {
                    log_this_wait = true;
                    std::fprintf(
                        stderr,
                        "[ws2-event] tid=%u wait-enter count=%u wait_all=%u "
                        "timeout=%u alertable=%u handles=",
                        ThreadManager::get_current_thread_id_static(), count,
                        wait_all ? 1u : 0u, timeout, alertable ? 1u : 0u);
                    for (Handle handle : handles) {
                        std::fprintf(stderr, " 0x%x", handle);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            const u32 result = mgr.wait_for_multiple_events(
                handles, wait_all, timeout, alertable);
            if (log_this_wait) {
                std::fprintf(
                    stderr, "[ws2-event] tid=%u wait-exit result=0x%x\n",
                    ThreadManager::get_current_thread_id_static(), result);
            }
            VITREO_TRACE(
                "[ws2_32] WSAWaitForMultipleEvents(count=%u wait_all=%u "
                "timeout=%u alertable=%u) -> 0x%x\n",
                count, wait_all ? 1u : 0u, timeout, alertable ? 1u : 0u,
                result);
            m.set_return(result);
        }, 20 });
    exports.entries.push_back(ExportEntry{
        "WSCEnumProtocols", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            GuestAddr buflen = m.arg_ptr(2);
            GuestAddr err_p = m.arg_ptr(3);
            if (buflen) (void)m.write_u32(buflen, 0);
            if (err_p) (void)m.write_u32(err_p, 0);
            m.set_return(0);
        }, 16 });
    exports.entries.push_back(ExportEntry{
        "WSCGetProviderPath", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg_ptr(2);
            GuestAddr err_p = m.arg_ptr(3);
            if (err_p) (void)m.write_u32(err_p, WSAEINVAL);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        }, 16 });

    return exports;
}

// =============================================================================
// wsock32.dll Export Table Factory (Winsock 1.1)
// =============================================================================
//
// wsock32.dll and ws2_32.dll expose the same BSD-socket functions but under
// different ordinal numbering. The BSD block 1..9 and 13..23 coincide, but the
// three functions inet_addr / inet_ntoa / ioctlsocket are ordinals 10/11/12 in
// wsock32 and 11/12/10 in ws2_32. An import of wsock32 purely by ordinal
// gets the wrong function — and a stdcall arg-count mismatch that corrupts
// the stack — if it is served the ws2_32 table. This factory registers the
// correct wsock32 ordinals against the same shared Ws2Manager.

DllExports create_wsock32_thunks(Ws2Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(48);

    auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes,
                   std::function<void(Marshaler&)> handler) {
        exports.entries.push_back(
            ExportEntry{name, ordinal, std::move(handler), stdcall_bytes});
    };

    // ---- BSD socket block (ordinals shared with ws2_32) ----
    add("accept", 1, 12, [&mgr](Marshaler& m) { impl_accept(m, mgr); });
    add("bind", 2, 12, [&mgr](Marshaler& m) { impl_bind(m, mgr); });
    add("closesocket", 3, 4, [&mgr](Marshaler& m) { impl_closesocket(m, mgr); });
    add("connect", 4, 12, [&mgr](Marshaler& m) { impl_connect(m, mgr); });
    add("getpeername", 5, 12, [&mgr](Marshaler& m) { impl_getpeername(m, mgr); });
    add("getsockname", 6, 12, [&mgr](Marshaler& m) { impl_getsockname(m, mgr); });
    add("getsockopt", 7, 20, [&mgr](Marshaler& m) { impl_getsockopt(m, mgr); });
    add("htonl", 8, 4, [&mgr](Marshaler& m) { impl_htonl(m, mgr); });
    add("htons", 9, 4, [&mgr](Marshaler& m) { impl_htons(m, mgr); });

    // ---- The three that differ from ws2_32 ----
    add("inet_addr", 10, 4, [&mgr](Marshaler& m) { impl_inet_addr(m, mgr); });
    add("inet_ntoa", 11, 4, [&mgr](Marshaler& m) { impl_inet_ntoa(m, mgr); });
    add("ioctlsocket", 12, 12, [&mgr](Marshaler& m) { impl_ioctlsocket(m, mgr); });

    add("listen", 13, 8, [&mgr](Marshaler& m) { impl_listen(m, mgr); });
    add("ntohl", 14, 4, [&mgr](Marshaler& m) { impl_ntohl(m, mgr); });
    add("ntohs", 15, 4, [&mgr](Marshaler& m) { impl_ntohs(m, mgr); });
    add("recv", 16, 16, [&mgr](Marshaler& m) { impl_recv(m, mgr); });
    add("recvfrom", 17, 24, [&mgr](Marshaler& m) { impl_recvfrom(m, mgr); });
    add("select", 18, 20, [&mgr](Marshaler& m) { impl_select(m, mgr); });
    add("send", 19, 16, [&mgr](Marshaler& m) { impl_send(m, mgr); });
    add("sendto", 20, 24, [&mgr](Marshaler& m) { impl_sendto(m, mgr); });
    add("setsockopt", 21, 20, [&mgr](Marshaler& m) { impl_setsockopt(m, mgr); });
    add("shutdown", 22, 8, [&mgr](Marshaler& m) { impl_shutdown(m, mgr); });
    add("socket", 23, 12, [&mgr](Marshaler& m) { impl_socket(m, mgr); });

    // ---- Name/protocol/service resolvers (ordinals shared with ws2_32) ----
    add("gethostbyaddr", 51, 12, [&mgr](Marshaler& m) { impl_gethostbyaddr(m, mgr); });
    add("gethostbyname", 52, 4, [&mgr](Marshaler& m) { impl_gethostbyname(m, mgr); });
    add("getprotobyname", 53, 4, [&mgr](Marshaler& m) { impl_getprotobyname(m, mgr); });
    add("getprotobynumber", 54, 4, [&mgr](Marshaler& m) { impl_getprotobynumber(m, mgr); });
    add("getservbyname", 55, 8, [&mgr](Marshaler& m) { impl_getservbyname(m, mgr); });
    add("getservbyport", 56, 8, [&mgr](Marshaler& m) { impl_getservbyport(m, mgr); });
    add("gethostname", 57, 8, [&mgr](Marshaler& m) { impl_gethostname(m, mgr); });

    // ---- Winsock 1.1 startup/error surface ----
    add("WSAGetLastError", 111, 0, [&mgr](Marshaler& m) { impl_WSAGetLastError(m, mgr); });
    add("WSASetLastError", 112, 4, [&mgr](Marshaler& m) { impl_WSASetLastError(m, mgr); });
    add("WSAStartup", 115, 8, [&mgr](Marshaler& m) { impl_WSAStartup(m, mgr); });
    add("WSACleanup", 116, 0, [&mgr](Marshaler& m) { impl_WSACleanup(m, mgr); });

    // Native export metadata is captured by automation_export_ordinals_probe.
    // Guests import AcceptEx by ordinal for an HTTP listener; a miss leaves
    // the listen socket with no posted accepts.
    add("AcceptEx", 1141, 32,
        [&mgr](Marshaler& m) { ws2_AcceptEx(m, mgr); });
    add("GetAcceptExSockaddrs", 1142, 32,
        [&mgr](Marshaler& m) { ws2_GetAcceptExSockaddrs(m, mgr); });

    // ord 151: __WSAFDIsSet(SOCKET, fd_set*) — pointer-width SOCKET slots.
    add("__WSAFDIsSet", 151, 8, [](Marshaler& m) {
        GuestAddr set_ptr = m.arg_ptr(1);
        if (!set_ptr) { m.set_return(0); return; }
        auto count_res = m.read_u32(set_ptr);
        if (count_res.is_err()) { m.set_return(0); return; }
        u32 count = count_res.value();
        constexpr u32 kMaxFdSet = 65536u;
        if (count > kMaxFdSet) count = kMaxFdSet;
        const bool is64 = m.cpu_is_64bit();
        const u32 arr_off = is64 ? 8u : 4u;
        const u32 stride = is64 ? 8u : 4u;
        const u64 sock = is64 ? m.arg(0) : (m.arg(0) & 0xFFFFFFFFull);
        for (u32 i = 0; i < count; ++i) {
            u64 entry = 0;
            if (is64) {
                auto r = m.read_u64(set_ptr + arr_off + i * stride);
                if (r.is_err()) continue;
                entry = r.value();
            } else {
                auto r = m.read_u32(set_ptr + arr_off + i * stride);
                if (r.is_err()) continue;
                entry = r.value();
            }
            if (entry == sock) { m.set_return(1); return; }
        }
        m.set_return(0);
    });

    return exports;
}

} // namespace vitreo::winapi
