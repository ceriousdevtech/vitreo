// SPDX-License-Identifier: LGPL-2.1-only

/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

/// @file win32_vfs_conversion_helpers.cpp
/// @brief Win32 → VFS Conversion Helpers
///
/// Closed cut from file.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Win32 → VFS Conversion Helpers
// =============================================================================

capsule::AccessMode FileManager::to_access_mode(u32 desired_access) noexcept {
    auto mode = capsule::AccessMode::None;

    // Generic access rights (high bits)
    if (desired_access & GENERIC_READ) {
        mode = mode | capsule::AccessMode::Read;
    }
    if (desired_access & GENERIC_WRITE) {
        mode = mode | capsule::AccessMode::Write;
    }
    if (desired_access & GENERIC_EXECUTE) {
        mode = mode | capsule::AccessMode::Execute;
    }
    if (desired_access & GENERIC_ALL) {
        mode = mode | capsule::AccessMode::All;
    }

    // Specific file access rights (low bits) — apps often pass these
    // instead of or in addition to GENERIC_* flags.
    constexpr u32 FILE_READ_DATA   = 0x0001;
    constexpr u32 FILE_WRITE_DATA  = 0x0002;
    constexpr u32 FILE_APPEND_DATA = 0x0004;
    constexpr u32 FILE_READ_EA     = 0x0008;
    constexpr u32 FILE_EXECUTE     = 0x0020;

    if (desired_access & FILE_READ_DATA) {
        mode = mode | capsule::AccessMode::Read;
    }
    if (desired_access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) {
        mode = mode | capsule::AccessMode::Write;
    }
    if (desired_access & FILE_READ_EA) {
        mode = mode | capsule::AccessMode::Read;
    }
    if (desired_access & FILE_EXECUTE) {
        mode = mode | capsule::AccessMode::Execute;
    }
    // DELETE is a data-less right, but it takes part in share arbitration
    // (Learn: CreateFileW dwShareMode / FILE_SHARE_DELETE).
    constexpr u32 DELETE_RIGHT = 0x00010000;
    if (desired_access & DELETE_RIGHT) {
        mode = mode | capsule::AccessMode::Delete;
    }
    // Rights that change a file's metadata or security without writing its
    // data. A writable file grants them with any open; a protected system
    // file refuses each with ERROR_ACCESS_DENIED (builtin_dll_file_probe).
    constexpr u32 FILE_WRITE_EA = 0x0010;
    constexpr u32 FILE_WRITE_ATTRIBUTES = 0x0100;
    constexpr u32 WRITE_DAC_RIGHT = 0x00040000;
    constexpr u32 WRITE_OWNER_RIGHT = 0x00080000;
    if (desired_access & (FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                          WRITE_DAC_RIGHT | WRITE_OWNER_RIGHT)) {
        mode = mode | capsule::AccessMode::WriteMetadata;
    }

    return mode;
}

capsule::ShareMode FileManager::to_share_mode(u32 share_mode) noexcept {
    auto mode = capsule::ShareMode::None;

    if (share_mode & FILE_SHARE_READ) {
        mode = mode | capsule::ShareMode::Read;
    }
    if (share_mode & FILE_SHARE_WRITE) {
        mode = mode | capsule::ShareMode::Write;
    }
    if (share_mode & FILE_SHARE_DELETE) {
        mode = mode | capsule::ShareMode::Delete;
    }

    return mode;
}

capsule::CreationDisposition FileManager::to_disposition(u32 creation_disp) noexcept {
    switch (creation_disp) {
        case CREATE_NEW:        return capsule::CreationDisposition::CreateNew;
        case CREATE_ALWAYS:     return capsule::CreationDisposition::CreateAlways;
        case OPEN_EXISTING:     return capsule::CreationDisposition::OpenExisting;
        case OPEN_ALWAYS:       return capsule::CreationDisposition::OpenAlways;
        case TRUNCATE_EXISTING: return capsule::CreationDisposition::TruncateExisting;
        default:                return capsule::CreationDisposition::OpenExisting;
    }
}

} // namespace vitreo::winapi
