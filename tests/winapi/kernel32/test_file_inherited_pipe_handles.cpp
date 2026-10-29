/// @file test_file_inherited_pipe_handles.cpp
/// @brief Unit tests for inheritable anonymous pipe handles: import at the same handle values, duplicate survival, explicit handle lists

#include "kernel32_file_fixture.hpp"

TEST_F(FileConsoleBridgeTest, InheritableAnonymousPipeImportsAtSameHandles) {
    FileManager parent(vfs, handles, guest_mem);
    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(parent.create_anonymous_pipe(read_handle, write_handle, true));
    const std::string descriptors = parent.prepare_inherited_named_pipes();
    ASSERT_FALSE(descriptors.empty());

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        const auto values = runtime::inherited_handle_values(descriptors, {}, {});
        if (!values) _exit(16);
        HandleTable child_handles(*values);
        // Console/process startup can allocate low values before import.
        ConsoleManager child_console(child_handles, true);
        std::vector<Handle> startup_handles;
        for (int i = 0; i < 32; ++i) {
            const Handle h = child_handles.allocate(HandleType::Event, nullptr);
            if (h == read_handle || h == write_handle) _exit(17);
            startup_handles.push_back(h);
        }
        FileManager child(vfs, child_handles, guest_mem);
        if (!child.import_inherited_named_pipes(descriptors)) _exit(10);
        if (child_handles.get_type(read_handle) != HandleType::Pipe) _exit(11);
        if (child_handles.get_type(write_handle) != HandleType::Pipe) _exit(12);
        if (!child.read_file(read_handle, kBufAddr, 4, kWrittenAddr)) _exit(13);
        char received[4]{};
        if (guest_mem.read_bytes(kBufAddr, reinterpret_cast<Byte*>(received), 4).is_err()) {
            _exit(14);
        }
        if (std::memcmp(received, "ping", 4) != 0) _exit(15);
        _exit(0);
    }

    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>("ping"), 4).is_ok());
    ASSERT_TRUE(parent.write_file(write_handle, kBufAddr, 4, kWrittenAddr));

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}

// Learn CloseHandle: object destroyed only after the last handle closes.
// DuplicateHandle shares PipeEndpoint — closing one alias must leave the other
// usable (Chromium Mojo often duplicates pipe ends before CloseHandle).
TEST_F(FileConsoleBridgeTest, DuplicatePipeHandleSurvivesPartialClose) {
    FileManager fm(vfs, handles, guest_mem);
    Handle read_handle = NULL_HANDLE;
    Handle write_handle = NULL_HANDLE;
    ASSERT_TRUE(fm.create_anonymous_pipe(read_handle, write_handle, false));

    const Handle read_dup = fm.duplicate_handle(
        read_handle, 0, handles.get_access(read_handle));
    ASSERT_NE(read_dup, NULL_HANDLE);
    ASSERT_NE(read_dup, INVALID_HANDLE);
    ASSERT_NE(read_dup, read_handle);

    ASSERT_TRUE(fm.close_file(read_handle));
    EXPECT_EQ(handles.get_type(read_handle), HandleType::Invalid);
    EXPECT_EQ(handles.get_type(read_dup), HandleType::Pipe);

    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>("dup!"), 4).is_ok());
    ASSERT_TRUE(fm.write_file(write_handle, kBufAddr, 4, kWrittenAddr));
    ASSERT_TRUE(fm.read_file(read_dup, kBufAddr + 16, 4, kWrittenAddr));
    char received[4]{};
    ASSERT_TRUE(guest_mem.read_bytes(
        kBufAddr + 16, reinterpret_cast<Byte*>(received), 4).is_ok());
    EXPECT_EQ(std::memcmp(received, "dup!", 4), 0);

    ASSERT_TRUE(fm.close_file(read_dup));
    EXPECT_EQ(handles.get_type(read_dup), HandleType::Invalid);
}

TEST_F(FileConsoleBridgeTest,
       ExplicitHandleListSerializesOnlySelectedInheritablePipes) {
    FileManager parent(vfs, handles, guest_mem);
    Handle selected_read = NULL_HANDLE;
    Handle selected_write = NULL_HANDLE;
    Handle omitted_read = NULL_HANDLE;
    Handle omitted_write = NULL_HANDLE;
    ASSERT_TRUE(parent.create_anonymous_pipe(
        selected_read, selected_write, true));
    ASSERT_TRUE(parent.create_anonymous_pipe(
        omitted_read, omitted_write, true));

    const std::array<Handle, 2> selected{selected_read, selected_write};
    const std::string descriptors =
        parent.prepare_inherited_named_pipes(selected);

    // A descriptor is "<kind>,<handle>,<fd>,..." and records are ';'-separated,
    // so the handle is field 1 of a record. Substring-matching ",<n>," over the
    // whole blob conflates that field with the *fd* field, and host fd numbers
    // depend on how many files the rest of the binary has open: in a full run
    // an unrelated pipe's fd happened to equal omitted_read, and this test
    // failed while passing in isolation. Compare the handle field itself.
    std::vector<Handle> serialized;
    for (std::size_t pos = 0; pos < descriptors.size();) {
        const std::size_t end = descriptors.find(';', pos);
        const std::string record = descriptors.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        const std::size_t first = record.find(',');
        ASSERT_NE(first, std::string::npos) << "record: " << record;
        const std::size_t second = record.find(',', first + 1);
        ASSERT_NE(second, std::string::npos) << "record: " << record;
        serialized.push_back(static_cast<Handle>(std::stoull(
            record.substr(first + 1, second - first - 1))));
        if (end == std::string::npos) break;
        pos = end + 1;
    }

    const auto serializes = [&](Handle h) {
        return std::find(serialized.begin(), serialized.end(), h) !=
               serialized.end();
    };
    EXPECT_TRUE(serializes(selected_read)) << descriptors;
    EXPECT_TRUE(serializes(selected_write)) << descriptors;
    EXPECT_FALSE(serializes(omitted_read)) << descriptors;
    EXPECT_FALSE(serializes(omitted_write)) << descriptors;
    parent.restore_inherited_pipe_close_on_exec();
}
