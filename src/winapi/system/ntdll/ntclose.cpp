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

/// @file ntclose.cpp
/// @brief NtClose
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtClose
// =============================================================================

u32 NtdllExtManager::nt_close(u64 handle) {
    VITREO_TRACE("[ntdll] NtClose(handle=0x%llx)\n",
                 static_cast<unsigned long long>(handle));

    if (handle == 0 || handle == 0xFFFFFFFF || handle == 0xFFFFFFFFFFFFFFFFULL) {
        return nt_status::STATUS_INVALID_HANDLE;
    }

    // If we have a handle table, try to close the handle
    if (m_handles != nullptr) {
        const Handle guest_handle = static_cast<Handle>(handle & 0xFFFFFFFF);
        bool ok = m_handles->close(guest_handle);
        if (!ok) {
            // Handle wasn't in our table — may belong to another subsystem.
            // Return success anyway since many apps close pseudo-handles.
            VITREO_TRACE("[ntdll] NtClose: handle 0x%llx not in table, returning success\n",
                         static_cast<unsigned long long>(handle));
        }
        // Unit configurations do not always install RuntimeContext's close
        // callback. Clean up an NT-owned VFS object here if the callback did
        // not already do so.
        if (owns_file_handle(guest_handle)) {
            (void)on_handle_close(guest_handle, HandleType::File);
        }
    }

    return nt_status::STATUS_SUCCESS;
}

bool NtdllExtManager::set_delete_disposition(Handle handle,
                                             bool delete_on_close) {
    std::lock_guard<std::mutex> lock(m_file_state_mutex);
    if (m_handle_paths.find(handle) == m_handle_paths.end()) return false;
    if (delete_on_close) {
        m_delete_pending.insert(handle);
    } else {
        m_delete_pending.erase(handle);
    }
    return true;
}

bool NtdllExtManager::on_handle_close(Handle handle, HandleType type) noexcept {
    if (type != HandleType::File) return false;
    capsule::FileHandle vfs_handle = capsule::INVALID_FILE_HANDLE;
    std::string pending_delete_path;
    {
        std::lock_guard<std::mutex> lock(m_file_state_mutex);
        auto found = m_vfs_handles.find(handle);
        if (found == m_vfs_handles.end()) return false;
        vfs_handle = found->second;
        m_vfs_handles.erase(found);
        m_synchronous_file_handles.erase(handle);
        m_directory_queries.erase(handle);
        auto path_it = m_handle_paths.find(handle);
        if (m_delete_pending.erase(handle) != 0 &&
            path_it != m_handle_paths.end()) {
            pending_delete_path = path_it->second;
        }
        if (path_it != m_handle_paths.end()) m_handle_paths.erase(path_it);
    }
    if (m_vfs != nullptr) {
        (void)m_vfs->close(vfs_handle);
        // Carry out the pending delete only after the descriptor is released:
        // the host refuses to unlink a directory that is still open. Vfs::remove
        // covers files and empty directories, matching the Windows rule that a
        // non-empty directory survives.
        if (!pending_delete_path.empty()) {
            auto removed = m_vfs->remove(pending_delete_path);
            VITREO_TRACE("[ntdll] delete-on-close \"%s\" -> %s\n",
                         pending_delete_path.c_str(),
                         removed.is_ok() ? "removed" : "not removed");
        }
    }
    VITREO_TRACE("[ntdll] closed NT file handle=0x%x vfs=%u\n",
                 handle, vfs_handle);
    return true;
}

bool NtdllExtManager::owns_file_handle(Handle handle) const noexcept {
    std::lock_guard<std::mutex> lock(m_file_state_mutex);
    return m_vfs_handles.contains(handle);
}

void NtdllExtManager::forget_directory_query(Handle handle) noexcept {
    std::lock_guard<std::mutex> lock(m_file_state_mutex);
    m_directory_queries.erase(handle);
}

} // namespace vitreo::winapi
