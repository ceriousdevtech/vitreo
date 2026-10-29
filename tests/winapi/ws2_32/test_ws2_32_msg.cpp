/// @file test_ws2_32_msg.cpp
/// @brief WSASendMsg / WSARecvMsg / WSASendTo / WSARecvFrom overlapped and
///        control-data contracts, and WSAGetOverlappedResult.
///
/// Every expected value is copied from tools/oracle_probe/ws2_msg_probe
/// (.expected / .x86.expected) or ws2_ovres_probe (.expected / .x86.expected),
/// measured on Windows -- never from running Vitreo. Line names in the comments
/// are the probe's keys. Sockets are real loopback sockets.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <winapi/networking/ws2_32/ws2_32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kStackBase = 0x80000;
constexpr GuestAddr kStackSize = 0x10000;
constexpr GuestAddr kData = 0x200000;
constexpr GuestAddr kDataSize = 0x100000;

constexpr u64 kSocketError = 0xFFFFFFFFu;

class Ws2MsgContractTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(mem_.map_region(kStackBase, kStackSize, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem_.map_region(kData, kDataSize, MemoryProtection::ReadWrite).is_ok());
        handles_ = std::make_unique<HandleTable>();
        sync_ = std::make_unique<SyncManager>(*handles_);
        mgr_.set_sync_manager(sync_.get());
        mgr_.set_memory(&mem_);
        vmm_ = std::make_unique<VirtualMemoryManager>(mem_);
        heap_ = std::make_unique<HeapManager>(*vmm_, mem_, *handles_);
        mgr_.set_heap_manager(heap_.get());
        if (is64()) cpu_.init_long_mode();
        else cpu_.init_protected_mode();
        cpu_.set_sp(kStackBase + kStackSize - 0x200);
        exp_ = create_ws2_thunks(mgr_);
        ASSERT_EQ(call("WSAStartup", {0x0202, kData + 0xF0000}), 0u);
    }

    void TearDown() override {
        mgr_.shutdown_all_sockets();
        sync_.reset();
        handles_.reset();
    }

    [[nodiscard]] bool is64() const { return GetParam(); }
    [[nodiscard]] u32 ptr_size() const { return is64() ? 8u : 4u; }

    u64 call(const std::string& name, std::initializer_list<u64> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exp_.entries) {
            if (e.name == name) { entry = &e; break; }
        }
        if (entry == nullptr) {
            ADD_FAILURE() << "missing export " << name;
            return 0;
        }
        const u64 saved_sp = cpu_.sp();
        const std::vector<u64> a(args);
        if (is64()) {
            const u64 frame = 0x28 + 8 * (a.size() > 4 ? a.size() - 4 : 0);
            const u64 sp = (saved_sp - frame - 0x40) & ~u64{0xF};
            cpu_.set_sp(sp);
            (void)mem_.write_u64(sp, 0xDEADBEEF);
            u64* regs[4] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
            for (usize i = 0; i < a.size(); ++i) {
                if (i < 4) *regs[i] = a[i];
                else (void)mem_.write_u64(sp + 0x28 + 8 * (i - 4), a[i]);
            }
        } else {
            const u64 sp = saved_sp - 4 - 4 * a.size();
            cpu_.set_sp(sp);
            (void)mem_.write_u32(static_cast<GuestAddr>(sp), 0xDEADBEEF);
            for (usize i = 0; i < a.size(); ++i) {
                (void)mem_.write_u32(static_cast<GuestAddr>(sp + 4 + 4 * i),
                                     static_cast<u32>(a[i]));
            }
        }
        Marshaler m(mem_);
        m.attach_cpu(cpu_);
        cpu_.gpr.rax = 0x5A5A5A5A5A5A5A5Aull;
        entry->handler(m);
        cpu_.set_sp(saved_sp);
        return is64() ? cpu_.gpr.rax : (cpu_.gpr.rax & 0xFFFFFFFFull);
    }

    u32 wsa_error() { return static_cast<u32>(call("WSAGetLastError", {})); }

    void put_ptr(GuestAddr at, u64 v) {
        if (is64()) (void)mem_.write_u64(at, v);
        else (void)mem_.write_u32(at, static_cast<u32>(v));
    }
    [[nodiscard]] u64 get_ptr(GuestAddr at) {
        return is64() ? mem_.read_u64(at).value() : mem_.read_u32(at).value();
    }

    /// A UDP socket bound to 127.0.0.1; *sa_out receives its sockaddr_in.
    u64 udp_bound(GuestAddr sa_out) {
        const u64 s = call("socket", {2, 2, 17});
        EXPECT_NE(static_cast<u32>(s), 0xFFFFFFFFu);
        (void)mem_.write_u16(sa_out, 2);
        (void)mem_.write_u16(sa_out + 2, 0);
        (void)mem_.write_u32(sa_out + 4, htonl(INADDR_LOOPBACK));
        (void)mem_.write_u64(sa_out + 8, 0);
        EXPECT_EQ(call("bind", {s, sa_out, 16}), 0u);
        (void)mem_.write_u32(kData + 0xE0000, 16);
        EXPECT_EQ(call("getsockname", {s, sa_out, kData + 0xE0000}), 0u);
        return s;
    }
    void set_nonblocking(u64 s) {
        (void)mem_.write_u32(kData + 0xE0010, 1);
        ASSERT_EQ(call("ioctlsocket", {s, 0x8004667E, kData + 0xE0010}), 0u);
    }
    u64 setopt(u64 s, u32 level, u32 opt, u32 value, u32 len = 4) {
        (void)mem_.write_u32(kData + 0xE0020, value);
        return call("setsockopt", {s, level, opt, kData + 0xE0020, len});
    }

    // WSAMSG: name@0 namelen@P lpBuffers@2P dwBufferCount@3P Control.len@4P
    // Control.buf@5P dwFlags@6P (ws2def.h; wsamsg=56 on x64, 28 on x86).
    [[nodiscard]] u32 off(u32 slot) const { return slot * ptr_size(); }
    void build_msg(GuestAddr msg, GuestAddr name, u32 namelen, GuestAddr wsabuf,
                   u32 count, GuestAddr control, u32 control_len, u32 flags = 0) {
        put_ptr(msg + off(0), name);
        (void)mem_.write_u32(msg + off(1), namelen);
        put_ptr(msg + off(2), wsabuf);
        (void)mem_.write_u32(msg + off(3), count);
        (void)mem_.write_u32(msg + off(4), control_len);
        put_ptr(msg + off(5), control);
        (void)mem_.write_u32(msg + off(6), flags);
    }
    void build_wsabuf(GuestAddr wsabuf, GuestAddr data, u32 len) {
        (void)mem_.write_u32(wsabuf, len);
        put_ptr(wsabuf + ptr_size(), data);
    }
    // WSACMSGHDR: SIZE_T cmsg_len; INT level; INT type; data at 16 (x64) / 12.
    [[nodiscard]] u32 cmsg_hdr() const { return is64() ? 16u : 12u; }
    [[nodiscard]] u32 cmsg_space_int() const { return is64() ? 24u : 16u; }
    void put_int_cmsg(GuestAddr at, i32 level, i32 type, i32 value) {
        put_ptr(at, cmsg_hdr() + 4u);
        (void)mem_.write_u32(at + ptr_size(), static_cast<u32>(level));
        (void)mem_.write_u32(at + ptr_size() + 4, static_cast<u32>(type));
        (void)mem_.write_u32(at + cmsg_hdr(), static_cast<u32>(value));
    }
    struct Cmsg { u64 len; i32 level; i32 type; u32 value; };
    Cmsg read_cmsg(GuestAddr at) {
        Cmsg c{};
        c.len = get_ptr(at);
        c.level = static_cast<i32>(mem_.read_u32(at + ptr_size()).value());
        c.type = static_cast<i32>(mem_.read_u32(at + ptr_size() + 4).value());
        c.value = mem_.read_u32(at + cmsg_hdr()).value();
        return c;
    }

    /// WSASendMsg of `n` bytes to `to` with one INT control message
    /// (type < 0: none). Returns the call's return value.
    u64 send_msg(u64 s, GuestAddr to, u32 n, i32 level, i32 type, i32 value,
                 u32 flags = 0, GuestAddr sent = kData + 0xD0000) {
        const GuestAddr base = kData + 0xC0000;
        for (u32 i = 0; i < n; ++i) (void)mem_.write_u8(base + 0x400 + i, static_cast<u8>(i));
        build_wsabuf(base + 0x100, base + 0x400, n);
        u32 control_len = 0;
        if (type >= 0) {
            put_int_cmsg(base + 0x200, level, type, value);
            control_len = cmsg_space_int();
        }
        build_msg(base, to, to != 0 ? 16u : 0u, base + 0x100, 1,
                  type >= 0 ? base + 0x200 : 0, control_len);
        return call("WSASendMsg", {s, base, flags, sent, 0, 0});
    }

    /// WSARecvMsg into a `cap`-byte buffer with a `ctl`-byte control block.
    struct RecvOut {
        u64 rc; u32 err; u32 got; u32 flags; u32 control_len; GuestAddr control;
    };
    RecvOut recv_msg(u64 s, u32 cap, u32 ctl) {
        const GuestAddr base = kData + 0xB0000;
        (void)mem_.write_bytes(base + 0x200, std::vector<Byte>(128, 0xAB).data(), 128);
        build_wsabuf(base + 0x100, base + 0x400, cap);
        build_msg(base, base + 0x80, 128, base + 0x100, 1, ctl != 0 ? base + 0x200 : 0, ctl);
        (void)mem_.write_u32(base + 0x300, 0xFFFF);
        RecvOut out{};
        out.rc = call("WSARecvMsg", {s, base, base + 0x300, 0, 0});
        out.err = out.rc == 0 ? 0u : wsa_error();
        out.got = mem_.read_u32(base + 0x300).value();
        out.flags = mem_.read_u32(base + off(6)).value();
        out.control_len = mem_.read_u32(base + off(4)).value();
        out.control = base + 0x200;
        return out;
    }

    static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    GuestMemory mem_;
    cpu::CpuState cpu_;
    std::unique_ptr<HandleTable> handles_;
    std::unique_ptr<SyncManager> sync_;
    std::unique_ptr<VirtualMemoryManager> vmm_;
    std::unique_ptr<HeapManager> heap_;
    Ws2Manager mgr_;
    DllExports exp_;
};

// ws2_msg_probe v4_send_ecn1 / v4_recv_tos_ecn1: IP_RECVTOS delivers the TOS
// byte as IPPROTO_IP/IP_TOS (3), an INT, in a WSACMSGHDR whose length is the
// header plus 4 and whose space is 24 (x64) / 16 (x86); the ECN codepoint
// sent with IP_ECN (50) is that byte.
TEST_P(Ws2MsgContractTest, SendMsgEcnArrivesAsIpTosControlMessage) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    set_nonblocking(r);
    ASSERT_EQ(setopt(r, 0, 40, 1), 0u);  // IP_RECVTOS
    ASSERT_EQ(send_msg(s, ra, 10, 0, 50, 1), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0xD0000).value(), 10u);
    settle();
    const RecvOut o = recv_msg(r, 256, 128);
    ASSERT_EQ(o.rc, 0u);
    EXPECT_EQ(o.got, 10u);
    EXPECT_EQ(o.flags, 0u);
    EXPECT_EQ(o.control_len, cmsg_space_int());
    const Cmsg c = read_cmsg(o.control);
    EXPECT_EQ(c.len, cmsg_hdr() + 4u);
    EXPECT_EQ(c.level, 0);
    EXPECT_EQ(c.type, 3);  // IP_TOS
    EXPECT_EQ(c.value, 1u);

    // v4_recv_tos_recvecn: with IP_RECVECN as well, IP_ECN (50) comes first,
    // then IP_TOS; both carry the codepoint.
    ASSERT_EQ(setopt(r, 0, 50, 1), 0u);  // IP_RECVECN
    ASSERT_EQ(send_msg(s, ra, 10, 0, 50, 1), 0u);
    settle();
    const RecvOut o2 = recv_msg(r, 256, 128);
    ASSERT_EQ(o2.rc, 0u);
    EXPECT_EQ(o2.control_len, 2 * cmsg_space_int());
    const Cmsg first = read_cmsg(o2.control);
    const Cmsg second = read_cmsg(o2.control + cmsg_space_int());
    EXPECT_EQ(first.type, 50);
    EXPECT_EQ(first.value, 1u);
    EXPECT_EQ(second.type, 3);
    EXPECT_EQ(second.value, 1u);

    // ctl2_cap_30: two messages into 30 control bytes: WSAEMSGSIZE, MSG_CTRUNC,
    // Control.len = the full size. x64 gets a 30-byte prefix; x86 gets the
    // first message and the second one's 12-byte header only.
    ASSERT_EQ(send_msg(s, ra, 10, 0, 50, 2), 0u);
    settle();
    const RecvOut t = recv_msg(r, 256, 30);
    EXPECT_EQ(t.rc, kSocketError);
    EXPECT_EQ(t.err, 10040u);
    EXPECT_EQ(t.got, 10u);
    EXPECT_EQ(t.flags, 0x200u);
    EXPECT_EQ(t.control_len, 2 * cmsg_space_int());
    std::vector<Byte> bytes(48);
    ASSERT_TRUE(mem_.read_bytes(t.control, bytes.data(), bytes.size()).is_ok());
    if (is64()) {
        // 1400000000000000 00000000 32000000 0200000000000000 140000000000 abab..
        EXPECT_EQ(bytes[24], 0x14);
        EXPECT_EQ(bytes[29], 0x00);
        EXPECT_EQ(bytes[30], 0xAB);
    } else {
        // 10000000 00000000 32000000 02000000 | 10000000 00000000 03000000 | abab
        EXPECT_EQ(bytes[16], 0x10);
        EXPECT_EQ(bytes[24], 0x03);
        EXPECT_EQ(bytes[28], 0xAB);
    }
}

// ws2_msg_probe v4_send_ecn_bad / snd4_IP_ECN_3 / v4_send_ipv6ecn_on_v4 /
// v4_send_unknown_cmsg / v4_send_flag_bogus / send_tcp / v4_send_null_msg /
// v4_send_noname_unconnected / v4_send_shortname / v4_send_null_count.
TEST_P(Ws2MsgContractTest, SendMsgRejectsWhatWindowsRejects) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    set_nonblocking(r);
    EXPECT_EQ(send_msg(s, ra, 10, 0, 50, 7), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(send_msg(s, ra, 10, 0, 50, 3), kSocketError);  // CE is refused
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(send_msg(s, ra, 10, 41, 50, 1), kSocketError);  // IPV6_ECN to IPv4
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(send_msg(s, ra, 10, 0, 999, 1), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(send_msg(s, ra, 10, 0, -1, 0, 0x10000), kSocketError);
    EXPECT_EQ(wsa_error(), 10045u);
    EXPECT_EQ(send_msg(s, 0, 10, 0, -1, 0), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(call("WSASendMsg", {s, 0, 0, kData + 0xD0000, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10014u);
    // Accepted flags (v4_send_flag_oob / _dontroute / _partial).
    EXPECT_EQ(send_msg(s, ra, 10, 0, -1, 0, 0x1), 0u);
    EXPECT_EQ(send_msg(s, ra, 10, 0, -1, 0, 0x8000), 0u);
    // NULL count without OVERLAPPED: WSAEFAULT, but the datagram was sent.
    EXPECT_EQ(send_msg(s, ra, 4, 0, -1, 0, 0, 0), kSocketError);
    EXPECT_EQ(wsa_error(), 10014u);
    settle();
    int arrived = 0;
    for (;;) {
        const RecvOut o = recv_msg(r, 256, 0);
        if (o.rc != 0) break;
        ++arrived;
    }
    EXPECT_EQ(arrived, 3);
    // send_tcp / recv_tcp: stream sockets are refused with WSAEINVAL.
    const u64 t = call("socket", {2, 1, 6});
    EXPECT_EQ(send_msg(t, 0, 10, 0, -1, 0), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(recv_msg(t, 64, 0).err, 10022u);
    // send_bad_socket.
    EXPECT_EQ(send_msg(0x1234, ra, 10, 0, -1, 0), kSocketError);
    EXPECT_EQ(wsa_error(), 10038u);
}

// ws2_msg_probe uso_recv: UDP_SEND_MSG_SIZE (IPPROTO_UDP/2) 4 splits a
// 10-byte send into datagrams of 4, 4 and 2; 0 sends one datagram.
TEST_P(Ws2MsgContractTest, SendMsgSegmentsByUdpSendMsgSize) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    set_nonblocking(r);
    ASSERT_EQ(send_msg(s, ra, 10, 17, 2, 4), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0xD0000).value(), 10u);
    ASSERT_EQ(send_msg(s, ra, 10, 17, 2, 0), 0u);
    settle();
    std::vector<u32> sizes;
    for (;;) {
        const RecvOut o = recv_msg(r, 256, 0);
        if (o.rc != 0) break;
        sizes.push_back(o.got);
    }
    EXPECT_EQ(sizes, (std::vector<u32>{4, 4, 2, 10}));
}

// ws2_msg_probe v4_recv_trunc: a 10-byte datagram into 4 bytes fails
// WSAEMSGSIZE with 4 bytes delivered and MSG_TRUNC (0x100).
TEST_P(Ws2MsgContractTest, RecvMsgTruncationFailsWithMsgSize) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    set_nonblocking(r);
    ASSERT_EQ(send_msg(s, ra, 10, 0, -1, 0), 0u);
    settle();
    const RecvOut o = recv_msg(r, 4, 0);
    EXPECT_EQ(o.rc, kSocketError);
    EXPECT_EQ(o.err, 10040u);
    EXPECT_EQ(o.got, 4u);
    EXPECT_EQ(o.flags, 0x100u);
    // v4_recv_empty: nothing queued -> WSAEWOULDBLOCK, count untouched.
    const RecvOut e = recv_msg(r, 64, 64);
    EXPECT_EQ(e.err, 10035u);
    EXPECT_EQ(e.got, 0xFFFFu);
}

// ws2_msg_probe x_*: receive-control options on a stream socket are
// WSAEINVAL, a 1-byte value is WSAEFAULT, the stored value reads back as a
// normalised BOOL, an IPv4 socket accepts IPv6-level ones, and a V6ONLY IPv6
// socket refuses the IP-level ones.
TEST_P(Ws2MsgContractTest, ReceiveControlOptionRules) {
    const u64 u4 = call("socket", {2, 2, 17});
    EXPECT_EQ(setopt(u4, 41, 19, 1), 0u);  // IPV6_PKTINFO on AF_INET
    EXPECT_EQ(setopt(u4, 0, 40, 5), 0u);   // IP_RECVTOS
    (void)mem_.write_u32(kData + 0xE0030, 4);
    ASSERT_EQ(call("getsockopt", {u4, 0, 40, kData + 0xE0040, kData + 0xE0030}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0xE0040).value(), 1u);
    ASSERT_EQ(call("getsockopt", {u4, 0, 50, kData + 0xE0040, kData + 0xE0030}), 0u);
    EXPECT_EQ(mem_.read_u32(kData + 0xE0040).value(), 0u);  // IP_RECVECN
    EXPECT_EQ(setopt(u4, 0, 19, 1, 1), kSocketError);
    EXPECT_EQ(wsa_error(), 10014u);
    const u64 t = call("socket", {2, 1, 6});
    EXPECT_EQ(setopt(t, 0, 19, 1), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    const u64 v6 = call("socket", {23, 2, 17});
    EXPECT_EQ(setopt(v6, 0, 19, 1), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(setopt(v6, 41, 50, 1), 0u);  // IPV6_RECVECN
}

// ws2_ovres_probe map: WSAGetOverlappedResult maps OVERLAPPED.Internal to a
// Winsock error, always reports InternalHigh in *lpcbTransfer, writes
// *lpdwFlags only on success, and fails WSA_IO_INCOMPLETE (996) for a
// pending operation without touching the outputs (gor_pending_nowait). A bad
// socket is WSAENOTSOCK even for a completed OVERLAPPED (gor_bad_socket).
TEST_P(Ws2MsgContractTest, GetOverlappedResultMapsStatusesLikeWindows) {
    const u64 s = call("socket", {2, 2, 17});
    const GuestAddr ov = kData + 0x1000;
    const GuestAddr xfer = kData + 0x1100, flags = kData + 0x1104;
    const u32 high = is64() ? 8u : 4u;
    struct Row { u32 status; u32 ok; u32 err; };
    const Row rows[] = {
        {0x00000000, 1, 0},     {0x80000005, 0, 10040}, {0xC0000001, 0, 10022},
        {0xC0000120, 0, 995},   {0xC000020D, 0, 10054}, {0xC0000236, 0, 10061},
        {0xC0000241, 0, 10053}, {0xC00000B5, 0, 10060}, {0xC000013B, 0, 10053},
        {0xC000023C, 0, 10051}, {0xC000023D, 0, 10065}, {0xC0000237, 0, 10101},
        {0xC000A012, 0, 10052}, {0xC00000B0, 0, 10058}, {0x00000102, 1, 1460},
    };
    for (const Row& row : rows) {
        put_ptr(ov, row.status);
        put_ptr(ov + high, 7);
        (void)mem_.write_u32(xfer, 0xAAAA);
        (void)mem_.write_u32(flags, 0xBBBB);
        const u64 rc = call("WSAGetOverlappedResult", {s, ov, xfer, 0, flags});
        EXPECT_EQ(rc, row.ok) << std::hex << row.status;
        EXPECT_EQ(wsa_error(), row.err) << std::hex << row.status;
        EXPECT_EQ(mem_.read_u32(xfer).value(), 7u);
        EXPECT_EQ(mem_.read_u32(flags).value(), row.ok ? 0u : 0xBBBBu);
    }
    put_ptr(ov, 0x103);  // STATUS_PENDING
    (void)mem_.write_u32(xfer, 0xAAAA);
    EXPECT_EQ(call("WSAGetOverlappedResult", {s, ov, xfer, 0, flags}), 0u);
    EXPECT_EQ(wsa_error(), 996u);
    EXPECT_EQ(mem_.read_u32(xfer).value(), 0xAAAAu);
    put_ptr(ov, 0);
    EXPECT_EQ(call("WSAGetOverlappedResult", {0x1234, ov, xfer, 0, flags}), 0u);
    EXPECT_EQ(wsa_error(), 10038u);
}

// ws2_ovres_probe sendto_ov / sendto_routine / sendto_nullcount_sync /
// sendto_noname / sendto_flag_oob: an overlapped WSASendTo that completes
// inline returns 0 with Internal/InternalHigh final and hEvent signalled; with
// a completion routine it reports WSA_IO_PENDING and queues the routine.
TEST_P(Ws2MsgContractTest, SendToOverlappedCompletesInline) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    set_nonblocking(r);
    const GuestAddr data = kData + 0x2000, wsabuf = kData + 0x2100;
    build_wsabuf(wsabuf, data, 10);
    const u64 ev = call("WSACreateEvent", {});
    const GuestAddr ov = kData + 0x3000;
    const u32 high = is64() ? 8u : 4u;
    const u32 event_off = is64() ? 24u : 16u;
    put_ptr(ov, 0x1111);
    put_ptr(ov + high, 0x2222);
    put_ptr(event_off + ov, ev);
    const GuestAddr sent = kData + 0x3100;
    (void)mem_.write_u32(sent, 0xCCCC);
    EXPECT_EQ(call("WSASendTo", {s, wsabuf, 1, sent, 0, ra, 16, ov, 0}), 0u);
    EXPECT_EQ(mem_.read_u32(sent).value(), 10u);
    EXPECT_EQ(get_ptr(ov), 0u);
    EXPECT_EQ(get_ptr(ov + high), 10u);
    put_ptr(kData + 0x3200, ev);
    EXPECT_EQ(call("WSAWaitForMultipleEvents", {1, kData + 0x3200, 1, 0, 0}), 0u);

    struct Apc { u32 err; u32 bytes; GuestAddr ov; };
    std::vector<Apc> apcs;
    mgr_.set_wsa_completion_routine_queue(
        [&](u32, GuestAddr, u32 err, u32 bytes, GuestAddr o, u32) {
            apcs.push_back({err, bytes, o});
        });
    EXPECT_EQ(call("WSASendTo", {s, wsabuf, 1, sent, 0, ra, 16, ov, 0x401000}), kSocketError);
    EXPECT_EQ(wsa_error(), 997u);
    ASSERT_EQ(apcs.size(), 1u);
    EXPECT_EQ(apcs[0].err, 0u);
    EXPECT_EQ(apcs[0].bytes, 10u);

    EXPECT_EQ(call("WSASendTo", {s, wsabuf, 1, 0, 0, ra, 16, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(call("WSASendTo", {s, wsabuf, 1, sent, 0, 0, 0, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10057u);
    EXPECT_EQ(call("WSASendTo", {s, wsabuf, 1, sent, 1, ra, 16, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10045u);
    EXPECT_EQ(call("WSASendTo", {s, wsabuf, 1, sent, 0, ra, 8, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10014u);
    settle();
    int arrived = 0;
    for (;;) {
        const RecvOut o = recv_msg(r, 256, 0);
        if (o.rc != 0) break;
        ++arrived;
    }
    EXPECT_EQ(arrived, 3);  // event send, routine send, NULL-count send
}

// ws2_ovres_probe recvfrom_short_fromlen / _left and recvfrom_trunc_sync.
TEST_P(Ws2MsgContractTest, RecvFromChecksFromlenFirstAndReportsTruncation) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    set_nonblocking(r);
    ASSERT_EQ(send_msg(s, ra, 10, 0, -1, 0), 0u);
    settle();
    const GuestAddr buf = kData + 0x2000, wsabuf = kData + 0x2100;
    const GuestAddr got = kData + 0x2200, fl = kData + 0x2204;
    const GuestAddr from = kData + 0x2300, fromlen = kData + 0x2380;
    build_wsabuf(wsabuf, buf, 64);
    (void)mem_.write_u32(fl, 0);
    (void)mem_.write_u32(fromlen, 8);
    EXPECT_EQ(call("WSARecvFrom", {r, wsabuf, 1, got, fl, from, fromlen, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10014u);
    EXPECT_EQ(mem_.read_u32(fromlen).value(), 8u);
    // The datagram is still there; a 4-byte buffer takes its head and fails.
    build_wsabuf(wsabuf, buf, 4);
    (void)mem_.write_u32(fromlen, 16);
    EXPECT_EQ(call("WSARecvFrom", {r, wsabuf, 1, got, fl, from, fromlen, 0, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10040u);
    EXPECT_EQ(mem_.read_u32(got).value(), 4u);
}

// ws2_ovres_probe gor_cancelled: CancelIoEx completes a pending receive with
// STATUS_CANCELLED; WSAGetOverlappedResult reports WSA_OPERATION_ABORTED with
// the count written (0) and the flags untouched.
TEST_P(Ws2MsgContractTest, CancelCompletesPendingReceiveWithOperationAborted) {
    const GuestAddr ra = kData + 0x10;
    const u64 r = udp_bound(ra);
    const GuestAddr buf = kData + 0x2000, wsabuf = kData + 0x2100;
    const GuestAddr got = kData + 0x2200, fl = kData + 0x2204;
    build_wsabuf(wsabuf, buf, 64);
    (void)mem_.write_u32(fl, 0);
    const GuestAddr ov = kData + 0x3000;
    put_ptr(ov, 0);
    put_ptr(ov + (is64() ? 8u : 4u), 0);
    EXPECT_EQ(call("WSARecv", {r, wsabuf, 1, got, fl, ov, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 997u);
    EXPECT_EQ(mgr_.cancel_overlapped_socket_io(static_cast<u32>(r), ov, false, 0), 1u);
    EXPECT_EQ(get_ptr(ov) & 0xFFFFFFFFu, 0xC0000120u);
    const GuestAddr xfer = kData + 0x3100, flags = kData + 0x3104;
    (void)mem_.write_u32(xfer, 0xAAAA);
    (void)mem_.write_u32(flags, 0xBBBB);
    EXPECT_EQ(call("WSAGetOverlappedResult", {r, ov, xfer, 0, flags}), 0u);
    EXPECT_EQ(wsa_error(), 995u);
    EXPECT_EQ(mem_.read_u32(xfer).value(), 0u);
    EXPECT_EQ(mem_.read_u32(flags).value(), 0xBBBBu);
}

// ws2_ovres_probe recv_immediate: an overlapped receive whose data is already
// queued returns 0 with Internal = 0, InternalHigh = count and hEvent set.
TEST_P(Ws2MsgContractTest, OverlappedReceiveCompletesInlineWhenDataIsQueued) {
    const GuestAddr ra = kData + 0x10, sa = kData + 0x40;
    const u64 r = udp_bound(ra);
    const u64 s = udp_bound(sa);
    ASSERT_EQ(send_msg(s, ra, 3, 0, -1, 0), 0u);
    settle();
    const GuestAddr buf = kData + 0x2000, wsabuf = kData + 0x2100;
    const GuestAddr got = kData + 0x2200, fl = kData + 0x2204;
    build_wsabuf(wsabuf, buf, 64);
    (void)mem_.write_u32(fl, 0);
    const u64 ev = call("WSACreateEvent", {});
    const GuestAddr ov = kData + 0x3000;
    put_ptr(ov, 0x1111);
    put_ptr(ov + (is64() ? 8u : 4u), 0x2222);
    put_ptr(ov + (is64() ? 24u : 16u), ev);
    EXPECT_EQ(call("WSARecv", {r, wsabuf, 1, got, fl, ov, 0}), 0u);
    EXPECT_EQ(mem_.read_u32(got).value(), 3u);
    EXPECT_EQ(get_ptr(ov), 0u);
    EXPECT_EQ(get_ptr(ov + (is64() ? 8u : 4u)), 3u);
    put_ptr(kData + 0x3200, ev);
    EXPECT_EQ(call("WSAWaitForMultipleEvents", {1, kData + 0x3200, 1, 0, 0}), 0u);

    // A synchronous datagram WSARecv that fits is a plain success
    // (ws2_ovres_probe rt_recvfrom_immediate delivers 10 of 10, rc=0).
    ASSERT_EQ(send_msg(s, ra, 10, 0, -1, 0), 0u);
    settle();
    EXPECT_EQ(call("WSARecv", {r, wsabuf, 1, got, fl, 0, 0}), 0u);
    EXPECT_EQ(mem_.read_u32(got).value(), 10u);
}

// ws2_misc_probe inet_addr lines: Windows' grammar (1-4 parts, octal and hex,
// trailing whitespace ends the address, leading whitespace does not), last
// error untouched except for NULL (WSAEFAULT).
TEST_P(Ws2MsgContractTest, InetAddrFollowsWindowsGrammar) {
    struct Row { const char* in; u32 v; };
    const Row rows[] = {
        {"1.2.3.4", 0x04030201}, {"1.2.3", 0x03000201}, {"1.2", 0x02000001},
        {"1", 0x01000000}, {"0x7f.1", 0x0100007f}, {"0177.0.0.1", 0x0100007f},
        {"0x7f000001", 0x0100007f}, {"017700000001", 0x0100007f},
        {"1.2.3.256", 0xffffffff}, {"1.2.65535", 0xffff0201}, {"1.16777215", 0xffffff01},
        {"4294967296", 0xffffffff}, {"", 0xffffffff}, {" ", 0x0},
        {"  1.2.3.4", 0xffffffff}, {"1.2.3.4 ", 0x04030201}, {"1.2.3.4x", 0xffffffff},
        {"1.2.3.4 x", 0x04030201}, {"1..2.3", 0xffffffff}, {"1.2.3.4.5", 0xffffffff},
        {"0x", 0xffffffff}, {"08", 0xffffffff}, {"1.2.3.4\t", 0x04030201},
        {"-1", 0xffffffff}, {"1.2.3.", 0xffffffff}, {".1.2.3", 0xffffffff},
    };
    const GuestAddr str = kData + 0x4000;
    for (const Row& row : rows) {
        (void)mem_.write_bytes(str, reinterpret_cast<const Byte*>(row.in), std::strlen(row.in) + 1);
        call("WSASetLastError", {0xDEAD});
        EXPECT_EQ(static_cast<u32>(call("inet_addr", {str})), row.v) << '"' << row.in << '"';
        EXPECT_EQ(wsa_error(), 0xDEADu) << row.in;
    }
    EXPECT_EQ(static_cast<u32>(call("inet_addr", {0})), 0xffffffffu);
    EXPECT_EQ(wsa_error(), 10014u);
}

// ws2_misc_probe inet_ntoa / inet_ntoa_same_buffer / ntop_*.
TEST_P(Ws2MsgContractTest, InetNtoaAndNtopMatchWindows) {
    call("WSASetLastError", {0xDEAD});
    const u64 p = call("inet_ntoa", {0x04030201});
    ASSERT_NE(p, 0u);
    EXPECT_EQ(wsa_error(), 0u);
    char text[32] = {};
    ASSERT_TRUE(mem_.read_bytes(p, reinterpret_cast<Byte*>(text), 16).is_ok());
    EXPECT_STREQ(text, "1.2.3.4");
    EXPECT_EQ(call("inet_ntoa", {0xFFFFFFFF}), p);  // same per-thread buffer
    const GuestAddr src = kData + 0x4100, dst = kData + 0x4200;
    (void)mem_.write_u32(src, 0x04030201);
    EXPECT_EQ(call("inet_ntop", {2, src, dst, 4}), 0u);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(call("inet_ntop", {2, src, 0, 16}), 0u);
    EXPECT_EQ(wsa_error(), 10022u);
    EXPECT_EQ(call("inet_ntop", {30, src, dst, 64}), 0u);  // Darwin's AF_INET6
    EXPECT_EQ(wsa_error(), 10047u);
    EXPECT_EQ(call("inet_ntop", {2, src, dst, 8}), dst);
    EXPECT_EQ(call("inet_ntop", {2, src, dst, 7}), 0u);
    EXPECT_EQ(wsa_error(), 10022u);
}

// ws2_misc_probe close_bogus / close_null / set_bogus / reset_bogus /
// close_twice / enum_bad_socket / enum_null_out / enum_bad_event / enum_ok.
TEST_P(Ws2MsgContractTest, EventHandleAndEnumNetworkEventsErrors) {
    EXPECT_EQ(call("WSACloseEvent", {0x1234}), 0u);
    EXPECT_EQ(wsa_error(), 6u);
    EXPECT_EQ(call("WSACloseEvent", {0}), 0u);
    EXPECT_EQ(wsa_error(), 6u);
    EXPECT_EQ(call("WSASetEvent", {0x1234}), 0u);
    EXPECT_EQ(wsa_error(), 6u);
    EXPECT_EQ(call("WSAResetEvent", {0x1234}), 0u);
    EXPECT_EQ(wsa_error(), 6u);
    const u64 ev = call("WSACreateEvent", {});
    EXPECT_EQ(call("WSACloseEvent", {ev}), 1u);
    EXPECT_EQ(call("WSACloseEvent", {ev}), 0u);
    EXPECT_EQ(wsa_error(), 6u);

    const GuestAddr ne = kData + 0x5000;
    EXPECT_EQ(call("WSAEnumNetworkEvents", {0x1234, 0, ne}), kSocketError);
    EXPECT_EQ(wsa_error(), 10038u);
    const u64 s = call("socket", {2, 2, 17});
    const u64 e2 = call("WSACreateEvent", {});
    ASSERT_EQ(call("WSAEventSelect", {s, e2, 1}), 0u);
    ASSERT_EQ(call("WSASetEvent", {e2}), 1u);
    EXPECT_EQ(call("WSAEnumNetworkEvents", {s, e2, 0}), kSocketError);
    EXPECT_EQ(wsa_error(), 10022u);
    put_ptr(kData + 0x5100, e2);
    EXPECT_EQ(call("WSAWaitForMultipleEvents", {1, kData + 0x5100, 1, 0, 0}), 0u);  // still set
    std::vector<Byte> fill(44, 0xCC);
    (void)mem_.write_bytes(ne, fill.data(), fill.size());
    EXPECT_EQ(call("WSAEnumNetworkEvents", {s, e2, ne}), 0u);
    EXPECT_EQ(mem_.read_u32(ne).value(), 0u);
    EXPECT_EQ(mem_.read_u32(ne + 4).value(), 0xCCCCCCCCu);  // iErrorCode[0] untouched
    EXPECT_EQ(call("WSAWaitForMultipleEvents", {1, kData + 0x5100, 1, 0, 0}), 258u);
    EXPECT_EQ(call("WSAEnumNetworkEvents", {s, 0x1234, ne}), kSocketError);
    EXPECT_EQ(wsa_error(), 10038u);
    // wsasocket_registered_io: WSA_FLAG_REGISTERED_IO is accepted.
    const u64 rio = call("WSASocketW", {2, 2, 17, 0, 0, 0x01 | 0x100});
    EXPECT_NE(static_cast<u32>(rio), 0xFFFFFFFFu);
}

// ws2_misc_probe ghbn_empty / ghbn_localhost: "" is the local host, and
// "localhost" is reported under the computer's own name.
TEST_P(Ws2MsgContractTest, GetHostByNameEmptyAndLocalhost) {
    const GuestAddr name = kData + 0x6000;
    (void)mem_.write_u8(name, 0);
    const u64 h = call("gethostbyname", {name});
    ASSERT_NE(h, 0u);
    // HOSTENT: h_name, h_aliases, h_addrtype (short) at 2P, h_length at 2P+2.
    EXPECT_EQ(mem_.read_u16(h + 2 * ptr_size()).value(), 2u);
    EXPECT_EQ(mem_.read_u16(h + 2 * ptr_size() + 2).value(), 4u);
    const char* lh = "localhost";
    (void)mem_.write_bytes(name, reinterpret_cast<const Byte*>(lh), std::strlen(lh) + 1);
    const u64 h2 = call("gethostbyname", {name});
    ASSERT_NE(h2, 0u);
    char got[64] = {};
    (void)mem_.read_bytes(get_ptr(h2), reinterpret_cast<Byte*>(got), 63);
    (void)mem_.write_u32(kData + 0x6100, 0);
    ASSERT_EQ(call("gethostname", {kData + 0x6200, 256}), 0u);
    char host[64] = {};
    (void)mem_.read_bytes(kData + 0x6200, reinterpret_cast<Byte*>(host), 63);
    EXPECT_STREQ(got, host);
}

// ws2_misc_probe acceptex_data_*: with dwReceiveDataLength > 0, AcceptEx
// completes only once the first data arrives, delivering it at the front of
// lpOutputBuffer; GetAcceptExSockaddrs then reports a 16-byte IPv4 address.
TEST_P(Ws2MsgContractTest, AcceptExWithReceiveDataWaitsForData) {
    const u64 l = call("socket", {2, 1, 6});
    const GuestAddr sa = kData + 0x10;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {l, sa, 16}), 0u);
    (void)mem_.write_u32(kData + 0xE0000, 16);
    ASSERT_EQ(call("getsockname", {l, sa, kData + 0xE0000}), 0u);
    ASSERT_EQ(call("listen", {l, 4}), 0u);
    const u64 acc = call("socket", {2, 1, 6});
    const u64 ev = call("WSACreateEvent", {});
    const GuestAddr ov = kData + 0x3000, out = kData + 0x3400, got = kData + 0x3800;
    for (u32 i = 0; i < 4 * ptr_size(); i += 4) (void)mem_.write_u32(ov + i, 0);
    put_ptr(ov + (is64() ? 24u : 16u), ev);
    EXPECT_EQ(call("AcceptEx", {l, acc, out, 16, 32, 32, got, ov}), 0u);
    EXPECT_EQ(wsa_error(), 997u);
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in host{};
    host.sin_family = AF_INET;
    host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    host.sin_port = static_cast<in_port_t>(mem_.read_u16(sa + 2).value());
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&host), sizeof(host)), 0);
    put_ptr(kData + 0x3900, ev);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(call("WSAWaitForMultipleEvents", {1, kData + 0x3900, 1, 0, 0}), 258u);
    ASSERT_EQ(::send(client, "hello", 5, 0), 5);
    EXPECT_EQ(call("WSAWaitForMultipleEvents", {1, kData + 0x3900, 1, 2000, 0}), 0u);
    const GuestAddr xfer = kData + 0x3A00, flags = kData + 0x3A04;
    EXPECT_EQ(call("WSAGetOverlappedResult", {l, ov, xfer, 0, flags}), 1u);
    EXPECT_EQ(mem_.read_u32(xfer).value(), 5u);
    char data[6] = {};
    ASSERT_TRUE(mem_.read_bytes(out, reinterpret_cast<Byte*>(data), 5).is_ok());
    EXPECT_STREQ(data, "hello");
    const GuestAddr la = kData + 0x3B00, ll = kData + 0x3B10, ra = kData + 0x3B20,
                    rl = kData + 0x3B30;
    call("GetAcceptExSockaddrs", {out, 16, 32, 32, la, ll, ra, rl});
    EXPECT_EQ(mem_.read_u32(ll).value(), 16u);
    EXPECT_EQ(mem_.read_u32(rl).value(), 16u);
    ::close(client);
}

INSTANTIATE_TEST_SUITE_P(Bitness, Ws2MsgContractTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

}  // namespace
