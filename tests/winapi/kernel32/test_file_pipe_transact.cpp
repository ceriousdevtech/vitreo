/// @file test_file_pipe_transact.cpp
/// @brief Unit tests for DisconnectNamedPipe reconnect and TransactNamedPipe request/reply

#include "kernel32_file_fixture.hpp"

TEST_F(FileConsoleBridgeTest, DisconnectNamedPipeAllowsNextClient) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "disabled by VITREO_IPC=0";
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vtm_reconnect_" + std::to_string(::getpid());

    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle first_client =
        fm.create_file(pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    ASSERT_NE(first_client, INVALID_HANDLE);
    EXPECT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);
    EXPECT_EQ(fm.connect_named_pipe(server, 0),
              PipeConnectResult::AlreadyConnected);

    EXPECT_EQ(fm.disconnect_named_pipe(server),
              PipeDisconnectResult::Disconnected);
    EXPECT_EQ(fm.disconnect_named_pipe(server),
              PipeDisconnectResult::NotConnected);

    const Handle second_client =
        fm.create_file(pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    ASSERT_NE(second_client, INVALID_HANDLE);
    EXPECT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);
    EXPECT_EQ(fm.disconnect_named_pipe(server),
              PipeDisconnectResult::Disconnected);
}

TEST_F(FileConsoleBridgeTest, TransactNamedPipeWritesRequestAndReadsReply) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "disabled by VITREO_IPC=0";
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe = "\\\\.\\pipe\\vtm_transact_" + std::to_string(::getpid());

    Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    Handle client = fm.create_file(pipe, GENERIC_READ | GENERIC_WRITE, 0, 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);

    constexpr GuestAddr kRequest = 0x10400;
    constexpr GuestAddr kReply = 0x10500;
    constexpr GuestAddr kServerBuffer = 0x10600;
    constexpr GuestAddr kServerCount = 0x10700;
    constexpr GuestAddr kClientCount = 0x10800;
    const u8 request[] = {'p', 'i', 'n', 'g'};
    const u8 reply[] = {'p', 'o', 'n', 'g'};
    ASSERT_TRUE(guest_mem.write_bytes(kRequest, reinterpret_cast<const Byte*>(request), 4).is_ok());

    std::thread responder([&] {
        ASSERT_TRUE(fm.read_file(server, kServerBuffer, 4, kServerCount));
        ASSERT_TRUE(guest_mem.write_bytes(
            kServerBuffer, reinterpret_cast<const Byte*>(reply), 4).is_ok());
        ASSERT_TRUE(fm.write_file(server, kServerBuffer, 4, kServerCount));
    });

    EXPECT_TRUE(fm.transact_named_pipe(client, kRequest, 4, kReply, 4, kClientCount));
    responder.join();
    EXPECT_EQ(guest_mem.read_u32(kClientCount).value(), 4u);
    u8 received[4]{};
    ASSERT_TRUE(guest_mem.read_bytes(kReply, reinterpret_cast<Byte*>(received), 4).is_ok());
    EXPECT_EQ(std::memcmp(received, reply, 4), 0);
}
