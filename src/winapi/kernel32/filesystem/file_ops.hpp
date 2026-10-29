// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file file_ops.hpp
/// @brief Extended kernel32 file and system operations.
///
/// Provides DeleteFileW, CreateDirectoryW, GetSystemDirectoryW, GetVersion,
/// LoadLibraryExW, GetStartupInfoA, SetFileAttributesW, SetFileTime,
/// lstrcatW, lstrcpyW, MoveFileExW.

#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"
#include "core/memory/guest_memory.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;
class FileManager;
class RegistryThunks;

// =============================================================================
// FileOpsManager
// =============================================================================

/// Manages extended kernel32 file operations and related system queries.
class FileOpsManager {
public:
    /// Construct with guest memory and VFS references
    /// @param memory Reference to guest memory for wide-string operations
    /// @param vfs    Reference to the VFS for directory operations
    explicit FileOpsManager(GuestMemory& memory, capsule::Vfs& vfs);

    /// Optionally attach a ModuleManager for LoadLibraryExW delegation
    void set_module_manager(ModuleManager* mm) noexcept { m_modules = mm; }

    /// Drop directory-enumeration cursor state when a file handle closes
    /// (GetFileInformationByHandleEx #5).
    void on_file_handle_closed(Handle h) noexcept;

    /// Share the directory-enumeration cursor across DuplicateHandle so both
    /// handles advance the same FILE_OBJECT cursor (GetFileInformationByHandleEx #5).
    void on_file_handle_duplicated(Handle source, Handle duplicate) noexcept;

    /// Optionally attach a FileManager so handle-based file APIs (e.g.
    /// SetFileTime) can resolve Win32 handles back to VFS handles.
    void set_file_manager(FileManager* fm) noexcept { m_file_manager = fm; }

    /// The registry MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT) records its
    /// operations in (HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager
    /// \\PendingFileRenameOperations). Without one that flag fails.
    void set_registry(RegistryThunks* registry) noexcept { m_registry = registry; }

    /// No copy
    FileOpsManager(const FileOpsManager&) = delete;
    FileOpsManager& operator=(const FileOpsManager&) = delete;

    /// No move (contains references)
    FileOpsManager(FileOpsManager&&) = delete;
    FileOpsManager& operator=(FileOpsManager&&) = delete;

    ~FileOpsManager() = default;

    // -------------------------------------------------------------------------
    // File Operations (stubs)
    // -------------------------------------------------------------------------

    /// DeleteFileA/W — real VFS delete. Both forward to delete_file_common so
    /// their error mapping cannot drift apart.
    void impl_DeleteFileW(Marshaler& m);
    void impl_DeleteFileA(Marshaler& m);
    void delete_file_common(Marshaler& m, bool wide);

    /// CreateDirectoryW — stub, always returns TRUE
    void impl_CreateDirectoryW(Marshaler& m);

    /// SetFileAttributesW — stub, always returns TRUE
    void impl_SetFileAttributesW(Marshaler& m);
    void impl_SetFileAttributesA(Marshaler& m);

    /// SetFileTime — stub, always returns TRUE
    void impl_SetFileTime(Marshaler& m);

    /// MoveFileExW — stub, always returns TRUE
    void impl_MoveFileExW(Marshaler& m);
    void impl_MoveFileExA(Marshaler& m);
    void move_file_ex_common(Marshaler& m, bool wide);

    // -------------------------------------------------------------------------
    // System Queries
    // -------------------------------------------------------------------------

    /// GetSystemDirectoryW — write "C:\Windows\System32" (UTF-16LE) to buffer
    void impl_GetSystemDirectoryW(Marshaler& m);

    /// GetVersion — return packed Win7-compat version number
    void impl_GetVersion(Marshaler& m);

    /// LoadLibraryExW — delegate to ModuleManager or return dummy handle
    void impl_LoadLibraryExW(Marshaler& m);

    /// GetStartupInfoA — zero-fill STARTUPINFOA (68 bytes), set cb=68
    void impl_GetStartupInfoA(Marshaler& m);

    // -------------------------------------------------------------------------
    // String Operations
    // -------------------------------------------------------------------------

    /// lstrcpyW — copy wide string in guest memory, return dst pointer
    void impl_lstrcpyW(Marshaler& m);

    /// lstrcatW — concatenate wide string in guest memory, return dst pointer
    void impl_lstrcatW(Marshaler& m);

    /// ReplaceFileW — stub, returns TRUE
    void impl_ReplaceFileW(Marshaler& m);
    /// GetFileInformationByName (Windows 11 24H2+, WinBase.h 10.0.26100)
    void impl_GetFileInformationByName(Marshaler& m);

    /// CreateSymbolicLinkW — stub, returns TRUE
    void impl_CreateSymbolicLinkW(Marshaler& m);

    /// MoveFileTransactedW — passthrough (ignores hTransaction & progress routine)
    void impl_MoveFileTransactedW(Marshaler& m);

    /// DeleteFileTransactedW — passthrough (ignores hTransaction)
    void impl_DeleteFileTransactedW(Marshaler& m);

    /// CreateDirectoryTransactedW — passthrough; uses arg(1) as new path
    /// (template directory and hTransaction are ignored).
    void impl_CreateDirectoryExW(Marshaler& m);
    /// Shared core of CreateDirectoryExW / CreateDirectoryTransactedW, whose
    /// new-directory path is at @p new_dir_arg (arg 1) rather than arg 0.
    void create_directory_from_template(Marshaler& m, usize new_dir_arg,
                                        const char* api);
    void impl_CreateDirectoryTransactedW(Marshaler& m);

    /// GetFileInformationByHandleEx — return real file metadata and
    /// resumable directory enumeration for supported information classes.
    void impl_GetFileInformationByHandleEx(Marshaler& m);

    /// SetFileInformationByHandle — apply real changes for known info
    /// classes (FileEndOfFileInfo, FileDispositionInfo, FileBasicInfo,
    /// FileAllocationInfo, FileRenameInfo).
    void impl_SetFileInformationByHandle(Marshaler& m);

private:
    /// Shared body of CopyFileExA/W; @p wide selects how the paths are decoded.
    void copy_file_ex_common(Marshaler& m, bool wide);

    struct DirectoryEnumerationState {
        std::vector<capsule::DirectoryEntry> entries;
        usize next_index = 0;
    };

    GuestMemory& m_memory;
    capsule::Vfs& m_vfs;
    ModuleManager* m_modules = nullptr;
    FileManager* m_file_manager = nullptr;
    RegistryThunks* m_registry = nullptr;
    /// Appends a MOVEFILE_DELAY_UNTIL_REBOOT operation; false + last error on failure.
    bool record_pending_rename(const std::string& src_utf8, const std::string* dst_utf8, bool replace);
    std::mutex m_directory_enumeration_mutex;
    /// Shared across DuplicateHandle'd directory handles (FILE_OBJECT cursor).
    std::unordered_map<u32, std::shared_ptr<DirectoryEnumerationState>>
        m_directory_enumerations;

    /// Write a UTF-16LE string to guest memory (including null terminator)
    /// @return Number of characters written (excluding null terminator)
    u32 write_wide_literal(GuestAddr addr, const char* str);

    /// Find the length of a wide string in guest memory (in chars, excluding null)
    u32 guest_wstrlen(GuestAddr addr);
};

// =============================================================================
// Export Table Factory
// =============================================================================

/// Build DllExports for file-operation thunks
/// @param mgr Reference to FileOpsManager (captured by lambdas)
/// @return DllExports containing all file-ops entries
[[nodiscard]] DllExports create_file_ops_thunks(FileOpsManager& mgr);

} // namespace vitreo::winapi
