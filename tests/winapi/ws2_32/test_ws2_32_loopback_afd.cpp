/// @file test_ws2_32_loopback_afd.cpp
/// @brief NtDeviceIoControlFile(IOCTL_AFD_RECEIVE) on a socket handle.
///
/// Every expected status, IO_STATUS_BLOCK and delivery rule is
/// tools/oracle_probe/afd_recv_probe.x86.expected (this fixture is a 32-bit
/// guest: AFD_RECV_INFO is 16 bytes, WSABUF 8).

#include "ws2_loopback_fixture.hpp"

namespace {

constexpr u32 IOCTL_AFD_RECEIVE = 0x00012017u;
constexpr u32 kUntouched = 0xEEEEEEEEu;

} // namespace

class Ws2AfdTest : public Ws2LoopbackTest {
protected:
    static constexpr GuestAddr ADDR_IN = OUT_ADDR + 0x400;
    static constexpr GuestAddr BOUND = OUT_ADDR + 0x440;
    static constexpr GuestAddr BOUND_LEN = OUT_ADDR + 0x480;
    static constexpr GuestAddr PAYLOAD = OUT_ADDR + 0x4C0;
    static constexpr GuestAddr INFO = OUT_ADDR + 0x500;
    static constexpr GuestAddr WSABUF = OUT_ADDR + 0x540;
    static constexpr GuestAddr IOSB = OUT_ADDR + 0x580;
    static constexpr GuestAddr RECV_BUF = OUT_ADDR + 0x600;

    struct Delivered {
        int count = 0;
        GuestAddr apc_context = 0;
        u32 status = 0;
        u32 bytes = 0;
        bool synchronous = false;
    };

    void SetUp() override {
        Ws2LoopbackTest::SetUp();
        // The runtime gives Winsock the guest memory completions are written
        // to (dll_registry: ws2_mgr.set_memory).
        mgr_.set_memory(&mem_);
        mgr_.set_nt_completion_callback([this](Handle, GuestAddr apc_context, Handle, GuestAddr, GuestAddr,
                                               u32, u32 bytes, u32 status, u32, bool synchronous) {
            std::lock_guard<std::mutex> lock(delivered_mutex_);
            ++delivered_.count;
            delivered_.apc_context = apc_context;
            delivered_.status = status;
            delivered_.bytes = bytes;
            delivered_.synchronous = synchronous;
        });
    }

    /// A UDP socket bound to an ephemeral loopback port; @p port receives it.
    u32 bound_udp(u16& port) {
        const u32 s = static_cast<u32>(inv_->call("socket", {2u, 2u, 17u}));
        EXPECT_TRUE(mem_.write_u16(ADDR_IN, 2u).is_ok());
        EXPECT_TRUE(mem_.write_u16(ADDR_IN + 2, 0u).is_ok());
        EXPECT_TRUE(mem_.write_u32(ADDR_IN + 4, 0x0100007Fu).is_ok());
        EXPECT_EQ(inv_->call("bind", {s, ADDR_IN, 16u}), 0u);
        EXPECT_TRUE(mem_.write_u32(BOUND_LEN, 16u).is_ok());
        EXPECT_EQ(inv_->call("getsockname", {s, BOUND, BOUND_LEN}), 0u);
        port = mem_.read_u16(BOUND + 2).value_or(0);
        return s;
    }

    void send_datagram(u16 port, const std::string& text) {
        const u32 sender = static_cast<u32>(inv_->call("socket", {2u, 2u, 17u}));
        EXPECT_TRUE(mem_.write_bytes(PAYLOAD, reinterpret_cast<const Byte*>(text.data()), text.size()).is_ok());
        EXPECT_TRUE(mem_.write_u16(ADDR_IN + 2, port).is_ok());
        EXPECT_EQ(inv_->call("sendto", {sender, PAYLOAD, static_cast<u32>(text.size()), 0u, ADDR_IN, 16u}),
                  text.size());
        (void)inv_->call("closesocket", {sender});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    /// NtDeviceIoControlFile(socket, Event=0, Apc=0, ApcContext=IOSB, IOSB,
    /// IOCTL_AFD_RECEIVE, &info, info_length, NULL, 0).
    u32 afd_receive(u32 s, u32 buffer_length, u32 afd_flags, u32 tdi_flags, u32 info_length = 16,
                    u32 code = IOCTL_AFD_RECEIVE) {
        EXPECT_TRUE(mem_.write_u32(WSABUF, buffer_length).is_ok());
        EXPECT_TRUE(mem_.write_u32(WSABUF + 4, RECV_BUF).is_ok());
        EXPECT_TRUE(mem_.write_u32(INFO, WSABUF).is_ok());
        EXPECT_TRUE(mem_.write_u32(INFO + 4, 1u).is_ok());
        EXPECT_TRUE(mem_.write_u32(INFO + 8, afd_flags).is_ok());
        EXPECT_TRUE(mem_.write_u32(INFO + 12, tdi_flags).is_ok());
        EXPECT_TRUE(mem_.write_u32(IOSB, kUntouched).is_ok());
        EXPECT_TRUE(mem_.write_u32(IOSB + 4, kUntouched).is_ok());
        Marshaler m(mem_);
        m.attach_cpu(cpu_);
        bool handled = false;
        const u32 status = mgr_.afd_device_control(m, s, code, INFO, info_length, 0, 0, IOSB, IOSB, 0, 0, handled);
        EXPECT_TRUE(handled);
        return status;
    }

    std::string received(u32 length) {
        std::string text(length, '\0');
        EXPECT_TRUE(mem_.read_bytes(RECV_BUF, reinterpret_cast<Byte*>(text.data()), length).is_ok());
        return text;
    }

    Delivered delivered() {
        std::lock_guard<std::mutex> lock(delivered_mutex_);
        return delivered_;
    }

    std::mutex delivered_mutex_;
    Delivered delivered_;
};

TEST_F(Ws2AfdTest, ReceiveCompletesSynchronouslyWhenADatagramIsQueued) {
    u16 port = 0;
    const u32 s = bound_udp(port);
    send_datagram(port, "hello-afd");
    // udp_sync_data: ret=0 iosb=0 info=9, delivered as a synchronous completion.
    EXPECT_EQ(afd_receive(s, 64, 0, 0x20), 0u);
    EXPECT_EQ(mem_.read_u32(IOSB).value(), 0u);
    EXPECT_EQ(mem_.read_u32(IOSB + 4).value(), 9u);
    EXPECT_EQ(received(9), "hello-afd");
    const Delivered d = delivered();
    EXPECT_EQ(d.count, 1);
    EXPECT_TRUE(d.synchronous);
    EXPECT_EQ(d.apc_context, IOSB);
    (void)inv_->call("closesocket", {s});
}

TEST_F(Ws2AfdTest, PeekLeavesTheDatagramAndTruncationDiscardsTheRest) {
    u16 port = 0;
    const u32 s = bound_udp(port);
    send_datagram(port, "peekme");
    // tdi_a0: normal|peek succeeds; udp_after_peek: the datagram is still there.
    EXPECT_EQ(afd_receive(s, 64, 0, 0xA0), 0u);
    EXPECT_EQ(mem_.read_u32(IOSB + 4).value(), 6u);
    EXPECT_EQ(afd_receive(s, 64, 0, 0x20), 0u);
    EXPECT_EQ(received(6), "peekme");
    // udp_truncated: STATUS_BUFFER_OVERFLOW with the buffer filled;
    // udp_after_truncated: the next datagram, not the rest of this one.
    send_datagram(port, "0123456789abcdef");
    EXPECT_EQ(afd_receive(s, 4, 0, 0x20), 0x80000005u);
    EXPECT_EQ(mem_.read_u32(IOSB).value(), 0x80000005u);
    EXPECT_EQ(mem_.read_u32(IOSB + 4).value(), 4u);
    EXPECT_EQ(received(4), "0123");
    send_datagram(port, "next");
    EXPECT_EQ(afd_receive(s, 64, 0, 0x20), 0u);
    EXPECT_EQ(received(4), "next");
    (void)inv_->call("closesocket", {s});
}

TEST_F(Ws2AfdTest, InvalidRequestsFailWithoutTouchingTheStatusBlock) {
    u16 port = 0;
    const u32 s = bound_udp(port);
    // short_input, tdi_0, peek alone, tdi_100: STATUS_INVALID_PARAMETER.
    EXPECT_EQ(afd_receive(s, 64, 0, 0x20, 12), 0xC000000Du);
    EXPECT_EQ(afd_receive(s, 64, 0, 0x0), 0xC000000Du);
    EXPECT_EQ(afd_receive(s, 64, 0, 0x80), 0xC000000Du);
    EXPECT_EQ(afd_receive(s, 64, 0, 0x100), 0xC000000Du);
    // tdi_40: an expedited receive is not supported on a datagram socket.
    EXPECT_EQ(afd_receive(s, 64, 0, 0x40), 0xC00000BBu);
    // unknown_afd_code: STATUS_INVALID_DEVICE_REQUEST.
    EXPECT_EQ(afd_receive(s, 64, 0, 0x20, 16, 0x00012FFFu), 0xC0000010u);
    EXPECT_EQ(mem_.read_u32(IOSB).value(), kUntouched);
    EXPECT_EQ(delivered().count, 0);
    (void)inv_->call("closesocket", {s});
}

TEST_F(Ws2AfdTest, NothingQueuedFailsOnANonblockingSocketOrPendsWhenOverlapped) {
    u16 port = 0;
    const u32 s = bound_udp(port);
    EXPECT_TRUE(mem_.write_u32(OUT_ADDR + 0x700, 1u).is_ok());
    ASSERT_EQ(inv_->call("ioctlsocket", {s, 0x8004667Eu, OUT_ADDR + 0x700}), 0u);  // FIONBIO=1
    // nonblocking_empty: STATUS_DEVICE_NOT_READY, status block untouched.
    EXPECT_EQ(afd_receive(s, 64, 0, 0x20), 0xC00000A3u);
    EXPECT_EQ(mem_.read_u32(IOSB).value(), kUntouched);
    // event_pending / iocp_pending: AFD_OVERLAPPED pends and leaves the status
    // block alone; the datagram's arrival completes it asynchronously.
    EXPECT_EQ(afd_receive(s, 64, 0x2, 0x20), 0x103u);
    EXPECT_EQ(mem_.read_u32(IOSB).value(), kUntouched);
    send_datagram(port, "late");
    for (int i = 0; i < 100 && delivered().count == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const Delivered d = delivered();
    ASSERT_EQ(d.count, 1);
    EXPECT_FALSE(d.synchronous);
    EXPECT_EQ(d.apc_context, IOSB);
    EXPECT_EQ(d.bytes, 4u);
    EXPECT_EQ(mem_.read_u32(IOSB).value(), 0u);
    EXPECT_EQ(mem_.read_u32(IOSB + 4).value(), 4u);
    EXPECT_EQ(received(4), "late");
    (void)inv_->call("closesocket", {s});
}
