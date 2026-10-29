// SPDX-License-Identifier: LGPL-2.1-only

/// @file directory_disk.cpp
/// @brief Directory and disk query APIs.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

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
    // UINT GetDriveTypeW(LPCWSTR lpRootPathName)
    // Learn + files_ns_probe: NULL / "X:" / "X:\" (and "X:\subdir\" with a
    // trailing slash) report the mounted volume's type; unmounted letters and
    // paths without the required trailing slash return DRIVE_NO_ROOT_DIR.
    constexpr u32 DRIVE_NO_ROOT_DIR = 1;
    constexpr u32 DRIVE_FIXED = 3;
    constexpr u32 DRIVE_CDROM = 5;

    GuestAddr path_ptr = m.arg_ptr(0);
    std::string path;
    if (path_ptr == 0) {
        // Root of the current directory's drive.
        path = m_current_directory;
        if (path.size() >= 2 && path[1] == ':') {
            path = path.substr(0, 2) + "\\";
        } else {
            path = "C:\\";
        }
    } else {
        auto path_res = m.read_wide_string(path_ptr);
        if (path_res.is_err()) {
            m.set_return(DRIVE_NO_ROOT_DIR);
            return;
        }
        path = path_res.value();
    }

    if (path.empty()) {
        m.set_return(DRIVE_NO_ROOT_DIR);
        return;
    }

    // "X:" alone is accepted (current directory on that drive). Any longer
    // form needs a trailing separator (files_ns_probe GetDriveTypeW_subdir*).
    const bool drive_only = path.size() == 2 && path[1] == ':';
    const bool has_trailing =
        !path.empty() && (path.back() == '\\' || path.back() == '/');
    if (!drive_only && !has_trailing) {
        m.set_return(DRIVE_NO_ROOT_DIR);
        return;
    }
    // A directory below the root is probed as a volume mount point; not
    // being one leaves ERROR_NOT_A_REPARSE_POINT behind while the drive's
    // type is still returned (files_ns_probe GetDriveTypeW_subdir).
    if (!drive_only && path.size() > 3) {
        set_last_error(error_codes::ERROR_NOT_A_REPARSE_POINT);
    }

    if (path.size() < 2 || path[1] != ':') {
        m.set_return(DRIVE_NO_ROOT_DIR);
        return;
    }
    const char letter =
        static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    if (letter < 'A' || letter > 'Z') {
        m.set_return(DRIVE_NO_ROOT_DIR);
        return;
    }

    const auto drives = m_vfs.mounted_drives();
    const bool mounted =
        std::find(drives.begin(), drives.end(), letter) != drives.end();
    if (!mounted) {
        m.set_return(DRIVE_NO_ROOT_DIR);
        return;
    }

    const std::string root = std::string(1, letter) + ":\\";
    auto resolved = m_vfs.resolve_components(root);
    if (resolved.is_ok() && resolved.value().first->is_optical()) {
        m.set_return(DRIVE_CDROM);
        return;
    }
    m.set_return(DRIVE_FIXED);
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

    std::string path;
    if (path_arg == nullptr) {
        if (m_current_directory.size() >= 2 && m_current_directory[1] == ':') {
            path = m_current_directory.substr(0, 2) + "\\";
        }
    } else {
        if (path_arg->empty()) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
            m.set_return(0);
            return;
        }
        path = *path_arg;
    }

    char letter = 0;
    if (path.size() >= 2 && path[1] == ':') {
        letter = static_cast<char>(
            std::toupper(static_cast<unsigned char>(path[0])));
    } else if (m_current_directory.size() >= 2 && m_current_directory[1] == ':') {
        letter = static_cast<char>(
            std::toupper(static_cast<unsigned char>(m_current_directory[0])));
    }
    const auto drives = m_vfs.mounted_drives();
    if (letter < 'A' || letter > 'Z' ||
        std::find(drives.begin(), drives.end(), letter) == drives.end()) {
        fail(error_codes::ERROR_PATH_NOT_FOUND);
        return;
    }
    auto info = m_vfs.stat(path);
    if (info.is_err()) {
        fail(vfs_error_to_win32(info.error()));
        return;
    }
    if (!capsule::has_attribute(info.value().attributes,
                                capsule::FileAttributes::Directory)) {
        fail(error_codes::ERROR_DIRECTORY);
        return;
    }

    // The volume's own geometry, else the host's totals in 4 KiB clusters.
    u32 sectors_per_cluster = 8;
    u32 bytes_per_sector = 512;
    u32 free_cl = 0;
    u32 total_cl = 0;
    capsule::VolumeInfo volume;
    auto resolved = m_vfs.resolve_components(std::string(1, letter) + ":\\");
    if (resolved.is_ok() && resolved.value().first->volume_info(volume) &&
        volume.sectors_per_cluster > 0 && volume.bytes_per_sector > 0) {
        sectors_per_cluster = volume.sectors_per_cluster;
        bytes_per_sector = volume.bytes_per_sector;
        free_cl = volume.free_clusters;
        total_cl = volume.total_clusters;
    } else {
        u64 total_bytes = 0, free_bytes = 0;
        get_host_disk_stats(total_bytes, free_bytes);
        const u64 cluster_size = static_cast<u64>(sectors_per_cluster) * bytes_per_sector;
        free_cl = static_cast<u32>(std::min(free_bytes / cluster_size,
                                            static_cast<u64>(0xFFFFFFFFu)));
        total_cl = static_cast<u32>(std::min(total_bytes / cluster_size,
                                             static_cast<u64>(0xFFFFFFFFu)));
    }

    if (spc_ptr != 0) (void)m_memory.write_u32(spc_ptr, sectors_per_cluster);
    if (bps_ptr != 0) (void)m_memory.write_u32(bps_ptr, bytes_per_sector);
    if (free_ptr != 0) (void)m_memory.write_u32(free_ptr, free_cl);
    if (total_ptr != 0) (void)m_memory.write_u32(total_ptr, total_cl);
    VITREO_TRACE("[k32ext] GetDiskFreeSpace('%s') -> spc=%u bps=%u free=%u total=%u\n",
                 path.c_str(), sectors_per_cluster, bytes_per_sector, free_cl, total_cl);
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_GetDiskFreeSpaceA(Marshaler& m) {
    const GuestAddr path_ptr = m.arg_ptr(0);
    if (path_ptr == 0) {
        get_disk_free_space(m, nullptr);
        return;
    }
    auto value = m.read_ansi_string(path_ptr, 260);
    if (value.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    get_disk_free_space(m, &value.value());
}

void Kernel32ExtManager::impl_GetDiskFreeSpaceExW(Marshaler& m) {
    // BOOL GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName,
    //   PULARGE_INTEGER lpFreeBytesAvailableToCaller,
    //   PULARGE_INTEGER lpTotalNumberOfBytes,
    //   PULARGE_INTEGER lpTotalNumberOfFreeBytes)
    // Learn + files_ns_probe: path must name an existing directory on a
    // mounted volume; a file -> ERROR_DIRECTORY; missing/unmounted ->
    // ERROR_PATH_NOT_FOUND. Optical volumes report 0 free bytes.
    GuestAddr path_ptr = m.arg_ptr(0);
    GuestAddr free_avail = m.arg_ptr(1);
    GuestAddr total_bytes_ptr = m.arg_ptr(2);
    GuestAddr total_free = m.arg_ptr(3);

    std::string path;
    if (path_ptr == 0) {
        path = m_current_directory;
    } else {
        auto path_res = m.read_wide_string(path_ptr);
        if (path_res.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        path = path_res.value();
    }

    // Resolve to a drive letter and check mount.
    char letter = 0;
    if (path.size() >= 2 && path[1] == ':') {
        letter = static_cast<char>(
            std::toupper(static_cast<unsigned char>(path[0])));
    } else if (!m_current_directory.empty() && m_current_directory[1] == ':') {
        letter = static_cast<char>(
            std::toupper(static_cast<unsigned char>(m_current_directory[0])));
    }
    if (letter < 'A' || letter > 'Z') {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }
    const auto drives = m_vfs.mounted_drives();
    if (std::find(drives.begin(), drives.end(), letter) == drives.end()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }

    auto info = m_vfs.stat(path);
    if (info.is_err()) {
        // GetDiskFreeSpaceExW reports PATH_NOT_FOUND for any missing path
        // (files_ns_probe GetDiskFreeSpaceExW_missing_dir -> 3), not
        // FILE_NOT_FOUND for a missing leaf.
        using capsule::VfsError;
        const auto err = info.error();
        if (err == VfsError::FileNotFound || err == VfsError::PathNotFound) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        } else {
            set_last_error(vfs_error_to_win32(err));
        }
        m.set_return(0);
        return;
    }
    if (!capsule::has_attribute(info.value().attributes,
                                capsule::FileAttributes::Directory)) {
        set_last_error(error_codes::ERROR_DIRECTORY);
        m.set_return(0);
        return;
    }

    u64 total_bytes = 0, free_bytes = 0, avail_bytes = 0;
    capsule::VolumeInfo volume{};
    const std::string root = std::string(1, letter) + ":\\";
    auto resolved = m_vfs.resolve_components(root);
    const bool optical =
        resolved.is_ok() && resolved.value().first->is_optical();
    const bool has_volume =
        resolved.is_ok() && resolved.value().first->volume_info(volume);
    if (has_volume && volume.total_bytes > 0) {
        // Exact counts (host volumes): all free bytes, and the caller's share.
        total_bytes = volume.total_bytes;
        free_bytes = volume.free_bytes;
        avail_bytes = volume.available_bytes;
    } else if (has_volume && volume.bytes_per_sector > 0 &&
               volume.sectors_per_cluster > 0) {
        const u64 cluster = static_cast<u64>(volume.bytes_per_sector) *
                            static_cast<u64>(volume.sectors_per_cluster);
        total_bytes = cluster * static_cast<u64>(volume.total_clusters);
        free_bytes = cluster * static_cast<u64>(volume.free_clusters);
        avail_bytes = free_bytes;
    } else {
        get_host_disk_stats(total_bytes, free_bytes);
        avail_bytes = free_bytes;
    }
    if (optical) {
        free_bytes = 0;
        avail_bytes = 0;
    }

    auto write_u64 = [&](GuestAddr addr, u64 val) {
        (void)m_memory.write_u32(addr,     static_cast<u32>(val & 0xFFFFFFFFu));
        (void)m_memory.write_u32(addr + 4, static_cast<u32>(val >> 32));
    };
    if (free_avail != 0)      write_u64(free_avail, avail_bytes);
    if (total_bytes_ptr != 0) write_u64(total_bytes_ptr, total_bytes);
    if (total_free != 0)      write_u64(total_free, free_bytes);
    m.set_return(1);
}

} // namespace vitreo::winapi
