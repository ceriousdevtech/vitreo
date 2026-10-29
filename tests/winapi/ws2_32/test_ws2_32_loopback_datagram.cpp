/// @file test_ws2_32_loopback_datagram.cpp
/// @brief ws2_32 loopback tests for UDP datagram message extensions:
///        WSARecvMsg, WSASendMsg and extension-function discovery.
///
/// The shared fixture, the localhost server and the thunk-invocation harness are
/// documented in ws2_loopback_fixture.hpp.

#include "ws2_loopback_fixture.hpp"

TEST_F(Ws2LoopbackTest, WsaRecvMsgReturnsWindowsEcnControlMessage) {
    const uint32_t sock = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_GE(sock, 0x10000u);

    sockaddr_in receiver{};
    receiver.sin_family = AF_INET;
    receiver.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    receiver.sin_port = 0;
    ASSERT_EQ(::bind(mgr_.get_native_fd(sock),
                     reinterpret_cast<sockaddr*>(&receiver),
                     sizeof(receiver)),
              0) << std::strerror(errno);
    socklen_t receiver_len = sizeof(receiver);
    ASSERT_EQ(::getsockname(mgr_.get_native_fd(sock),
                            reinterpret_cast<sockaddr*>(&receiver),
                            &receiver_len),
              0);

    constexpr GuestAddr option_value = OUT_ADDR + 0x20;
    ASSERT_TRUE(mem_.write_u32(option_value, 1u).is_ok());
    ASSERT_EQ(inv_->call("setsockopt", {sock, 0u, 50u, option_value, 4u}), 0u);

    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sender, 0);
    const int ect0 = 2;
    ASSERT_EQ(::setsockopt(sender, IPPROTO_IP, IP_TOS, &ect0, sizeof(ect0)), 0);
    constexpr char payload[] = "ecn";
    ASSERT_EQ(::sendto(sender, payload, sizeof(payload) - 1, 0,
                       reinterpret_cast<sockaddr*>(&receiver), sizeof(receiver)),
              static_cast<ssize_t>(sizeof(payload) - 1));

    // x86 WSAMSG layout: name@0, namelen@4, buffers@8, count@12,
    // Control.len@16, Control.buf@20, flags@24.  WSABUF is {len, buf}.
    constexpr GuestAddr msg = OUT_ADDR + 0x100;
    constexpr GuestAddr wsabuf = OUT_ADDR + 0x140;
    constexpr GuestAddr data = OUT_ADDR + 0x180;
    constexpr GuestAddr source = OUT_ADDR + 0x1c0;
    constexpr GuestAddr control = OUT_ADDR + 0x200;
    constexpr GuestAddr bytes = OUT_ADDR + 0x280;
    ASSERT_TRUE(mem_.write_u32(wsabuf, 16u).is_ok());
    ASSERT_TRUE(mem_.write_u32(wsabuf + 4, data).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg, source).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg + 4, 16u).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg + 8, wsabuf).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg + 12, 1u).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg + 16, 64u).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg + 20, control).is_ok());
    ASSERT_TRUE(mem_.write_u32(msg + 24, 0u).is_ok());

    EXPECT_EQ(inv_->call("WSARecvMsg", {sock, msg, bytes, 0u, 0u}), 0u);
    EXPECT_EQ(mem_.read_u32(bytes).value_or(0), sizeof(payload) - 1);
    std::array<Byte, sizeof(payload) - 1> received{};
    ASSERT_TRUE(mem_.read_bytes(data, received.data(), received.size()).is_ok());
    EXPECT_EQ(std::memcmp(received.data(), payload, received.size()), 0);

    EXPECT_EQ(mem_.read_u32(msg + 16).value_or(0), 16u);
    EXPECT_EQ(mem_.read_u32(control).value_or(0), 16u);
    EXPECT_EQ(mem_.read_u32(control + 4).value_or(~0u), 0u);
    EXPECT_EQ(mem_.read_u32(control + 8).value_or(0), 50u);
    EXPECT_EQ(mem_.read_u32(control + 12).value_or(~0u), 2u);
    EXPECT_EQ(mem_.read_u32(msg + 24).value_or(~0u), 0u);

    ::close(sender);
    EXPECT_EQ(inv_->call("closesocket", {sock}), 0u);
}

TEST_F(Ws2LoopbackTest, WsaSendMsgDeliversADatagramOverLoopback) {
    constexpr GuestAddr ADDR_IN   = OUT_ADDR + 0x400;
    constexpr GuestAddr MSG       = OUT_ADDR + 0x440;
    constexpr GuestAddr WSABUF    = OUT_ADDR + 0x480;
    constexpr GuestAddr PAYLOAD   = OUT_ADDR + 0x4C0;
    constexpr GuestAddr SENT      = OUT_ADDR + 0x500;
    constexpr GuestAddr RECV_BUF  = OUT_ADDR + 0x540;
    constexpr GuestAddr BOUND     = OUT_ADDR + 0x580;
    constexpr GuestAddr BOUND_LEN = OUT_ADDR + 0x5C0;

    const u64 receiver = inv_->call("socket", {2u, 2u, 17u});
    const u64 sender = inv_->call("socket", {2u, 2u, 17u});
    ASSERT_NE(receiver, static_cast<u64>(~0u));
    ASSERT_NE(sender, static_cast<u64>(~0u));

    // Bind the receiver to an ephemeral loopback port and read it back.
    ASSERT_TRUE(mem_.write_u16(ADDR_IN, 2u).is_ok());          // AF_INET
    ASSERT_TRUE(mem_.write_u16(ADDR_IN + 2, 0u).is_ok());      // port 0
    ASSERT_TRUE(mem_.write_u32(ADDR_IN + 4, 0x0100007Fu).is_ok());  // 127.0.0.1
    ASSERT_TRUE(mem_.write_u32(ADDR_IN + 8, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(ADDR_IN + 12, 0u).is_ok());
    ASSERT_EQ(inv_->call("bind", {receiver, ADDR_IN, 16u}), 0u);
    ASSERT_TRUE(mem_.write_u32(BOUND_LEN, 16u).is_ok());
    ASSERT_EQ(inv_->call("getsockname", {receiver, BOUND, BOUND_LEN}), 0u);
    const u16 port = mem_.read_u16(BOUND + 2).value_or(0);
    ASSERT_NE(port, 0u);

    static constexpr char payload[] = "sdr-probe";
    ASSERT_TRUE(mem_.write_bytes(
        PAYLOAD, reinterpret_cast<const Byte*>(payload),
        sizeof(payload) - 1).is_ok());

    // x86 WSAMSG: name@0, namelen@4, buffers@8, count@12, control.len@16,
    // control.buf@20, flags@24. WSABUF is {len, buf}.
    ASSERT_TRUE(mem_.write_u16(ADDR_IN + 2, port).is_ok());
    ASSERT_TRUE(mem_.write_u32(WSABUF, sizeof(payload) - 1).is_ok());
    ASSERT_TRUE(mem_.write_u32(WSABUF + 4, PAYLOAD).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG, ADDR_IN).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG + 4, 16u).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG + 8, WSABUF).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG + 12, 1u).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG + 16, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG + 20, 0u).is_ok());
    ASSERT_TRUE(mem_.write_u32(MSG + 24, 0u).is_ok());

    EXPECT_EQ(inv_->call("WSASendMsg", {sender, MSG, 0u, SENT, 0u, 0u}), 0u);
    EXPECT_EQ(mem_.read_u32(SENT).value_or(0), sizeof(payload) - 1);

    // The datagram must actually arrive, not merely be accepted.
    const u64 got = inv_->call(
        "recvfrom", {receiver, RECV_BUF, 64u, 0u, 0u, 0u});
    ASSERT_EQ(got, sizeof(payload) - 1);
    std::array<Byte, sizeof(payload) - 1> received{};
    ASSERT_TRUE(mem_.read_bytes(
        RECV_BUF, received.data(), received.size()).is_ok());
    EXPECT_EQ(std::memcmp(received.data(), payload, received.size()), 0);

    (void)inv_->call("closesocket", {sender});
    (void)inv_->call("closesocket", {receiver});
}

TEST_F(Ws2LoopbackTest, WsaIoctlResolvesBothMessageExtensionPointers) {
    constexpr u32 SIO_GET_EXTENSION_FUNCTION_POINTER = 0xC8000006u;
    constexpr GuestAddr GUID_ADDR = OUT_ADDR + 0x300;
    constexpr GuestAddr FUNC_OUT = OUT_ADDR + 0x340;
    constexpr GuestAddr RETURNED = OUT_ADDR + 0x360;

    // The extension pointers come out of the thunk registry, which the
    // loopback fixture does not wire up, so give the manager one here.
    ThunkRegistry registry(0x70000000);
    ASSERT_TRUE(
        registry.register_dll("ws2_32.dll", create_ws2_thunks(mgr_)).is_ok());
    mgr_.set_thunk_registry(&registry);

    const u64 sock = inv_->call("socket", {2u, 2u, 17u});  // AF_INET, SOCK_DGRAM
    ASSERT_NE(sock, static_cast<u64>(~0u));

    const auto resolve = [&](const std::array<u8, 16>& guid) -> u64 {
        EXPECT_TRUE(mem_.write_bytes(
            GUID_ADDR, reinterpret_cast<const Byte*>(guid.data()),
            guid.size()).is_ok());
        EXPECT_TRUE(mem_.write_u64(FUNC_OUT, 0).is_ok());
        const u64 rc = inv_->call(
            "WSAIoctl", {sock, SIO_GET_EXTENSION_FUNCTION_POINTER, GUID_ADDR,
                         16u, FUNC_OUT, 8u, RETURNED, 0u, 0u});
        EXPECT_EQ(rc, 0u);
        return mem_.read_u64(FUNC_OUT).value_or(0);
    };

    // WSAID_WSARECVMSG = {f689d7c8-6f1f-436b-8a53-e54fe351c322}
    const std::array<u8, 16> recv_guid{
        0xc8, 0xd7, 0x89, 0xf6, 0x1f, 0x6f, 0x6b, 0x43,
        0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22};
    // WSAID_WSASENDMSG = {a441e712-754f-43ca-84a7-0dee44cf606d}
    const std::array<u8, 16> send_guid{
        0x12, 0xe7, 0x41, 0xa4, 0x4f, 0x75, 0xca, 0x43,
        0x84, 0xa7, 0x0d, 0xee, 0x44, 0xcf, 0x60, 0x6d};

    const u64 recv_ptr = resolve(recv_guid);
    const u64 send_ptr = resolve(send_guid);
    EXPECT_NE(recv_ptr, 0u) << "WSARecvMsg must be discoverable";
    EXPECT_NE(send_ptr, 0u) << "WSASendMsg must be discoverable";
    // Distinct entry points, not the same thunk handed out twice.
    EXPECT_NE(recv_ptr, send_ptr);

    (void)inv_->call("closesocket", {sock});
    mgr_.set_thunk_registry(nullptr);
}
