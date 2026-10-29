/// @file file_stream.cpp
/// @brief FileStream implementation

#include "capsule/vfs/file_stream.hpp"

#include <span>
#include <utility>

namespace vitreo::capsule {

// =============================================================================
// Construction / Destruction
// =============================================================================

FileStream::FileStream(IFileSystem& fs, FileHandle handle, AccessMode access) noexcept
    : m_fs{&fs}
    , m_handle{handle}
    , m_access{access}
    , m_open{true}
{}

FileStream::FileStream(FileStream&& other) noexcept
    : m_fs{other.m_fs}
    , m_handle{other.m_handle}
    , m_access{other.m_access}
    , m_open{other.m_open}
{
    other.m_open   = false;
    other.m_handle = INVALID_FILE_HANDLE;
}

FileStream& FileStream::operator=(FileStream&& other) noexcept {
    if (this != &other) {
        if (m_open) {
            (void)close();
        }
        m_fs     = other.m_fs;
        m_handle = other.m_handle;
        m_access = other.m_access;
        m_open   = other.m_open;

        other.m_open   = false;
        other.m_handle = INVALID_FILE_HANDLE;
    }
    return *this;
}

FileStream::~FileStream() {
    if (m_open) {
        (void)close();
    }
}

// =============================================================================
// I/O Operations
// =============================================================================

Result<usize, VfsError> FileStream::read(std::span<u8> buffer) {
    if (!m_open) {
        return Result<usize, VfsError>::err(VfsError::HandleClosed);
    }
    if (!is_readable()) {
        return Result<usize, VfsError>::err(VfsError::AccessDenied);
    }
    return m_fs->read(m_handle, buffer);
}

Result<usize, VfsError> FileStream::write(std::span<const u8> data) {
    if (!m_open) {
        return Result<usize, VfsError>::err(VfsError::HandleClosed);
    }
    if (!is_writable()) {
        return Result<usize, VfsError>::err(VfsError::AccessDenied);
    }
    return m_fs->write(m_handle, data);
}

Result<u64, VfsError> FileStream::seek(i64 offset, SeekOrigin origin) {
    if (!m_open) {
        return Result<u64, VfsError>::err(VfsError::HandleClosed);
    }
    return m_fs->seek(m_handle, offset, origin);
}

Result<u64, VfsError> FileStream::tell() {
    return seek(0, SeekOrigin::Current);
}

Result<u64, VfsError> FileStream::size() {
    // Save current position.
    auto saved = tell();
    if (saved.is_err()) {
        return saved;
    }
    // Seek to end.
    auto end_pos = seek(0, SeekOrigin::End);
    if (end_pos.is_err()) {
        return end_pos;
    }
    // Restore original position.
    auto restore = seek(static_cast<i64>(saved.value()), SeekOrigin::Begin);
    if (restore.is_err()) {
        return restore;
    }
    return end_pos;
}

Result<void, VfsError> FileStream::truncate([[maybe_unused]] u64 new_size) {
    if (!m_open) {
        return Result<void, VfsError>::err(VfsError::HandleClosed);
    }
    // Would require access to the underlying file descriptor which is
    // encapsulated inside the IFileSystem implementation.
    return Result<void, VfsError>::err(VfsError::NotSupported);
}

Result<void, VfsError> FileStream::flush() {
    if (!m_open) {
        return Result<void, VfsError>::err(VfsError::HandleClosed);
    }
    // Best-effort; no fd access available through the interface.
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> FileStream::close() {
    if (!m_open) {
        return Result<void, VfsError>::ok();
    }
    auto result = m_fs->close(m_handle);
    m_open   = false;
    m_handle = INVALID_FILE_HANDLE;
    return result;
}

// =============================================================================
// Queries
// =============================================================================

bool FileStream::is_open() const noexcept {
    return m_open;
}

bool FileStream::is_readable() const noexcept {
    return has_access(m_access, AccessMode::Read);
}

bool FileStream::is_writable() const noexcept {
    return has_access(m_access, AccessMode::Write);
}

FileHandle FileStream::handle() const noexcept {
    return m_handle;
}

} // namespace vitreo::capsule
