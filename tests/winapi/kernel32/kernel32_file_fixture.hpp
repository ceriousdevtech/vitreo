#pragma once
/// @file kernel32_file_fixture.hpp
/// @brief Shared gtest fixtures (MockFileSystem, FileManagerTest, FileConsoleBridgeTest) for the Kernel32 file tests (test_file*.cpp)
/// @note gtest runs one suite's tests in (file, line) order, so the test_file*.cpp names that
///       hold FileConsoleBridgeTest sort alphabetically in the original source order; keep that
///       when adding or renaming one.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/filesystem/file_ops.hpp>
#include <winapi/kernel32/console/console.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/filesystem.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/memory/guest_memory.hpp>
#include <ipc/ipc_gate.hpp>
#include "runtime/execution/inherited_handles.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include <libproc.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;

// =============================================================================
// MockFileSystem — minimal IFileSystem for unit testing
// =============================================================================

/// In-memory mock filesystem that tracks open/close calls
class MockFileSystem : public IFileSystem {
public:
    /// How many times open() was called (atomic for concurrent CreateFile tests)
    std::atomic<u32> open_count{0};

    /// Last parameters passed to open()
    AccessMode last_access = AccessMode::None;
    ShareMode last_share = ShareMode::None;
    CreationDisposition last_disposition = CreationDisposition::OpenExisting;
    std::string last_path;

    /// If true, open() will fail
    bool fail_open = false;

    /// Set of closed VFS handles
    std::unordered_set<FileHandle> closed_handles;

    /// Directory entries returned for handle-based enumeration tests.
    std::vector<DirectoryEntry> directory_entries;

    /// Whether newly opened handles identify a directory via stat_handle().
    bool opened_handle_is_directory = false;

    // -------------------------------------------------------------------------
    // IFileSystem interface
    // -------------------------------------------------------------------------

    Result<FileHandle, VfsError> open(
        std::string_view path,
        AccessMode access,
        ShareMode share,
        CreationDisposition disposition) override
    {
        last_path = std::string(path);
        last_access = access;
        last_share = share;
        last_disposition = disposition;

        if (fail_open) {
            return Result<FileHandle, VfsError>::err(VfsError::PathNotFound);
        }

        const u32 id = ++open_count;
        // Return a unique VFS handle based on open count
        return Result<FileHandle, VfsError>::ok(id);
    }

    Result<void, VfsError> close(FileHandle handle) override {
        std::lock_guard<std::mutex> lock(closed_mutex_);
        closed_handles.insert(handle);
        return Result<void, VfsError>::ok();
    }

    Result<usize, VfsError> read(FileHandle /*handle*/, std::span<u8> /*buffer*/) override {
        return Result<usize, VfsError>::err(VfsError::NotSupported);
    }

    Result<usize, VfsError> write(FileHandle /*handle*/, std::span<const u8> /*data*/) override {
        return Result<usize, VfsError>::err(VfsError::NotSupported);
    }

    Result<u64, VfsError> seek(FileHandle /*handle*/, i64 /*offset*/, SeekOrigin /*origin*/) override {
        return Result<u64, VfsError>::err(VfsError::NotSupported);
    }

    Result<FileInfo, VfsError> stat(std::string_view /*path*/) override {
        return Result<FileInfo, VfsError>::err(VfsError::NotSupported);
    }

    Result<FileInfo, VfsError> stat_handle(FileHandle /*handle*/) override {
        FileInfo info{};
        if (opened_handle_is_directory) {
            info.attributes = FileAttributes::Directory;
        }
        return Result<FileInfo, VfsError>::ok(info);
    }

    Result<std::vector<DirectoryEntry>, VfsError> list_dir(std::string_view /*path*/) override {
        return Result<std::vector<DirectoryEntry>, VfsError>::err(VfsError::NotSupported);
    }

    Result<std::vector<DirectoryEntry>, VfsError>
    list_dir_handle(FileHandle /*handle*/) override {
        return Result<std::vector<DirectoryEntry>, VfsError>::ok(
            directory_entries);
    }

    Result<void, VfsError> create_dir(std::string_view /*path*/) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    Result<void, VfsError> remove(std::string_view /*path*/) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    Result<void, VfsError> rename(std::string_view /*old_path*/, std::string_view /*new_path*/) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

private:
    std::mutex closed_mutex_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class FileManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_fs = new MockFileSystem();
        // Vfs takes ownership via unique_ptr
        auto mount_result = vfs.mount_drive('C', std::unique_ptr<IFileSystem>(mock_fs));
        ASSERT_TRUE(mount_result.is_ok());
    }

    Vfs vfs;
    HandleTable handles;
    MockFileSystem* mock_fs = nullptr; // raw pointer; Vfs owns it

    /// Helper to create a FileManager (must be called after SetUp)
    FileManager make_fm() { return FileManager(vfs, handles); }
};

/// Fixture with GuestMemory for ReadFile/WriteFile and console bridge tests
class FileConsoleBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_fs = new MockFileSystem();
        auto mount_result = vfs.mount_drive('C', std::unique_ptr<IFileSystem>(mock_fs));
        ASSERT_TRUE(mount_result.is_ok());

        // Map a guest memory page for buffer/bytes-written locations
        auto map_result = guest_mem.map_region(
            0x10000, 4096, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result.is_ok());
    }

    Vfs vfs;
    HandleTable handles;
    GuestMemory guest_mem;
    MockFileSystem* mock_fs = nullptr;

    static constexpr GuestAddr kBufAddr     = 0x10000;
    static constexpr GuestAddr kWrittenAddr = 0x10100;
};
