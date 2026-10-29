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

/// @file ntreadfile_implementation.cpp
/// @brief NtReadFile Implementation
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtReadFile Implementation
// =============================================================================

u32 NtdllExtManager::nt_read_file(
    Handle handle,
    std::span<u8> buffer,
    i64 byte_offset,
    u32& bytes_read)
{
    bytes_read = 0;

    if (!has_file_support()) {
        VITREO_TRACE("[ntdll] NtReadFile: no VFS/HandleTable configured\n");
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

    // Seek to offset if specified (byte_offset >= 0)
    if (byte_offset >= 0) {
        auto seek_result = m_vfs->seek(vfs_handle, byte_offset, capsule::SeekOrigin::Begin);
        if (seek_result.is_err()) {
            return nt_status::STATUS_ACCESS_DENIED;
        }
    }

    // Read from VFS
    auto result = m_vfs->read(vfs_handle, buffer);
    if (result.is_err()) {
        return vfs_error_to_ntstatus(result.error());
    }

    bytes_read = static_cast<u32>(result.value());
    debug::read_sha1_update(handle, buffer.data(), bytes_read);

    // Check for end of file
    if (bytes_read == 0 && !buffer.empty()) {
        return nt_status::STATUS_END_OF_FILE;
    }

    return nt_status::STATUS_SUCCESS;
}

} // namespace vitreo::winapi
