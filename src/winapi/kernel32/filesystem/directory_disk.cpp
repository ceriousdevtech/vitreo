// SPDX-License-Identifier: LGPL-2.1-only

/// @file directory_disk.cpp
/// @brief Directory and disk query APIs.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

#include "winapi/kernel32/filesystem/drive_type.hpp"
#include "winapi/kernel32/filesystem/disk_space.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// Directory / Disk
// =============================================================================

void Kernel32ExtManager::impl_GetCurrentDirectoryA(Marshaler& m) {
    // DWORD GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer)
    // Learn: *A writes ANSI (CP_ACP) TCHARs, not host UTF-8 bytes.
    u32 buf_len = static_cast<u32>(m.arg(0));
    GuestAddr buf = m.arg_ptr(1);

    const std::string acp = utf8_to_acp(m_current_directory);
    const u32 needed = static_cast<u32>(acp.size());

    if (buf == 0 || buf_len == 0) {
        m.set_return(needed + 1);
        return;
    }

    if (buf_len <= needed) {
        m.set_return(needed + 1);
        return;
    }

    for (u32 i = 0; i < needed; ++i) {
        (void)m_memory.write_u8(buf + i, static_cast<u8>(acp[i]));
    }
    (void)m_memory.write_u8(buf + needed, 0);
    m.set_return(needed);
}

void Kernel32ExtManager::impl_RemoveDirectoryA(Marshaler& m) {
    // BOOL RemoveDirectoryA(LPCSTR lpPathName): RemoveDirectoryW's contract
    // on the CP_ACP name (it previously returned TRUE and removed nothing).
    remove_directory_common(m, /*wide=*/false);
}

void Kernel32ExtManager::impl_GetDriveTypeW(Marshaler& m) {
    m.set_return(query_drive_type(m, m_vfs, m_current_directory, true));
}

void Kernel32ExtManager::get_disk_free_space(Marshaler& m,
                                             const std::string* path_arg) {
    // BOOL GetDiskFreeSpace{A,W}(lpRootPathName, LPDWORD lpSectorsPerCluster,
    //   LPDWORD lpBytesPerSector, LPDWORD lpNumberOfFreeClusters,
    //   LPDWORD lpTotalNumberOfClusters)
    // Measured on Windows (disk_free_probe, x86 == x64):
    //  - NULL is the current drive's root. Any existing directory on a
    //    mounted drive works, with or without the trailing backslash Learn
    //    asks for ("C:" and "C:/" included).
    //  - "" fails ERROR_PATH_NOT_FOUND before touching any output.
    //  - An unmounted drive fails ERROR_PATH_NOT_FOUND, a missing directory
    //    ERROR_FILE_NOT_FOUND (its parent exists), a file ERROR_DIRECTORY;
    //    each writes 0 to *lpBytesPerSector and leaves the other outputs.
    //  - Success leaves the last error alone, any output may be NULL, and
    //    the counts are the volume's real ones (no 2 GB cap).
    const GuestAddr spc_ptr = m.arg_ptr(1);
    const GuestAddr bps_ptr = m.arg_ptr(2);
    const GuestAddr free_ptr = m.arg_ptr(3);
    const GuestAddr total_ptr = m.arg_ptr(4);
    auto fail = [&](u32 error) {
        if (bps_ptr != 0) (void)m_memory.write_u32(bps_ptr, 0);
        set_last_error(error);
        m.set_return(0);
    };

    const auto directory = query_disk_directory_volume(m_vfs, m_current_directory, path_arg, false);
    if (directory.error) {
        if (directory.preserve_outputs) { set_last_error(directory.error); m.set_return(0); }
        else fail(directory.error);
        return;
    }
    const auto& volume = directory.volume;
    if (!volume.sectors_per_cluster || !volume.bytes_per_sector) {
        fail(disk_space_unsupported()); return;
    }
    if (spc_ptr != 0) (void)m_memory.write_u32(spc_ptr, volume.sectors_per_cluster);
    if (bps_ptr != 0) (void)m_memory.write_u32(bps_ptr, volume.bytes_per_sector);
    if (free_ptr != 0) (void)m_memory.write_u32(free_ptr, directory.optical ? 0 : volume.free_clusters);
    if (total_ptr != 0) (void)m_memory.write_u32(total_ptr, volume.total_clusters);
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetDiskFreeSpaceA(Marshaler& m) {
    const GuestAddr path_ptr = m.arg_ptr(0);
    if (path_ptr == 0) {
        get_disk_free_space(m, nullptr);
        return;
    }
    std::string path;
    if (!read_disk_space_path(m, false, path)) { m.set_return(0); return; }
    get_disk_free_space(m, &path);
}

void Kernel32ExtManager::impl_GetDiskFreeSpaceExW(Marshaler& m) {
    get_disk_free_space_ex(m, true);
}

void Kernel32ExtManager::get_disk_free_space_ex(Marshaler& m, bool wide) {
    std::string path;
    const std::string* path_arg = nullptr;
    if (m.arg_ptr(0)) {
        if (!read_disk_space_path(m, wide, path)) { m.set_return(0); return; }
        path_arg = &path;
    }
    const auto directory = query_disk_directory_volume(m_vfs, m_current_directory, path_arg, true);
    const auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    if (directory.error) { fail(directory.error); return; }
    const auto& volume = directory.volume;
    u64 total_bytes = volume.total_bytes;
    u64 free_bytes = volume.free_bytes;
    u64 available = volume.available_bytes;
    if (!total_bytes) {
        // Geometry-only optical providers have known zero free counts. A
        // nonoptical provider's caller-available clusters cannot establish
        // the distinct all-users free-byte count required by Ex.
        if (!directory.optical || !volume.sectors_per_cluster || !volume.bytes_per_sector) {
            fail(disk_space_unsupported()); return;
        }
        const u64 cluster = static_cast<u64>(volume.sectors_per_cluster) * volume.bytes_per_sector;
        total_bytes = cluster * volume.total_clusters;
        free_bytes = cluster * volume.free_clusters;
        available = free_bytes;
    }
    if (directory.optical) { free_bytes = 0; available = 0; }
    for (const auto& [pointer, value] : {std::pair{m.arg_ptr(1), available},
        std::pair{m.arg_ptr(2), total_bytes}, std::pair{m.arg_ptr(3), free_bytes}}) {
        if (pointer && m.memory().write_u64(pointer, value).is_err()) {
            fail(error_codes::ERROR_NOACCESS); return;
        }
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
