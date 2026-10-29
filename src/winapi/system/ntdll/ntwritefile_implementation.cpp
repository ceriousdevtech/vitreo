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

/// @file ntwritefile_implementation.cpp
/// @brief NtWriteFile Implementation
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtWriteFile Implementation
// =============================================================================

std::optional<u32> NtdllExtManager::vfs_handle_for(Handle handle) const {
    {
        std::lock_guard<std::mutex> lock(m_file_state_mutex);
        auto it = m_vfs_handles.find(handle);
        if (it != m_vfs_handles.end()) return it->second;
    }
    if (m_file_manager != nullptr) {
        const capsule::FileHandle win32 = m_file_manager->vfs_handle_for(handle);
        if (win32 != capsule::INVALID_FILE_HANDLE) return win32;
    }
    return std::nullopt;
}

u32 NtdllExtManager::nt_write_file(
    Handle handle,
    std::span<const u8> buffer,
    i64 byte_offset,
    u32& bytes_written)
{
    bytes_written = 0;

    if (!has_file_support()) {
        VITREO_TRACE("[ntdll] NtWriteFile: no VFS/HandleTable configured\n");
        return nt_status::STATUS_NOT_IMPLEMENTED;
    }

    // Check handle type
    auto type = m_handles->get_type(handle);
    if (type == HandleType::Invalid) {
        return nt_status::STATUS_INVALID_HANDLE;
    }

    // Look up VFS handle
    const auto mapped_handle = vfs_handle_for(handle);
    if (!mapped_handle.has_value()) {
        return nt_status::STATUS_INVALID_HANDLE;
    }
    const capsule::FileHandle vfs_handle = *mapped_handle;

    // Explicit offset: seek-and-write. FILE_WRITE_TO_END_OF_FILE: at EOF.
    // FILE_USE_FILE_POINTER_POSITION / NULL: the current position.
    if (byte_offset >= 0) {
        auto seek_result = m_vfs->seek(vfs_handle, byte_offset, capsule::SeekOrigin::Begin);
        if (seek_result.is_err()) {
            return nt_status::STATUS_ACCESS_DENIED;
        }
    } else if (byte_offset == kWriteAtEndOfFile) {
        auto seek_result = m_vfs->seek(vfs_handle, 0, capsule::SeekOrigin::End);
        if (seek_result.is_err()) {
            return nt_status::STATUS_ACCESS_DENIED;
        }
    }

    // Write to VFS
    auto result = m_vfs->write(vfs_handle, buffer);
    if (result.is_err()) {
        return vfs_error_to_ntstatus(result.error());
    }

    bytes_written = static_cast<u32>(result.value());
    debug::write_sha1_update(handle, buffer.data(), bytes_written);
    return nt_status::STATUS_SUCCESS;
}

} // namespace vitreo::winapi
