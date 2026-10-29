// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_file.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): NtCreateFile layouts and FileDispositionInformation

#include "ntdll_ext_test_fixture.hpp"

TEST_F(NtdllExtTest, NtCreateFileDecodesAmd64NativeLayouts) {
    constexpr GuestAddr kHandle = 0x10100;
    constexpr GuestAddr kIosb = 0x10120;
    constexpr GuestAddr kObjectAttributes = 0x10200;
    constexpr GuestAddr kUnicodeString = 0x10240;
    constexpr GuestAddr kPath = 0x10300;
    constexpr GuestAddr kRsp = 0x2f000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x10000, 0x20000, MemoryProtection::ReadWrite).is_ok());

    const std::u16string path = u"C:\\layout.txt";
    for (usize i = 0; i < path.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(kPath + i * 2, path[i]).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(kUnicodeString,
                                 static_cast<u16>(path.size() * 2)).is_ok());
    ASSERT_TRUE(memory.write_u16(kUnicodeString + 2,
                                 static_cast<u16>((path.size() + 1) * 2)).is_ok());
    ASSERT_TRUE(memory.write_u64(kUnicodeString + 8, kPath).is_ok());

    ASSERT_TRUE(memory.write_u32(kObjectAttributes, 0x30).is_ok());
    ASSERT_TRUE(memory.write_u64(kObjectAttributes + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(
        kObjectAttributes + 0x10, kUnicodeString).is_ok());
    ASSERT_TRUE(memory.write_u32(kObjectAttributes + 0x18, 0).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(kRsp);
    cpu.gpr.rcx = kHandle;
    cpu.gpr.rdx = 0x80;
    cpu.gpr.r8 = kObjectAttributes;
    cpu.gpr.r9 = kIosb;
    ASSERT_TRUE(memory.write_u64(kRsp, 0x140001000).is_ok());
    const std::array<u64, 7> stack_args{
        0, 0, 7, nt_file::FILE_OPEN, 0, 0, 0};
    for (usize i = 0; i < stack_args.size(); ++i) {
        ASSERT_TRUE(memory.write_u64(
            kRsp + 0x28 + i * 8, stack_args[i]).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kIosb + 4, 0xA5A5A5A5).is_ok());

    const auto exports = make_ntdll_ext_exports(mgr);
    const auto create = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtCreateFile";
        });
    ASSERT_NE(create, exports.entries.end());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    create->handler(marshaler);

    EXPECT_NE(static_cast<u32>(cpu.gpr.rax),
              nt_status::STATUS_OBJECT_NAME_INVALID);
    EXPECT_EQ(memory.read_u32(kIosb).value(),
              static_cast<u32>(cpu.gpr.rax));
    EXPECT_EQ(memory.read_u32(kIosb + 4).value(), 0xA5A5A5A5u);
    EXPECT_TRUE(memory.read_u64(kIosb + 8).is_ok());
}

// =============================================================================
// NtSetInformationFile(FileDispositionInformation) — pending-delete contract
// =============================================================================
//
// NT marks an object for deletion and removes it when the last handle closes.
// Reporting STATUS_SUCCESS without arming the delete leaves a caller that
// re-checks the path looping against an object that never disappears.

namespace {

class NtDispositionTest : public ::testing::Test {
protected:
    void SetUp() override {
        char path_template[] = "/tmp/vitreo_ntdisp_XXXXXX";
        char* root = ::mkdtemp(path_template);
        ASSERT_NE(root, nullptr);
        root_ = root;
        std::filesystem::create_directories(root_ + "/emptydir");
        std::filesystem::create_directories(root_ + "/fulldir");
        std::ofstream(root_ + "/fulldir/child.txt") << "child";
        std::ofstream(root_ + "/file.txt") << "data";
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root_)).is_ok());
        manager_.set_vfs(&vfs_);
        manager_.set_handle_table(&handles_);
        handles_.set_close_callback(
            [this](Handle handle, HandleType type, void*) {
                return manager_.on_handle_close(handle, type);
            });
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    /// NtSetInformationFile(handle, FileDispositionInformation, {del}).
    u32 set_disposition(Handle handle, bool del) {
        u8 buffer[1] = { static_cast<u8>(del ? 1 : 0) };
        return manager_.nt_set_information_file(handle, 13, buffer, 1);
    }

    std::string root_;
    capsule::Vfs vfs_;
    NtdllExtManager manager_;
    HandleTable handles_;
};

} // namespace

TEST_F(NtDispositionTest, RemovesFileWhenHandleCloses) {
    auto opened = manager_.nt_create_file("C:\\file.txt", 0x10000, 0, 0x7,
                                          nt_file::FILE_OPEN, 0);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);

    EXPECT_EQ(set_disposition(opened.handle, true), nt_status::STATUS_SUCCESS);
    EXPECT_TRUE(std::filesystem::exists(root_ + "/file.txt"))
        << "deletion happens at close, not at the disposition call";

    EXPECT_TRUE(handles_.close(opened.handle));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/file.txt"))
        << "FileDispositionInformation must actually delete at close";
}

TEST_F(NtDispositionTest, RemovesEmptyDirectoryWhenHandleCloses) {
    auto opened = manager_.nt_create_file("C:\\emptydir", 0x10000, 0, 0x7,
                                          nt_file::FILE_OPEN,
                                          nt_file::FILE_DIRECTORY_FILE);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);

    EXPECT_EQ(set_disposition(opened.handle, true), nt_status::STATUS_SUCCESS);
    EXPECT_TRUE(handles_.close(opened.handle));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/emptydir"));
}

TEST_F(NtDispositionTest, NonEmptyDirectorySurvives) {
    auto opened = manager_.nt_create_file("C:\\fulldir", 0x10000, 0, 0x7,
                                          nt_file::FILE_OPEN,
                                          nt_file::FILE_DIRECTORY_FILE);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);

    EXPECT_EQ(set_disposition(opened.handle, true), nt_status::STATUS_SUCCESS);
    EXPECT_TRUE(handles_.close(opened.handle));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/fulldir/child.txt"))
        << "a non-empty directory must not be removed";
}

TEST_F(NtDispositionTest, ClearingDispositionKeepsFile) {
    auto opened = manager_.nt_create_file("C:\\file.txt", 0x10000, 0, 0x7,
                                          nt_file::FILE_OPEN, 0);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);

    EXPECT_EQ(set_disposition(opened.handle, true), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(set_disposition(opened.handle, false), nt_status::STATUS_SUCCESS);
    EXPECT_TRUE(handles_.close(opened.handle));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/file.txt"));
}

TEST_F(NtDispositionTest, DeleteOnCloseCreateOptionRemovesFile) {
    auto opened = manager_.nt_create_file("C:\\file.txt", 0x10000, 0, 0x7,
                                          nt_file::FILE_OPEN,
                                          nt_file::FILE_DELETE_ON_CLOSE);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);
    EXPECT_TRUE(handles_.close(opened.handle));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/file.txt"))
        << "FILE_DELETE_ON_CLOSE must remove the file at close";
}

TEST_F(NtDispositionTest, UnknownHandleIsRejected) {
    EXPECT_EQ(set_disposition(static_cast<Handle>(0x7777), true),
              nt_status::STATUS_INVALID_HANDLE);
}

TEST_F(NtDispositionTest, ZeroLengthBufferIsRejected) {
    auto opened = manager_.nt_create_file("C:\\file.txt", 0x10000, 0, 0x7,
                                          nt_file::FILE_OPEN, 0);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);
    u8 buffer[1] = { 1 };
    EXPECT_EQ(manager_.nt_set_information_file(opened.handle, 13, buffer, 0),
              nt_status::STATUS_INVALID_PARAMETER);
    EXPECT_TRUE(handles_.close(opened.handle));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/file.txt"));
}
