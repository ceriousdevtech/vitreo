/// @file test_file_info_by_handle_ex.cpp
/// @brief Unit tests for GetFileInformationByHandleEx (named-pipe name, extended directory info) over the file / console bridge fixture

#include "kernel32_file_fixture.hpp"

TEST_F(FileConsoleBridgeTest,
       GetFileInformationByHandleExReturnsInheritedNamedPipeName) {
    FileManager fm(vfs, handles, guest_mem);
    const Handle pipe =
        fm.create_local_named_pipe("\\\\.\\pipe\\crashpad_1234_TEST");
    ASSERT_NE(pipe, INVALID_HANDLE);

    FileOpsManager file_ops(guest_mem, vfs);
    file_ops.set_file_manager(&fm);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x10800;
    cpu.set_sp(stack);
    ASSERT_TRUE(guest_mem.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 4, pipe).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 8, 2).is_ok());  // FileNameInfo
    ASSERT_TRUE(guest_mem.write_u32(stack + 12, kBufAddr).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 16, 256).is_ok());

    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);
    set_last_error(0xDEADu);
    file_ops.impl_GetFileInformationByHandleEx(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    constexpr std::string_view expected = "\\crashpad_1234_TEST";
    ASSERT_EQ(guest_mem.read_u32(kBufAddr).value(),
              expected.size() * sizeof(u16));
    for (usize index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(guest_mem.read_u16(kBufAddr + 4 + index * 2).value(),
                  static_cast<u16>(expected[index]));
    }
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
}

TEST_F(FileConsoleBridgeTest,
       GetFileInformationByHandleExReportsRequiredPipeNameBytes) {
    FileManager fm(vfs, handles, guest_mem);
    const Handle pipe =
        fm.create_local_named_pipe("\\\\.\\pipe\\crashpad_short_buffer");
    ASSERT_NE(pipe, INVALID_HANDLE);

    FileOpsManager file_ops(guest_mem, vfs);
    file_ops.set_file_manager(&fm);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x10800;
    cpu.set_sp(stack);
    ASSERT_TRUE(guest_mem.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 4, pipe).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 8, 2).is_ok());  // FileNameInfo
    ASSERT_TRUE(guest_mem.write_u32(stack + 12, kBufAddr).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 16, 4).is_ok());

    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);
    file_ops.impl_GetFileInformationByHandleEx(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(guest_mem.read_u32(kBufAddr).value(),
              std::string_view("\\crashpad_short_buffer").size() *
                  sizeof(u16));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MORE_DATA);
}

TEST_F(FileConsoleBridgeTest,
       GetFileInformationByHandleExEnumeratesExtendedDirectoryInfo) {
    DirectoryEntry first;
    first.name = "a";
    first.info.size = 5;
    first.info.creation_time = 11;
    first.info.last_access_time = 12;
    first.info.last_write_time = 13;
    first.info.attributes = FileAttributes::Archive;
    first.info.file_id = 0x1122334455667788ull;
    DirectoryEntry second;
    second.name = "beta";
    second.info.attributes = FileAttributes::Directory;
    second.info.file_id = 0x8877665544332211ull;
    mock_fs->directory_entries = {first, second};

    FileManager fm(vfs, handles, guest_mem);
    const Handle directory = fm.create_file(
        "C:\\directory", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(directory, INVALID_HANDLE);
    FileOpsManager file_ops(guest_mem, vfs);
    file_ops.set_file_manager(&fm);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x10800;
    cpu.set_sp(stack);
    ASSERT_TRUE(guest_mem.write_u32(stack, 0xDEADBEEF).is_ok());
    auto invoke = [&](u32 info_class, u32 buffer_size) {
        EXPECT_TRUE(guest_mem.write_u32(stack + 4, directory).is_ok());
        EXPECT_TRUE(guest_mem.write_u32(stack + 8, info_class).is_ok());
        EXPECT_TRUE(guest_mem.write_u32(stack + 12, kBufAddr).is_ok());
        EXPECT_TRUE(guest_mem.write_u32(stack + 16, buffer_size).is_ok());
        Marshaler marshaler(guest_mem);
        marshaler.attach_cpu(cpu);
        file_ops.impl_GetFileInformationByHandleEx(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };

    // A 96-byte buffer fits only the first FILE_ID_EXTD_DIR_INFO record.
    // Native files_meta_probe enumerates "." and ".." before actual entries.
    // Advance past those two records before checking the mock file metadata.
    ASSERT_EQ(invoke(20, 96), 1u); // "."
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + 88).value(), static_cast<u16>('.'));
    ASSERT_EQ(invoke(19, 96), 1u); // ".."
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + 60).value(), 4u);
    ASSERT_EQ(invoke(19, 96), 1u); // "a"
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + 0).value(), 0u);
    EXPECT_EQ(guest_mem.read_u64(kBufAddr + 40).value(), 5u);
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + 60).value(), 2u);
    EXPECT_EQ(guest_mem.read_u64(kBufAddr + 72).value(),
              0x1122334455667788ull);
    EXPECT_EQ(guest_mem.read_u64(kBufAddr + 80).value(), 0u);
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + 88).value(),
              static_cast<u16>('a'));

    // The non-restart class resumes after the last returned record.
    ASSERT_EQ(invoke(19, 128), 1u); // FileIdExtdDirectoryInfo
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + 56).value(),
              static_cast<u32>(FileAttributes::Directory));
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + 60).value(), 8u);
    EXPECT_EQ(guest_mem.read_u64(kBufAddr + 72).value(),
              0x8877665544332211ull);
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + 88).value(),
              static_cast<u16>('b'));

    EXPECT_EQ(invoke(19, 128), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NO_MORE_FILES);

    // Restart refreshes the snapshot and returns the dot entry again.
    EXPECT_EQ(invoke(20, 96), 1u);
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + 88).value(), static_cast<u16>('.'));
    ASSERT_EQ(invoke(19, 96), 1u);
    ASSERT_EQ(invoke(19, 96), 1u);
    EXPECT_EQ(guest_mem.read_u64(kBufAddr + 72).value(),
              0x1122334455667788ull);
}

TEST_F(FileConsoleBridgeTest,
       GetFileInformationByHandleExWritesFullAndIdBothLayouts) {
    DirectoryEntry first;
    first.name = "one";
    first.info.file_id = 0x0102030405060708ull;
    DirectoryEntry second;
    second.name = "two";
    second.info.file_id = 0x1112131415161718ull;
    mock_fs->directory_entries = {first, second};

    FileManager fm(vfs, handles, guest_mem);
    const Handle directory = fm.create_file(
        "C:\\directory", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(directory, INVALID_HANDLE);
    FileOpsManager file_ops(guest_mem, vfs);
    file_ops.set_file_manager(&fm);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x10800;
    cpu.set_sp(stack);
    ASSERT_TRUE(guest_mem.write_u32(stack, 0xDEADBEEF).is_ok());
    auto invoke = [&](u32 info_class) {
        EXPECT_TRUE(guest_mem.write_u32(stack + 4, directory).is_ok());
        EXPECT_TRUE(guest_mem.write_u32(stack + 8, info_class).is_ok());
        EXPECT_TRUE(guest_mem.write_u32(stack + 12, kBufAddr).is_ok());
        EXPECT_TRUE(guest_mem.write_u32(stack + 16, 256).is_ok());
        Marshaler marshaler(guest_mem);
        marshaler.attach_cpu(cpu);
        file_ops.impl_GetFileInformationByHandleEx(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };

    ASSERT_EQ(invoke(15), 1u); // FileFullDirectoryRestartInfo
    // Directory handles synthesize "." and ".." before listed names
    // (files_meta_probe FileFullDirectoryRestartInfo).
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + 60).value(), 2u); // "."
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + 68).value(),
              static_cast<u16>('.'));
    const u32 next1 = guest_mem.read_u32(kBufAddr + 0).value();
    ASSERT_NE(next1, 0u);
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + next1 + 60).value(), 4u); // ".."
    const u32 next2 = guest_mem.read_u32(kBufAddr + next1).value();
    ASSERT_NE(next2, 0u);
    EXPECT_EQ(guest_mem.read_u32(kBufAddr + next1 + next2 + 60).value(), 6u);
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + next1 + next2 + 68).value(),
              static_cast<u16>('o'));

    ASSERT_EQ(invoke(11), 1u); // FileIdBothDirectoryRestartInfo
    // Restart again: first entry is "."; ShortName empty for ".".
    EXPECT_EQ(guest_mem.read_u8(kBufAddr + 68).value(), 0u);
    EXPECT_EQ(guest_mem.read_u16(kBufAddr + 104).value(),
              static_cast<u16>('.'));
}

TEST_F(FileConsoleBridgeTest,
       GetFileInformationByHandleExRejectsUndersizedDirectoryBuffer) {
    mock_fs->directory_entries = {DirectoryEntry{"entry", FileInfo{}}};
    FileManager fm(vfs, handles, guest_mem);
    const Handle directory = fm.create_file(
        "C:\\directory", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(directory, INVALID_HANDLE);
    FileOpsManager file_ops(guest_mem, vfs);
    file_ops.set_file_manager(&fm);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr stack = 0x10800;
    cpu.set_sp(stack);
    ASSERT_TRUE(guest_mem.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 4, directory).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 8, 20).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 12, kBufAddr).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(stack + 16, 87).is_ok());
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);
    file_ops.impl_GetFileInformationByHandleEx(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}
