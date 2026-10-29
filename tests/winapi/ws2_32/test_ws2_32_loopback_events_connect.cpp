/// @file test_ws2_32_loopback_events_connect.cpp
/// @brief ws2_32 loopback tests for WSAEventSelect / WSAEnumNetworkEvents
///        connect-phase semantics (FD_CONNECT, FD_WRITE, refused connects).
///
/// The shared fixture, the localhost server and the thunk-invocation harness are
/// documented in ws2_loopback_fixture.hpp.

#include "ws2_loopback_fixture.hpp"

// Chromium TCPSocketWin::DoConnect: WSAEventSelect(FD_CONNECT) then connect().
// MSDN: nonblocking connect returns SOCKET_ERROR + WSAEWOULDBLOCK, never 0.
// Vitreo's Steam poll-wait must not convert EINPROGRESS→0 when FD_CONNECT is
// registered — that trips Chromium's NOTREACHED and kills NetworkService.
// Learn WSAEventSelect: the provider signals the event when connect completes;
// Chromium ObjectWatcher uses RegisterWaitForSingleObject (not
// WSAWaitForMultipleEvents), so SetEvent must happen asynchronously.
TEST_F(Ws2LoopbackTest, WsaEventSelectConnectReturnsWouldBlock) {
    LoopbackServer srv(std::vector<uint8_t>{'x'}, /*max_chunk=*/1);
    ASSERT_TRUE(srv.ok());

    uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    const Handle event = sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);

    constexpr u32 kFdConnect = 0x10;
    uint32_t es = inv_->call("WSAEventSelect", {sock, event, kFdConnect});
    ASSERT_EQ(es, 0u);

    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(srv.port())).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);

    uint32_t cr = inv_->call("connect", {sock, ADDR_ADDR, 16u});
    EXPECT_EQ(cr, 0xFFFFFFFFu)
        << "nonblocking connect after WSAEventSelect must not return 0";
    EXPECT_EQ(mgr_.last_error(), 10035u) // WSAEWOULDBLOCK
        << "expected WSAEWOULDBLOCK, not synchronous success";

    const u32 wait_rc = sync_->wait_for_single_object(event, 2000);
    EXPECT_EQ(wait_rc, WAIT_OBJECT_0)
        << "FD_CONNECT must SetEvent asynchronously for ObjectWatcher/RegisterWait";

    // Guest buffer for WSANETWORKEVENTS (44 bytes) at OUT_ADDR.
    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    uint32_t enum_rc =
        inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR});
    EXPECT_EQ(enum_rc, 0u);
    auto reported = mem_.read_u32(OUT_ADDR);
    ASSERT_TRUE(reported.is_ok());
    EXPECT_EQ(reported.value() & kFdConnect, kFdConnect)
        << "WSAEnumNetworkEvents must report FD_CONNECT after completion";

    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
}

TEST_F(Ws2LoopbackTest, WsaEnumReportsFdWriteWithConnect) {
    // Windows records FD_WRITE when a socket first connects. Chromium's
    // NetworkService ObjectWatcher waits for that bit before send().
    LoopbackServer srv(std::vector<uint8_t>{'x'}, /*max_chunk=*/1);
    ASSERT_GT(srv.port(), 0);

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    const Handle event = sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);

    constexpr u32 kFdConnect = 0x10;
    constexpr u32 kFdWrite = 0x02;
    uint32_t es =
        inv_->call("WSAEventSelect", {sock, event, kFdConnect | kFdWrite});
    ASSERT_EQ(es, 0u);

    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(srv.port())).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);

    uint32_t cr = inv_->call("connect", {sock, ADDR_ADDR, 16u});
    EXPECT_EQ(cr, 0xFFFFFFFFu);
    EXPECT_EQ(mgr_.last_error(), 10035u);

    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0);

    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    ASSERT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}), 0u);
    auto reported = mem_.read_u32(OUT_ADDR);
    ASSERT_TRUE(reported.is_ok());
    EXPECT_EQ(reported.value() & kFdConnect, kFdConnect);
    EXPECT_EQ(reported.value() & kFdWrite, kFdWrite)
        << "FD_WRITE must be recorded with FD_CONNECT on first connect";

    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
}

TEST_F(Ws2LoopbackTest, WsaEventSelectFdWriteAfterConnect) {
    // Chromium: WSAEventSelect(FD_CONNECT) → connect → enum → then
    // WSAEventSelect(FD_READ|FD_WRITE|FD_CLOSE). FD_WRITE must fire then.
    LoopbackServer srv(std::vector<uint8_t>{'x'}, /*max_chunk=*/1);
    ASSERT_GT(srv.port(), 0);

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    const Handle event = sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);

    constexpr u32 kFdConnect = 0x10;
    constexpr u32 kFdWrite = 0x02;
    constexpr u32 kFdRead = 0x01;
    constexpr u32 kFdClose = 0x20;
    ASSERT_EQ(inv_->call("WSAEventSelect", {sock, event, kFdConnect}), 0u);

    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(srv.port())).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);

    ASSERT_EQ(inv_->call("connect", {sock, ADDR_ADDR, 16u}), 0xFFFFFFFFu);
    ASSERT_EQ(mgr_.last_error(), 10035u);
    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    ASSERT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}), 0u);

    ASSERT_EQ(sync_->reset_event(event), true);
    ASSERT_EQ(
        inv_->call("WSAEventSelect",
                   {sock, event, kFdRead | kFdWrite | kFdClose}),
        0u);
    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0)
        << "FD_WRITE must SetEvent after post-connect WSAEventSelect";
    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    ASSERT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}), 0u);
    auto reported = mem_.read_u32(OUT_ADDR);
    ASSERT_TRUE(reported.is_ok());
    EXPECT_EQ(reported.value() & kFdWrite, kFdWrite);

    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
}

// Learn WSAEventSelect / WSAEnumNetworkEvents: a refused nonblocking connect
// records FD_CONNECT with iErrorCode[FD_CONNECT_BIT] = WSAECONNREFUSED, and no
// FD_WRITE (never connected) or FD_CLOSE (no circuit). The wait's provider
// refresh runs before WSAEnumNetworkEvents and must not consume the error;
// Chromium falls back from ::1 to 127.0.0.1 only on that error.
TEST_F(Ws2LoopbackTest, RefusedConnectReportsErrorAfterEventRefresh) {
    int probe_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(probe_fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(probe_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(::getsockname(probe_fd, reinterpret_cast<sockaddr*>(&addr), &alen), 0);
    const uint16_t closed_port = ntohs(addr.sin_port);
    ::close(probe_fd);

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    const Handle event =
        sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);
    constexpr u32 kFdWrite = 0x02;
    constexpr u32 kFdConnect = 0x10;
    constexpr u32 kFdClose = 0x20;
    constexpr u32 kFdConnectBit = 4;
    constexpr u32 kWsaEConnRefused = 10061;
    ASSERT_EQ(inv_->call("WSAEventSelect",
                         {sock, event, kFdWrite | kFdConnect | kFdClose}),
              0u);
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(closed_port)).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);
    (void)inv_->call("connect", {sock, ADDR_ADDR, 16u});

    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    ASSERT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}), 0u);
    const u32 events = mem_.read_u32(OUT_ADDR).value_or(0);
    EXPECT_EQ(events & (kFdWrite | kFdConnect | kFdClose), kFdConnect);
    EXPECT_EQ(mem_.read_u32(OUT_ADDR + 4 + kFdConnectBit * 4).value_or(0),
              kWsaEConnRefused);

    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
}
