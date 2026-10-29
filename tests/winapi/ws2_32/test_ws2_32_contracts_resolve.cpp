/// @file test_ws2_32_contracts_resolve.cpp
/// @brief ws2_32 contract tests for name and service resolution (getaddrinfo,
///        getnameinfo, protocol/service/host databases).
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// ws2_resolve_probe gai_empty_node: empty nodename returns local (non-loopback)
// addresses when the host has them.
TEST_P(Ws2ContractTest, GetAddrInfoEmptyNodeReturnsLocalAddresses) {
    startup();
    const GuestAddr node = put_str(kData, "");
    const GuestAddr out = kData + 0x100;
    (void)mem_.write_u64(out, 0);
    EXPECT_EQ(call("getaddrinfo", {node, 0, 0, out}), 0u);
    const GuestAddr root = is64() ? mem_.read_u64(out).value()
                                  : mem_.read_u32(out).value();
    ASSERT_NE(root, 0u);
    // ai_family at +4; ai_addr at +32 (x64) / +24 (x86).
    const u32 fam = mem_.read_u32(root + 4).value();
    EXPECT_TRUE(fam == 2u || fam == 23u);
    call("freeaddrinfo", {root});
}

// ws2_resolve_probe gai_bad_service / gai_canon_null_node: EAI_SERVICE→10109,
// AI_CANONNAME+null node→10022.
TEST_P(Ws2ContractTest, GetAddrInfoErrorMappingMatchesOracle) {
    startup();
    const GuestAddr out = kData + 0x100;
    (void)mem_.write_u64(out, 0);
    const GuestAddr hints = kData + 0x200;
    (void)mem_.write_bytes(hints, std::vector<Byte>(48, 0).data(), 48);
    (void)mem_.write_u32(hints + 0, 0x2);  // AI_CANONNAME
    (void)mem_.write_u32(hints + 4, 2);    // AF_INET
    EXPECT_EQ(call("getaddrinfo", {0, put_str(kData + 0x300, "80"), hints, out}),
              10022u);
    EXPECT_EQ(wsa_error(), 10022u);

    (void)mem_.write_u32(hints + 0, 0);
    (void)mem_.write_u32(hints + 8, 1);  // SOCK_STREAM
    EXPECT_EQ(call("getaddrinfo",
                   {put_str(kData, "127.0.0.1"),
                    put_str(kData + 0x40, "nosuchservicezz"), hints, out}),
              10109u);
    EXPECT_EQ(wsa_error(), 10109u);
}

// ws2_resolve_probe: getprotobyname_tcp name="tcp" proto=6 aliases=[TCP,]
// off_proto=16 (x64) / 8 (x86); getprotobyname_zzz NULL err=11004.
TEST_P(Ws2ContractTest, GetProtoByNameUsesGuestLayoutAndWindowsDatabase) {
    startup();
    const u64 pe = call("getprotobyname", {put_str(kData, "TCP")});
    ASSERT_NE(pe, 0u);
    const u32 w = is64() ? 8 : 4;
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(pe)))), "tcp");
    const GuestAddr aliases = static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(pe + w)));
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(aliases))), "TCP");
    EXPECT_EQ(ptr(aliases + w), 0u);
    EXPECT_EQ(mem_.read_u16(static_cast<GuestAddr>(pe + 2 * w)).value(), 6u);
    // ipv6's Windows alias is "IPv6" (Darwin's /etc/protocols says IPV6).
    const u64 v6 = call("getprotobyname", {put_str(kData, "ipv6")});
    ASSERT_NE(v6, 0u);
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(v6 + w)))))), "IPv6");
    EXPECT_EQ(mem_.read_u16(static_cast<GuestAddr>(v6 + 2 * w)).value(), 41u);
    // One structure per thread: the second call reuses the first's storage.
    EXPECT_EQ(v6, pe);
    (void)call("WSASetLastError", {0xDEAD});
    EXPECT_EQ(call("getprotobyname", {put_str(kData, "zzz")}), 0u);
    EXPECT_EQ(wsa_error(), 11004u);
    (void)call("WSASetLastError", {0xDEAD});
    EXPECT_EQ(call("getprotobynumber", {999}), 0u);
    EXPECT_EQ(wsa_error(), 11004u);
    const u64 udp = call("getprotobynumber", {17});
    ASSERT_NE(udp, 0u);
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(udp)))), "udp");
}

// ws2_resolve_probe: getservbyname_http_tcp name="http" port=80 proto="tcp"
// aliases=[www,www-http,]; getservbyport_53_null -> domain/tcp; misses 11004.
// SERVENT swaps s_proto/s_port on _WIN64 (winsock2.h).
TEST_P(Ws2ContractTest, GetServEntriesUseGuestLayoutAndWindowsDatabase) {
    startup();
    const u32 w = is64() ? 8 : 4;
    const GuestAddr port_off = is64() ? 24 : 8;
    const GuestAddr proto_off = is64() ? 16 : 12;
    const u64 se = call("getservbyname", {put_str(kData, "http"), 0});
    ASSERT_NE(se, 0u);
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(se)))), "http");
    EXPECT_EQ(ntohs(mem_.read_u16(static_cast<GuestAddr>(se + port_off)).value()), 80);
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(se + proto_off)))), "tcp");
    const GuestAddr aliases = static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(se + w)));
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(aliases))), "www");
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(aliases + w))), "www-http");
    EXPECT_EQ(ptr(aliases + 2 * w), 0u);
    const u64 dom = call("getservbyport", {htons(53), 0});
    ASSERT_NE(dom, 0u);
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(dom)))), "domain");
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(dom + proto_off)))), "tcp");
    const u64 udom = call("getservbyname", {put_str(kData, "domain"), put_str(kData + 0x40, "udp")});
    ASSERT_NE(udom, 0u);
    EXPECT_EQ(str(static_cast<GuestAddr>(ptr(static_cast<GuestAddr>(udom + proto_off)))), "udp");
    EXPECT_EQ(call("getservbyport", {htons(1), put_str(kData, "tcp")}), 0u);
    EXPECT_EQ(wsa_error(), 11004u);
    EXPECT_EQ(call("getservbyname", {put_str(kData, "zzz"), put_str(kData + 0x40, "tcp")}), 0u);
    EXPECT_EQ(wsa_error(), 11004u);
}

// ws2_resolve_probe: gethostname returns GetComputerNameEx(DnsHostname) and
// fails WSAEFAULT unless namelen > strlen(name); the name resolves.
TEST_P(Ws2ContractTest, GetHostNameIsMachineNameAndResolves) {
    startup();
    const std::string host = mgr_.host_name();  // "VITREO" without a provider
    const u64 n = host.size();
    EXPECT_EQ(call("gethostname", {kData, n + 1}), 0u);
    EXPECT_EQ(str(kData), host);
    EXPECT_EQ(static_cast<u32>(call("gethostname", {kData, n})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(static_cast<u32>(call("gethostname", {0, 256})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(static_cast<u32>(call("gethostname", {kData, 0xFFFFFFFF})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(static_cast<u32>(call("GetHostNameW", {kData, 1})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(call("GetHostNameW", {kData, 256}), 0u);
    EXPECT_EQ(mem_.read_u16(kData).value(), static_cast<u16>(host[0]));
    // gethostname_resolves / gethostbyname_own: rc=0, non-NULL, AF_INET.
    put_str(kData + 0x100, "vitreo");
    EXPECT_EQ(call("getaddrinfo", {kData + 0x100, 0, 0, kData + 0x200}), 0u);
    EXPECT_NE(ptr(kData + 0x200), 0u);
    const u64 he = call("gethostbyname", {kData + 0x100});
    ASSERT_NE(he, 0u);
    EXPECT_EQ(mem_.read_u16(static_cast<GuestAddr>(he + (is64() ? 16 : 8))).value(), 2u);
}

// Learn getaddrinfo/freeaddrinfo: results are independently heap-allocated;
// keeping one live must not exhaust a fixed bump arena.
TEST_P(Ws2ContractTest, GetAddrInfoDoesNotExhaustWhenOneResultKeptAlive) {
    startup();
    put_str(kData + 0x100, "127.0.0.1");
    EXPECT_EQ(call("getaddrinfo", {kData + 0x100, 0, 0, kData + 0x200}), 0u);
    const GuestAddr kept = static_cast<GuestAddr>(ptr(kData + 0x200));
    ASSERT_NE(kept, 0u);
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(call("getaddrinfo", {kData + 0x100, 0, 0, kData + 0x280}), 0u)
            << "iter " << i;
        const GuestAddr root = static_cast<GuestAddr>(ptr(kData + 0x280));
        ASSERT_NE(root, 0u) << "iter " << i;
        ASSERT_NE(root, kept) << "iter " << i;
        (void)call("freeaddrinfo", {root});
    }
    (void)call("freeaddrinfo", {kept});
}

// ws2_resolve_probe gni_*: numeric/service formatting, error returns (rc ==
// WSAGetLastError, 0 on success), ERROR_INSUFFICIENT_BUFFER partial copies.
TEST_P(Ws2ContractTest, GetNameInfoMatchesOracle) {
    startup();
    const GuestAddr sa = kData, host = kData + 0x100, serv = kData + 0x300;
    auto sin = [&](u16 port) {
        (void)mem_.write_u16(sa, 2);
        (void)mem_.write_u16(sa + 2, htons(port));
        (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
        for (int i = 8; i < 16; ++i) (void)mem_.write_u8(sa + i, 0);
    };
    auto unset = [&]() { put_str(host, "unset"); put_str(serv, "unset"); };
    auto gni = [&](u64 salen, u64 hl, u64 sl, u64 flags) {
        (void)call("WSASetLastError", {0xDEAD});
        return static_cast<u32>(call("getnameinfo", {sa, salen, hl ? host : 0, hl, sl ? serv : 0, sl, flags}));
    };
    sin(80); unset();
    EXPECT_EQ(gni(16, 1025, 32, 2 | 8), 0u);
    EXPECT_EQ(wsa_error(), 0u);
    EXPECT_EQ(str(host), "127.0.0.1");
    EXPECT_EQ(str(serv), "80");
    EXPECT_EQ(gni(16, 1025, 32, 2), 0u);
    EXPECT_EQ(str(serv), "http");
    sin(53);
    EXPECT_EQ(gni(16, 0, 32, 0x10), 0u);
    EXPECT_EQ(str(serv), "domain");
    sin(61234);
    EXPECT_EQ(gni(16, 0, 32, 0), 0u);
    EXPECT_EQ(str(serv), "61234");
    sin(80); unset();
    EXPECT_EQ(gni(8, 1025, 32, 2 | 8), 10014u);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(str(host), "unset");
    EXPECT_EQ(gni(64, 1025, 32, 2 | 8), 0u);
    (void)mem_.write_u16(sa, 99);
    EXPECT_EQ(gni(16, 1025, 32, 2), 10047u);
    sin(80); unset();
    EXPECT_EQ(gni(16, 4, 32, 2 | 8), 122u);            // gni_hostlen_small
    EXPECT_EQ(wsa_error(), 122u);
    EXPECT_EQ(str(host), "127.t");
    EXPECT_EQ(str(serv), "unset");
    unset();
    EXPECT_EQ(gni(16, 1025, 2, 2 | 8), 122u);          // gni_servlen_small
    EXPECT_EQ(str(host), "127.0.0.1");
    EXPECT_EQ(str(serv), "80set");
    unset();
    EXPECT_EQ(gni(16, 1025, 3, 2), 122u);              // gni_servname_small
    EXPECT_EQ(str(serv), "httet");
    EXPECT_EQ(static_cast<u32>(call("getnameinfo", {0, 16, host, 1025, serv, 32, 2})), 10014u);
    EXPECT_EQ(gni(16, 1025, 32, 0x10000 | 2), 0u);     // unknown flags ignored
    // gni_v6_linklocal_scope: host="fe80::1%3"; gni_v6_short_salen: 10014.
    for (int i = 0; i < 28; ++i) (void)mem_.write_u8(sa + i, 0);
    (void)mem_.write_u16(sa, 23);
    (void)mem_.write_u8(sa + 8, 0xfe);
    (void)mem_.write_u8(sa + 9, 0x80);
    (void)mem_.write_u8(sa + 23, 1);
    (void)mem_.write_u32(sa + 24, 3);
    EXPECT_EQ(gni(28, 1025, 0, 2), 0u);
    EXPECT_EQ(str(host), "fe80::1%3");
    EXPECT_EQ(gni(16, 1025, 0, 2), 10014u);
}

// Learn WSAAsyncGetHostByName / WSACancelAsyncRequest: nonzero HANDLE, packed
// HOSTENT in buf, PostMessage(wParam=handle, HIWORD(lParam)=error);
// cancel after completion -> WSAEINVAL. Ordinals 103 / 108.
TEST_P(Ws2ContractTest, WsaAsyncGetHostByNamePostsHostentAndCancel) {
    startup();
    u64 posted_hwnd = 0, posted_wparam = 0, posted_lparam = 0;
    u32 posted_msg = 0;
    int posts = 0;
    mgr_.set_post_message([&](u64 hwnd, u32 msg, u64 wp, u64 lp) {
        posted_hwnd = hwnd;
        posted_msg = msg;
        posted_wparam = wp;
        posted_lparam = lp;
        ++posts;
        return true;
    });
    const GuestAddr name = put_str(kData, "127.0.0.1");
    const GuestAddr buf = kData + 0x100;
    constexpr u32 kMaxGetHost = 1024;
    (void)mem_.write_bytes(buf, std::vector<Byte>(kMaxGetHost, 0).data(), kMaxGetHost);
    const u64 handle =
        call("WSAAsyncGetHostByName", {0x42, 0x400, name, buf, kMaxGetHost});
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(posts, 1);
    EXPECT_EQ(posted_hwnd, 0x42u);
    EXPECT_EQ(posted_msg, 0x400u);
    EXPECT_EQ(posted_wparam, handle);
    EXPECT_EQ((posted_lparam >> 16) & 0xFFFF, 0u);  // WSAGETASYNCERROR
    const GuestAddr h_name = static_cast<GuestAddr>(ptr(buf));
    EXPECT_EQ(str(h_name), "127.0.0.1");
    const u16 addrtype =
        mem_.read_u16(buf + (is64() ? 16u : 8u)).value();
    EXPECT_EQ(addrtype, 2u);  // AF_INET
    EXPECT_EQ(static_cast<u32>(call("WSACancelAsyncRequest", {handle})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);  // WSAEINVAL — already completed
    EXPECT_EQ(static_cast<u32>(call("WSACancelAsyncRequest", {0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);
}

// ws2_resolve_probe gai_* and ws2_corex_probe gx_*: one entry per address
// with the hints' socktype/protocol, AI_NUMERICHOST in ai_flags for a
// numeric node, WSAGetLastError 0 on success.
TEST_P(Ws2ContractTest, GetAddrInfoShapesResultsLikeWindows) {
    startup();
    const GuestAddr out = kData + 0x100, hints = kData + 0x200;
    const u32 off_addr = is64() ? 32u : 24u, off_next = is64() ? 40u : 28u;
    auto gai = [&](const char* node, const char* serv, u32 flags, u32 fam, u32 type,
                   u32 proto) -> u32 {
        (void)mem_.write_bytes(hints, std::vector<Byte>(48, 0).data(), 48);
        (void)mem_.write_u32(hints + 0, flags);
        (void)mem_.write_u32(hints + 4, fam);
        (void)mem_.write_u32(hints + 8, type);
        (void)mem_.write_u32(hints + 12, proto);
        (void)mem_.write_u64(out, 0);
        (void)call("WSASetLastError", {0xDEAD});
        return static_cast<u32>(call("getaddrinfo",
                                     {node ? put_str(kData + 0x400, node) : 0,
                                      serv ? put_str(kData + 0x500, serv) : 0, hints, out}));
    };
    auto count = [&](GuestAddr root) {
        u32 n = 0;
        for (GuestAddr p = root; p != 0; p = ptr(p + off_next)) ++n;
        return n;
    };
    // gai_numeric_v4: {4,2,0,0,16,-,0x7f000001:0} n=1 wsaerr=0
    ASSERT_EQ(gai("127.0.0.1", nullptr, 0, 0, 0, 0), 0u);
    EXPECT_EQ(wsa_error(), 0u);
    GuestAddr r = ptr(out);
    EXPECT_EQ(count(r), 1u);
    EXPECT_EQ(mem_.read_u32(r + 0).value(), 4u);
    EXPECT_EQ(mem_.read_u32(r + 4).value(), 2u);
    EXPECT_EQ(mem_.read_u32(r + 8).value(), 0u);
    EXPECT_EQ(mem_.read_u32(r + 12).value(), 0u);
    (void)call("freeaddrinfo", {r});
    // gai_numeric_v4_stream {4,2,1,0,...:80}; gai_service_num_big :4464
    ASSERT_EQ(gai("127.0.0.1", "70000", 0, 2, 1, 0), 0u);
    r = ptr(out);
    EXPECT_EQ(mem_.read_u32(r + 8).value(), 1u);
    EXPECT_EQ(mem_.read_u32(r + 12).value(), 0u);
    EXPECT_EQ(ntohs(mem_.read_u16(ptr(r + off_addr) + 2).value()), 4464u);
    (void)call("freeaddrinfo", {r});
    // gx_localhost_unspec: ::1 then 127.0.0.1, flags 0
    ASSERT_EQ(gai("localhost", nullptr, 0, 0, 0, 0), 0u);
    r = ptr(out);
    EXPECT_EQ(count(r), 2u);
    EXPECT_EQ(mem_.read_u32(r + 0).value(), 0u);
    EXPECT_EQ(mem_.read_u32(r + 4).value(), 23u);
    EXPECT_EQ(mem_.read_u32(ptr(r + off_next) + 4).value(), 2u);
    (void)call("freeaddrinfo", {r});
    // gx_serv_http_socktype0 {4,2,1,0,...:80}
    ASSERT_EQ(gai("127.0.0.1", "http", 0, 2, 0, 0), 0u);
    r = ptr(out);
    EXPECT_EQ(mem_.read_u32(r + 8).value(), 1u);
    EXPECT_EQ(ntohs(mem_.read_u16(ptr(r + off_addr) + 2).value()), 80u);
    (void)call("freeaddrinfo", {r});
    EXPECT_EQ(gai("127.1", nullptr, 0, 0, 0, 0), 11001u);          // gx_v4_short_form
    EXPECT_EQ(gai("127.0.0.1", nullptr, 0, 2, 99, 0), 10044u);     // gai_bad_socktype
    EXPECT_EQ(gai("127.0.0.1", "http", 0, 2, 2, 0), 10109u);       // gai_dgram_service_http
    EXPECT_EQ(gai("127.0.0.1", "http", 8, 2, 1, 0), 11001u);       // gai_numericserv_name
    EXPECT_EQ(gai("127.0.0.1", nullptr, 0, 23, 0, 0), 11001u);     // gx_v4_with_v6_hint
    EXPECT_EQ(wsa_error(), 11001u);
}

// ws2_resolve_probe GetNameInfoW_numeric: rc=0 host="127.0.0.1" serv="80".
TEST_P(Ws2ContractTest, GetNameInfoWWritesWideStrings) {
    startup();
    loop4(kData, 80);
    const GuestAddr host = kData + 0x100, serv = kData + 0x400;
    EXPECT_EQ(call("GetNameInfoW", {kData, 16, host, 64, serv, 16, 0x2 | 0x8}), 0u);
    EXPECT_EQ(wsa_error(), 0u);
    const char want[] = "127.0.0.1";
    for (u32 i = 0; i <= 9; ++i) {
        EXPECT_EQ(mem_.read_u16(host + 2 * i).value(), static_cast<u16>(want[i])) << i;
    }
    EXPECT_EQ(mem_.read_u16(serv).value(), u16{'8'});
    EXPECT_EQ(mem_.read_u16(serv + 2).value(), u16{'0'});
    EXPECT_EQ(mem_.read_u16(serv + 4).value(), 0u);
}

}  // namespace
