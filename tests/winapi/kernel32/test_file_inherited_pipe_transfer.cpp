/// @file test_file_inherited_pipe_transfer.cpp
/// @brief Unit tests for inherited file handle import at the same numeric value and inheritable named-pipe client transfer

#include "kernel32_file_fixture.hpp"

TEST(InheritedFileHandleList, FileHandleImportsAtSameNumericValue) {
    char tmpl[] = "/tmp/vitreo_inherit_file_XXXXXX";
    ASSERT_NE(::mkdtemp(tmpl), nullptr);
    const std::string root = tmpl;
    const std::string host_file = root + "/log.txt";
    {
        std::ofstream out(host_file);
        out << "seed";
    }

    Vfs parent_vfs;
    ASSERT_TRUE(parent_vfs
                    .mount_drive('C', std::make_unique<HostFileSystem>(root))
                    .is_ok());
    HandleTable parent_handles;
    GuestMemory parent_mem;
    ASSERT_TRUE(parent_mem
                    .map_region(0x10000, 4096, MemoryProtection::ReadWrite)
                    .is_ok());
    FileManager parent(parent_vfs, parent_handles, parent_mem);

    const Handle file_h = parent.create_file(
        "C:\\log.txt", GENERIC_WRITE, 0, /*OPEN_EXISTING*/ 3, 0);
    ASSERT_NE(file_h, INVALID_HANDLE);
    ASSERT_NE(file_h, NULL_HANDLE);
    ASSERT_TRUE(parent_handles.set_flags(file_h, HandleFlags::Inherit,
                                         HandleFlags::Inherit));

    const std::array<Handle, 1> selected{file_h};
    const std::string descriptors =
        parent.prepare_inherited_named_pipes(selected);
    ASSERT_FALSE(descriptors.empty());
    EXPECT_EQ(descriptors.front(), 'F');
    EXPECT_NE(descriptors.find("," + std::to_string(file_h) + ","),
              std::string::npos);

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        Vfs child_vfs;
        if (!child_vfs
                 .mount_drive('C', std::make_unique<HostFileSystem>(root))
                 .is_ok()) {
            _exit(10);
        }
        HandleTable child_handles;
        GuestMemory child_mem;
        if (!child_mem.map_region(0x10000, 4096, MemoryProtection::ReadWrite)
                 .is_ok()) {
            _exit(11);
        }
        FileManager child(child_vfs, child_handles, child_mem);
        if (!child.import_inherited_named_pipes(descriptors)) _exit(12);
        if (child_handles.get_type(file_h) != HandleType::File) _exit(13);
        const char msg[] = "child";
        if (child_mem
                .write_bytes(0x10000, reinterpret_cast<const Byte*>(msg), 5)
                .is_err()) {
            _exit(14);
        }
        if (!child.write_file(file_h, 0x10000, 5, 0x10100)) _exit(15);
        _exit(0);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);

    // Best-effort cleanup.
    ::unlink(host_file.c_str());
    ::rmdir(root.c_str());
}

// Chromium hands a child its Mojo channel by opening \\.\pipe\mojo.<pid>.<n>,
// marking that client handle inheritable, and passing its numeric value in
// --mojo-platform-channel-handle. Serialization used to skip any connected
// endpoint that had a name, so the child received nothing at that handle and
// its service exited. Re-opening by name would create a *different*
// connection, so the live socket must travel.
TEST_F(FileConsoleBridgeTest, InheritableNamedPipeClientTransfersConnection) {
    FileManager parent(vfs, handles, guest_mem);
    const std::string pipe_name =
        "\\\\.\\pipe\\vitreo_mojo_test_" + std::to_string(::getpid());

    const Handle server = parent.create_named_pipe_server(pipe_name);
    ASSERT_NE(server, NULL_HANDLE);
    const Handle client = parent.create_file(
        pipe_name, GENERIC_READ | GENERIC_WRITE, 0, /*OPEN_EXISTING*/ 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(parent.connect_named_pipe(server, /*timeout_ms=*/2000),
              PipeConnectResult::AlreadyConnected);

    ASSERT_TRUE(handles.set_flags(client, HandleFlags::Inherit,
                                  HandleFlags::Inherit));
    const std::array<Handle, 1> selected{client};
    const std::string descriptors =
        parent.prepare_inherited_named_pipes(selected);
    ASSERT_FALSE(descriptors.empty())
        << "a named pipe client marked inheritable must serialize";
    EXPECT_EQ(descriptors[0], 'A') << "it transfers as a connected endpoint";

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        HandleTable child_handles;
        FileManager child(vfs, child_handles, guest_mem);
        if (!child.import_inherited_named_pipes(descriptors)) _exit(10);
        // Chromium indexes the handle by its numeric value, so it must land
        // at exactly the handle the parent advertised.
        if (child_handles.get_type(client) != HandleType::Pipe) _exit(11);
        if (!child.read_file(client, kBufAddr, 4, kWrittenAddr)) _exit(12);
        char received[4]{};
        if (guest_mem.read_bytes(kBufAddr, reinterpret_cast<Byte*>(received), 4)
                .is_err()) {
            _exit(13);
        }
        if (std::memcmp(received, "mojo", 4) != 0) _exit(14);
        _exit(0);
    }

    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>("mojo"), 4).is_ok());
    ASSERT_TRUE(parent.write_file(server, kBufAddr, 4, kWrittenAddr));

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}
