// SPDX-License-Identifier: LGPL-2.1-only

/// @file sysinfo.cpp
/// @brief Implementation of system information APIs (WIN32-064)

#include "sysinfo.hpp"
#include "winapi/kernel32/error.hpp"
#include "virtual_firmware.hpp"
#include "graphics/common/perf_diag.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mach/mach_time.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/statvfs.h>
#endif

/// @file new_sysinfomanager_method_implementations.cpp
/// @brief New SysInfoManager method implementations
///
/// Closed cut from sysinfo.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// New SysInfoManager method implementations
// =============================================================================

bool SysInfoManager::global_memory_status_ex(GuestAddr buffer, Marshaler& m) const {
#ifdef __APPLE__
    auto len_result = m.read_u32(buffer);
    u32 dw_length = len_result.is_ok() ? len_result.value() : 0;
    if (dw_length < 64) {
        VITREO_TRACE("[sysinfo] GlobalMemoryStatusEx: dwLength=%u < 64, failing\n", dw_length);
        return false;
    }

    // Get total physical RAM via sysctl
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    u64 total_phys = 0;
    size_t sz = sizeof(total_phys);
    sysctl(mib, 2, &total_phys, &sz, nullptr, 0);

    // Get free/available pages via vm_statistics64
    vm_statistics64_data_t vm_stat{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64,
                      reinterpret_cast<host_info64_t>(&vm_stat), &count);

    u64 page_size = static_cast<u64>(vm_page_size);
    u64 avail_phys = (static_cast<u64>(vm_stat.free_count) +
                      static_cast<u64>(vm_stat.inactive_count)) * page_size;

    u32 mem_load = 0;
    if (total_phys > 0) {
        mem_load = static_cast<u32>(((total_phys - avail_phys) * 100) / total_phys);
    }

    // Learn MEMORYSTATUSEX: ullTotalVirtual is this process's user-mode VA
    // size (≈2 GB for a non-LAA 32-bit process, not a hardcoded 4 GB);
    // ullAvailVirtual is the unreserved/uncommitted remainder. Reporting
    // 0xFFFE0000 for both told a 32-bit SIZE_T consumer the whole window
    // was empty after hundreds of MB were already mapped.
    const u64 max_va =
        (m_memory != nullptr && m_memory->maximum_user_address() != 0)
            ? static_cast<u64>(m_memory->maximum_user_address())
            : (m.cpu_is_64bit() ? 0x00007FFFFFFEFFFFull
                                : 0x000000007FFEFFFFull);
    constexpr u64 kMinUserVa = 0x10000ull;
    const u64 total_virtual =
        max_va >= kMinUserVa ? (max_va - kMinUserVa + 1) : max_va;
    const u64 mapped = (m_memory != nullptr)
                           ? static_cast<u64>(m_memory->total_mapped_size())
                           : 0u;
    const u64 available_virtual =
        mapped >= total_virtual ? 0u : total_virtual - mapped;

    // Write MEMORYSTATUSEX struct (64 bytes)
    (void)m.write_u32(buffer + 0,  64);           // dwLength
    (void)m.write_u32(buffer + 4,  mem_load);     // dwMemoryLoad
    (void)m.write_u64(buffer + 8,  total_phys);   // ullTotalPhys
    (void)m.write_u64(buffer + 16, avail_phys);   // ullAvailPhys
    (void)m.write_u64(buffer + 24, total_phys);   // ullTotalPageFile
    (void)m.write_u64(buffer + 32, avail_phys);   // ullAvailPageFile
    (void)m.write_u64(buffer + 40, total_virtual);
    (void)m.write_u64(buffer + 48, available_virtual);
    (void)m.write_u64(buffer + 56, 0);            // ullAvailExtendedVirtual

    VITREO_TRACE("[sysinfo] GlobalMemoryStatusEx: total=%llu avail=%llu load=%u%%\n",
                 total_phys, avail_phys, mem_load);
    return true;
#else
    (void)buffer; (void)m;
    return false;
#endif
}

// Marshal a SYSTEM_INFO into guest memory with the width the guest expects.
// 32-bit SYSTEM_INFO is 36 bytes (4-byte pointer/DWORD_PTR fields); 64-bit is
// 48 bytes (8-byte fields).  Writing the host struct blindly overflows a 32-bit
// guest's buffer and corrupts adjacent stack (return address).
void marshal_system_info(GuestAddr p, const SystemInfo& info, Marshaler& m) {
    if (p == 0) return;
    const bool w64 = m.cpu_is_64bit();
    (void)m.write_u16(p + 0, info.processor_architecture);
    (void)m.write_u16(p + 2, info.reserved);
    (void)m.write_u32(p + 4, info.page_size);
    if (w64) {
        (void)m.write_u64(p + 8,  info.min_app_address);
        (void)m.write_u64(p + 16, info.max_app_address);
        (void)m.write_u64(p + 24, info.active_processor_mask);
        (void)m.write_u32(p + 32, info.number_of_processors);
        (void)m.write_u32(p + 36, info.processor_type);
        (void)m.write_u32(p + 40, info.allocation_granularity);
        (void)m.write_u16(p + 44, info.processor_level);
        (void)m.write_u16(p + 46, info.processor_revision);
    } else {
        (void)m.write_u32(p + 8,  static_cast<u32>(info.min_app_address));
        (void)m.write_u32(p + 12,
            static_cast<u32>(info.max_app_address > 0xFFFEFFFFull
                             ? 0xFFFEFFFFull : info.max_app_address));
        (void)m.write_u32(p + 16, static_cast<u32>(info.active_processor_mask));
        (void)m.write_u32(p + 20, info.number_of_processors);
        (void)m.write_u32(p + 24, info.processor_type);
        (void)m.write_u32(p + 28, info.allocation_granularity);
        (void)m.write_u16(p + 32, info.processor_level);
        (void)m.write_u16(p + 34, info.processor_revision);
    }
}

void SysInfoManager::get_native_system_info(GuestAddr info_ptr, Marshaler& m) const {
    const SystemInfo info = get_system_info();
    marshal_system_info(info_ptr, info, m);
    VITREO_TRACE("[sysinfo] GetNativeSystemInfo: ncpu=%u\n",
                 info.number_of_processors);
}

bool SysInfoManager::get_disk_free_space_ex_w(const std::string& dir,
                                               GuestAddr free_caller,
                                               GuestAddr total_bytes,
                                               GuestAddr total_free,
                                               Marshaler& m) const {
#ifdef __APPLE__
    struct statvfs st{};
    if (statvfs("/", &st) != 0) {
        VITREO_TRACE("[sysinfo] GetDiskFreeSpaceExW: statvfs failed\n");
        return false;
    }

    u64 free_bytes  = static_cast<u64>(st.f_bavail) * static_cast<u64>(st.f_frsize);
    u64 total       = static_cast<u64>(st.f_blocks) * static_cast<u64>(st.f_frsize);
    u64 total_avail = static_cast<u64>(st.f_bfree)  * static_cast<u64>(st.f_frsize);

    if (free_caller) (void)m.write_u64(free_caller, free_bytes);
    if (total_bytes) (void)m.write_u64(total_bytes, total);
    if (total_free)  (void)m.write_u64(total_free,  total_avail);

    VITREO_TRACE("[sysinfo] GetDiskFreeSpaceExW(%s): free=%llu total=%llu\n",
                 dir.c_str(), free_bytes, total);
    return true;
#else
    (void)dir; (void)free_caller; (void)total_bytes; (void)total_free; (void)m;
    return false;
#endif
}

bool SysInfoManager::get_volume_information_w(GuestAddr root, GuestAddr vol_name_buf,
                                               u32 vol_name_size, GuestAddr serial,
                                               GuestAddr max_comp_len, GuestAddr flags,
                                               GuestAddr fs_name_buf, u32 fs_name_size,
                                               Marshaler& m) const {
    // Learn + files_ns_probe: trailing backslash required on a volume root;
    // subdirectories fail ERROR_DIR_NOT_ROOT; unmounted letters
    // ERROR_PATH_NOT_FOUND; too-small name buffers ERROR_BAD_LENGTH.
    // Serial matches BY_HANDLE_FILE_INFORMATION.dwVolumeSerialNumber
    // (host st_dev truncated) for a file on the same volume.
    constexpr u32 kCaseSensitiveSearch = 0x1;
    constexpr u32 kCasePreservedNames = 0x2;
    constexpr u32 kUnicodeOnDisk = 0x4;
    constexpr u32 kPersistentAcls = 0x8;

    std::string path;
    if (root == 0) {
        path = "C:\\";
    } else {
        auto path_res = m.read_wide_string(root);
        if (path_res.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        path = path_res.value();
    }

    if (path.empty()) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }

    // "X:" without trailing slash -> ERROR_INVALID_NAME.
    if (path.size() == 2 && path[1] == ':') {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }
    if (path.back() != '\\' && path.back() != '/') {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }

    // Volume root must be exactly "X:\"; anything longer is not the root.
    if (!(path.size() == 3 && path[1] == ':' &&
          (path[2] == '\\' || path[2] == '/'))) {
        set_last_error(error_codes::ERROR_DIR_NOT_ROOT);
        return false;
    }

    const char letter =
        static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    if (letter < 'A' || letter > 'Z' || !m_vfs) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        return false;
    }
    const auto drives = m_vfs->mounted_drives();
    if (std::find(drives.begin(), drives.end(), letter) == drives.end()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        return false;
    }

    const std::string root_path = std::string(1, letter) + ":\\";
    capsule::VolumeInfo volume{};
    auto resolved = m_vfs->resolve_components(root_path);
    const bool has_volume =
        resolved.is_ok() && resolved.value().first->volume_info(volume);
    const std::string label = has_volume ? volume.label : std::string{};
    const std::string filesystem =
        has_volume ? volume.filesystem_name : std::string{"NTFS"};
    const u32 max_comp =
        has_volume ? volume.max_component_length : 255u;
    const u32 fs_flags = has_volume
        ? volume.filesystem_flags
        : (kCaseSensitiveSearch | kCasePreservedNames | kUnicodeOnDisk |
           kPersistentAcls);

    // Name buffers: when a non-zero size is requested the buffer must fit
    // the string plus the terminating NUL (files_ns_probe *_fs_exact /
    // *_label_zero). Size 0 with a non-null buffer is ERROR_BAD_LENGTH.
    auto write_name = [&](GuestAddr buf, u32 buf_chars,
                          const std::string& utf8) -> bool {
        if (buf == 0) return true;
        if (buf_chars == 0) return false;
        // Count UTF-16 units via write_wide_string's contract: ASCII labels
        // and "NTFS"/"CDFS" are BMP-only so char count == byte count.
        const u32 needed = static_cast<u32>(utf8.size()) + 1u;
        if (needed > buf_chars) return false;
        (void)m.write_wide_string(buf, utf8);
        return true;
    };

    if (!write_name(vol_name_buf, vol_name_size, label) ||
        !write_name(fs_name_buf, fs_name_size, filesystem)) {
        set_last_error(error_codes::ERROR_BAD_LENGTH);
        return false;
    }

    u32 volume_serial = 0x12345678u;
    if (auto st = m_vfs->stat(root_path); st.is_ok()) {
        volume_serial = static_cast<u32>(st.value().volume_id);
    }
    if (serial) (void)m.write_u32(serial, volume_serial);
    if (max_comp_len) (void)m.write_u32(max_comp_len, max_comp);
    if (flags) (void)m.write_u32(flags, fs_flags);

    VITREO_TRACE("[sysinfo] GetVolumeInformationW(%s) -> label='%s' fs='%s' "
                 "serial=0x%x flags=0x%x\n",
                 root_path.c_str(), label.c_str(), filesystem.c_str(),
                 volume_serial, fs_flags);
    return true;
}

u32 SysInfoManager::get_drive_type_w(const std::string& path) const {
    constexpr u32 DRIVE_NO_ROOT_DIR = 1;
    constexpr u32 DRIVE_FIXED = 3;
    constexpr u32 DRIVE_CDROM = 5;

    std::string p = path;
    if (p.empty()) {
        // NULL root is handled by the thunk (current drive); empty string
        // from the guest is DRIVE_NO_ROOT_DIR (files_ns_probe).
        return DRIVE_NO_ROOT_DIR;
    }

    const bool drive_only = p.size() == 2 && p[1] == ':';
    const bool has_trailing =
        !p.empty() && (p.back() == '\\' || p.back() == '/');
    if (!drive_only && !has_trailing) {
        return DRIVE_NO_ROOT_DIR;
    }
    if (p.size() < 2 || p[1] != ':') {
        return DRIVE_NO_ROOT_DIR;
    }
    const char letter =
        static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
    if (letter < 'A' || letter > 'Z' || !m_vfs) {
        return DRIVE_NO_ROOT_DIR;
    }
    const auto drives = m_vfs->mounted_drives();
    if (std::find(drives.begin(), drives.end(), letter) == drives.end()) {
        return DRIVE_NO_ROOT_DIR;
    }
    const std::string root = std::string(1, letter) + ":\\";
    auto resolved = m_vfs->resolve_components(root);
    if (resolved.is_ok() && resolved.value().first->is_optical()) {
        VITREO_TRACE("[sysinfo] GetDriveTypeW(%s) -> CDROM\n", path.c_str());
        return DRIVE_CDROM;
    }
    VITREO_TRACE("[sysinfo] GetDriveTypeW(%s) -> FIXED\n", path.c_str());
    return DRIVE_FIXED;
}

u32 SysInfoManager::get_logical_drives() const {
    u32 mask = 0;
    const std::vector<char> drives = m_vfs ? m_vfs->mounted_drives()
                                           : std::vector<char>{'C', 'D'};
    for (char drive : drives) {
        mask |= 1u << static_cast<u32>(drive - 'A');
    }
    VITREO_TRACE("[sysinfo] GetLogicalDrives -> 0x%x\n", mask);
    return mask;
}

u32 SysInfoManager::get_logical_drive_strings_w(u32 buf_len, GuestAddr buffer, Marshaler& m) const {
    const std::vector<char> drives = m_vfs ? m_vfs->mounted_drives()
                                           : std::vector<char>{'C'};
    const u32 required = static_cast<u32>(drives.size() * 4 + 1);
    if (buffer == 0 || buf_len < required) {
        return required;
    }
    GuestAddr cursor = buffer;
    for (char drive : drives) {
        const char16_t data[] = { static_cast<char16_t>(drive), u':', u'\\', u'\0' };
        (void)m.memory().write_bytes(cursor, reinterpret_cast<const Byte*>(data), sizeof(data));
        cursor += sizeof(data);
    }
    (void)m.memory().write_u16(cursor, 0);
    return required - 1;
}

} // namespace vitreo::winapi
