// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_environment.cpp
/// @brief The process environment block and the path APIs that read it.
///
/// GetEnvironmentVariable / SetEnvironmentVariable and the environment strings
/// block, ExpandEnvironmentStrings, and the path queries a guest uses to find
/// itself: GetModuleFileName's directory, GetTempPath, GetFullPathName,
/// GetCurrentDirectory, SearchPath, GetDiskFreeSpace.
///
/// Environment values are guest-visible strings in a capsule, not host ones, so
/// %-token expansion happens here rather than through getenv.
///
/// Split out of kernel32_ext.cpp. Contiguous and closed: the one anonymous
/// helper it carries, expand_environment_percent_tokens, is used nowhere else,
/// and nothing here needed a file-local helper from outside the region.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win_path.hpp"
#include "platform/host_volume_space.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <sys/statvfs.h>

/// @file disk.cpp
/// @brief Disk
///
/// Closed cut from environment_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Disk
// =============================================================================

/// The host boot volume's size and the space a caller may write, purgeable
/// space included (platform/host_volume_space.hpp).
void get_host_disk_stats(u64& out_total_bytes, u64& out_free_bytes) {
    platform::HostVolumeSpace space;
    if (platform::host_volume_space("/", space)) {
        out_total_bytes = space.total_bytes;
        out_free_bytes  = space.available_bytes;
    } else {
        // Fallback
        out_total_bytes = 500ULL * 1024 * 1024 * 1024;
        out_free_bytes  = 200ULL * 1024 * 1024 * 1024;
    }
}

void Kernel32ExtManager::impl_GetDiskFreeSpaceW(Marshaler& m) {
    // Same contract as GetDiskFreeSpaceA (directory_disk.cpp
    // get_disk_free_space); only the path's encoding differs.
    const GuestAddr path_ptr = m.arg_ptr(0);
    if (path_ptr == 0) {
        get_disk_free_space(m, nullptr);
        return;
    }
    auto value = m.read_wide_string(path_ptr);
    if (value.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    get_disk_free_space(m, &value.value());
}

} // namespace vitreo::winapi
