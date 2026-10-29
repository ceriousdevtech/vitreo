#pragma once

/// @file host_filesystem.hpp
/// @brief Host filesystem implementation using POSIX APIs
///
/// HostFileSystem maps VFS operations to the host macOS filesystem via POSIX.
/// It is the primary implementation for accessing files stored on the host.

#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

#include "filesystem.hpp"

namespace vitreo::capsule {

// =============================================================================
// Open File State
// =============================================================================

/// State for an open file
struct OpenFileState {
    mutable std::mutex mutex;      // Per-handle lock for fd operations
    int fd = -1;                    // POSIX file descriptor
    std::string path;               // Relative path within filesystem
    AccessMode access = AccessMode::None;
    ShareMode share = ShareMode::None;
    bool is_directory = false;
    /// Current file position, in bytes from the start of the file.
    ///
    /// Windows keeps the file pointer in the file object, not in the kernel's
    /// descriptor, and SetFilePointer only updates that field — it performs no
    /// I/O. Tracking it here lets seek() stay a pure in-memory update and lets
    /// read()/write() apply it with pread()/pwrite(), which matters because
    /// guests seek far more often than they read (SimCity 4 issues one
    /// SetFilePointer per byte consumed). Guarded by `mutex`.
    u64 position = 0;
    /// Once exported with dup(), the open file description owns the cursor so
    /// aliases in this process and inherited aliases in another process share
    /// it. Unshared files retain the in-memory seek path above.
    bool native_position = false;
    /// Host identity of the open file (share-access / range-lock key).
    u64 dev = 0;
    u64 ino = 0;
    /// Share-access registration (share_access.hpp); 0 when none.
    u64 share_token = 0;
    /// SetFileTime(0xFFFFFFFF/0xFFFFFFFF): I/O through this handle must not
    /// move that timestamp; the value to hold it at.
    bool hold_atime = false;
    bool hold_mtime = false;
    struct timespec held_atime{};
    struct timespec held_mtime{};
};

// =============================================================================
// HostFileSystem
// =============================================================================

/// Filesystem implementation that wraps the host filesystem via POSIX APIs
///
/// Paths are relative to a root directory. For example, if root is
/// "/path/to/capsule/drives/c", then a path "Users/file.txt" maps to
/// "/path/to/capsule/drives/c/Users/file.txt".
class HostFileSystem : public IFileSystem {
public:
    /// Construct with root directory
    /// @param root_path Absolute path to the root directory on host
    explicit HostFileSystem(std::string root_path);
    
    /// Destructor - closes all open files
    ~HostFileSystem() override;
    
    // Non-copyable, non-movable (due to mutex)
    HostFileSystem(const HostFileSystem&) = delete;
    HostFileSystem& operator=(const HostFileSystem&) = delete;
    HostFileSystem(HostFileSystem&&) = delete;
    HostFileSystem& operator=(HostFileSystem&&) = delete;
    
    /// Get the root path
    [[nodiscard]] const std::string& root_path() const noexcept { return root_; }
    
    // -------------------------------------------------------------------------
    // IFileSystem Implementation
    // -------------------------------------------------------------------------
    
    [[nodiscard]] Result<FileHandle, VfsError> open(
        std::string_view path,
        AccessMode access,
        ShareMode share,
        CreationDisposition disposition
    ) override;
    
    [[nodiscard]] Result<void, VfsError> close(FileHandle handle) override;
    
    [[nodiscard]] Result<int, VfsError> duplicate_native_fd(
        FileHandle handle) override;

    [[nodiscard]] Result<FileHandle, VfsError> adopt_native_fd(
        int fd,
        AccessMode access,
        ShareMode share,
        std::string_view path) override;

    [[nodiscard]] Result<usize, VfsError> read(
        FileHandle handle,
        std::span<u8> buffer
    ) override;
    
    [[nodiscard]] Result<usize, VfsError> write(
        FileHandle handle,
        std::span<const u8> data
    ) override;
    
    [[nodiscard]] Result<u64, VfsError> seek(
        FileHandle handle,
        i64 offset,
        SeekOrigin origin
    ) override;
    
    [[nodiscard]] Result<void, VfsError> truncate(
        FileHandle handle,
        u64 new_size
    ) override;
    
    [[nodiscard]] Result<void, VfsError> flush(FileHandle handle) override;

    [[nodiscard]] Result<void, VfsError> set_file_times(
        FileHandle handle,
        i64 atime_unix_seconds,
        i64 mtime_unix_seconds
    ) override;

    [[nodiscard]] Result<FileInfo, VfsError> stat(std::string_view path) override;
    [[nodiscard]] Result<FileInfo, VfsError> lstat(std::string_view path) override;

    [[nodiscard]] Result<FileInfo, VfsError> stat_handle(FileHandle handle) override;
    
    [[nodiscard]] Result<std::vector<DirectoryEntry>, VfsError> list_dir(
        std::string_view path
    ) override;

    [[nodiscard]] Result<std::vector<DirectoryEntry>, VfsError>
    list_dir_handle(FileHandle handle) override;
    
    [[nodiscard]] Result<void, VfsError> create_dir(std::string_view path) override;
    
    [[nodiscard]] Result<void, VfsError> remove(std::string_view path) override;
    
    [[nodiscard]] Result<void, VfsError> rename(
        std::string_view old_path,
        std::string_view new_path
    ) override;

    [[nodiscard]] Result<void, VfsError> rename_open(
        FileHandle handle,
        std::string_view new_relative_path,
        bool replace_existing
    ) override;

    [[nodiscard]] Result<void, VfsError> create_hard_link(
        std::string_view new_path,
        std::string_view existing_path
    ) override;

    [[nodiscard]] Result<std::string, VfsError> resolve_host_path(
        std::string_view path
    ) const override;

    /// The open descriptor's current host path (F_GETPATH), made relative to
    /// the canonical root.
    [[nodiscard]] Result<std::string, VfsError> final_path(
        FileHandle handle
    ) override;

    /// Capsule host volumes report as NTFS with the Win32 case/unicode
    /// flags GetVolumeInformationW probes for (files_ns_probe). Label is
    /// empty so a 1-WCHAR buffer succeeds (label_one). The geometry and
    /// space are the host volume holding the root (statvfs), in NTFS terms:
    /// 512-byte sectors and a cluster of the host's allocation unit
    /// (disk_free_probe: 8 sectors x 512 on a 4 KiB-cluster volume).
    [[nodiscard]] bool volume_info(VolumeInfo& info) const noexcept override;

private:
    /// Convert relative path to full host path
    [[nodiscard]] std::string to_host_path(std::string_view relative) const;

    /// Re-spell `host_path` so every component that already exists on the host
    /// uses that component's real casing. Windows resolves paths
    /// case-insensitively, but host volumes need not: macOS boot volumes
    /// usually are case-insensitive while mounted ISO9660/UDF media are not.
    /// Components that do not exist keep the casing the guest asked for, so
    /// creation still uses the requested name.
    [[nodiscard]] std::string resolve_case_insensitively(
        const std::string& host_path) const;
    
    /// Get open file state for a handle
    [[nodiscard]] std::shared_ptr<OpenFileState> get_open_file(FileHandle handle);
    
    /// Allocate a new file handle
    [[nodiscard]] FileHandle allocate_handle();
    
    /// Convert POSIX errno to VfsError
    [[nodiscard]] static VfsError errno_to_vfs_error(int err);

    /// Resolve an errno for an operation on `host_path`, distinguishing a
    /// missing leaf from a missing directory. POSIX collapses both into ENOENT,
    /// but Windows reports ERROR_FILE_NOT_FOUND vs ERROR_PATH_NOT_FOUND and
    /// applications branch on the difference. Non-ENOENT values are unchanged.
    [[nodiscard]] static VfsError errno_to_vfs_error_for(int err,
                                                         const std::string& host_path);
    
    /// Convert stat result to FileInfo
    /// @p stored is the Win32 attribute word kept in the file's
    /// com.vitreo.dosattrib extended attribute, when present.
    [[nodiscard]] static FileInfo stat_to_file_info(
        const struct stat& st, std::optional<u32> stored = std::nullopt);

public:
    [[nodiscard]] Result<void, VfsError> set_attributes(
        std::string_view path, u32 attributes) override;
    [[nodiscard]] Result<void, VfsError> set_handle_attributes(
        FileHandle handle, u32 attributes) override;
    [[nodiscard]] Result<void, VfsError> set_file_times_ft(
        FileHandle handle, const u64* creation, const u64* access,
        const u64* write) override;
    [[nodiscard]] Result<void, VfsError> lock_range(
        FileHandle handle, u64 offset, u64 length, bool exclusive) override;
    [[nodiscard]] Result<void, VfsError> unlock_range(
        FileHandle handle, u64 offset, u64 length) override;
private:
    
    /// Root directory on host
    std::string root_;
    
    /// Open file table
    std::unordered_map<FileHandle, std::shared_ptr<OpenFileState>> open_files_;
    
    /// Next handle to allocate
    FileHandle next_handle_ = 1;
    
    /// Mutex for thread safety
    mutable std::mutex mutex_;
};

} // namespace vitreo::capsule
