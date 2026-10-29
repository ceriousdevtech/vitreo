/// @file test_kernel32_cpuz_iocp.cpp
/// @brief Unit tests for KERNEL32 overlapped file completion and I/O completion port contracts

#include "kernel32_cpuz_fixture.hpp"
#include <atomic>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file_pipe_internal.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

TEST_F(Kernel32CpuzTest, OverlappedFileCompletionSignalsEventAndQueuesPacket) {
    constexpr GuestAddr overlapped = DATA_BASE + 0x100;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x200;
    constexpr GuestAddr key_out = DATA_BASE + 0x204;
    constexpr GuestAddr overlapped_out = DATA_BASE + 0x208;
    const GuestAddr stack = cpu.sp();
    const Handle event = sync.create_event(false, false, "");
    const Handle file = handles.allocate(HandleType::File, nullptr, 0);
    ASSERT_NE(event, NULL_HANDLE);
    ASSERT_NE(file, NULL_HANDLE);

    // Create an unassociated completion port.
    ASSERT_TRUE(memory.write_u32(stack + 4, INVALID_HANDLE).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    // Associate the file with a caller-chosen completion key.
    ASSERT_TRUE(memory.write_u32(stack + 4, file).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x12345678).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    ASSERT_EQ(cpu.gpr.rax, port);

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0x103).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, event).is_ok());
    mgr.complete_overlapped_file_io(file, 802, overlapped, false);

    EXPECT_EQ(memory.read_u32(overlapped + 0).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(overlapped + 4).value_or(0), 802u);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, overlapped_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(bytes_out).value_or(0), 802u);
    EXPECT_EQ(memory.read_u32(key_out).value_or(0), 0x12345678u);
    EXPECT_EQ(memory.read_u32(overlapped_out).value_or(0), overlapped);
}

TEST_F(Kernel32CpuzTest, CreateIoCompletionPortAcceptsWs2SocketHandles) {
    // Battle.net associates WSASocket handles with an IOCP before connect().
    // Vitreo sockets are not HandleTable File/Pipe entries; rejecting them
    // with ERROR_INVALID_HANDLE aborts TCP before Winsock connect runs.
    const GuestAddr stack = cpu.sp();
    constexpr u32 kFakeSocket = 0x10058;

    ASSERT_TRUE(memory.write_u32(stack + 4, INVALID_HANDLE).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    // Without a probe, unknown handles must still fail honestly.
    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x42).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    mgr.set_socket_handle_probe(
        [](u32 handle) { return handle == kFakeSocket; });

    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x42).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    EXPECT_EQ(cpu.gpr.rax, port);

    // SetFileCompletionNotificationModes accepts the same Winsock IFS handle.
    // Steam sets FILE_SKIP_SET_EVENT_ON_HANDLE immediately after association.
    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x02).is_ok());
    mgr.impl_SetFileCompletionNotificationModes(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    // Non-socket HandleTable types remain rejected.
    const Handle thread = handles.allocate(HandleType::Thread, nullptr, 0);
    ASSERT_NE(thread, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(stack + 4, thread).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, HandleInformationAcceptsWs2SocketHandles) {
    // libuv's uv__tcp_set_socket clears HANDLE_FLAG_INHERIT on every socket it
    // creates and returns GetLastError() when the call fails, so rejecting a
    // socket here closes it between FIONBIO and connect(). A socket is a kernel
    // handle on Windows; socket() returns it inheritable.
    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr flags_out = DATA_BASE + 0x240;
    constexpr u32 kFakeSocket = 0x10059;
    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);

    // Without a probe an unknown handle still fails honestly.
    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x0).is_ok());
    mgr.impl_SetHandleInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    mgr.set_socket_handle_probe(
        [](u32 handle) { return handle == kFakeSocket; });

    // A socket that has never been written reads back inheritable.
    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, flags_out).is_ok());
    mgr.impl_GetHandleInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(flags_out).value_or(0xFFFFFFFFu), 1u);

    // Clearing HANDLE_FLAG_INHERIT succeeds and is what reads back.
    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x0).is_ok());
    mgr.impl_SetHandleInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, flags_out).is_ok());
    mgr.impl_GetHandleInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(flags_out).value_or(0xFFFFFFFFu), 0u);

    // The mask still scopes the write: setting PROTECT_FROM_CLOSE alone leaves
    // the cleared inherit bit alone.
    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x2).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x3).is_ok());
    mgr.impl_SetHandleInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(stack + 4, kFakeSocket).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, flags_out).is_ok());
    mgr.impl_GetHandleInformation(marshaler);
    EXPECT_EQ(memory.read_u32(flags_out).value_or(0xFFFFFFFFu), 2u);

    // A handle that is neither a table entry nor a socket is still rejected.
    ASSERT_TRUE(memory.write_u32(stack + 4, 0x10060).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x1).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x0).is_ok());
    mgr.impl_SetHandleInformation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, CloseHandleTearsDownIoCompletionPort) {
    constexpr GuestAddr bytes_out = DATA_BASE + 0x200;
    constexpr GuestAddr key_out = DATA_BASE + 0x204;
    constexpr GuestAddr overlapped_out = DATA_BASE + 0x208;
    const GuestAddr stack = cpu.sp();

    handles.set_close_callback([this](Handle h, HandleType type, void*) {
        if (type == HandleType::IoCompletionPort) {
            return mgr.on_iocp_port_close(h);
        }
        return true;
    });

    ASSERT_TRUE(memory.write_u32(stack + 4, INVALID_HANDLE).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);
    EXPECT_EQ(handles.get_type(port), HandleType::IoCompletionPort);
    // Must not use the legacy synthetic 0xCC namespace when a HandleTable is wired.
    EXPECT_NE(port & 0xFF000000u, 0xCC000000u);

    ASSERT_TRUE(handles.close(port));
    EXPECT_EQ(handles.get_type(port), HandleType::Invalid);

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x55).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_PostQueuedCompletionStatus(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, overlapped_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest,
       BeginShutdownCancelsInfiniteCompletionPortWaitWithoutPostingPacket) {
    constexpr GuestAddr bytes_out = DATA_BASE + 0x200;
    constexpr GuestAddr key_out = DATA_BASE + 0x204;
    constexpr GuestAddr overlapped_out = DATA_BASE + 0x208;
    const GuestAddr stack = cpu.sp();

    ASSERT_TRUE(memory.write_u32(stack + 4, INVALID_HANDLE).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(overlapped_out, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, overlapped_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0xFFFFFFFFu).is_ok());

    std::atomic<bool> started{false};
    std::thread waiter([&]() {
        started.store(true, std::memory_order_release);
        mgr.impl_GetQueuedCompletionStatus(marshaler);
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    mgr.begin_shutdown();
    waiter.join();

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(overlapped_out).value_or(1), 0u);
}

TEST_F(Kernel32CpuzTest, CancelIo_InvalidHandleFails) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0xBEEF);  // hFile
    set_last_error(error_codes::ERROR_SUCCESS);
    mgr.impl_CancelIo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// Overlapped I/O
// =============================================================================

TEST_F(Kernel32CpuzTest, GetOverlappedResultReturnsCompletedByteCount) {
    constexpr GuestAddr overlapped = DATA_BASE + 0x480;
    constexpr GuestAddr bytes_addr = DATA_BASE + 0x500;
    const Handle file = handles.allocate(HandleType::File, nullptr, 0);
    ASSERT_NE(file, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 802).is_ok());
    (void)memory.write_u32(bytes_addr, 0xDEAD);  // pre-fill

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  file);        // hFile
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  overlapped);  // lpOverlapped
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), bytes_addr);  // lpBytesTransferred
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);           // bWait
    mgr.impl_GetOverlappedResult(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    auto r = memory.read_u32(bytes_addr);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 802u);
}

TEST_F(Kernel32CpuzTest, GetOverlappedResultReportsIncompletePendingOperation) {
    constexpr GuestAddr overlapped = DATA_BASE + 0x480;
    constexpr GuestAddr bytes_addr = DATA_BASE + 0x500;
    const Handle file = handles.allocate(HandleType::File, nullptr, 0);
    ASSERT_NE(file, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0x103).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, file).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, overlapped).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, bytes_addr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    mgr.impl_GetOverlappedResult(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_INCOMPLETE);
}
