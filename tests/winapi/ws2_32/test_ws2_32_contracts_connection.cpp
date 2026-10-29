/// @file test_ws2_32_contracts_connection.cpp
/// @brief ws2_32 contract tests for connection setup/teardown, endpoint queries,
///        address sharing and shutdown rules.
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// ws2_core_probe: accept_wouldblock "INVALID raw=0xffffffffffffffff err=10035".
TEST_P(Ws2ContractTest, AcceptWouldBlockReturnsFullWidthInvalidSocket) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    // bind 127.0.0.1:0, listen, FIONBIO=1
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {s, sa, 16}), 0u);
    ASSERT_EQ(call("listen", {s, 5}), 0u);
    (void)mem_.write_u32(kData + 0x100, 1);
    ASSERT_EQ(call("ioctlsocket", {s, 0x8004667E, kData + 0x100}), 0u);
    EXPECT_EQ(call("accept", {s, 0, 0}), invalid_socket());
    EXPECT_EQ(wsa_error(), 10035u);
    EXPECT_EQ(call("accept", {0x1234, 0, 0}), invalid_socket());
}

// ws2_core_probe accept_short_addrlen: INVALID err=10014; connection still
// pending so accept_after_short succeeds (addrlen buffer unchanged on fail).
TEST_P(Ws2ContractTest, AcceptShortAddrlenFailsWithoutConsuming) {
    startup();
    const u64 listener = call("socket", {2, 1, 6});
    const u64 client = call("socket", {2, 1, 6});
    ASSERT_NE(listener, invalid_socket());
    ASSERT_NE(client, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {client, sa, 16}), 0u);
    const GuestAddr peer = kData + 0x100;
    (void)mem_.write_bytes(peer, std::vector<Byte>(16, 0xCC).data(), 16);
    (void)mem_.write_u32(kData + 0x200, 4);  // too small
    EXPECT_EQ(call("accept", {listener, peer, kData + 0x200}), invalid_socket());
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 4u);
    EXPECT_EQ(mem_.read_u16(peer).value(), 0xCCCCu);
    (void)mem_.write_u32(kData + 0x200, 16);
    const u64 accepted = call("accept", {listener, peer, kData + 0x200});
    ASSERT_NE(accepted, invalid_socket());
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 16u);
    EXPECT_EQ(mem_.read_u16(peer).value(), 2u);
}

// ws2_core_probe connect_tcp_any_addr / connect_port_zero: WSAEADDRNOTAVAIL;
// udp_connect_zero disconnects (rc=0).
TEST_P(Ws2ContractTest, ConnectAnyAddressAndUdpDisconnectMatchOracle) {
    startup();
    const u64 t = call("socket", {2, 1, 6});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, htons(12345));
    (void)mem_.write_u32(sa + 4, 0);  // INADDR_ANY
    EXPECT_EQ(static_cast<u32>(call("connect", {t, sa, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10049u);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    (void)mem_.write_u16(sa + 2, 0);
    EXPECT_EQ(static_cast<u32>(call("connect", {t, sa, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10049u);
    // ws2_corex_probe udp4_connect_zero_unconnected: an all-zero address on
    // a UDP socket that has no association is WSAEADDRNOTAVAIL (only a
    // connected socket dissolves -- see UdpConnectZeroDissolvesOnlyAnAssociation).
    const u64 u = call("socket", {2, 2, 17});
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, 0);
    EXPECT_EQ(static_cast<u32>(call("connect", {u, sa, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10049u);
}

// ws2_core_probe send/recv_after_shutdown_*: WSAESHUTDOWN (10058).
TEST_P(Ws2ContractTest, ShutdownDirectionsReportWsaeShutdown) {
    startup();
    const u64 listener = call("socket", {2, 1, 6});
    const u64 s = call("socket", {2, 1, 6});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {s, sa, 16}), 0u);
    ASSERT_EQ(call("shutdown", {s, 1}), 0u);  // SD_SEND
    EXPECT_EQ(static_cast<u32>(call("send", {s, kData + 0x100, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10058u);
    ASSERT_EQ(call("shutdown", {s, 0}), 0u);  // SD_RECEIVE
    EXPECT_EQ(static_cast<u32>(call("recv", {s, kData + 0x100, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10058u);
}

// ws2_core_probe connect_nb_refused: rc=-1 err=10035 — nonblocking connect
    // returns WSAEWOULDBLOCK immediately (no host poll wait).
TEST_P(Ws2ContractTest, ConnectNonblockingReturnsWouldBlockImmediately) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    (void)mem_.write_u32(kData + 0x100, 1);
    ASSERT_EQ(call("ioctlsocket", {s, 0x8004667E, kData + 0x100}), 0u);
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, htons(1));
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(static_cast<u32>(call("connect", {s, sa, 16})), 0xFFFFFFFFu);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_EQ(wsa_error(), 10035u);
    EXPECT_LT(ms, 500);  // must not poll-wait up to 5s
}

// ws2_core_probe listen_unbound / listen_udp / listen_connected.
TEST_P(Ws2ContractTest, ListenUnboundUdpAndConnectedMatchOracle) {
    startup();
    const u64 tcp = call("socket", {2, 1, 6});
    EXPECT_EQ(static_cast<u32>(call("listen", {tcp, 1})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);

    const u64 udp = call("socket", {2, 2, 17});
    EXPECT_EQ(static_cast<u32>(call("listen", {udp, 1})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);

    const u64 listener = call("socket", {2, 1, 6});
    const u64 client = call("socket", {2, 1, 6});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {client, sa, 16}), 0u);
    EXPECT_EQ(static_cast<u32>(call("listen", {client, 1})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10056u);
}

// ws2_core_probe getpeername_short / getpeername_null_*: WSAEFAULT (10014),
// namelen unchanged on short buffer.
TEST_P(Ws2ContractTest, GetPeerNameShortOrNullReportsWsaeFault) {
    startup();
    const u64 listener = call("socket", {2, 1, 6});
    const u64 client = call("socket", {2, 1, 6});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {client, sa, 16}), 0u);
    const u64 server = call("accept", {listener, 0, 0});
    ASSERT_NE(server, invalid_socket());

    const GuestAddr name = kData + 0x100;
    (void)mem_.write_bytes(name, std::vector<Byte>(16, 0xCC).data(), 16);
    (void)mem_.write_u32(kData + 0x200, 4);
    EXPECT_EQ(static_cast<u32>(call("getpeername", {client, name, kData + 0x200})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 4u);
    EXPECT_EQ(mem_.read_u16(name).value(), 0xCCCCu);

    (void)mem_.write_u32(kData + 0x200, 16);
    EXPECT_EQ(static_cast<u32>(call("getpeername", {client, 0, kData + 0x200})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
}

// ws2_core_probe getsockname_short / getsockname_null_* / getsockname6_short16:
// WSAEFAULT (10014), namelen unchanged.
TEST_P(Ws2ContractTest, GetSockNameShortOrNullReportsWsaeFault) {
    startup();
    const u64 listener = call("socket", {2, 1, 6});
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);

    const GuestAddr name = kData + 0x100;
    (void)mem_.write_bytes(name, std::vector<Byte>(16, 0xCC).data(), 16);
    (void)mem_.write_u32(kData + 0x200, 4);
    EXPECT_EQ(static_cast<u32>(
                  call("getsockname", {listener, name, kData + 0x200})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 4u);

    (void)mem_.write_u32(kData + 0x200, 16);
    EXPECT_EQ(static_cast<u32>(call("getsockname", {listener, 0, kData + 0x200})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(static_cast<u32>(call("getsockname", {listener, name, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);

    const u64 s6 = call("socket", {23, 1, 6});
    ASSERT_NE(s6, invalid_socket());
    const GuestAddr sa6 = kData + 0x300;
    (void)mem_.write_bytes(sa6, std::vector<Byte>(28, 0).data(), 28);
    (void)mem_.write_u16(sa6, 23);
    ASSERT_EQ(call("bind", {s6, sa6, 28}), 0u);
    (void)mem_.write_u32(kData + 0x200, 16);
    EXPECT_EQ(static_cast<u32>(call("getsockname", {s6, name, kData + 0x200})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 16u);
}

// ws2_core_probe getsockname_unbound: rc=-1 err=10022.
TEST_P(Ws2ContractTest, GetSockNameUnboundReportsWsaeinval) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    (void)mem_.write_u32(kData + 0x40, 16);
    EXPECT_EQ(static_cast<u32>(call("getsockname", {s, kData, kData + 0x40})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(mem_.read_u32(kData + 0x40).value(), 16u);
}

// Learn WSASendDisconnect: shutdown(SD_SEND); TCP/IP rejects disconnect data
// (WSAEOPNOTSUPP). dumpbin @93.
TEST_P(Ws2ContractTest, WsaSendDisconnectShutsSendAndRejectsData) {
    startup();
    const u64 listener = call("socket", {2, 1, 6});
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(listener, invalid_socket());
    ASSERT_NE(s, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, kData + 0x40}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    ASSERT_EQ(call("connect", {s, sa, 16}), 0u);
    EXPECT_EQ(call("WSASendDisconnect", {s, 0}), 0u);
    const GuestAddr wsabuf = kData + 0x100;
    (void)mem_.write_u32(wsabuf, 4);  // len
    if (is64()) (void)mem_.write_u64(wsabuf + 8, kData + 0x200);
    else (void)mem_.write_u32(wsabuf + 4, static_cast<u32>(kData + 0x200));
    EXPECT_EQ(static_cast<u32>(call("WSASendDisconnect", {s, wsabuf})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);  // WSAEOPNOTSUPP
}

// ws2_core_probe connect_nb_again / connect_nb_ok_select (rc=1 w=1 x=0): a
// second connect on a pending nonblocking socket (WSAEALREADY or, once
// complete, WSAEISCONN -- Learn connect) must not turn the successful
// connect into an exceptfds failure.
TEST_P(Ws2ContractTest, SecondConnectOnPendingSocketKeepsSuccess) {
    startup();
    const u64 l = tcp_listener();
    loop4(kData, port_of(l));
    const u64 c = call("socket", {2, 1, 6});
    set_nb(c, 1);
    EXPECT_EQ(rc32(call("connect", {c, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10035u);
    EXPECT_EQ(rc32(call("connect", {c, kData, 16})), 0xFFFFFFFFu);
    const u32 again = wsa_error();
    EXPECT_TRUE(again == 10037u || again == 10056u) << again;
    const GuestAddr wset = kData + 0x200, xset = kData + 0x400;
    const u32 off = is64() ? 8u : 4u;
    (void)mem_.write_u32(wset, 1);
    (void)mem_.write_u32(xset, 1);
    if (is64()) {
        (void)mem_.write_u64(wset + off, c);
        (void)mem_.write_u64(xset + off, c);
    } else {
        (void)mem_.write_u32(wset + off, static_cast<u32>(c));
        (void)mem_.write_u32(xset + off, static_cast<u32>(c));
    }
    (void)mem_.write_u32(kData + 0x600, 3);
    (void)mem_.write_u32(kData + 0x604, 0);
    EXPECT_EQ(call("select", {0, 0, wset, xset, kData + 0x600}), 1u);
    EXPECT_EQ(mem_.read_u32(wset).value(), 1u);
    EXPECT_EQ(mem_.read_u32(xset).value(), 0u);
    EXPECT_EQ(rc32(call("connect", {c, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10056u);                               // connect_nb_after_complete
    (void)mem_.write_u32(kData + 0x40, 4);
    ASSERT_EQ(call("getsockopt", {c, 0xFFFF, 0x1007, kData + 0x80, kData + 0x40}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0x80).value(), 0u);           // SO_ERROR stays 0
}

// ws2_core_probe connect_short_namelen 10014 / connect_bad_family 10047 /
// connect_on_listener 10022 / connect_nb_refused_send 10057; ws2_corex_probe
// tcp4_connect_v6addr 10047, tcp4_connect_unspec 10047,
// tcp4_connect_bad_family_len8 10014, tcp4_connect_null 10014,
// tcp6_connect_v4addr 10014.
TEST_P(Ws2ContractTest, ConnectAddressValidationMatchesOracle) {
    startup();
    const u64 l = tcp_listener();
    loop4(kData, port_of(l));
    const u64 c = call("socket", {2, 1, 6});
    EXPECT_EQ(rc32(call("connect", {c, kData, 8})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    (void)mem_.write_u16(kData, 99);
    EXPECT_EQ(rc32(call("connect", {c, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10047u);
    EXPECT_EQ(rc32(call("connect", {c, kData, 8})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    (void)mem_.write_u16(kData, 0);
    EXPECT_EQ(rc32(call("connect", {c, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10047u);
    EXPECT_EQ(rc32(call("connect", {c, 0, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    (void)mem_.write_bytes(kData + 0x100, std::vector<Byte>(28, 0).data(), 28);
    (void)mem_.write_u16(kData + 0x100, 23);
    (void)mem_.write_u8(kData + 0x100 + 23, 1);                  // ::1
    EXPECT_EQ(rc32(call("connect", {c, kData + 0x100, 28})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10047u);
    loop4(kData, port_of(l));
    const u64 c6 = call("socket", {23, 1, 0});
    EXPECT_EQ(rc32(call("connect", {c6, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("connect", {l, kData, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);

    // A refused nonblocking connect leaves the socket unconnected.
    const u64 dead = bound(1, 6);
    loop4(kData + 0x200, port_of(dead));
    (void)call("closesocket", {dead});
    const u64 r = call("socket", {2, 1, 6});
    set_nb(r, 1);
    EXPECT_EQ(rc32(call("connect", {r, kData + 0x200, 16})), 0xFFFFFFFFu);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    (void)mem_.write_u32(kData + 0x40, 4);
    ASSERT_EQ(call("getsockopt", {r, 0xFFFF, 0x1007, kData + 0x80, kData + 0x40}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0x80).value(), 10061u);      // refused_SO_ERROR
    EXPECT_EQ(rc32(call("send", {r, kData, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
}

// ws2_core_probe udp_connect_zero rc=0, udp_getpeername_after_zero 10057,
// udp_send_after_zero 10057, udp_connect_unspec 10047; ws2_corex_probe
// udp4_connect_zero_unconnected 10049 + udp4_getsockname_after_zero rc=0,
// udp4_local_after_dissolve addr=0x0 port_nonzero=1.
TEST_P(Ws2ContractTest, UdpConnectZeroDissolvesOnlyAnAssociation) {
    startup();
    const u64 peer = bound(2, 17);
    loop4(kData, port_of(peer));
    const u64 u = call("socket", {2, 2, 17});
    (void)mem_.write_bytes(kData + 0x100, std::vector<Byte>(16, 0).data(), 16);
    (void)mem_.write_u16(kData + 0x100, 2);                       // 0.0.0.0:0
    EXPECT_EQ(rc32(call("connect", {u, kData + 0x100, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10049u);
    EXPECT_NE(port_of(u), 0u);                                    // implicit bind
    ASSERT_EQ(call("connect", {u, kData, 16}), 0u);
    EXPECT_EQ(call("connect", {u, kData + 0x100, 16}), 0u);
    (void)mem_.write_u32(kData + 0x40, 16);
    EXPECT_EQ(rc32(call("getpeername", {u, kData + 0x200, kData + 0x40})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
    EXPECT_EQ(rc32(call("send", {u, kData, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
    (void)mem_.write_u16(kData + 0x100, 0);                       // AF_UNSPEC
    EXPECT_EQ(rc32(call("connect", {u, kData + 0x100, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10047u);
}

// ws2_core_probe udp_shutdown_rd rc=0 + udp_recv_after_shutdown 10058,
// udp_shutdown_unconnected rc=0, shutdown_send_again rc=0,
// shutdown_unconnected 10057, udp_send_unconnected 10057; ws2_corex_probe
// listener_shutdown 10057, tcp_shut_both_again rc=0,
// tcp_shut_after_peer_closed(_again) rc=0.
TEST_P(Ws2ContractTest, ShutdownRulesMatchOracle) {
    startup();
    const u64 a = bound(2, 17), b = bound(2, 17);
    loop4(kData, port_of(b));
    EXPECT_EQ(call("shutdown", {b, 0}), 0u);
    (void)put_str(kData + 0x100, "late");
    ASSERT_EQ(call("sendto", {a, kData + 0x100, 4, 0, kData, 16}), 4u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(rc32(call("recv", {b, kData + 0x200, 64, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10058u);
    const u64 u = call("socket", {2, 2, 17});
    EXPECT_EQ(rc32(call("send", {u, kData + 0x100, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
    EXPECT_EQ(call("shutdown", {u, 1}), 0u);
    const u64 t = call("socket", {2, 1, 6});
    EXPECT_EQ(rc32(call("shutdown", {t, 2})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
    const u64 l = tcp_listener();
    EXPECT_EQ(rc32(call("shutdown", {l, 2})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10057u);
    u64 c = 0, s = 0;
    tcp_pair(&c, &s);
    EXPECT_EQ(call("shutdown", {c, 1}), 0u);
    EXPECT_EQ(call("shutdown", {c, 1}), 0u);
    EXPECT_EQ(call("shutdown", {c, 2}), 0u);
    EXPECT_EQ(call("shutdown", {c, 0}), 0u);
    EXPECT_EQ(call("closesocket", {c}), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(call("shutdown", {s, 1}), 0u);
    EXPECT_EQ(call("shutdown", {s, 1}), 0u);
}

// ws2_core_probe tcp_recv_after_shutdown_data 10053: data arriving after
// SD_RECEIVE resets the connection and the receiver reports the local abort.
TEST_P(Ws2ContractTest, StreamDataAfterReceiveShutdownAbortsConnection) {
    startup();
    u64 c = 0, s = 0;
    tcp_pair(&c, &s);
    EXPECT_EQ(call("shutdown", {s, 0}), 0u);
    set_nb(s, 1);
    EXPECT_EQ(rc32(call("recv", {s, kData + 0x200, 64, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10058u);                               // _nodata
    set_nb(s, 0);
    (void)put_str(kData + 0x100, "data");
    EXPECT_EQ(call("send", {c, kData + 0x100, 4, 0}), 4u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(rc32(call("recv", {s, kData + 0x200, 64, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10053u);
    EXPECT_EQ(rc32(call("send", {c, kData + 0x100, 4, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10054u);                               // tcp_peer_send_after_reset
}

// ws2_opts_probe rebind udp/tcp first=0 second=1 10013, rebind_any
// first=0 second=0 rc=0, rebind_any first=0 second=2 10048,
// rebind first=0 second=0 10048.
TEST_P(Ws2ContractTest, AddressSharingRulesMatchOracle) {
    startup();
    for (u64 type : {u64{2}, u64{1}}) {
        const u64 s1 = call("socket", {2, type, 0});
        loop4(kData, 0);
        ASSERT_EQ(call("bind", {s1, kData, 16}), 0u);
        if (type == 1) ASSERT_EQ(call("listen", {s1, 1}), 0u);
        const u16 port = port_of(s1);
        loop4(kData, port);
        const u64 s2 = call("socket", {2, type, 0});
        EXPECT_EQ(rc32(call("bind", {s2, kData, 16})), 0xFFFFFFFFu);
        EXPECT_EQ(wsa_error(), 10048u);
        const u64 s3 = call("socket", {2, type, 0});
        (void)mem_.write_u32(kData + 0x40, 1);
        ASSERT_EQ(call("setsockopt", {s3, 0xFFFF, 4, kData + 0x40, 4}), 0u);
        EXPECT_EQ(rc32(call("bind", {s3, kData, 16})), 0xFFFFFFFFu);
        EXPECT_EQ(wsa_error(), 10013u);
        (void)mem_.write_u32(kData + 4, 0);                        // 0.0.0.0:port
        const u64 s4 = call("socket", {2, type, 0});
        EXPECT_EQ(call("bind", {s4, kData, 16}), 0u);
        const u64 s5 = call("socket", {2, type, 0});
        ASSERT_EQ(call("setsockopt", {s5, 0xFFFF, 0xFFFFFFFBull, kData + 0x40, 4}), 0u);
        EXPECT_EQ(rc32(call("bind", {s5, kData, 16})), 0xFFFFFFFFu);
        EXPECT_EQ(wsa_error(), 10048u);
        for (u64 s : {s1, s2, s3, s4, s5}) (void)call("closesocket", {s});
    }
}

}  // namespace
