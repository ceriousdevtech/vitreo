// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_delete_on_close.cpp
/// @brief Pending-delete disposition contract tests
///
/// Windows removes a file or empty directory when the last handle carrying a
/// pending-delete disposition closes. Two entry points arm it:
/// FILE_FLAG_DELETE_ON_CLOSE at CreateFile, and
/// SetFileInformationByHandle(FileDispositionInfo) on an open handle.
///
/// FileDispositionInfo previously returned TRUE while doing nothing. Callers
/// that delete a directory and then re-check the path spin forever against an
/// object that never disappears — Chrome's setup.exe retries the deletion of
/// its CR_*.tmp unpack directory in an unbounded loop, which is what this
/// contract has to stop.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <capsule/vfs/vfs.hpp>
#include "capsule/vfs/host_filesystem.hpp"
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/filesystem/file_ops.hpp>
#include <winapi/core/marshaler.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr DATA_BASE  = 0x2000;
constexpr GuestAddr STACK_BASE = 0x80000;

// CreateFile dwDesiredAccess / dwCreationDisposition values used below.
constexpr u32 GENERIC_READ_ACCESS = 0x80000000u;
constexpr u32 DELETE_ACCESS       = 0x00010000u;
constexpr u32 OPEN_EXISTING       = 3u;
constexpr u32 FileDispositionInfoClass = 4u;
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE: a second handle that
// asks for DELETE coexists with the first only if both share delete.
constexpr u32 SHARE_ALL           = 7u;

class DeleteOnCloseTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/vitreo_delonclose_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root_ = dir;

        std::filesystem::create_directories(root_ + "/sub");
        std::ofstream(root_ + "/sub/present.txt") << "data";
        std::filesystem::create_directories(root_ + "/emptydir");
        std::filesystem::create_directories(root_ + "/fulldir");
        std::ofstream(root_ + "/fulldir/child.txt") << "child";

        ASSERT_TRUE(memory_.map_region(0x1000, 0x100000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root_)).is_ok());

        // Route HandleTable closes back into FileManager exactly as the
        // runtime does, so CloseHandle drives the real delete-on-close path.
        handles_.set_close_callback(
            [this](Handle h, HandleType type, void* object) {
                return files_.on_handle_close(h, type, object);
            });

        file_ops_.set_file_manager(&files_);

        cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + 0x800);
        marshaler_.attach_cpu(cpu_);
    }

    void TearDown() override {
        // handles_ outlives files_ (members destroy in reverse order) and its
        // destructor closes whatever a failed ASSERT left open through this
        // callback; detach it so that close never reaches a dead FileManager.
        handles_.set_close_callback(nullptr);
        std::filesystem::remove_all(root_);
    }

    /// Call SetFileInformationByHandle(h, FileDispositionInfo, {delete}).
    u32 set_disposition(Handle h, bool delete_file) {
        (void)memory_.write_u8(DATA_BASE, delete_file ? 1u : 0u);
        (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 4),
                                static_cast<u32>(h));
        (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 8),
                                FileDispositionInfoClass);
        (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 12), DATA_BASE);
        (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 16), 1u);
        set_last_error(error_codes::ERROR_SUCCESS);
        file_ops_.impl_SetFileInformationByHandle(marshaler_);
        return static_cast<u32>(cpu_.gpr.rax);
    }

    std::string    root_;
    GuestMemory    memory_;
    capsule::Vfs   vfs_;
    cpu::CpuState  cpu_;
    HandleTable    handles_;
    Marshaler      marshaler_{memory_};
    FileManager    files_{vfs_, handles_, memory_};
    FileOpsManager file_ops_{memory_, vfs_};
};

// 1. The regression: disposition on a file must actually remove it at close.
TEST_F(DeleteOnCloseTest, FileDispositionRemovesFileOnClose) {
    Handle h = files_.create_file("C:\\sub\\present.txt",
                                  GENERIC_READ_ACCESS | DELETE_ACCESS, 0,
                                  OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(set_disposition(h, true), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    // Still present while the handle is open — deletion happens at last close.
    EXPECT_TRUE(std::filesystem::exists(root_ + "/sub/present.txt"));

    EXPECT_TRUE(files_.close_file(h));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/sub/present.txt"))
        << "FileDispositionInfo must delete at close, not report a fake TRUE";
}

// 2. Chrome's shape: an empty directory opened with backup semantics.
TEST_F(DeleteOnCloseTest, FileDispositionRemovesEmptyDirectoryOnClose) {
    Handle h = files_.create_file("C:\\emptydir", DELETE_ACCESS, 0,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(set_disposition(h, true), 1u);
    EXPECT_TRUE(files_.close_file(h));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/emptydir"))
        << "an empty directory marked for deletion must be gone after close";
}

// 3. A non-empty directory survives — Windows cannot remove it, and the guest
//    must observe that rather than a success that loses the subtree.
TEST_F(DeleteOnCloseTest, NonEmptyDirectorySurvivesClose) {
    Handle h = files_.create_file("C:\\fulldir", DELETE_ACCESS, 0,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(set_disposition(h, true), 1u);
    EXPECT_TRUE(files_.close_file(h));
    EXPECT_TRUE(std::filesystem::is_directory(root_ + "/fulldir"));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/fulldir/child.txt"))
        << "a non-empty directory must not be removed, and must keep its child";
}

// 4. Clearing the disposition disarms the delete.
TEST_F(DeleteOnCloseTest, ClearingDispositionKeepsFile) {
    Handle h = files_.create_file("C:\\sub\\present.txt",
                                  GENERIC_READ_ACCESS | DELETE_ACCESS, 0,
                                  OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(set_disposition(h, true), 1u);
    EXPECT_EQ(set_disposition(h, false), 1u);
    EXPECT_TRUE(files_.close_file(h));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/sub/present.txt"))
        << "DeleteFile=FALSE must cancel the pending delete";
}

// 5. FILE_FLAG_DELETE_ON_CLOSE arms the same disposition at open time.
TEST_F(DeleteOnCloseTest, CreateFileDeleteOnCloseFlagRemovesFile) {
    Handle h = files_.create_file("C:\\sub\\present.txt",
                                  GENERIC_READ_ACCESS | DELETE_ACCESS, 0,
                                  OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(std::filesystem::exists(root_ + "/sub/present.txt"));

    EXPECT_TRUE(files_.close_file(h));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/sub/present.txt"))
        << "FILE_FLAG_DELETE_ON_CLOSE must remove the file at close";
}

// 6. An untracked handle reports failure rather than a success that cannot
//    happen.
TEST_F(DeleteOnCloseTest, UntrackedHandleFailsWithInvalidHandle) {
    EXPECT_EQ(set_disposition(static_cast<Handle>(0x4444), true), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// 7. Closing without any disposition leaves the file alone.
TEST_F(DeleteOnCloseTest, PlainCloseKeepsFile) {
    Handle h = files_.create_file("C:\\sub\\present.txt", GENERIC_READ_ACCESS,
                                  0, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(files_.close_file(h));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/sub/present.txt"));
}

// 8. Windows refuses to open a directory without FILE_FLAG_BACKUP_SEMANTICS,
//    and a trailing backslash must not defeat that check — Chrome's installer
//    opens "...\CR_XXXXX.tmp\" in exactly that form, so the delete-on-close
//    path above is only reachable through a backup-semantics open.
TEST_F(DeleteOnCloseTest, DirectoryOpenRequiresBackupSemantics) {
    EXPECT_EQ(files_.create_file("C:\\emptydir", DELETE_ACCESS, 0,
                                 OPEN_EXISTING, 0),
              INVALID_HANDLE);
    EXPECT_EQ(files_.create_file("C:\\emptydir\\", DELETE_ACCESS, 0,
                                 OPEN_EXISTING, 0),
              INVALID_HANDLE);

    Handle h = files_.create_file("C:\\emptydir\\", DELETE_ACCESS, 0,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(files_.close_file(h));
}

// FILE_RENAME_INFO on x86 is { DWORD Flags; HANDLE RootDirectory (4 bytes);
// DWORD FileNameLength; WCHAR FileName[] } — name at offset 12 (winbase.h).
// The handler read the x64 layout (name at 20) for every bitness, so a 32-bit
// SetFileInformationByHandle(FileRenameInfo) renamed to garbage or failed.
TEST_F(DeleteOnCloseTest, FileRenameInfoUsesTheX86Layout) {
    Handle h = files_.create_file("C:\\sub\\present.txt",
                                  GENERIC_READ_ACCESS | DELETE_ACCESS, 0,
                                  OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    const char16_t name[] = u"C:\\sub\\renamed.txt";
    const u32 name_bytes = static_cast<u32>((sizeof(name) / 2 - 1) * 2);
    const GuestAddr info = DATA_BASE + 0x100;
    (void)memory_.write_u32(info + 0, 0);            // ReplaceIfExists
    (void)memory_.write_u32(info + 4, 0);            // RootDirectory
    (void)memory_.write_u32(info + 8, name_bytes);   // FileNameLength
    for (u32 i = 0; i < name_bytes / 2; ++i) {
        (void)memory_.write_u16(info + 12 + i * 2, static_cast<u16>(name[i]));
    }
    constexpr u32 FileRenameInfoClass = 3u;
    (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 4), static_cast<u32>(h));
    (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 8), FileRenameInfoClass);
    (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 12), info);
    (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 16), 12u + name_bytes);
    file_ops_.impl_SetFileInformationByHandle(marshaler_);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 1u);
    EXPECT_TRUE(files_.close_file(h));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/sub/renamed.txt"));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/sub/present.txt"));
}

} // namespace

// Windows removes a delete-pending object at LAST handle close, and refuses
// new opens while the delete is pending. Deleting on whichever handle carried
// the disposition tears the object out from under handles the guest still holds.
TEST_F(DeleteOnCloseTest, DeletesOnlyWhenLastHandleCloses) {
    Handle a = files_.create_file("C:\\sub\\present.txt", DELETE_ACCESS, SHARE_ALL, 3u /* OPEN_EXISTING */, 0);
    Handle b = files_.create_file("C:\\sub\\present.txt", DELETE_ACCESS, SHARE_ALL, 3u /* OPEN_EXISTING */, 0);
    ASSERT_NE(a, INVALID_HANDLE);
    ASSERT_NE(b, INVALID_HANDLE);

    EXPECT_EQ(set_disposition(a, true), 1u);

    EXPECT_TRUE(files_.close_file(a));
    EXPECT_TRUE(std::filesystem::exists(root_ + "/sub/present.txt"))
        << "one handle remains open — the object must survive";

    EXPECT_TRUE(files_.close_file(b));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/sub/present.txt"))
        << "last handle closed — the object must now be gone";
}

TEST_F(DeleteOnCloseTest, OpenOfDeletePendingObjectIsRefused) {
    Handle h = files_.create_file("C:\\sub\\present.txt", DELETE_ACCESS, SHARE_ALL, 3u /* OPEN_EXISTING */, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_EQ(set_disposition(h, true), 1u);

    Handle again = files_.create_file("C:\\sub\\present.txt", DELETE_ACCESS,
                                      SHARE_ALL, 3u /* OPEN_EXISTING */, 0);
    EXPECT_EQ(again, INVALID_HANDLE)
        << "a delete-pending object cannot be reopened";
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED)
        << "Windows reports STATUS_DELETE_PENDING as ERROR_ACCESS_DENIED, "
           "not as the object being absent";

    EXPECT_TRUE(files_.close_file(h));
    EXPECT_FALSE(std::filesystem::exists(root_ + "/sub/present.txt"));
}
