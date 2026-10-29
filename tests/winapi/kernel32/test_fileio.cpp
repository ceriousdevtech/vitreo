/// @file test_fileio.cpp
/// @brief Unit tests for FileManager ReadFile/WriteFile/SetFilePointer/GetFileSize (WIN32-011)

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <capsule/vfs/filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/handle.hpp>
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
// In-Memory Filesystem (IFileSystem implementation for testing)
// =============================================================================

/// Fully in-memory IFileSystem that stores file content in vectors.
/// Supports open / close / read / write / seek / stat.
/// Directory operations return VfsError::NotSupported.
class TestFileSystem final : public IFileSystem {
public:
    // -- helpers ---------------------------------------------------------------

    /// Seed a file with known content before tests run.
    void seed_file(const std::string& path, std::vector<u8> content) {
        stored_files_[path] = std::move(content);
    }

    /// Inspect stored data (for assertions outside the VFS layer).
    const std::vector<u8>* peek(const std::string& path) const {
        auto it = stored_files_.find(path);
        return it != stored_files_.end() ? &it->second : nullptr;
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

        FileHandle fh     = next_handle_++;
        auto& mf          = open_files_[fh];
        mf.data            = stored_files_[key]; // snapshot
        mf.position        = 0;
        mf.access          = access;
        path_for_handle_[fh] = key;
        return Result<FileHandle, VfsError>::ok(fh);
    }

    [[nodiscard]] Result<void, VfsError> close(FileHandle handle) override {
        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<void, VfsError>::err(VfsError::InvalidHandle);

        // Flush data back to persistent storage
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

        // Keep persistent storage in sync
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

    [[nodiscard]] Result<void, VfsError> truncate(
            FileHandle handle, u64 new_size) override {
        auto it = open_files_.find(handle);
        if (it == open_files_.end())
            return Result<void, VfsError>::err(VfsError::InvalidHandle);

        it->second.data.resize(static_cast<usize>(new_size), 0);
        auto pit = path_for_handle_.find(handle);
        if (pit != path_for_handle_.end())
            stored_files_[pit->second] = it->second.data;
        return Result<void, VfsError>::ok();
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

    // Unimplemented directory operations — return NotSupported
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

class FileIOTest : public ::testing::Test {
protected:
    // Guest-memory layout (all within a single 64 KiB mapped region)
    static constexpr GuestAddr kBaseAddr   = 0x10000;
    static constexpr usize     kRegionSize = 0x10000; // 64 KiB
    static constexpr GuestAddr kBufAddr    = kBaseAddr;            // data buffer
    static constexpr GuestAddr kBuf2Addr   = kBaseAddr + 0x1000;   // second buffer
    static constexpr GuestAddr kCountAddr  = kBaseAddr + 0x2000;   // u32 count

    GuestMemory                  memory_;
    HandleTable                  handles_;
    Vfs                          vfs_;
    TestFileSystem*              test_fs_ = nullptr; // non-owning; Vfs owns it
    std::unique_ptr<FileManager> fm_;

    void SetUp() override {
        auto mr = memory_.map_region(kBaseAddr, kRegionSize,
                                     MemoryProtection::ReadWrite);
        ASSERT_TRUE(mr.is_ok()) << "Failed to map guest memory region";

        auto fs  = std::make_unique<TestFileSystem>();
        test_fs_ = fs.get();
        auto mv  = vfs_.mount_drive('C', std::move(fs));
        ASSERT_TRUE(mv.is_ok()) << "Failed to mount C: drive";

        fm_ = std::make_unique<FileManager>(vfs_, handles_, memory_);
    }

    // ----- helpers -----------------------------------------------------------

    /// Write raw bytes into guest memory at `addr`.
    void guest_write(GuestAddr addr, const void* src, usize len) {
        auto r = memory_.write_bytes(addr,
                                     static_cast<const Byte*>(src), len);
        ASSERT_TRUE(r.is_ok());
    }

    /// Read raw bytes from guest memory at `addr`.
    void guest_read(GuestAddr addr, void* dst, usize len) {
        auto r = memory_.read_bytes(addr,
                                    static_cast<Byte*>(dst), len);
        ASSERT_TRUE(r.is_ok());
    }

    /// Read the u32 stored at `addr` in guest memory.
    u32 guest_read_u32(GuestAddr addr) {
        auto r = memory_.read_u32(addr);
        EXPECT_TRUE(r.is_ok());
        return r.value();
    }

    /// Zero a region of guest memory.
    void guest_zero(GuestAddr addr, usize len) {
        std::vector<u8> z(len, 0);
        guest_write(addr, z.data(), len);
    }

    /// Open a seeded file for reading via C:\ path.
    Handle open_read(const char* win_path) {
        return fm_->create_file(win_path, GENERIC_READ,
                                FILE_SHARE_READ, OPEN_EXISTING, 0);
    }

    /// Open / create a file for read+write.
    Handle open_rw(const char* win_path) {
        return fm_->create_file(win_path,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                CREATE_ALWAYS, 0);
    }
};

// =============================================================================
// ReadFile Tests
// =============================================================================

TEST_F(FileIOTest, ReadFileKnownContent) {
    const std::vector<u8> content = {'H', 'e', 'l', 'l', 'o'};
    test_fs_->seed_file("test.txt", content);

    Handle h = open_read("C:\\test.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    guest_zero(kBufAddr, 16);
    bool ok = fm_->read_file(h, kBufAddr, 5, kCountAddr);
    EXPECT_TRUE(ok);

    u8 buf[5]{};
    guest_read(kBufAddr, buf, 5);
    EXPECT_EQ(buf[0], 'H');
    EXPECT_EQ(buf[1], 'e');
    EXPECT_EQ(buf[2], 'l');
    EXPECT_EQ(buf[3], 'l');
    EXPECT_EQ(buf[4], 'o');
}

TEST_F(FileIOTest, ReadFileWritesCorrectBytesReadCount) {
    test_fs_->seed_file("count.txt", {'A', 'B', 'C', 'D'});

    Handle h = open_read("C:\\count.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    guest_zero(kCountAddr, 4);
    bool ok = fm_->read_file(h, kBufAddr, 4, kCountAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 4u);
}

TEST_F(FileIOTest, ReadFileThunkReportsSynchronousOverlappedCompletion) {
    test_fs_->seed_file("async.txt", {'A', 'B', 'C', 'D'});
    const Handle h = open_read("C:\\async.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr overlapped = kBaseAddr + 0x3000;
    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);

    Handle completed_handle = 0;
    u32 completed_bytes = 0;
    GuestAddr completed_overlapped = 0;
    fm_->set_overlapped_completion_callback(
        [&](Handle file, u32 bytes, GuestAddr ov, bool is_64bit,
            u32 nt_status, u32 win_error, bool completed_synchronously) {
            completed_handle = file;
            completed_bytes = bytes;
            completed_overlapped = ov;
            EXPECT_FALSE(is_64bit);
            EXPECT_EQ(nt_status, 0u);
            EXPECT_EQ(win_error, 0u);
            EXPECT_TRUE(completed_synchronously);
        });

    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 4).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, overlapped).is_ok());
    impl_ReadFile(marshaler, *fm_);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(completed_handle, h);
    EXPECT_EQ(completed_bytes, 4u);
    EXPECT_EQ(completed_overlapped, overlapped);
}

TEST_F(FileIOTest, ReadFileThunkHonorsOverlappedOffset) {
    test_fs_->seed_file("offset.txt", {'A', 'B', 'C', 'D', 'E'});
    const Handle h = open_read("C:\\offset.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr overlapped = kBaseAddr + 0x3000;
    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(overlapped + 8, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(overlapped + 12, 0).is_ok());

    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, overlapped).is_ok());
    impl_ReadFile(marshaler, *fm_);

    u8 data[2]{};
    guest_read(kBufAddr, data, sizeof(data));
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(data[0], 'C');
    EXPECT_EQ(data[1], 'D');
    // Learn (ReadFile, sync + OVERLAPPED): the file pointer advances to
    // Offset + bytes transferred (files_core_probe sync_ReadFile_overlapped
    // pointer_after = 5 for Offset=2, n=3).
    EXPECT_EQ(fm_->set_file_pointer(h, 0, nullptr, FILE_CURRENT), 4u);
}

// async_writefile_probe (x86 and x64 identical): WriteFile with an OVERLAPPED
// on a FILE_FLAG_OVERLAPPED disk handle returns FALSE + ERROR_IO_PENDING even
// when the write already completed, writes 0 to *lpNumberOfBytesWritten and
// leaves the file pointer; a synchronous handle returns TRUE with the count
// and moves the pointer to Offset + count.
TEST_F(FileIOTest, WriteFileOverlappedHandleReportsPendingAndKeepsPointer) {
    for (const bool async_handle : {true, false}) {
        SCOPED_TRACE(async_handle ? "FILE_FLAG_OVERLAPPED" : "synchronous");
        constexpr u32 kFileFlagOverlapped = 0x40000000u;
        const Handle h = fm_->create_file(
            async_handle ? "C:\\async_write.bin" : "C:\\sync_write.bin",
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
            async_handle ? kFileFlagOverlapped : 0);
        ASSERT_NE(h, INVALID_HANDLE);
        ASSERT_EQ(fm_->is_overlapped_file(h), async_handle);

        constexpr GuestAddr overlapped = kBaseAddr + 0x3000;
        constexpr GuestAddr stack = kBaseAddr + 0x4000;
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.rsp = stack;
        Marshaler marshaler(memory_);
        marshaler.attach_cpu(cpu);
        guest_zero(overlapped, 20);
        ASSERT_TRUE(memory_.write_u32(overlapped + 8, 3).is_ok());  // Offset
        ASSERT_TRUE(memory_.write_bytes(kBufAddr, reinterpret_cast<const Byte*>("WXYZ"), 4).is_ok());
        ASSERT_TRUE(memory_.write_u32(kCountAddr, 0x77777777u).is_ok());

        bool completion_seen = false;
        fm_->set_overlapped_completion_callback(
            [&](Handle, u32 bytes, GuestAddr ov, bool, u32 nt_status, u32,
                bool completed_synchronously) {
                completion_seen = true;
                EXPECT_EQ(bytes, 4u);
                EXPECT_EQ(ov, overlapped);
                EXPECT_EQ(nt_status, 0u);
                EXPECT_EQ(completed_synchronously, !async_handle);
            });
        set_last_error(0xdead);
        ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
        ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
        ASSERT_TRUE(memory_.write_u32(stack + 12, 4).is_ok());
        ASSERT_TRUE(memory_.write_u32(stack + 16, kCountAddr).is_ok());
        ASSERT_TRUE(memory_.write_u32(stack + 20, overlapped).is_ok());
        impl_WriteFile(marshaler, *fm_);

        EXPECT_TRUE(completion_seen);
        if (async_handle) {
            EXPECT_EQ(cpu.gpr.rax, 0u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);
            EXPECT_EQ(guest_read_u32(kCountAddr), 0u);
            EXPECT_EQ(fm_->set_file_pointer(h, 0, nullptr, FILE_CURRENT), 0u);
        } else {
            EXPECT_EQ(cpu.gpr.rax, 1u);
            EXPECT_EQ(get_last_error(), 0xdeadu);
            EXPECT_EQ(guest_read_u32(kCountAddr), 4u);
            EXPECT_EQ(fm_->set_file_pointer(h, 0, nullptr, FILE_CURRENT), 7u);
        }
        // The bytes landed at Offset either way.
        ASSERT_EQ(fm_->set_file_pointer(h, 3, nullptr, FILE_BEGIN), 3u);
        guest_zero(kBufAddr, 4);
        ASSERT_TRUE(fm_->read_file(h, kBufAddr, 4, 0));
        u8 data[4]{};
        guest_read(kBufAddr, data, sizeof(data));
        EXPECT_EQ(std::string(reinterpret_cast<char*>(data), 4), "WXYZ");
        fm_->close_file(h);
    }
}

TEST_F(FileIOTest, ReadFileExRejectsNullOverlapped) {
    test_fs_->seed_file("ex.txt", {'A'});
    const Handle h = open_read("C:\\ex.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, 0x2000).is_ok());
    set_last_error(error_codes::ERROR_SUCCESS);
    impl_ReadFileEx(marshaler, *fm_);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(FileIOTest, ReadFileExFillsBufferAndQueuesCompletion) {
    test_fs_->seed_file("exoff.txt", {'A', 'B', 'C', 'D', 'E'});
    const Handle h = open_read("C:\\exoff.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr overlapped = kBaseAddr + 0x3000;
    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    constexpr GuestAddr completion = 0x7000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(overlapped + 8, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(overlapped + 12, 0).is_ok());

    GuestAddr queued_pfn = 0;
    u32 queued_error = 0xFFFFFFFFu;
    u32 queued_bytes = 0;
    GuestAddr queued_ov = 0;
    fm_->set_io_completion_routine_queue(
        [&](GuestAddr pfn, u32 error, u32 bytes, GuestAddr ov) {
            queued_pfn = pfn;
            queued_error = error;
            queued_bytes = bytes;
            queued_ov = ov;
        });

    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, overlapped).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, completion).is_ok());
    impl_ReadFileEx(marshaler, *fm_);

    u8 data[2]{};
    guest_read(kBufAddr, data, sizeof(data));
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(data[0], 'C');
    EXPECT_EQ(data[1], 'D');
    EXPECT_EQ(memory_.read_u32(overlapped).value_or(1), 0u);
    EXPECT_EQ(memory_.read_u32(overlapped + 4).value_or(0), 2u);
    EXPECT_EQ(queued_pfn, completion);
    EXPECT_EQ(queued_error, 0u);
    EXPECT_EQ(queued_bytes, 2u);
    EXPECT_EQ(queued_ov, overlapped);
}

TEST_F(FileIOTest, ReadFileExAtEofQueuesHandleEof) {
    test_fs_->seed_file("exeof.txt", {'A', 'B'});
    const Handle h = open_read("C:\\exeof.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr overlapped = kBaseAddr + 0x3000;
    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(overlapped + 8, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(overlapped + 12, 0).is_ok());

    u32 queued_error = 0;
    u32 queued_bytes = 0xFFFFFFFFu;
    fm_->set_io_completion_routine_queue(
        [&](GuestAddr, u32 error, u32 bytes, GuestAddr) {
            queued_error = error;
            queued_bytes = bytes;
        });

    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 4).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, overlapped).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, 0x7000).is_ok());
    impl_ReadFileEx(marshaler, *fm_);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(queued_error, error_codes::ERROR_HANDLE_EOF);
    EXPECT_EQ(queued_bytes, 0u);
    EXPECT_EQ(memory_.read_u32(overlapped).value_or(0), 0xC0000011u);
}

TEST_F(FileIOTest, WriteFileExRejectsNullOverlapped) {
    const Handle h = fm_->create_file("C:\\wex.txt", GENERIC_WRITE,
                                      FILE_SHARE_READ, CREATE_ALWAYS, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, 0x2000).is_ok());
    set_last_error(error_codes::ERROR_SUCCESS);
    impl_WriteFileEx(marshaler, *fm_);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(FileIOTest, WriteFileExWritesAtOffsetAndQueuesCompletion) {
    test_fs_->seed_file("wexoff.txt", {'A', 'B', 'C', 'D', 'E'});
    const Handle h = fm_->create_file(
        "C:\\wexoff.txt", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    constexpr GuestAddr overlapped = kBaseAddr + 0x3000;
    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    constexpr GuestAddr completion = 0x7000;
    const u8 payload[] = {'X', 'Y'};
    guest_write(kBufAddr, payload, sizeof(payload));

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(overlapped + 8, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(overlapped + 12, 0).is_ok());

    GuestAddr queued_pfn = 0;
    u32 queued_error = 0xFFFFFFFFu;
    u32 queued_bytes = 0;
    GuestAddr queued_ov = 0;
    fm_->set_io_completion_routine_queue(
        [&](GuestAddr pfn, u32 error, u32 bytes, GuestAddr ov) {
            queued_pfn = pfn;
            queued_error = error;
            queued_bytes = bytes;
            queued_ov = ov;
        });

    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 2).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 16, overlapped).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 20, completion).is_ok());
    impl_WriteFileEx(marshaler, *fm_);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory_.read_u32(overlapped).value_or(1), 0u);
    EXPECT_EQ(memory_.read_u32(overlapped + 4).value_or(0), 2u);
    EXPECT_EQ(queued_pfn, completion);
    EXPECT_EQ(queued_error, 0u);
    EXPECT_EQ(queued_bytes, 2u);
    EXPECT_EQ(queued_ov, overlapped);

    fm_->set_file_pointer(h, 0, nullptr, FILE_BEGIN);
    guest_zero(kBuf2Addr, 5);
    EXPECT_TRUE(fm_->read_file(h, kBuf2Addr, 5, kCountAddr));
    u8 data[5]{};
    guest_read(kBuf2Addr, data, sizeof(data));
    EXPECT_EQ(data[0], 'A');
    EXPECT_EQ(data[1], 'B');
    EXPECT_EQ(data[2], 'X');
    EXPECT_EQ(data[3], 'Y');
    EXPECT_EQ(data[4], 'E');
}

TEST_F(FileIOTest, ReadFilePartialBufferSmallerThanFile) {
    std::vector<u8> ten(10);
    for (u8 i = 0; i < 10; ++i) ten[i] = static_cast<u8>(i + 1);
    test_fs_->seed_file("big.txt", ten);

    Handle h = open_read("C:\\big.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    guest_zero(kBufAddr, 16);
    bool ok = fm_->read_file(h, kBufAddr, 5, kCountAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 5u);

    u8 buf[5]{};
    guest_read(kBufAddr, buf, 5);
    for (u8 i = 0; i < 5; ++i)
        EXPECT_EQ(buf[i], static_cast<u8>(i + 1));
}

TEST_F(FileIOTest, ReadFileFromEmptyFileReturnsZeroBytes) {
    test_fs_->seed_file("empty.txt", {});

    Handle h = open_read("C:\\empty.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    guest_zero(kCountAddr, 4);
    bool ok = fm_->read_file(h, kBufAddr, 100, kCountAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 0u);
}

TEST_F(FileIOTest, ReadFileInvalidHandleReturnsFalse) {
    bool ok = fm_->read_file(0xDEAD, kBufAddr, 10, kCountAddr);
    EXPECT_FALSE(ok);
}

TEST_F(FileIOTest, ReadFileLargerRequestThanContent) {
    test_fs_->seed_file("short.txt", {'X', 'Y'});

    Handle h = open_read("C:\\short.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    guest_zero(kBufAddr, 1024);
    bool ok = fm_->read_file(h, kBufAddr, 1024, kCountAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 2u);

    u8 buf[2]{};
    guest_read(kBufAddr, buf, 2);
    EXPECT_EQ(buf[0], 'X');
    EXPECT_EQ(buf[1], 'Y');
}

// =============================================================================
// WriteFile Tests
// =============================================================================

TEST_F(FileIOTest, WriteFileThenReadFileRoundtrip) {
    Handle h = open_rw("C:\\rw.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    const u8 data[] = {'W', 'o', 'r', 'l', 'd'};
    guest_write(kBufAddr, data, 5);
    bool wok = fm_->write_file(h, kBufAddr, 5, kCountAddr);
    EXPECT_TRUE(wok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 5u);

    // Seek back to beginning
    u32 pos = fm_->set_file_pointer(h, 0, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, 0u);

    // Read back into a different guest buffer
    guest_zero(kBuf2Addr, 16);
    bool rok = fm_->read_file(h, kBuf2Addr, 5, kCountAddr);
    EXPECT_TRUE(rok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 5u);

    u8 out[5]{};
    guest_read(kBuf2Addr, out, 5);
    EXPECT_EQ(std::memcmp(out, data, 5), 0);
}

TEST_F(FileIOTest, WriteFileUpdatesBytesWritten) {
    Handle h = open_rw("C:\\wr.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    std::vector<u8> data(10, 0xAB);
    guest_write(kBufAddr, data.data(), 10);

    guest_zero(kCountAddr, 4);
    bool ok = fm_->write_file(h, kBufAddr, 10, kCountAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 10u);
}

TEST_F(FileIOTest, WriteFileAppendsData) {
    Handle h = open_rw("C:\\append.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // First write: "Hello"
    const u8 hello[] = {'H', 'e', 'l', 'l', 'o'};
    guest_write(kBufAddr, hello, 5);
    EXPECT_TRUE(fm_->write_file(h, kBufAddr, 5, kCountAddr));

    // Second write: " World"
    const u8 world[] = {' ', 'W', 'o', 'r', 'l', 'd'};
    guest_write(kBufAddr, world, 6);
    EXPECT_TRUE(fm_->write_file(h, kBufAddr, 6, kCountAddr));

    // Seek to beginning and read all 11 bytes
    fm_->set_file_pointer(h, 0, nullptr, FILE_BEGIN);
    guest_zero(kBuf2Addr, 16);
    EXPECT_TRUE(fm_->read_file(h, kBuf2Addr, 11, kCountAddr));
    EXPECT_EQ(guest_read_u32(kCountAddr), 11u);

    u8 out[11]{};
    guest_read(kBuf2Addr, out, 11);
    EXPECT_EQ(std::memcmp(out, "Hello World", 11), 0);
}

TEST_F(FileIOTest, WriteFileToNewFile) {
    Handle h = fm_->create_file("C:\\brand_new.txt",
                                GENERIC_WRITE, FILE_SHARE_READ,
                                CREATE_ALWAYS, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    const u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    guest_write(kBufAddr, payload, 4);
    bool ok = fm_->write_file(h, kBufAddr, 4, kCountAddr);
    EXPECT_TRUE(ok);
    EXPECT_EQ(guest_read_u32(kCountAddr), 4u);
}

TEST_F(FileIOTest, WriteFileInvalidHandleReturnsFalse) {
    bool ok = fm_->write_file(0xDEAD, kBufAddr, 10, kCountAddr);
    EXPECT_FALSE(ok);
}

// =============================================================================
// SetFilePointer Tests
// =============================================================================

TEST_F(FileIOTest, SetFilePointerFileBegin) {
    std::vector<u8> data = {10, 20, 30, 40, 50};
    test_fs_->seed_file("seek.txt", data);

    Handle h = fm_->create_file("C:\\seek.txt", GENERIC_READ,
                                FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    u32 pos = fm_->set_file_pointer(h, 3, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, 3u);

    // Read 1 byte — should be data[3] == 40
    guest_zero(kBufAddr, 4);
    EXPECT_TRUE(fm_->read_file(h, kBufAddr, 1, kCountAddr));
    u8 val = 0;
    guest_read(kBufAddr, &val, 1);
    EXPECT_EQ(val, 40u);
}

TEST_F(FileIOTest, SetFilePointerFileCurrent) {
    std::vector<u8> data = {10, 20, 30, 40, 50};
    test_fs_->seed_file("seekc.txt", data);

    Handle h = fm_->create_file("C:\\seekc.txt", GENERIC_READ,
                                FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    // Read 2 bytes to advance position to 2
    fm_->read_file(h, kBufAddr, 2, kCountAddr);

    // Move 1 forward from current → position 3
    u32 pos = fm_->set_file_pointer(h, 1, nullptr, FILE_CURRENT);
    EXPECT_EQ(pos, 3u);

    // Read 1 byte — should be data[3] == 40
    u8 val = 0;
    EXPECT_TRUE(fm_->read_file(h, kBufAddr, 1, kCountAddr));
    guest_read(kBufAddr, &val, 1);
    EXPECT_EQ(val, 40u);
}

TEST_F(FileIOTest, SetFilePointerFileEnd) {
    std::vector<u8> data = {10, 20, 30, 40, 50};
    test_fs_->seed_file("seeke.txt", data);

    Handle h = fm_->create_file("C:\\seeke.txt", GENERIC_READ,
                                FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    // Seek to 2 bytes before end → position 3
    u32 pos = fm_->set_file_pointer(h, -2, nullptr, FILE_END);
    EXPECT_EQ(pos, 3u);

    // Read 1 byte — should be data[3] == 40
    u8 val = 0;
    EXPECT_TRUE(fm_->read_file(h, kBufAddr, 1, kCountAddr));
    guest_read(kBufAddr, &val, 1);
    EXPECT_EQ(val, 40u);
}

TEST_F(FileIOTest, SetFilePointerReturnsNewPosition) {
    test_fs_->seed_file("pos.txt", std::vector<u8>(200, 0));

    Handle h = open_read("C:\\pos.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(fm_->set_file_pointer(h, 0,  nullptr, FILE_BEGIN),   0u);
    EXPECT_EQ(fm_->set_file_pointer(h, 50, nullptr, FILE_BEGIN),  50u);
    EXPECT_EQ(fm_->set_file_pointer(h, 10, nullptr, FILE_CURRENT), 60u);
    EXPECT_EQ(fm_->set_file_pointer(h, -5, nullptr, FILE_END),   195u);
}

TEST_F(FileIOTest, SetFilePointerInvalidHandleReturnsFailure) {
    u32 pos = fm_->set_file_pointer(0xBEEF, 0, nullptr, FILE_BEGIN);
    EXPECT_EQ(pos, 0xFFFFFFFFu);
}

TEST_F(FileIOTest, SetEndOfFileTruncatesAtCurrentPosition) {
    test_fs_->seed_file("truncate.txt", std::vector<u8>(10, 0xAB));
    Handle h = open_rw("C:\\truncate.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    ASSERT_EQ(fm_->set_file_pointer(h, 4, nullptr, FILE_BEGIN), 4u);
    EXPECT_TRUE(fm_->set_end_of_file(h));
    EXPECT_EQ(fm_->get_file_size(h, nullptr), 4u);
}

TEST_F(FileIOTest, SetEndOfFileExtendsAtCurrentPosition) {
    test_fs_->seed_file("extend.txt", std::vector<u8>(4, 0xCD));
    Handle h = open_rw("C:\\extend.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    ASSERT_EQ(fm_->set_file_pointer(h, 10, nullptr, FILE_BEGIN), 10u);
    EXPECT_TRUE(fm_->set_end_of_file(h));
    EXPECT_EQ(fm_->get_file_size(h, nullptr), 10u);
}

TEST_F(FileIOTest, SetEndOfFileRejectsInvalidHandle) {
    EXPECT_FALSE(fm_->set_end_of_file(0xBEEF));
}

TEST_F(FileIOTest, SetFileValidDataRequiresVolumePrivilege) {
    Handle h = open_rw("C:\\valid-data.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    auto exports = make_file_exports(*fm_);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                           [](const ExportEntry& entry) {
                               return entry.name == "SetFileValidData";
                           });
    ASSERT_NE(it, exports.entries.end());

    constexpr GuestAddr stack = kBaseAddr + 0x4000;
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory_.write_u32(stack + 4, h).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 8, 4096).is_ok());
    ASSERT_TRUE(memory_.write_u32(stack + 12, 0).is_ok());

    set_last_error(error_codes::ERROR_SUCCESS);
    it->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 1314u);
}

// =============================================================================
// GetFileSize Tests
// =============================================================================

TEST_F(FileIOTest, GetFileSizeReturnsCorrectSize) {
    std::vector<u8> data(100, 0xFF);
    test_fs_->seed_file("sized.txt", data);

    Handle h = open_read("C:\\sized.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    u32 size = fm_->get_file_size(h, nullptr);
    EXPECT_EQ(size, 100u);
}

TEST_F(FileIOTest, GetFileSizeAfterWrite) {
    Handle h = open_rw("C:\\growing.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(fm_->get_file_size(h, nullptr), 0u);

    std::vector<u8> data(42, 0xCC);
    guest_write(kBufAddr, data.data(), 42);
    fm_->write_file(h, kBufAddr, 42, kCountAddr);

    EXPECT_EQ(fm_->get_file_size(h, nullptr), 42u);
}

TEST_F(FileIOTest, GetFileSizeInvalidHandleReturnsFailure) {
    u32 size = fm_->get_file_size(0xBEEF, nullptr);
    EXPECT_EQ(size, 0xFFFFFFFFu);
}

// =============================================================================
// Multi-Write / Roundtrip
// =============================================================================

TEST_F(FileIOTest, MultipleWriteThenReadRoundtrip) {
    Handle h = open_rw("C:\\multi.txt");
    ASSERT_NE(h, INVALID_HANDLE);

    // Write three chunks sequentially
    const u8 c1[] = {'A', 'B', 'C'};
    const u8 c2[] = {'D', 'E'};
    const u8 c3[] = {'F', 'G', 'H', 'I'};

    guest_write(kBufAddr, c1, 3);
    EXPECT_TRUE(fm_->write_file(h, kBufAddr, 3, kCountAddr));

    guest_write(kBufAddr, c2, 2);
    EXPECT_TRUE(fm_->write_file(h, kBufAddr, 2, kCountAddr));

    guest_write(kBufAddr, c3, 4);
    EXPECT_TRUE(fm_->write_file(h, kBufAddr, 4, kCountAddr));

    // Total size should be 9
    EXPECT_EQ(fm_->get_file_size(h, nullptr), 9u);

    // Seek to beginning and read all
    fm_->set_file_pointer(h, 0, nullptr, FILE_BEGIN);
    guest_zero(kBuf2Addr, 16);
    EXPECT_TRUE(fm_->read_file(h, kBuf2Addr, 9, kCountAddr));
    EXPECT_EQ(guest_read_u32(kCountAddr), 9u);

    u8 out[9]{};
    guest_read(kBuf2Addr, out, 9);
    EXPECT_EQ(std::memcmp(out, "ABCDEFGHI", 9), 0);
}

TEST_F(FileIOTest, GetFileSizePreservesFilePosition) {
    std::vector<u8> data(50, 0xAA);
    test_fs_->seed_file("preserve.txt", data);

    Handle h = fm_->create_file("C:\\preserve.txt", GENERIC_READ,
                                FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    // Seek to position 20
    fm_->set_file_pointer(h, 20, nullptr, FILE_BEGIN);

    // GetFileSize should not disturb position
    EXPECT_EQ(fm_->get_file_size(h, nullptr), 50u);

    // Position should still be 20 — reading should yield data[20]
    u8 val = 0;
    EXPECT_TRUE(fm_->read_file(h, kBufAddr, 1, kCountAddr));
    guest_read(kBufAddr, &val, 1);
    EXPECT_EQ(val, 0xAA);

    // And bytes_read == 1 confirms we read from the correct offset
    EXPECT_EQ(guest_read_u32(kCountAddr), 1u);

    // Current position should now be 21 (20 + 1 byte read)
    u32 pos = fm_->set_file_pointer(h, 0, nullptr, FILE_CURRENT);
    EXPECT_EQ(pos, 21u);
}
