#pragma once

/// @file filesystem.hpp
/// @brief IFileSystem interface and core VFS types for Vitreo
///
/// Defines the abstract filesystem interface and supporting types that all
/// filesystem implementations must adhere to.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vitreo/common.hpp"

namespace vitreo::capsule {

// =============================================================================
// VFS Error Codes
// =============================================================================

/// Error codes specific to VFS operations
enum class VfsError {
    None = 0,
    
    // Path errors
    InvalidPath,
    /// An intermediate directory in the path does not exist.
    /// Maps to ERROR_PATH_NOT_FOUND (3).
    PathNotFound,
    /// The parent directory exists but the final component does not.
    /// Maps to ERROR_FILE_NOT_FOUND (2). Windows distinguishes these two, and
    /// applications branch on it: a missing file is often benign ("nothing to
    /// back up"), while a missing directory is treated as fatal.
    FileNotFound,
    PathAlreadyExists,
    NotADirectory,
    NotAFile,
    DirectoryNotEmpty,
    
    // Access errors
    AccessDenied,
    SharingViolation,
    InvalidHandle,
    HandleClosed,
    
    // I/O errors
    IoError,
    ReadError,
    WriteError,
    SeekError,
    EndOfFile,
    
    // Resource errors
    TooManyOpenFiles,
    DiskFull,
    OutOfMemory,
    
    // Drive errors
    DriveNotMounted,
    DriveAlreadyMounted,
    InvalidDriveLetter,
    
    // General errors
    InvalidArgument,
    NotSameDevice,
    NotSupported,
    InternalError,
    /// A byte range is locked by another handle (ERROR_LOCK_VIOLATION, 33).
    LockViolation,
    /// UnlockFile on a range that is not locked (ERROR_NOT_LOCKED, 158).
    NotLocked,
};

/// Convert VfsError to string for debugging
constexpr const char* vfs_error_to_string(VfsError e) noexcept {
    switch (e) {
        case VfsError::None:              return "None";
        case VfsError::InvalidPath:       return "InvalidPath";
        case VfsError::PathNotFound:      return "PathNotFound";
        case VfsError::FileNotFound:      return "FileNotFound";
        case VfsError::PathAlreadyExists: return "PathAlreadyExists";
        case VfsError::NotADirectory:     return "NotADirectory";
        case VfsError::NotAFile:          return "NotAFile";
        case VfsError::DirectoryNotEmpty: return "DirectoryNotEmpty";
        case VfsError::AccessDenied:      return "AccessDenied";
        case VfsError::SharingViolation:  return "SharingViolation";
        case VfsError::InvalidHandle:     return "InvalidHandle";
        case VfsError::HandleClosed:      return "HandleClosed";
        case VfsError::IoError:           return "IoError";
        case VfsError::ReadError:         return "ReadError";
        case VfsError::WriteError:        return "WriteError";
        case VfsError::SeekError:         return "SeekError";
        case VfsError::EndOfFile:         return "EndOfFile";
        case VfsError::TooManyOpenFiles:  return "TooManyOpenFiles";
        case VfsError::DiskFull:          return "DiskFull";
        case VfsError::OutOfMemory:       return "OutOfMemory";
        case VfsError::DriveNotMounted:   return "DriveNotMounted";
        case VfsError::LockViolation:     return "LockViolation";
        case VfsError::NotLocked:         return "NotLocked";
        case VfsError::DriveAlreadyMounted: return "DriveAlreadyMounted";
        case VfsError::InvalidDriveLetter: return "InvalidDriveLetter";
        case VfsError::InvalidArgument:   return "InvalidArgument";
        case VfsError::NotSameDevice:     return "NotSameDevice";
        case VfsError::NotSupported:      return "NotSupported";
        case VfsError::InternalError:     return "InternalError";
    }
    return "Unknown";
}

// =============================================================================
// File Handle
// =============================================================================

/// Opaque file handle type
using FileHandle = u32;

/// Invalid file handle sentinel
constexpr FileHandle INVALID_FILE_HANDLE = 0;

// =============================================================================
// Access and Share Modes
// =============================================================================

/// File access mode (what operations are allowed)
enum class AccessMode : u32 {
    None        = 0,
    Read        = 1 << 0,
    Write       = 1 << 1,
    ReadWrite   = Read | Write,
    Execute     = 1 << 2,
    Delete      = 1 << 3,
    /// FILE_WRITE_ATTRIBUTES, FILE_WRITE_EA, WRITE_DAC, WRITE_OWNER: changing
    /// a file's metadata or security without writing its data. A writable
    /// file grants it with any open; a protected one refuses it (a system
    /// DLL: ERROR_ACCESS_DENIED, builtin_dll_file_probe).
    WriteMetadata = 1 << 4,
    All         = Read | Write | Execute | Delete | WriteMetadata,
};

/// Bitwise OR for AccessMode
inline constexpr AccessMode operator|(AccessMode a, AccessMode b) noexcept {
    return static_cast<AccessMode>(static_cast<u32>(a) | static_cast<u32>(b));
}

/// Bitwise AND for AccessMode
inline constexpr AccessMode operator&(AccessMode a, AccessMode b) noexcept {
    return static_cast<AccessMode>(static_cast<u32>(a) & static_cast<u32>(b));
}

/// Check if access mode has flag
inline constexpr bool has_access(AccessMode mode, AccessMode flag) noexcept {
    return (static_cast<u32>(mode) & static_cast<u32>(flag)) != 0;
}

/// Share mode (what operations other processes can perform)
enum class ShareMode : u32 {
    None        = 0,            // Exclusive access
    Read        = 1 << 0,       // Allow other reads
    Write       = 1 << 1,       // Allow other writes
    Delete      = 1 << 2,       // Allow other deletes
    ReadWrite   = Read | Write,
    All         = Read | Write | Delete,
};

/// Bitwise OR for ShareMode
inline constexpr ShareMode operator|(ShareMode a, ShareMode b) noexcept {
    return static_cast<ShareMode>(static_cast<u32>(a) | static_cast<u32>(b));
}

/// Bitwise AND for ShareMode
inline constexpr ShareMode operator&(ShareMode a, ShareMode b) noexcept {
    return static_cast<ShareMode>(static_cast<u32>(a) & static_cast<u32>(b));
}

// =============================================================================
// Creation Disposition
// =============================================================================

/// How to handle file creation/opening
enum class CreationDisposition {
    CreateNew,        // Fail if exists
    CreateAlways,     // Overwrite if exists
    OpenExisting,     // Fail if not exists
    OpenAlways,       // Create if not exists
    TruncateExisting, // Open and truncate
};

// =============================================================================
// Seek Origin
// =============================================================================

/// Seek origin for file positioning
enum class SeekOrigin {
    Begin,    // From start of file
    Current,  // From current position
    End,      // From end of file
};

// =============================================================================
// File Attributes
// =============================================================================

/// File attributes (Windows-compatible)
enum class FileAttributes : u32 {
    None        = 0,
    ReadOnly    = 1 << 0,
    Hidden      = 1 << 1,
    System      = 1 << 2,
    Directory   = 1 << 4,
    Archive     = 1 << 5,
    Normal      = 1 << 7,
    Temporary   = 1 << 8,
    ReparsePoint = 1 << 10, // FILE_ATTRIBUTE_REPARSE_POINT (symlink / junction)
    Offline     = 1 << 12,
    NotContentIndexed = 1 << 13,
};

/// Bitwise OR for FileAttributes
inline constexpr FileAttributes operator|(FileAttributes a, FileAttributes b) noexcept {
    return static_cast<FileAttributes>(static_cast<u32>(a) | static_cast<u32>(b));
}

/// Bitwise AND for FileAttributes
inline constexpr FileAttributes operator&(FileAttributes a, FileAttributes b) noexcept {
    return static_cast<FileAttributes>(static_cast<u32>(a) & static_cast<u32>(b));
}

/// Check if attributes has flag
inline constexpr bool has_attribute(FileAttributes attrs, FileAttributes flag) noexcept {
    return (static_cast<u32>(attrs) & static_cast<u32>(flag)) != 0;
}

// =============================================================================
// File Information
// =============================================================================

/// File timestamp (Windows FILETIME compatible - 100ns intervals since 1601)
using FileTime = u64;

/// Get current time as FileTime
FileTime current_file_time() noexcept;

/// Information about a file or directory
struct FileInfo {
    u64 size = 0;                               // File size in bytes
    FileAttributes attributes = FileAttributes::None;
    FileTime creation_time = 0;                 // When created
    FileTime last_access_time = 0;              // Last read
    FileTime last_write_time = 0;               // Last modified
    u64 volume_id = 0;                          // Stable filesystem/volume identity
    u64 file_id = 0;                            // Stable identity within the volume
    u32 link_count = 1;                         // Number of hard links
    FileTime change_time = 0;                   // Last metadata change
};

/// Directory entry (for listing)
struct DirectoryEntry {
    std::string name;           // Filename without path
    FileInfo info;              // File information
};

/// What a volume reports about itself. The geometry is GetDiskFreeSpace's
/// view: zero sectors_per_cluster or bytes_per_sector means the volume does
/// not know its geometry, and callers fall back to the host's statistics.
/// free_clusters / total_clusters are DWORD counts, clamped to 0xFFFFFFFF;
/// the byte counts are exact when total_bytes is nonzero.
struct VolumeInfo {
    std::string label;
    std::string filesystem_name;
    u32 max_component_length = 255;
    u32 filesystem_flags = 0;
    u32 sectors_per_cluster = 0;
    u32 bytes_per_sector = 0;
    /// Clusters available to the caller (GetDiskFreeSpace's free count).
    u32 free_clusters = 0;
    u32 total_clusters = 0;
    u64 total_bytes = 0;
    /// All free bytes on the volume, and the part available to the caller.
    u64 free_bytes = 0;
    u64 available_bytes = 0;
};

// =============================================================================
// IFileSystem Interface
// =============================================================================

/// Abstract filesystem interface
///
/// All filesystem implementations (host, overlay, memory, etc.) implement this
/// interface. The Vfs class delegates to the appropriate implementation based
/// on the drive letter.
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /// Whether this filesystem represents read-only optical media.
    [[nodiscard]] virtual bool is_optical() const noexcept { return false; }

    /// Return filesystem-specific volume metadata when available.
    [[nodiscard]] virtual bool volume_info(VolumeInfo&) const noexcept { return false; }

    /// Raw volume read at a logical byte offset — the CreateFile("\\\\.\\X:")
    /// + ReadFile / OVERLAPPED contract used by CD checks (e.g. ISO9660 PVD
    /// at offset 32768). Default: NotSupported.
    [[nodiscard]] virtual Result<usize, VfsError> read_volume(
        u64 /*offset*/, std::span<u8> /*buffer*/) {
        return Result<usize, VfsError>::err(VfsError::NotSupported);
    }
    
    // -------------------------------------------------------------------------
    // File Operations
    // -------------------------------------------------------------------------
    
    /// Open or create a file
    /// @param path Relative path within this filesystem (no drive letter)
    /// @param access Read/write access mode
    /// @param share How the file can be shared
    /// @param disposition How to handle file creation
    /// @return File handle or error
    [[nodiscard]] virtual Result<FileHandle, VfsError> open(
        std::string_view path,
        AccessMode access,
        ShareMode share,
        CreationDisposition disposition
    ) = 0;
    
    /// Close an open file handle
    /// @param handle File handle from open()
    /// @return Success or error
    [[nodiscard]] virtual Result<void, VfsError> close(FileHandle handle) = 0;

    /// Dup the host FD behind an open handle (shares offset). Used for
    /// DuplicateHandle / SCM_RIGHTS transfer. Default: NotSupported.
    [[nodiscard]] virtual Result<int, VfsError> duplicate_native_fd(
        FileHandle /*handle*/) {
        return Result<int, VfsError>::err(VfsError::NotSupported);
    }

    /// Adopt a host FD into a new open handle. Takes ownership of @p fd only
    /// on success; on failure the caller retains ownership. Default: NotSupported.
    [[nodiscard]] virtual Result<FileHandle, VfsError> adopt_native_fd(
        int /*fd*/,
        AccessMode /*access*/,
        ShareMode /*share*/,
        std::string_view /*path*/) {
        return Result<FileHandle, VfsError>::err(VfsError::NotSupported);
    }
    
    /// Read data from a file
    /// @param handle File handle
    /// @param buffer Buffer to read into
    /// @return Number of bytes read, or error
    [[nodiscard]] virtual Result<usize, VfsError> read(
        FileHandle handle,
        std::span<u8> buffer
    ) = 0;
    
    /// Write data to a file
    /// @param handle File handle
    /// @param data Data to write
    /// @return Number of bytes written, or error
    [[nodiscard]] virtual Result<usize, VfsError> write(
        FileHandle handle,
        std::span<const u8> data
    ) = 0;
    
    /// Seek to a position in a file
    /// @param handle File handle
    /// @param offset Offset from origin
    /// @param origin Where to seek from
    /// @return New position, or error
    [[nodiscard]] virtual Result<u64, VfsError> seek(
        FileHandle handle,
        i64 offset,
        SeekOrigin origin
    ) = 0;
    
    /// Truncate or extend a file to a given size
    /// @param handle File handle
    /// @param new_size Desired file size in bytes
    /// @return Success or error
    [[nodiscard]] virtual Result<void, VfsError> truncate(
        FileHandle handle,
        u64 new_size
    ) {
        (void)handle;
        (void)new_size;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
    
    /// Flush pending writes to disk
    /// @param handle File handle
    /// @return Success or error
    [[nodiscard]] virtual Result<void, VfsError> flush(FileHandle handle) {
        (void)handle;
        return Result<void, VfsError>::ok();
    }

    /// Set last-access and last-write times on an open file. Times are
    /// expressed in seconds since the Unix epoch; pass any negative value
    /// to leave that timestamp unchanged.
    [[nodiscard]] virtual Result<void, VfsError> set_file_times(
        FileHandle handle,
        i64 atime_unix_seconds,
        i64 mtime_unix_seconds
    ) {
        (void)handle;
        (void)atime_unix_seconds;
        (void)mtime_unix_seconds;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    // -------------------------------------------------------------------------
    // Metadata Operations
    // -------------------------------------------------------------------------
    
    /// Get file/directory information
    /// @param path Path to query (no drive letter)
    /// @return FileInfo or error
    [[nodiscard]] virtual Result<FileInfo, VfsError> stat(std::string_view path) = 0;

    /// Like stat, but does not follow a final symbolic link (lstat). Default
    /// follows (stat). HostFileSystem overrides so FindFirstFile / DeleteFile
    /// see REPARSE_POINT links and delete the link itself.
    [[nodiscard]] virtual Result<FileInfo, VfsError> lstat(std::string_view path) {
        return stat(path);
    }

    /// Get file information for an open file handle (fstat-equivalent).
    /// Default implementation returns NotSupported. Implementations should
    /// override to provide accurate metadata for handle-based queries such as
    /// GetFileInformationByHandle{,Ex}.
    [[nodiscard]] virtual Result<FileInfo, VfsError> stat_handle(FileHandle handle) {
        (void)handle;
        return Result<FileInfo, VfsError>::err(VfsError::NotSupported);
    }

    /// SetFileAttributes: store the Win32 attribute word (FileAttributes
    /// values). Backends without attribute storage refuse.
    [[nodiscard]] virtual Result<void, VfsError> set_attributes(
        std::string_view path, u32 attributes) {
        (void)path; (void)attributes;
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    [[nodiscard]] virtual Result<void, VfsError> set_handle_attributes(
        FileHandle handle, u32 attributes) {
        (void)handle; (void)attributes;
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }

    /// SetFileTime with FILETIME precision. Each pointer may be null (leave
    /// unchanged). A value of all ones for access/write suspends updates of
    /// that time through this handle (Learn: SetFileTime).
    [[nodiscard]] virtual Result<void, VfsError> set_file_times_ft(
        FileHandle handle, const u64* creation, const u64* access,
        const u64* write) {
        (void)handle; (void)creation; (void)access; (void)write;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    /// LockFile/LockFileEx/UnlockFile(Ex) on the file object @p handle.
    [[nodiscard]] virtual Result<void, VfsError> lock_range(
        FileHandle handle, u64 offset, u64 length, bool exclusive) {
        (void)handle; (void)offset; (void)length; (void)exclusive;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
    [[nodiscard]] virtual Result<void, VfsError> unlock_range(
        FileHandle handle, u64 offset, u64 length) {
        (void)handle; (void)offset; (void)length;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
    
    // -------------------------------------------------------------------------
    // Directory Operations
    // -------------------------------------------------------------------------
    
    /// List contents of a directory
    /// @param path Directory path (no drive letter)
    /// @return Vector of directory entries, or error
    [[nodiscard]] virtual Result<std::vector<DirectoryEntry>, VfsError> list_dir(
        std::string_view path
    ) = 0;

    /// List the directory represented by an open handle. The default
    /// implementation reports NotSupported for backends that cannot retain
    /// path identity for directory handles.
    [[nodiscard]] virtual Result<std::vector<DirectoryEntry>, VfsError>
    list_dir_handle(FileHandle handle) {
        (void)handle;
        return Result<std::vector<DirectoryEntry>, VfsError>::err(
            VfsError::NotSupported);
    }
    
    /// Create a directory
    /// @param path Directory path to create
    /// @return Success or error
    [[nodiscard]] virtual Result<void, VfsError> create_dir(std::string_view path) = 0;
    
    // -------------------------------------------------------------------------
    // Modification Operations
    // -------------------------------------------------------------------------
    
    /// Remove a file or empty directory
    /// @param path Path to remove
    /// @return Success or error
    [[nodiscard]] virtual Result<void, VfsError> remove(std::string_view path) = 0;
    
    /// Rename/move a file or directory
    /// @param old_path Current path
    /// @param new_path New path
    /// @return Success or error
    [[nodiscard]] virtual Result<void, VfsError> rename(
        std::string_view old_path,
        std::string_view new_path
    ) = 0;

    /// Rename the file behind an open handle (SetFileInformationByHandle
    /// FileRenameInfo). The handle's own share mode must not block the
    /// rename; other opens still do. Default: not supported.
    [[nodiscard]] virtual Result<void, VfsError> rename_open(
        FileHandle handle,
        std::string_view new_relative_path,
        bool replace_existing
    ) {
        (void)handle;
        (void)new_relative_path;
        (void)replace_existing;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    /// Add a second directory entry for an existing file on this filesystem.
    /// Backends that cannot represent hard links fail honestly.
    [[nodiscard]] virtual Result<void, VfsError> create_hard_link(
        std::string_view new_path,
        std::string_view existing_path
    ) {
        (void)new_path;
        (void)existing_path;
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }

    /// GetFinalPathNameByHandle backing: the path of the object `handle` is
    /// open on, relative to this filesystem's root. It is spelled with the
    /// names the filesystem stores (on-disk case, not the opener's), has no
    /// "." / ".." segments, follows renames made after the open, uses '/'
    /// separators, and is "" for the root itself. PathNotFound when the object
    /// no longer lies under this root (a link that resolved elsewhere);
    /// NotSupported when the backend cannot name an open object.
    [[nodiscard]] virtual Result<std::string, VfsError> final_path(
        FileHandle handle
    ) {
        (void)handle;
        return Result<std::string, VfsError>::err(VfsError::NotSupported);
    }

    /// Resolve a relative VFS path to a host path when supported.
    /// Filesystems that are not host-backed return NotSupported.
    [[nodiscard]] virtual Result<std::string, VfsError> resolve_host_path(
        std::string_view path
    ) const {
        (void)path;
        return Result<std::string, VfsError>::err(VfsError::NotSupported);
    }
};

} // namespace vitreo::capsule
