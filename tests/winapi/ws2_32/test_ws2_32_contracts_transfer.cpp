/// @file test_ws2_32_contracts_transfer.cpp
/// @brief ws2_32 contract tests for the recv/send/sendto data paths, truncation,
///        flags and receive/send faults.
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// ws2_core_probe udp_recv_trunc: rc=-1 err=10040, first 10 bytes kept;
// udp_recv_next still gets the following datagram.
TEST_P(Ws2ContractTest, UdpRecvTruncationReportsWsaeMsgSize) {
    startup();
    const u64 a = call("socket", {2, 2, 17});
    const u64 b = call("socket", {2, 2, 17});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {a, sa, 16}), 0u);
    ASSERT_EQ(call("bind", {b, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {b, kData + 0x50, kData + 0x40}), 0u);
    char big[100];
    for (int i = 0; i < 100; ++i) big[i] = static_cast<char>('A' + i % 26);
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>(big), 100);
    (void)mem_.write_bytes(kData + 0x200, reinterpret_cast<const Byte*>("second"), 6);
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 100, 0, kData + 0x50, 16}), 100u);
    ASSERT_EQ(call("sendto", {a, kData + 0x200, 6, 0, kData + 0x50, 16}), 6u);
    (void)mem_.write_bytes(kData + 0x300, std::vector<Byte>(16, 0).data(), 16);
    EXPECT_EQ(static_cast<u32>(call("recv", {b, kData + 0x300, 10, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10040u);  // WSAEMSGSIZE
    EXPECT_EQ(str(kData + 0x300), "ABCDEFGHIJ");
    (void)mem_.write_bytes(kData + 0x300, std::vector<Byte>(16, 0).data(), 16);
    EXPECT_EQ(call("recv", {b, kData + 0x300, 64, 0}), 6u);
    EXPECT_EQ(str(kData + 0x300), "second");
}

// ws2_core_probe recvfrom_trunc: rc=-1 err=10040 with buffer filled.
TEST_P(Ws2ContractTest, UdpRecvFromTruncationReportsWsaeMsgSize) {
    startup();
    const u64 a = call("socket", {2, 2, 17});
    const u64 b = call("socket", {2, 2, 17});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {a, sa, 16}), 0u);
    ASSERT_EQ(call("bind", {b, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {b, kData + 0x50, kData + 0x40}), 0u);
    char big[100];
    for (int i = 0; i < 100; ++i) big[i] = static_cast<char>('A' + i % 26);
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>(big), 100);
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 100, 0, kData + 0x50, 16}), 100u);
    (void)mem_.write_bytes(kData + 0x300, std::vector<Byte>(16, 0).data(), 16);
    (void)mem_.write_u32(kData + 0x3F0, 16);
    EXPECT_EQ(static_cast<u32>(
                  call("recvfrom", {b, kData + 0x300, 10, 0, kData + 0x380, kData + 0x3F0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10040u);
    EXPECT_EQ(str(kData + 0x300), "ABCDEFGHIJ");
    EXPECT_EQ(mem_.read_u16(kData + 0x380).value(), 2u);
}

// ws2_core_probe recvfrom_short_fromlen: WSAEFAULT without consuming; next
// recvfrom gets the datagram.
TEST_P(Ws2ContractTest, RecvFromShortFromlenFailsWithoutConsuming) {
    startup();
    const u64 a = call("socket", {2, 2, 17});
    const u64 b = call("socket", {2, 2, 17});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {a, sa, 16}), 0u);
    ASSERT_EQ(call("bind", {b, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {b, kData + 0x50, kData + 0x40}), 0u);
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>("fromlen"), 7);
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 7, 0, kData + 0x50, 16}), 7u);
    (void)mem_.write_bytes(kData + 0x380, std::vector<Byte>(16, 0xCC).data(), 16);
    (void)mem_.write_u32(kData + 0x3F0, 8);  // too small for sockaddr_in
    (void)mem_.write_bytes(kData + 0x300, std::vector<Byte>(16, 0).data(), 16);
    EXPECT_EQ(static_cast<u32>(
                  call("recvfrom", {b, kData + 0x300, 64, 0, kData + 0x380, kData + 0x3F0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(kData + 0x3F0).value(), 8u);
    EXPECT_EQ(mem_.read_u16(kData + 0x380).value(), 0xCCCCu);
    EXPECT_EQ(str(kData + 0x300), "");
    (void)mem_.write_u32(kData + 0x3F0, 16);
    EXPECT_EQ(call("recvfrom", {b, kData + 0x300, 64, 0, kData + 0x380, kData + 0x3F0}), 7u);
    EXPECT_EQ(str(kData + 0x300), "fromlen");
}

// ws2_core_probe udp_recv_trunc: oversize datagram -> WSAEMSGSIZE (10040) with
// the buffer filled; WSARecv must also set MSG_PARTIAL (0x8000) in *lpFlags.
TEST_P(Ws2ContractTest, WsaRecvDatagramTruncationReportsWsaeMsgSize) {
    startup();
    const u64 a = call("socket", {2, 2, 17});
    const u64 b = call("socket", {2, 2, 17});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {b, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {b, kData + 0x50, kData + 0x40}), 0u);
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>("ABCDEFGHIJKLMNOP"), 16);
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 16, 0, kData + 0x50, 16}), 16u);

    const GuestAddr wsabuf = kData + 0x200;
    const GuestAddr buf = kData + 0x300;
    const GuestAddr nbytes = kData + 0x400;
    const GuestAddr flags = kData + 0x404;
    (void)mem_.write_bytes(buf, std::vector<Byte>(16, 0).data(), 16);
    (void)mem_.write_u32(wsabuf, 10);  // short buffer
    if (is64()) (void)mem_.write_u64(wsabuf + 8, buf);
    else (void)mem_.write_u32(wsabuf + 4, static_cast<u32>(buf));
    (void)mem_.write_u32(nbytes, 0xDEAD);
    (void)mem_.write_u32(flags, 0);
    EXPECT_EQ(static_cast<u32>(call("WSARecv", {b, wsabuf, 1, nbytes, flags, 0, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10040u);
    EXPECT_EQ(mem_.read_u32(nbytes).value(), 10u);
    EXPECT_EQ((mem_.read_u32(flags).value() & 0x8000u), 0x8000u);
    std::string got;
    for (u32 i = 0; i < 10; ++i)
        got.push_back(static_cast<char>(mem_.read_u8(buf + i).value()));
    EXPECT_EQ(got, "ABCDEFGHIJ");
}

// ws2_core_probe tcp_sendto_other_addr / udp_connected_sendto_other: TCP
// ignores to; connected UDP overrides destination for that datagram.
TEST_P(Ws2ContractTest, SendToIgnoresTcpAddrAndOverridesConnectedUdp) {
    startup();
    // Connected UDP override first (isolate from TCP pair).
    const u64 u = call("socket", {2, 2, 17});
    const u64 b = call("socket", {2, 2, 17});
    const u64 d = call("socket", {2, 2, 17});
    const GuestAddr uba = kData + 0x180;
    (void)mem_.write_u16(uba, 2);
    (void)mem_.write_u16(uba + 2, 0);
    (void)mem_.write_u32(uba + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {b, uba, 16}), 0u);
    ASSERT_EQ(call("bind", {d, uba, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {b, kData + 0x50, kData + 0x40}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {d, kData + 0x60, kData + 0x40}), 0u);
    ASSERT_EQ(call("connect", {u, kData + 0x50, 16}), 0u);
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>("tod"), 3);
    ASSERT_EQ(call("sendto", {u, kData + 0x100, 3, 0, kData + 0x60, 16}), 3u)
        << "err=" << wsa_error();
    (void)mem_.write_u32(kData + 0x280, 500);  // 500 ms RCVTIMEO
    ASSERT_EQ(call("setsockopt", {d, 0xFFFF, 0x1006, kData + 0x280, 4}), 0u);
    EXPECT_EQ(call("recv", {d, kData + 0x200, 64, 0}), 3u) << "err=" << wsa_error();
    EXPECT_EQ(str(kData + 0x200), "tod");

    const u64 listener = call("socket", {2, 1, 6});
    const u64 c = call("socket", {2, 1, 6});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {c, sa, 16}), 0u);
    const u64 s = call("accept", {listener, 0, 0});
    ASSERT_NE(s, invalid_socket());
    const GuestAddr other = kData + 0x80;
    (void)mem_.write_u16(other, 2);
    (void)mem_.write_u16(other + 2, htons(9));
    (void)mem_.write_u32(other + 4, htonl(0x01020304));
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>("tcpto"), 5);
    EXPECT_EQ(call("sendto", {c, kData + 0x100, 5, 0, other, 16}), 5u);
    (void)mem_.write_bytes(kData + 0x200, std::vector<Byte>(16, 0).data(), 16);
    EXPECT_EQ(call("recv", {s, kData + 0x200, 64, 0}), 5u);
    EXPECT_EQ(str(kData + 0x200), "tcpto");
}

// ws2_core_probe udp_sendto_*: short tolen/bad ptr -> 10014; bad family /
// AF mismatch -> 10047; INADDR_ANY -> 10049; null to -> 10057.
TEST_P(Ws2ContractTest, SendToDatagramAddrValidationMatchesOracle) {
    startup();
    const u64 u = call("socket", {2, 2, 17});
    const GuestAddr da = kData + 0x50;
    (void)mem_.write_u16(da, 2);
    (void)mem_.write_u16(da + 2, htons(9));
    (void)mem_.write_u32(da + 4, htonl(INADDR_LOOPBACK));
    (void)mem_.write_bytes(kData + 0x100, reinterpret_cast<const Byte*>("x"), 1);

    EXPECT_EQ(static_cast<u32>(call("sendto", {u, kData + 0x100, 1, 0, da, 8})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);

    (void)mem_.write_u16(da, 99);
    EXPECT_EQ(static_cast<u32>(call("sendto", {u, kData + 0x100, 1, 0, da, 16})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10047u);
    (void)mem_.write_u16(da, 2);

    (void)mem_.write_u32(da + 4, 0);  // INADDR_ANY
    EXPECT_EQ(static_cast<u32>(call("sendto", {u, kData + 0x100, 1, 0, da, 16})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10049u);
    (void)mem_.write_u32(da + 4, htonl(INADDR_LOOPBACK));

    EXPECT_EQ(static_cast<u32>(call("sendto", {u, kData + 0x100, 1, 0, 0, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
    EXPECT_EQ(static_cast<u32>(call("sendto", {u, kData + 0x100, 1, 0, 0, 16})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);

    const GuestAddr six = kData + 0x80;
    (void)mem_.write_bytes(six, std::vector<Byte>(28, 0).data(), 28);
    (void)mem_.write_u16(six, 23);  // AF_INET6
    (void)mem_.write_u16(six + 2, htons(9));
    (void)mem_.write_u8(six + 8 + 15, 1);  // ::1
    EXPECT_EQ(static_cast<u32>(call("sendto", {u, kData + 0x100, 1, 0, six, 28})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10047u);

    EXPECT_EQ(static_cast<u32>(
                  call("sendto", {u, kData + 0x100, 1, 0, GuestAddr{16}, 16})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
}

// ws2_opts_probe rcvtimeo_recv: rc=-1 err=10060 after ~500 ms; the socket
// stays usable (rcvtimeo_recv_after_timeout rc=2).
TEST_P(Ws2ContractTest, ReceiveTimeoutFailsWithWsaeTimedOut) {
    startup();
    const u64 s = call("socket", {2, 2, 17});
    (void)mem_.write_u16(kData, 2);
    (void)mem_.write_u16(kData + 2, 0);
    (void)mem_.write_u32(kData + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {s, kData, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 200);
    ASSERT_EQ(call("setsockopt", {s, 0xFFFF, 0x1006, kData + 0x40, 4}), 0u);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(static_cast<u32>(call("recv", {s, kData + 0x100, 16, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10060u);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(150));
}

// ws2_core_probe udp/tcp_recv_waitall_nb + send_flag_*: unknown bits and
// MSG_WAITALL on nonblocking/datagram -> WSAEOPNOTSUPP; MSG_DONTROUTE ok.
TEST_P(Ws2ContractTest, RecvSendFlagsMatchOracle) {
    startup();
    const u64 u = call("socket", {2, 2, 17});
    (void)mem_.write_u32(kData + 0x100, 1);
    ASSERT_EQ(call("ioctlsocket", {u, 0x8004667E, kData + 0x100}), 0u);
    EXPECT_EQ(static_cast<u32>(call("recv", {u, kData, 16, 0x8})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);
    EXPECT_EQ(static_cast<u32>(call("recv", {u, kData, 16, 0x100})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);

    const u64 listener = call("socket", {2, 1, 6});
    const u64 c = call("socket", {2, 1, 6});
    (void)mem_.write_u16(kData, 2);
    (void)mem_.write_u16(kData + 2, 0);
    (void)mem_.write_u32(kData + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, kData, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, kData, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {c, kData, 16}), 0u);
    const u64 s = call("accept", {listener, 0, 0});
    ASSERT_NE(s, invalid_socket());
    (void)mem_.write_bytes(kData + 0x200, reinterpret_cast<const Byte*>("x"), 1);
    EXPECT_EQ(static_cast<u32>(call("send", {c, kData + 0x200, 1, 0x100})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);
    EXPECT_EQ(static_cast<u32>(call("send", {c, kData + 0x200, 1, 0x8})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);
    EXPECT_EQ(call("send", {c, kData + 0x200, 1, 0x4}), 1u);
    EXPECT_EQ(call("send", {c, kData + 0x200, 1, 0x8000}), 1u);
}

// ws2_core_probe recvfrom_null_from 10014 (fl untouched) followed by
// udp_recv_len0 10040 and udp_recv_after_len0 rc=4: the failed call did not
// consume "nofrom". ws2_corex_probe udp_null_buf 10014 + udp_after_null_buf
// "one", udp_len_neg 10014, udp_recvfrom_null_fromlen 10014.
TEST_P(Ws2ContractTest, DatagramReceiveFaultsKeepTheDatagram) {
    startup();
    const u64 a = bound(2, 17), b = bound(2, 17);
    loop4(kData, port_of(b));
    (void)put_str(kData + 0x100, "nofrom");
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 6, 0, kData, 16}), 6u);
    (void)put_str(kData + 0x100, "zero");
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 4, 0, kData, 16}), 4u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)mem_.write_u32(kData + 0x40, 16);
    EXPECT_EQ(rc32(call("recvfrom", {b, kData + 0x200, 64, 0, 0, kData + 0x40})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(kData + 0x40).value(), 16u);
    EXPECT_EQ(rc32(call("recvfrom", {b, kData + 0x200, 64, 0, kData + 0x300, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("recv", {b, 0, 10, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("recv", {b, kData + 0x200, 0xFFFFFFFFull, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("recv", {b, kData + 0x200, 0, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10040u);                               // "nofrom" consumed
    EXPECT_EQ(call("recv", {b, kData + 0x200, 64, 0}), 4u);        // "zero"
}

// ws2_core_probe tcp_recv_len_neg 10014, tcp_recv_null_buf 10053,
// tcp_recv_after_null_buf 10053 (b=""), tcp_recv_bad_ptr 10053;
// ws2_corex_probe nodata_null_buf 10035, nodata_len0 10035, data_len_neg
// 10014 then data_after_len_neg "keep", data_peek_null 10014 then 4,
// data_peer_recv_after_abort 10054, data_send_after_abort 10053,
// data_shutdown_after_abort 0, data_getpeername_after_abort 0.
TEST_P(Ws2ContractTest, StreamReceiveFaultsMatchOracle) {
    startup();
    u64 c = 0, s = 0;
    tcp_pair(&c, &s);
    set_nb(s, 1);
    EXPECT_EQ(rc32(call("recv", {s, 0, 10, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10035u);
    EXPECT_EQ(rc32(call("recv", {s, kData + 0x200, 0, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10035u);
    EXPECT_EQ(rc32(call("recv", {s, kData + 0x200, 0xFFFFFFFFull, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    (void)put_str(kData + 0x100, "keep");
    ASSERT_EQ(call("send", {c, kData + 0x100, 4, 0}), 4u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(rc32(call("recv", {s, kData + 0x200, 0xFFFFFF9Cull, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("recv", {s, 0, 10, 2})), 0xFFFFFFFFu);    // MSG_PEEK
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("recv", {s, 0, 65536, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10053u);
    EXPECT_EQ(rc32(call("recv", {s, kData + 0x200, 64, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10053u);
    EXPECT_EQ(rc32(call("send", {s, kData + 0x100, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10053u);
    EXPECT_EQ(call("shutdown", {s, 1}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    EXPECT_EQ(call("getpeername", {s, kData + 0x300, kData + 0x40}), 0u);
    EXPECT_EQ(mem_.read_u16(kData + 0x300).value(), 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(rc32(call("recv", {c, kData + 0x200, 64, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10054u);
}

// ws2_core_probe send_len0 rc=0; ws2_corex_probe send_null_buf 10014,
// send_len_neg 10014, send_null_len0 0, send_after_faults 2,
// udp_sendto_len_neg 10014, udp_sendto_len0 0 + udp_len0_arrives 0.
TEST_P(Ws2ContractTest, SendFaultsAndZeroLengthMatchOracle) {
    startup();
    u64 c = 0, s = 0;
    tcp_pair(&c, &s);
    EXPECT_EQ(call("send", {c, kData + 0x100, 0, 0}), 0u);
    EXPECT_EQ(call("send", {c, 0, 0, 0}), 0u);
    EXPECT_EQ(rc32(call("send", {c, 0, 10, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("send", {c, kData + 0x100, 0xFFFFFFFFull, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    (void)put_str(kData + 0x100, "ok");
    EXPECT_EQ(call("send", {c, kData + 0x100, 2, 0}), 2u);
    const u64 u = call("socket", {2, 2, 17}), b = bound(2, 17);
    loop4(kData, port_of(b));
    EXPECT_EQ(rc32(call("sendto", {u, kData + 0x100, 0xFFFFFFFFull, 0, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(call("sendto", {u, kData + 0x100, 0, 0, kData, 16}), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    set_nb(b, 1);
    EXPECT_EQ(call("recv", {b, kData + 0x200, 64, 0}), 0u);
}

}  // namespace
