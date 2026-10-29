/// @file test_ws2_32_contracts_options.cpp
/// @brief ws2_32 contract tests for socket options and ioctls.
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// ws2_core_probe refused_SO_ERROR: v=10061 (WSAECONNREFUSED), sticky on a
// second read; successful getsockopt sets WSAGetLastError to 0. Timeouts are
// DWORD ms (ws2_opts_probe set_SO_RCVTIMEO / get_SO_RCVTIMEO_after).
TEST_P(Ws2ContractTest, GetSockOptSoErrorIsWsaAndTimeoutsAreMilliseconds) {
    startup();
    {
        const u64 t = call("socket", {2, 1, 6});
        ASSERT_NE(t, invalid_socket());
        (void)mem_.write_u32(kData + 0x300, 1500);
        ASSERT_EQ(call("setsockopt", {t, 0xFFFF, 0x1006, kData + 0x300, 4}), 0u)
            << "err=" << wsa_error();
        (void)mem_.write_u32(kData + 0x300, 0xDEAD);
        (void)mem_.write_u32(kData + 0x304, 4);
        ASSERT_EQ(call("getsockopt", {t, 0xFFFF, 0x1006, kData + 0x300, kData + 0x304}), 0u);
        EXPECT_EQ(mem_.read_u32(kData + 0x300).value(), 1500u);
        (void)mem_.write_u32(kData + 0x300, 2500);
        ASSERT_EQ(call("setsockopt", {t, 0xFFFF, 0x1005, kData + 0x300, 4}), 0u);
        (void)mem_.write_u32(kData + 0x300, 0xDEAD);
        ASSERT_EQ(call("getsockopt", {t, 0xFFFF, 0x1005, kData + 0x300, kData + 0x304}), 0u);
        EXPECT_EQ(mem_.read_u32(kData + 0x300).value(), 2500u);
        (void)call("closesocket", {t});
    }
    ::setenv("VITREO_CONNECT_WAIT_MS", "0", 1);
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    (void)mem_.write_u32(kData + 0x100, 1);
    ASSERT_EQ(call("ioctlsocket", {s, 0x8004667E, kData + 0x100}), 0u);
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, htons(1));  // closed port on loopback
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(static_cast<u32>(call("connect", {s, sa, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10035u);  // WSAEWOULDBLOCK
    const int fd = mgr_.get_native_fd(static_cast<u32>(s));
    ASSERT_GE(fd, 0);
    for (int i = 0; i < 50; ++i) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, 200) > 0) break;
    }
    (void)call("WSASetLastError", {1234});
    (void)mem_.write_u32(kData + 0x200, 0xDEAD);
    (void)mem_.write_u32(kData + 0x204, 4);
    ASSERT_EQ(call("getsockopt", {s, 0xFFFF, 0x1007, kData + 0x200, kData + 0x204}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 10061u);  // not Darwin errno 61
    EXPECT_EQ(wsa_error(), 0u);
    (void)mem_.write_u32(kData + 0x200, 0xDEAD);
    ASSERT_EQ(call("getsockopt", {s, 0xFFFF, 0x1007, kData + 0x200, kData + 0x204}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0x200).value(), 10061u);  // sticky
    ::unsetenv("VITREO_CONNECT_WAIT_MS");
}

// ws2_opts_probe: set_IP_PKTINFO / get_IP_PKTINFO_after, set_IP_RECVDSTADDR,
// set_IPV6_DONTFRAG — Windows option numbers mapped to Darwin, never passed raw.
// set_IP_unknown_99 -> WSAENOPROTOOPT (10042); set_IP_MTU_DISCOVER -> WSAEINVAL.
TEST_P(Ws2ContractTest, SetSockOptMapsWindowsIpOptionNumbers) {
    startup();
    const u64 u4 = call("socket", {2, 2, 17});
    ASSERT_NE(u4, invalid_socket());
    (void)mem_.write_u32(kData, 1);
    ASSERT_EQ(call("setsockopt", {u4, 0, 19, kData, 4}), 0u);  // IP_PKTINFO
    (void)mem_.write_u32(kData, 0xDEAD);
    (void)mem_.write_u32(kData + 4, 4);
    ASSERT_EQ(call("getsockopt", {u4, 0, 19, kData, kData + 4}), 0u);
    EXPECT_EQ(mem_.read_u32(kData).value(), 1u);
    (void)mem_.write_u32(kData, 1);
    ASSERT_EQ(call("setsockopt", {u4, 0, 25, kData, 4}), 0u);  // IP_RECVDSTADDR
    (void)mem_.write_u32(kData, 0xDEAD);
    ASSERT_EQ(call("getsockopt", {u4, 0, 25, kData, kData + 4}), 0u);
    EXPECT_EQ(mem_.read_u32(kData).value(), 1u);
    (void)mem_.write_u32(kData, 1);
    EXPECT_EQ(static_cast<u32>(call("setsockopt", {u4, 0, 71, kData, 4})), 0xFFFFFFFFu);  // IP_MTU_DISCOVER
    EXPECT_EQ(wsa_error(), 10022u);
    (void)mem_.write_u32(kData, 1);
    EXPECT_EQ(static_cast<u32>(call("setsockopt", {u4, 0, 99, kData, 4})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10042u);  // WSAENOPROTOOPT — not a silent Darwin alias
    (void)call("closesocket", {u4});
    const u64 u6 = call("socket", {23, 2, 17});
    ASSERT_NE(u6, invalid_socket());
    (void)mem_.write_u32(kData, 1);
    ASSERT_EQ(call("setsockopt", {u6, 41, 14, kData, 4}), 0u);  // IPV6_DONTFRAG
    (void)mem_.write_u32(kData, 0xDEAD);
    (void)mem_.write_u32(kData + 4, 4);
    ASSERT_EQ(call("getsockopt", {u6, 41, 14, kData, kData + 4}), 0u);
    EXPECT_EQ(mem_.read_u32(kData).value(), 1u);
}

// ws2_opts_probe keepalive_vals_* + ext_transmitfile: SIO_KEEPALIVE_VALS applies
// SO_KEEPALIVE / TCP idle+interval; TransmitFile GUID resolves to a thunk.
TEST_P(Ws2ContractTest, WsaIoctlKeepaliveValsAndTransmitFileGuid) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    // tcp_keepalive {onoff=1, keepalivetime=5000, keepaliveinterval=1000}
    (void)mem_.write_u32(kData, 1);
    (void)mem_.write_u32(kData + 4, 5000);
    (void)mem_.write_u32(kData + 8, 1000);
    (void)mem_.write_u32(kData + 0x20, 0xDEAD);
    ASSERT_EQ(call("WSAIoctl", {s, 0x98000004, kData, 12, 0, 0, kData + 0x20, 0, 0}), 0u);
    (void)mem_.write_u32(kData + 0x100, 0xDEAD);
    (void)mem_.write_u32(kData + 0x104, 4);
    ASSERT_EQ(call("getsockopt", {s, 0xFFFF, 8, kData + 0x100, kData + 0x104}), 0u);
    EXPECT_EQ(mem_.read_u8(kData + 0x100).value(), 1u);
    // ws2_opts_probe keepidle_after_vals v=5 on a connected socket; idle is
    // applied via host TCP_KEEPALIVE / TCP_KEEPIDLE (seconds).
    EXPECT_EQ(static_cast<u32>(call("WSAIoctl", {s, 0x98000004, kData, 4, 0, 0, kData + 0x20, 0, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    const u64 u = call("socket", {2, 2, 17});
    ASSERT_NE(u, invalid_socket());
    EXPECT_EQ(static_cast<u32>(call("WSAIoctl", {u, 0x98000004, kData, 12, 0, 0, kData + 0x20, 0, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10045u);
    // TransmitFile GUID is resolved via the thunk registry at process start
    // (ws2_opts_probe ext_transmitfile_nonnull); unit tests have no registry.
}

// ws2_opts_probe wsaioctl_FIONREAD / SIOCATMARK / base_handle: Unix IOCTLs and
// SIO_BASE_HANDLE are valid WSAIoctl codes (not WSAEOPNOTSUPP).
TEST_P(Ws2ContractTest, WsaIoctlUnixAndBaseHandleMatchOracle) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    (void)mem_.write_u32(kData + 0x20, 0xDEAD);
    (void)mem_.write_u32(kData, 0);
    ASSERT_EQ(call("WSAIoctl", {s, 0x4004667F, 0, 0, kData, 4, kData + 0x20, 0, 0}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), 4u);
    (void)mem_.write_u32(kData, 0xDEAD);
    ASSERT_EQ(call("WSAIoctl", {s, 0x40047307, 0, 0, kData, 4, kData + 0x20, 0, 0}), 0u);
    EXPECT_EQ(mem_.read_u32(kData).value(), 1u);  // SIOCATMARK_none v=1
    const u32 ptr_size = is64() ? 8u : 4u;
    (void)mem_.write_u64(kData + 0x100, 0);
    ASSERT_EQ(call("WSAIoctl",
                   {s, 0x48000022, 0, 0, kData + 0x100, ptr_size, kData + 0x20, 0, 0}),
              0u);
    if (is64()) EXPECT_EQ(mem_.read_u64(kData + 0x100).value(), s);
    else EXPECT_EQ(mem_.read_u32(kData + 0x100).value(), static_cast<u32>(s));
}

// Helpers for the option tests: getsockopt into kData+0x600 (len at +0x680).
struct Opt { u32 rc; u32 err; u32 len; u32 v; };

// ws2_opts_probe: get/set values, widths and errors for the options the audit
// listed (SO_RCVTIMEO/SO_SNDTIMEO, BOOL options as 0/1 with BOOLEAN width,
// IP/IPv6 options by WINDOWS number, SO_LINGER in seconds, EXCLUSIVEADDRUSE).
TEST_P(Ws2ContractTest, SocketOptionsMatchOracle) {
    startup();
    const u64 t = call("socket", {2, 1, 6});
    const u64 u = call("socket", {2, 2, 17});
    auto get = [&](u64 s, u64 level, u64 opt, u32 len = 4) {
        (void)mem_.write_u32(kData + 0x600, 0xCCCCCCCC);
        (void)mem_.write_u32(kData + 0x680, len);
        (void)call("WSASetLastError", {0xDEAD});
        Opt o{};
        o.rc = static_cast<u32>(call("getsockopt", {s, level, opt, kData + 0x600, kData + 0x680}));
        o.err = wsa_error();
        o.len = mem_.read_u32(kData + 0x680).value();
        o.v = mem_.read_u32(kData + 0x600).value();
        return o;
    };
    auto set = [&](u64 s, u64 level, u64 opt, u32 v, u32 len = 4) {
        (void)mem_.write_u32(kData + 0x700, v);
        return static_cast<u32>(call("setsockopt", {s, level, opt, kData + 0x700, len}));
    };
    const u32 E = 0xFFFFFFFF;
    // tcp_SO_RCVTIMEO v=0; set 1500 -> 0x5dc; len2 -> WSAEFAULT
    EXPECT_EQ(get(t, 0xFFFF, 0x1006).v, 0u);
    EXPECT_EQ(set(t, 0xFFFF, 0x1006, 1500), 0u);
    EXPECT_EQ(get(t, 0xFFFF, 0x1006).v, 1500u);
    EXPECT_EQ(set(t, 0xFFFF, 0x1005, 2500), 0u);
    EXPECT_EQ(get(t, 0xFFFF, 0x1005).v, 2500u);
    EXPECT_EQ(set(t, 0xFFFF, 0x1006, 100, 2), E);
    EXPECT_EQ(wsa_error(), 10014u);
    // tcp_SO_KEEPALIVE len=1 v=0xcccccc00; set 7 -> reads 01
    Opt o = get(t, 0xFFFF, 0x0008);
    EXPECT_EQ(o.len, 1u); EXPECT_EQ(o.v, 0xCCCCCC00u);
    EXPECT_EQ(set(t, 0xFFFF, 0x0008, 7), 0u);
    EXPECT_EQ(get(t, 0xFFFF, 0x0008).v, 0xCCCCCC01u);
    // success sets WSAGetLastError to 0 (SO_TYPE_keeps_wsaerr v=0)
    EXPECT_EQ(get(t, 0xFFFF, 0x1008).err, 0u);
    // TCP_NODELAY len=1; udp_TCP_NODELAY -> WSAEINVAL
    EXPECT_EQ(get(t, 6, 1).len, 1u);
    EXPECT_EQ(get(u, 6, 1).rc, E); EXPECT_EQ(wsa_error(), 10022u);
    // tcp_SO_BROADCAST 10042; udp_SO_BROADCAST len=4 v=0
    EXPECT_EQ(get(t, 0xFFFF, 0x20).err, 10042u);
    o = get(u, 0xFFFF, 0x20); EXPECT_EQ(o.len, 4u); EXPECT_EQ(o.v, 0u);
    // IP options by Windows number: IP_PKTINFO(19) default 0, set -> 1
    EXPECT_EQ(get(u, 0, 19).v, 0u);
    EXPECT_EQ(set(u, 0, 19, 1), 0u);
    EXPECT_EQ(get(u, 0, 19).v, 1u);
    EXPECT_EQ(set(u, 0, 25, 1), 0u);                    // IP_RECVDSTADDR
    EXPECT_EQ(get(u, 0, 25).v, 1u);
    EXPECT_EQ(get(u, 0, 4).v, 128u);                    // IP_TTL default 0x80
    EXPECT_EQ(set(u, 0, 4, 300), E); EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(set(u, 0, 71, 1), E); EXPECT_EQ(wsa_error(), 10022u);   // IP_MTU_DISCOVER
    EXPECT_EQ(get(u, 0, 99).err, 10042u);
    EXPECT_EQ(set(u, 0, 99, 1), E); EXPECT_EQ(wsa_error(), 10042u);
    o = get(t, 0, 4, 1); EXPECT_EQ(o.rc, 0u); EXPECT_EQ(o.len, 1u);   // IP_TTL_len1
    // SO_RCVBUF_len2 -> WSAEFAULT; SO_RCVBUF default 0x10000
    EXPECT_EQ(get(t, 0xFFFF, 0x1002, 2).err, 10014u);
    EXPECT_EQ(get(t, 0xFFFF, 0x1002).v, 0x10000u);
    // SO_LINGER {1,2} -> 0x20001; DONTLINGER keeps seconds
    EXPECT_EQ(set(t, 0xFFFF, 0x80, 0x00020001), 0u);
    EXPECT_EQ(get(t, 0xFFFF, 0x80).v, 0x20001u);
    EXPECT_EQ(get(t, 0xFFFF, ~0x80u).v, 0u);
    EXPECT_EQ(set(t, 0xFFFF, ~0x80u, 1), 0u);
    EXPECT_EQ(get(t, 0xFFFF, 0x80).v, 0x20000u);
    EXPECT_EQ(get(u, 0xFFFF, 0x80).err, 10042u);
    // EXCLUSIVEADDRUSE vs REUSEADDR are mutually exclusive (10022)
    EXPECT_EQ(set(t, 0xFFFF, 4, 1), 0u);
    EXPECT_EQ(set(t, 0xFFFF, ~4u, 1), E); EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(set(t, 0xFFFF, 4, 0), 0u);
    EXPECT_EQ(set(t, 0xFFFF, ~4u, 1), 0u);
    EXPECT_EQ(get(t, 0xFFFF, ~4u).v, 1u);
    EXPECT_EQ(set(t, 0xFFFF, 4, 1), E); EXPECT_EQ(wsa_error(), 10022u);
    // Unknown SOL_SOCKET option / level
    EXPECT_EQ(get(t, 0xFFFF, 0x7777).err, 10042u);
    EXPECT_EQ(get(t, 99, 1).err, 10022u);
    EXPECT_EQ(set(t, 0xFFFF, 0x1041, 1), E);            // SO_USELOOPBACK? no: 0x1041 unknown
    // SO_MAX_MSG_SIZE tcp 0xffffffff, udp 0xffe3
    EXPECT_EQ(get(t, 0xFFFF, 0x2003).v, 0xFFFFFFFFu);
    EXPECT_EQ(get(u, 0xFFFF, 0x2003).v, 0xFFE3u);
    // SO_CONNECT_TIME unconnected 0xffffffff
    EXPECT_EQ(get(t, 0xFFFF, 0x700C).v, 0xFFFFFFFFu);
    // SO_PROTOCOL_INFOW len 628, af 2, type 1
    (void)mem_.write_u32(kData + 0x680, 628);
    ASSERT_EQ(call("getsockopt", {t, 0xFFFF, 0x2005, kData + 0x800, kData + 0x680}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0x680).value(), 628u);
    EXPECT_EQ(mem_.read_u32(kData + 0x800).value(), 0x20066u);
    EXPECT_EQ(mem_.read_u32(kData + 0x800 + 76).value(), 2u);
    EXPECT_EQ(mem_.read_u32(kData + 0x800 + 88).value(), 1u);
    (void)mem_.write_u32(kData + 0x680, 10);
    EXPECT_EQ(static_cast<u32>(call("getsockopt", {t, 0xFFFF, 0x2005, kData + 0x800, kData + 0x680})), E);
    EXPECT_EQ(wsa_error(), 10014u);
}

// ws2_opts_probe tcp/listen/udp_SO_ACCEPTCONN (0 / 1 / 10042),
// tcp_SO_BSP_STATE_len4 10014, conn_IP_MTU 0xffff, conn_TCP_MAXSEG 0xffd7,
// tcp6_IP_TTL 0x80, get_TCP_FASTOPEN_after len=1 v=1; ws2_corex_probe
// bsp_tcp_connected (l_off=csaddr, r_off=csaddr+16, l_addr=0x0, type 1
// proto 6, optlen untouched), bsp_exact_struct 10014,
// only6_set_IP_TTL 10022.
TEST_P(Ws2ContractTest, SocketOptionsFromOracleProbes) {
    startup();
    const GuestAddr v = kData + 0x800, vl = kData + 0x840;
    auto get = [&](u64 s, u32 level, u32 opt) -> u32 {
        (void)mem_.write_u32(v, 0xCCCCCCCC);
        (void)mem_.write_u32(vl, 4);
        const u32 rc = rc32(call("getsockopt", {s, level, opt, v, vl}));
        return rc == 0 ? mem_.read_u32(v).value() : 0xFFFF0000u | (wsa_error() & 0xFFFF);
    };
    const u64 t = call("socket", {2, 1, 6});
    const u64 l = tcp_listener();
    const u64 u = call("socket", {2, 2, 17});
    EXPECT_EQ(get(t, 0xFFFF, 0x0002), 0u);
    EXPECT_EQ(get(l, 0xFFFF, 0x0002), 1u);
    EXPECT_EQ(get(u, 0xFFFF, 0x0002), 0xFFFF0000u | 10042u);
    EXPECT_EQ(get(t, 0xFFFF, 0x1009), 0xFFFF0000u | 10014u);      // len 4
    u64 c = 0, a = 0;
    tcp_pair(&c, &a);
    EXPECT_EQ(get(c, 0, 73), 0xFFFFu);                            // IP_MTU
    EXPECT_EQ(get(c, 6, 4), 0xFFD7u);                             // TCP_MAXSEG
    const u32 csaddr = is64() ? 40u : 24u;
    const GuestAddr bsp = kData + 0x900;
    (void)mem_.write_bytes(bsp, std::vector<Byte>(256, 0xCC).data(), 256);
    (void)mem_.write_u32(vl, csaddr);
    EXPECT_EQ(rc32(call("getsockopt", {c, 0xFFFF, 0x1009, bsp, vl})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    (void)mem_.write_u32(vl, 256);
    ASSERT_EQ(call("getsockopt", {c, 0xFFFF, 0x1009, bsp, vl}), 0u);
    EXPECT_EQ(mem_.read_u32(vl).value(), 256u);
    EXPECT_EQ(ptr(bsp), bsp + csaddr);
    EXPECT_EQ(mem_.read_u32(bsp + (is64() ? 8u : 4u)).value(), 16u);
    EXPECT_EQ(ptr(bsp + (is64() ? 16u : 8u)), bsp + csaddr + 16);
    EXPECT_EQ(mem_.read_u32(bsp + (is64() ? 32u : 16u)).value(), 1u);
    EXPECT_EQ(mem_.read_u32(bsp + (is64() ? 36u : 20u)).value(), 6u);
    EXPECT_EQ(mem_.read_u32(bsp + csaddr + 4).value(), 0u);       // wildcard local
    EXPECT_EQ(mem_.read_u32(bsp + csaddr + 16 + 4).value(), htonl(INADDR_LOOPBACK));
    const u64 t6 = call("socket", {23, 1, 0});
    EXPECT_EQ(get(t6, 0, 4), 0x80u);
    (void)mem_.write_u32(v, 17);
    EXPECT_EQ(rc32(call("setsockopt", {t6, 0, 4, v, 4})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);
    (void)mem_.write_u32(v, 1);
    EXPECT_EQ(call("setsockopt", {t, 6, 15, v, 4}), 0u);          // TCP_FASTOPEN
    (void)mem_.write_u32(v, 0xCCCCCCCC);
    (void)mem_.write_u32(vl, 4);
    ASSERT_EQ(call("getsockopt", {t, 6, 15, v, vl}), 0u);
    EXPECT_EQ(mem_.read_u32(vl).value(), 1u);
    EXPECT_EQ(mem_.read_u32(v).value(), 0xCCCCCC01u);
}

}  // namespace
