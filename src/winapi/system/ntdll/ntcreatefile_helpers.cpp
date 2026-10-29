// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_ext.cpp
/// @brief ntdll.dll extended function stubs (WIN32-086)

#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "capsule/vfs/filesystem.hpp"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/time.h>

/// @file ntcreatefile_helpers.cpp
/// @brief NtCreateFile Helpers
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtCreateFile Helpers
// =============================================================================

capsule::AccessMode NtdllExtManager::to_access_mode(u32 desired_access) noexcept {
    using capsule::AccessMode;
    auto mode = AccessMode::None;

    // Use constants from kernel32/file.hpp or define NT-specific ones
    // NT access masks (FILE_READ_DATA, FILE_WRITE_DATA are NT-specific, not Win32)
    constexpr u32 NT_FILE_READ_DATA  = 0x0001;
    constexpr u32 NT_FILE_WRITE_DATA = 0x0002;
    constexpr u32 NT_FILE_APPEND_DATA = 0x0004;
    constexpr u32 NT_GENERIC_READ    = 0x80000000;
    constexpr u32 NT_GENERIC_WRITE   = 0x40000000;
    constexpr u32 NT_GENERIC_EXECUTE = 0x20000000;
    constexpr u32 NT_GENERIC_ALL     = 0x10000000;

    if (desired_access & NT_GENERIC_READ) {
        mode = mode | AccessMode::Read;
    }
    if (desired_access & NT_GENERIC_WRITE) {
        mode = mode | AccessMode::Write;
    }
    if (desired_access & NT_GENERIC_EXECUTE) {
        mode = mode | AccessMode::Execute;
    }
    if (desired_access & NT_GENERIC_ALL) {
        mode = mode | AccessMode::All;
    }
    if (desired_access & NT_FILE_READ_DATA) {
        mode = mode | AccessMode::Read;
    }
    if (desired_access & (NT_FILE_WRITE_DATA | NT_FILE_APPEND_DATA)) {
        mode = mode | AccessMode::Write;
    }
    // The same rights CreateFileW maps (FileManager::to_access_mode): DELETE
    // takes part in share arbitration, and the metadata and security writes
    // are what a protected system file refuses (builtin_dll_file_probe).
    constexpr u32 NT_DELETE = 0x00010000;
    constexpr u32 NT_FILE_WRITE_EA = 0x0010;
    constexpr u32 NT_FILE_WRITE_ATTRIBUTES = 0x0100;
    constexpr u32 NT_WRITE_DAC = 0x00040000;
    constexpr u32 NT_WRITE_OWNER = 0x00080000;
    if (desired_access & NT_DELETE) {
        mode = mode | AccessMode::Delete;
    }
    if (desired_access & (NT_FILE_WRITE_EA | NT_FILE_WRITE_ATTRIBUTES |
                          NT_WRITE_DAC | NT_WRITE_OWNER)) {
        mode = mode | AccessMode::WriteMetadata;
    }

    return mode;
}

capsule::ShareMode NtdllExtManager::to_share_mode(u32 share_access) noexcept {
    using capsule::ShareMode;
    auto mode = ShareMode::None;

    // NT share access flags (same values as Win32, but use local names to avoid shadowing)
    constexpr u32 NT_FILE_SHARE_READ   = 0x00000001;
    constexpr u32 NT_FILE_SHARE_WRITE  = 0x00000002;
    constexpr u32 NT_FILE_SHARE_DELETE = 0x00000004;

    if (share_access & NT_FILE_SHARE_READ) {
        mode = mode | ShareMode::Read;
    }
    if (share_access & NT_FILE_SHARE_WRITE) {
        mode = mode | ShareMode::Write;
    }
    if (share_access & NT_FILE_SHARE_DELETE) {
        mode = mode | ShareMode::Delete;
    }

    return mode;
}

capsule::CreationDisposition NtdllExtManager::to_disposition(u32 create_disposition) noexcept {
    using capsule::CreationDisposition;
    switch (create_disposition) {
        case nt_file::FILE_SUPERSEDE:    return CreationDisposition::CreateAlways;    // Replace
        case nt_file::FILE_OPEN:         return CreationDisposition::OpenExisting;    // Open only
        case nt_file::FILE_CREATE:       return CreationDisposition::CreateNew;       // Create only
        case nt_file::FILE_OPEN_IF:      return CreationDisposition::OpenAlways;      // Open or create
        case nt_file::FILE_OVERWRITE:    return CreationDisposition::TruncateExisting;// Open + truncate
        case nt_file::FILE_OVERWRITE_IF: return CreationDisposition::CreateAlways;    // Create + truncate
        default:                         return CreationDisposition::OpenExisting;
    }
}

u32 NtdllExtManager::vfs_error_to_ntstatus(capsule::VfsError err) noexcept {
    using capsule::VfsError;
    switch (err) {
        case VfsError::None:              return nt_status::STATUS_SUCCESS;
        case VfsError::PathNotFound:      return nt_status::STATUS_OBJECT_PATH_NOT_FOUND;
        case VfsError::PathAlreadyExists: return nt_status::STATUS_OBJECT_NAME_COLLISION;
        case VfsError::InvalidPath:       return nt_status::STATUS_OBJECT_NAME_INVALID;
        case VfsError::NotADirectory:     return nt_status::STATUS_OBJECT_PATH_NOT_FOUND;
        case VfsError::FileNotFound:      return nt_status::STATUS_OBJECT_NAME_NOT_FOUND;
        case VfsError::NotAFile:          return nt_status::STATUS_OBJECT_NAME_NOT_FOUND;
        case VfsError::AccessDenied:      return nt_status::STATUS_ACCESS_DENIED;
        case VfsError::SharingViolation:  return nt_status::STATUS_SHARING_VIOLATION;
        case VfsError::InvalidHandle:     return nt_status::STATUS_INVALID_HANDLE;
        case VfsError::DiskFull:          return nt_status::STATUS_DISK_FULL;
        case VfsError::OutOfMemory:       return nt_status::STATUS_NO_MEMORY;
        case VfsError::NotSupported:      return nt_status::STATUS_INVALID_DEVICE_REQUEST;
        default:                          return nt_status::STATUS_INVALID_PARAMETER;
    }
}

} // namespace vitreo::winapi
