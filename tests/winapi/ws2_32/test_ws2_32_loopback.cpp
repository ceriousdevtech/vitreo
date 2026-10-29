/// @file test_ws2_32_loopback.cpp
/// @brief End-to-end ws2_32 loopback tests: numeric name resolution, byte-exact
///        TCP downloads, select() readiness and IPv6 sockaddr round trips.
///
/// The shared fixture, the localhost server and the thunk-invocation harness are
/// documented in ws2_loopback_fixture.hpp.

#include "ws2_loopback_fixture.hpp"

// =============================================================================
// Tests
// =============================================================================

TEST_F(Ws2LoopbackTest, GetAddrInfoWResolvesNumericAddressAndFreesResult) {
    constexpr GuestAddr node = DATA_BASE + 0x200;
    constexpr GuestAddr service = DATA_BASE + 0x240;
    constexpr GuestAddr result_out = DATA_BASE + 0x280;

    const std::u16string node_text = u"127.0.0.1";
    for (size_t i = 0; i <= node_text.size(); ++i) {
        ASSERT_TRUE(mem_.write_u16(
            node + static_cast<GuestAddr>(i * sizeof(char16_t)),
            static_cast<u16>(node_text[i])).is_ok());
    }
    const std::u16string service_text = u"80";
    for (size_t i = 0; i <= service_text.size(); ++i) {
        ASSERT_TRUE(mem_.write_u16(
            service + static_cast<GuestAddr>(i * sizeof(char16_t)),
            static_cast<u16>(service_text[i])).is_ok());
    }
    ASSERT_TRUE(mem_.write_u32(result_out, 0).is_ok());

    ASSERT_EQ(inv_->call("GetAddrInfoW", {node, service, 0u, result_out}), 0u);
    const GuestAddr result = mem_.read_u32(result_out).value_or(0);
    ASSERT_EQ(result, WS2_ADDRINFO_BASE);
    EXPECT_EQ(mem_.read_u32(result + 4).value_or(0), 2u);   // AF_INET
    EXPECT_EQ(mem_.read_u32(result + 16).value_or(0), 16u); // SOCKADDR_IN

    const GuestAddr sockaddr = mem_.read_u32(result + 24).value_or(0);
    ASSERT_NE(sockaddr, 0u);
    EXPECT_EQ(mem_.read_u16(sockaddr).value_or(0), 2u);
    EXPECT_EQ(mem_.read_u16(sockaddr + 2).value_or(0), htons(80));
    EXPECT_EQ(mem_.read_u32(sockaddr + 4).value_or(0), htonl(INADDR_LOOPBACK));

    EXPECT_EQ(inv_->call("FreeAddrInfoW", {result}), 0u);
    const GuestAddr reused = mgr_.addrinfo_alloc(64);
    EXPECT_EQ(reused, WS2_ADDRINFO_BASE);
    mgr_.addrinfo_free(reused);
}

TEST_F(Ws2LoopbackTest, SmallHttpExchangeIsByteExact) {
    // 16 KB payload with HTTP-200 framing.
    auto body = make_payload(16384, 0xA1);
    std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n";
    std::vector<uint8_t> wire;
    wire.insert(wire.end(), head.begin(), head.end());
    wire.insert(wire.end(), body.begin(), body.end());

    LoopbackServer srv(wire, /*max_chunk=*/4096);
    ASSERT_TRUE(srv.ok());

    auto got = download_via_thunks(
        srv.port(),
        wire.size(),
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");

    ASSERT_EQ(got.size(), wire.size());
    EXPECT_EQ(0, std::memcmp(got.data(), wire.data(), wire.size()));
}

TEST_F(Ws2LoopbackTest, NlaLookupEnumeratesActiveHostNetworkOnce) {
    bool host_online = false;
    struct ifaddrs* interfaces = nullptr;
    ASSERT_EQ(::getifaddrs(&interfaces), 0);
    for (const struct ifaddrs* current = interfaces;
         current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr) continue;
        if ((current->ifa_flags & IFF_UP) == 0 ||
            (current->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const int family = current->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) {
            host_online = true;
            break;
        }
    }
    ::freeifaddrs(interfaces);
    ASSERT_TRUE(host_online);

    // x86 WSAQUERYSETW has dwNameSpace at offset 20.
    constexpr GuestAddr restrictions = OUT_ADDR + 0x300;
    constexpr GuestAddr lookup = OUT_ADDR + 0x340;
    constexpr GuestAddr length = OUT_ADDR + 0x348;
    constexpr GuestAddr result = OUT_ADDR + 0x400;
    ASSERT_TRUE(mem_.write_u32(restrictions, 60u).is_ok());
    ASSERT_TRUE(mem_.write_u32(restrictions + 20, 15u).is_ok()); // NS_NLA

    ASSERT_EQ(inv_->call("WSALookupServiceBeginW",
                         {restrictions, 0x0ff0u, lookup}),
              0u);
    const u32 handle = mem_.read_u32(lookup).value_or(0);
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(mem_.write_u32(length, 512u).is_ok());
    EXPECT_EQ(inv_->call("WSALookupServiceNextW",
                         {handle, 0x0010u, length, result}),
              0u);
    EXPECT_EQ(mem_.read_u32(result).value_or(0), 60u);
    EXPECT_EQ(mem_.read_u32(result + 20).value_or(0), 15u);
    const GuestAddr name = mem_.read_u32(result + 4).value_or(0);
    EXPECT_GE(name, result + 60u);
    EXPECT_NE(mem_.read_u16(name).value_or(0), 0u);

    EXPECT_EQ(inv_->call("WSALookupServiceNextW",
                         {handle, 0x0010u, length, result}),
              0xffffffffu);
    EXPECT_EQ(inv_->call("WSAGetLastError", {}), 10110u);
    EXPECT_EQ(inv_->call("WSALookupServiceEnd", {handle}), 0u);
}

TEST_F(Ws2LoopbackTest, SelectAcceptsWinsockEffectivelyInfiniteTimeout) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    const uint32_t sock = mgr_.alloc_socket(pair[0]);

    ASSERT_TRUE(mem_.write_u32(FDSET_ADDR, 1u).is_ok());
    ASSERT_TRUE(mem_.write_u32(FDSET_ADDR + 4, sock).is_ok());
    ASSERT_TRUE(mem_.write_u32(TIMEVAL_ADDR, 0x7FFFFFFFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(TIMEVAL_ADDR + 4, 807u).is_ok());

    EXPECT_EQ(inv_->call("select",
                         {0u, 0u, FDSET_ADDR, 0u, TIMEVAL_ADDR}),
              1u);
    EXPECT_EQ(mem_.read_u32(FDSET_ADDR).value_or(0), 1u);
    EXPECT_EQ(mem_.read_u32(FDSET_ADDR + 4).value_or(0), sock);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
    ::close(pair[1]);
}

TEST_F(Ws2LoopbackTest, EightMbDownloadIsByteExactWithTinyChunks) {
    // 8 MB body — large enough to expose recv buffer / select bugs.
    // Tiny server-side max_chunk forces lots of recv() calls.
    auto body = make_payload(8 * 1024 * 1024, 0xBEEF);
    std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n";
    std::vector<uint8_t> wire;
    wire.insert(wire.end(), head.begin(), head.end());
    wire.insert(wire.end(), body.begin(), body.end());

    LoopbackServer srv(std::move(wire), /*max_chunk=*/1031);  // odd prime
    ASSERT_TRUE(srv.ok());

    // Recreate `wire` for verification.
    std::string head2 = "HTTP/1.1 200 OK\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n";
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), head2.begin(), head2.end());
    expected.insert(expected.end(), body.begin(), body.end());

    auto got = download_via_thunks(
        srv.port(),
        expected.size(),
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");

    ASSERT_EQ(got.size(), expected.size())
        << "Expected " << expected.size() << " bytes, got " << got.size();

    if (std::memcmp(got.data(), expected.data(), expected.size()) != 0) {
        // Find the first mismatch and report it.
        size_t i = 0;
        while (i < expected.size() && got[i] == expected[i]) ++i;
        ADD_FAILURE() << "First byte mismatch at offset " << i
                      << ": expected=0x" << std::hex
                      << static_cast<int>(expected[i])
                      << " got=0x" << static_cast<int>(got[i]);
    }
}

TEST_F(Ws2LoopbackTest, ServerEofMidBodyIsReportedAsZeroByteRecv) {
    // Server promises 1 MB but only sends 64 KB then closes — a pattern
    // similar to a CDN that drops the connection mid-stream. The recv()
    // contract requires we eventually return 0 (EOF), not a stale error.
    constexpr size_t kPromised = 1024 * 1024;
    constexpr size_t kActual = 65536;

    auto body = make_payload(kActual, 0x515A);
    std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(kPromised) +
                       "\r\nConnection: close\r\n\r\n";
    std::vector<uint8_t> wire;
    wire.insert(wire.end(), head.begin(), head.end());
    wire.insert(wire.end(), body.begin(), body.end());

    LoopbackServer srv(wire, /*max_chunk=*/4096);
    ASSERT_TRUE(srv.ok());

    auto got = download_via_thunks(
        srv.port(),
        wire.size(),
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");

    // We must have received at least the head + truncated body.
    EXPECT_GE(got.size(), head.size() + kActual);
    // And the bytes we DID receive must be byte-exact.
    EXPECT_EQ(0, std::memcmp(got.data(), wire.data(), got.size()));
}

TEST_F(Ws2LoopbackTest, ManySmallSelectPollsDoNotLoseData) {
    // Steam hammers select with timeout=0 between every recv. Replicate that
    // shape: many zero-timeout selects per chunk. This catches the
    // "zero-poll cache short-circuit returns idle when data is actually
    // arriving" bug class.
    auto body = make_payload(512 * 1024, 0xDEADC0DE);
    std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n";
    std::vector<uint8_t> wire;
    wire.insert(wire.end(), head.begin(), head.end());
    wire.insert(wire.end(), body.begin(), body.end());

    LoopbackServer srv(wire, /*max_chunk=*/2048);
    ASSERT_TRUE(srv.ok());

    auto got = download_via_thunks(
        srv.port(),
        wire.size(),
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");

    ASSERT_EQ(got.size(), wire.size());
    EXPECT_EQ(0, std::memcmp(got.data(), wire.data(), wire.size()));
}

TEST_F(Ws2LoopbackTest, ZeroTimeoutSelectObservesReadinessOnEveryCall) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    sockaddr_in receiver{};
    receiver.sin_family = AF_INET;
    receiver.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    receiver.sin_port = 0;
    const int receiver_fd = mgr_.get_native_fd(sock);
    ASSERT_GE(receiver_fd, 0);
    ASSERT_EQ(::bind(receiver_fd, reinterpret_cast<sockaddr*>(&receiver),
                     sizeof(receiver)),
              0) << std::strerror(errno);
    socklen_t receiver_len = sizeof(receiver);
    ASSERT_EQ(::getsockname(receiver_fd,
                            reinterpret_cast<sockaddr*>(&receiver),
                            &receiver_len),
              0);

    auto write_read_set = [&] {
        ASSERT_TRUE(mem_.write_u32(FDSET_ADDR, 1u).is_ok());
        ASSERT_TRUE(mem_.write_u32(FDSET_ADDR + 4, sock).is_ok());
        ASSERT_TRUE(mem_.write_u32(TIMEVAL_ADDR, 0u).is_ok());
        ASSERT_TRUE(mem_.write_u32(TIMEVAL_ADDR + 4, 0u).is_ok());
    };

    // Establish an idle result for this exact fd_set.
    write_read_set();
    EXPECT_EQ(inv_->call("select", {0u, FDSET_ADDR, 0u, 0u, TIMEVAL_ADDR}),
              0u);

    // Queue a datagram without passing through a guest socket API. A correct
    // zero-timeout select must consult current provider state on this very
    // next call; a cached prior result is not a Winsock readiness result.
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sender, 0);
    constexpr char payload[] = "ready";
    ASSERT_EQ(::sendto(sender, payload, sizeof(payload) - 1, 0,
                       reinterpret_cast<sockaddr*>(&receiver), sizeof(receiver)),
              static_cast<ssize_t>(sizeof(payload) - 1));

    pollfd ready_probe{receiver_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&ready_probe, 1, 1000), 1);
    ASSERT_NE(ready_probe.revents & POLLIN, 0);

    write_read_set();
    EXPECT_EQ(inv_->call("select", {0u, FDSET_ADDR, 0u, 0u, TIMEVAL_ADDR}),
              1u);
    EXPECT_EQ(mem_.read_u32(FDSET_ADDR).value_or(0), 1u);
    EXPECT_EQ(mem_.read_u32(FDSET_ADDR + 4).value_or(0), sock);

    ::close(sender);
    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

// Windows AF_INET6 is 23 and SOCKADDR_IN6 is 28 bytes (Learn: SOCKADDR_IN6_LH).
// Parsing that buffer as sockaddr_in used to treat flowinfo as sin_addr →
// connect(0.0.0.0) with family still 23 (macOS EINVAL).
TEST_F(Ws2LoopbackTest, Ipv6SockaddrIn6BindGetsocknameRoundTrip) {
    constexpr u32 kWinAfInet6 = 23;
    constexpr u32 kSockaddrIn6Bytes = 28;

    // socket(AF_INET6=23, SOCK_DGRAM=2, IPPROTO_UDP=17)
    uint32_t sock = inv_->call("socket", {kWinAfInet6, 2u, 17u});
    ASSERT_NE(sock, 0xFFFFFFFFu);

    // Windows sockaddr_in6 for [::1]:0 (any port via bind of port 0).
    // Layout: family@0, port@2, flowinfo@4, addr@8 (16), scope@24.
    uint8_t sa[kSockaddrIn6Bytes]{};
    sa[0] = static_cast<uint8_t>(kWinAfInet6 & 0xff);
    sa[1] = static_cast<uint8_t>((kWinAfInet6 >> 8) & 0xff);
    // ::1 in network order at offset 8
    sa[8 + 15] = 1;

    ASSERT_TRUE(mem_.write_bytes(ADDR_ADDR, sa, kSockaddrIn6Bytes).is_ok());
    uint32_t rc = inv_->call("bind", {sock, ADDR_ADDR, kSockaddrIn6Bytes});
    ASSERT_EQ(rc, 0u) << "bind(IPv6) failed";

    // namelen in/out
    ASSERT_TRUE(mem_.write_u32(OUT_ADDR, kSockaddrIn6Bytes).is_ok());
    // Clear destination buffer so we detect writes.
    uint8_t zeros[kSockaddrIn6Bytes]{};
    ASSERT_TRUE(mem_.write_bytes(ADDR_ADDR, zeros, kSockaddrIn6Bytes).is_ok());

    rc = inv_->call("getsockname", {sock, ADDR_ADDR, OUT_ADDR});
    ASSERT_EQ(rc, 0u);

    auto namelen = mem_.read_u32(OUT_ADDR);
    ASSERT_TRUE(namelen.is_ok());
    EXPECT_EQ(namelen.value(), kSockaddrIn6Bytes);

    uint8_t out[kSockaddrIn6Bytes]{};
    ASSERT_TRUE(mem_.read_bytes(ADDR_ADDR, out, kSockaddrIn6Bytes).is_ok());
    EXPECT_EQ(out[0], static_cast<uint8_t>(kWinAfInet6 & 0xff));
    EXPECT_EQ(out[1], 0u);
    // Bound loopback should still be ::1 (or unspecified if OS rewrote —
    // at least family/size must be Windows-shaped, never host AF=30).
    EXPECT_NE(out[0], 30u);

    // connect() with full Windows sockaddr_in6 to ::1:9 must not report
    // WSAEINVAL from a mis-parsed family-23-as-IPv4 sockaddr.
    uint8_t dest[kSockaddrIn6Bytes]{};
    dest[0] = static_cast<uint8_t>(kWinAfInet6 & 0xff);
    dest[2] = 0; // port 9 network order
    dest[3] = 9;
    dest[8 + 15] = 1;
    ASSERT_TRUE(mem_.write_bytes(ADDR_ADDR, dest, kSockaddrIn6Bytes).is_ok());
    rc = inv_->call("connect", {sock, ADDR_ADDR, kSockaddrIn6Bytes});
    // UDP connect to ::1:9 typically succeeds (sets default peer).
    EXPECT_EQ(rc, 0u) << "connect(IPv6) should succeed after correct marshal";

    inv_->call("closesocket", {sock});
}
