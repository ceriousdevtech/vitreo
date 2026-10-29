/// @file test_kernel32_cpuz_overlapped_pipes.cpp
/// @brief Unit tests for KERNEL32 overlapped named-pipe I/O contracts

#include "kernel32_cpuz_fixture.hpp"
#include <chrono>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file_pipe_internal.hpp>
#include <ipc/ipc_gate.hpp>
#include <array>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

TEST_F(Kernel32CpuzTest,
       OverlappedConnectNamedPipeReturnsPendingAndSignalsOnClientConnect) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";

    constexpr GuestAddr overlapped = DATA_BASE + 0x300;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x340;
    const GuestAddr stack = cpu.sp();
    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });

    const std::string pipe =
        "\\\\.\\pipe\\vtm_async_connect_" + std::to_string(::getpid());
    const Handle server = files.create_named_pipe_server(pipe);
    const Handle event = sync.create_event(
        /*manual_reset=*/true, /*initial_state=*/true, "");
    ASSERT_NE(server, INVALID_HANDLE);
    ASSERT_NE(event, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, event).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());

    mgr.impl_ConnectNamedPipe(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0x103u);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_TIMEOUT);

    const Handle client = files.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(sync.wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(overlapped + 4).value_or(1), 0u);

    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_GetOverlappedResult(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(bytes_out).value_or(1), 0u);

    EXPECT_TRUE(handles.close(client));
    EXPECT_TRUE(handles.close(server));
    EXPECT_TRUE(handles.close(event));
    handles.set_close_callback(nullptr);
    mgr.set_file_manager(nullptr);
}

// CancelIoEx on a pending overlapped ConnectNamedPipe completes it with
// STATUS_CANCELLED / ERROR_OPERATION_ABORTED right away
// (tools/oracle_probe/s4_pipe_connect_probe.expected cancelioex_*). The
// connect used to be an uncancellable 100 ms accept loop that CancelIoEx
// reported as ERROR_NOT_FOUND and that kept accepting after CloseHandle.
TEST_F(Kernel32CpuzTest, CancelIoExAbortsPendingOverlappedConnectNamedPipe) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";

    constexpr GuestAddr overlapped = DATA_BASE + 0x300;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x340;
    const GuestAddr stack = cpu.sp();
    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });
    const std::string pipe =
        "\\\\.\\pipe\\vtm_cancel_connect_" + std::to_string(::getpid());
    const Handle server = files.create_named_pipe_server(pipe);
    const Handle event = sync.create_event(true, false, "");
    ASSERT_NE(server, INVALID_HANDLE);

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, event).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
    mgr.impl_ConnectNamedPipe(marshaler);
    ASSERT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);

    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
    mgr.impl_CancelIoEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);                                     // cancelioex r=1
    ASSERT_EQ(sync.wait_for_single_object(event, 1000), WAIT_OBJECT_0);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0xC0000120u); // internal
    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_GetOverlappedResult(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_OPERATION_ABORTED);
    // Nothing is left to cancel (cancelioex_nothing_pending gle=490).
    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
    mgr.impl_CancelIoEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_FOUND);

    EXPECT_TRUE(handles.close(server));
    EXPECT_TRUE(handles.close(event));
    handles.set_close_callback(nullptr);
    mgr.set_file_manager(nullptr);
}

// DisconnectNamedPipe ends a pending overlapped ConnectNamedPipe: TRUE, the
// event is signalled, Internal = STATUS_PIPE_DISCONNECTED and
// GetOverlappedResult fails with ERROR_PIPE_NOT_CONNECTED. A second
// disconnect fails with ERROR_PIPE_NOT_CONNECTED, the pipe can be armed
// again, and Disconnect + CloseHandle keeps STATUS_PIPE_DISCONNECTED
// (tests/oracle_data/pipe_pending_connect_end_probe{32,64}.expected).
// The connect used to stay STATUS_PENDING forever, hanging a
// GetOverlappedResult(bWait=TRUE) waiter on shutdown.
TEST_F(Kernel32CpuzTest, DisconnectNamedPipeCompletesPendingOverlappedConnect) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";

    constexpr GuestAddr overlapped = DATA_BASE + 0x300;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x340;
    const GuestAddr stack = cpu.sp();
    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });
    const std::string pipe =
        "\\\\.\\pipe\\vtm_disconnect_connect_" + std::to_string(::getpid());
    const Handle event = sync.create_event(true, false, "");

    auto arm = [&](Handle server) {
        ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(overlapped + 16, event).is_ok());
        ASSERT_TRUE(sync.reset_event(event));
        ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
        mgr.impl_ConnectNamedPipe(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        ASSERT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);    // err=3e5
        EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0x103u);     // Internal=103
    };
    auto disconnect = [&](Handle server) {
        ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
        set_last_error(0);
        mgr.impl_DisconnectNamedPipe(marshaler);
    };
    auto overlapped_result = [&](Handle server) {
        ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, overlapped).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 12, bytes_out).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
        mgr.impl_GetOverlappedResult(marshaler);
    };

    // == disconnect while connect pending
    const Handle server = files.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    arm(server);
    disconnect(server);
    EXPECT_EQ(cpu.gpr.rax, 1u);                                         // -> 1 err=0
    EXPECT_EQ(get_last_error(), 0u);
    ASSERT_EQ(sync.wait_for_single_object(event, 2000), WAIT_OBJECT_0); // signaled
    EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0xC00000B0u);    // Internal
    EXPECT_EQ(memory.read_u32(overlapped + 4).value_or(1), 0u);         // InternalHigh
    overlapped_result(server);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PIPE_NOT_CONNECTED); // err=e9
    // Already disconnected (== disconnect ... then disconnect again).
    disconnect(server);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PIPE_NOT_CONNECTED);
    // -- second connect after disconnect, ended by CancelIo
    arm(server);
    ASSERT_TRUE(memory.write_u32(stack + 4, server).is_ok());
    mgr.impl_CancelIo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    ASSERT_EQ(sync.wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0xC0000120u);
    overlapped_result(server);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_OPERATION_ABORTED);  // err=3e3
    EXPECT_TRUE(handles.close(server));

    // == disconnect then close while connect pending
    const Handle server2 = files.create_named_pipe_server(pipe);
    ASSERT_NE(server2, INVALID_HANDLE);
    arm(server2);
    disconnect(server2);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_TRUE(handles.close(server2));
    ASSERT_EQ(sync.wait_for_single_object(event, 2000), WAIT_OBJECT_0);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0xC00000B0u);

    // == disconnect with no connect armed
    const Handle server3 = files.create_named_pipe_server(pipe);
    ASSERT_NE(server3, INVALID_HANDLE);
    disconnect(server3);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_TRUE(handles.close(server3));

    EXPECT_TRUE(handles.close(event));
    handles.set_close_callback(nullptr);
    mgr.set_file_manager(nullptr);
}

TEST_F(Kernel32CpuzTest,
       OverlappedPipeReadReturnsPendingAndQueuesDataCompletion) {
    constexpr GuestAddr overlapped = DATA_BASE + 0x500;
    constexpr GuestAddr read_buffer = DATA_BASE + 0x600;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x700;
    constexpr GuestAddr key_out = DATA_BASE + 0x704;
    constexpr GuestAddr overlapped_out = DATA_BASE + 0x708;
    constexpr GuestAddr write_buffer = DATA_BASE + 0x800;
    const GuestAddr stack = cpu.sp();

    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });

    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(files.create_anonymous_pipe(
        read_handle, write_handle, /*inheritable=*/false));

    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0xC0FFEEu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, read_buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, overlapped).is_ok());
    impl_ReadFile(marshaler, files);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(0), 0x103u);

    const std::array<u8, 4> payload{'m', 'o', 'j', 'o'};
    ASSERT_TRUE(memory.write_bytes(
        write_buffer, payload.data(), payload.size()).is_ok());
    u32 written = 0;
    ASSERT_TRUE(files.write_file(
        write_handle, write_buffer, payload.size(), 0, &written));
    ASSERT_EQ(written, payload.size());

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, overlapped_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 2000).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(bytes_out).value_or(0), payload.size());
    EXPECT_EQ(memory.read_u32(key_out).value_or(0), 0xC0FFEEu);
    EXPECT_EQ(memory.read_u32(overlapped_out).value_or(0), overlapped);
    EXPECT_EQ(memory.read_u32(overlapped).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(overlapped + 4).value_or(0), payload.size());
    for (usize index = 0; index < payload.size(); ++index) {
        EXPECT_EQ(memory.read_u8(read_buffer + index).value_or(0),
                  payload[index]);
    }

    mgr.set_file_manager(nullptr);
    EXPECT_TRUE(handles.close(read_handle));
    EXPECT_TRUE(handles.close(write_handle));
    handles.set_close_callback(nullptr);
}

TEST_F(Kernel32CpuzTest,
       OverlappedPipeReadWithAvailableDataCompletesSynchronously) {
    constexpr GuestAddr overlapped = DATA_BASE + 0x500;
    constexpr GuestAddr read_buffer = DATA_BASE + 0x600;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x700;
    constexpr GuestAddr key_out = DATA_BASE + 0x704;
    constexpr GuestAddr overlapped_out = DATA_BASE + 0x708;
    constexpr GuestAddr write_buffer = DATA_BASE + 0x800;
    const GuestAddr stack = cpu.sp();

    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });

    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(files.create_anonymous_pipe(
        read_handle, write_handle, /*inheritable=*/false));

    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0xC0FFEEu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    const std::array<u8, 4> payload{'m', 'o', 'j', 'o'};
    ASSERT_TRUE(memory.write_bytes(
        write_buffer, payload.data(), payload.size()).is_ok());
    u32 written = 0;
    ASSERT_TRUE(files.write_file(
        write_handle, write_buffer, payload.size(), 0, &written));
    ASSERT_EQ(written, payload.size());

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0x103u).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(bytes_out, 0xCCCCCCCCu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, read_buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, payload.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, overlapped).is_ok());
    impl_ReadFile(marshaler, files);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(bytes_out).value_or(0), payload.size());
    EXPECT_EQ(memory.read_u32(overlapped).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(overlapped + 4).value_or(0), payload.size());
    for (usize index = 0; index < payload.size(); ++index) {
        EXPECT_EQ(memory.read_u8(read_buffer + index).value_or(0),
                  payload[index]);
    }

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, overlapped_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(bytes_out).value_or(0), payload.size());
    EXPECT_EQ(memory.read_u32(key_out).value_or(0), 0xC0FFEEu);
    EXPECT_EQ(memory.read_u32(overlapped_out).value_or(0), overlapped);

    mgr.set_file_manager(nullptr);
    EXPECT_TRUE(handles.close(read_handle));
    EXPECT_TRUE(handles.close(write_handle));
    handles.set_close_callback(nullptr);
}

TEST_F(Kernel32CpuzTest, ClosedPipeReadStillQueuesCancellationToOriginalPort) {
    constexpr GuestAddr ov = DATA_BASE + 0x500;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x540;
    constexpr GuestAddr key_out = DATA_BASE + 0x544;
    constexpr GuestAddr ov_out = DATA_BASE + 0x548;
    const GuestAddr stack = cpu.sp();
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    auto endpoint = std::make_shared<PipeEndpoint>();
    endpoint->readable = true;
    endpoint->conn = ipc::UnixPipe::adopt_connected(sockets[0]);
    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        mgr.on_iocp_handle_close(h);
        return files.on_handle_close(h, type, object);
    });
    const Handle handle = allocate_pipe_endpoint(handles, endpoint);
    ASSERT_TRUE(memory.write_u32(stack + 4, handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x12345678).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);
    u32 count = 0;
    EXPECT_EQ(files.begin_overlapped_pipe_read(handle, 0, 0, ov, false, count),
              OverlappedPipeIoResult::Pending);
    EXPECT_TRUE(files.close_file(handle));
    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, ov_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 2000).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_OPERATION_ABORTED);
    EXPECT_EQ(memory.read_u32(ov_out).value_or(0), ov);
    EXPECT_EQ(memory.read_u32(key_out).value_or(0), 0x12345678u);
    files.shutdown_overlapped_pipe_io();
    files.clear_overlapped_completion_callback();
    mgr.set_file_manager(nullptr);
    handles.set_close_callback({});
    ::close(sockets[1]);
}

TEST_F(Kernel32CpuzTest, OverlappedPipeWritesRemainPendingUnderBackpressure) {
    constexpr GuestAddr buffer = 0x200000;
    constexpr u32 payload_size = 1024 * 1024;
    ASSERT_TRUE(memory.map_region(buffer, payload_size,
                                 MemoryProtection::ReadWrite).is_ok());
    std::vector<u8> payload(payload_size, 0xA7);
    ASSERT_TRUE(memory.write_bytes(buffer, payload.data(), payload.size()).is_ok());
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    const int send_size = 4096;
    ASSERT_EQ(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                           &send_size, sizeof(send_size)), 0);
    auto endpoint = std::make_shared<PipeEndpoint>();
    endpoint->writable = true;
    endpoint->sock = ipc::UnixPipe::adopt_connected(sockets[0]);
    ASSERT_NE(endpoint->sock, nullptr);
    FileManager files{vfs, handles, memory};
    const Handle handle = allocate_pipe_endpoint(handles, endpoint);
    u32 written = 0;
    EXPECT_EQ(files.begin_overlapped_pipe_write(handle, buffer, payload_size,
                  0, DATA_BASE + 0x500, false, written),
              OverlappedPipeIoResult::Pending);
    // Let the first worker encounter a full transport buffer. The next
    // submission must not wait for that worker (or for the peer to read).
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x800, 0x12345678).is_ok());
    auto second = std::async(std::launch::async, [&] {
        u32 count = 0;
        return files.begin_overlapped_pipe_write(handle, DATA_BASE + 0x800,
                     4, 0, DATA_BASE + 0x540, false, count);
    });
    const bool returned = second.wait_for(std::chrono::milliseconds(250)) ==
                          std::future_status::ready;
    EXPECT_TRUE(returned) << "Overlapped submission blocked behind a pending write";
    if (!returned) {
        (void)files.cancel_overlapped_pipe_io(handle, DATA_BASE + 0x500, false);
        (void)::shutdown(sockets[1], SHUT_RDWR);
    }
    const auto result = second.get();
    if (returned) {
        EXPECT_EQ(result, OverlappedPipeIoResult::Pending);
        std::vector<u8> received;
        std::array<u8, 8192> chunk;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (received.size() < payload_size + 4 &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{sockets[1], POLLIN, 0};
            if (::poll(&descriptor, 1, 50) <= 0) continue;
            const auto count = ::recv(sockets[1], chunk.data(), chunk.size(), 0);
            if (count <= 0) break;
            received.insert(received.end(), chunk.begin(), chunk.begin() + count);
        }
        payload.insert(payload.end(), {0x78, 0x56, 0x34, 0x12});
        EXPECT_EQ(received, payload) << "Pending write tails must not interleave";
        files.shutdown_overlapped_pipe_io();
        // Shutdown must release queued turns even when nobody drains the
        // transport. Reuse the OVERLAPPED records only after workers joined.
        EXPECT_EQ(files.begin_overlapped_pipe_write(handle, buffer, payload_size,
                      0, DATA_BASE + 0x500, false, written),
                  OverlappedPipeIoResult::Pending);
        EXPECT_EQ(files.begin_overlapped_pipe_write(handle, DATA_BASE + 0x800,
                      4, 0, DATA_BASE + 0x540, false, written),
                  OverlappedPipeIoResult::Pending);
        const auto shutdown_start = std::chrono::steady_clock::now();
        files.shutdown_overlapped_pipe_io();
        EXPECT_LT(std::chrono::steady_clock::now() - shutdown_start,
                  std::chrono::seconds(2));
    }
    (void)::shutdown(sockets[1], SHUT_RDWR);
    (void)::close(sockets[1]);
    files.shutdown_overlapped_pipe_io();
    EXPECT_TRUE(handles.close(handle));
}

TEST_F(Kernel32CpuzTest, ShutdownOverlappedPipeIoJoinsWithoutDeadlock) {
    // Regression: shutdown held m_overlapped_pipe_io_mutex while joining
    // jthreads that call consume_pending_pipe_io (same mutex). CEF network
    // and storage utilities then hung after TerminateProcess and never
    // exited the host pid, so Chromium never restarted them.
    constexpr GuestAddr overlapped = DATA_BASE + 0x500;
    constexpr GuestAddr read_buffer = DATA_BASE + 0x600;
    const GuestAddr stack = cpu.sp();

    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });

    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(files.create_anonymous_pipe(
        read_handle, write_handle, /*inheritable=*/false));

    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0xC0FFEEu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    ASSERT_NE(static_cast<Handle>(cpu.gpr.rax), NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, read_buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, overlapped).is_ok());
    impl_ReadFile(marshaler, files);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);

    const auto started = std::chrono::steady_clock::now();
    mgr.set_file_manager(nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::seconds(2));

    EXPECT_TRUE(handles.close(read_handle));
    EXPECT_TRUE(handles.close(write_handle));
    handles.set_close_callback(nullptr);
}

TEST_F(Kernel32CpuzTest,
       SkipCompletionPortOnSuccessStillQueuesAsyncPipeCompletions) {
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS must not drop packets for I/O that
    // previously returned ERROR_IO_PENDING. Chromium Mojo sets this flag on
    // its platform-channel pipes and waits on the completion port for async
    // reads; suppressing those packets stalls ChildProcessHost::Ping.
    constexpr GuestAddr overlapped = DATA_BASE + 0x500;
    constexpr GuestAddr read_buffer = DATA_BASE + 0x600;
    constexpr GuestAddr bytes_out = DATA_BASE + 0x700;
    constexpr GuestAddr key_out = DATA_BASE + 0x704;
    constexpr GuestAddr overlapped_out = DATA_BASE + 0x708;
    constexpr GuestAddr write_buffer = DATA_BASE + 0x800;
    const GuestAddr stack = cpu.sp();

    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    handles.set_close_callback(
        [&files](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });

    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(files.create_anonymous_pipe(
        read_handle, write_handle, /*inheritable=*/false));

    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0xC0FFEEu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    constexpr u8 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS = 0x01;
    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8,
                                 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS).is_ok());
    mgr.impl_SetFileCompletionNotificationModes(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(overlapped + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(overlapped + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, read_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, read_buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, overlapped).is_ok());
    impl_ReadFile(marshaler, files);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);

    const std::array<u8, 4> payload{'p', 'i', 'n', 'g'};
    ASSERT_TRUE(memory.write_bytes(
        write_buffer, payload.data(), payload.size()).is_ok());
    u32 written = 0;
    ASSERT_TRUE(files.write_file(
        write_handle, write_buffer, payload.size(), 0, &written));
    ASSERT_EQ(written, payload.size());

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, bytes_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, key_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, overlapped_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 2000).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(bytes_out).value_or(0), payload.size());
    EXPECT_EQ(memory.read_u32(key_out).value_or(0), 0xC0FFEEu);
    EXPECT_EQ(memory.read_u32(overlapped_out).value_or(0), overlapped);
    for (usize index = 0; index < payload.size(); ++index) {
        EXPECT_EQ(memory.read_u8(read_buffer + index).value_or(0),
                  payload[index]);
    }

    mgr.set_file_manager(nullptr);
    EXPECT_TRUE(handles.close(read_handle));
    EXPECT_TRUE(handles.close(write_handle));
    handles.set_close_callback(nullptr);
}
