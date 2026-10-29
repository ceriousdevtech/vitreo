/// @file test_ws2_32_contracts.cpp
/// @brief ws2_32 contract tests for WSAStartup/WSACleanup lifecycle, socket
///        creation and provider selection; also instantiates the Bitness suite.
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// Learn WSAStartup: negotiate wVersion, reject <1.0, WSAEFAULT for NULL
// lpWSAData; WSACleanup refcounts and fails WSANOTINITIALISED when unmatched.
TEST_P(Ws2ContractTest, WsaStartupNegotiatesVersionAndCleanupRefcounts) {
    EXPECT_EQ(call("WSAStartup", {0x0000, kData + 0x8000}), 10092u);  // WSAVERNOTSUPPORTED
    EXPECT_EQ(call("WSAStartup", {0x0202, 0}), 10014u);  // WSAEFAULT
    EXPECT_EQ(call("WSAStartup", {0x0202, kData + 0x8000}), 0u);
    EXPECT_EQ(mem_.read_u16(kData + 0x8000).value(), 0x0202u);
    EXPECT_EQ(mem_.read_u16(kData + 0x8002).value(), 0x0202u);
    const u32 desc = is64() ? 16u : 4u;
    EXPECT_EQ(str(kData + 0x8000 + desc), "WinSock 2.0");
    EXPECT_EQ(call("WSAStartup", {0x0303, kData + 0x9000}), 0u);
    EXPECT_EQ(mem_.read_u16(kData + 0x9000).value(), 0x0202u);  // not 3.3
    EXPECT_EQ(call("WSACleanup", {}), 0u);
    EXPECT_EQ(call("WSACleanup", {}), 0u);
    EXPECT_EQ(static_cast<u32>(call("WSACleanup", {})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10093u);  // WSANOTINITIALISED
    // ws2_core_probe pre_socket / rc_socket_after_clean2: INVALID err=10093
    EXPECT_EQ(call("socket", {2, 1, 6}), invalid_socket());
    EXPECT_EQ(wsa_error(), 10093u);
}

// ws2_core_probe: socket_af999 "INVALID raw=0xffffffffffffffff err=10047" on
// x64 and "raw=0xffffffff" on x86 -- INVALID_SOCKET is (SOCKET)~0.
TEST_P(Ws2ContractTest, SocketFailureReturnsFullWidthInvalidSocket) {
    startup();
    EXPECT_EQ(call("socket", {999, 1, 0}), invalid_socket());
    EXPECT_EQ(wsa_error(), 10047u);  // WSAEAFNOSUPPORT
    EXPECT_EQ(call("WSASocketW", {999, 1, 0, 0, 0, 0}), invalid_socket());
}

// ws2_core_probe socket_af_*: reject Windows-only families (AF_IPX=6,
// AF_BTH=32) with WSAEAFNOSUPPORT; AF_UNIX stream ok + sockaddr_un remap;
// AF_UNSPEC stream/0 -> WSAEINVAL; AF_UNIX dgram -> WSAEAFNOSUPPORT.
TEST_P(Ws2ContractTest, SocketAddressFamilyTranslationMatchesOracle) {
    startup();
    EXPECT_EQ(call("socket", {6, 2, 0}), invalid_socket());  // AF_IPX
    EXPECT_EQ(wsa_error(), 10047u);
    EXPECT_EQ(call("socket", {32, 1, 0}), invalid_socket());  // AF_BTH
    EXPECT_EQ(wsa_error(), 10047u);
    EXPECT_EQ(call("socket", {0, 1, 0}), invalid_socket());  // AF_UNSPEC
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_NE(call("socket", {0, 1, 6}), invalid_socket());  // UNSPEC+TCP
    EXPECT_EQ(call("socket", {1, 2, 0}), invalid_socket());  // AF_UNIX dgram
    EXPECT_EQ(wsa_error(), 10047u);
    const u64 u = call("socket", {1, 1, 0});  // AF_UNIX stream
    ASSERT_NE(u, invalid_socket());
    // Windows sockaddr_un: family@0, path@2 (no sun_len). Bind abstract path.
    const GuestAddr sa = kData + 0x300;
    (void)mem_.write_u16(sa, 1);
    const char* path = "/tmp/vitreo-ws2-afunix-test";
    (void)mem_.write_bytes(sa + 2, reinterpret_cast<const Byte*>(path),
                           std::strlen(path) + 1);
    const u32 sun_len = 2u + static_cast<u32>(std::strlen(path) + 1);
    ::unlink(path);
    EXPECT_EQ(call("bind", {u, sa, sun_len}), 0u);
    (void)mem_.write_u32(kData + 0x3F0, 128);
    EXPECT_EQ(call("getsockname", {u, kData + 0x380, kData + 0x3F0}), 0u);
    EXPECT_EQ(mem_.read_u16(kData + 0x380).value(), 1u);
    EXPECT_EQ(str(kData + 0x382), path);
    ::unlink(path);
}

// ws2_core_probe WSASocketW_*: FROM_PROTOCOL_INFO, multipoint/bad flags
// (WSAEPROTOTYPE 10041), all-minus-1 without info (WSAESOCKTNOSUPPORT 10044).
TEST_P(Ws2ContractTest, WsaSocketProtocolInfoAndFlagsMatchOracle) {
    startup();
    constexpr u64 kFromInfo = 0xFFFFFFFFull;  // FROM_PROTOCOL_INFO as DWORD
    EXPECT_EQ(call("WSASocketW", {kFromInfo, kFromInfo, kFromInfo, 0, 0, 0}),
              invalid_socket());
    EXPECT_EQ(wsa_error(), 10044u);
    EXPECT_EQ(call("WSASocketW", {2, 1, 0, 0, 0, 0x10000}), invalid_socket());
    EXPECT_EQ(wsa_error(), 10041u);
    EXPECT_EQ(call("WSASocketW", {2, 1, 0, 0, 0, 0x02}), invalid_socket());  // multipoint
    EXPECT_EQ(wsa_error(), 10041u);
    EXPECT_NE(call("WSASocketW", {2, 1, 0, 0, 1, 0}), invalid_socket());  // group
    EXPECT_NE(call("WSASocketW", {2, 1, 0, 0, 0, 0x81}), invalid_socket());  // overlapped|no_inherit
    const GuestAddr info = kData + 0x400;
    // Minimal WSAPROTOCOL_INFOW fields used by FROM_PROTOCOL_INFO.
    (void)mem_.write_u32(info + 76, 2);   // af
    (void)mem_.write_u32(info + 88, 1);   // type
    (void)mem_.write_u32(info + 92, 6);   // protocol
    const u64 s = call("WSASocketW",
                       {kFromInfo, kFromInfo, kFromInfo, info, 0, 1});
    ASSERT_NE(s, invalid_socket());
}

// ws2_core_probe inet6_v6only_default: "rc=0 v=1 len=4" -- Windows AF_INET6
// sockets are IPv6-only by default (Learn, IPPROTO_IPV6 socket options).
TEST_P(Ws2ContractTest, Inet6SocketDefaultsToV6Only) {
    startup();
    const u64 s = call("socket", {23, 1, 6});
    ASSERT_NE(s, invalid_socket());
    (void)mem_.write_u32(kData, 0xDEAD);
    (void)mem_.write_u32(kData + 4, 4);
    ASSERT_EQ(call("getsockopt", {s, 41, 27, kData, kData + 4}), 0u);
    EXPECT_EQ(mem_.read_u32(kData).value(), 1u);
    EXPECT_EQ(mem_.read_u32(kData + 4).value(), 4u);
}

// ws2_core_probe SetLastError_then_WSAGetLastError: v=1234 — shared TEB slot.
TEST_P(Ws2ContractTest, WsaGetLastErrorSharesKernel32LastError) {
    startup();
    vitreo::winapi::set_last_error(1234);
    EXPECT_EQ(wsa_error(), 1234u);
    (void)call("WSASetLastError", {4321});
    EXPECT_EQ(vitreo::winapi::get_last_error(), 4321u);
}

// ---------------------------------------------------------------------------
// ws2_core_probe / ws2_corex_probe / ws2_opts_probe / ws2_resolve_probe
// divergences fixed 2026-09-30 (impl_ws2core). Expected values are copied
// from the named .expected lines.
// ---------------------------------------------------------------------------

// ws2_core_probe WSAStartup: req=0x0 rc=10092 wVersion=0x202
// wHighVersion=0x202 iMaxSockets=0 iMaxUdpDg=0 vendor_null=0 desc="WinSock 2.0"
// status="Running" desc_tail=204; req=0x101 iMaxSockets=32767 iMaxUdpDg=65467;
// req=0x202 iMaxSockets=0 iMaxUdpDg=0.
TEST_P(Ws2ContractTest, WsaStartupFillsWsadataLikeWindows) {
    const GuestAddr d = kData + 0xA000;
    const u32 desc = is64() ? 16u : 4u;
    const u32 status = desc + 257u;
    const u32 max_sockets = is64() ? 4u : 390u;
    const u32 max_udp = is64() ? 6u : 392u;
    const u32 vendor = is64() ? 8u : 396u;
    auto poison = [&] { (void)mem_.write_bytes(d, std::vector<Byte>(408, 0xCC).data(), 408); };
    poison();
    EXPECT_EQ(call("WSAStartup", {0x0000, d}), 10092u);
    EXPECT_EQ(mem_.read_u16(d).value(), 0x0202u);
    EXPECT_EQ(mem_.read_u16(d + 2).value(), 0x0202u);
    EXPECT_EQ(mem_.read_u16(d + max_sockets).value(), 0u);
    EXPECT_EQ(mem_.read_u16(d + max_udp).value(), 0u);
    EXPECT_EQ(mem_.read_u32(d + vendor).value(), 0xCCCCCCCCu);  // vendor_null=0
    EXPECT_EQ(str(d + desc), "WinSock 2.0");
    EXPECT_EQ(str(d + status), "Running");
    EXPECT_EQ(mem_.read_u8(d + desc + 12).value(), 0xCCu);       // desc_tail=204
    // A refused version is not a startup (cleanup_after_null pattern).
    EXPECT_EQ(static_cast<u32>(call("WSACleanup", {})), 0xFFFFFFFFu);
    poison();
    EXPECT_EQ(call("WSAStartup", {0x0101, d}), 0u);
    EXPECT_EQ(mem_.read_u16(d).value(), 0x0101u);
    EXPECT_EQ(mem_.read_u16(d + max_sockets).value(), 32767u);
    EXPECT_EQ(mem_.read_u16(d + max_udp).value(), 65467u);
    EXPECT_EQ(mem_.read_u32(d + vendor).value(), 0xCCCCCCCCu);
    poison();
    EXPECT_EQ(call("WSAStartup", {0x0201, d}), 0u);               // 1.2 -> 1.1
    EXPECT_EQ(mem_.read_u16(d).value(), 0x0101u);
    EXPECT_EQ(mem_.read_u16(d + max_sockets).value(), 32767u);
    poison();
    EXPECT_EQ(call("WSAStartup", {0x0202, d}), 0u);
    EXPECT_EQ(mem_.read_u16(d + max_sockets).value(), 0u);
    EXPECT_EQ(mem_.read_u16(d + max_udp).value(), 0u);
    EXPECT_EQ(mem_.read_u8(d + status + 8).value(), 0xCCu);
}

// ws2_core_probe stale_closesocket_after_restart: rc=-1 err=10038 -- the final
// WSACleanup deallocated the socket as closesocket would (Learn WSACleanup).
TEST_P(Ws2ContractTest, FinalWsaCleanupDeallocatesSockets) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    EXPECT_EQ(call("WSACleanup", {}), 0u);
    EXPECT_EQ(rc32(call("closesocket", {s})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10093u);                               // stale_closesocket
    startup();
    EXPECT_EQ(rc32(call("closesocket", {s})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10038u);
}

// ws2_corex_probe cleanup_pending_recv 997, cleanup_with_pending rc=0,
// cleanup_pending_event wait=0, cleanup_pending_internal 0xc000020d
// (STATUS_CONNECTION_RESET: WSACleanup resets, closesocket aborts),
// cleanup_after_restart_send 10038.
TEST_P(Ws2ContractTest, FinalWsaCleanupResetsPendingStreamIo) {
    startup();
    u32 seen_nt = 0;
    GuestAddr seen_ov = 0;
    mgr_.set_overlapped_completion_callback(
        [&](Handle, u32, GuestAddr ov, bool, u32 nt, u32, bool) {
            seen_ov = ov;
            seen_nt = nt;
        });
    u64 c = 0, a = 0;
    tcp_pair(&c, &a);
    const GuestAddr ov = kData + 0x100, buf = kData + 0x200, wsabuf = kData + 0x300;
    (void)mem_.write_bytes(ov, std::vector<Byte>(32, 0).data(), 32);
    (void)mem_.write_u32(wsabuf, 64);
    if (is64()) (void)mem_.write_u64(wsabuf + 8, buf);
    else (void)mem_.write_u32(wsabuf + 4, static_cast<u32>(buf));
    (void)mem_.write_u32(kData + 0x404, 0);
    EXPECT_EQ(rc32(call("WSARecv", {a, wsabuf, 1, kData + 0x400, kData + 0x404, ov, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 997u);
    EXPECT_EQ(call("WSACleanup", {}), 0u);
    EXPECT_EQ(seen_ov, ov);
    EXPECT_EQ(seen_nt, 0xC000020Du);
    EXPECT_EQ(is64() ? mem_.read_u64(ov).value() : u64{mem_.read_u32(ov).value()},
              0xC000020Du);
    mgr_.clear_overlapped_completion_callback();
    startup();
    EXPECT_EQ(rc32(call("send", {c, kData, 1, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10038u);
}

// ws2_core_probe socket_type99 10044 / socket_stream_udp 10043 /
// socket_dgram_tcp 10043; ws2_corex_probe sock:* (provider matrix).
TEST_P(Ws2ContractTest, SocketProviderSelectionMatchesOracle) {
    startup();
    struct Case { u64 af, type, proto; u32 err; };
    const Case bad[] = {
        {2, 99, 0, 10044}, {2, 1, 17, 10043}, {2, 2, 6, 10043}, {2, 4, 0, 10044},
        {2, 5, 6, 10044}, {2, 0, 256, 10043}, {2, 3, 256, 10043}, {23, 1, 17, 10043},
        {23, 4, 0, 10044}, {0, 0, 0, 10022}, {0, 4, 0, 10022}, {0, 1, 17, 10043},
        {0, 4, 6, 10044}, {0, 2, 6, 10043}, {1, 2, 0, 10047}, {6, 2, 0, 10047},
        {2, 0, 0xFFFFFFFFull, 10043},
    };
    for (const auto& c : bad) {
        EXPECT_EQ(call("socket", {c.af, c.type, c.proto}), invalid_socket())
            << c.af << "," << c.type << "," << c.proto;
        EXPECT_EQ(wsa_error(), c.err) << c.af << "," << c.type << "," << c.proto;
    }
    // Type 0 selects by protocol: sock af=2 type=0 proto=17 so_type=2 and
    // af=0 type=0 proto=17 -> the IPv6 provider (iaf=23).
    struct Good { u64 af, type, proto; u32 so_type; u16 family; };
    const Good good[] = {{2, 0, 0, 1, 2}, {2, 0, 17, 2, 2}, {0, 0, 6, 1, 23},
                         {0, 2, 17, 2, 23}, {23, 0, 17, 2, 23}};
    for (const auto& g : good) {
        const u64 s = call("socket", {g.af, g.type, g.proto});
        ASSERT_NE(s, invalid_socket()) << g.af << "," << g.type << "," << g.proto;
        (void)mem_.write_u32(kData + 0x40, 4);
        ASSERT_EQ(call("getsockopt", {s, 0xFFFF, 0x1008, kData, kData + 0x40}), 0u);
        EXPECT_EQ(mem_.read_u32(kData).value(), g.so_type);
        (void)mem_.write_u32(kData + 0x40, 628);
        ASSERT_EQ(call("getsockopt", {s, 0xFFFF, 0x2005, kData + 0x100, kData + 0x40}), 0u);
        EXPECT_EQ(mem_.read_u32(kData + 0x100 + 76).value(), g.family);  // iAddressFamily
        (void)call("closesocket", {s});
    }
}

// ws2_core_probe WSADuplicateSocketW_self rc=0, WSASocketW_dup valid,
// dup_send_recv sent=3 got=3, WSADuplicateSocketW_badpid 10022,
// WSADuplicateSocketW_null 10014; ws2_corex_probe dup_info (resv_nonzero=1,
// af=2 type=1 proto=6), dup_second_use valid, dup_send_after_orig_closed rc=1,
// dup_bogus_socket 10038, from_info_forged_reserved 10022, dupA_*.
TEST_P(Ws2ContractTest, WsaDuplicateSocketWithinProcess) {
    startup();
    u64 c = 0, a = 0;
    tcp_pair(&c, &a);
    const GuestAddr info = kData + 0x1000;
    const u64 pid = static_cast<u64>(::getpid());
    ASSERT_EQ(call("WSADuplicateSocketW", {c, pid, info}), 0u);
    EXPECT_EQ(mem_.read_u32(info + 76).value(), 2u);
    EXPECT_EQ(mem_.read_u32(info + 88).value(), 1u);
    EXPECT_EQ(mem_.read_u32(info + 92).value(), 6u);
    EXPECT_EQ(mem_.read_u32(info + 0).value(), 0x20066u);
    EXPECT_NE(mem_.read_u32(info + 112).value(), 0u);
    const u64 fpi = is64() ? ~u64{0} : 0xFFFFFFFFull;
    const u64 d1 = call("WSASocketW", {fpi, fpi, fpi, info, 0, 0});
    ASSERT_NE(d1, invalid_socket());
    const u64 d2 = call("WSASocketW", {fpi, fpi, fpi, info, 0, 0});
    ASSERT_NE(d2, invalid_socket());
    EXPECT_EQ(call("closesocket", {c}), 0u);
    (void)put_str(kData + 0x2000, "dup");
    EXPECT_EQ(call("send", {d1, kData + 0x2000, 3, 0}), 3u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(call("recv", {a, kData + 0x2100, 16, 0}), 3u);
    EXPECT_EQ(str(kData + 0x2100).substr(0, 3), "dup");
    EXPECT_EQ(rc32(call("WSADuplicateSocketW", {d1, 0xFFFFFFF0u, info})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(rc32(call("WSADuplicateSocketW", {d1, pid, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(rc32(call("WSADuplicateSocketW", {0x1234, pid, info})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10038u);
    (void)mem_.write_u32(info + 112, 0x12345);                   // forged token
    EXPECT_EQ(call("WSASocketW", {fpi, fpi, fpi, info, 0, 0}), invalid_socket());
    EXPECT_EQ(wsa_error(), 10022u);
    // ANSI: dupA_udp rc=0, dupA_info type=2 proto=17, dupA_use valid,
    // dupA_port_same rc=1.
    const u64 u = bound(2, 17);
    ASSERT_EQ(call("WSADuplicateSocketA", {u, pid, info}), 0u);
    EXPECT_EQ(mem_.read_u32(info + 88).value(), 2u);
    EXPECT_EQ(str(info + 116), "MSAFD Tcpip [UDP/IP]");
    const u64 du = call("WSASocketA", {fpi, fpi, fpi, info, 0, 0});
    ASSERT_NE(du, invalid_socket());
    EXPECT_EQ(port_of(du), port_of(u));
}

INSTANTIATE_TEST_SUITE_P(Bitness, Ws2ContractTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

}  // namespace
