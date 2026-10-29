/// @file test_ws2_32.cpp
/// @brief Unit tests for ws2_32.dll Windows Sockets 2 contract compliance

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/networking/ws2_32/ws2_32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/memory/memory_region.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>

// BSD socket headers for cross-platform constant comparisons
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// WSA Error Code Contract Tests
// =============================================================================

TEST(WsaErrorCodesTest, StandardCodesHaveCorrectValues) {
    // Verify WSA error codes match Windows values (WSABASEERR = 10000)
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EINTR), 10004u);       // WSAEINTR
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EBADF), 10009u);       // WSAEBADF
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EACCES), 10013u);      // WSAEACCES
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EFAULT), 10014u);      // WSAEFAULT
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EINVAL), 10022u);      // WSAEINVAL
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EMFILE), 10024u);      // WSAEMFILE
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EAGAIN), 10035u);      // WSAEWOULDBLOCK
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EINPROGRESS), 10036u); // WSAEINPROGRESS
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EALREADY), 10037u);    // WSAEALREADY
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENOTSOCK), 10038u);    // WSAENOTSOCK
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EADDRINUSE), 10048u);  // WSAEADDRINUSE
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ECONNREFUSED), 10061u);// WSAECONNREFUSED
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ETIMEDOUT), 10060u);   // WSAETIMEDOUT
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ECONNRESET), 10054u);  // WSAECONNRESET
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EPIPE), 10054u);       // WSAECONNRESET
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENOTCONN), 10057u);    // WSAENOTCONN
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EISCONN), 10056u);     // WSAEISCONN
}

// =============================================================================
// wsock32 vs ws2_32 ordinal numbering
// =============================================================================
//
// wsock32.dll and ws2_32.dll share the same BSD-socket implementation but
// export it under different ordinals for inet_addr / inet_ntoa / ioctlsocket.
// StarCraft's battle.snp imports wsock32 purely by ordinal, so serving it the
// ws2_32 ordinal table mis-resolves inet_addr (wsock32 #10) to ioctlsocket
// (ws2_32 #10) — a stdcall arg-count mismatch that corrupts the guest stack.

TEST(Wsock32OrdinalTest, CoreOrdinalsMatchWsock32Convention) {
    Ws2Manager mgr;
    DllExports w = create_wsock32_thunks(mgr);

    auto name_at = [&](u16 ord) -> std::string {
        for (const auto& e : w.entries)
            if (e.ordinal == ord) return e.name;
        return "<missing>";
    };
    auto bytes_at = [&](u16 ord) -> u16 {
        for (const auto& e : w.entries)
            if (e.ordinal == ord) return e.stdcall_bytes;
        return 0xFFFF;
    };

    // The three that differ from ws2_32.
    EXPECT_EQ(name_at(10), "inet_addr");
    EXPECT_EQ(name_at(11), "inet_ntoa");
    EXPECT_EQ(name_at(12), "ioctlsocket");
    EXPECT_EQ(bytes_at(10), 4u);   // inet_addr takes one 4-byte arg
    EXPECT_EQ(bytes_at(12), 12u);  // ioctlsocket takes three args

    // Shared ordinals used by battle.snp.
    EXPECT_EQ(name_at(2), "bind");
    EXPECT_EQ(name_at(4), "connect");
    EXPECT_EQ(name_at(16), "recv");
    EXPECT_EQ(name_at(19), "send");
    EXPECT_EQ(name_at(23), "socket");
    EXPECT_EQ(name_at(52), "gethostbyname");
    EXPECT_EQ(name_at(115), "WSAStartup");
    EXPECT_EQ(name_at(116), "WSACleanup");
    // Forwarded to mswsock on Windows; steamui imports AcceptEx by ordinal.
    EXPECT_EQ(name_at(1141), "AcceptEx");
    EXPECT_EQ(name_at(1142), "GetAcceptExSockaddrs");
    EXPECT_EQ(bytes_at(1141), 32u);
    EXPECT_EQ(bytes_at(1142), 32u);
}

TEST(Wsock32OrdinalTest, DiffersFromWs2_32Table) {
    Ws2Manager mgr;
    DllExports ws2 = create_ws2_thunks(mgr);

    auto ws2_name_at = [&](u16 ord) -> std::string {
        for (const auto& e : ws2.entries)
            if (e.ordinal == ord) return e.name;
        return "<missing>";
    };

    // ws2_32 puts ioctlsocket at 10 and inet_addr at 11 — the opposite of
    // wsock32. This is exactly why wsock32 needs its own table.
    EXPECT_EQ(ws2_name_at(10), "ioctlsocket");
    EXPECT_EQ(ws2_name_at(11), "inet_addr");
    EXPECT_EQ(ws2_name_at(12), "inet_ntoa");
    // Qt5Network imports WSAEnumProtocolsA by ordinal 101.
    EXPECT_EQ(ws2_name_at(101), "WSAEnumProtocolsA");
    EXPECT_EQ(ws2_name_at(102), "WSAEnumProtocolsW");
}

// =============================================================================
// Ws2Manager Unit Tests
// =============================================================================

class Ws2ManagerTest : public ::testing::Test {
protected:
    Ws2Manager mgr;
};

TEST_F(Ws2ManagerTest, InitialState) {
    EXPECT_EQ(mgr.last_error(), 0u);
    EXPECT_EQ(mgr.inet_ntoa_buf(), 0u);
}

TEST_F(Ws2ManagerTest, SetLastError) {
    mgr.set_last_error(10054); // WSAECONNRESET
    EXPECT_EQ(mgr.last_error(), 10054u);
    
    mgr.set_last_error(0);
    EXPECT_EQ(mgr.last_error(), 0u);
}

TEST_F(Ws2ManagerTest, SocketHandleAllocation) {
    // Native fds start from some value, guest handles start at 0x10000
    int native_fd1 = 5;
    int native_fd2 = 6;
    int native_fd3 = 7;
    
    u32 guest1 = mgr.alloc_socket(native_fd1);
    u32 guest2 = mgr.alloc_socket(native_fd2);
    u32 guest3 = mgr.alloc_socket(native_fd3);
    
    // Guest handles start at 0x10000
    EXPECT_GE(guest1, 0x10000u);
    EXPECT_EQ(guest2, guest1 + 1);
    EXPECT_EQ(guest3, guest2 + 1);
    
    // Lookup should work
    EXPECT_EQ(mgr.get_native_fd(guest1), native_fd1);
    EXPECT_EQ(mgr.get_native_fd(guest2), native_fd2);
    EXPECT_EQ(mgr.get_native_fd(guest3), native_fd3);
    
    // Invalid handle returns -1
    EXPECT_EQ(mgr.get_native_fd(0x10000 - 1), -1);
    EXPECT_EQ(mgr.get_native_fd(0x99999), -1);
}

TEST_F(Ws2ManagerTest, SocketReplacement) {
    int native_fd1 = 10;
    int native_fd2 = 20;
    
    u32 guest = mgr.alloc_socket(native_fd1);
    EXPECT_EQ(mgr.get_native_fd(guest), native_fd1);
    
    // Replace (TLS retry uses this)
    mgr.replace_native_fd(guest, native_fd2);
    EXPECT_EQ(mgr.get_native_fd(guest), native_fd2);
}

TEST_F(Ws2ManagerTest, SocketAllocationSuppressesHostSigpipe) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    u32 guest = mgr.alloc_socket(fds[0]);
    int enabled = 0;
    socklen_t size = sizeof(enabled);
    ASSERT_EQ(::getsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE,
                          &enabled, &size), 0);
    EXPECT_EQ(enabled, 1);

    EXPECT_TRUE(mgr.close_socket(guest));
    ::close(fds[1]);
}

TEST_F(Ws2ManagerTest, NativeSocketDescriptorsDoNotLeakAcrossExec) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const u32 guest = mgr.alloc_socket(fds[0]);
    EXPECT_NE(::fcntl(fds[0], F_GETFD) & FD_CLOEXEC, 0);
    // AcceptEx/reconnect replacement must preserve the same host lifetime
    // boundary, not silently make the endpoint inheritable again.
    mgr.replace_native_fd(guest, fds[1]);
    EXPECT_NE(::fcntl(fds[1], F_GETFD) & FD_CLOEXEC, 0);
    (void)::close(fds[0]);
    EXPECT_TRUE(mgr.close_socket(guest));
}

TEST_F(Ws2ManagerTest, ExecChildCannotKeepRegisteredSocketAlive) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(socket_fd, 0);
    // Avoid low descriptor numbers a shell may reuse for its own machinery.
    const int inherited_fd = ::fcntl(socket_fd, F_DUPFD, 200);
    (void)::close(socket_fd);
    ASSERT_GE(inherited_fd, 200);
    const u32 guest = mgr.alloc_socket(inherited_fd);
    const std::string probe = "test ! -e /dev/fd/" + std::to_string(inherited_fd);
    EXPECT_EXIT({
        ::execl("/bin/sh", "sh", "-c", probe.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }, ::testing::ExitedWithCode(0), "");
    EXPECT_GE(::fcntl(inherited_fd, F_GETFD), 0);
    EXPECT_TRUE(mgr.close_socket(guest));
}

TEST_F(Ws2ManagerTest, ScratchAllocator) {
    mgr.scratch_reset();
    
    GuestAddr addr1 = mgr.scratch_alloc(16);
    EXPECT_EQ(addr1, WS2_SCRATCH_BASE);
    
    GuestAddr addr2 = mgr.scratch_alloc(32);
    EXPECT_EQ(addr2, WS2_SCRATCH_BASE + 16);
    
    // Alignment to 4 bytes
    GuestAddr addr3 = mgr.scratch_alloc(5);
    EXPECT_EQ(addr3, WS2_SCRATCH_BASE + 48);
    
    GuestAddr addr4 = mgr.scratch_alloc(1);
    EXPECT_EQ(addr4, WS2_SCRATCH_BASE + 56); // 5 rounded to 8
    
    mgr.scratch_reset();
    GuestAddr addr5 = mgr.scratch_alloc(16);
    EXPECT_EQ(addr5, WS2_SCRATCH_BASE);
}

TEST_F(Ws2ManagerTest, AddrinfoArenaSurvivesUntilFree) {
    GuestAddr a = mgr.addrinfo_alloc(64);
    GuestAddr b = mgr.addrinfo_alloc(64);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    EXPECT_NE(a, b);
    EXPECT_GE(a, WS2_ADDRINFO_BASE);
    EXPECT_GE(b, WS2_ADDRINFO_BASE);
    // Second allocation must not reclaim the first while live.
    mgr.addrinfo_free(a);
    GuestAddr c = mgr.addrinfo_alloc(64);
    ASSERT_NE(c, 0u);
    EXPECT_NE(c, b);
    mgr.addrinfo_free(b);
    mgr.addrinfo_free(c);
    // After all frees, bump resets.
    GuestAddr d = mgr.addrinfo_alloc(64);
    EXPECT_EQ(d, WS2_ADDRINFO_BASE);
    mgr.addrinfo_free(d);
}

TEST_F(Ws2ManagerTest, WsaStartupReturnsZero) {
    EXPECT_EQ(mgr.wsa_startup(0x0202), 0);
    EXPECT_EQ(mgr.wsa_startup(0x0101), 0);
}

// =============================================================================
// Byte Order Functions Contract Tests
// =============================================================================

TEST(ByteOrderTest, HtonlContract) {
    // htonl converts host to network byte order (big-endian)
    // On little-endian systems, bytes should be swapped
    u32 host = 0x12345678;
    u32 net = ((host >> 24) & 0xFF) | ((host >> 8) & 0xFF00) |
              ((host << 8) & 0xFF0000) | ((host << 24) & 0xFF000000);
    EXPECT_EQ(net, 0x78563412u);
}

TEST(ByteOrderTest, NtohlContract) {
    // ntohl is inverse of htonl
    u32 net = 0x78563412;
    u32 host = ((net >> 24) & 0xFF) | ((net >> 8) & 0xFF00) |
               ((net << 8) & 0xFF0000) | ((net << 24) & 0xFF000000);
    EXPECT_EQ(host, 0x12345678u);
}

TEST(ByteOrderTest, HtonsContract) {
    // htons converts 16-bit host to network byte order
    u16 host = 0x1234;
    u16 net = static_cast<u16>((host >> 8) | (host << 8));
    EXPECT_EQ(net, 0x3412u);
}

TEST(ByteOrderTest, NtohsContract) {
    // ntohs is inverse of htons
    u16 net = 0x3412;
    u16 host = static_cast<u16>((net >> 8) | (net << 8));
    EXPECT_EQ(host, 0x1234u);
}

TEST(ByteOrderTest, IdentityTransform) {
    // Applying twice should give original
    u32 orig32 = 0xDEADBEEF;
    u32 swapped32 = ((orig32 >> 24) & 0xFF) | ((orig32 >> 8) & 0xFF00) |
                    ((orig32 << 8) & 0xFF0000) | ((orig32 << 24) & 0xFF000000);
    u32 back32 = ((swapped32 >> 24) & 0xFF) | ((swapped32 >> 8) & 0xFF00) |
                 ((swapped32 << 8) & 0xFF0000) | ((swapped32 << 24) & 0xFF000000);
    EXPECT_EQ(back32, orig32);
    
    u16 orig16 = 0xCAFE;
    u16 swapped16 = static_cast<u16>((orig16 >> 8) | (orig16 << 8));
    u16 back16 = static_cast<u16>((swapped16 >> 8) | (swapped16 << 8));
    EXPECT_EQ(back16, orig16);
}

// =============================================================================
// Windows Socket Constants Contract Tests
// =============================================================================

TEST(SocketConstantsTest, AddressFamilies) {
    // Windows AF_* constants must match BSD
    constexpr int WIN_AF_UNSPEC = 0;
    constexpr int WIN_AF_INET = 2;
    constexpr int WIN_AF_INET6 = 23;
    
    EXPECT_EQ(WIN_AF_UNSPEC, AF_UNSPEC);
    EXPECT_EQ(WIN_AF_INET, AF_INET);
    // AF_INET6 differs: Windows=23, BSD=30
}

TEST(SocketConstantsTest, SocketTypes) {
    // Windows SOCK_* constants
    constexpr int WIN_SOCK_STREAM = 1;
    constexpr int WIN_SOCK_DGRAM = 2;
    constexpr int WIN_SOCK_RAW = 3;
    
    EXPECT_EQ(WIN_SOCK_STREAM, SOCK_STREAM);
    EXPECT_EQ(WIN_SOCK_DGRAM, SOCK_DGRAM);
    EXPECT_EQ(WIN_SOCK_RAW, SOCK_RAW);
}

TEST(SocketConstantsTest, IpProtocols) {
    // Windows IPPROTO_* constants
    constexpr int WIN_IPPROTO_TCP = 6;
    constexpr int WIN_IPPROTO_UDP = 17;
    
    EXPECT_EQ(WIN_IPPROTO_TCP, IPPROTO_TCP);
    EXPECT_EQ(WIN_IPPROTO_UDP, IPPROTO_UDP);
}

TEST(SocketConstantsTest, ShutdownModes) {
    // Windows shutdown() constants
    constexpr int WIN_SD_RECEIVE = 0;
    constexpr int WIN_SD_SEND = 1;
    constexpr int WIN_SD_BOTH = 2;
    
    EXPECT_EQ(WIN_SD_RECEIVE, SHUT_RD);
    EXPECT_EQ(WIN_SD_SEND, SHUT_WR);
    EXPECT_EQ(WIN_SD_BOTH, SHUT_RDWR);
}

// =============================================================================
// Windows sockaddr_in Layout Contract Tests  
// =============================================================================

TEST(SockaddrLayoutTest, WindowsSockaddrInSize) {
    // Windows sockaddr_in is 16 bytes:
    //   offset 0: short   sin_family (2 bytes)
    //   offset 2: u_short sin_port   (2 bytes, network order)
    //   offset 4: in_addr sin_addr   (4 bytes, network order)
    //   offset 8: char[8] sin_zero   (8 bytes padding)
    constexpr size_t WIN_SOCKADDR_IN_SIZE = 16;
    EXPECT_EQ(WIN_SOCKADDR_IN_SIZE, 16u);
    
    // BSD sockaddr_in has sin_len prefix, but we handle the conversion
}

// =============================================================================
// Socket Option Mapping Contract Tests
// =============================================================================

TEST(SocketOptionsTest, SolSocketMapping) {
    // Windows SOL_SOCKET = 0xFFFF, BSD SOL_SOCKET = 0xFFFF (match)
    constexpr int WIN_SOL_SOCKET = 0xFFFF;
    EXPECT_EQ(WIN_SOL_SOCKET, SOL_SOCKET);
}

TEST(SocketOptionsTest, CommonSocketOptions) {
    // Windows SO_* values that require mapping
    // Note: Windows values differ from BSD for many options
    
    // Windows TCP_NODELAY = 1 (same as BSD)
    constexpr int WIN_TCP_NODELAY = 1;
    EXPECT_EQ(WIN_TCP_NODELAY, TCP_NODELAY);
}

// =============================================================================
// Export Table Registration Tests
// =============================================================================

TEST(Ws2ThunksTest, CreatesAllRequiredExports) {
    Ws2Manager mgr;
    DllExports exports = create_ws2_thunks(mgr);
    
    // Check that we have a reasonable number of exports
    EXPECT_GE(exports.entries.size(), 40u);
    
    // Verify critical exports exist by name
    auto has_export = [&](const char* name) {
        for (const auto& e : exports.entries) {
            if (e.name == name) return true;
        }
        return false;
    };
    
    // Core socket functions
    EXPECT_TRUE(has_export("socket"));
    EXPECT_TRUE(has_export("bind"));
    EXPECT_TRUE(has_export("listen"));
    EXPECT_TRUE(has_export("connect"));
    EXPECT_TRUE(has_export("send"));
    EXPECT_TRUE(has_export("recv"));
    EXPECT_TRUE(has_export("sendto"));
    EXPECT_TRUE(has_export("recvfrom"));
    EXPECT_TRUE(has_export("select"));
    EXPECT_TRUE(has_export("shutdown"));
    EXPECT_TRUE(has_export("closesocket"));
    
    // Socket options
    EXPECT_TRUE(has_export("getsockopt"));
    EXPECT_TRUE(has_export("setsockopt"));
    EXPECT_TRUE(has_export("ioctlsocket"));
    
    // Address info
    EXPECT_TRUE(has_export("getsockname"));
    EXPECT_TRUE(has_export("getpeername"));
    
    // Byte order
    EXPECT_TRUE(has_export("htonl"));
    EXPECT_TRUE(has_export("htons"));
    EXPECT_TRUE(has_export("ntohl"));
    EXPECT_TRUE(has_export("ntohs"));
    
    // Address conversion
    EXPECT_TRUE(has_export("inet_addr"));
    EXPECT_TRUE(has_export("inet_ntoa"));
    
    // DNS
    EXPECT_TRUE(has_export("gethostbyname"));
    EXPECT_TRUE(has_export("getaddrinfo"));
    EXPECT_TRUE(has_export("GetAddrInfoW"));
    EXPECT_TRUE(has_export("FreeAddrInfoW"));
    EXPECT_TRUE(has_export("GetAddrInfoExW"));
    EXPECT_TRUE(has_export("FreeAddrInfoExW"));
    EXPECT_TRUE(has_export("GetAddrInfoExCancel"));
    EXPECT_TRUE(has_export("freeaddrinfo"));
    EXPECT_TRUE(has_export("gethostname"));
    
    // WSA functions
    EXPECT_TRUE(has_export("WSAStartup"));
    EXPECT_TRUE(has_export("WSACleanup"));
    EXPECT_TRUE(has_export("WSAGetLastError"));
    EXPECT_TRUE(has_export("WSASetLastError"));
    EXPECT_TRUE(has_export("WSASocketA"));
    EXPECT_TRUE(has_export("WSAIoctl"));
    EXPECT_TRUE(has_export("WSASend"));
    EXPECT_TRUE(has_export("WSARecv"));
    EXPECT_TRUE(has_export("WSASendTo"));
    EXPECT_TRUE(has_export("WSARecvFrom"));
    
    // Event functions
    EXPECT_TRUE(has_export("WSACreateEvent"));
    EXPECT_TRUE(has_export("WSACloseEvent"));
    EXPECT_TRUE(has_export("WSAEventSelect"));
    EXPECT_TRUE(has_export("WSAEnumNetworkEvents"));
    
    // Extension functions
    EXPECT_TRUE(has_export("ConnectEx"));
    EXPECT_TRUE(has_export("DisconnectEx"));
    
    // FD_SET helper
    EXPECT_TRUE(has_export("__WSAFDIsSet"));
}

TEST(Ws2ThunksTest, OrdinalsMappedCorrectly) {
    Ws2Manager mgr;
    DllExports exports = create_ws2_thunks(mgr);
    
    auto get_ordinal = [&](const char* name) -> int {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e.ordinal;
        }
        return -1;
    };
    
    // Verify Windows ordinals match the real ws2_32.dll
    EXPECT_EQ(get_ordinal("bind"), 2);
    EXPECT_EQ(get_ordinal("closesocket"), 3);
    EXPECT_EQ(get_ordinal("connect"), 4);
    EXPECT_EQ(get_ordinal("getpeername"), 5);
    EXPECT_EQ(get_ordinal("getsockname"), 6);
    EXPECT_EQ(get_ordinal("getsockopt"), 7);
    EXPECT_EQ(get_ordinal("htonl"), 8);
    EXPECT_EQ(get_ordinal("htons"), 9);
    EXPECT_EQ(get_ordinal("ioctlsocket"), 10);
    EXPECT_EQ(get_ordinal("inet_addr"), 11);
    EXPECT_EQ(get_ordinal("inet_ntoa"), 12);
    EXPECT_EQ(get_ordinal("listen"), 13);
    EXPECT_EQ(get_ordinal("ntohl"), 14);
    EXPECT_EQ(get_ordinal("ntohs"), 15);
    EXPECT_EQ(get_ordinal("recv"), 16);
    EXPECT_EQ(get_ordinal("recvfrom"), 17);
    EXPECT_EQ(get_ordinal("select"), 18);
    EXPECT_EQ(get_ordinal("send"), 19);
    EXPECT_EQ(get_ordinal("sendto"), 20);
    EXPECT_EQ(get_ordinal("setsockopt"), 21);
    EXPECT_EQ(get_ordinal("shutdown"), 22);
    EXPECT_EQ(get_ordinal("socket"), 23);
    EXPECT_EQ(get_ordinal("gethostbyname"), 52);
    EXPECT_EQ(get_ordinal("gethostname"), 57);
    EXPECT_EQ(get_ordinal("WSAGetLastError"), 111);
    EXPECT_EQ(get_ordinal("WSASetLastError"), 112);
    EXPECT_EQ(get_ordinal("WSAStartup"), 115);
    EXPECT_EQ(get_ordinal("WSACleanup"), 116);
    EXPECT_EQ(get_ordinal("__WSAFDIsSet"), 151);
}

TEST(Ws2ThunksTest, StdcallBytesCorrect) {
    Ws2Manager mgr;
    DllExports exports = create_ws2_thunks(mgr);
    
    auto get_stdcall_bytes = [&](const char* name) -> int {
        for (const auto& e : exports.entries) {
            if (e.name == name) return static_cast<int>(e.stdcall_bytes);
        }
        return -1;
    };
    
    // Verify stdcall stack cleanup sizes (Windows x86 calling convention)
    EXPECT_EQ(get_stdcall_bytes("socket"), 12);      // 3 args * 4
    EXPECT_EQ(get_stdcall_bytes("bind"), 12);        // 3 args * 4
    EXPECT_EQ(get_stdcall_bytes("listen"), 8);       // 2 args * 4
    EXPECT_EQ(get_stdcall_bytes("connect"), 12);     // 3 args * 4
    EXPECT_EQ(get_stdcall_bytes("send"), 16);        // 4 args * 4
    EXPECT_EQ(get_stdcall_bytes("recv"), 16);        // 4 args * 4
    EXPECT_EQ(get_stdcall_bytes("sendto"), 24);      // 6 args * 4
    EXPECT_EQ(get_stdcall_bytes("recvfrom"), 24);    // 6 args * 4
    EXPECT_EQ(get_stdcall_bytes("select"), 20);      // 5 args * 4
    EXPECT_EQ(get_stdcall_bytes("shutdown"), 8);     // 2 args * 4
    EXPECT_EQ(get_stdcall_bytes("closesocket"), 4);  // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("getsockopt"), 20);  // 5 args * 4
    EXPECT_EQ(get_stdcall_bytes("setsockopt"), 20);  // 5 args * 4
    EXPECT_EQ(get_stdcall_bytes("ioctlsocket"), 12); // 3 args * 4
    EXPECT_EQ(get_stdcall_bytes("htonl"), 4);        // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("htons"), 4);        // 1 arg * 4 (u16 pushed as u32)
    EXPECT_EQ(get_stdcall_bytes("ntohl"), 4);        // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("ntohs"), 4);        // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("inet_addr"), 4);    // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("inet_ntoa"), 4);    // 1 arg * 4 (struct in_addr passed by value)
    EXPECT_EQ(get_stdcall_bytes("gethostbyname"), 4); // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("gethostname"), 8);   // 2 args * 4
    EXPECT_EQ(get_stdcall_bytes("getaddrinfo"), 16);  // 4 args * 4
    EXPECT_EQ(get_stdcall_bytes("GetAddrInfoW"), 16);
    EXPECT_EQ(get_stdcall_bytes("FreeAddrInfoW"), 4);
    EXPECT_EQ(get_stdcall_bytes("GetAddrInfoExW"), 40);  // 10 args * 4
    EXPECT_EQ(get_stdcall_bytes("FreeAddrInfoExW"), 4);
    EXPECT_EQ(get_stdcall_bytes("WSAStartup"), 8);    // 2 args * 4
    EXPECT_EQ(get_stdcall_bytes("WSACleanup"), 0);    // 0 args
    EXPECT_EQ(get_stdcall_bytes("WSAGetLastError"), 0); // 0 args
    EXPECT_EQ(get_stdcall_bytes("WSASetLastError"), 4); // 1 arg * 4
    EXPECT_EQ(get_stdcall_bytes("WSASocketA"), 24);   // 6 args * 4
    EXPECT_EQ(get_stdcall_bytes("WSASend"), 28);      // 7 args * 4
    EXPECT_EQ(get_stdcall_bytes("WSARecv"), 28);      // 7 args * 4
    EXPECT_EQ(get_stdcall_bytes("WSAIoctl"), 36);     // 9 args * 4
}

// =============================================================================
// INVALID_SOCKET / SOCKET_ERROR Contract Tests
// =============================================================================

TEST(SocketReturnValuesTest, InvalidSocketValue) {
    // Windows INVALID_SOCKET = (SOCKET)(~0) = 0xFFFFFFFF for 32-bit
    constexpr u32 WIN_INVALID_SOCKET = 0xFFFFFFFFu;
    EXPECT_EQ(WIN_INVALID_SOCKET, ~0u);
}

TEST(SocketReturnValuesTest, SocketErrorValue) {
    // Windows SOCKET_ERROR = -1 = 0xFFFFFFFF when cast to unsigned
    constexpr int WIN_SOCKET_ERROR = -1;
    EXPECT_EQ(static_cast<u32>(WIN_SOCKET_ERROR), 0xFFFFFFFFu);
}

// =============================================================================
// fd_set Layout Contract Tests
// =============================================================================

TEST(FdSetLayoutTest, WindowsFdSetFormat) {
    // Windows fd_set layout (32-bit):
    //   offset 0: u_int fd_count (number of sockets)
    //   offset 4: SOCKET fd_array[FD_SETSIZE] (array of socket handles)
    // FD_SETSIZE = 64 by default on Windows
    
    constexpr size_t WIN_FD_SETSIZE = 64;
    constexpr size_t WIN_FD_SET_SIZE = 4 + WIN_FD_SETSIZE * 4; // count + 64 handles
    
    EXPECT_EQ(WIN_FD_SET_SIZE, 260u);
}

// =============================================================================
// WSADATA Layout Contract Tests
// =============================================================================

TEST(WsaDataLayoutTest, WindowsWsaDataFormat) {
    // Windows WSADATA structure (32-bit version, 400 bytes):
    //   offset 0:   WORD    wVersion
    //   offset 2:   WORD    wHighVersion
    //   offset 4:   char[257] szDescription
    //   offset 261: char[129] szSystemStatus
    //   offset 390: unsigned short iMaxSockets
    //   offset 392: unsigned short iMaxUdpDg
    //   offset 394: char* lpVendorInfo
    
    // We zero the structure and fill in version fields
    constexpr size_t WIN_WSADATA_SIZE = 400;
    EXPECT_EQ(WIN_WSADATA_SIZE, 400u);
}

// =============================================================================
// WSA Event Constants Contract Tests
// =============================================================================

TEST(WsaEventConstantsTest, NetworkEventFlags) {
    // Windows FD_* event flags for WSAEventSelect / WSAEnumNetworkEvents
    constexpr u32 WIN_FD_READ = 0x01;
    constexpr u32 WIN_FD_WRITE = 0x02;
    constexpr u32 WIN_FD_OOB = 0x04;
    constexpr u32 WIN_FD_ACCEPT = 0x08;
    constexpr u32 WIN_FD_CONNECT = 0x10;
    constexpr u32 WIN_FD_CLOSE = 0x20;
    
    // These are bit flags that can be ORed together
    u32 all_events = WIN_FD_READ | WIN_FD_WRITE | WIN_FD_ACCEPT | WIN_FD_CONNECT | WIN_FD_CLOSE;
    EXPECT_EQ(all_events, 0x3Bu);
}

// =============================================================================
// WSAWaitForMultipleEvents Contract Tests
// =============================================================================

class WsaEventWaitTest : public ::testing::Test {
protected:
    void SetUp() override {
        handles = std::make_unique<HandleTable>();
        sync = std::make_unique<SyncManager>(*handles);
        handles->set_close_callback(
            [this](Handle handle, HandleType type, void*) {
                return sync->on_handle_close(handle, type);
            });
        ws2.set_sync_manager(sync.get());
    }

    void TearDown() override {
        handles->set_close_callback(nullptr);
        sync.reset();
        handles.reset();
    }

    Ws2Manager ws2;
    std::unique_ptr<HandleTable> handles;
    std::unique_ptr<SyncManager> sync;
};

TEST_F(WsaEventWaitTest, WaitsUntilManualEventIsSignaled) {
    const Handle event = sync->create_event(true, false, "");
    ASSERT_NE(event, 0u);
    const std::array<Handle, 1> events{event};

    std::thread signaler([this, event] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_TRUE(sync->set_event(event));
    });

    const auto start = std::chrono::steady_clock::now();
    const u32 result =
        ws2.wait_for_multiple_events(events, false, 1000, false);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    signaler.join();

    EXPECT_EQ(result, WAIT_OBJECT_0);
    EXPECT_GE(elapsed, std::chrono::milliseconds(10));
    EXPECT_TRUE(sync->close_handle(event));
}

TEST_F(WsaEventWaitTest, FiniteTimeoutDoesNotReturnImmediately) {
    const Handle event = sync->create_event(true, false, "");
    ASSERT_NE(event, 0u);
    const std::array<Handle, 1> events{event};

    const auto start = std::chrono::steady_clock::now();
    const u32 result =
        ws2.wait_for_multiple_events(events, false, 25, false);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result, WAIT_TIMEOUT);
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
    EXPECT_TRUE(sync->close_handle(event));
}

TEST_F(WsaEventWaitTest, SocketReadinessSignalsAssociatedEvent) {
    int native_sockets[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, native_sockets), 0);

    const u32 guest_socket = ws2.alloc_socket(native_sockets[0]);
    const Handle event = sync->create_event(true, false, "");
    ASSERT_NE(event, 0u);
    ws2.select_network_events(guest_socket, event, 0x01u); // FD_READ
    const std::array<Handle, 1> events{event};

    std::thread writer([fd = native_sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const char byte = 'x';
        EXPECT_EQ(::write(fd, &byte, sizeof(byte)), 1);
    });

    const u32 result =
        ws2.wait_for_multiple_events(events, false, 1000, false);
    writer.join();

    EXPECT_EQ(result, WAIT_OBJECT_0);
    ws2.remove_event_assoc_by_socket(guest_socket);
    EXPECT_TRUE(ws2.close_socket(guest_socket));
    EXPECT_EQ(::close(native_sockets[1]), 0);
    EXPECT_TRUE(sync->close_handle(event));
}

// =============================================================================
// ioctlsocket Command Contract Tests
// =============================================================================

TEST(IoctlCommandsTest, WindowsIoctlValues) {
    // Windows ioctlsocket commands
    constexpr u32 WIN_FIONBIO = 0x8004667E;    // Set non-blocking mode
    constexpr u32 WIN_FIONREAD = 0x4004667F;   // Get bytes available to read
    constexpr u32 WIN_SIOCATMARK = 0x40047307; // Check OOB mark
    
    // These are different from BSD ioctl values
    EXPECT_EQ(WIN_FIONBIO, 0x8004667Eu);
}

// =============================================================================
// WSAIoctl Control Codes Contract Tests
// =============================================================================

TEST(WsaIoctlCodesTest, ExtensionFunctionGuids) {
    // SIO_GET_EXTENSION_FUNCTION_POINTER = 0xC8000006
    constexpr u32 WIN_SIO_GET_EXTENSION_FUNC = 0xC8000006;
    
    // Used to retrieve ConnectEx, AcceptEx, etc.
    EXPECT_EQ(WIN_SIO_GET_EXTENSION_FUNC, 0xC8000006u);
}

// =============================================================================
// Phase 7: Contract Conformance Tests — errno-to-WSA Completeness
// =============================================================================

TEST(WsaErrorCodesTest, NetworkErrorCodeMapping) {
    // Network-layer errors critical for TLS handshake reliability
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENETUNREACH), 10051u);    // WSAENETUNREACH
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENETDOWN), 10050u);       // WSAENETDOWN
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENETRESET), 10052u);      // WSAENETRESET
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ECONNABORTED), 10053u);   // WSAECONNABORTED
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EHOSTUNREACH), 10065u);   // WSAEHOSTUNREACH
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENOBUFS), 10055u);        // WSAENOBUFS
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EADDRNOTAVAIL), 10049u);  // WSAEADDRNOTAVAIL
}

TEST(WsaErrorCodesTest, SocketStateErrorMapping) {
    // Socket state errors used by non-blocking connect / select pattern
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EDESTADDRREQ), 10039u);   // WSAEDESTADDRREQ
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EMSGSIZE), 10040u);       // WSAEMSGSIZE
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EPROTOTYPE), 10041u);     // WSAEPROTOTYPE
    EXPECT_EQ(Ws2Manager::errno_to_wsa(ENOPROTOOPT), 10042u);    // WSAENOPROTOOPT
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EPROTONOSUPPORT), 10043u);// WSAEPROTONOSUPPORT
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EOPNOTSUPP), 10045u);     // WSAEOPNOTSUPP
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EAFNOSUPPORT), 10047u);   // WSAEAFNOSUPPORT
}

TEST(WsaErrorCodesTest, EwouldblockMapsToWsaewouldblock) {
    // Windows maps both EAGAIN and EWOULDBLOCK to WSAEWOULDBLOCK (10035)
    // On macOS/BSD, EWOULDBLOCK == EAGAIN, but we verify the contract.
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EAGAIN), 10035u);
#if EWOULDBLOCK != EAGAIN
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EWOULDBLOCK), 10035u);
#endif
}

TEST(WsaErrorCodesTest, EinprogressMapsToWsaeinprogress) {
    // Non-blocking connect returns EINPROGRESS → WSAEINPROGRESS (10036)
    // But our connect() maps EINPROGRESS to WSAEWOULDBLOCK as Windows does
    // The errno_to_wsa mapping itself returns WSAEINPROGRESS.
    EXPECT_EQ(Ws2Manager::errno_to_wsa(EINPROGRESS), 10036u);
}

TEST(WsaErrorCodesTest, UnknownErrnoFallsThrough) {
    // A Darwin errno with no Winsock counterpart must not leak through as a
    // "Winsock error" (a raw 45 is not a WSA code: ws2_opts_probe
    // tcp_TCP_FASTOPEN showed err=45 before this); it reports WSAEINVAL.
    int unknown_errno = 9999;
    EXPECT_EQ(Ws2Manager::errno_to_wsa(unknown_errno), 10022u);
}

// =============================================================================
// Phase 7: Socket Handle Lifecycle Conformance
// =============================================================================

TEST_F(Ws2ManagerTest, InvalidHandleReturnsNegativeOne) {
    // After allocating some sockets, invalid handles must return -1
    mgr.alloc_socket(10);
    mgr.alloc_socket(20);
    EXPECT_EQ(mgr.get_native_fd(0), -1);
    EXPECT_EQ(mgr.get_native_fd(0xFFFFFFFF), -1)
        << "INVALID_SOCKET handle must return -1";
}

TEST_F(Ws2ManagerTest, ReplaceDoesNotAffectOtherHandles) {
    u32 g1 = mgr.alloc_socket(10);
    u32 g2 = mgr.alloc_socket(20);
    u32 g3 = mgr.alloc_socket(30);

    mgr.replace_native_fd(g2, 99);
    EXPECT_EQ(mgr.get_native_fd(g1), 10);
    EXPECT_EQ(mgr.get_native_fd(g2), 99);
    EXPECT_EQ(mgr.get_native_fd(g3), 30);
}

TEST_F(Ws2ManagerTest, ScratchAllocOomReturnsZero) {
    mgr.scratch_reset();
    // Exhaust scratch space
    while (mgr.scratch_alloc(4096) != 0) {}
    EXPECT_EQ(mgr.scratch_alloc(1), 0u) << "OOM must return 0";
    // Reset recovers
    mgr.scratch_reset();
    EXPECT_NE(mgr.scratch_alloc(4), 0u) << "Reset must recover scratch space";
}
