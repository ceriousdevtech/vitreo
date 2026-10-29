/// @file test_ws2_32_loopback_overlapped.cpp
/// @brief ws2_32 loopback tests for overlapped socket I/O: ConnectEx,
///        WSASend/WSARecv completion and the overlapped completion callback.
///
/// The shared fixture, the localhost server and the thunk-invocation harness are
/// documented in ws2_loopback_fixture.hpp.

#include "ws2_loopback_fixture.hpp"

TEST_F(Ws2LoopbackTest, ConnectExCompletesThroughOverlappedCallback) {
    constexpr u32 kWsaIoPending = 997;
    constexpr u32 kStatusPending = 0x00000103u;
    constexpr u32 kStatusSuccess = 0;
    LoopbackServer server({}, 4096);
    ASSERT_TRUE(server.ok());

    const uint32_t socket = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(socket, 0x10000u);

    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(server.port())).is_ok());
    ASSERT_TRUE(
        mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) {
        ASSERT_TRUE(mem_.write_u8(ADDR_ADDR + i, 0).is_ok());
    }

    constexpr GuestAddr bytes_sent = OUT_ADDR + 0x100;
    constexpr GuestAddr overlapped = OUT_ADDR + 0x120;
    ASSERT_TRUE(mem_.write_u32(bytes_sent, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(overlapped + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(overlapped + 16, 0).is_ok());

    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    bool completed = false;
    Handle completed_handle = 0;
    GuestAddr completed_overlapped = 0;
    u32 completed_bytes = 0xFFFFFFFFu;
    u32 completed_status = 0xFFFFFFFFu;
    u32 completed_error = 0xFFFFFFFFu;
    bool completed_synchronously = false;
    mgr_.set_overlapped_completion_callback(
        [&](Handle handle, u32 bytes, GuestAddr ov, bool is_64bit,
            u32 nt_status, u32 win_error, bool synchronous) {
            std::lock_guard<std::mutex> lock(completion_mutex);
            completed = true;
            completed_handle = handle;
            completed_overlapped = ov;
            completed_bytes = bytes;
            completed_status = nt_status;
            completed_error = win_error;
            completed_synchronously = synchronous;
            EXPECT_FALSE(is_64bit);
            completion_cv.notify_one();
        });

    const uint32_t result = inv_->call(
        "ConnectEx",
        {socket, ADDR_ADDR, 16u, 0u, 0u, bytes_sent, overlapped});
    if (result == 0) {
        EXPECT_EQ(mgr_.last_error(), kWsaIoPending);
        EXPECT_EQ(mem_.read_u32(overlapped).value_or(0), kStatusPending);
    } else {
        EXPECT_EQ(result, 1u);
    }

    {
        std::unique_lock<std::mutex> lock(completion_mutex);
        ASSERT_TRUE(completion_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return completed; }));
    }
    EXPECT_EQ(completed_handle, socket);
    EXPECT_EQ(completed_overlapped, overlapped);
    EXPECT_EQ(completed_bytes, 0u);
    EXPECT_EQ(completed_status, kStatusSuccess);
    EXPECT_EQ(completed_error, 0u);
    EXPECT_EQ(completed_synchronously, result != 0);
    EXPECT_EQ(mem_.read_u32(bytes_sent).value_or(0xFFFFFFFFu), 0u);

    mgr_.clear_overlapped_completion_callback();
    EXPECT_EQ(inv_->call("closesocket", {socket}), 0u);
}

TEST_F(Ws2LoopbackTest, ConnectExRefusedPreservesNativeStatus) {
    constexpr u32 kWsaIoPending = 997;
    constexpr u32 kStatusPending = 0x00000103u;
    constexpr u32 kStatusRefused = 0xC0000236u;
    // Obtain an unused ephemeral port, then close it so the kernel refuses
    // connections (a bound but non-listening macOS socket can drop SYNs).
    const int reserved = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(reserved, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(reserved, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(reserved, reinterpret_cast<sockaddr*>(&address), &length), 0);
    ::close(reserved);

    const uint32_t socket = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(socket, 0x10000u);

    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, address.sin_port).is_ok());
    ASSERT_TRUE(
        mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) {
        ASSERT_TRUE(mem_.write_u8(ADDR_ADDR + i, 0).is_ok());
    }

    constexpr GuestAddr bytes_sent = OUT_ADDR + 0x100;
    constexpr GuestAddr overlapped = OUT_ADDR + 0x120;
    ASSERT_TRUE(mem_.write_u32(bytes_sent, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(overlapped + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(overlapped + 16, 0).is_ok());

    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    bool completed = false;
    Handle completed_handle = 0;
    GuestAddr completed_overlapped = 0;
    u32 completed_bytes = 0xFFFFFFFFu;
    u32 completed_status = 0xFFFFFFFFu;
    u32 completed_error = 0xFFFFFFFFu;
    bool completed_synchronously = false;
    mgr_.set_overlapped_completion_callback(
        [&](Handle handle, u32 bytes, GuestAddr ov, bool is_64bit,
            u32 nt_status, u32 win_error, bool synchronous) {
            std::lock_guard<std::mutex> lock(completion_mutex);
            completed = true;
            completed_handle = handle;
            completed_overlapped = ov;
            completed_bytes = bytes;
            completed_status = nt_status;
            completed_error = win_error;
            completed_synchronously = synchronous;
            EXPECT_FALSE(is_64bit);
            completion_cv.notify_one();
        });

    const uint32_t result = inv_->call(
        "ConnectEx",
        {socket, ADDR_ADDR, 16u, 0u, 0u, bytes_sent, overlapped});
    if (result == 0) {
        EXPECT_EQ(mgr_.last_error(), kWsaIoPending);
        EXPECT_EQ(mem_.read_u32(overlapped).value_or(0), kStatusPending);
    } else {
        EXPECT_EQ(result, 1u);
    }

    {
        std::unique_lock<std::mutex> lock(completion_mutex);
        ASSERT_TRUE(completion_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return completed; }));
    }
    EXPECT_EQ(completed_handle, socket);
    EXPECT_EQ(completed_overlapped, overlapped);
    EXPECT_EQ(completed_bytes, 0u);
    EXPECT_EQ(completed_status, kStatusRefused);
    EXPECT_EQ(completed_error, 10061u);
    EXPECT_EQ(completed_synchronously, result != 0);
    EXPECT_EQ(mem_.read_u32(bytes_sent).value_or(0xFFFFFFFFu), 0u);

    mgr_.clear_overlapped_completion_callback();
    EXPECT_EQ(inv_->call("closesocket", {socket}), 0u);
}

TEST_F(Ws2LoopbackTest, UpdateConnectContextSucceedsAfterConnectEx) {
    // Windows requires SO_UPDATE_CONNECT_CONTEXT (0x7010) after an overlapped
    // ConnectEx completes, and SO_UPDATE_ACCEPT_CONTEXT (0x700B) after
    // AcceptEx. libuv's uv__process_tcp_connect_req fails the whole connection
    // with WSAGetLastError() when the call fails, so answering WSAENOPROTOOPT
    // closes every socket immediately after a successful connect.
    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    // On a socket that never connected Windows refuses CONNECT_CONTEXT with
    // WSAENOTCONN (ws2_opts_probe.x86 set_SO_UPDATE_CONNECT_CONTEXT_unconn).
    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    ASSERT_TRUE(mem_.write_u32(option_value, 0u).is_ok());
    EXPECT_EQ(inv_->call("setsockopt", {sock, 0xFFFFu, 0x7010u, option_value, 4u}),
              0xFFFFFFFFu);
    EXPECT_EQ(mgr_.last_error(), 10057u);

    ASSERT_TRUE(mem_.write_u32(option_value, sock).is_ok());
    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x700Bu, option_value, 4u}),
              0u);

    // An option that really is unsupported must still fail honestly.
    EXPECT_NE(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x7011u, option_value, 4u}),
              0u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, WsaSendOverlappedSignalsEventOnSyncComplete) {
    // Chromium TCPSocketWin::Write: WSASend(..., &overlapped) returning 0 is
    // only treated as complete when OVERLAPPED.hEvent is signaled
    // (ResetEventIfSignaled). Missing SetEvent leaves Write in ERR_IO_PENDING
    // forever — NetworkService never sends the TLS ClientHello.
    LoopbackServer srv(std::vector<uint8_t>{}, /*max_chunk=*/1);
    ASSERT_GT(srv.port(), 0);

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(srv.port())).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);
    ASSERT_EQ(inv_->call("connect", {sock, ADDR_ADDR, 16u}), 0u);

    const Handle hevent =
        sync_->create_event(/*manual=*/true, /*initial=*/false, "");
    ASSERT_NE(hevent, 0u);

    constexpr GuestAddr kBuf = DATA_BASE + 0x6000;
    constexpr GuestAddr kWsaBuf = DATA_BASE + 0x6100;
    constexpr GuestAddr kOv = DATA_BASE + 0x6200;
    constexpr GuestAddr kBytes = DATA_BASE + 0x6300;
    const char payload[] = "HELLO";
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(mem_.write_u8(kBuf + i, static_cast<u8>(payload[i])).is_ok());
    }
    ASSERT_TRUE(mem_.write_u32(kWsaBuf + 0, 5).is_ok());
    ASSERT_TRUE(mem_.write_u32(kWsaBuf + 4, static_cast<u32>(kBuf)).is_ok());
    ASSERT_TRUE(mem_.write_u32(kOv + 0, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(kOv + 4, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(kOv + 16, hevent).is_ok());  // hEvent
    ASSERT_TRUE(mem_.write_u32(kBytes, 0).is_ok());

    const uint32_t rc =
        inv_->call("WSASend", {sock, kWsaBuf, 1u, kBytes, 0u, kOv, 0u});
    EXPECT_EQ(rc, 0u);
    EXPECT_EQ(mem_.read_u32(kBytes).value_or(0), 5u);
    EXPECT_EQ(mem_.read_u32(kOv).value_or(1), 0u);       // STATUS_SUCCESS
    EXPECT_EQ(mem_.read_u32(kOv + 4).value_or(0), 5u);   // InternalHigh
    EXPECT_EQ(sync_->wait_for_single_object(hevent, 0), WAIT_OBJECT_0)
        << "OVERLAPPED.hEvent must be signaled on sync WSASend success";

    inv_->call("closesocket", {sock});
    EXPECT_TRUE(sync_->close_handle(hevent));
}

TEST_F(Ws2LoopbackTest, WsaRecvOverlappedPostsCompletionWhenDataArrives) {
    // Battle.net / Chromium: SOCKET associated with IOCP, WSARecv(OVERLAPPED)
    // must return WSA_IO_PENDING and later deliver bytes via completion — not
    // WSAEWOULDBLOCK with no packet (that stalls Schannel after ClientHello).
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

    std::thread server([&] {
        int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const char payload[] = "HELLO";
        (void)::send(cfd, payload, 5, 0);
        ::shutdown(cfd, SHUT_WR);
        ::close(cfd);
    });

    mgr_.set_memory(&mem_);
    std::atomic<u32> completed_bytes{0xFFFFFFFFu};
    std::atomic<GuestAddr> completed_ov{0};
    mgr_.set_overlapped_completion_callback(
        [&](Handle, u32 bytes, GuestAddr ov, bool is64, u32 nt_status, u32,
            bool) {
            if (is64) {
                (void)mem_.write_u64(ov, nt_status);
                (void)mem_.write_u64(ov + 8, bytes);
            } else {
                (void)mem_.write_u32(ov, nt_status);
                (void)mem_.write_u32(ov + 4, bytes);
            }
            completed_bytes.store(bytes, std::memory_order_release);
            completed_ov.store(ov, std::memory_order_release);
        });

    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);
    EXPECT_TRUE(mem_.write_u32(OUT_ADDR + 0x10, 1u).is_ok());
    ASSERT_EQ(inv_->call("ioctlsocket", {sock, 0x8004667Eu, OUT_ADDR + 0x10}), 0u);

    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());
    ASSERT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(port)).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
    for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);
    // Nonblocking (FIONBIO) connect: WSAEWOULDBLOCK, then the connection
    // completes (ws2_core_probe connect_nb_ok / connect_nb_ok_select).
    EXPECT_EQ(inv_->call("connect", {sock, ADDR_ADDR, 16u}), 0xFFFFFFFFu);
    EXPECT_EQ(mgr_.last_error(), 10035u);
    {
        struct pollfd pfd {};
        pfd.fd = mgr_.get_native_fd(sock);
        pfd.events = POLLOUT;
        ASSERT_EQ(::poll(&pfd, 1, 2000), 1);
    }

    constexpr GuestAddr kBuf = DATA_BASE + 0x5000;
    constexpr GuestAddr kWsaBuf = DATA_BASE + 0x5100;
    constexpr GuestAddr kOv = DATA_BASE + 0x5200;
    constexpr GuestAddr kBytes = DATA_BASE + 0x5300;
    constexpr GuestAddr kFlags = DATA_BASE + 0x5304;
    ASSERT_TRUE(mem_.write_u32(kWsaBuf + 0, 64).is_ok());
    ASSERT_TRUE(mem_.write_u32(kWsaBuf + 4, static_cast<u32>(kBuf)).is_ok());
    ASSERT_TRUE(mem_.write_u32(kOv + 0, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(kOv + 4, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(kOv + 16, 0).is_ok());
    ASSERT_TRUE(mem_.write_u32(kBytes, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(kFlags, 0).is_ok());

    const uint32_t rc = inv_->call(
        "WSARecv", {sock, kWsaBuf, 1u, kBytes, kFlags, kOv, 0u});
    EXPECT_EQ(rc, 0xFFFFFFFFu);
    EXPECT_EQ(mgr_.last_error(), 997u) << "must be WSA_IO_PENDING, not WSAEWOULDBLOCK";
    EXPECT_EQ(mem_.read_u32(kOv).value_or(0), 0x103u);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (completed_bytes.load(std::memory_order_acquire) == 0xFFFFFFFFu &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(completed_bytes.load(), 5u);
    EXPECT_EQ(completed_ov.load(), kOv);
    EXPECT_EQ(mem_.read_u32(kOv).value_or(1), 0u);
    EXPECT_EQ(mem_.read_u32(kOv + 4).value_or(0), 5u);

    char got[6] = {};
    for (int i = 0; i < 5; ++i) {
        got[i] = static_cast<char>(mem_.read_u8(kBuf + i).value_or(0));
    }
    EXPECT_STREQ(got, "HELLO");

    inv_->call("closesocket", {sock});
    mgr_.clear_overlapped_completion_callback();
    stop.store(true);
    ::close(listen_fd);
    server.join();
}
