/// @file test_file_console_bridge.cpp
/// @brief Unit tests for the file / console bridge (WIN32-089): CreatePipe handle width, CreateFile failure handle width, WriteFile console delegation

#include "kernel32_file_fixture.hpp"

// =============================================================================
// Console Bridge Tests (WIN32-089)
// =============================================================================

TEST_F(FileConsoleBridgeTest, CreatePipeWritesPointerWidthHandlesOnWin64) {
    constexpr GuestAddr read_out = 0x10200;
    constexpr GuestAddr write_out = 0x10210;
    constexpr GuestAddr attributes = 0x10220;

    FileManager fm(vfs, handles, guest_mem);
    DllExports exports = make_file_exports(fm);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) { return candidate.name == "CreatePipe"; });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x10800;
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(guest_mem.write_u64(read_out, 0xDEADBEEFDEADBEEFull).is_ok());
    ASSERT_TRUE(guest_mem.write_u64(write_out, 0xBAD0C0DEBAD0C0DEull).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(attributes + 0, 24).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(attributes + 4, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u64(attributes + 8, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(attributes + 16, 1).is_ok());

    cpu.gpr.rcx = read_out;
    cpu.gpr.rdx = write_out;
    cpu.gpr.r8 = attributes;
    cpu.gpr.r9 = 0;
    entry->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u64 read_handle = guest_mem.read_u64(read_out).value_or(0);
    const u64 write_handle = guest_mem.read_u64(write_out).value_or(0);
    EXPECT_EQ(read_handle >> 32, 0u);
    EXPECT_EQ(write_handle >> 32, 0u);
    ASSERT_EQ(handles.get_type(static_cast<Handle>(read_handle)), HandleType::Pipe);
    ASSERT_EQ(handles.get_type(static_cast<Handle>(write_handle)), HandleType::Pipe);
    EXPECT_EQ(handles.get_flags(static_cast<Handle>(read_handle)), HandleFlags::Inherit);
    EXPECT_EQ(handles.get_flags(static_cast<Handle>(write_handle)), HandleFlags::Inherit);

    EXPECT_TRUE(fm.close_file(static_cast<Handle>(read_handle)));
    EXPECT_TRUE(fm.close_file(static_cast<Handle>(write_handle)));
}

TEST_F(FileConsoleBridgeTest, CreateFileFailureReturnsFullWidthInvalidHandle) {
    constexpr GuestAddr path = 0x10040;
    constexpr GuestAddr stack = 0x10800;
    FileManager fm(vfs, handles, guest_mem);
    mock_fs->fail_open = true;

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(marshaler.write_wide_string(path, "C:\\missing.txt").is_ok());
    cpu.gpr.rcx = path;
    cpu.gpr.rdx = GENERIC_READ;
    cpu.gpr.r8 = FILE_SHARE_READ;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(guest_mem.write_u64(stack + 0x28, OPEN_EXISTING).is_ok());
    ASSERT_TRUE(guest_mem.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u64(stack + 0x38, 0).is_ok());

    impl_CreateFileW(marshaler, fm);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFFFFFFFFFFFULL);

    ASSERT_TRUE(marshaler.write_ansi_string(path, "C:\\missing.txt").is_ok());
    impl_CreateFileA(marshaler, fm);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFFFFFFFFFFFULL);
}

TEST_F(FileConsoleBridgeTest, CreateFileFailurePreservesPe32InvalidHandle) {
    constexpr GuestAddr path = 0x10040;
    constexpr GuestAddr stack = 0x10800;
    FileManager fm(vfs, handles, guest_mem);
    mock_fs->fail_open = true;

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(marshaler.write_wide_string(path, "C:\\missing.txt").is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 8, GENERIC_READ).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 12, FILE_SHARE_READ).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 20, OPEN_EXISTING).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 28, 0).is_ok());

    impl_CreateFileW(marshaler, fm);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFFFu);
}

TEST_F(FileConsoleBridgeTest, WriteFile_ConsoleHandle_DelegatesToConsole) {
    // Create a ConsoleManager and capture output
    ConsoleManager cm(handles);
    std::string captured;
    cm.set_output_callback([&](std::string_view sv) { captured = std::string(sv); });

    // Get stdout handle (ConsoleOutput type)
    Handle stdout_h = cm.get_std_handle(STD_OUTPUT_HANDLE);
    ASSERT_NE(stdout_h, INVALID_HANDLE);
    EXPECT_EQ(handles.get_type(stdout_h), HandleType::ConsoleOutput);

    // Set up FileManager with console bridge
    FileManager fm(vfs, handles, guest_mem);
    fm.set_console_manager(cm);

    // Write "hello" into guest buffer
    const char* msg = "hello";
    auto wr = guest_mem.write_bytes(kBufAddr,
        reinterpret_cast<const Byte*>(msg), 5);
    ASSERT_TRUE(wr.is_ok());

    // Call write_file — should delegate to ConsoleManager
    bool ok = fm.write_file(stdout_h, kBufAddr, 5, kWrittenAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured, "hello");
}

TEST_F(FileConsoleBridgeTest, WriteFile_FileHandle_UsesVfs) {
    // Make mock_fs accept writes
    mock_fs->fail_open = false;

    FileManager fm(vfs, handles, guest_mem);

    // Open a file through VFS — this is a File-typed handle
    Handle fh = fm.create_file("C:\\test.txt", GENERIC_WRITE, 0, CREATE_ALWAYS, 0);
    ASSERT_NE(fh, INVALID_HANDLE);
    EXPECT_EQ(handles.get_type(fh), HandleType::File);

    // Attempt a VFS write (mock returns NotSupported, so write_file returns false)
    const char* data = "abc";
    (void)guest_mem.write_bytes(kBufAddr,
        reinterpret_cast<const Byte*>(data), 3);

    bool ok = fm.write_file(fh, kBufAddr, 3, kWrittenAddr);
    // MockFileSystem::write returns NotSupported → false, proving VFS path was taken
    EXPECT_FALSE(ok);
}

TEST_F(FileConsoleBridgeTest, WriteFile_ConsoleInput_ReturnsError) {
    // Create a ConsoleManager to get stdin handle
    ConsoleManager cm(handles);
    Handle stdin_h = cm.get_std_handle(STD_INPUT_HANDLE);
    ASSERT_NE(stdin_h, INVALID_HANDLE);
    EXPECT_EQ(handles.get_type(stdin_h), HandleType::ConsoleInput);

    FileManager fm(vfs, handles, guest_mem);
    fm.set_console_manager(cm);

    // Writing to a ConsoleInput handle should fail
    bool ok = fm.write_file(stdin_h, kBufAddr, 5, kWrittenAddr);
    EXPECT_FALSE(ok);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FileConsoleBridgeTest, WriteFile_ConsoleHandle_NoConsoleManager_ReturnsFalse) {
    // Create a ConsoleManager just to get the handle, but do NOT call set_console_manager
    ConsoleManager cm(handles);
    Handle stdout_h = cm.get_std_handle(STD_OUTPUT_HANDLE);
    ASSERT_NE(stdout_h, INVALID_HANDLE);

    FileManager fm(vfs, handles, guest_mem);
    // m_console is null — should fall through to VFS path, handle not found → false
    bool ok = fm.write_file(stdout_h, kBufAddr, 5, kWrittenAddr);
    EXPECT_FALSE(ok);
}
