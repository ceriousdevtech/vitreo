#pragma once

/// @file directory.hpp
/// @brief Directory handle and higher-level directory operations for VFS
///
/// Provides DirHandle for iterating directory entries and DirectoryOps
/// for recursive directory creation/removal and existence checks.

#include <optional>
#include <string_view>
#include <vector>

#include "vitreo/common.hpp"
#include "capsule/vfs/filesystem.hpp"

namespace vitreo::capsule {

// =============================================================================
// DirHandle — iterate over a directory listing
// =============================================================================

/// Handle returned by open_directory() that owns a snapshot of directory
/// entries and supports forward iteration with rewind.
///
/// Move-only — copying would duplicate the entire entry vector.
class DirHandle {
public:
    /// Construct from a pre-fetched listing.
    explicit DirHandle(std::vector<DirectoryEntry> entries) noexcept
        : m_entries(std::move(entries))
        , m_index(0)
        , m_open(true) {}

    // Move-only
    DirHandle(DirHandle&& other) noexcept
        : m_entries(std::move(other.m_entries))
        , m_index(other.m_index)
        , m_open(other.m_open) {
        other.m_open = false;
    }

    DirHandle& operator=(DirHandle&& other) noexcept {
        if (this != &other) {
            m_entries = std::move(other.m_entries);
            m_index   = other.m_index;
            m_open    = other.m_open;
            other.m_open = false;
        }
        return *this;
    }

    DirHandle(const DirHandle&) = delete;
    DirHandle& operator=(const DirHandle&) = delete;

    ~DirHandle() = default;

    // -------------------------------------------------------------------------
    // Iteration
    // -------------------------------------------------------------------------

    /// Return the next entry, or nullopt when exhausted.
    /// Returns HandleClosed error if the handle has been closed.
    [[nodiscard]] Result<std::optional<DirectoryEntry>, VfsError> next() noexcept;

    /// Reset the cursor to the beginning.
    [[nodiscard]] Result<void, VfsError> rewind() noexcept;

    /// Mark the handle as closed; further next() calls will fail.
    void close() noexcept;

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /// True if the handle has not been closed.
    [[nodiscard]] bool is_open() const noexcept { return m_open; }

    /// Total number of entries in the listing.
    [[nodiscard]] usize count() const noexcept { return m_entries.size(); }

    /// Entries remaining from the current cursor position.
    [[nodiscard]] usize remaining() const noexcept {
        if (m_index >= m_entries.size()) return 0;
        return m_entries.size() - m_index;
    }

private:
    std::vector<DirectoryEntry> m_entries;
    usize m_index;
    bool  m_open;
};

// =============================================================================
// DirectoryOps — higher-level helpers that operate on an IFileSystem
// =============================================================================

/// Utility class providing recursive create/remove, existence checks,
/// and a convenience wrapper that returns a DirHandle.
class DirectoryOps {
public:
    /// Construct with a reference to any IFileSystem implementation.
    explicit DirectoryOps(IFileSystem& fs) noexcept : m_fs(fs) {}

    // -------------------------------------------------------------------------
    // Operations
    // -------------------------------------------------------------------------

    /// Create a directory.  When @p recursive is true every missing parent
    /// component is created (mkdir -p semantics).
    [[nodiscard]] Result<void, VfsError> create_directory(
        std::string_view path, bool recursive = false);

    /// Remove a directory.  When @p recursive is true the contents are
    /// removed depth-first before the directory itself (rm -rf semantics).
    [[nodiscard]] Result<void, VfsError> remove_directory(
        std::string_view path, bool recursive = false);

    /// Return true if @p path exists and has the Directory attribute.
    [[nodiscard]] Result<bool, VfsError> is_directory(std::string_view path) const;

    /// List the directory and wrap the results in a DirHandle.
    [[nodiscard]] Result<DirHandle, VfsError> open_directory(std::string_view path);

    /// Return true if @p path exists (file or directory).
    [[nodiscard]] Result<bool, VfsError> exists(std::string_view path) const;

private:
    IFileSystem& m_fs;
};

} // namespace vitreo::capsule
