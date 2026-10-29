/// @file test_file_named_pipe.cpp
/// @brief Unit tests for Windows named-pipe state (Chromium Mojo channel semantics): WaitNamedPipe, pipe modes, peer process ids

#include "kernel32_file_fixture.hpp"

// ---------------------------------------------------------------------------
// Windows named-pipe state (Chromium Mojo channel semantics)
// ---------------------------------------------------------------------------

namespace {
constexpr u32 kPipeReadModeByte    = 0x00000000;
constexpr u32 kPipeReadModeMessage = 0x00000002;
constexpr u32 kPipeWait            = 0x00000000;
constexpr u32 kPipeNoWait          = 0x00000001;
constexpr u32 kPipeClientEnd       = 0x00000000;
constexpr u32 kPipeServerEnd       = 0x00000001;
constexpr u32 kPipeTypeByte        = 0x00000000;
constexpr u32 kPipeTypeMessage     = 0x00000004;
}  // namespace

// Test 1: a named-pipe client reports pipe/client/byte and accepts byte+wait.
TEST_F(FileConsoleBridgeTest, WaitNamedPipeSeesExistingServerImmediately) {
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vitreo_wait_" + std::to_string(::getpid());

    EXPECT_FALSE(fm.wait_named_pipe(pipe, /*timeout_ms=*/0));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);

    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, NULL_HANDLE);
    // Success leaves the caller's last error untouched (measured on Windows 11:
    // tools/oracle_probe/last_error_on_success_probe.expected, WaitNamedPipeW).
    set_last_error(0xDEADu);
    EXPECT_TRUE(fm.wait_named_pipe(pipe, /*timeout_ms=*/1));
    EXPECT_EQ(get_last_error(), 0xDEADu);

    EXPECT_FALSE(fm.wait_named_pipe("not-a-pipe", 1));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_NAME);

    ASSERT_TRUE(handles.close(server));
}

TEST_F(FileConsoleBridgeTest, NamedPipeClientReportsWindowsState) {
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vitreo_state_" + std::to_string(::getpid());
    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, NULL_HANDLE);
    const Handle client = fm.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, /*OPEN_EXISTING*/ 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);

    EXPECT_EQ(handles.get_type(client), HandleType::Pipe);

    u32 flags = 0xFFFFFFFFu, out_size = 0, in_size = 0, instances = 0, err = 0;
    ASSERT_TRUE(fm.named_pipe_info(client, &flags, &out_size, &in_size,
                                   &instances, err));
    EXPECT_EQ(flags & kPipeServerEnd, kPipeClientEnd) << "it is the client end";
    EXPECT_EQ(flags & 0x4u, kPipeTypeByte) << "stream transport is byte type";
    EXPECT_GT(out_size, 0u);
    EXPECT_GT(in_size, 0u);
    EXPECT_GE(instances, 1u);

    u32 mode = kPipeReadModeByte | kPipeWait;
    ASSERT_TRUE(fm.set_named_pipe_handle_state(client, &mode, nullptr, nullptr,
                                               err));
    EXPECT_EQ(err, error_codes::ERROR_SUCCESS);

    // The server end must report itself as such.
    ASSERT_TRUE(fm.named_pipe_info(server, &flags, nullptr, nullptr, nullptr,
                                   err));
    EXPECT_EQ(flags & kPipeServerEnd, kPipeServerEnd);
}

// Test 2: a byte-type pipe must refuse message read mode rather than pretend.
TEST_F(FileConsoleBridgeTest, BytePipeRejectsMessageReadMode) {
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vitreo_msgmode_" + std::to_string(::getpid());
    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, NULL_HANDLE);
    const Handle client = fm.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, /*OPEN_EXISTING*/ 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);

    u32 mode = kPipeReadModeMessage | kPipeWait;
    u32 err = 0;
    EXPECT_FALSE(fm.set_named_pipe_handle_state(client, &mode, nullptr,
                                                nullptr, err));
    EXPECT_EQ(err, error_codes::ERROR_INVALID_PARAMETER);

    // The rejected request must not have changed anything.
    u32 flags = 0;
    ASSERT_TRUE(fm.named_pipe_info(client, &flags, nullptr, nullptr, nullptr,
                                   err));
    EXPECT_EQ(flags & 0x4u, kPipeTypeByte);
}

TEST_F(FileConsoleBridgeTest,
       MessagePipeAcceptsMessageModeAndPreservesPartialMessage) {
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vitreo_message_" + std::to_string(::getpid());
    NamedPipeConfig config{};
    config.message_type = true;
    config.message_read_mode = true;
    config.input_buffer_size = 512;
    config.output_buffer_size = 512;
    config.max_instances = 2;

    const Handle server =
        fm.create_named_pipe_server(pipe, false, config);
    ASSERT_NE(server, NULL_HANDLE);
    const Handle client = fm.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);

    u32 flags = 0, out_size = 0, in_size = 0, instances = 0, err = 0;
    ASSERT_TRUE(fm.named_pipe_info(
        client, &flags, &out_size, &in_size, &instances, err));
    EXPECT_EQ(flags & kPipeTypeMessage, kPipeTypeMessage);
    EXPECT_EQ(out_size, 512u);
    EXPECT_EQ(in_size, 512u);
    EXPECT_EQ(instances, 2u);

    u32 mode = kPipeReadModeMessage | kPipeWait;
    ASSERT_TRUE(fm.set_named_pipe_handle_state(
        client, &mode, nullptr, nullptr, err));
    EXPECT_EQ(err, error_codes::ERROR_SUCCESS);

    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>("hello"), 5).is_ok());
    ASSERT_TRUE(fm.write_file(server, kBufAddr, 5, kWrittenAddr));

    constexpr GuestAddr first_read = kBufAddr + 0x200;
    constexpr GuestAddr second_read = kBufAddr + 0x300;
    EXPECT_FALSE(fm.read_file(client, first_read, 3, kWrittenAddr));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MORE_DATA);
    EXPECT_EQ(guest_mem.read_u32(kWrittenAddr).value_or(0), 3u);
    std::array<Byte, 3> first{};
    ASSERT_TRUE(guest_mem.read_bytes(
        first_read, first.data(), first.size()).is_ok());
    EXPECT_EQ(std::memcmp(first.data(), "hel", first.size()), 0);

    set_last_error(0xDEADu);
    EXPECT_TRUE(fm.read_file(client, second_read, 8, kWrittenAddr));
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    EXPECT_EQ(guest_mem.read_u32(kWrittenAddr).value_or(0), 2u);
    std::array<Byte, 2> second{};
    ASSERT_TRUE(guest_mem.read_bytes(
        second_read, second.data(), second.size()).is_ok());
    EXPECT_EQ(std::memcmp(second.data(), "lo", second.size()), 0);
}

// Test 3: a non-pipe handle is ERROR_INVALID_HANDLE, not a silent success.
TEST_F(FileConsoleBridgeTest, SetNamedPipeHandleStateRejectsNonPipeHandle) {
    FileManager fm(vfs, handles, guest_mem);
    const Handle file = fm.create_file(
        "C:\\vitreo_pipe_state_test.txt", GENERIC_READ | GENERIC_WRITE, 0,
        /*CREATE_ALWAYS*/ 2, 0);
    ASSERT_NE(file, INVALID_HANDLE);

    u32 mode = kPipeReadModeByte | kPipeWait;
    u32 err = 0;
    EXPECT_FALSE(fm.set_named_pipe_handle_state(file, &mode, nullptr, nullptr,
                                                err));
    EXPECT_EQ(err, error_codes::ERROR_INVALID_HANDLE);

    u32 flags = 0;
    EXPECT_FALSE(fm.named_pipe_info(file, &flags, nullptr, nullptr, nullptr,
                                    err));
    EXPECT_EQ(err, error_codes::ERROR_INVALID_HANDLE);
}

// Test 4: the Mojo shape — a configured named-pipe client survives inheritance
// at the same handle value, keeps its Windows-visible state, and still carries
// data both ways.
TEST_F(FileConsoleBridgeTest, InheritedMojoShapedPipeKeepsStateAndHandle) {
    FileManager parent(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vitreo_mojo_state_" + std::to_string(::getpid());
    const Handle server = parent.create_named_pipe_server(pipe);
    ASSERT_NE(server, NULL_HANDLE);
    const Handle client = parent.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, /*OPEN_EXISTING*/ 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(parent.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);

    u32 err = 0;
    u32 mode = kPipeReadModeByte | kPipeNoWait;
    ASSERT_TRUE(parent.set_named_pipe_handle_state(client, &mode, nullptr,
                                                   nullptr, err));

    ASSERT_TRUE(handles.set_flags(client, HandleFlags::Inherit,
                                  HandleFlags::Inherit));
    const std::array<Handle, 1> selected{client};
    const std::string descriptors =
        parent.prepare_inherited_named_pipes(selected);
    ASSERT_FALSE(descriptors.empty());

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        HandleTable child_handles;
        FileManager child(vfs, child_handles, guest_mem);
        if (!child.import_inherited_named_pipes(descriptors)) _exit(10);
        // Chromium indexes the channel by the numeric handle it was told.
        if (child_handles.get_type(client) != HandleType::Pipe) _exit(11);

        u32 cerr = 0, cflags = 0;
        if (!child.named_pipe_info(client, &cflags, nullptr, nullptr, nullptr,
                                   cerr)) {
            _exit(12);
        }
        if ((cflags & kPipeServerEnd) != kPipeClientEnd) _exit(13);
        if ((cflags & 0x4u) != kPipeTypeByte) _exit(14);

        // The non-blocking mode set by the parent must have survived: asking
        // for it again is a no-op, whereas message mode is still refused.
        u32 msg = kPipeReadModeMessage;
        if (child.set_named_pipe_handle_state(client, &msg, nullptr, nullptr,
                                              cerr)) {
            _exit(15);
        }
        if (cerr != error_codes::ERROR_INVALID_PARAMETER) _exit(16);

        if (!child.read_file(client, kBufAddr, 4, kWrittenAddr)) _exit(17);
        char got[4]{};
        if (guest_mem.read_bytes(kBufAddr, reinterpret_cast<Byte*>(got), 4)
                .is_err()) {
            _exit(18);
        }
        if (std::memcmp(got, "down", 4) != 0) _exit(19);

        // And back the other way.
        if (guest_mem.write_bytes(kBufAddr,
                                  reinterpret_cast<const Byte*>("up!!"), 4)
                .is_err()) {
            _exit(20);
        }
        if (!child.write_file(client, kBufAddr, 4, kWrittenAddr)) _exit(21);
        _exit(0);
    }

    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>("down"), 4).is_ok());
    ASSERT_TRUE(parent.write_file(server, kBufAddr, 4, kWrittenAddr));

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);

    ASSERT_TRUE(parent.read_file(server, kBufAddr, 4, kWrittenAddr));
    char back[4]{};
    ASSERT_TRUE(guest_mem.read_bytes(
        kBufAddr, reinterpret_cast<Byte*>(back), 4).is_ok());
    EXPECT_EQ(std::memcmp(back, "up!!", 4), 0);
}

// Test 5: GetFileType must not claim an unrecognized handle is a console
// device. Chromium validates a channel handle's type before using it.
TEST_F(FileConsoleBridgeTest, GetFileTypeRejectsUnknownHandle) {
    FileManager fm(vfs, handles, guest_mem);
    DllExports exports = make_file_exports(fm);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetFileType"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(static_cast<u32>(entry->stdcall_bytes), 4u);

    // SetUp already maps [0x10000, 0x11000); park the stack in it.
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x10800;
    cpu.set_sp(stack);
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    constexpr u32 kFileTypeUnknown = 0x0000;
    constexpr u32 kFileTypePipe    = 0x0003;

    // A handle that was never allocated.
    ASSERT_TRUE(guest_mem.write_u32(stack + 4, 0x0BADF00Du).is_ok());
    set_last_error(0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kFileTypeUnknown);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    // A real pipe still reports FILE_TYPE_PIPE.
    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(fm.create_anonymous_pipe(read_handle, write_handle, false));
    ASSERT_TRUE(guest_mem.write_u32(stack + 4, read_handle).is_ok());
    set_last_error(0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kFileTypePipe);
}

TEST_F(FileConsoleBridgeTest, ReadFileOnClosedPipeReportsBrokenPipe) {
    FileManager fm(vfs, handles, guest_mem);
    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(fm.create_anonymous_pipe(
        read_handle, write_handle, /*inheritable=*/false));

    ASSERT_TRUE(guest_mem.write_u32(kWrittenAddr, 0xFFFFFFFFu).is_ok());
    handles.set_close_callback(
        [&fm](Handle h, HandleType type, void* object) {
            return fm.on_handle_close(h, type, object);
        });
    EXPECT_TRUE(handles.close(write_handle));

    u32 bytes_read = 0xFFFFFFFFu;
    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(fm.read_file(
        read_handle, kBufAddr, 36, kWrittenAddr, &bytes_read));
    EXPECT_EQ(bytes_read, 0u);
    EXPECT_EQ(guest_mem.read_u32(kWrittenAddr).value(), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BROKEN_PIPE);

    EXPECT_TRUE(handles.close(read_handle));
    handles.set_close_callback(nullptr);
}

// Stage 4 full Win32 path: CreateNamedPipe(server) + CreateFile(client) +
// ConnectNamedPipe(accept) + WriteFile/ReadFile over the AF_UNIX socket.
// In-process loopback (server+client in one process). Requires VITREO_IPC=1.
TEST_F(FileConsoleBridgeTest, CrossProcessNamedPipeFullPath) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe = "\\\\.\\pipe\\vtm_fmpipe_" + std::to_string(::getpid());

    Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    // Client connects (queued in the listen backlog).
    Handle client = fm.create_file(pipe, GENERIC_READ | GENERIC_WRITE, 0, /*OPEN_EXISTING*/3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    // Server accepts the pending connection. Because CreateFile completed
    // before ConnectNamedPipe was called, Windows reports
    // ERROR_PIPE_CONNECTED even though the accepted stream is usable.
    ASSERT_EQ(fm.connect_named_pipe(server, /*timeout_ms=*/2000),
              PipeConnectResult::AlreadyConnected);

    // Client writes "hello".
    const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(guest_mem.write_bytes(kBufAddr, reinterpret_cast<const Byte*>(hello), 5).is_ok());
    ASSERT_TRUE(fm.write_file(client, kBufAddr, 5, kWrittenAddr));

    // Server reads them back.
    constexpr GuestAddr kReadBuf = 0x10200, kReadCnt = 0x10300;
    ASSERT_TRUE(fm.read_file(server, kReadBuf, 5, kReadCnt));
    auto cnt = guest_mem.read_u32(kReadCnt);
    ASSERT_TRUE(cnt.is_ok());
    EXPECT_EQ(cnt.value(), 5u);
    u8 got[5] = {0};
    ASSERT_TRUE(guest_mem.read_bytes(kReadBuf, reinterpret_cast<Byte*>(got), 5).is_ok());
    EXPECT_EQ(std::memcmp(got, hello, 5), 0);
}

// GetNamedPipeClientProcessId / GetNamedPipeServerProcessId: Vitreo guest
// PIDs are host getpid(), so AF_UNIX LOCAL_PEERPID is the Windows-visible
// peer identity Chromium Mojo OpenRemoteProcess needs.
TEST_F(FileConsoleBridgeTest, NamedPipePeerProcessIdsAcrossFork) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    FileManager parent(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vtm_pipe_pid_" + std::to_string(::getpid());

    const Handle server = parent.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);

    u32 server_self = 0;
    u32 error = error_codes::ERROR_SUCCESS;
    ASSERT_TRUE(parent.named_pipe_server_process_id(server, server_self, error));
    EXPECT_EQ(server_self, static_cast<u32>(::getpid()));
    EXPECT_FALSE(parent.named_pipe_client_process_id(server, server_self, error));
    EXPECT_EQ(error, error_codes::ERROR_PIPE_NOT_CONNECTED);

    int to_parent[2] = {-1, -1};
    int to_child[2] = {-1, -1};
    ASSERT_EQ(::pipe(to_parent), 0);
    ASSERT_EQ(::pipe(to_child), 0);
    const pid_t child_pid = ::fork();
    ASSERT_GE(child_pid, 0);
    if (child_pid == 0) {
        ::close(to_parent[0]);
        ::close(to_child[1]);
        HandleTable child_handles;
        FileManager child(vfs, child_handles, guest_mem);
        const Handle client = child.create_file(
            pipe, GENERIC_READ | GENERIC_WRITE, 0, /*OPEN_EXISTING*/ 3, 0);
        if (client == INVALID_HANDLE) _exit(20);
        u32 server_pid = 0;
        u32 child_error = error_codes::ERROR_SUCCESS;
        if (!child.named_pipe_server_process_id(
                client, server_pid, child_error) ||
            server_pid != static_cast<u32>(::getppid())) {
            _exit(21);
        }
        const char ready = 'R';
        if (::write(to_parent[1], &ready, 1) != 1) _exit(22);
        char ack = 0;
        if (::read(to_child[0], &ack, 1) != 1) _exit(23);
        _exit(0);
    }

    ::close(to_parent[1]);
    ::close(to_child[0]);
    char ready = 0;
    ASSERT_EQ(::read(to_parent[0], &ready, 1), 1);
    EXPECT_EQ(ready, 'R');
    ASSERT_EQ(parent.connect_named_pipe(server, /*timeout_ms=*/2000),
              PipeConnectResult::AlreadyConnected);

    u32 client_pid = 0;
    ASSERT_TRUE(parent.named_pipe_client_process_id(server, client_pid, error));
    EXPECT_EQ(client_pid, static_cast<u32>(child_pid));
    EXPECT_NE(client_pid, static_cast<u32>(::getpid()));

    const char ack = 'A';
    ASSERT_EQ(::write(to_child[1], &ack, 1), 1);
    ::close(to_parent[0]);
    ::close(to_child[1]);

    int status = 0;
    ASSERT_EQ(::waitpid(child_pid, &status, 0), child_pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
