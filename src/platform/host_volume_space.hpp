// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file host_volume_space.hpp
/// @brief Size and free space of the host volume that holds a path.
///
/// GetDiskFreeSpace(Ex) and FileFsSizeInformation answer "how much can be
/// written here". On APFS that is more than the blocks that are unallocated
/// right now (statvfs f_bavail): purgeable space (local snapshots, caches)
/// is reclaimed by the system when an app needs the room. Apple publishes
/// that figure as NSURLVolumeAvailableCapacityForImportantUsageKey, for
/// resources the user explicitly asked for (a download or an install), and
/// Finder and System Settings report it as the volume's available space.
/// Windows has no purgeable class, so a guest sees it as free space.
///
/// Sources: statvfs(3); Apple Foundation "Checking Volume Storage Capacity"
/// and the CFURL volume capacity keys (macOS 10.13+).

#include <vitreo/types.hpp>

namespace vitreo::platform {

struct HostVolumeSpace {
    u64 total_bytes = 0;
    /// All free bytes on the volume, reclaimable purgeable space included.
    u64 free_bytes = 0;
    /// The part of free_bytes this process may write.
    u64 available_bytes = 0;
    /// statvfs f_frsize: the unit f_blocks/f_bavail count in.
    u64 allocation_unit = 0;
};

/// Reads the volume holding `path`. False when the volume cannot be queried
/// (statvfs fails); `out` is then zeroed.
[[nodiscard]] bool host_volume_space(const char* path,
                                     HostVolumeSpace& out) noexcept;

}  // namespace vitreo::platform
