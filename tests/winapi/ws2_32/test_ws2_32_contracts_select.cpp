/// @file test_ws2_32_contracts_select.cpp
/// @brief ws2_32 contract tests for select() and fd_set handling.
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// ws2_core_probe connect_nb_refused_select: rc=1 w=0 x=1 — failed nonblocking
// connect is reported in exceptfds, not writefds.
TEST_P(Ws2ContractTest, SelectReportsFailedConnectInExceptfds) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    (void)mem_.write_u32(kData + 0x100, 1);
    ASSERT_EQ(call("ioctlsocket", {s, 0x8004667E, kData + 0x100}), 0u);
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, htons(1));
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(static_cast<u32>(call("connect", {s, sa, 16})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10035u);
    // fd_set: count=1, socket at array[0]; x64 pad then 8-byte SOCKET.
    const GuestAddr wset = kData + 0x200, xset = kData + 0x400;
    const u32 off = is64() ? 8u : 4u;
    (void)mem_.write_u32(wset, 1);
    (void)mem_.write_u32(xset, 1);
    if (is64()) {
        (void)mem_.write_u64(wset + off, s);
        (void)mem_.write_u64(xset + off, s);
    } else {
        (void)mem_.write_u32(wset + off, static_cast<u32>(s));
        (void)mem_.write_u32(xset + off, static_cast<u32>(s));
    }
    (void)mem_.write_u32(kData + 0x600, 8);   // tv_sec
    (void)mem_.write_u32(kData + 0x604, 0);   // tv_usec
    const u64 rc = call("select", {0, 0, wset, xset, kData + 0x600});
    EXPECT_EQ(rc, 1u);
    EXPECT_EQ(mem_.read_u32(wset).value(), 0u);
    EXPECT_EQ(mem_.read_u32(xset).value(), 1u);
}

// ws2_core_probe select_closed_socket / select_bogus_handle / select_mixed_bogus:
// any closed or unknown SOCKET in a set -> WSAENOTSOCK; fd_count unchanged.
TEST_P(Ws2ContractTest, SelectInvalidSocketReturnsWsaenotsock) {
    startup();
    const u32 off = is64() ? 8u : 4u;
    const GuestAddr rset = kData + 0x200;
    (void)mem_.write_u32(kData + 0x600, 0);
    (void)mem_.write_u32(kData + 0x604, 0);

    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    ASSERT_EQ(call("closesocket", {s}), 0u);
    (void)mem_.write_u32(rset, 1);
    if (is64()) (void)mem_.write_u64(rset + off, s);
    else (void)mem_.write_u32(rset + off, static_cast<u32>(s));
    EXPECT_EQ(static_cast<u32>(call("select", {0, rset, 0, 0, kData + 0x600})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10038u);
    EXPECT_EQ(mem_.read_u32(rset).value(), 1u);

    (void)mem_.write_u32(rset, 1);
    if (is64()) (void)mem_.write_u64(rset + off, 0x1234);
    else (void)mem_.write_u32(rset + off, 0x1234u);
    EXPECT_EQ(static_cast<u32>(call("select", {0, rset, 0, 0, kData + 0x600})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10038u);

    const u64 l = call("socket", {2, 1, 6});
    ASSERT_NE(l, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {l, sa, 16}), 0u);
    ASSERT_EQ(call("listen", {l, 1}), 0u);
    (void)mem_.write_u32(rset, 2);
    if (is64()) {
        (void)mem_.write_u64(rset + off, l);
        (void)mem_.write_u64(rset + off + 8, 0x1234);
    } else {
        (void)mem_.write_u32(rset + off, static_cast<u32>(l));
        (void)mem_.write_u32(rset + off + 4, 0x1234u);
    }
    EXPECT_EQ(static_cast<u32>(call("select", {0, rset, 0, 0, kData + 0x600})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10038u);
    EXPECT_EQ(mem_.read_u32(rset).value(), 2u);
}

// ws2_core_probe select_all_null / select_all_empty: WSAEINVAL (10022).
TEST_P(Ws2ContractTest, SelectNullOrEmptySetsReturnWsaeinval) {
    startup();
    (void)mem_.write_u32(kData + 0x600, 0);
    (void)mem_.write_u32(kData + 0x604, 0);
    EXPECT_EQ(static_cast<u32>(call("select", {0, 0, 0, 0, kData + 0x600})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(static_cast<u32>(call("select", {0, 0, 0, 0, 0})), 0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);

    const GuestAddr r = kData + 0x200, w = kData + 0x300, x = kData + 0x400;
    (void)mem_.write_u32(r, 0);
    (void)mem_.write_u32(w, 0);
    (void)mem_.write_u32(x, 0);
    EXPECT_EQ(static_cast<u32>(call("select", {0, r, w, x, kData + 0x600})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 10022u);
}

// ws2_select_timeout_probe: on an idle socket a nonzero timeout waits
// floor(timeout) + 1 ms and never returns early -- {0,500} waits ~1000 us and
// {0,1000} ~1990 us -- while {0,0} returns at once. A host poll() in whole
// milliseconds that truncated made every sub-millisecond select a spin.
TEST_P(Ws2ContractTest, SelectSubMillisecondTimeoutWaitsAMillisecond) {
    startup();
    const u64 s = call("socket", {2, 2, 17});
    ASSERT_NE(s, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {s, sa, 16}), 0u);
    const GuestAddr rset = kData + 0x200;
    const u32 off = is64() ? 8u : 4u;
    const auto wait_us = [&](u32 usec) {
        (void)mem_.write_u32(rset, 1);
        if (is64()) (void)mem_.write_u64(rset + off, s);
        else (void)mem_.write_u32(rset + off, static_cast<u32>(s));
        (void)mem_.write_u32(kData + 0x600, 0);
        (void)mem_.write_u32(kData + 0x604, usec);
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(call("select", {0, rset, 0, 0, kData + 0x600}), 0u) << usec;
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    // Lower bounds only: a loaded host may wait longer, never shorter.
    EXPECT_GE(wait_us(1), 900) << "a 1 us timeout must still wait ~1 ms";
    EXPECT_GE(wait_us(500), 900);
    EXPECT_GE(wait_us(1000), 1900) << "1 ms waits floor(1) + 1 = 2 ms";
    EXPECT_LT(wait_us(0), 500000) << "a zero timeout polls";
}

// Host fd_set is limited to FD_SETSIZE (1024) bit indices; Vitreo select uses
// poll so a SOCKET whose host fd is >= 1024 still works (Winsock has no such
//'host fd' limit).
TEST_P(Ws2ContractTest, SelectWorksWhenHostFdExceedsFdSetsize) {
    startup();
    std::vector<int> pads;
    pads.reserve(1100);
    for (int i = 0; i < 1100; ++i) {
        const int fd = ::open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        pads.push_back(fd);
    }
    if (pads.empty() || pads.back() < FD_SETSIZE) {
        for (int fd : pads) ::close(fd);
        GTEST_SKIP() << "could not raise host fds past FD_SETSIZE";
    }
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {s, sa, 16}), 0u);
    ASSERT_EQ(call("listen", {s, 1}), 0u);
    const GuestAddr rset = kData + 0x200;
    const u32 off = is64() ? 8u : 4u;
    (void)mem_.write_u32(rset, 1);
    if (is64()) (void)mem_.write_u64(rset + off, s);
    else (void)mem_.write_u32(rset + off, static_cast<u32>(s));
    (void)mem_.write_u32(kData + 0x600, 0);
    (void)mem_.write_u32(kData + 0x604, 0);
    EXPECT_EQ(call("select", {0, rset, 0, 0, kData + 0x600}), 0u);
    for (int fd : pads) ::close(fd);
}

// Mono FD_SETSIZE=1024: __WSAFDIsSet must see sockets past index 64
// (ws2_core_probe select_index100_isset).
TEST_P(Ws2ContractTest, WsaFdIsSetHonorsLargeFdSetCount) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    const GuestAddr set = kData + 0x1000;
    constexpr u32 kCount = 101;
    (void)mem_.write_u32(set, kCount);
    const u32 off = is64() ? 8u : 4u;
    const u32 stride = is64() ? 8u : 4u;
    for (u32 i = 0; i < kCount; ++i) {
        if (is64()) (void)mem_.write_u64(set + off + i * stride, 0);
        else (void)mem_.write_u32(set + off + i * stride, 0);
    }
    if (is64()) (void)mem_.write_u64(set + off + 100 * stride, s);
    else (void)mem_.write_u32(set + off + 100 * stride, static_cast<u32>(s));
    EXPECT_EQ(call("__WSAFDIsSet", {s, set}), 1u);
}

}  // namespace
