#pragma once

/// @file file_stream.hpp
/// @brief FileStream class for RAII file read/write/seek operations
///
/// Wraps an IFileSystem handle with ownership semantics.  Move-only;
/// the destructor auto-closes the handle when the stream goes out of scope.

#include <span>

#include "vitreo/common.hpp"
#include "capsule/vfs/filesystem.hpp"

namespace vitreo::capsule {

/// RAII stream wrapper around an IFileSystem file handle.
///
/// FileStream is not default-constructible.  It must be created from
/// an existing IFileSystem reference, a valid FileHandle, and the
/// AccessMode that was used to open the handle.
///
/// The stream is move-only.  Copying is deleted.  The destructor will
/// close the underlying handle if the stream is still open.
class FileStream {
public:
    // -------------------------------------------------------------------------
    // Construction / Destruction
    // -------------------------------------------------------------------------

    /// Construct a FileStream that owns @p handle opened on @p fs.
    FileStream(IFileSystem& fs, FileHandle handle, AccessMode access) noexcept;

    /// Not default-constructible.
    FileStream() = delete;

    /// Not copyable.
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    /// Move constructor – transfers ownership.
    FileStream(FileStream&& other) noexcept;

    /// Move assignment – closes current handle first if open.
    FileStream& operator=(FileStream&& other) noexcept;

    /// Destructor – auto-closes the handle when still open.
    ~FileStream();

    // -------------------------------------------------------------------------
    // I/O Operations
    // -------------------------------------------------------------------------

    /// Read up to @p buffer.size() bytes from the current position.
    /// @return Number of bytes actually read, or VfsError.
    [[nodiscard]] Result<usize, VfsError> read(std::span<u8> buffer);

    /// Write @p data.size() bytes at the current position.
    /// @return Number of bytes actually written, or VfsError.
    [[nodiscard]] Result<usize, VfsError> write(std::span<const u8> data);

    /// Seek to an absolute or relative position.
    /// @return New absolute position, or VfsError.
    [[nodiscard]] Result<u64, VfsError> seek(i64 offset, SeekOrigin origin);

    /// Return the current file position (equivalent to seek(0, Current)).
    [[nodiscard]] Result<u64, VfsError> tell();

    /// Return the file size (seeks to end and back).
    [[nodiscard]] Result<u64, VfsError> size();

    /// Truncate the file to @p new_size bytes.
    /// Returns NotSupported — would require access to the underlying fd.
    [[nodiscard]] Result<void, VfsError> truncate(u64 new_size);

    /// Flush pending writes.  Best-effort; returns ok() unconditionally.
    [[nodiscard]] Result<void, VfsError> flush();

    /// Close the handle.  Safe to call multiple times.
    [[nodiscard]] Result<void, VfsError> close();

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /// True while the handle has not been closed.
    [[nodiscard]] bool is_open() const noexcept;

    /// True if the access mode includes Read.
    [[nodiscard]] bool is_readable() const noexcept;

    /// True if the access mode includes Write.
    [[nodiscard]] bool is_writable() const noexcept;

    /// Return the underlying file handle (or INVALID_FILE_HANDLE if closed).
    [[nodiscard]] FileHandle handle() const noexcept;

private:
    IFileSystem* m_fs;
    FileHandle   m_handle;
    AccessMode   m_access;
    bool         m_open;
};

} // namespace vitreo::capsule
