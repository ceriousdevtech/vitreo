/// @file test_ws2_32_loopback_sockopt.cpp
/// @brief ws2_32 loopback tests for setsockopt/getsockopt option-number and
///        layout translation between Winsock and the host.
///
/// The shared fixture, the localhost server and the thunk-invocation harness are
/// documented in ws2_loopback_fixture.hpp.

#include "ws2_loopback_fixture.hpp"

TEST_F(Ws2LoopbackTest, ExclusiveAddressUseAllowsCurlWakeupSocketSetup) {
    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    ASSERT_TRUE(mem_.write_u32(option_value, 1u).is_ok());

    // Windows SO_EXCLUSIVEADDRUSE is -5 at SOL_SOCKET. Curl enables it on
    // the loopback listener used to wake its threaded DNS resolver.
    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0xFFFFFFFBu, option_value, 4u}),
              0u);

    int reuse = 1;
    socklen_t reuse_len = sizeof(reuse);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), SOL_SOCKET, SO_REUSEADDR,
                           &reuse, &reuse_len),
              0);
    EXPECT_EQ(reuse, 0);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, BroadcastOptionUsesHostBroadcastNotLinger) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    ASSERT_TRUE(mem_.write_u32(option_value, 1u).is_ok());

    // Winsock SOL_SOCKET/SO_BROADCAST are 0xFFFF/0x0020.  Darwin's
    // SO_LINGER is also 0x0080, so passing guest constants through can set
    // an unrelated option or reject the call with EINVAL.
    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x0020u, option_value, 4u}),
              0u);

    int enabled = 0;
    socklen_t enabled_len = sizeof(enabled);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), SOL_SOCKET, SO_BROADCAST,
                           &enabled, &enabled_len),
              0);
    EXPECT_NE(enabled, 0);

    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(result_value, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0xFFFFu, 0x0020u,
                          result_value, result_length}),
              0u);
    EXPECT_NE(mem_.read_u32(result_value).value_or(0), 0u);
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, ReceiveBufferOptionRoundTripsWithWinsockNumber) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(option_value, 256u * 1024u).is_ok());

    // Winsock SO_RCVBUF is 0x1002. Darwin assigns a different number, so
    // both directions must translate the guest constant explicitly.
    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x1002u, option_value, 4u}),
              0u);

    int native_size = 0;
    socklen_t native_length = sizeof(native_size);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), SOL_SOCKET, SO_RCVBUF,
                           &native_size, &native_length),
              0);
    EXPECT_GT(native_size, 0);

    ASSERT_TRUE(mem_.write_u32(result_value, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0xFFFFu, 0x1002u,
                          result_value, result_length}),
              0u);
    EXPECT_EQ(mem_.read_u32(result_value).value_or(0),
              static_cast<uint32_t>(native_size));
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, ZeroSendBufferDisablesWinsockBuffering) {
    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(option_value, 0u).is_ok());

    // Winsock explicitly permits SO_SNDBUF=0 to disable send buffering.
    // Darwin rejects that value, so the compatibility layer must preserve the
    // Windows-visible state without forwarding the invalid host option.
    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x1001u, option_value, 4u}),
              0u);

    ASSERT_TRUE(mem_.write_u32(result_value, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0xFFFFu, 0x1001u,
                          result_value, result_length}),
              0u);
    EXPECT_EQ(mem_.read_u32(result_value).value_or(0xFFFFFFFFu), 0u);
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, IpDontFragmentUsesWinsockOptionNumber) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(option_value, 1u).is_ok());

    // Winsock IPPROTO_IP/IP_DONTFRAGMENT are level 0 and option 14. Darwin's
    // equivalent IP_DONTFRAG constant is 28, so the option must be translated.
    EXPECT_EQ(inv_->call("setsockopt", {sock, 0u, 14u, option_value, 4u}), 0u);

    int native_enabled = 0;
    socklen_t native_length = sizeof(native_enabled);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), IPPROTO_IP, IP_DONTFRAG,
                           &native_enabled, &native_length),
              0);
    EXPECT_NE(native_enabled, 0);

    ASSERT_TRUE(mem_.write_u32(result_value, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0u, 14u, result_value, result_length}),
              0u);
    EXPECT_NE(mem_.read_u32(result_value).value_or(0), 0u);
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, RecvTosOptionMapsToHostRecvTos) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(option_value, 1u).is_ok());

    // ws2ipdef.h gives IP_RECVTOS and IP_RECVTCLASS the same number, 40 —
    // both request the received TOS byte as control data. Darwin spells it
    // IP_RECVTOS, so the option number must be translated; passing 40 through
    // reaches the host as a foreign option and fails with EINVAL.
    EXPECT_EQ(inv_->call("setsockopt", {sock, 0u, 40u, option_value, 4u}), 0u);

    int native_enabled = 0;
    socklen_t native_length = sizeof(native_enabled);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), IPPROTO_IP, IP_RECVTOS,
                           &native_enabled, &native_length),
              0);
    EXPECT_NE(native_enabled, 0);

    ASSERT_TRUE(mem_.write_u32(result_value, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0u, 40u, result_value, result_length}),
              0u);
    EXPECT_NE(mem_.read_u32(result_value).value_or(0), 0u);
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, RandomizePortAndRecvEcnMapToHostBehavior) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(option_value, 1u).is_ok());

    // Windows SO_RANDOMIZE_PORT is 0x3005. Darwin already randomizes
    // ephemeral ports, so a valid DWORD request succeeds without a host
    // setsockopt number being passed through.
    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x3005u, option_value, 4u}),
              0u);

    // Windows IP_RECVECN is option 50. Darwin exposes the equivalent receive
    // control facility as IP_RECVTOS; its returned TOS byte contains ECN.
    EXPECT_EQ(inv_->call("setsockopt", {sock, 0u, 50u, option_value, 4u}), 0u);

    int native_enabled = 0;
    socklen_t native_length = sizeof(native_enabled);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), IPPROTO_IP, IP_RECVTOS,
                           &native_enabled, &native_length),
              0);
    EXPECT_NE(native_enabled, 0);

    ASSERT_TRUE(mem_.write_u32(result_value, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0u, 50u, result_value, result_length}),
              0u);
    EXPECT_NE(mem_.read_u32(result_value).value_or(0), 0u);
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, TcpKeepaliveOptionsMapToHost) {
    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    ASSERT_TRUE(mem_.write_u32(option_value, 45u).is_ok());

    // Windows IPPROTO_TCP / TCP_KEEPIDLE (= TCP_KEEPALIVE) is level=6, opt=3.
    // Must not return WSAENOPROTOOPT; Darwin TCP_KEEPALIVE is a different number.
    EXPECT_EQ(inv_->call("setsockopt", {sock, 6u, 3u, option_value, 4u}), 0u);

#ifdef TCP_KEEPALIVE
    int idle = 0;
    socklen_t idle_len = sizeof(idle);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), IPPROTO_TCP, TCP_KEEPALIVE,
                           &idle, &idle_len),
              0);
    EXPECT_EQ(idle, 45);
#endif

    ASSERT_TRUE(mem_.write_u32(option_value, 7u).is_ok());
    // TCP_KEEPCNT = 16, TCP_KEEPINTVL = 17
    EXPECT_EQ(inv_->call("setsockopt", {sock, 6u, 16u, option_value, 4u}), 0u);
    ASSERT_TRUE(mem_.write_u32(option_value, 5u).is_ok());
    EXPECT_EQ(inv_->call("setsockopt", {sock, 6u, 17u, option_value, 4u}), 0u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, LingerMarshalsWinsockFourByteLayout) {
    const uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
    ASSERT_GE(sock, 0x10000u);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    ASSERT_TRUE(mem_.write_u16(option_value, 1u).is_ok());
    ASSERT_TRUE(mem_.write_u16(option_value + 2, 7u).is_ok());

    EXPECT_EQ(inv_->call("setsockopt",
                         {sock, 0xFFFFu, 0x0080u, option_value, 4u}),
              0u);

    // Winsock's l_linger is seconds; Darwin's SO_LINGER is in clock ticks,
    // so the host option in seconds is SO_LINGER_SEC.
    struct linger native_linger {};
    socklen_t native_len = sizeof(native_linger);
    ASSERT_EQ(::getsockopt(mgr_.get_native_fd(sock), SOL_SOCKET, SO_LINGER_SEC,
                           &native_linger, &native_len),
              0);
    EXPECT_NE(native_linger.l_onoff, 0);
    EXPECT_EQ(native_linger.l_linger, 7);

    constexpr GuestAddr result_value = OUT_ADDR + 0x30;
    constexpr GuestAddr result_length = OUT_ADDR + 0x34;
    ASSERT_TRUE(mem_.write_u32(result_value, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(result_length, 4u).is_ok());
    EXPECT_EQ(inv_->call("getsockopt",
                         {sock, 0xFFFFu, 0x0080u,
                          result_value, result_length}),
              0u);
    EXPECT_EQ(mem_.read_u16(result_value).value_or(0), 1u);
    EXPECT_EQ(mem_.read_u16(result_value + 2).value_or(0), 7u);
    EXPECT_EQ(mem_.read_u32(result_length).value_or(0), 4u);

    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}
