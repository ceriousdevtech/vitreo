/// @file test_filepos.cpp
/// @brief Unit tests for file positioning APIs (WIN32-037)
///
/// Tests GetFileSize, GetFileSizeEx, SetFilePointer, SetFilePointerEx
/// and the make_filepos_exports registration, exercising the underlying
/// FileManager methods directly (thunk wrappers are thin argument readers).

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <capsule/vfs/filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/filesystem/filepos.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/system/ntdll/ntdll_ext.hpp>
#include <winapi/system/ntdll/ntdll.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;

// =============================================================================
// In-Memory File Object
// =============================================================================

namespace {

struct MemoryFile {
    std::vector<u8> data;
    u64             position = 0;
    AccessMode      access   = AccessMode::None;
};

// =============================================================================
// SeekableFileSystem — IFileSystem with full seek/size support
// =============================================================================

/// Fully in-memory IFileSystem that stores file content in vectors.
/// Supports open / close / read / write / seek / stat.
class SeekableFileSystem final : public IFileSystem {
public:
    /// Seed a file with known content before tests run.
    void seed_file(const std::string& path, std::vector<u8> content) {
        stored_files_[path] = std::move(content);
    }

    // -- IFileSystem -----------------------------------------------------------

    [[nodiscard]] Result<FileHandle, VfsError> open(
            std::string_view path,
            AccessMode access,
            ShareMode /*share*/,
            CreationDisposition disp) override {

        std::string key(path);
        bool exists = stored_files_.count(key) > 0;

        switch (disp) {
            case CreationDisposition::CreateNew:
                if (exists)
                    return Result<FileHandle, VfsError>::err(
                        VfsError::PathAlreadyExists);
                stored_files_[key] = {};
                break;
            case CreationDisposition::CreateAlways:
                stored_files_[key] = {};
                break;
            case CreationDisposition::OpenExisting:
                if (!exists)
                    return Result<FileHandle, VfsError>::err(
                        VfsError::PathNotFound);
                break;
            case CreationDisposition::OpenAlways:
                if (!exists) stored_files_[key] = {};
                break;
            case CreationDisposition::TruncateExisting:
                if (!exists)
                    return Result<FileHandle, VfsError>::err(
                        VfsError::PathNotFound);
                stored_files_[key].clear();
                break;
        }

        FileHandle fh      = next_handle_++;
        auto& mf           = open_files_[fh];
        mf.data             = stored_files_[key]; // snapshot
        mf.position         = 0;
        mf.access           = access;
        path_for_handle_[fh] = key;
        return Result<FileHandle, VfsError>::ok(fh);
    }

    [[nodiscard]] Result<void, VfsError> close(FileHandle handle) override {
        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<void, VfsError>::err(VfsError::InvalidHandle);

        auto pit = path_for_handle_.find(handle);
        if (pit != path_for_handle_.end()) {
            stored_files_[pit->second] = it->second.data;
            path_for_handle_.erase(pit);
        }
        open_files_.erase(it);
        return Result<void, VfsError>::ok();
    }

    [[nodiscard]] Result<usize, VfsError> read(
            FileHandle handle, std::span<u8> buffer) override {

        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<usize, VfsError>::err(VfsError::InvalidHandle);

        auto& mf    = it->second;
        usize avail = (mf.position < mf.data.size())
                          ? (mf.data.size() - static_cast<usize>(mf.position))
                          : 0u;
        usize n = std::min(avail, buffer.size());
        if (n > 0) {
            std::memcpy(buffer.data(),
                        mf.data.data() + static_cast<usize>(mf.position), n);
            mf.position += n;
        }
        return Result<usize, VfsError>::ok(n);
    }

    [[nodiscard]] Result<usize, VfsError> write(
            FileHandle handle, std::span<const u8> data) override {

        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<usize, VfsError>::err(VfsError::InvalidHandle);

        auto& mf  = it->second;
        usize pos = static_cast<usize>(mf.position);
        usize end = pos + data.size();
        if (end > mf.data.size()) mf.data.resize(end);
        std::memcpy(mf.data.data() + pos, data.data(), data.size());
        mf.position += data.size();

        auto pit = path_for_handle_.find(handle);
        if (pit != path_for_handle_.end())
            stored_files_[pit->second] = mf.data;

        return Result<usize, VfsError>::ok(data.size());
    }

    [[nodiscard]] Result<u64, VfsError> seek(
            FileHandle handle, i64 offset, SeekOrigin origin) override {

        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<u64, VfsError>::err(VfsError::InvalidHandle);

        auto& mf = it->second;
        i64 base = 0;
        switch (origin) {
            case SeekOrigin::Begin:
                base = 0;
                break;
            case SeekOrigin::Current:
                base = static_cast<i64>(mf.position);
                break;
            case SeekOrigin::End:
                base = static_cast<i64>(mf.data.size());
                break;
        }
        i64 new_pos = base + offset;
        if (new_pos < 0)
            return Result<u64, VfsError>::err(VfsError::SeekError);

        mf.position = static_cast<u64>(new_pos);
        return Result<u64, VfsError>::ok(mf.position);
    }

    [[nodiscard]] Result<FileInfo, VfsError> stat(
            std::string_view path) override {

        std::string key(path);
        auto it = stored_files_.find(key);
        if (it == stored_files_.end())
            return Result<FileInfo, VfsError>::err(VfsError::PathNotFound);

        FileInfo info{};
        info.size       = it->second.size();
        info.attributes = FileAttributes::Normal;
        return Result<FileInfo, VfsError>::ok(info);
    }

    /// fstat-equivalent: GetFileSize/GetFileSizeEx query the open handle
    /// (Learn: they do not move the file pointer), so report this handle's
    /// own view of the file.
    [[nodiscard]] Result<FileInfo, VfsError> stat_handle(
            FileHandle handle) override {

        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<FileInfo, VfsError>::err(VfsError::InvalidHandle);

        FileInfo info{};
        info.size       = it->second.data.size();
        info.attributes = FileAttributes::Normal;
        return Result<FileInfo, VfsError>::ok(info);
    }

    [[nodiscard]] Result<std::vector<DirectoryEntry>, VfsError>
    list_dir(std::string_view) override {
        return Result<std::vector<DirectoryEntry>, VfsError>::err(
            VfsError::NotSupported);
    }
    [[nodiscard]] Result<void, VfsError> create_dir(std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
    [[nodiscard]] Result<void, VfsError> remove(std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
    [[nodiscard]] Result<void, VfsError> rename(
            std::string_view, std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

private:
    std::unordered_map<std::string, std::vector<u8>> stored_files_;
    std::unordered_map<FileHandle, MemoryFile>       open_files_;
    std::unordered_map<FileHandle, std::string>      path_for_handle_;
    FileHandle next_handle_ = 1;
};

} // anonymous namespace

// =============================================================================
// Test Fixture
// =============================================================================

class FilePosTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kBaseAddr   = 0x10000;
    static constexpr usize     kRegionSize = 0x10000; // 64 KiB

    GuestMemory                  memory_;
    HandleTable                  handles_;
    Vfs                          vfs_;
    SeekableFileSystem*          fs_ = nullptr; // non-owning; Vfs owns it
    std::unique_ptr<FileManager> fm_;
    NtdllExtManager             nt_files_;

    void SetUp() override {
        auto mr = memory_.map_region(kBaseAddr, kRegionSize,
                                     MemoryProtection::ReadWrite);
        ASSERT_TRUE(mr.is_ok()) << "Failed to map guest memory region";

        auto ufs = std::make_unique<SeekableFileSystem>();
        fs_ = ufs.get();
        auto mv = vfs_.mount_drive('C', std::move(ufs));
        ASSERT_TRUE(mv.is_ok()) << "Failed to mount C: drive";

        fm_ = std::make_unique<FileManager>(vfs_, handles_, memory_);
        nt_files_.set_vfs(&vfs_);
        nt_files_.set_handle_table(&handles_);
    }

    // ----- helpers -----------------------------------------------------------

    Handle open_read(const char* win_path) {
        return fm_->create_file(win_path, GENERIC_READ,
                                FILE_SHARE_READ, OPEN_EXISTING, 0);
    }

    Handle open_rw(const char* win_path) {
        return fm_->create_file(win_path,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                CREATE_ALWAYS, 0);
    }
};

// =============================================================================
// GetFileSize Tests
// =============================================================================

TEST_F(FilePosTest, GetFileSizeReturnsCorrectSize) {
    std::vector<u8> data(256, 0xAA);
    fs_->seed_file("sized.txt", data);

    Handle h = open_read("C:\\sized.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 size = fm_->get_file_size(h, nullptr);
    EXPECT_EQ(size, 256u);
}

TEST_F(FilePosTest, GetFileSizeEmptyFile) {
    fs_->seed_file("empty.txt", {});

    Handle h = open_read("C:\\empty.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 size = fm_->get_file_size(h, nullptr);
    EXPECT_EQ(size, 0u);
}

TEST_F(FilePosTest, GetFileSizeReturnsHighPart) {
    // For a small file the high part should be 0
    std::vector<u8> data(1000, 0xBB);
    fs_->seed_file("hpart.txt", data);

    Handle h = open_read("C:\\hpart.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 high = 0xDEAD; // sentinel — should be overwritten with 0
    u32 low  = fm_->get_file_size(h, &high);
    EXPECT_EQ(low, 1000u);
    EXPECT_EQ(high, 0u);
}

TEST_F(FilePosTest, GetFileSizeInvalidHandleReturnsFailure) {
    u32 size = fm_->get_file_size(0xDEAD, nullptr);
    EXPECT_EQ(size, INVALID_FILE_SIZE);
}

TEST_F(FilePosTest, GetFileSizePreservesFilePosition) {
    std::vector<u8> data(100, 0xCC);
    fs_->seed_file("preserve.txt", data);

    Handle h = open_read("C:\\preserve.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Seek to middle
    fm_->set_file_pointer(h, 42, nullptr, FILE_BEGIN);

    // GetFileSize should not disturb position
    EXPECT_EQ(fm_->get_file_size(h, nullptr), 100u);

    // Verify position is still 42
    u32 pos = fm_->set_file_pointer(h, 0, nullptr, FILE_CURRENT);
    EXPECT_EQ(pos, 42u);
}

// =============================================================================
// GetFileSizeEx Tests (64-bit size)
// =============================================================================

TEST_F(FilePosTest, GetFileSizeExReturns64BitSize) {
    // Use get_file_size with both low and high to simulate GetFileSizeEx
    std::vector<u8> data(500, 0xDD);
    fs_->seed_file("ex.txt", data);

    Handle h = open_read("C:\\ex.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 high = 0;
    u32 low  = fm_->get_file_size(h, &high);
    u64 full = (static_cast<u64>(high) << 32) | static_cast<u64>(low);

    EXPECT_EQ(full, 500u);
}

TEST_F(FilePosTest, GetFileSizeExInvalidHandleFails) {
    u32 high = 0;
    u32 low  = fm_->get_file_size(0xBEEF, &high);
    // On failure, returns INVALID_FILE_SIZE
    EXPECT_EQ(low, INVALID_FILE_SIZE);
}

TEST_F(FilePosTest, GetFileSizeExAcceptsHandleCreatedByNtCreateFile) {
    fs_->seed_file("native.txt", std::vector<u8>(37, 0x5A));
    const auto opened = nt_files_.nt_create_file(
        "C:\\native.txt", 0x0080, 0, 0x7, nt_file::FILE_OPEN, 0);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rcx = opened.handle;
    cpu.gpr.rdx = kBaseAddr;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);

    impl_GetFileSizeEx(marshaler, *fm_, &nt_files_);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    ASSERT_TRUE(memory_.read_u64(kBaseAddr).is_ok());
    EXPECT_EQ(memory_.read_u64(kBaseAddr).value(), 37u);
}

// =============================================================================
// SetFilePointer Tests — FILE_BEGIN
// =============================================================================

TEST_F(FilePosTest, SetFilePointerFileBeginAbsolute) {
    std::vector<u8> data = {10, 20, 30, 40, 50};
    fs_->seed_file("seek.txt", data);

    Handle h = open_read("C:\\seek.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 pos = fm_->set_file_pointer(h, 3, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, 3u);
}

TEST_F(FilePosTest, SetFilePointerFileBeginToStart) {
    std::vector<u8> data(100, 0);
    fs_->seed_file("start.txt", data);

    Handle h = open_read("C:\\start.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Move somewhere first
    fm_->set_file_pointer(h, 50, nullptr, FILE_BEGIN);

    // Seek back to beginning
    u32 pos = fm_->set_file_pointer(h, 0, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, 0u);
}

// =============================================================================
// SetFilePointer Tests — FILE_CURRENT
// =============================================================================

TEST_F(FilePosTest, SetFilePointerFileCurrentForward) {
    std::vector<u8> data(100, 0);
    fs_->seed_file("cur.txt", data);

    Handle h = open_read("C:\\cur.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    fm_->set_file_pointer(h, 10, nullptr, FILE_BEGIN);
    u32 pos = fm_->set_file_pointer(h, 5, nullptr, FILE_CURRENT);
    EXPECT_EQ(pos, 15u);
}

TEST_F(FilePosTest, SetFilePointerFileCurrentBackward) {
    std::vector<u8> data(100, 0);
    fs_->seed_file("curback.txt", data);

    Handle h = open_read("C:\\curback.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    fm_->set_file_pointer(h, 20, nullptr, FILE_BEGIN);
    u32 pos = fm_->set_file_pointer(h, -5, nullptr, FILE_CURRENT);
    EXPECT_EQ(pos, 15u);
}

// =============================================================================
// SetFilePointer Tests — FILE_END
// =============================================================================

TEST_F(FilePosTest, SetFilePointerFileEndFromEnd) {
    std::vector<u8> data(50, 0);
    fs_->seed_file("end.txt", data);

    Handle h = open_read("C:\\end.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 pos = fm_->set_file_pointer(h, -10, nullptr, FILE_END);
    EXPECT_EQ(pos, 40u);
}

TEST_F(FilePosTest, SetFilePointerFileEndAtEnd) {
    std::vector<u8> data(30, 0);
    fs_->seed_file("atend.txt", data);

    Handle h = open_read("C:\\atend.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 pos = fm_->set_file_pointer(h, 0, nullptr, FILE_END);
    EXPECT_EQ(pos, 30u);
}

// =============================================================================
// SetFilePointer — 64-bit via distance_high
// =============================================================================

TEST_F(FilePosTest, SetFilePointerWithHighWord) {
    std::vector<u8> data(200, 0);
    fs_->seed_file("high.txt", data);

    Handle h = open_read("C:\\high.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Seek with a high word of 0 — should just use the low word
    i32 high = 0;
    u32 pos = fm_->set_file_pointer(h, 100, &high, FILE_BEGIN);
    EXPECT_EQ(pos, 100u);
    EXPECT_EQ(high, 0);
}

// =============================================================================
// SetFilePointerEx Tests (64-bit offset)
// =============================================================================

TEST_F(FilePosTest, SetFilePointerExAbsoluteSeek) {
    // Test the underlying mechanism: decompose i64 → low/high, call
    // set_file_pointer, recompose to u64.
    std::vector<u8> data(500, 0);
    fs_->seed_file("ex_seek.txt", data);

    Handle h = open_read("C:\\ex_seek.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Simulate SetFilePointerEx with 64-bit offset 250
    i64 distance = 250;
    i32 dist_low  = static_cast<i32>(static_cast<u32>(distance & 0xFFFFFFFF));
    i32 dist_high = static_cast<i32>(static_cast<u32>(
        (static_cast<u64>(distance) >> 32) & 0xFFFFFFFF));

    u32 low = fm_->set_file_pointer(h, dist_low, &dist_high, FILE_BEGIN);
    EXPECT_NE(low, INVALID_SET_FILE_POINTER);

    u64 new_pos = static_cast<u64>(low)
                | (static_cast<u64>(static_cast<u32>(dist_high)) << 32);
    EXPECT_EQ(new_pos, 250u);
}

TEST_F(FilePosTest, SetFilePointerExRelativeSeek) {
    std::vector<u8> data(300, 0);
    fs_->seed_file("ex_rel.txt", data);

    Handle h = open_read("C:\\ex_rel.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Seek to 100 first
    fm_->set_file_pointer(h, 100, nullptr, FILE_BEGIN);

    // Move forward 50 via 64-bit path
    i64 distance = 50;
    i32 dist_low  = static_cast<i32>(static_cast<u32>(distance & 0xFFFFFFFF));
    i32 dist_high = static_cast<i32>(static_cast<u32>(
        (static_cast<u64>(distance) >> 32) & 0xFFFFFFFF));

    u32 low = fm_->set_file_pointer(h, dist_low, &dist_high, FILE_CURRENT);
    EXPECT_NE(low, INVALID_SET_FILE_POINTER);

    u64 new_pos = static_cast<u64>(low)
                | (static_cast<u64>(static_cast<u32>(dist_high)) << 32);
    EXPECT_EQ(new_pos, 150u);
}

TEST_F(FilePosTest, SetFilePointerExFromEnd) {
    std::vector<u8> data(200, 0);
    fs_->seed_file("ex_end.txt", data);

    Handle h = open_read("C:\\ex_end.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Seek 20 bytes before end via 64-bit path
    i64 distance = -20;
    i32 dist_low  = static_cast<i32>(static_cast<u32>(distance & 0xFFFFFFFF));
    i32 dist_high = static_cast<i32>(static_cast<u32>(
        (static_cast<u64>(distance) >> 32) & 0xFFFFFFFF));

    u32 low = fm_->set_file_pointer(h, dist_low, &dist_high, FILE_CURRENT);
    // Position 0 + (-20) would be negative → from FILE_END instead
    // Re-do with FILE_END
    dist_low  = static_cast<i32>(static_cast<u32>(distance & 0xFFFFFFFF));
    dist_high = static_cast<i32>(static_cast<u32>(
        (static_cast<u64>(distance) >> 32) & 0xFFFFFFFF));

    low = fm_->set_file_pointer(h, dist_low, &dist_high, FILE_END);
    EXPECT_NE(low, INVALID_SET_FILE_POINTER);

    u64 new_pos = static_cast<u64>(low)
                | (static_cast<u64>(static_cast<u32>(dist_high)) << 32);
    EXPECT_EQ(new_pos, 180u);
}

// =============================================================================
// Error Cases
// =============================================================================

TEST_F(FilePosTest, SetFilePointerInvalidHandleReturnsFailure) {
    u32 pos = fm_->set_file_pointer(0xBEEF, 0, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, INVALID_SET_FILE_POINTER);
}

TEST_F(FilePosTest, SeekBeforeBeginningFails) {
    std::vector<u8> data(10, 0);
    fs_->seed_file("neg.txt", data);

    Handle h = open_read("C:\\neg.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Seeking to a negative absolute position should fail
    u32 pos = fm_->set_file_pointer(h, -100, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, INVALID_SET_FILE_POINTER);
}

TEST_F(FilePosTest, SeekBeforeBeginningFromCurrentFails) {
    std::vector<u8> data(10, 0);
    fs_->seed_file("negcur.txt", data);

    Handle h = open_read("C:\\negcur.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // At position 0, seeking backwards should fail
    u32 pos = fm_->set_file_pointer(h, -5, nullptr, FILE_CURRENT);
    EXPECT_EQ(pos, INVALID_SET_FILE_POINTER);
}

// =============================================================================
// make_filepos_exports Registration
// =============================================================================

TEST_F(FilePosTest, MakeFileposExportsRegistersAllFour) {
    DllExports exports = make_filepos_exports(*fm_);
    ASSERT_EQ(exports.entries.size(), 4u);

    EXPECT_EQ(exports.entries[0].name, "GetFileSize");
    EXPECT_EQ(exports.entries[1].name, "GetFileSizeEx");
    EXPECT_EQ(exports.entries[2].name, "SetFilePointer");
    EXPECT_EQ(exports.entries[3].name, "SetFilePointerEx");
}

TEST_F(FilePosTest, MakeFileposExportsHandlersAreCallable) {
    DllExports exports = make_filepos_exports(*fm_);

    // Verify each handler is non-null (callable)
    for (const auto& entry : exports.entries) {
        EXPECT_TRUE(static_cast<bool>(entry.handler))
            << "Handler for " << entry.name << " is null";
    }
}
