// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32_helpers.cpp
/// @brief Shared Winsock helpers promoted out of ws2_32.cpp for TU splits.

#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace vitreo::winapi {

[[nodiscard]] u64 env_u64_or(const char* name, u64 fallback) {
    const char* s = std::getenv(name);
    if (!s || *s == '\0') {
        return fallback;
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s) {
        return fallback;
    }
    return static_cast<u64>(v);
}

/// Windows AF_INET6 is 23; macOS/BSD use a different constant (30).
/// Microsoft Learn SOCKADDR_IN6: family + port + flowinfo + 16-byte addr + scope.


/// Read a Windows sockaddr_in (16B, AF=2) or sockaddr_in6 (28B, AF=23) from
/// guest memory into a host sockaddr. Passing an IPv6 address through the
/// IPv4-only path used to treat flowinfo bytes as sin_addr → connect(0.0.0.0)
/// with sin_family still equal to Windows 23 (EINVAL on macOS).
bool read_win_sockaddr_mapped(Marshaler& m, GuestAddr ptr, int addrlen,
                                     MappedSockaddr& out) {
    if (!ptr || addrlen < 2) return false;
    auto fam = m.read_u16(ptr);
    if (fam.is_err()) return false;
    const u16 family = fam.value();
    std::memset(&out.ss, 0, sizeof(out.ss));

    if (family == kWinAfInet) {
        if (addrlen < 16) return false;
        Byte raw[16];
        if (m.memory().read_bytes(ptr, raw, 16).is_err()) return false;
        auto* sin = reinterpret_cast<sockaddr_in*>(&out.ss);
        sin->sin_len = static_cast<u8>(sizeof(sockaddr_in));
        sin->sin_family = AF_INET;
        std::memcpy(&sin->sin_port, raw + 2, 2);
        std::memcpy(&sin->sin_addr, raw + 4, 4);
        out.len = static_cast<socklen_t>(sizeof(sockaddr_in));
        return true;
    }

    if (family == kWinAfInet6) {
        if (addrlen < kWinSockaddrIn6Bytes) return false;
        Byte raw[kWinSockaddrIn6Bytes];
        if (m.memory()
                .read_bytes(ptr, raw, static_cast<usize>(kWinSockaddrIn6Bytes))
                .is_err()) {
            return false;
        }
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out.ss);
        sin6->sin6_len = static_cast<u8>(sizeof(sockaddr_in6));
        sin6->sin6_family = AF_INET6; // host constant (30 on macOS)
        std::memcpy(&sin6->sin6_port, raw + 2, 2);
        std::memcpy(&sin6->sin6_flowinfo, raw + 4, 4);
        std::memcpy(&sin6->sin6_addr, raw + 8, 16);
        std::memcpy(&sin6->sin6_scope_id, raw + 24, 4);
        out.len = static_cast<socklen_t>(sizeof(sockaddr_in6));
        return true;
    }

    // Windows sockaddr_un: u16 sun_family + char sun_path[108] (no sun_len).
    // Darwin inserts sun_len before sun_family; path must be remapped.
    if (family == kWinAfUnix) {
        if (addrlen < 2) return false;
        const int path_bytes =
            std::min(addrlen - 2, kWinUnixPathMax);
        Byte path[kWinUnixPathMax]{};
        if (path_bytes > 0 &&
            m.memory()
                .read_bytes(ptr + 2, path, static_cast<usize>(path_bytes))
                .is_err()) {
            return false;
        }
        auto* sun = reinterpret_cast<sockaddr_un*>(&out.ss);
        std::memset(sun, 0, sizeof(*sun));
        sun->sun_family = AF_UNIX;
        const usize host_cap = sizeof(sun->sun_path);
        const usize n = std::min(static_cast<usize>(path_bytes), host_cap);
        std::memcpy(sun->sun_path, path, n);
        const socklen_t host_len = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + n);
        sun->sun_len = static_cast<u8>(
            std::min(host_len, static_cast<socklen_t>(sizeof(sockaddr_un))));
        out.len = sun->sun_len;
        return true;
    }

    return false;
}

/// Legacy IPv4-only reader — rejects non-AF_INET so callers cannot silently
/// mis-parse sockaddr_in6 as sockaddr_in.
bool read_win_sockaddr(Marshaler& m, GuestAddr ptr, int addrlen,
                              struct sockaddr_in& out) {
    MappedSockaddr mapped{};
    if (!read_win_sockaddr_mapped(m, ptr, addrlen, mapped)) return false;
    if (mapped.ss.ss_family != AF_INET) return false;
    out = *reinterpret_cast<const sockaddr_in*>(&mapped.ss);
    return true;
}

void format_mapped_sockaddr(const MappedSockaddr& addr, char* buf,
                                   size_t buflen, u16* port_out) {
    if (port_out) *port_out = 0;
    if (!buf || buflen == 0) return;
    buf[0] = '\0';
    if (addr.ss.ss_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(&addr.ss);
        inet_ntop(AF_INET, &sin->sin_addr, buf, buflen);
        if (port_out) *port_out = ntohs(sin->sin_port);
    } else if (addr.ss.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&addr.ss);
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf, buflen);
        if (port_out) *port_out = ntohs(sin6->sin6_port);
    }
}

/// Host sockaddr → Windows guest layout. AF_INET6 must become Windows family
/// 23 and the 28-byte SOCKADDR_IN6 layout (Learn: SOCKADDR_IN6_LH) — never the
/// macOS sockaddr_in6 with sin6_len / family 30.
/// Returns bytes written (16 or 28), or 0 if family unsupported / buffer short.
u32 write_win_sockaddr(GuestMemory& mem, GuestAddr ptr, u32 guest_capacity,
                       const sockaddr_storage& ss) {
    if (!ptr || guest_capacity == 0) return 0;
    if (ss.ss_family == AF_INET) {
        if (guest_capacity < 16) return 0;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
        (void)mem.write_u16(ptr + 0, kWinAfInet);
        (void)mem.write_u16(ptr + 2, sin->sin_port);
        (void)mem.write_u32(ptr + 4, sin->sin_addr.s_addr);
        for (u32 j = 8; j < 16; j++) (void)mem.write_u8(ptr + j, 0);
        return 16;
    }
    if (ss.ss_family == AF_INET6) {
        if (guest_capacity < static_cast<u32>(kWinSockaddrIn6Bytes)) return 0;
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        Byte raw[kWinSockaddrIn6Bytes]{};
        // family (host order), port/flowinfo/addr/scope (network order) —
        // same packing as Windows SOCKADDR_IN6 (Learn: SOCKADDR_IN6_LH).
        const u16 win_family = kWinAfInet6;
        std::memcpy(raw + 0, &win_family, 2);
        std::memcpy(raw + 2, &sin6->sin6_port, 2);
        std::memcpy(raw + 4, &sin6->sin6_flowinfo, 4);
        std::memcpy(raw + 8, &sin6->sin6_addr, 16);
        std::memcpy(raw + 24, &sin6->sin6_scope_id, 4);
        if (mem.write_bytes(ptr, raw, static_cast<usize>(kWinSockaddrIn6Bytes))
                .is_err()) {
            return 0;
        }
        return static_cast<u32>(kWinSockaddrIn6Bytes);
    }
    if (ss.ss_family == AF_UNIX) {
        const auto* sun = reinterpret_cast<const sockaddr_un*>(&ss);
        u32 path_len = 0;
        if (sun->sun_len > offsetof(sockaddr_un, sun_path)) {
            path_len = static_cast<u32>(sun->sun_len -
                                        offsetof(sockaddr_un, sun_path));
        } else {
            path_len = static_cast<u32>(
                ::strnlen(sun->sun_path, sizeof(sun->sun_path)));
        }
        if (path_len > static_cast<u32>(kWinUnixPathMax)) {
            path_len = static_cast<u32>(kWinUnixPathMax);
        }
        const u32 need = 2u + path_len;
        if (guest_capacity < need) return 0;
        (void)mem.write_u16(ptr, kWinAfUnix);
        if (path_len > 0 &&
            mem.write_bytes(ptr + 2,
                            reinterpret_cast<const Byte*>(sun->sun_path),
                            path_len)
                .is_err()) {
            return 0;
        }
        return need;
    }
    return 0;
}

u32 write_win_sockaddr(Marshaler& m, GuestAddr ptr, u32 guest_capacity,
                       const sockaddr_storage& ss) {
    return write_win_sockaddr(m.memory(), ptr, guest_capacity, ss);
}

// WSA error codes

// =============================================================================
// Per-socket recv streaming SHA-1 (opt-in via VITREO_RECV_SHA1=1)
//
// Diagnostic for the "(1:200)" corruption: hashes the exact byte stream the
// guest observes via recv() so we can compare against a known-good reference
// (curl --raw of the same URL). Emitted on closesocket and at 8-MiB intervals.
// =============================================================================
#ifdef __APPLE__
struct RecvDigest {
    CC_SHA1_CTX ctx{};
    uint64_t    bytes      = 0;
    uint64_t    next_emit  = 8ull * 1024ull * 1024ull;
    RecvDigest() { CC_SHA1_Init(&ctx); }
};
static std::mutex s_recv_digest_mutex;
static std::unordered_map<u32, std::unique_ptr<RecvDigest>> s_recv_digests;

[[nodiscard]] static bool recv_sha1_enabled() {
    static const bool enabled = (std::getenv("VITREO_RECV_SHA1") != nullptr);
    return enabled;
}

void recv_sha1_update(u32 guest_socket, const void* data, size_t n) {
    if (!recv_sha1_enabled() || n == 0) return;
    std::lock_guard<std::mutex> lk(s_recv_digest_mutex);
    auto& slot = s_recv_digests[guest_socket];
    if (!slot) slot = std::make_unique<RecvDigest>();
    CC_SHA1_Update(&slot->ctx, data, static_cast<CC_LONG>(n));
    slot->bytes += n;
    if (slot->bytes >= slot->next_emit) {
        slot->next_emit += 8ull * 1024ull * 1024ull;
        // Snapshot the digest without consuming the context: copy, finalize
        // the copy, keep the live one running.
        CC_SHA1_CTX snap = slot->ctx;
        unsigned char d[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1_Final(d, &snap);
        char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1] = {};
        for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i)
            std::snprintf(hex + i * 2, 3, "%02x", d[i]);
        std::fprintf(stderr,
                     "[ws2-sha1] socket=0x%x bytes=%llu sha1=%s (partial)\n",
                     guest_socket,
                     static_cast<unsigned long long>(slot->bytes),
                     hex);
    }
}

void recv_sha1_finalize(u32 guest_socket) {
    if (!recv_sha1_enabled()) return;
    std::lock_guard<std::mutex> lk(s_recv_digest_mutex);
    auto it = s_recv_digests.find(guest_socket);
    if (it == s_recv_digests.end()) return;
    unsigned char d[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(d, &it->second->ctx);
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1] = {};
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", d[i]);
    std::fprintf(stderr,
                 "[ws2-sha1] socket=0x%x FINAL bytes=%llu sha1=%s\n",
                 guest_socket,
                 static_cast<unsigned long long>(it->second->bytes),
                 hex);
    s_recv_digests.erase(it);
}
#else
bool recv_sha1_enabled() { return false; }
void recv_sha1_update(u32, const void*, size_t) {}
void recv_sha1_finalize(u32) {}
#endif

Ws2PerfStats s_ws2_perf_stats;

/// Generation counter bumped on every recv/send/connect/close/accept.
/// Used by the impl_select zero-timeout fast path to invalidate its
/// thread-local "idle" cache whenever socket state may have changed.
std::atomic<u64> s_ws2_io_gen{0};
std::atomic<u64> s_ws2_perf_next_recv_bytes{8ull * 1024ull * 1024ull};

[[nodiscard]] u64 ns_between(std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b) {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

[[nodiscard]] u64 now_steady_ns() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

[[nodiscard]] bool ws2_perf_enabled() {
    // Marker file as well as env vars: a store-launched guest does not
    // reliably inherit the environment, so an env-only switch is silently off
    // in exactly the process whose sockets are in question.
    static const bool enabled =
        (::access("/tmp/vitreo-net-stats", F_OK) == 0) ||
        (std::getenv("VITREO_NET_STATS") != nullptr) ||
        (std::getenv("VITREO_IO_STATS") != nullptr);
    return enabled;
}

static void append_ws2_perf_line(const char* line) {
    FILE* fp = std::fopen("/tmp/vitreo_netstats.log", "a");
    if (!fp) return;
    std::fputs(line, fp);
    std::fputc('\n', fp);
    std::fclose(fp);
}

static void ensure_ws2_perf_log_initialized() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        if (!ws2_perf_enabled()) {
            return;
        }
        FILE* fp = std::fopen("/tmp/vitreo_netstats.log", "w");
        if (!fp) {
            return;
        }
        std::fputs("[ws2-perf] enabled\n", fp);
        std::fclose(fp);
        s_ws2_perf_stats.start_ns.store(now_steady_ns(), std::memory_order_relaxed);
    });
}

void maybe_emit_ws2_perf_live() {
    if (!ws2_perf_enabled()) return;
    ensure_ws2_perf_log_initialized();

    const u64 recv_b = s_ws2_perf_stats.recv_bytes.load(std::memory_order_relaxed);
    u64 next = s_ws2_perf_next_recv_bytes.load(std::memory_order_relaxed);
    if (recv_b < next) return;

    while (recv_b >= next) {
        const u64 desired = next + (8ull * 1024ull * 1024ull);
        if (s_ws2_perf_next_recv_bytes.compare_exchange_weak(
                next, desired, std::memory_order_relaxed, std::memory_order_relaxed)) {
            const u64 recv_calls = s_ws2_perf_stats.recv_calls.load(std::memory_order_relaxed);
            const u64 recv_ns = s_ws2_perf_stats.recv_wait_ns.load(std::memory_order_relaxed);
            const u64 send_calls = s_ws2_perf_stats.send_calls.load(std::memory_order_relaxed);
            const u64 send_b = s_ws2_perf_stats.send_bytes.load(std::memory_order_relaxed);
            const u64 send_ns = s_ws2_perf_stats.send_wait_ns.load(std::memory_order_relaxed);
            const u64 sel_calls = s_ws2_perf_stats.select_calls.load(std::memory_order_relaxed);
            const u64 sel_ns = s_ws2_perf_stats.select_wait_ns.load(std::memory_order_relaxed);
            const u64 sel_skip = s_ws2_perf_stats.select_skipped.load(std::memory_order_relaxed);
            const u64 start_ns = s_ws2_perf_stats.start_ns.load(std::memory_order_relaxed);
            const u64 elapsed_ns = (start_ns > 0) ? (now_steady_ns() - start_ns) : 0;
            const u64 io_ns = recv_ns + send_ns + sel_ns;
            const u64 other_ns = (elapsed_ns > io_ns) ? (elapsed_ns - io_ns) : 0;

            char line[512];
            std::snprintf(
                line, sizeof(line),
                "[ws2-perf-live] recv_calls=%llu recv_bytes=%llu recv_ms=%.3f "
                "send_calls=%llu send_bytes=%llu send_ms=%.3f "
                "select_calls=%llu select_skipped=%llu select_ms=%.3f "
                "elapsed_ms=%.3f other_ms=%.3f",
                static_cast<unsigned long long>(recv_calls),
                static_cast<unsigned long long>(recv_b),
                static_cast<double>(recv_ns) / 1e6,
                static_cast<unsigned long long>(send_calls),
                static_cast<unsigned long long>(send_b),
                static_cast<double>(send_ns) / 1e6,
                static_cast<unsigned long long>(sel_calls),
                static_cast<unsigned long long>(sel_skip),
                static_cast<double>(sel_ns) / 1e6,
                static_cast<double>(elapsed_ns) / 1e6,
                static_cast<double>(other_ns) / 1e6);
            std::fprintf(stderr, "%s\n", line);
            append_ws2_perf_line(line);
            return;
        }
    }
}

[[nodiscard]] bool tls_dump_enabled() {
    static const bool enabled = (std::getenv("VITREO_TLS_DUMP") != nullptr);
    return enabled;
}

// =============================================================================
// Socket flag translation (Windows → macOS/BSD)
// Windows and macOS share MSG_OOB(0x1), MSG_PEEK(0x2), MSG_DONTROUTE(0x4),
// but MSG_WAITALL differs: Windows=0x8, macOS=0x40.
// Unknown flag bits -> WSAEOPNOTSUPP (ws2_core_probe tcp_recv_flag_0x100 /
// send_flag_0x100 / send_flag_waitall). MSG_WAITALL on a non-blocking or
// datagram socket -> WSAEOPNOTSUPP (udp/tcp_recv_waitall_nb). MSG_PARTIAL
// (0x8000) is accepted and ignored for host I/O.
// =============================================================================
namespace {
constexpr int kWinMsgOob = 0x1;
constexpr int kWinMsgPeek = 0x2;
constexpr int kWinMsgDontRoute = 0x4;
constexpr int kWinMsgWaitAll = 0x8;
constexpr int kWinMsgPartial = 0x8000;
}  // namespace

int translate_recv_flags(int win_flags) {
    int native = 0;
    if (win_flags & kWinMsgOob) native |= MSG_OOB;
    if (win_flags & kWinMsgPeek) native |= MSG_PEEK;
    if (win_flags & kWinMsgWaitAll) native |= MSG_WAITALL;
    return native;
}

int translate_send_flags(int win_flags) {
    int native = 0;
    if (win_flags & kWinMsgOob) native |= MSG_OOB;
    if (win_flags & kWinMsgDontRoute) native |= MSG_DONTROUTE;
    return native;
}

u32 check_recv_flags(int win_flags, bool datagram, bool nonblocking, int* native_out) {
    constexpr int kKnown = kWinMsgOob | kWinMsgPeek | kWinMsgWaitAll | kWinMsgPartial;
    if ((win_flags & ~kKnown) != 0) return WSAEOPNOTSUPP;
    if ((win_flags & kWinMsgWaitAll) != 0 && (datagram || nonblocking)) {
        return WSAEOPNOTSUPP;
    }
    *native_out = translate_recv_flags(win_flags);
    return 0;
}

u32 check_send_flags(int win_flags, int* native_out) {
    constexpr int kKnown = kWinMsgOob | kWinMsgDontRoute | kWinMsgPartial;
    // MSG_WAITALL is not a send flag on Windows (send_flag_waitall -> 10045).
    if ((win_flags & ~kKnown) != 0) return WSAEOPNOTSUPP;
    *native_out = translate_send_flags(win_flags);
    return 0;
}

DatagramRecvResult recv_datagram_msg(int native_fd, void* buf, size_t len,
                                     int native_flags, bool want_from) {
    // Learn recv/recvfrom (message-oriented): fill buf with the first part of
    // the datagram and fail WSAEMSGSIZE when truncated; excess UDP is lost.
    // Darwin recv() silently truncates — use recvmsg + MSG_TRUNC instead
    // (ws2_core_probe udp_recv_trunc / udp_recv_len0 / recvfrom_trunc).
    DatagramRecvResult out;
    char sink = 0;
    struct iovec iov{};
    iov.iov_base = (len > 0 && buf != nullptr) ? buf : &sink;
    iov.iov_len = len;
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (want_from) {
        msg.msg_name = &out.from;
        msg.msg_namelen = sizeof(out.from);
    }
    const ssize_t rc = ::recvmsg(native_fd, &msg, native_flags);
    if (rc < 0) {
        out.failed = true;
        out.sys_errno = errno;
        return out;
    }
    out.copied = rc;
    out.truncated = (msg.msg_flags & MSG_TRUNC) != 0;
    // Zero-length buffer still consumes the datagram; Darwin reports
    // MSG_TRUNC. If a platform omits the flag, treat len==0 success as
    // truncated (ws2_core_probe udp_recv_len0 -> WSAEMSGSIZE).
    if (len == 0 && rc >= 0) out.truncated = true;
    if (want_from) out.from_len = msg.msg_namelen;
    return out;
}


bool ws2_guest_range_ok(Marshaler& m, GuestAddr addr, u64 len, bool write) {
    if (len == 0) return true;
    if (addr == 0) return false;
    if (write) {
        return m.memory().is_range_writable(addr, static_cast<usize>(len),
                                            m.cpu_is_64bit());
    }
    return m.memory().is_mapped(addr, static_cast<usize>(len)) &&
           m.memory().has_protection(addr, static_cast<usize>(len),
                                     MemoryProtection::Read);
}

void ws2_abort_stream(Ws2Manager& mgr, u32 guest_socket, int fd) {
    // Keep the peer for getpeername, then drop the connection with a reset
    // (SO_LINGER {1, 0} makes the TCP disconnect a drop) without releasing
    // the host descriptor the guest SOCKET still names.
    sockaddr_storage peer{};
    socklen_t pl = sizeof(peer);
    std::vector<u8> peer_bytes;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0) {
        const auto* b = reinterpret_cast<const u8*>(&peer);
        peer_bytes.assign(b, b + pl);
    }
    struct linger l{1, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    (void)::disconnectx(fd, SAE_ASSOCID_ANY, SAE_CONNID_ANY);
    mgr.update_socket_state(guest_socket, [&](Ws2Manager::SocketState& st) {
        st.aborted = true;
        if (!peer_bytes.empty()) st.aborted_peer = peer_bytes;
    });
}

u32 ws2_stream_recv_gate(Ws2Manager& mgr, u32 guest_socket, int fd) {
    const auto st = mgr.socket_state(guest_socket);
    if (st.aborted) return WSAECONNABORTED;
    // A failed connect leaves the socket unconnected (ws2_core_probe
    // connect_nb_refused_send: WSAENOTCONN, not Darwin's EPIPE/reset).
    if (st.connect_error != 0 && !st.connect_in_progress) return WSAENOTCONN;
    if (!st.shut_rd) return 0;
    // Learn shutdown: data arriving after SD_RECEIVE resets a TCP connection;
    // the host does the same. Winsock then reports the local abort
    // (ws2_core_probe tcp_recv_after_shutdown_data: WSAECONNABORTED).
    sockaddr_storage peer{};
    socklen_t pl = sizeof(peer);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0) {
        return WSAESHUTDOWN;
    }
    // The host no longer has a peer. A peer reset leaves ECONNRESET. Without
    // our own FIN (no SD_SEND) the connection cannot have closed gracefully,
    // so it was the reset for data arriving after SD_RECEIVE. With SD_SEND
    // latched too, FIN_WAIT_2 / LAST_ACK completion also drops the peer on
    // Darwin: that is a clean close, still WSAESHUTDOWN
    // (ShutdownDirectionsReportWsaeShutdown).
    int err = 0;
    socklen_t el = sizeof(err);
    (void)::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err == ECONNRESET) return WSAECONNRESET;
    if (st.shut_wr) return WSAESHUTDOWN;
    mgr.update_socket_state(guest_socket, [](Ws2Manager::SocketState& s) { s.aborted = true; });
    return WSAECONNABORTED;
}

// Return address of the guest code that called the current thunk.
//
// At thunk entry the return address sits at the top of the guest stack, so
// this is one read - but it must be a pointer-width read, not a fixed u64. On
// a 32-bit guest a u64 read splices the first stack argument into the high
// half and prints a nonsense address.
//
// Only a single frame, which is enough to name the calling function; there is
// no frame-pointer walk because optimised guest code frequently omits EBP.
u64 guest_caller_ret(Marshaler& m) {
    auto* cpu = m.cpu();
    if (cpu == nullptr) return 0;
    if (m.cpu_is_64bit()) {
        auto r = m.memory().read_u64(cpu->sp());
        return r.is_ok() ? r.value() : 0;
    }
    auto r = m.memory().read_u32(cpu->sp());
    return r.is_ok() ? static_cast<u64>(r.value()) : 0;
}

} // namespace vitreo::winapi
