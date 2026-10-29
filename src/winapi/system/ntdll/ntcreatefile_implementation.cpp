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

/// @file ntcreatefile_implementation.cpp
/// @brief NtCreateFile Implementation
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtCreateFile Implementation
// =============================================================================

NtdllExtManager::CreateFileResult NtdllExtManager::nt_create_file(
    std::string_view path,
    u32 desired_access,
    u32 file_attributes,
    u32 share_access,
    u32 create_disposition,
    u32 create_options)
{
    (void)file_attributes; // Currently ignored

    CreateFileResult result{};
    result.status = nt_status::STATUS_SUCCESS;
    result.handle = INVALID_HANDLE;
    result.information = nt_file::FILE_DOES_NOT_EXIST;

    // Check if VFS/HandleTable are available
    if (!has_file_support()) {
        VITREO_TRACE("[ntdll] NtCreateFile: no VFS/HandleTable configured\n");
        result.status = nt_status::STATUS_NOT_IMPLEMENTED;
        return result;
    }

    // Convert NT parameters to VFS enums
    auto access = to_access_mode(desired_access);
    auto share  = to_share_mode(share_access);
    auto disp   = to_disposition(create_disposition);

    // Check if requesting directory vs file
    bool want_directory = (create_options & nt_file::FILE_DIRECTORY_FILE) != 0;
    bool want_non_directory = (create_options & nt_file::FILE_NON_DIRECTORY_FILE) != 0;
    const bool synchronous =
        (create_options & (nt_file::FILE_SYNCHRONOUS_IO_ALERT |
                           nt_file::FILE_SYNCHRONOUS_IO_NONALERT)) != 0;

    // For directory requests, we might need special handling
    if (want_directory) {
        // Try to open as directory or create directory
        auto stat_result = m_vfs->stat(path);
        if (stat_result.is_ok()) {
            // Path exists - check if it's a directory
            auto info = stat_result.value();
            if (!capsule::has_attribute(info.attributes, capsule::FileAttributes::Directory)) {
                result.status = nt_status::STATUS_OBJECT_NAME_NOT_FOUND; // Not a directory
                return result;
            }
            // A directory handle is a real VFS open object. Directory queries
            // are handle-relative and must not fall back to a remembered path.
            auto open_result = m_vfs->open(
                path, access, share, capsule::CreationDisposition::OpenExisting);
            if (open_result.is_err()) {
                result.status = vfs_error_to_ntstatus(open_result.error());
                return result;
            }
            Handle win_handle = m_handles->allocate(HandleType::File, nullptr);
            if (win_handle == NULL_HANDLE || win_handle == INVALID_HANDLE) {
                (void)m_vfs->close(open_result.value());
                result.status = nt_status::STATUS_NO_MEMORY;
                return result;
            }
            {
                std::lock_guard<std::mutex> lock(m_file_state_mutex);
                m_vfs_handles[win_handle] = open_result.value();
                if (synchronous) m_synchronous_file_handles.insert(win_handle);
                // Retain the name so a pending-delete disposition can be
                // carried out at close; the VFS deletes by path.
                m_handle_paths[win_handle] = std::string(path);
                if ((create_options & nt_file::FILE_DELETE_ON_CLOSE) != 0) {
                    m_delete_pending.insert(win_handle);
                }
            }
            result.handle = win_handle;
            result.information = nt_file::FILE_OPENED;
            result.status = nt_status::STATUS_SUCCESS;
            VITREO_TRACE("[ntdll] NtCreateFile: opened directory '%.*s' -> handle=0x%x\n",
                        static_cast<int>(path.size()), path.data(), win_handle);
            return result;
        } else if (create_disposition == nt_file::FILE_CREATE ||
                   create_disposition == nt_file::FILE_OPEN_IF) {
            // Create directory
            auto create_result = m_vfs->create_dir(path);
            if (create_result.is_err()) {
                result.status = vfs_error_to_ntstatus(create_result.error());
                return result;
            }
            auto open_result = m_vfs->open(
                path, access, share, capsule::CreationDisposition::OpenExisting);
            if (open_result.is_err()) {
                result.status = vfs_error_to_ntstatus(open_result.error());
                return result;
            }
            Handle win_handle = m_handles->allocate(HandleType::File, nullptr);
            if (win_handle == NULL_HANDLE || win_handle == INVALID_HANDLE) {
                (void)m_vfs->close(open_result.value());
                result.status = nt_status::STATUS_NO_MEMORY;
                return result;
            }
            {
                std::lock_guard<std::mutex> lock(m_file_state_mutex);
                m_vfs_handles[win_handle] = open_result.value();
                if (synchronous) m_synchronous_file_handles.insert(win_handle);
                // Retain the name so a pending-delete disposition can be
                // carried out at close; the VFS deletes by path.
                m_handle_paths[win_handle] = std::string(path);
                if ((create_options & nt_file::FILE_DELETE_ON_CLOSE) != 0) {
                    m_delete_pending.insert(win_handle);
                }
            }
            result.handle = win_handle;
            result.information = nt_file::FILE_CREATED;
            result.status = nt_status::STATUS_SUCCESS;
            VITREO_TRACE("[ntdll] NtCreateFile: created directory '%.*s' -> handle=0x%x\n",
                        static_cast<int>(path.size()), path.data(), win_handle);
            return result;
        } else {
            result.status = nt_status::STATUS_OBJECT_PATH_NOT_FOUND;
            return result;
        }
    }

    // Regular file operation
    // Check if file exists (needed for setting Information field correctly)
    bool file_existed = false;
    {
        auto stat_result = m_vfs->stat(path);
        file_existed = stat_result.is_ok();

        // If want_non_directory and it's a directory, fail
        if (file_existed && want_non_directory) {
            auto info = stat_result.value();
            if (capsule::has_attribute(info.attributes, capsule::FileAttributes::Directory)) {
                result.status = nt_status::STATUS_OBJECT_NAME_NOT_FOUND;
                return result;
            }
        }
    }

    // Open/create via VFS
    auto vfs_result = m_vfs->open(path, access, share, disp);
    if (vfs_result.is_err()) {
        result.status = vfs_error_to_ntstatus(vfs_result.error());
        VITREO_TRACE("[ntdll] NtCreateFile: vfs.open('%.*s') failed: %s\n",
                    static_cast<int>(path.size()), path.data(),
                    capsule::vfs_error_to_string(vfs_result.error()));
        return result;
    }

    capsule::FileHandle vfs_handle = vfs_result.value();

    // Allocate a Win32 handle pointing at the VFS file handle. The handle
    // carries its DesiredAccess as CreateFileW's do: section creation checks
    // it (FILE_EXECUTE for an executable image section, measured by
    // tools/oracle_probe/nt_create_section_probe image_rx_rd).
    constexpr u32 kMaximumAllowed = 0x02000000u;
    Handle win_handle = m_handles->allocate(
        HandleType::File, nullptr, 0,
        (desired_access & kMaximumAllowed) != 0 ? HandleAccess::All : desired_access);
    if (win_handle == NULL_HANDLE || win_handle == INVALID_HANDLE) {
        // Roll back VFS open
        (void)m_vfs->close(vfs_handle);
        result.status = nt_status::STATUS_NO_MEMORY;
        return result;
    }

    // Track the mapping for later cleanup
    {
        std::lock_guard<std::mutex> lock(m_file_state_mutex);
        m_vfs_handles[win_handle] = vfs_handle;
        if (synchronous) m_synchronous_file_handles.insert(win_handle);
        // Retain the name so a pending-delete disposition can be carried out
        // at close; the VFS deletes by path.
        m_handle_paths[win_handle] = std::string(path);
        if ((create_options & nt_file::FILE_DELETE_ON_CLOSE) != 0) {
            m_delete_pending.insert(win_handle);
        }
    }

    debug::write_sha1_open(win_handle, path);
    debug::read_sha1_open(win_handle, path);
    debug::watch_open(win_handle, path);

    result.handle = win_handle;
    result.status = nt_status::STATUS_SUCCESS;

    // Set Information field based on what happened
    switch (create_disposition) {
        case nt_file::FILE_SUPERSEDE:
            result.information = file_existed ? nt_file::FILE_SUPERSEDED : nt_file::FILE_CREATED;
            break;
        case nt_file::FILE_OPEN:
            result.information = nt_file::FILE_OPENED;
            break;
        case nt_file::FILE_CREATE:
            result.information = nt_file::FILE_CREATED;
            break;
        case nt_file::FILE_OPEN_IF:
            result.information = file_existed ? nt_file::FILE_OPENED : nt_file::FILE_CREATED;
            break;
        case nt_file::FILE_OVERWRITE:
            result.information = nt_file::FILE_OVERWRITTEN;
            break;
        case nt_file::FILE_OVERWRITE_IF:
            result.information = file_existed ? nt_file::FILE_OVERWRITTEN : nt_file::FILE_CREATED;
            break;
        default:
            result.information = nt_file::FILE_OPENED;
            break;
    }

    VITREO_TRACE("[ntdll] NtCreateFile: '%.*s' access=0x%x disp=%u -> handle=0x%x info=%u\n",
                static_cast<int>(path.size()), path.data(),
                desired_access, create_disposition, win_handle, result.information);

    return result;
}

} // namespace vitreo::winapi
