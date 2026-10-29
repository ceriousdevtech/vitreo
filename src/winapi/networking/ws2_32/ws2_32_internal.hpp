#pragma once
/// @file ws2_32_internal.hpp
/// @brief Shared surface between ws2_32.cpp and its extracted API groups.
///
/// Private to the Winsock implementation. These were file-local before
/// ws2_32.cpp was split; this header exists so the pieces can still see each
/// other, not to publish them.

#include "winapi/core/marshaler.hpp"
#include "winapi/networking/ws2_32/ws2_32.hpp"

#include <vitreo/common.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <string>
#include <optional>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// Winsock error codes
// -----------------------------------------------------------------------------
//
// Winsock does not reuse the Win32 error space: its codes start at 10000 and
// are what WSAGetLastError reports, so a guest checking for WSAEWOULDBLOCK is
// checking for 10035 and nothing else.

inline constexpr u32 WSA_IO_PENDING = 997;  // same as ERROR_IO_PENDING
inline constexpr u32 WSA_OPERATION_ABORTED = 995;  // ERROR_OPERATION_ABORTED
inline constexpr u32 WSAEINTR = 10004;
inline constexpr u32 WSAEBADF = 10009;
inline constexpr u32 WSAEACCES = 10013;
inline constexpr u32 WSAEFAULT = 10014;
inline constexpr u32 WSAEINVAL = 10022;
inline constexpr u32 WSAEMFILE = 10024;
inline constexpr u32 WSAEWOULDBLOCK = 10035;
inline constexpr u32 WSAEINPROGRESS = 10036;
inline constexpr u32 WSAEALREADY = 10037;
inline constexpr u32 WSAENOTSOCK = 10038;
inline constexpr u32 WSAEDESTADDRREQ = 10039;
inline constexpr u32 WSAEMSGSIZE = 10040;
inline constexpr u32 WSAEPROTOTYPE = 10041;
inline constexpr u32 WSAENOPROTOOPT = 10042;
inline constexpr u32 WSAEPROTONOSUPPORT = 10043;
inline constexpr u32 WSAESOCKTNOSUPPORT = 10044;
inline constexpr u32 WSAEOPNOTSUPP = 10045;
inline constexpr u32 WSAEAFNOSUPPORT = 10047;
inline constexpr u32 WSAEADDRINUSE = 10048;
inline constexpr u32 WSAEADDRNOTAVAIL = 10049;
inline constexpr u32 WSAENETDOWN = 10050;
inline constexpr u32 WSAENETUNREACH = 10051;
inline constexpr u32 WSAENETRESET = 10052;
inline constexpr u32 WSAECONNABORTED = 10053;
inline constexpr u32 WSAECONNRESET = 10054;
inline constexpr u32 WSAENOBUFS = 10055;
inline constexpr u32 WSAEISCONN = 10056;
inline constexpr u32 WSAENOTCONN = 10057;
inline constexpr u32 WSAESHUTDOWN = 10058;
inline constexpr u32 WSAETIMEDOUT = 10060;
inline constexpr u32 WSAECONNREFUSED = 10061;
inline constexpr u32 WSAEHOSTUNREACH = 10065;
inline constexpr u32 WSANOTINITIALISED = 10093;
inline constexpr u32 WSAVERNOTSUPPORTED = 10092;
inline constexpr u32 WSAHOST_NOT_FOUND = 11001;
inline constexpr u32 WSATRY_AGAIN = 11002;
inline constexpr u32 WSANO_RECOVERY = 11003;
inline constexpr u32 WSASYSCALLFAILURE = 10107;
inline constexpr u32 WSANO_DATA = 11004;
inline constexpr u32 WSATYPE_NOT_FOUND = 10109;
inline constexpr u32 WSA_NOT_ENOUGH_MEMORY = 8;

// Windows AF_* values and sockaddr sizes. These differ from the host's:
// AF_INET6 is 23 on Windows and 30 on Darwin, so a family read from guest
// memory must be translated rather than passed through.
inline constexpr u16 kWinAfUnspec = 0;
inline constexpr u16 kWinAfUnix = 1;
inline constexpr u16 kWinAfInet = 2;
inline constexpr u16 kWinAfInet6 = 23;
inline constexpr int kWinSockaddrIn6Bytes = 28;
/// Windows afunix.h: ADDRESS_FAMILY + sun_path[UNIX_PATH_MAX=108].
inline constexpr int kWinUnixPathMax = 108;
inline constexpr int kWinSockaddrUnBytes = 2 + kWinUnixPathMax;

/// A guest sockaddr decoded into host form, with the family already mapped —
/// Windows and BSD disagree on AF_* numbering, so the raw bytes cannot be
/// used directly.
struct MappedSockaddr {
    sockaddr_storage ss{};
    socklen_t len{0};
};

/// Render a decoded address as text, and report its port separately.
void format_mapped_sockaddr(const MappedSockaddr& addr, char* buf, size_t buflen,
                            u16* port_out);

/// Write a host sockaddr back into guest memory in Windows form. Returns the
/// number of bytes written, or 0 when the guest buffer is too small.
[[nodiscard]] u32 write_win_sockaddr(GuestMemory& mem, GuestAddr ptr,
                                     u32 guest_capacity,
                                     const sockaddr_storage& ss);
[[nodiscard]] u32 write_win_sockaddr(Marshaler& m, GuestAddr ptr, u32 guest_capacity,
                                     const sockaddr_storage& ss);

/// Return address of the guest code that called the current thunk, for
/// diagnostics that need to say which caller triggered a socket operation.
[[nodiscard]] u64 guest_caller_ret(Marshaler& m);

// -----------------------------------------------------------------------------
// Name resolution (ws2_32_resolve.cpp)
// -----------------------------------------------------------------------------

void impl_freeaddrinfo(Marshaler& m, Ws2Manager& mgr);
void impl_gethostbyname(Marshaler& m, Ws2Manager& mgr);
void impl_getaddrinfo(Marshaler& m, Ws2Manager& mgr);
void impl_FreeAddrInfoW(Marshaler& m, Ws2Manager& mgr);
void impl_GetAddrInfoW(Marshaler& m, Ws2Manager& mgr);
void impl_GetAddrInfoExW(Marshaler& m, Ws2Manager& mgr);
void impl_FreeAddrInfoExW(Marshaler& m, Ws2Manager& mgr);
void impl_GetAddrInfoExCancel(Marshaler& m, Ws2Manager& mgr);
void impl_inet_ntop(Marshaler& m, Ws2Manager& mgr);
void impl_inet_pton(Marshaler& m, Ws2Manager& mgr);
void impl_gethostbyaddr(Marshaler& m, Ws2Manager& mgr);
void impl_getservbyname(Marshaler& m, Ws2Manager& mgr);

/// Service name for a host-order port from the Windows services database
/// (ws2_netdb.cpp); empty when the port has no entry for that protocol.
[[nodiscard]] std::string ws2_service_name(u16 port, bool udp);
/// Port (host order) of a named service for "tcp" or "udp" in the Windows
/// services database, by name or alias.
[[nodiscard]] std::optional<u16> ws2_service_port(const std::string& name,
                                                  const char* proto);
void impl_getnameinfo(Marshaler& m, Ws2Manager& mgr);
void impl_GetNameInfoW(Marshaler& m, Ws2Manager& mgr);

/// Build the calling thread's HOSTENT (ws2_netdb.cpp) in the pointer-width
/// layout of the guest. Returns 0 when no storage could be allocated.
[[nodiscard]] GuestAddr write_hostent(Marshaler& m, Ws2Manager& mgr,
                                      const std::string& name,
                                      const std::vector<std::string>& aliases,
                                      u16 addrtype,
                                      const std::vector<std::vector<u8>>& addrs);

/// Bytes needed for a packed HOSTENT + strings in `buf` (WSAAsyncGetHostBy*).
[[nodiscard]] u32 hostent_packed_size(Marshaler& m, const std::string& name,
                                      const std::vector<std::string>& aliases,
                                      const std::vector<std::vector<u8>>& addrs);

/// Write HOSTENT into a caller buffer (MAXGETHOSTSTRUCT). False if too small.
[[nodiscard]] bool write_hostent_into(
    Marshaler& m, GuestAddr base, u32 capacity, const std::string& name,
    const std::vector<std::string>& aliases, u16 addrtype,
    const std::vector<std::vector<u8>>& addrs);


/// Winsock's SOCKET_ERROR: -1 widened to the DWORD a guest compares against.
inline constexpr u32 SOCKET_ERROR_VAL = static_cast<u32>(-1);  // 0xFFFFFFFF

/// VITREO_WSA_EVENT_DIAG: trace WSAEventSelect / WSAWaitForMultipleEvents.
[[nodiscard]] inline bool wsa_event_diag_enabled() {
    static const bool enabled =
        std::getenv("VITREO_WSA_EVENT_DIAG") != nullptr;
    return enabled;
}

/// Learn WSAEventSelect: recv, recvfrom, WSARecv, WSARecvEx and WSARecvFrom
/// re-enable FD_READ and FD_OOB whatever they return. Held for the call, so
/// every return path re-enables.
class FdReadReenable {
public:
    FdReadReenable(Ws2Manager& mgr, u32 guest_socket)
        : m_mgr(mgr), m_socket(guest_socket) {}
    ~FdReadReenable() { m_mgr.reenable_fd_read(m_socket, -1); }
    FdReadReenable(const FdReadReenable&) = delete;
    FdReadReenable& operator=(const FdReadReenable&) = delete;

private:
    Ws2Manager& m_mgr;
    u32 m_socket;
};

/// Darwin: keep SIGPIPE from killing the process on send to a closed peer.
inline void suppress_sigpipe(int fd) {
    int enabled = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, sizeof(enabled));
}

/// Winsock's INVALID_SOCKET: the all-ones DWORD a guest compares against, which
/// is not the -1 that BSD returns as an int.
inline constexpr u32 INVALID_SOCKET_VAL = 0xFFFFFFFF;

/// A SOCKET argument. SOCKET is UINT_PTR: on x64 a value with any of the high
/// 32 bits set is not a Vitreo socket (handles live below 2^32), so it maps to
/// 0, which no socket uses, instead of aliasing a live handle by truncation.
[[nodiscard]] inline u32 ws2_socket_arg(Marshaler& m, usize index) {
    const u64 v = m.arg(index);
    if (m.cpu_is_64bit() && (v >> 32) != 0) return 0;
    return static_cast<u32>(v);
}

/// Map a failed host send/recv errno to the Winsock error. A blocking socket
/// with SO_RCVTIMEO/SO_SNDTIMEO set reports the host timeout as EAGAIN;
/// Winsock reports WSAETIMEDOUT (ws2_opts_probe rcvtimeo_recv: err=10060).
[[nodiscard]] u32 ws2_io_error(Ws2Manager& mgr, u32 guest_socket, int fd, int err,
                               bool is_send);

/// FIONBIO / FIONREAD / SIOCATMARK for ioctlsocket and WSAIoctl
/// (ws2_sockopt.cpp). *inout is the FIONBIO input or the query result.
/// Returns 0 or the WSA error.
[[nodiscard]] u32 ws2_unix_ioctl(Ws2Manager& mgr, u32 guest_socket, int fd, u32 cmd,
                                 u32* inout);

/// Put Windows' per-socket defaults on a new host socket (ws2_sockopt.cpp).
void apply_windows_socket_defaults(int fd, int host_af, int host_type);

/// WSAPROTOCOL_INFO{A,W} (628 / 372 bytes) of the MSAFD provider behind host
/// socket `fd`, as SO_PROTOCOL_INFO{A,W} reports it (ws2_sockopt.cpp).
bool ws2_write_socket_protocol_info(Marshaler& m, GuestAddr base, bool unicode, int fd);

/// closesocket without the thunk: releases the guest SOCKET, completes its
/// pending overlapped I/O and drops the per-socket diagnostics
/// (ws2_socket_lifecycle.cpp). False for an unknown handle.
bool close_guest_socket(Ws2Manager& mgr, u32 guest_socket, bool reset = false);

/// Winsock's receive-side check of a stream socket whose local side aborted
/// the connection, or that is shut for receive (ws2_recv_connect.cpp):
/// 0 to proceed, else the WSA error to fail with.
[[nodiscard]] u32 ws2_stream_recv_gate(Ws2Manager& mgr, u32 guest_socket, int fd);

/// Abort a connected stream the way AFD does when received data cannot be
/// copied to the caller: the host connection is reset (the peer sees
/// WSAECONNRESET) and the socket fails later I/O with WSAECONNABORTED.
void ws2_abort_stream(Ws2Manager& mgr, u32 guest_socket, int fd);

/// True when [addr, addr+len) is writable (or readable) guest memory.
[[nodiscard]] bool ws2_guest_range_ok(Marshaler& m, GuestAddr addr, u64 len,
                                      bool write);

/// Return INVALID_SOCKET, which is (SOCKET)~0: SOCKET is UINT_PTR, so on x64
/// the guest compares all 64 bits of RAX (ws2_core_probe: raw=0xffffffffffffffff
/// on x64, 0xffffffff on x86). A zero-extended 0xFFFFFFFF reads as a valid
/// handle to a 64-bit caller.
inline void set_invalid_socket_return(Marshaler& m) {
    m.set_return(m.cpu_is_64bit() ? ~u64{0} : u64{INVALID_SOCKET_VAL});
}

/// Learn: most Winsock APIs fail with WSANOTINITIALISED (10093) when the
/// WSAStartup refcount is zero (ws2_core_probe pre_* / stale_* / rc_socket_after_clean2).
/// Byte-order helpers, inet_pton/inet_addr, __WSAFDIsSet, and WSACreateEvent do not.
enum class Ws2NotInitReturn { SocketError, InvalidSocket, ErrorAsReturn, NullPointer };

inline bool require_winsock(Ws2Manager& mgr, Marshaler& m, Ws2NotInitReturn how) {
    if (mgr.winsock_ready()) return true;
    mgr.set_last_error(WSANOTINITIALISED);
    switch (how) {
    case Ws2NotInitReturn::InvalidSocket:
        set_invalid_socket_return(m);
        break;
    case Ws2NotInitReturn::ErrorAsReturn:
        m.set_return(WSANOTINITIALISED);
        break;
    case Ws2NotInitReturn::NullPointer:
        m.set_return(0);
        break;
    case Ws2NotInitReturn::SocketError:
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        break;
    }
    return false;
}

/// NTSTATUS values the overlapped path writes into an OVERLAPPED's Internal
/// field. STATUS_PENDING is what a caller polls on until the operation lands.
inline constexpr u32 STATUS_SUCCESS  = 0;
inline constexpr u32 STATUS_PENDING  = 0x00000103;
inline constexpr u32 STATUS_CANCELLED = 0xC0000120;

// -----------------------------------------------------------------------------
// WSAMSG control data and overlapped status mapping (ws2_msg_control.cpp)
// -----------------------------------------------------------------------------

/// Per-socket receive-control options (SocketState::recv_ctl): which
/// WSACMSGHDR messages WSARecvMsg delivers (ws2ipdef.h IP_PKTINFO..IP_RECVECN).
namespace ws2ctl {
inline constexpr u32 kV4Pktinfo = 1u << 0;    // IP_PKTINFO
inline constexpr u32 kV4Hoplimit = 1u << 1;   // IP_HOPLIMIT / IP_RECVTTL
inline constexpr u32 kV4Recvif = 1u << 2;     // IP_RECVIF
inline constexpr u32 kV4Dstaddr = 1u << 3;    // IP_RECVDSTADDR
inline constexpr u32 kV4Tos = 1u << 4;        // IP_RECVTOS / IP_RECVTCLASS
inline constexpr u32 kV4ArrivalIf = 1u << 5;  // IP_ORIGINAL_ARRIVAL_IF
inline constexpr u32 kV4Ecn = 1u << 6;        // IP_RECVECN
inline constexpr u32 kV6Pktinfo = 1u << 8;    // IPV6_PKTINFO
inline constexpr u32 kV6Hoplimit = 1u << 9;   // IPV6_HOPLIMIT
inline constexpr u32 kV6Recvif = 1u << 10;    // IPV6_RECVIF
inline constexpr u32 kV6Dstaddr = 1u << 11;   // IPV6_RECVDSTADDR
inline constexpr u32 kV6Tclass = 1u << 12;    // IPV6_RECVTCLASS
inline constexpr u32 kV6Ecn = 1u << 13;       // IPV6_RECVECN
} // namespace ws2ctl

/// The ws2ctl bit for a Windows (level, optname), or 0 when it is not a
/// receive-control option.
[[nodiscard]] u32 ws2_recv_ctl_bit(int level, int opt);
/// Enable the host receive options `mask` needs on `fd`. 0 or a WSA error.
[[nodiscard]] u32 ws2_apply_recv_ctl_host(int fd, int host_family, u32 mask);
/// The Windows control-data image (packed WSACMSGHDRs) for a datagram the
/// host received into `native`, per the socket's `mask`.
[[nodiscard]] std::vector<u8> ws2_build_recv_control(const struct msghdr& native,
                                                     u32 mask, bool is64,
                                                     int host_family);
struct Ws2ControlPublish {
    u32 control_len = 0;  // value for WSAMSG.Control.len
    bool truncated = false;  // MSG_CTRUNC: the image did not fit
};
/// Copy the image into WSAMSG.Control as Windows does, truncating per bitness.
[[nodiscard]] Ws2ControlPublish ws2_publish_recv_control(
    GuestMemory& mem, bool is64, GuestAddr buf, u32 cap, const std::vector<u8>& image);
struct Ws2SendControl {
    std::vector<u8> native;  // host msg_control bytes
    u32 segment = 0;         // UDP_SEND_MSG_SIZE, 0 = one datagram
};
/// Validate and translate WSASendMsg control data. 0 or the WSA error.
[[nodiscard]] u32 ws2_translate_send_control(GuestMemory& mem, bool is64,
                                             GuestAddr buf, u32 len, int fd,
                                             int host_family,
                                             const struct sockaddr* dest,
                                             Ws2SendControl* out);
/// OVERLAPPED.Internal for a failed overlapped Winsock operation, and the
/// Winsock error WSAGetOverlappedResult reports for a status.
[[nodiscard]] u32 ws2_wsa_error_to_ntstatus(u32 wsa);
[[nodiscard]] u32 ws2_ntstatus_to_wsa_error(u32 status);

/// Read a Windows sockaddr_in (16 bytes, AF=2) or sockaddr_in6 (28 bytes,
/// AF=23) out of guest memory into a host sockaddr. Windows numbers AF_INET6 23
/// and Darwin numbers it 30, so this translates rather than copies — passing an
/// IPv6 address down the IPv4 path reads flowinfo bytes as the address and
/// connects to 0.0.0.0 with a family macOS rejects.
[[nodiscard]] bool read_win_sockaddr_mapped(Marshaler& m, GuestAddr ptr,
                                            int addrlen, MappedSockaddr& out);

/// VITREO_WSA_DIAG tracing for a completed receive.
void diagnose_recv(u32 guest_socket, ssize_t rc, int err);
void track_recv(u32 guest_socket, ssize_t bytes);
void track_send(u32 guest_socket, ssize_t bytes);
void dump_socket_stats(u32 guest_socket);

/// VITREO_TRACE gating for the socket calls, and a numeric environment override
/// with a default — both already had external linkage; only the declaration was
/// missing, which is why every file that wanted them re-derived them instead.
[[nodiscard]] bool ws2_trace_enabled();
[[nodiscard]] u64 env_u64_or(const char* name, u64 fallback);

// Windows shutdown() how constants (not the same numbering as POSIX SHUT_*).
inline constexpr int SD_RECEIVE = 0;
inline constexpr int SD_SEND = 1;
inline constexpr int SD_BOTH = 2;

[[nodiscard]] int translate_recv_flags(int win_flags);
[[nodiscard]] int translate_send_flags(int win_flags);
/// Validate Windows recv/send flags; returns 0 or WSAEOPNOTSUPP and fills *native.
[[nodiscard]] u32 check_recv_flags(int win_flags, bool datagram, bool nonblocking,
                                   int* native_out);
[[nodiscard]] u32 check_send_flags(int win_flags, int* native_out);

/// Datagram recv via recvmsg so MSG_TRUNC maps to WSAEMSGSIZE (Learn recv /
/// ws2_core_probe udp_recv_trunc). `copied` is bytes placed in `buf` (0 when
/// len==0); `truncated` is set when the datagram exceeded `len`.
struct DatagramRecvResult {
    ssize_t copied = 0;
    bool truncated = false;
    bool failed = false;
    int sys_errno = 0;
    sockaddr_storage from{};
    socklen_t from_len = 0;
};
[[nodiscard]] DatagramRecvResult recv_datagram_msg(
    int native_fd, void* buf, size_t len, int native_flags, bool want_from);

[[nodiscard]] u64 ns_between(std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b);
[[nodiscard]] u64 now_steady_ns();
[[nodiscard]] bool ws2_perf_enabled();
void maybe_emit_ws2_perf_live();

struct Ws2PerfStats {
    std::atomic<u64> recv_calls{0};
    std::atomic<u64> recv_bytes{0};
    std::atomic<u64> recv_wait_ns{0};
    std::atomic<u64> send_calls{0};
    std::atomic<u64> send_bytes{0};
    std::atomic<u64> send_wait_ns{0};
    std::atomic<u64> select_calls{0};
    std::atomic<u64> select_wait_ns{0};
    std::atomic<u64> select_skipped{0};
    std::atomic<u64> start_ns{0};
};
extern Ws2PerfStats s_ws2_perf_stats;
extern std::atomic<u64> s_ws2_io_gen;

void recv_sha1_update(u32 guest_socket, const void* data, size_t n);
void recv_sha1_finalize(u32 guest_socket);

[[nodiscard]] bool tls_dump_enabled();

// -----------------------------------------------------------------------------
// Export handlers
// -----------------------------------------------------------------------------
//
// The handlers ws2_32_registration.cpp names. They live in a nested namespace
// rather than directly in vitreo::winapi because promoting them out of file
// scope would otherwise put names like impl_socket and impl_WSAStartup into the
// namespace every Winsock DLL shares — wsock32.cpp keeps its own static
// impl_WSAStartup, and two of those in one namespace is a trap waiting for
// whoever includes both headers.

namespace ws2 {

void impl_WSAStartup(Marshaler& m, Ws2Manager& mgr);
void impl_WSACleanup(Marshaler& m, Ws2Manager& mgr);
void impl_WSARecvMsg(Marshaler& m, Ws2Manager& mgr);
void impl_WSAGetOverlappedResult(Marshaler& m, Ws2Manager& mgr);
void impl_WSASendMsg(Marshaler& m, Ws2Manager& mgr);
void impl_ConnectEx(Marshaler& m, Ws2Manager& mgr);
void impl_DisconnectEx(Marshaler& m, Ws2Manager& mgr);
void impl_TransmitFile(Marshaler& m, Ws2Manager& mgr);
void impl_TransmitPackets(Marshaler& m, Ws2Manager& mgr);
void impl_GetHostNameW(Marshaler& m, Ws2Manager& mgr);
void impl_bind(Marshaler& m, Ws2Manager& mgr);
void impl_closesocket(Marshaler& m, Ws2Manager& mgr);
void impl_connect(Marshaler& m, Ws2Manager& mgr);
void impl_ioctlsocket(Marshaler& m, Ws2Manager& mgr);
void impl_getpeername(Marshaler& m, Ws2Manager& mgr);
void impl_gethostname(Marshaler& m, Ws2Manager& mgr);
void impl_getprotobyname(Marshaler& m, Ws2Manager& mgr);
void impl_getprotobynumber(Marshaler& m, Ws2Manager& mgr);
void impl_getservbyport(Marshaler& m, Ws2Manager& mgr);
void impl_getsockname(Marshaler& m, Ws2Manager& mgr);
void impl_getsockopt(Marshaler& m, Ws2Manager& mgr);
void impl_inet_addr(Marshaler& m, Ws2Manager& /*mgr*/);
void impl_inet_ntoa(Marshaler& m, Ws2Manager& mgr);
void impl_setsockopt(Marshaler& m, Ws2Manager& mgr);
void impl_listen(Marshaler& m, Ws2Manager& mgr);
void impl_accept(Marshaler& m, Ws2Manager& mgr);
void impl_ntohl(Marshaler& m, Ws2Manager& /*mgr*/);
void impl_htonl(Marshaler& m, Ws2Manager& /*mgr*/);
void impl_ntohs(Marshaler& m, Ws2Manager& /*mgr*/);
void impl_htons(Marshaler& m, Ws2Manager& /*mgr*/);
void impl_WSANtohs(Marshaler& m, Ws2Manager& mgr);
void impl_WSAHtons(Marshaler& m, Ws2Manager& mgr);
void impl_WSANtohl(Marshaler& m, Ws2Manager& mgr);
void impl_WSAHtonl(Marshaler& m, Ws2Manager& mgr);
void impl_WSAAccept(Marshaler& m, Ws2Manager& mgr);
void impl_WSAPoll(Marshaler& m, Ws2Manager& mgr);
void impl_shutdown(Marshaler& m, Ws2Manager& mgr);
void impl_WSASendDisconnect(Marshaler& m, Ws2Manager& mgr);
void impl_WSAAsyncGetHostByName(Marshaler& m, Ws2Manager& mgr);
void impl_WSACancelAsyncRequest(Marshaler& m, Ws2Manager& mgr);
void impl_recvfrom(Marshaler& m, Ws2Manager& mgr);
void impl_sendto(Marshaler& m, Ws2Manager& mgr);
void impl_recv(Marshaler& m, Ws2Manager& mgr);
void impl_select(Marshaler& m, Ws2Manager& mgr);
void impl_send(Marshaler& m, Ws2Manager& mgr);
void impl_socket(Marshaler& m, Ws2Manager& mgr);
void impl_WSAGetLastError(Marshaler& m, Ws2Manager& mgr);
void impl_WSASetLastError(Marshaler& m, Ws2Manager& mgr);
void impl_WSAIoctl(Marshaler& m, Ws2Manager& mgr);
void impl_WSARecv(Marshaler& m, Ws2Manager& mgr);
void impl_WSARecvFrom(Marshaler& m, Ws2Manager& mgr);
void impl_WSASend(Marshaler& m, Ws2Manager& mgr);
void impl_WSASendTo(Marshaler& m, Ws2Manager& mgr);
void impl_WSASocketA(Marshaler& m, Ws2Manager& mgr);
void impl_WSADuplicateSocket(Marshaler& m, Ws2Manager& mgr, bool unicode);
void impl_WSASocketW(Marshaler& m, Ws2Manager& mgr);
void impl_WSAStringToAddressA(Marshaler& m, Ws2Manager& mgr);
void impl_WSAAddressToStringA(Marshaler& m, Ws2Manager& mgr);
void impl_WSAStringToAddressW(Marshaler& m, Ws2Manager& mgr);
void impl_WSAAddressToStringW(Marshaler& m, Ws2Manager& mgr);
void impl_WSAEnumProtocolsA(Marshaler& m, Ws2Manager& mgr);
void impl_WSAEnumProtocolsW(Marshaler& m, Ws2Manager& mgr);
void impl_WSAConnect(Marshaler& m, Ws2Manager& mgr);

} // namespace ws2

} // namespace vitreo::winapi
