/// @file test_kernel32_cpuz_paths.cpp
/// @brief Unit tests for KERNEL32 path, directory and file-system contracts

#include <winapi/kernel32/environment/environment.hpp>
#include "kernel32_cpuz_fixture.hpp"
#include <chrono>
#include <utility>
#include <capsule/vfs/host_filesystem.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file_pipe_internal.hpp>
#include <winapi/kernel32/filesystem/file_search.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/kernel32/system/time.hpp>
#include <cstdio>
#include <tuple>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unistd.h>

TEST_F(Kernel32CpuzTest, SetReparsePointCreatesDirectoryJunction) {
    // libuv creates a Windows directory junction the way Windows does it:
    // CreateDirectory, open the directory, then FSCTL_SET_REPARSE_POINT on
    // that FILE handle. Rejecting a non-Device handle answered
    // ERROR_INVALID_HANDLE, which Node surfaces as
    // "EBADF: bad file descriptor, symlink ...".
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("vitreo_junction_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "target");
    fs::create_directories(root / "link");
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(
                             root.string()))
                    .is_ok());

    FileManager files{vfs, handles, memory};
    constexpr u32 kBackupSemantics = 0x02000000u;
    const Handle dir = files.create_file("C:\\link", 0x40000000u, 7, 3,
                                         kBackupSemantics);
    ASSERT_NE(dir, INVALID_HANDLE);

    // REPARSE_DATA_BUFFER for IO_REPARSE_TAG_MOUNT_POINT.
    constexpr GuestAddr buf = DATA_BASE + 0xA00;
    const std::string substitute = "\\??\\C:\\target";
    const u32 name_bytes = static_cast<u32>(substitute.size() * 2);
    ASSERT_TRUE(memory.write_u32(buf + 0, 0xA0000003u).is_ok());
    ASSERT_TRUE(memory.write_u16(buf + 4,
                                 static_cast<u16>(8 + name_bytes + 2)).is_ok());
    ASSERT_TRUE(memory.write_u16(buf + 6, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(buf + 8, 0).is_ok());                 // subst off
    ASSERT_TRUE(memory.write_u16(buf + 10,
                                 static_cast<u16>(name_bytes)).is_ok()); // subst len
    ASSERT_TRUE(memory.write_u16(buf + 12,
                                 static_cast<u16>(name_bytes + 2)).is_ok());
    ASSERT_TRUE(memory.write_u16(buf + 14, 0).is_ok());
    for (usize i = 0; i < substitute.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(
                        buf + 16 + static_cast<u32>(i * 2),
                        static_cast<u16>(substitute[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(buf + 16 + name_bytes, 0).is_ok());

    u32 bytes_returned = 0xFFFFFFFFu;
    EXPECT_TRUE(files.device_io_control(dir, 0x000900A4u, buf,
                                        16 + name_bytes + 2, 0, 0,
                                        bytes_returned));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_TRUE(fs::is_symlink(root / "link"));
    EXPECT_EQ(fs::read_symlink(root / "link"), root / "target");

    // A reparse tag Vitreo does not realize must fail rather than silently
    // report success — a caller that believes a junction exists reads the
    // wrong tree.
    fs::create_directories(root / "link2");
    const Handle dir2 = files.create_file("C:\\link2", 0x40000000u, 7, 3,
                                          kBackupSemantics);
    ASSERT_NE(dir2, INVALID_HANDLE);
    ASSERT_TRUE(memory.write_u32(buf + 0, 0xA000000Cu).is_ok()); // SYMLINK
    EXPECT_FALSE(files.device_io_control(dir2, 0x000900A4u, buf,
                                         16 + name_bytes + 2, 0, 0,
                                         bytes_returned));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_SUPPORTED);
    EXPECT_TRUE(fs::exists(root / "link2"));
    EXPECT_FALSE(fs::is_symlink(root / "link2"));

    (void)files.close_file(dir);
    (void)files.close_file(dir2);
    fs::remove_all(root);
}

// =============================================================================
// File Locking
// =============================================================================

// Every row of files_core_probe.expected "# LockFile/UnlockFile" that does
// not need I/O: two handles on one real file, exclusive locks that may not
// overlap any lock (the handle's own included), UnlockFile only on an exact
// range this handle locked, and success leaving the last error untouched.
TEST_F(Kernel32CpuzTest, LockFileUnlockFileFollowTheProbedContract) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("vitreo_lockfile_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    { std::ofstream(root / "locked.bin") << std::string(64, 'x'); }
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(
                             root.string()))
                    .is_ok());
    FileManager files{vfs, handles, memory};
    mgr.set_file_manager(&files);
    constexpr u32 kGenericReadWrite = 0xC0000000u;
    constexpr u32 kShareAll = 7;
    constexpr u32 kOpenExisting = 3;
    const Handle h1 = files.create_file("C:\\locked.bin", kGenericReadWrite,
                                        kShareAll, kOpenExisting, 0x80);
    const Handle h2 = files.create_file("C:\\locked.bin", kGenericReadWrite,
                                        kShareAll, kOpenExisting, 0x80);
    ASSERT_NE(h1, INVALID_HANDLE);
    ASSERT_NE(h2, INVALID_HANDLE);

    // Returns {BOOL, last error}; the last error is 0xDEAD unless set.
    auto call = [&](bool lock, Handle h, u64 offset, u64 length) {
        const u64 sp = cpu.gpr.rsp;
        (void)memory.write_u32(static_cast<u32>(sp + 4), static_cast<u32>(h));
        (void)memory.write_u32(static_cast<u32>(sp + 8), static_cast<u32>(offset));
        (void)memory.write_u32(static_cast<u32>(sp + 12), static_cast<u32>(offset >> 32));
        (void)memory.write_u32(static_cast<u32>(sp + 16), static_cast<u32>(length));
        (void)memory.write_u32(static_cast<u32>(sp + 20), static_cast<u32>(length >> 32));
        set_last_error(0xDEADu);
        if (lock) {
            mgr.impl_LockFile(marshaler);
        } else {
            mgr.impl_UnlockFile(marshaler);
        }
        return std::pair<u64, u32>{cpu.gpr.rax, get_last_error()};
    };
    using Result = std::pair<u64, u32>;
    const Result ok{1u, 0xDEADu};
    const Result violation{0u, error_codes::ERROR_LOCK_VIOLATION};
    const Result not_locked{0u, error_codes::ERROR_NOT_LOCKED};
    const Result bad_handle{0u, error_codes::ERROR_INVALID_HANDLE};

    EXPECT_EQ(call(true, h1, 0, 10), ok);            // LockFile_h1_0_10
    EXPECT_EQ(call(true, h2, 5, 10), violation);     // LockFile_h2_overlap
    EXPECT_EQ(call(true, h1, 0, 1), violation);      // LockFile_h1_overlap_self
    EXPECT_EQ(call(true, h2, 100, 10), ok);          // LockFile_h2_disjoint
    EXPECT_EQ(call(true, h1, 1ull << 20, 16), ok);   // LockFile_h1_past_eof
    EXPECT_EQ(call(true, h1, 200, 0), ok);           // LockFile_zero_length
    EXPECT_EQ(call(false, h1, 0, 5), not_locked);    // UnlockFile_partial
    EXPECT_EQ(call(false, h1, 100, 10), not_locked); // UnlockFile_other_handles_lock
    EXPECT_EQ(call(false, h1, 0, 10), ok);           // UnlockFile_exact
    EXPECT_EQ(call(false, h1, 0, 10), not_locked);   // UnlockFile_again
    EXPECT_EQ(call(true, h2, 0, 10), ok);            // LockFile_h2_after_unlock
    EXPECT_EQ(call(true, 0xAA, 0, 0x100), bad_handle);   // LockFile_invalid_handle
    EXPECT_EQ(call(false, 0xAA, 0, 0x100), bad_handle);  // UnlockFile_invalid_handle

    (void)files.close_file(h1);
    (void)files.close_file(h2);
    mgr.set_file_manager(nullptr);
    fs::remove_all(root);
}

// =============================================================================
// Path / Directory APIs
// =============================================================================

TEST_F(Kernel32CpuzTest, SearchPathAReportsMissingFileForSizeQuery) {
    constexpr GuestAddr filename = DATA_BASE + 0x400;
    constexpr GuestAddr file_part = DATA_BASE + 0x500;
    const GuestAddr stack = cpu.sp();
    const char* missing = "RPAWINET.DLL";
    for (usize i = 0; i <= std::strlen(missing); ++i) {
        ASSERT_TRUE(
            memory.write_u8(filename + i, static_cast<u8>(missing[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(file_part, 0xDEADBEEFu).is_ok());

    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, file_part).is_ok());
    set_last_error(error_codes::ERROR_SUCCESS);
    mgr.impl_SearchPathA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(memory.read_u32(file_part).value_or(0xFFFFFFFFu), 0u);
}

TEST_F(Kernel32CpuzTest, SearchPathAFindsVfsDirectory) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vitreo_search_path_directory_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "Parent" / "Child"));
    ASSERT_TRUE(
        vfs.mount_drive(
               'C', std::make_unique<capsule::HostFileSystem>(root.string()))
            .is_ok());

    constexpr GuestAddr path = DATA_BASE + 0x400;
    constexpr GuestAddr filename = DATA_BASE + 0x480;
    constexpr GuestAddr output = DATA_BASE + 0x600;
    constexpr GuestAddr file_part = DATA_BASE + 0x900;
    const GuestAddr stack = cpu.sp();
    const char* path_text = "C:\\Parent";
    const char* filename_text = "Child";
    ASSERT_TRUE(memory.write_bytes(
        path, reinterpret_cast<const u8*>(path_text),
        std::strlen(path_text) + 1).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        filename, reinterpret_cast<const u8*>(filename_text),
        std::strlen(filename_text) + 1).is_ok());

    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, output).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, file_part).is_ok());
    mgr.impl_SearchPathA(marshaler);

    const std::string expected = "C:\\Parent\\Child";
    EXPECT_EQ(cpu.gpr.rax, expected.size());
    for (usize i = 0; i <= expected.size(); ++i) {
        EXPECT_EQ(
            memory.read_u8(output + i).value_or(0xFF),
            i == expected.size() ? 0 : static_cast<u8>(expected[i]));
    }
    EXPECT_EQ(
        memory.read_u32(file_part).value_or(0),
        output + std::strlen("C:\\Parent\\"));

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST_F(Kernel32CpuzTest, SearchPathWFindsVfsFileAndReturnsFilePart) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vitreo_search_path_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "Tools"));
    {
        std::ofstream file(root / "Tools" / "Runner.EXE", std::ios::binary);
        ASSERT_TRUE(file.good());
        file << "MZ";
    }
    ASSERT_TRUE(
        vfs.mount_drive(
               'C', std::make_unique<capsule::HostFileSystem>(root.string()))
            .is_ok());

    constexpr GuestAddr path = DATA_BASE + 0x400;
    constexpr GuestAddr filename = DATA_BASE + 0x480;
    constexpr GuestAddr extension = DATA_BASE + 0x500;
    constexpr GuestAddr output = DATA_BASE + 0x600;
    constexpr GuestAddr file_part = DATA_BASE + 0x900;
    const GuestAddr stack = cpu.sp();
    write_guest_wide_string(path, "C:\\Tools");
    write_guest_wide_string(filename, "Runner");
    write_guest_wide_string(extension, ".EXE");

    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, extension).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, file_part).is_ok());
    mgr.impl_SearchPathW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, std::strlen("C:\\Tools\\Runner.EXE") + 1);

    ASSERT_TRUE(memory.write_u32(stack + 16, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, output).is_ok());
    mgr.impl_SearchPathW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, std::strlen("C:\\Tools\\Runner.EXE"));
    EXPECT_EQ(read_guest_wide_string(output), "C:\\Tools\\Runner.EXE");
    EXPECT_EQ(
        memory.read_u32(file_part).value_or(0),
        output + std::strlen("C:\\Tools\\") * 2);
    EXPECT_EQ(
        read_guest_wide_string(memory.read_u32(file_part).value_or(0)),
        "Runner.EXE");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

// A built-in DLL is a file in System32 (the VFS's generated file, which the
// module manager provides in a process), so SearchPath finds it there.
TEST_F(Kernel32CpuzTest, SearchPathAFindsGeneratedSystemDll) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vitreo_search_path_builtin_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    const auto image = std::make_shared<const std::vector<u8>>(
        std::vector<u8>{'M', 'Z'});
    vfs.set_generated_file_source(
        [&](std::string_view path) -> std::shared_ptr<const std::vector<u8>> {
            std::string lower(path);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            return lower == "c:\\windows\\system32\\d3dx9_31.dll" ? image
                                                                  : nullptr;
        },
        nullptr);

    FileSearchManager search;
    search.set_vfs(&vfs);
    mgr.set_file_search_manager(&search);

    constexpr GuestAddr filename = DATA_BASE + 0x400;
    constexpr GuestAddr output = DATA_BASE + 0x600;
    constexpr GuestAddr file_part = DATA_BASE + 0x900;
    const GuestAddr stack = cpu.sp();
    const char* filename_text = "d3dx9_31.dll";
    ASSERT_TRUE(memory.write_bytes(
        filename, reinterpret_cast<const u8*>(filename_text),
        std::strlen(filename_text) + 1).is_ok());

    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, output).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, file_part).is_ok());
    mgr.impl_SearchPathA(marshaler);

    const std::string expected = "C:\\Windows\\System32\\d3dx9_31.dll";
    EXPECT_EQ(cpu.gpr.rax, expected.size());
    EXPECT_EQ(read_guest_ansi_string(output), expected);

    vfs.set_generated_file_source(nullptr, nullptr);
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST_F(Kernel32CpuzTest, SearchPathASearchesApplicationDirectoryBeforeCurrent) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vitreo_search_path_appdir_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "App"));
    ASSERT_TRUE(std::filesystem::create_directories(root / "Work"));
    {
        std::ofstream helper(root / "App" / "ISSetup.dll", std::ios::binary);
        ASSERT_TRUE(helper.good());
        helper << "MZ";
    }
    ASSERT_TRUE(
        vfs.mount_drive(
               'C', std::make_unique<capsule::HostFileSystem>(root.string()))
            .is_ok());

    ThunkRegistry registry{0x70000000};
    ModuleManager modules{registry};
    modules.set_main_module(0x400000, "C:\\App\\Setup.exe");
    mgr.set_module_manager(&modules);
    // The helper sits beside the .exe and nowhere on the current directory
    // or PATH, so only the application-directory step can find it.
    mgr.set_current_directory("C:\\Work");

    constexpr GuestAddr filename = DATA_BASE + 0x400;
    constexpr GuestAddr output = DATA_BASE + 0x600;
    constexpr GuestAddr file_part = DATA_BASE + 0x900;
    const GuestAddr stack = cpu.sp();
    const char* filename_text = "ISSetup.dll";
    ASSERT_TRUE(memory.write_bytes(
        filename, reinterpret_cast<const u8*>(filename_text),
        std::strlen(filename_text) + 1).is_ok());

    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());  // lpPath = NULL
    ASSERT_TRUE(memory.write_u32(stack + 8, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, output).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, file_part).is_ok());
    mgr.impl_SearchPathA(marshaler);

    const std::string expected = "C:\\App\\ISSetup.dll";
    EXPECT_EQ(cpu.gpr.rax, expected.size());
    EXPECT_EQ(read_guest_ansi_string(output), expected);
    EXPECT_EQ(memory.read_u32(file_part).value_or(0),
              output + std::strlen("C:\\App\\"));

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST_F(Kernel32CpuzTest, FindFirstChangeNotificationSignalsOnFileCreate) {
    constexpr u32 FILE_NOTIFY_CHANGE_FILE_NAME = 0x00000001;
    const auto root = std::filesystem::temp_directory_path() /
        ("vitreo_ffcn_" + std::to_string(::getpid()) + "_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "watch"));
    ASSERT_TRUE(
        vfs.mount_drive(
               'C', std::make_unique<capsule::HostFileSystem>(root.string()))
            .is_ok());

    constexpr GuestAddr path = DATA_BASE + 0x400;
    const GuestAddr stack = cpu.sp();
    write_guest_wide_string(path, "C:\\watch");
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());  // no subtree
    ASSERT_TRUE(memory.write_u32(stack + 12, FILE_NOTIFY_CHANGE_FILE_NAME)
                    .is_ok());
    mgr.impl_FindFirstChangeNotificationW(marshaler);
    const Handle notify = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(notify, INVALID_HANDLE);
    ASSERT_NE(notify, NULL_HANDLE);

    // No change yet — wait should time out.
    EXPECT_EQ(sync.wait_for_single_object(notify, 50), WAIT_TIMEOUT);

    {
        std::ofstream file(root / "watch" / "created.txt");
        ASSERT_TRUE(file.good());
        file << "x";
    }

    // Poller interval is 200ms; allow a generous wait for the signal.
    EXPECT_EQ(sync.wait_for_single_object(notify, 2000), WAIT_OBJECT_0);

    ASSERT_TRUE(memory.write_u32(stack + 4, notify).is_ok());
    mgr.impl_FindNextChangeNotification(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(stack + 4, notify).is_ok());
    mgr.impl_FindCloseChangeNotification(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST_F(Kernel32CpuzTest, FindFirstChangeNotificationOnFileReturnsErrorDirectory) {
    // Chromium FilePathWatcher starts at the hosts *file* path and walks up
    // only when GetLastError is a known non-fatal code. Windows ERROR_DIRECTORY
    // is 267 — a wrong constant aborts the walk and CEF DNS hosts watch fails.
    constexpr u32 FILE_NOTIFY_CHANGE_FILE_NAME = 0x00000001;
    constexpr u32 kErrorDirectory = 267;
    const auto root = std::filesystem::temp_directory_path() /
        ("vitreo_ffcn_file_" + std::to_string(::getpid()) + "_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "watch"));
    {
        std::ofstream file(root / "watch" / "hosts");
        ASSERT_TRUE(file.good());
        file << "127.0.0.1 localhost\n";
    }
    ASSERT_TRUE(
        vfs.mount_drive(
               'C', std::make_unique<capsule::HostFileSystem>(root.string()))
            .is_ok());

    constexpr GuestAddr path = DATA_BASE + 0x400;
    const GuestAddr stack = cpu.sp();
    write_guest_wide_string(path, "C:\\watch\\hosts");
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, FILE_NOTIFY_CHANGE_FILE_NAME)
                    .is_ok());
    mgr.impl_FindFirstChangeNotificationW(marshaler);
    EXPECT_EQ(static_cast<Handle>(cpu.gpr.rax), INVALID_HANDLE);
    EXPECT_EQ(winapi::get_last_error(), kErrorDirectory);

    // Parent directory watch must still succeed so Chromium can walk up.
    write_guest_wide_string(path, "C:\\watch");
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    mgr.impl_FindFirstChangeNotificationW(marshaler);
    const Handle notify = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(notify, INVALID_HANDLE);
    ASSERT_NE(notify, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(stack + 4, notify).is_ok());
    mgr.impl_FindCloseChangeNotification(marshaler);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST_F(Kernel32CpuzTest, Kernel32ExportsSearchPathAWithSixArguments) {
    auto exports = create_kernel32_ext_thunks(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "SearchPathA";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 24u);
}

TEST_F(Kernel32CpuzTest, GetTempPathA_WritesTempPath) {
    // GetTempPathA(nBufferLength, lpBuffer)
    // arg0 = buf_len, arg1 = buf_ptr
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 260);  // nBufferLength
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), buf);  // lpBuffer
    mgr.impl_GetTempPathA(marshaler);

    // Learn GetTempPath: TMP, TEMP, USERPROFILE, then the Windows directory.
    // This fixture has no environment, so the Windows directory it is.
    EXPECT_EQ(cpu.gpr.rax, 11u);  // length of "C:\Windows\" excluding null
    std::string path = read_guest_ansi_string(buf);
    EXPECT_EQ(path, "C:\\Windows\\");

    // Too small: the size needed including the NUL, buffer untouched.
    (void)memory.write_u8(buf, 0x77);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 5);
    mgr.impl_GetTempPathA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 12u);
    EXPECT_EQ(memory.read_u8(buf).value_or(0), 0x77u);
}

TEST_F(Kernel32CpuzTest, GetTempPathA_UsesTmpBeforeTemp) {
    EnvironmentManager env("probe.exe", memory);
    env.set_variable("TMP", "C:\\Users\\Default\\AppData\\Local\\Temp");
    env.set_variable("TEMP", "C:\\Other");
    mgr.set_environment_manager(&env);
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 260);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), buf);
    mgr.impl_GetTempPathA(marshaler);
    EXPECT_EQ(read_guest_ansi_string(buf), "C:\\Users\\Default\\AppData\\Local\\Temp\\");
    EXPECT_EQ(cpu.gpr.rax, 36u);
    mgr.set_environment_manager(nullptr);
}

TEST_F(Kernel32CpuzTest, GetWindowsDirectoryA_WritesWindowsDir) {
    // GetWindowsDirectoryA(lpBuffer, uSize)
    // arg0 = buf_ptr, arg1 = uSize
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);  // lpBuffer
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 260);  // uSize
    mgr.impl_GetWindowsDirectoryA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 10u);  // length of "C:\Windows"
    std::string path = read_guest_ansi_string(buf);
    EXPECT_EQ(path, "C:\\Windows");
}

// Learn (GetSystemWindowsDirectory): the shared Windows directory on a
// multi-user Terminal Services system; "on a system that is not running
// Terminal Services, this is the same as GetWindowsDirectory". A capsule is
// only ever the latter, so the two must agree exactly — and both must be
// exported, because the missing-export stub's 0xFFFFFFFF reads to a caller as
// a returned character count over an untouched buffer.
TEST_F(Kernel32CpuzTest, GetSystemWindowsDirectoryMatchesGetWindowsDirectory) {
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 260);
    mgr.impl_GetWindowsDirectoryA(marshaler);
    const u64 windows_len = cpu.gpr.rax;
    const std::string windows_dir = read_guest_ansi_string(buf);
    ASSERT_EQ(windows_dir, "C:\\Windows");

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto find = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
                            [&](const ExportEntry& e) { return e.name == name; });
    };
    const auto wide = find("GetSystemWindowsDirectoryW");
    const auto ansi = find("GetSystemWindowsDirectoryA");
    ASSERT_NE(wide, exports.entries.end()) << "GetSystemWindowsDirectoryW";
    ASSERT_NE(ansi, exports.entries.end()) << "GetSystemWindowsDirectoryA";

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 260);
    cpu.gpr.rax = 0xDEADBEEF;
    ansi->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, windows_len);
    EXPECT_EQ(read_guest_ansi_string(buf), windows_dir);
}

TEST_F(Kernel32CpuzTest, GetSystemDirectoryA_WritesSystemDir) {
    // GetSystemDirectoryA(lpBuffer, uSize)
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);  // lpBuffer
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 260);  // uSize
    mgr.impl_GetSystemDirectoryA(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 19u);  // length of "C:\Windows\system32"
    std::string path = read_guest_ansi_string(buf);
    EXPECT_EQ(path, "C:\\Windows\\system32");
}

TEST_F(Kernel32CpuzTest, GetSystemWow64DirectoryWReportsRequiredSizeAndPath) {
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 8);
    mgr.impl_GetSystemWow64DirectoryW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 20u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 20);
    mgr.impl_GetSystemWow64DirectoryW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 19u);
    EXPECT_EQ(read_guest_wide_string(buf), "C:\\Windows\\SysWOW64");
}

TEST_F(Kernel32CpuzTest, GetSystemWow64DirectoryAReportsRequiredSizeAndPath) {
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), buf);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 8);
    mgr.impl_GetSystemWow64DirectoryA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 20u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 20);
    mgr.impl_GetSystemWow64DirectoryA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 19u);
    EXPECT_EQ(read_guest_ansi_string(buf), "C:\\Windows\\SysWOW64");
}

// =============================================================================
// File / Directory
// =============================================================================

// CreateDirectoryA performs a real VFS create, as DeleteFileA does; it no
// longer returns TRUE unconditionally. TRUE without a directory on disk is the
// worse failure of the two -- the caller writes into a path that isn't there.
TEST_F(Kernel32CpuzTest, CreateDirectoryA_CreatesRealDirectory) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vitreo_create_directory_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root));
    ASSERT_TRUE(
        vfs.mount_drive(
               'C', std::make_unique<capsule::HostFileSystem>(root.string()))
            .is_ok());

    const char* dir = "C:\\TestDir";
    ASSERT_TRUE(memory.write_bytes(
        DATA_BASE, reinterpret_cast<const u8*>(dir),
        std::strlen(dir) + 1).is_ok());

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), DATA_BASE);  // lpPathName
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);          // lpSecurityAttributes
    mgr.impl_CreateDirectoryA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_TRUE(std::filesystem::is_directory(root / "TestDir"));

    // Re-creating an existing directory fails with ERROR_ALREADY_EXISTS.
    mgr.impl_CreateDirectoryA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ALREADY_EXISTS);

    std::filesystem::remove_all(root);
}

// DeleteFileA moved to FileOpsManager (file_ops.cpp) and now performs a real
// VFS delete instead of returning TRUE unconditionally. Its contract — including
// the refusal to delete directories — is covered by the file-ops tests.

// =============================================================================
// GetTempFileNameW / GetTempFileNameA
//
// Learn GetTempFileNameW, plus what it leaves open measured on Windows 11 by
// tools/oracle_probe/gettempfilename_probe (.expected = x64, .x86.expected =
// x86; the two agree). Each expected value below cites its probe line.
// =============================================================================

namespace {

class GetTempFileNameTest : public Kernel32CpuzTest {
protected:
    void SetUp() override {
        Kernel32CpuzTest::SetUp();
        root = std::filesystem::temp_directory_path() /
               ("vitreo_gtfn_" + std::to_string(::getpid()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directories(root / "T" / "sub"));
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string()))
                        .is_ok());
        mgr.set_current_directory("C:\\T");
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::permissions(root / "T" / "deny", std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(root, ec);
    }

    void write_units(GuestAddr addr, std::u16string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u16(addr + i * 2, s[i]);
        (void)memory.write_u16(addr + s.size() * 2, 0);
    }
    void write_bytes(GuestAddr addr, std::string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(addr + i, static_cast<u8>(s[i]));
        (void)memory.write_u8(addr + s.size(), 0);
    }
    std::u16string read_units(GuestAddr addr) {
        std::u16string out;
        for (u32 i = 0; i < 400; ++i) {
            const u16 unit = memory.read_u16(addr + i * 2).value_or(0);
            if (unit == 0) break;
            out.push_back(static_cast<char16_t>(unit));
        }
        return out;
    }

    /// Sets the last error to 0xDEAD (the probe's seed) and calls the export.
    u32 call(bool wide, bool x64, GuestAddr path, GuestAddr prefix, u32 unique, GuestAddr out) {
        if (x64) {
            cpu.init_long_mode();
            cpu.gpr.rsp = STACK_BASE + 0x800;
            cpu.gpr.rcx = path;
            cpu.gpr.rdx = prefix;
            cpu.gpr.r8 = unique;
            cpu.gpr.r9 = out;
        } else {
            cpu.init_protected_mode();
            cpu.gpr.rsp = STACK_BASE + 0x800;
            const GuestAddr sp = cpu.sp();
            (void)memory.write_u32(sp + 4, static_cast<u32>(path));
            (void)memory.write_u32(sp + 8, static_cast<u32>(prefix));
            (void)memory.write_u32(sp + 12, unique);
            (void)memory.write_u32(sp + 16, static_cast<u32>(out));
        }
        set_last_error(0xDEADu);
        if (wide) mgr.impl_GetTempFileNameW(marshaler);
        else mgr.impl_GetTempFileNameA(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }
    /// Fills the output buffer with 0xCC so "untouched" is observable.
    void poison_out() {
        for (u32 i = 0; i < 0x400; ++i) (void)memory.write_u8(kOut + i, 0xCC);
    }
    bool host_exists(const std::string& rel) { return std::filesystem::exists(root / rel); }

    static constexpr GuestAddr kPath = DATA_BASE;
    static constexpr GuestAddr kPrefix = DATA_BASE + 0x400;
    static constexpr GuestAddr kOut = DATA_BASE + 0x800;
    std::filesystem::path root;
};

}  // namespace

// "W u=A ... out=<D>\abcA.tmp file=0", "u=1A2B", "u=12345 ret=0x2345",
// "u=ABCDE ret=0xBCDE", "u=FFFFFFFF ret=0xFFFF", "u=10001 ret=0x1": uppercase
// unpadded hex of the low 16 bits, lowercase ".tmp", no file, the last error
// untouched (57005).
TEST_F(GetTempFileNameTest, NonzeroUniqueFormatsLow16BitsAsUppercaseHex) {
    const std::pair<u32, std::u16string> cases[] = {
        {0x1u, u"1"}, {0xAu, u"A"}, {0xABu, u"AB"}, {0x1A2Bu, u"1A2B"}, {0xFFFFu, u"FFFF"},
        {0x10001u, u"1"}, {0x12345u, u"2345"}, {0xABCDEu, u"BCDE"}, {0xFFFFFFFFu, u"FFFF"}};
    for (bool x64 : {false, true}) {
        for (const auto& [unique, hex] : cases) {
            write_units(kPath, u"C:\\T\\");
            write_units(kPrefix, u"abc");
            const u32 ret = call(true, x64, kPath, kPrefix, unique, kOut);
            EXPECT_EQ(ret, unique & 0xFFFFu) << x64;
            EXPECT_EQ(read_units(kOut), u"C:\\T\\abc" + hex + u".tmp");
            EXPECT_EQ(get_last_error(), 0xDEADu);
            EXPECT_FALSE(host_exists("T/abc" + std::string(hex.begin(), hex.end()) + ".tmp"));

            write_bytes(kPath, "C:\\T\\");
            write_bytes(kPrefix, "abc");
            EXPECT_EQ(call(false, x64, kPath, kPrefix, unique, kOut), unique & 0xFFFFu);
            EXPECT_EQ(read_guest_ansi_string(kOut),
                      "C:\\T\\abc" + std::string(hex.begin(), hex.end()) + ".tmp");
            EXPECT_EQ(get_last_error(), 0xDEADu);
        }
    }
}

// "pre=abcdef -> abc12", "pre=empty / pre=NULL -> 12.tmp", "pre=ABC",
// "pre=nonascii -> \u00E9\u4E2D\u0101 12" (no 7-bit mask), "pre=a\b -> a\b12",
// "A pre=nonascii -> \xE9\xE8z12" (CP_ACP round trip).
TEST_F(GetTempFileNameTest, PrefixIsFirstThreeUnitsUnmasked) {
    for (bool x64 : {false, true}) {
        const std::pair<std::u16string, std::u16string> cases[] = {
            {u"abcdef", u"abc"}, {u"", u""}, {u"ab", u"ab"}, {u"ABC", u"ABC"},
            {u"\u00e9\u4e2d\u0101x", u"\u00e9\u4e2d\u0101"}, {u"a\\b", u"a\\b"}};
        for (const auto& [prefix, used] : cases) {
            write_units(kPath, u"C:\\T\\");
            write_units(kPrefix, prefix);
            EXPECT_EQ(call(true, x64, kPath, kPrefix, 0x12, kOut), 0x12u);
            EXPECT_EQ(read_units(kOut), u"C:\\T\\" + used + u"12.tmp");
        }
        write_units(kPath, u"C:\\T\\");
        EXPECT_EQ(call(true, x64, kPath, 0, 0x12, kOut), 0x12u);
        EXPECT_EQ(read_units(kOut), u"C:\\T\\12.tmp");

        write_bytes(kPath, "C:\\T\\");
        write_bytes(kPrefix, "\xE9\xE8z");
        EXPECT_EQ(call(false, x64, kPath, kPrefix, 0x12, kOut), 0x12u);
        EXPECT_EQ(read_guest_ansi_string(kOut), "C:\\T\\\xE9\xE8z12.tmp");
        EXPECT_EQ(call(false, x64, kPath, 0, 0x12, kOut), 0x12u);
        EXPECT_EQ(read_guest_ansi_string(kOut), "C:\\T\\12.tmp");
    }
}

// "path=noslash", "path=dblslash -> <D>\\abc12", "path=fwdslash -> <CWD>/\abc12",
// "path=dot -> .\abc12", "path=dotslash", "path=dotdot", "path=sub",
// "path=C: -> C:\abc12", "path=C:\", "path=bsbs -> \\abc12",
// "path=empty / path=NULL / path=bs -> \abc12 le=3" (success, error set).
TEST_F(GetTempFileNameTest, PathDropsOneTrailingBackslashAndAddsOne) {
    for (bool x64 : {false, true}) {
        const std::tuple<std::u16string, std::u16string, u32> cases[] = {
            {u"C:\\T", u"C:\\T\\abc12.tmp", 0xDEADu},
            {u"C:\\T\\\\", u"C:\\T\\\\abc12.tmp", 0xDEADu},
            {u"C:\\T/", u"C:\\T/\\abc12.tmp", 0xDEADu},
            {u".", u".\\abc12.tmp", 0xDEADu},
            {u".\\", u".\\abc12.tmp", 0xDEADu},
            {u"..", u"..\\abc12.tmp", 0xDEADu},
            {u"sub", u"sub\\abc12.tmp", 0xDEADu},
            {u"C:", u"C:\\abc12.tmp", 0xDEADu},
            {u"C:\\", u"C:\\abc12.tmp", 0xDEADu},
            {u"\\\\", u"\\\\abc12.tmp", 0xDEADu},
            {u"", u"\\abc12.tmp", error_codes::ERROR_PATH_NOT_FOUND},
            {u"\\", u"\\abc12.tmp", error_codes::ERROR_PATH_NOT_FOUND}};
        for (const auto& [path, name, error] : cases) {
            write_units(kPath, path);
            write_units(kPrefix, u"abc");
            EXPECT_EQ(call(true, x64, kPath, kPrefix, 0x12, kOut), 0x12u)
                << std::string(path.begin(), path.end());
            EXPECT_EQ(read_units(kOut), name);
            EXPECT_EQ(get_last_error(), error);
        }
        write_units(kPrefix, u"abc");
        EXPECT_EQ(call(true, x64, 0, kPrefix, 0x12, kOut), 0x12u);
        EXPECT_EQ(read_units(kOut), u"\\abc12.tmp");
        EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
        write_bytes(kPrefix, "abc");
        EXPECT_EQ(call(false, x64, 0, kPrefix, 0x12, kOut), 0x12u);
        EXPECT_EQ(read_guest_ansi_string(kOut), "\\abc12.tmp");
        EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
    }
}

// "path=missing / missing_u0 / missing\ / missing/ / missing2_u0 / isfile /
// NUL ret=0x0 le=267 out=<stripped path>"; "A path=missing_u0 ... untouched".
TEST_F(GetTempFileNameTest, PathThatIsNotADirectoryFailsErrorDirectory) {
    {
        std::ofstream file(root / "T" / "afile");
        ASSERT_TRUE(file.good());
    }
    for (bool x64 : {false, true}) {
        const std::pair<std::u16string, std::u16string> cases[] = {
            {u"C:\\T\\missing", u"C:\\T\\missing"},
            {u"C:\\T\\missing\\", u"C:\\T\\missing"},
            {u"C:\\T\\missing/", u"C:\\T\\missing/"},
            {u"C:\\T\\missing\\deeper", u"C:\\T\\missing\\deeper"},
            {u"C:\\T\\afile", u"C:\\T\\afile"},
            {u"NUL", u"NUL"}};
        for (const auto& [path, buffer] : cases) {
            for (u32 unique : {0x12u, 0u}) {
                write_units(kPath, path);
                write_units(kPrefix, u"abc");
                poison_out();
                EXPECT_EQ(call(true, x64, kPath, kPrefix, unique, kOut), 0u);
                EXPECT_EQ(get_last_error(), error_codes::ERROR_DIRECTORY);
                EXPECT_EQ(read_units(kOut), buffer);
            }
        }
        write_bytes(kPath, "C:\\T\\missing");
        write_bytes(kPrefix, "abc");
        poison_out();
        EXPECT_EQ(call(false, x64, kPath, kPrefix, 0, kOut), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_DIRECTORY);
        EXPECT_EQ(memory.read_u8(kOut).value_or(0), 0xCCu);
    }
}

// "len=247 ... le=267" (checked, not too long), "len=248 ... le=111
// out=untouched", "len=247+bs le=267", "len=247+bsbs le=111", the same for
// every prefix length and for the A form and uUnique 0.
TEST_F(GetTempFileNameTest, PathLongerThan247AfterStrippingFailsBufferOverflow) {
    auto long_path = [](usize len) {
        std::u16string p = u"C:\\";
        while (p.size() < len) p.push_back(u'x');
        return p;
    };
    for (bool x64 : {false, true}) {
        for (std::u16string_view prefix : {u"", u"abc", u"abcdef"}) {
            for (u32 unique : {0x12u, 0u}) {
                write_units(kPrefix, prefix);
                write_units(kPath, long_path(247));
                poison_out();
                EXPECT_EQ(call(true, x64, kPath, kPrefix, unique, kOut), 0u);
                EXPECT_EQ(get_last_error(), error_codes::ERROR_DIRECTORY);
                EXPECT_EQ(read_units(kOut), long_path(247));

                write_units(kPath, long_path(247) + u"\\");
                EXPECT_EQ(call(true, x64, kPath, kPrefix, unique, kOut), 0u);
                EXPECT_EQ(get_last_error(), error_codes::ERROR_DIRECTORY);

                for (const std::u16string& p : {long_path(248), long_path(247) + u"\\\\",
                                                long_path(248) + u"\\", long_path(300)}) {
                    write_units(kPath, p);
                    poison_out();
                    EXPECT_EQ(call(true, x64, kPath, kPrefix, unique, kOut), 0u);
                    EXPECT_EQ(get_last_error(), error_codes::ERROR_BUFFER_OVERFLOW);
                    EXPECT_EQ(memory.read_u16(kOut).value_or(0), 0xCCCCu);
                }
            }
        }
        write_bytes(kPrefix, "abc");
        std::u16string p = long_path(248);
        write_bytes(kPath, std::string(p.begin(), p.end()));
        EXPECT_EQ(call(false, x64, kPath, kPrefix, 0x12, kOut), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_BUFFER_OVERFLOW);
    }
}

// "exists_u12 ret=0x12 le=57005 ... size_after=5": a nonzero number never
// checks, creates or truncates.
TEST_F(GetTempFileNameTest, NonzeroUniqueLeavesExistingFileAlone) {
    {
        std::ofstream file(root / "T" / "exi12.tmp");
        file << "hello";
    }
    write_units(kPath, u"C:\\T");
    write_units(kPrefix, u"exi");
    EXPECT_EQ(call(true, false, kPath, kPrefix, 0x12, kOut), 0x12u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(std::filesystem::file_size(root / "T" / "exi12.tmp"), 5u);
}

// "counter first n=.. tick_plus_n=1" ... "after_collision": the first number
// tried is (GetTickCount() + n) & 0xFFFF; n counts generating calls past the
// directory check (nonzero uUnique, a missing directory and an overflowing
// path do not count; uUnique 0x10000 and the A form do). "W pre=u0 ret=TIME
// le=0 ... file=1": the file is created and ERROR_SUCCESS is left.
TEST_F(GetTempFileNameTest, ZeroUniqueStartsAtTickPlusCallCountAndCreatesFile) {
    u32 n = 0;
    auto generate = [&](bool x64, u32 unique, const char16_t* prefix) {
        write_units(kPath, u"C:\\T\\");
        write_units(kPrefix, prefix);
        const u32 t0 = tick_count32();
        const u32 ret = call(true, x64, kPath, kPrefix, unique, kOut);
        const u32 t1 = tick_count32();
        EXPECT_NE(ret, 0u);
        EXPECT_LE((ret - t0 - n) & 0xFFFFu, t1 - t0) << "n=" << n;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%X", ret);
        std::u16string name = u"C:\\T\\" + std::u16string(prefix);
        for (const char* h = hex; *h; ++h) name.push_back(static_cast<char16_t>(*h));
        EXPECT_EQ(read_units(kOut), name + u".tmp");
        std::string host = "T/" + std::string(name.begin() + 5, name.end()) + ".tmp";
        EXPECT_TRUE(host_exists(host)) << host;
        EXPECT_EQ(std::filesystem::file_size(root / host), 0u);
        ++n;
    };
    generate(false, 0, u"c0");
    write_units(kPath, u"C:\\T\\");
    write_units(kPrefix, u"nz");
    for (int i = 0; i < 3; ++i) (void)call(true, false, kPath, kPrefix, 0x12 + i, kOut);
    write_units(kPath, u"C:\\T\\missing");
    for (int i = 0; i < 3; ++i) (void)call(true, false, kPath, kPrefix, 0, kOut);
    generate(true, 0, u"c1");
    generate(true, 0x10000, u"c2");
    generate(false, 0x30000, u"c3");

    // The A form shares the counter and the contract.
    write_bytes(kPath, ".");
    write_bytes(kPrefix, "b");
    const u32 t0 = tick_count32();
    const u32 ret = call(false, true, kPath, kPrefix, 0, kOut);
    EXPECT_LE((ret - t0 - n) & 0xFFFFu, tick_count32() - t0);
    char expect[32];
    std::snprintf(expect, sizeof(expect), ".\\b%X.tmp", ret);
    EXPECT_EQ(read_guest_ansi_string(kOut), expect);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_TRUE(host_exists(std::string("T/") + (expect + 2)));
}

// "collision skip200 lands_after_run=1", "dircollision skips_dirs=1": an
// existing file or directory moves on by one without advancing n.
TEST_F(GetTempFileNameTest, ZeroUniqueSkipsExistingFilesAndDirectories) {
    for (bool x64 : {false, true}) {
        const u32 n = x64 ? 1 : 0;
        const u32 start = (tick_count32() + n) & 0xFFFFu;
        constexpr u32 kTaken = 400;  // covers the ticks that pass before the call
        u32 last = 0;
        for (u32 k = 0; k < kTaken; ++k) {
            u32 v = (start + k) & 0xFFFFu;
            if (v == 0) continue;
            char name[32];
            std::snprintf(name, sizeof(name), "cc%X.tmp", v);
            if (k % 2) {
                std::filesystem::create_directory(root / "T" / name);
            } else {
                std::ofstream(root / "T" / name).put('x');
            }
            last = v;
        }
        write_units(kPath, u"C:\\T");
        write_units(kPrefix, u"cc");
        const u32 ret = call(true, x64, kPath, kPrefix, 0, kOut);
        EXPECT_EQ(ret, last == 0xFFFFu ? 1u : last + 1) << x64;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
        for (u32 k = 0; k < kTaken; ++k) {
            char name[32];
            std::snprintf(name, sizeof(name), "cc%X.tmp", (start + k) & 0xFFFFu);
            std::filesystem::remove_all(root / "T" / name);
        }
        std::filesystem::remove(root / "T" / ("cc" + [&] { char h[8]; std::snprintf(h, 8, "%X", ret); return std::string(h); }() + ".tmp"));
    }
}

// "genfail deny_u0 ret=0x0 le=5 buffer_is_tick_plus_n+0=1", "A deny_u0 ...
// out=untouched", "deny_u12 ret=0x12": a CreateFile error other than
// "exists" fails at once and leaves the candidate in the W buffer;
// "pre=a\b_u0 ret=0x0 le=3 out=<D>\a\b<h>.tmp".
TEST_F(GetTempFileNameTest, ZeroUniqueStopsOnOtherCreateErrors) {
    std::filesystem::create_directory(root / "T" / "deny");
    std::filesystem::permissions(root / "T" / "deny",
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    for (bool x64 : {false, true}) {
        write_units(kPath, u"C:\\T\\deny");
        write_units(kPrefix, u"abc");
        const u32 n = x64 ? 1 : 0;
        const u32 t0 = tick_count32();
        EXPECT_EQ(call(true, x64, kPath, kPrefix, 0, kOut), 0u);
        const u32 t1 = tick_count32();
        EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
        const std::u16string buffer = read_units(kOut);
        ASSERT_EQ(buffer.rfind(u"C:\\T\\deny\\abc", 0), 0u);
        const std::string hex(buffer.begin() + 13, buffer.end() - 4);
        EXPECT_LE((static_cast<u32>(std::stoul(hex, nullptr, 16)) - t0 - n) & 0xFFFFu, t1 - t0);

        EXPECT_EQ(call(true, x64, kPath, kPrefix, 0x12, kOut), 0x12u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
    }
    write_bytes(kPath, "C:\\T\\deny");
    write_bytes(kPrefix, "abc");
    poison_out();
    EXPECT_EQ(call(false, false, kPath, kPrefix, 0, kOut), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(memory.read_u8(kOut).value_or(0), 0xCCu);

    write_units(kPath, u"C:\\T");
    write_units(kPrefix, u"a\\b");
    EXPECT_EQ(call(true, true, kPath, kPrefix, 0, kOut), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
    EXPECT_EQ(read_units(kOut).rfind(u"C:\\T\\a\\b", 0), 0u);
}

// A NULL lpTempFileName faults on Windows (the probe's first draft died with
// 0xC0000005 there), so it raises STATUS_ACCESS_VIOLATION for a write.
TEST_F(GetTempFileNameTest, NullOutputBufferRaisesAccessViolation) {
    write_units(kPath, u"C:\\T");
    write_units(kPrefix, u"abc");
    (void)call(true, true, kPath, kPrefix, 0x12, 0);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
}

// SetSearchPathMode (tests/oracle_data/setsearchpathmode_probe{32,64}.expected,
// x86 == x64): only 0x1, 0x10000 and 0x8001 are valid (else
// ERROR_INVALID_PARAMETER); after 0x8001, 0x1 and 0x10000 fail with
// ERROR_ACCESS_DENIED while 0x8001 still succeeds. SearchPath(NULL, ...) finds
// xcopy.exe in the current directory when not safe and in System32 when safe.
TEST_F(Kernel32CpuzTest, SetSearchPathModeValidatesAndMovesTheCurrentDirectory) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("vitreo_spm_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "Windows" / "System32"));
    ASSERT_TRUE(std::filesystem::create_directories(root / "Work"));
    for (const auto& p : {root / "Windows" / "System32" / "xcopy.exe", root / "Work" / "xcopy.exe"}) {
        std::ofstream file(p, std::ios::binary);
        file << "MZ";
    }
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr.set_current_directory("C:\\Work");

    const GuestAddr stack = cpu.sp();
    auto set_mode = [&](u32 flags) {
        ASSERT_TRUE(memory.write_u32(stack + 4, flags).is_ok());
        set_last_error(0);
        mgr.impl_SetSearchPathMode(marshaler);
    };
    constexpr GuestAddr filename = DATA_BASE + 0x480;
    constexpr GuestAddr output = DATA_BASE + 0x600;
    write_guest_wide_string(filename, "xcopy.exe");
    auto where = [&]() {
        (void)memory.write_u32(stack + 4, 0);
        (void)memory.write_u32(stack + 8, filename);
        (void)memory.write_u32(stack + 12, 0);
        (void)memory.write_u32(stack + 16, 260);
        (void)memory.write_u32(stack + 20, output);
        (void)memory.write_u32(stack + 24, 0);
        mgr.impl_SearchPathW(marshaler);
        return cpu.gpr.rax != 0 ? read_guest_wide_string(output) : std::string("none");
    };

    for (const u32 bad : {0x0u, 0x8000u, 0x10001u, 0x18000u, 0x3u, 0x80000001u}) {  // == flags
        SCOPED_TRACE(bad);
        set_mode(bad);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    }
    EXPECT_EQ(where(), "C:\\Work\\xcopy.exe");                 // default -> cwd
    set_mode(0x1);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    set_mode(0x1);                                             // enable-again
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(where(), "C:\\Windows\\System32\\xcopy.exe");   // safe -> system
    set_mode(0x10000);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(where(), "C:\\Work\\xcopy.exe");                 // unsafe -> cwd

    set_mode(0x8001);                                          // == permanent
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(where(), "C:\\Windows\\System32\\xcopy.exe");
    set_mode(0x1);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    set_mode(0x8001);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    set_mode(0x10000);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    set_mode(0x0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}
