/// @file test_ws2_32_contracts_overlapped.cpp
/// @brief ws2_32 contract tests for overlapped I/O, completion routines, event
///        waits and TransmitFile.
///
/// The shared fixture and the provenance of the expected values are documented
/// in ws2_32_contract_fixture.hpp.

#include "ws2_32_contract_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using namespace vitreo::winapi::ws2_contract_test;

// ws2_ioctl_ov_probe.expected: overlapped FIONREAD completes immediately
// (rc=0, bytes=4, Internal=0, InternalHigh=0, hEvent signaled).
TEST_P(Ws2ContractTest, WsaIoctlOverlappedCompletesImmediately) {
    startup();
    const u64 s = call("socket", {2, 1, 6});
    ASSERT_NE(s, invalid_socket());
    const Handle hevent = sync_->create_event(false, false, "");
    ASSERT_NE(hevent, 0u);
    ASSERT_EQ(sync_->wait_for_single_object(hevent, 0), WAIT_TIMEOUT);
    const GuestAddr ov = kData + 0x100;
    const GuestAddr out = kData + 0x200;
    const GuestAddr ret = kData + 0x210;
    (void)mem_.write_u32(out, 0xCCCC);
    (void)mem_.write_u32(ret, 0xDEAD);
    if (is64()) {
        (void)mem_.write_u64(ov, 0xBEEF);
        (void)mem_.write_u64(ov + 8, 0xBEEF);
        (void)mem_.write_u64(ov + 24, hevent);
    } else {
        (void)mem_.write_u32(ov, 0xBEEF);
        (void)mem_.write_u32(ov + 4, 0xBEEF);
        (void)mem_.write_u32(ov + 16, hevent);
    }
    ASSERT_EQ(call("WSAIoctl", {s, 0x4004667F, 0, 0, out, 4, ret, ov, 0}), 0u);
    EXPECT_EQ(mem_.read_u32(ret).value(), 4u);
    EXPECT_EQ(mem_.read_u32(out).value(), 0u);
    if (is64()) {
        EXPECT_EQ(mem_.read_u64(ov).value(), 0u);       // Internal STATUS_SUCCESS
        EXPECT_EQ(mem_.read_u64(ov + 8).value(), 0u);   // InternalHigh 0
    } else {
        EXPECT_EQ(mem_.read_u32(ov).value(), 0u);
        EXPECT_EQ(mem_.read_u32(ov + 4).value(), 0u);
    }
    EXPECT_EQ(sync_->wait_for_single_object(hevent, 0), WAIT_OBJECT_0);
}

// Learn stream WSARecv: multiple outstanding receives complete in issue order.
TEST_P(Ws2ContractTest, WsaRecvStreamOverlappedCompletesInIssueOrder) {
    startup();
    struct Done {
        GuestAddr ov = 0;
        u32 bytes = 0;
        u32 win = 0;
    };
    std::vector<Done> done;
    std::mutex mu;
    mgr_.set_overlapped_completion_callback(
        [&](Handle, u32 bytes, GuestAddr ov, bool, u32, u32 win, bool) {
            std::lock_guard<std::mutex> lk(mu);
            done.push_back(Done{ov, bytes, win});
        });

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

    const GuestAddr ov1 = kData + 0x100, ov2 = kData + 0x180;
    const GuestAddr b1 = kData + 0x200, b2 = kData + 0x210;
    const GuestAddr wb1 = kData + 0x300, wb2 = kData + 0x320;
    const GuestAddr nb = kData + 0x400, fl = kData + 0x404;
    (void)mem_.write_u32(wb1, 1);
    (void)mem_.write_u32(wb2, 1);
    if (is64()) {
        (void)mem_.write_u64(wb1 + 8, b1);
        (void)mem_.write_u64(wb2 + 8, b2);
        (void)mem_.write_u64(ov1, 0);
        (void)mem_.write_u64(ov1 + 8, 0);
        (void)mem_.write_u64(ov1 + 24, 0);
        (void)mem_.write_u64(ov2, 0);
        (void)mem_.write_u64(ov2 + 8, 0);
        (void)mem_.write_u64(ov2 + 24, 0);
    } else {
        (void)mem_.write_u32(wb1 + 4, static_cast<u32>(b1));
        (void)mem_.write_u32(wb2 + 4, static_cast<u32>(b2));
        (void)mem_.write_u32(ov1, 0);
        (void)mem_.write_u32(ov1 + 4, 0);
        (void)mem_.write_u32(ov1 + 16, 0);
        (void)mem_.write_u32(ov2, 0);
        (void)mem_.write_u32(ov2 + 4, 0);
        (void)mem_.write_u32(ov2 + 16, 0);
    }
    (void)mem_.write_u32(nb, 0);
    (void)mem_.write_u32(fl, 0);
    EXPECT_EQ(static_cast<u32>(
                  call("WSARecv", {server, wb1, 1, nb, fl, ov1, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 997u);  // WSA_IO_PENDING
    EXPECT_EQ(static_cast<u32>(
                  call("WSARecv", {server, wb2, 1, nb, fl, ov2, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 997u);
    (void)mem_.write_bytes(kData + 0x500, reinterpret_cast<const Byte*>("AB"), 2);
    ASSERT_EQ(call("send", {client, kData + 0x500, 2, 0}), 2u);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (done.size() >= 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(done.size(), 2u);
    EXPECT_EQ(done[0].ov, ov1);
    EXPECT_EQ(done[0].bytes, 1u);
    EXPECT_EQ(done[1].ov, ov2);
    EXPECT_EQ(done[1].bytes, 1u);
    EXPECT_EQ(mem_.read_u8(b1).value(), static_cast<u8>('A'));
    EXPECT_EQ(mem_.read_u8(b2).value(), static_cast<u8>('B'));
    mgr_.set_overlapped_completion_callback({});
}

// Learn WSAWaitForMultipleEvents: fAlertable drains user APCs and returns
// WSA_WAIT_IO_COMPLETION (0xC0).
TEST_P(Ws2ContractTest, WsaWaitForMultipleEventsAlertableReturnsIoCompletion) {
    startup();
    bool drained = false;
    sync_->set_alertable_apc_drain([&]() {
        if (drained) return false;
        drained = true;
        return true;
    });
    const Handle ev = sync_->create_event(false, false, "");
    ASSERT_NE(ev, 0u);
    const GuestAddr hbuf = kData + 0x100;
    if (is64()) (void)mem_.write_u64(hbuf, ev);
    else (void)mem_.write_u32(hbuf, ev);
    // count, handles, wait_all=0, timeout=INFINITE-ish, alertable=1
    EXPECT_EQ(call("WSAWaitForMultipleEvents",
                   {1, hbuf, 0, 50, 1}),
              0xC0u);
    EXPECT_TRUE(drained);
    sync_->set_alertable_apc_drain({});
}

// Learn WSASend: non-NULL lpCompletionRoutine ignores hEvent (app context) and
// queues the APC; Vitreo must not SetEvent that context value.
TEST_P(Ws2ContractTest, WsaSendCompletionRoutineDoesNotSignalHEvent) {
    startup();
    struct Queued {
        u32 tid = 0;
        GuestAddr pfn = 0;
        u32 error = 0;
        u32 bytes = 0;
        GuestAddr ov = 0;
        u32 flags = 0;
    };
    Queued seen{};
    mgr_.set_wsa_completion_routine_queue(
        [&](u32 tid, GuestAddr pfn, u32 err, u32 bytes, GuestAddr ov,
            u32 flags) {
            seen = {tid, pfn, err, bytes, ov, flags};
        });
    const u64 s = call("socket", {2, 2, 17});  // UDP — sendto-less WSASend needs connect
    ASSERT_NE(s, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, htons(9));  // discard
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("connect", {s, sa, 16}), 0u);
    const Handle hevent = sync_->create_event(false, false, "");
    ASSERT_NE(hevent, 0u);
    ASSERT_EQ(sync_->wait_for_single_object(hevent, 0), WAIT_TIMEOUT);
    const GuestAddr ov = kData + 0x100;
    const GuestAddr buf = kData + 0x200;
    const GuestAddr wsabuf = kData + 0x300;
    const GuestAddr bytes_sent = kData + 0x400;
    const GuestAddr routine = 0x401000;
    (void)mem_.write_bytes(buf, reinterpret_cast<const Byte*>("hi"), 2);
    (void)mem_.write_u32(wsabuf, 2);
    if (is64()) {
        (void)mem_.write_u64(wsabuf + 8, buf);
        (void)mem_.write_u64(ov, 0);
        (void)mem_.write_u64(ov + 8, 0);
        (void)mem_.write_u64(ov + 24, hevent);  // context, not an event to signal
    } else {
        (void)mem_.write_u32(wsabuf + 4, static_cast<u32>(buf));
        (void)mem_.write_u32(ov, 0);
        (void)mem_.write_u32(ov + 4, 0);
        (void)mem_.write_u32(ov + 16, hevent);
    }
    EXPECT_EQ(call("WSASend", {s, wsabuf, 1, bytes_sent, 0, ov, routine}), 0u);
    EXPECT_EQ(seen.pfn, routine);
    EXPECT_EQ(seen.error, 0u);
    EXPECT_EQ(seen.bytes, 2u);
    EXPECT_EQ(seen.ov, ov);
    EXPECT_EQ(sync_->wait_for_single_object(hevent, 0), WAIT_TIMEOUT);
    mgr_.clear_wsa_completion_routine_queue();
}

// Learn closesocket: pending overlapped I/O completes with WSA_OPERATION_ABORTED.
TEST_P(Ws2ContractTest, CloseSocketAbortsPendingOverlappedWithOperationAborted) {
    startup();
    struct Done {
        Handle h = 0;
        u32 bytes = 0;
        GuestAddr ov = 0;
        u32 nt = 0;
        u32 win = 0;
    };
    Done seen{};
    mgr_.set_overlapped_completion_callback(
        [&](Handle h, u32 bytes, GuestAddr ov, bool, u32 nt, u32 win, bool) {
            seen = {h, bytes, ov, nt, win};
        });
    // TCP listen + accept so a peer can connect; post overlapped recv that
    // stays pending until closesocket.
    const u64 listener = call("socket", {2, 1, 6});
    ASSERT_NE(listener, invalid_socket());
    const GuestAddr sa = kData;
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, 0);  // any port
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("bind", {listener, sa, 16}), 0u);
    ASSERT_EQ(call("listen", {listener, 1}), 0u);
    const GuestAddr namelen = kData + 0x80;
    (void)mem_.write_u32(namelen, 16);
    ASSERT_EQ(call("getsockname", {listener, sa, namelen}), 0u);
    const u16 port = ntohs(mem_.read_u16(sa + 2).value());
    const u64 client = call("socket", {2, 1, 6});
    ASSERT_NE(client, invalid_socket());
    (void)mem_.write_u16(sa, 2);
    (void)mem_.write_u16(sa + 2, htons(port));
    (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    ASSERT_EQ(call("connect", {client, sa, 16}), 0u);
    const u64 server = call("accept", {listener, 0, 0});
    ASSERT_NE(server, invalid_socket());
    const GuestAddr ov = kData + 0x100;
    const GuestAddr buf = kData + 0x200;
    const GuestAddr wsabuf = kData + 0x300;
    const GuestAddr bytes_recvd = kData + 0x400;
    const GuestAddr flags = kData + 0x404;
    (void)mem_.write_u32(wsabuf, 64);
    if (is64()) {
        (void)mem_.write_u64(wsabuf + 8, buf);
        (void)mem_.write_u64(ov, 0x103);  // STATUS_PENDING placeholder
        (void)mem_.write_u64(ov + 8, 0);
        (void)mem_.write_u64(ov + 24, 0);
    } else {
        (void)mem_.write_u32(wsabuf + 4, static_cast<u32>(buf));
        (void)mem_.write_u32(ov, 0x103);
        (void)mem_.write_u32(ov + 4, 0);
        (void)mem_.write_u32(ov + 16, 0);
    }
    (void)mem_.write_u32(flags, 0);
    EXPECT_EQ(static_cast<u32>(call("WSARecv", {server, wsabuf, 1, bytes_recvd, flags, ov, 0})),
              0xFFFFFFFFu);
    EXPECT_EQ(wsa_error(), 997u);  // WSA_IO_PENDING
    EXPECT_EQ(call("closesocket", {server}), 0u);
    EXPECT_EQ(seen.ov, ov);
    // ws2_ovres_probe closed_under_recv / close_tcp_recv_routine: a pending
    // receive on a CONNECTED stream completes with STATUS_CONNECTION_ABORTED,
    // and a completion routine is told WSAECONNABORTED (10053). (Learn's
    // WSA_OPERATION_ABORTED is what a datagram socket gets: close_udp_recv.)
    EXPECT_EQ(seen.win, 10053u);
    EXPECT_EQ(seen.nt, 0xC0000241u);
    if (is64()) {
        EXPECT_EQ(mem_.read_u64(ov).value(), 0xC0000241u);
    } else {
        EXPECT_EQ(mem_.read_u32(ov).value(), 0xC0000241u);
    }
    mgr_.clear_overlapped_completion_callback();
    (void)call("closesocket", {client});
    (void)call("closesocket", {listener});
}

// ws2_transmit_probe.expected: TransmitFile sends file bytes (and optional
// head/tail); TransmitPackets sends memory+file elements. File pointer is
// unchanged after TransmitFile (tf_full_fptr / tf_partial4_fptr pos=0).
TEST_P(Ws2ContractTest, TransmitFileAndPacketsSendFileBytes) {
    startup();
    const auto root = std::filesystem::temp_directory_path() /
                      ("vitreo_tf_" + std::to_string(::getpid()) +
                       (is64() ? "_64" : "_32"));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "payload.bin", std::ios::binary).write("ABCDEFGHIJ", 10);

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(root.string()))
                    .is_ok());
    auto files = std::make_unique<FileManager>(vfs, *handles_, mem_);
    mgr_.set_file_manager(files.get());

    const Handle h = files->create_file(
        "C:\\payload.bin", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

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
    const u64 a = call("accept", {listener, 0, 0});
    ASSERT_NE(a, invalid_socket());

    auto recv_exact = [&](u64 sock, GuestAddr dest, u32 n) {
        u32 got = 0;
        while (got < n) {
            const u64 r = call("recv", {sock, dest + got, n - got, 0});
            ASSERT_NE(static_cast<u32>(r), 0xFFFFFFFFu) << "err=" << wsa_error();
            ASSERT_GT(r, 0u);
            got += static_cast<u32>(r);
        }
    };

    // tf_full: entire file "ABCDEFGHIJ"
    EXPECT_EQ(call("TransmitFile", {c, h, 0, 0, 0, 0, 0}), 1u);
    (void)mem_.write_bytes(kData + 0x100, std::vector<Byte>(16, 0).data(), 16);
    recv_exact(a, kData + 0x100, 10);
    EXPECT_EQ(str(kData + 0x100), "ABCDEFGHIJ");
    // Pointer unchanged (oracle tf_full_fptr pos=0).
    EXPECT_EQ(files->set_file_pointer(h, 0, nullptr, FILE_CURRENT), 0u);

    // tf_partial4: first 4 bytes; pointer still 0.
    EXPECT_EQ(call("TransmitFile", {c, h, 4, 0, 0, 0, 0}), 1u);
    (void)mem_.write_bytes(kData + 0x100, std::vector<Byte>(8, 0).data(), 8);
    recv_exact(a, kData + 0x100, 4);
    EXPECT_EQ(str(kData + 0x100), "ABCD");
    EXPECT_EQ(files->set_file_pointer(h, 0, nullptr, FILE_CURRENT), 0u);

    // Seek to 4; head+file rest+tail -> "HDREFGHIJTLR"
    EXPECT_EQ(files->set_file_pointer(h, 4, nullptr, FILE_BEGIN), 4u);
    const GuestAddr tb = kData + 0x300;
    const GuestAddr head = kData + 0x380;
    const GuestAddr tail = kData + 0x390;
    (void)mem_.write_bytes(head, reinterpret_cast<const Byte*>("HDR"), 3);
    (void)mem_.write_bytes(tail, reinterpret_cast<const Byte*>("TLR"), 3);
    if (is64()) {
        (void)mem_.write_u64(tb, head);
        (void)mem_.write_u32(tb + 8, 3);
        (void)mem_.write_u64(tb + 16, tail);
        (void)mem_.write_u32(tb + 24, 3);
    } else {
        (void)mem_.write_u32(tb, static_cast<u32>(head));
        (void)mem_.write_u32(tb + 4, 3);
        (void)mem_.write_u32(tb + 8, static_cast<u32>(tail));
        (void)mem_.write_u32(tb + 12, 3);
    }
    EXPECT_EQ(call("TransmitFile", {c, h, 0, 0, 0, tb, 0}), 1u);
    (void)mem_.write_bytes(kData + 0x100, std::vector<Byte>(16, 0).data(), 16);
    recv_exact(a, kData + 0x100, 12);
    EXPECT_EQ(str(kData + 0x100), "HDREFGHIJTLR");

    // TransmitPackets: MEM + file offset 2 length 3 -> "MEM234"
    std::ofstream(root / "tp.bin", std::ios::binary).write("0123456789", 10);
    const Handle htp = files->create_file(
        "C:\\tp.bin", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(htp, INVALID_HANDLE);
    const GuestAddr el = kData + 0x500;
    const GuestAddr mem_el = kData + 0x580;
    (void)mem_.write_bytes(mem_el, reinterpret_cast<const Byte*>("MEM"), 3);
    // element 0: MEMORY
    (void)mem_.write_u32(el, 1);  // TP_ELEMENT_MEMORY
    (void)mem_.write_u32(el + 4, 3);
    if (is64()) (void)mem_.write_u64(el + 8, mem_el);
    else (void)mem_.write_u32(el + 8, static_cast<u32>(mem_el));
    // element 1: FILE offset 2, length 3
    (void)mem_.write_u32(el + 24, 2);  // TP_ELEMENT_FILE
    (void)mem_.write_u32(el + 28, 3);
    (void)mem_.write_u64(el + 32, 2);  // nFileOffset
    if (is64()) (void)mem_.write_u64(el + 40, htp);
    else (void)mem_.write_u32(el + 40, htp);
    EXPECT_EQ(call("TransmitPackets", {c, el, 2, 0, 0, 0}), 1u);
    (void)mem_.write_bytes(kData + 0x100, std::vector<Byte>(16, 0).data(), 16);
    recv_exact(a, kData + 0x100, 6);
    EXPECT_EQ(str(kData + 0x100), "MEM234");

    mgr_.set_file_manager(nullptr);
    files.reset();
    std::filesystem::remove_all(root);
}

}  // namespace
