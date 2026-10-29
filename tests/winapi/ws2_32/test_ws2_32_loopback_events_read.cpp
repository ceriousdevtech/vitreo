/// @file test_ws2_32_loopback_events_read.cpp
/// @brief ws2_32 loopback tests for FD_READ / FD_CLOSE re-enabling on
///        established sockets under WSAEventSelect.
///
/// The shared fixture, the localhost server and the thunk-invocation harness are
/// documented in ws2_loopback_fixture.hpp.

#include "ws2_loopback_fixture.hpp"

TEST_F(Ws2LoopbackTest, WsaEnumFdReadRequiresReadableBytes) {
    // POLLIN with FIONREAD==0 must not report FD_READ. Chromium's
    // TCPSocketWin::DidSignalRead treats any FD_READ as "ready" and
    // RetryRead()s; a false edge after ClientHello can complete the read
    // path with EOF/error and never re-arm WatchForRead.
    std::atomic<bool> stop{false};
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen), 0);
    const uint16_t port = ntohs(addr.sin_port);
    ASSERT_EQ(::listen(listen_fd, 1), 0);

    std::atomic<int> accepted{-1};
    std::thread server([&] {
        int cfd = ::accept(listen_fd, nullptr, nullptr);
        accepted.store(cfd, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (cfd >= 0) ::close(cfd);
    });

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    const Handle event =
        sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);

    constexpr u32 kFdRead = 0x01;
    constexpr u32 kFdClose = 0x20;
    ASSERT_EQ(inv_->call("WSAEventSelect", {sock, event, kFdRead | kFdClose}),
              0u);
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(port)).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);
    // WSAEventSelect makes the socket nonblocking, so connect reports
    // WSAEWOULDBLOCK and completes in the background (ws2_core_probe
    // connect_nb_ok: rc=-1 err=10035).
    EXPECT_EQ(inv_->call("connect", {sock, ADDR_ADDR, 16u}), 0xFFFFFFFFu);
    EXPECT_EQ(mgr_.last_error(), 10035u);

    for (int i = 0; i < 50 && accepted.load(std::memory_order_acquire) < 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(accepted.load(std::memory_order_acquire), 0);

    (void)sync_->reset_event(event);
    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    ASSERT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}), 0u);
    EXPECT_EQ(mem_.read_u32(OUT_ADDR).value_or(0xFFu) & kFdRead, 0u)
        << "connected idle socket must not report FD_READ";

    const char payload[] = "SRV";
    ASSERT_EQ(
        ::send(accepted.load(), payload, 3, 0),
        3);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (u32 i = 0; i < 44; i += 4) {
        ASSERT_TRUE(mem_.write_u32(OUT_ADDR + i, 0).is_ok());
    }
    ASSERT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}), 0u);
    EXPECT_EQ(mem_.read_u32(OUT_ADDR).value_or(0) & kFdRead, kFdRead)
        << "FD_READ only after payload is available";

    stop.store(true, std::memory_order_release);
    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
    ::close(listen_fd);
    server.join();
}

// Learn WSAEventSelect: recv (not WSAEnumNetworkEvents) reenables FD_READ.
// A loopback HTTP server that RegisterWaits on the event and reads with recv
// must see the next request. MSG_PEEK does not reenable; this path does not
// peek.
TEST_F(Ws2LoopbackTest, RecvReenablesFdReadWithoutEnum) {
    std::atomic<int> stage{0};
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen), 0);
    const uint16_t port = ntohs(addr.sin_port);
    ASSERT_EQ(::listen(listen_fd, 1), 0);

    std::thread server([&] {
        int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        ASSERT_EQ(::send(cfd, "A", 1, 0), 1);
        while (stage.load(std::memory_order_acquire) < 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_EQ(::send(cfd, "B", 1, 0), 1);
        while (stage.load(std::memory_order_acquire) < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::close(cfd);
    });

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    const Handle event =
        sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);
    constexpr u32 kFdRead = 0x01;
    ASSERT_EQ(inv_->call("WSAEventSelect", {sock, event, kFdRead}), 0u);
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(port)).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);
    // WSAEventSelect makes the socket non-blocking, so connect may return
    // SOCKET_ERROR / WSAEWOULDBLOCK while the loopback handshake finishes.
    (void)inv_->call("connect", {sock, ADDR_ADDR, 16u});

    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    ASSERT_TRUE(sync_->reset_event(event));
    constexpr GuestAddr BUF = OUT_ADDR + 0x800;
    ASSERT_EQ(inv_->call("recv", {sock, BUF, 1u, 0u}), 1u);
    EXPECT_EQ(sync_->wait_for_single_object(event, 50), WAIT_TIMEOUT)
        << "drained socket must not stay readable";

    stage.store(1, std::memory_order_release);
    EXPECT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0)
        << "recv must reenable FD_READ without WSAEnumNetworkEvents";
    ASSERT_EQ(inv_->call("recv", {sock, BUF, 1u, 0u}), 1u);

    stage.store(2, std::memory_order_release);
    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
    ::close(listen_fd);
    server.join();
}

// Learn WSAEventSelect: WSARecv is an FD_READ reenabling function, and
// FD_CLOSE is recorded once (no reenabling function) when the peer shuts down
// its send side, without an FD_READ for the closure.
TEST_F(Ws2LoopbackTest, WsaRecvReenablesFdReadAndGracefulCloseRecordsFdCloseOnce) {
    std::atomic<int> stage{0};
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen), 0);
    const uint16_t port = ntohs(addr.sin_port);
    ASSERT_EQ(::listen(listen_fd, 1), 0);

    std::thread server([&] {
        int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        (void)::send(cfd, "A", 1, 0);
        while (stage.load(std::memory_order_acquire) < 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)::send(cfd, "B", 1, 0);
        while (stage.load(std::memory_order_acquire) < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::shutdown(cfd, SHUT_WR);
        while (stage.load(std::memory_order_acquire) < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::close(cfd);
    });

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    const Handle event =
        sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(event, 0u);
    constexpr u32 kFdRead = 0x01;
    constexpr u32 kFdClose = 0x20;
    constexpr u32 kFdCloseBit = 5;
    ASSERT_EQ(inv_->call("WSAEventSelect", {sock, event, kFdRead | kFdClose}),
              0u);
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(port)).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);
    (void)inv_->call("connect", {sock, ADDR_ADDR, 16u});

    const auto enum_events = [&]() -> u32 {
        for (u32 i = 0; i < 44; i += 4) {
            (void)mem_.write_u32(OUT_ADDR + i, 0xFFFFFFFFu);
        }
        EXPECT_EQ(inv_->call("WSAEnumNetworkEvents", {sock, event, OUT_ADDR}),
                  0u);
        return mem_.read_u32(OUT_ADDR).value_or(0xFFFFFFFFu);
    };
    constexpr GuestAddr kBuf = DATA_BASE + 0x5000;
    constexpr GuestAddr kWsaBuf = DATA_BASE + 0x5100;
    constexpr GuestAddr kBytes = DATA_BASE + 0x5300;
    constexpr GuestAddr kFlags = DATA_BASE + 0x5304;
    const auto wsa_recv_one = [&]() -> u32 {
        (void)mem_.write_u32(kWsaBuf + 0, 1);
        (void)mem_.write_u32(kWsaBuf + 4, static_cast<u32>(kBuf));
        (void)mem_.write_u32(kBytes, 0);
        (void)mem_.write_u32(kFlags, 0);
        EXPECT_EQ(inv_->call("WSARecv", {sock, kWsaBuf, 1u, kBytes, kFlags, 0u, 0u}),
                  0u);
        return mem_.read_u32(kBytes).value_or(0);
    };

    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    EXPECT_EQ(enum_events() & kFdRead, kFdRead);
    EXPECT_EQ(wsa_recv_one(), 1u);
    EXPECT_EQ(sync_->wait_for_single_object(event, 50), WAIT_TIMEOUT);

    stage.store(1, std::memory_order_release);
    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0)
        << "WSARecv must reenable FD_READ";
    EXPECT_EQ(enum_events() & kFdRead, kFdRead);
    EXPECT_EQ(wsa_recv_one(), 1u);

    stage.store(2, std::memory_order_release);
    ASSERT_EQ(sync_->wait_for_single_object(event, 2000), WAIT_OBJECT_0)
        << "graceful close must record FD_CLOSE";
    const u32 closing = enum_events();
    EXPECT_EQ(closing & (kFdRead | kFdClose), kFdClose);
    EXPECT_EQ(mem_.read_u32(OUT_ADDR + 4 + kFdCloseBit * 4).value_or(1), 0u)
        << "graceful close reports error 0";
    EXPECT_EQ(enum_events() & kFdClose, 0u) << "FD_CLOSE is recorded once";
    EXPECT_EQ(sync_->wait_for_single_object(event, 50), WAIT_TIMEOUT);

    stage.store(3, std::memory_order_release);
    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(event));
    ::close(listen_fd);
    server.join();
}

// Learn (WSASendMsg / WSARecvMsg): both are provider extensions with no
// ws2_32 export, reachable only through
// WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER). A UDP sender that uses
// WSASendMsg — Valve's SteamNetworkingSockets does — gets no function pointer
// if the ioctl does not know the GUID, and then never transmits at all: its
// sockets bind and stay silent, which surfaces far away as an unreachable
// relay network rather than as a socket error.
// Learn (WSASendMsg): the send-side twin of WSARecvMsg. It is the only UDP
// send path some senders use, so returning WSAEOPNOTSUPP makes them transmit
// nothing while every other socket call still succeeds — the datagrams simply
// never leave, which looks like an unreachable network rather than an error.
// SteamNetworkingSockets' service thread (GameNetworkingSockets,
// steamnetworkingsockets_socketthread.cpp) associates *every* UDP socket with a
// single **auto-reset** event — CreateEvent(nullptr, false, false, nullptr) —
// via WSAEventSelect(FD_READ), waits with WaitForSingleObject (which consumes
// the signal), and then calls WSAEnumNetworkEvents(sock, NULL, &events) with a
// NULL handle.
//
// Learn (WSAEnumNetworkEvents): the call reports and *clears the socket's
// internal network event record*; resetting hEventObject is an additional
// optional service. So a provider that suppresses re-signalling until the
// event is reset must lift that suppression on the enumerate too, or the
// second datagram never wakes the waiter.
TEST_F(Ws2LoopbackTest, EnumNetworkEventsWithNullHandleReArmsTheSharedEvent) {
    constexpr GuestAddr ADDR_IN   = OUT_ADDR + 0x600;
    constexpr GuestAddr NETEVENTS = OUT_ADDR + 0x640;
    constexpr GuestAddr BOUND     = OUT_ADDR + 0x6C0;
    constexpr GuestAddr BOUND_LEN = OUT_ADDR + 0x700;
    constexpr GuestAddr RECV_BUF  = OUT_ADDR + 0x740;
    constexpr u32 kFdRead = 0x01u;

    const u64 receiver = inv_->call("socket", {2u, 2u, 17u});
    const u64 sender = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_NE(receiver, static_cast<u64>(~0u));
    ASSERT_NE(sender, static_cast<u64>(~0u));

    ASSERT_TRUE(mem_.write_u16(ADDR_IN, 2u).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_IN + 2, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_IN + 4, 0x0100007Fu).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_IN + 8, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_IN + 12, 0u).is_ok());
    ASSERT_EQ(inv_->call("bind", {receiver, ADDR_IN, 16u}), 0u);
    ASSERT_TRUE(mem_.write_u32(BOUND_LEN, 16u).is_ok());
    ASSERT_EQ(inv_->call("getsockname", {receiver, BOUND, BOUND_LEN}), 0u);
    const u16 port = mem_.read_u16(BOUND + 2).value_or(0);
    ASSERT_NE(port, 0u);
    ASSERT_TRUE(mem_.write_u16(ADDR_IN + 2, port).is_ok());

    // Exactly the event the service thread creates:
    // CreateEvent(nullptr, /*manual reset*/ false, /*initial*/ false, nullptr).
    // WSACreateEvent would give a *manual-reset* event (Learn), which stays
    // signalled once set and therefore cannot show whether the provider
    // re-signals — the distinction this test exists to make.
    const u64 event = static_cast<u64>(
        sync_->create_event(/*manual_reset=*/false, /*initial_state=*/false,
                            ""));
    ASSERT_NE(event, 0u);
    ASSERT_EQ(inv_->call("WSAEventSelect", {receiver, event, kFdRead}), 0u);

    const auto deliver_one = [&](const char* body, usize len) {
        ASSERT_TRUE(mem_.write_bytes(
            RECV_BUF + 0x100, reinterpret_cast<const Byte*>(body), len).is_ok());
        ASSERT_EQ(inv_->call("sendto",
                             {sender, RECV_BUF + 0x100, static_cast<u64>(len),
                              0u, ADDR_IN, 16u}),
                  static_cast<u64>(len));
    };

    // The observable that matters is the *event waking a waiter*, not what the
    // enumerate reports: a socket that still has data will report FD_READ from
    // a fresh readiness poll regardless. Valve's service thread blocks on the
    // auto-reset event with kernel32 WaitForSingleObject, so wait through the
    // SyncManager directly — going through WSAWaitForMultipleEvents would take
    // a different path that lifts the latch on its own and hide the defect.
    const auto event_wakes_within = [&](u32 timeout_ms) {
        return sync_->wait_for_single_object(
                   static_cast<Handle>(event), timeout_ms) == 0u;
    };
    const auto consume_like_the_service_thread = [&]() {
        EXPECT_EQ(inv_->call("WSAEnumNetworkEvents",
                             {receiver, 0u, NETEVENTS}), 0u);
    };

    // First datagram: consumed the way the service thread does, with a NULL
    // event handle.
    deliver_one("one", 3);
    ASSERT_TRUE(event_wakes_within(2000))
        << "first datagram must wake the shared event";
    consume_like_the_service_thread();
    ASSERT_EQ(inv_->call("recvfrom", {receiver, RECV_BUF, 64u, 0u, 0u, 0u}), 3u);

    // Second datagram: this is the regression. With the latch only lifted by a
    // non-NULL handle, FD_READ is never reported again and a real service
    // thread stops receiving entirely after its first packet.
    deliver_one("two", 3);
    EXPECT_TRUE(event_wakes_within(2000))
        << "the shared event must wake again after a NULL-handle enumerate; "
           "otherwise a service thread stops receiving after its first packet";
    consume_like_the_service_thread();
    EXPECT_EQ(inv_->call("recvfrom", {receiver, RECV_BUF, 64u, 0u, 0u, 0u}), 3u);

    (void)inv_->call("closesocket", {sender});
    (void)inv_->call("closesocket", {receiver});
}
