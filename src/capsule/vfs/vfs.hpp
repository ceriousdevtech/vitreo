#pragma once

/// @file vfs.hpp
/// @brief Virtual Filesystem with Windows drive letter support
///
/// The Vfs class manages mounted drives (C:, D:, etc.) and delegates file
/// operations to the appropriate IFileSystem implementation based on the
/// drive letter in the path.

#include <functional>
#include <memory>
#include <utility>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "filesystem.hpp"

namespace vitreo::capsule {

// =============================================================================
// Windows Path Utilities
// =============================================================================

/// Parse a Windows-style path and extract components
struct ParsedPath {
    char drive_letter = 0;       // 'C', 'D', etc. (0 if no drive)
    std::string relative_path;   // Path without drive (forward slashes)
    bool is_absolute = false;    // Has drive letter or starts with backslash
};

/// Parse a Windows-style path string
/// Supports: "C:\folder\file", "C:/folder/file", "\folder\file", "folder\file"
[[nodiscard]] ParsedPath parse_windows_path(std::string_view path) noexcept;

/// Normalize path separators (backslash to forward slash)
[[nodiscard]] std::string normalize_path_separators(std::string_view path) noexcept;

/// Check if a character is a valid drive letter (A-Z, a-z)
[[nodiscard]] constexpr bool is_valid_drive_letter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// Normalize drive letter to uppercase
[[nodiscard]] constexpr char normalize_drive_letter(char c) noexcept {
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

// =============================================================================
// Vfs Class
// =============================================================================

/// Virtual Filesystem with Windows drive letter support
///
/// The Vfs manages a collection of mounted drives, each mapped to an
/// IFileSystem implementation. Windows paths like "C:\Users\file.txt"
/// are parsed and dispatched to the appropriate filesystem.
///
/// Example:
/// @code
///     Vfs vfs;
///     vfs.mount_drive('C', std::make_unique<HostFileSystem>("/path/to/c"));
///     
///     auto handle = vfs.open("C:\\Users\\file.txt", AccessMode::Read, ...);
/// @endcode
class Vfs {
public:
    /// Default constructor
    Vfs() = default;
    
    /// Destructor
    ~Vfs() = default;
    
    // Non-copyable / non-movable (mutex + unique_ptr drive map)
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;
    Vfs(Vfs&&) = delete;
    Vfs& operator=(Vfs&&) = delete;
    
    // -------------------------------------------------------------------------
    // Drive Management
    // -------------------------------------------------------------------------
    
    /// Mount a filesystem to a drive letter
    /// @param letter Drive letter (A-Z, case insensitive)
    /// @param fs Filesystem implementation
    /// @return Success or error
    [[nodiscard]] Result<void, VfsError> mount_drive(
        char letter,
        std::unique_ptr<IFileSystem> fs
    );
    
    /// Unmount a drive
    /// @param letter Drive letter to unmount
    /// @return Success or error
    [[nodiscard]] Result<void, VfsError> unmount_drive(char letter);
    
    /// Check if a drive is mounted
    [[nodiscard]] bool is_drive_mounted(char letter) const noexcept;
    
    /// Get list of mounted drive letters
    [[nodiscard]] std::vector<char> mounted_drives() const;
    
    /// Get the default drive letter (used for relative paths)
    [[nodiscard]] char default_drive() const noexcept { return default_drive_; }
    
    /// Set the default drive letter
    void set_default_drive(char letter) noexcept;

    /// WOW64 file-system redirection (Learn, "File System Redirector";
    /// measured by tests/winapi/kernel32/oracle/wow64_fsredir_probe.c). In a
    /// 32-bit process every path under %windir%\System32 names the same path
    /// under %windir%\SysWOW64, except catroot, catroot2, drivers\etc,
    /// driverstore, logfiles and spool; %windir%\Sysnative names the real
    /// System32. A path relative to a current directory inside System32 is
    /// redirected with no exemption (the probe: drivers\etc reached from
    /// there is SysWOW64's). Each thread can switch it off
    /// (Wow64DisableWow64FsRedirection). A 64-bit process has no redirection
    /// and no Sysnative.
    void set_wow64_view(bool wow64) noexcept { wow64_view_ = wow64; }
    [[nodiscard]] bool wow64_view() const noexcept { return wow64_view_; }
    static void set_thread_redirection_disabled(bool disabled) noexcept;
    [[nodiscard]] static bool thread_redirection_disabled() noexcept;
    /// @p full (a fully qualified path) as the WOW64 view names it;
    /// @p relative says it was reached through the current directory.
    [[nodiscard]] std::string wow64_redirect(std::string full, bool relative) const;
    /// @p path (any form) as the file it reaches in this process's view.
    [[nodiscard]] std::string viewed_path(std::string_view path) const;
    [[nodiscard]] static bool is_cwd_relative(std::string_view utf8_path) noexcept;

    /// Publish immutable runtime-generated file bytes at a Windows path.
    /// Mounted filesystem content takes precedence, so a real guest file is
    /// never shadowed by a generated fallback.
    [[nodiscard]] Result<void, VfsError> publish_generated_file(
        std::string_view path,
        std::shared_ptr<const std::vector<u8>> bytes);

    /// Generated files supplied on demand, for paths nothing has published
    /// yet (a built-in system DLL no process code has loaded): @p source
    /// returns the bytes for a Windows path the mounts do not hold, and a
    /// non-null result is published there; @p listing names the generated
    /// files of one directory, with their sizes, for enumeration. Both run
    /// under the VFS lock and must not call back into the Vfs.
    using GeneratedFileSource =
        std::function<std::shared_ptr<const std::vector<u8>>(
            std::string_view path)>;
    using GeneratedListing =
        std::function<std::vector<std::pair<std::string, u64>>(
            std::string_view directory)>;
    void set_generated_file_source(GeneratedFileSource source,
                                   GeneratedListing listing);
    /// True when @p path is served as a generated file: the mount holds no
    /// file there and one is published or the source provides one.
    [[nodiscard]] bool is_generated_file(std::string_view path);
    
    // -------------------------------------------------------------------------
    // File Operations (IFileSystem-like interface)
    // -------------------------------------------------------------------------
    
    /// Open or create a file
    /// @param path Windows path (e.g., "C:\folder\file.txt")
    [[nodiscard]] Result<FileHandle, VfsError> open(
        std::string_view path,
        AccessMode access,
        ShareMode share,
        CreationDisposition disposition
    );
    
    /// Close an open file handle
    [[nodiscard]] Result<void, VfsError> close(FileHandle handle);

    /// Dup the host FD for DuplicateHandle / SCM_RIGHTS (shares file offset).
    [[nodiscard]] Result<int, VfsError> duplicate_native_fd(FileHandle handle);

    /// Adopt a transferred host FD onto the default drive (or @p drive).
    [[nodiscard]] Result<FileHandle, VfsError> adopt_native_fd(
        int fd,
        AccessMode access,
        ShareMode share,
        std::string_view path = {},
        char drive = 0);
    
    /// Read data from a file
    [[nodiscard]] Result<usize, VfsError> read(
        FileHandle handle,
        std::span<u8> buffer
    );
    
    /// Write data to a file
    [[nodiscard]] Result<usize, VfsError> write(
        FileHandle handle,
        std::span<const u8> data
    );
    
    /// Seek to a position in a file
    [[nodiscard]] Result<u64, VfsError> seek(
        FileHandle handle,
        i64 offset,
        SeekOrigin origin
    );
    
    /// Truncate or extend a file to a given size
    [[nodiscard]] Result<void, VfsError> truncate(FileHandle handle, u64 new_size);
    
    /// Flush pending writes to disk
    [[nodiscard]] Result<void, VfsError> flush(FileHandle handle);

    /// Set last-access and last-write times on an open file (Unix seconds).
    /// Pass a negative value for either timestamp to leave it unchanged.
    [[nodiscard]] Result<void, VfsError> set_file_times(
        FileHandle handle,
        i64 atime_unix_seconds,
        i64 mtime_unix_seconds
    );
    
    /// Get file/directory information
    [[nodiscard]] Result<FileInfo, VfsError> stat(std::string_view path);

    /// Like stat without following a final symbolic link.
    [[nodiscard]] Result<FileInfo, VfsError> lstat(std::string_view path);

    /// Get file information for an open handle (fstat-equivalent)
    [[nodiscard]] Result<FileInfo, VfsError> stat_handle(FileHandle handle);

    /// SetFileAttributes (by path / by handle). See IFileSystem.
    [[nodiscard]] Result<void, VfsError> set_attributes(std::string_view path,
                                                        u32 attributes);
    [[nodiscard]] Result<void, VfsError> set_handle_attributes(FileHandle handle,
                                                               u32 attributes);
    /// SetFileTime with FILETIME values (null = unchanged).
    [[nodiscard]] Result<void, VfsError> set_file_times_ft(
        FileHandle handle, const u64* creation, const u64* access,
        const u64* write);
    /// Byte-range locks on the file object behind @p handle.
    [[nodiscard]] Result<void, VfsError> lock_range(FileHandle handle, u64 offset,
                                                    u64 length, bool exclusive);
    [[nodiscard]] Result<void, VfsError> unlock_range(FileHandle handle,
                                                      u64 offset, u64 length);
    
    /// List contents of a directory
    [[nodiscard]] Result<std::vector<DirectoryEntry>, VfsError> list_dir(
        std::string_view path
    );

    /// List contents of the directory represented by an open VFS handle.
    [[nodiscard]] Result<std::vector<DirectoryEntry>, VfsError>
    list_dir_handle(FileHandle handle);

    /// Resolve a Windows path to its mounted filesystem and relative path.
    /// Intended for bridge layers that need host-path aware integration.
    [[nodiscard]] Result<std::pair<IFileSystem*, std::string>, VfsError>
    resolve_components(std::string_view path);

    /// Resolve a Windows path to a concrete host path when supported.
    [[nodiscard]] Result<std::string, VfsError> resolve_host_path(
        std::string_view path
    );
    
    /// GetFinalPathNameByHandle backing: the guest path ("C:\\dir\\name",
    /// "C:\\" for a root) of the object `handle` is open on now, spelled as
    /// the filesystem stores it (IFileSystem::final_path).
    [[nodiscard]] Result<std::string, VfsError> final_path_for_handle(
        FileHandle handle
    );

    /// Create a directory
    [[nodiscard]] Result<void, VfsError> create_dir(std::string_view path);
    
    /// Remove a file or empty directory
    [[nodiscard]] Result<void, VfsError> remove(std::string_view path);
    
    /// Rename/move a file or directory
    [[nodiscard]] Result<void, VfsError> rename(
        std::string_view old_path,
        std::string_view new_path
    );

    /// FileRenameInfo: rename the object `handle` is open on. `new_path` is a
    /// guest path (absolute, or relative to the process CWD).
    [[nodiscard]] Result<void, VfsError> rename_open(
        FileHandle handle,
        std::string_view new_path,
        bool replace_existing
    );

    /// Create a hard link. Both names must resolve to the same mounted drive.
    [[nodiscard]] Result<void, VfsError> create_hard_link(
        std::string_view new_path,
        std::string_view existing_path
    );

    /// Set current working directory (used for drive-relative paths like C:foo)
    void set_cwd(const std::string& cwd);

    /// Get current working directory
    [[nodiscard]] std::string cwd() const;

private:
    struct GeneratedOpenFile {
        std::shared_ptr<const std::vector<u8>> bytes;
        u64 position{};
        /// The spelling the file was published under ("C:\\dir\\name").
        std::string final_path;
    };

    /// @p path as one spelling, "X:\\a\\b": the drive letter, backslashes,
    /// no trailing separator. The generated source and listing see this.
    [[nodiscard]] std::string generated_spelling(std::string_view path) const;
    [[nodiscard]] std::string generated_path_key(
        std::string_view path) const;
    /// The key of a path the WOW64 view already applied to.
    [[nodiscard]] std::string generated_key_of_viewed(
        std::string_view viewed) const;

    /// The generated file at @p path: published, or supplied (and then
    /// published) by generated_source_. Null when there is none. Caller holds
    /// mutex_.
    [[nodiscard]] std::shared_ptr<const std::vector<u8>> generated_file_locked(
        std::string_view path);
    /// Caller holds mutex_; @p path is a valid drive path.
    void publish_generated_locked(std::string_view path,
                                  std::shared_ptr<const std::vector<u8>> bytes);
    /// What stat and directory enumeration report for a generated file.
    [[nodiscard]] static FileInfo generated_file_info(u64 size) noexcept;

    /// Resolve path to filesystem and relative path.
    /// Caller must hold mutex_.
    [[nodiscard]] Result<std::pair<IFileSystem*, std::string>, VfsError> 
    resolve_path(std::string_view path);
    
    /// Get filesystem for a handle. Caller must hold mutex_.
    [[nodiscard]] Result<IFileSystem*, VfsError> get_fs_for_handle(FileHandle handle);

    /// Protects drives_, handle_to_drive_, default_drive_, and cwd_.
    /// Concurrent CreateFile/CloseHandle races previously corrupted
    /// handle_to_drive_ (host fault in get_fs_for_handle).
    mutable std::mutex mutex_;
    
    /// Mounted drives (uppercase letter -> filesystem)
    // In-flight path enumerations retain the mount after releasing mutex_.
    std::unordered_map<char, std::shared_ptr<IFileSystem>> drives_;
    
    /// Handle to drive letter mapping
    std::unordered_map<FileHandle, char> handle_to_drive_;

    std::unordered_map<std::string, std::shared_ptr<const std::vector<u8>>>
        generated_files_;
    /// generated_path_key -> published spelling, for final_path_for_handle.
    std::unordered_map<std::string, std::string> generated_names_;
    std::unordered_map<FileHandle, GeneratedOpenFile> generated_handles_;
    FileHandle next_generated_handle_ = 0xF0000000u;
    GeneratedFileSource generated_source_;
    GeneratedListing generated_listing_;
    
    /// Default drive for relative paths
    char default_drive_ = 'C';

    bool wow64_view_ = false;
    
    /// Current working directory (Windows path, e.g. "C:\\Program Files")
    std::string cwd_ = "C:\\";
};

} // namespace vitreo::capsule
