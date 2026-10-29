/// @file test_file_stream.cpp
/// @brief Unit tests for FileStream (VFS-003 File Read/Write/Seek Operations)

#include <gtest/gtest.h>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "vitreo/common.hpp"
#include "capsule/vfs/file_stream.hpp"
#include "capsule/vfs/filesystem.hpp"

using namespace vitreo;
using namespace vitreo::capsule;

// =============================================================================
// Mock Filesystem
// =============================================================================

/// In-memory mock IFileSystem used to test FileStream without touching disk.
class MockFS : public IFileSystem {
public:
    struct FileState {
        std::vector<u8> data;
        u64 position = 0;
        AccessMode access = AccessMode::None;
        bool closed = false;
    };

    std::unordered_map<FileHandle, FileState> files;
    FileHandle next_handle = 1;
    u32 close_count = 0;  // track how many times close() is called

    // -- helpers for tests ----------------------------------------------------

    /// Pre-populate a file with some content and return its handle.
    FileHandle add_file(std::vector<u8> content, AccessMode access) {
        auto h = next_handle++;
        FileState state;
        state.data = std::move(content);
        state.access = access;
        files[h] = std::move(state);
        return h;
    }

    // -- IFileSystem ----------------------------------------------------------

    Result<FileHandle, VfsError> open(
        std::string_view /*path*/,
        AccessMode access,
        ShareMode /*share*/,
        CreationDisposition /*disp*/) override
    {
        auto h = next_handle++;
        FileState state;
        state.access = access;
        files[h] = std::move(state);
        return Result<FileHandle, VfsError>::ok(h);
    }

    Result<void, VfsError> close(FileHandle h) override {
        auto it = files.find(h);
        if (it == files.end()) {
            return Result<void, VfsError>::err(VfsError::InvalidHandle);
        }
        if (it->second.closed) {
            return Result<void, VfsError>::err(VfsError::HandleClosed);
        }
        it->second.closed = true;
        ++close_count;
        return Result<void, VfsError>::ok();
    }

    Result<usize, VfsError> read(FileHandle h, std::span<u8> buf) override {
        auto it = files.find(h);
        if (it == files.end()) {
            return Result<usize, VfsError>::err(VfsError::InvalidHandle);
        }
        auto& f = it->second;
        if (f.closed) {
            return Result<usize, VfsError>::err(VfsError::HandleClosed);
        }
        usize avail = (f.position < f.data.size())
                          ? f.data.size() - static_cast<usize>(f.position)
                          : 0;
        usize to_read = std::min(avail, buf.size());
        if (to_read > 0) {
            std::memcpy(buf.data(), f.data.data() + f.position, to_read);
        }
        f.position += to_read;
        return Result<usize, VfsError>::ok(to_read);
    }

    Result<usize, VfsError> write(FileHandle h, std::span<const u8> data) override {
        auto it = files.find(h);
        if (it == files.end()) {
            return Result<usize, VfsError>::err(VfsError::InvalidHandle);
        }
        auto& f = it->second;
        if (f.closed) {
            return Result<usize, VfsError>::err(VfsError::HandleClosed);
        }
        if (f.position + data.size() > f.data.size()) {
            f.data.resize(static_cast<usize>(f.position) + data.size());
        }
        std::memcpy(f.data.data() + f.position, data.data(), data.size());
        f.position += data.size();
        return Result<usize, VfsError>::ok(data.size());
    }

    Result<u64, VfsError> seek(FileHandle h, i64 offset, SeekOrigin origin) override {
        auto it = files.find(h);
        if (it == files.end()) {
            return Result<u64, VfsError>::err(VfsError::InvalidHandle);
        }
        auto& f = it->second;
        if (f.closed) {
            return Result<u64, VfsError>::err(VfsError::HandleClosed);
        }
        i64 new_pos = 0;
        switch (origin) {
            case SeekOrigin::Begin:
                new_pos = offset;
                break;
            case SeekOrigin::Current:
                new_pos = static_cast<i64>(f.position) + offset;
                break;
            case SeekOrigin::End:
                new_pos = static_cast<i64>(f.data.size()) + offset;
                break;
        }
        if (new_pos < 0) {
            return Result<u64, VfsError>::err(VfsError::SeekError);
        }
        f.position = static_cast<u64>(new_pos);
        return Result<u64, VfsError>::ok(f.position);
    }

    // -- stubs for unused methods ---------------------------------------------

    Result<FileInfo, VfsError> stat(std::string_view) override {
        return Result<FileInfo, VfsError>::err(VfsError::NotSupported);
    }

    Result<std::vector<DirectoryEntry>, VfsError> list_dir(std::string_view) override {
        return Result<std::vector<DirectoryEntry>, VfsError>::err(VfsError::NotSupported);
    }

    Result<void, VfsError> create_dir(std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    Result<void, VfsError> remove(std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    Result<void, VfsError> rename(std::string_view, std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
};

// =============================================================================
// Test Fixture
// =============================================================================

class FileStreamTest : public ::testing::Test {
protected:
    MockFS mock;
};

// =============================================================================
// Basic Construction & Queries
// =============================================================================

TEST_F(FileStreamTest, ConstructAndIsOpen) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    EXPECT_TRUE(stream.is_open());
    EXPECT_EQ(stream.handle(), h);
}

TEST_F(FileStreamTest, IsReadableWritable_ReadWriteMode) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    EXPECT_TRUE(stream.is_readable());
    EXPECT_TRUE(stream.is_writable());
}

TEST_F(FileStreamTest, IsReadableOnly) {
    auto h = mock.add_file({}, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    EXPECT_TRUE(stream.is_readable());
    EXPECT_FALSE(stream.is_writable());
}

TEST_F(FileStreamTest, IsWritableOnly) {
    auto h = mock.add_file({}, AccessMode::Write);
    FileStream stream(mock, h, AccessMode::Write);

    EXPECT_FALSE(stream.is_readable());
    EXPECT_TRUE(stream.is_writable());
}

// =============================================================================
// Write & Read Round-Trip
// =============================================================================

TEST_F(FileStreamTest, WriteAndReadBack) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    const u8 payload[] = {'H', 'e', 'l', 'l', 'o'};
    auto wr = stream.write(std::span<const u8>{payload, 5});
    ASSERT_TRUE(wr.is_ok());
    EXPECT_EQ(wr.value(), 5u);

    // Seek back to the beginning
    auto sk = stream.seek(0, SeekOrigin::Begin);
    ASSERT_TRUE(sk.is_ok());
    EXPECT_EQ(sk.value(), 0u);

    u8 buf[8] = {};
    auto rd = stream.read(std::span<u8>{buf, 8});
    ASSERT_TRUE(rd.is_ok());
    EXPECT_EQ(rd.value(), 5u);
    EXPECT_EQ(std::memcmp(buf, payload, 5), 0);
}

// =============================================================================
// Seek Origins
// =============================================================================

TEST_F(FileStreamTest, SeekBegin) {
    std::vector<u8> data(100, 0xAA);
    auto h = mock.add_file(data, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    auto r = stream.seek(42, SeekOrigin::Begin);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42u);
}

TEST_F(FileStreamTest, SeekCurrent) {
    std::vector<u8> data(100, 0xBB);
    auto h = mock.add_file(data, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    stream.seek(10, SeekOrigin::Begin);
    auto r = stream.seek(5, SeekOrigin::Current);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 15u);
}

TEST_F(FileStreamTest, SeekEnd) {
    std::vector<u8> data(100, 0xCC);
    auto h = mock.add_file(data, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    auto r = stream.seek(-10, SeekOrigin::End);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 90u);
}

TEST_F(FileStreamTest, SeekNegativeReturnsError) {
    std::vector<u8> data(10, 0);
    auto h = mock.add_file(data, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    auto r = stream.seek(-1, SeekOrigin::Begin);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::SeekError);
}

// =============================================================================
// Tell & Size
// =============================================================================

TEST_F(FileStreamTest, TellReturnsCurrentPosition) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    auto t0 = stream.tell();
    ASSERT_TRUE(t0.is_ok());
    EXPECT_EQ(t0.value(), 0u);

    const u8 buf[] = {1, 2, 3};
    stream.write(std::span<const u8>{buf, 3});

    auto t1 = stream.tell();
    ASSERT_TRUE(t1.is_ok());
    EXPECT_EQ(t1.value(), 3u);
}

TEST_F(FileStreamTest, SizeReturnsFileSize) {
    std::vector<u8> data(77, 0xFF);
    auto h = mock.add_file(data, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    // Move position away from 0 first to verify size() restores position.
    stream.seek(10, SeekOrigin::Begin);

    auto sz = stream.size();
    ASSERT_TRUE(sz.is_ok());
    EXPECT_EQ(sz.value(), 77u);

    // Position should be restored to 10.
    auto pos = stream.tell();
    ASSERT_TRUE(pos.is_ok());
    EXPECT_EQ(pos.value(), 10u);
}

// =============================================================================
// EOF / Extend
// =============================================================================

TEST_F(FileStreamTest, ReadAtEofReturnsZeroBytes) {
    std::vector<u8> data{1, 2, 3};
    auto h = mock.add_file(data, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    stream.seek(0, SeekOrigin::End);
    u8 buf[8] = {};
    auto rd = stream.read(std::span<u8>{buf, 8});
    ASSERT_TRUE(rd.is_ok());
    EXPECT_EQ(rd.value(), 0u);
}

TEST_F(FileStreamTest, WriteExtendsFile) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    const u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    stream.write(std::span<const u8>{payload, 4});

    EXPECT_EQ(mock.files[h].data.size(), 4u);
    EXPECT_EQ(mock.files[h].data[0], 0xDE);
    EXPECT_EQ(mock.files[h].data[3], 0xEF);
}

// =============================================================================
// Access Control
// =============================================================================

TEST_F(FileStreamTest, ReadOnlyRejectsWrite) {
    auto h = mock.add_file({}, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);

    const u8 buf[] = {1};
    auto wr = stream.write(std::span<const u8>{buf, 1});
    ASSERT_TRUE(wr.is_err());
    EXPECT_EQ(wr.error(), VfsError::AccessDenied);
}

TEST_F(FileStreamTest, WriteOnlyRejectsRead) {
    std::vector<u8> data{1, 2, 3};
    auto h = mock.add_file(data, AccessMode::Write);
    FileStream stream(mock, h, AccessMode::Write);

    u8 buf[4] = {};
    auto rd = stream.read(std::span<u8>{buf, 4});
    ASSERT_TRUE(rd.is_err());
    EXPECT_EQ(rd.error(), VfsError::AccessDenied);
}

// =============================================================================
// Close Behaviour
// =============================================================================

TEST_F(FileStreamTest, CloseMarksNotOpen) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    EXPECT_TRUE(stream.is_open());
    auto r = stream.close();
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(stream.is_open());
}

TEST_F(FileStreamTest, ClosePreventsFurtherRead) {
    auto h = mock.add_file({1, 2, 3}, AccessMode::Read);
    FileStream stream(mock, h, AccessMode::Read);
    stream.close();

    u8 buf[4] = {};
    auto rd = stream.read(std::span<u8>{buf, 4});
    ASSERT_TRUE(rd.is_err());
    EXPECT_EQ(rd.error(), VfsError::HandleClosed);
}

TEST_F(FileStreamTest, ClosePreventsFurtherWrite) {
    auto h = mock.add_file({}, AccessMode::Write);
    FileStream stream(mock, h, AccessMode::Write);
    stream.close();

    const u8 buf[] = {1};
    auto wr = stream.write(std::span<const u8>{buf, 1});
    ASSERT_TRUE(wr.is_err());
    EXPECT_EQ(wr.error(), VfsError::HandleClosed);
}

TEST_F(FileStreamTest, DoubleCloseIsSafe) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    auto r1 = stream.close();
    ASSERT_TRUE(r1.is_ok());

    // Second close should succeed (no-op).
    auto r2 = stream.close();
    ASSERT_TRUE(r2.is_ok());

    // close() should only have been forwarded to the FS once.
    EXPECT_EQ(mock.close_count, 1u);
}

// =============================================================================
// Truncate & Flush
// =============================================================================

TEST_F(FileStreamTest, TruncateReturnsNotSupported) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    auto r = stream.truncate(0);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::NotSupported);
}

TEST_F(FileStreamTest, FlushReturnsOk) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);

    auto r = stream.flush();
    ASSERT_TRUE(r.is_ok());
}

// =============================================================================
// Move Semantics
// =============================================================================

TEST_F(FileStreamTest, MoveConstructorTransfersOwnership) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream original(mock, h, AccessMode::ReadWrite);

    FileStream moved(std::move(original));

    EXPECT_TRUE(moved.is_open());
    EXPECT_EQ(moved.handle(), h);

    // Original should be invalidated.
    EXPECT_FALSE(original.is_open());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(original.handle(), INVALID_FILE_HANDLE);
}

TEST_F(FileStreamTest, MoveAssignmentTransfersOwnership) {
    auto h1 = mock.add_file({}, AccessMode::ReadWrite);
    auto h2 = mock.add_file({}, AccessMode::Read);

    FileStream a(mock, h1, AccessMode::ReadWrite);
    FileStream b(mock, h2, AccessMode::Read);

    // Move-assign b <- a.  b's old handle (h2) should be closed first.
    b = std::move(a);

    EXPECT_TRUE(b.is_open());
    EXPECT_EQ(b.handle(), h1);
    EXPECT_TRUE(mock.files[h2].closed);  // old handle was closed
}

TEST_F(FileStreamTest, DestructorAutoCloses) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    {
        FileStream stream(mock, h, AccessMode::ReadWrite);
        EXPECT_FALSE(mock.files[h].closed);
    }
    // After stream goes out of scope the handle should be closed.
    EXPECT_TRUE(mock.files[h].closed);
    EXPECT_EQ(mock.close_count, 1u);
}

// =============================================================================
// Seek After Close
// =============================================================================

TEST_F(FileStreamTest, SeekAfterCloseReturnsError) {
    auto h = mock.add_file({}, AccessMode::ReadWrite);
    FileStream stream(mock, h, AccessMode::ReadWrite);
    stream.close();

    auto r = stream.seek(0, SeekOrigin::Begin);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::HandleClosed);
}
