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

/// @file ntsetinformationfile.cpp
/// @brief NtSetInformationFile
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtSetInformationFile
// =============================================================================

// FILE_INFORMATION_CLASS values we handle
static constexpr u32 FileEndOfFileInformation    = 20;
static constexpr u32 FileAllocationInformation    = 19;
static constexpr u32 FileDispositionInformation   = 13;
static constexpr u32 FilePositionInformation      = 14;

u32 NtdllExtManager::nt_set_information_file(Handle handle, u32 info_class,
                                              const u8* info_buffer, u32 info_length) {
    if (!has_file_support()) {
        return nt_status::STATUS_NOT_IMPLEMENTED;
    }

    const auto mapped_handle = vfs_handle_for(handle);
    if (!mapped_handle.has_value()) {
        return nt_status::STATUS_INVALID_HANDLE;
    }
    const capsule::FileHandle vfs_handle = *mapped_handle;

    switch (info_class) {
        case FileEndOfFileInformation: {
            // EndOfFileInformation: single LARGE_INTEGER (8 bytes) = new file size
            if (info_length < 8 || !info_buffer) {
                return nt_status::STATUS_INVALID_PARAMETER;
            }
            u64 new_size = 0;
            std::memcpy(&new_size, info_buffer, 8);

            auto trunc_result = m_vfs->truncate(vfs_handle, new_size);
            if (trunc_result.is_err()) {
                return nt_status::STATUS_ACCESS_DENIED;
            }
            VITREO_TRACE("[ntdll] NtSetInformationFile(EndOfFile): handle=0x%x size=%llu\n",
                        handle, static_cast<unsigned long long>(new_size));
            return nt_status::STATUS_SUCCESS;
        }

        case FileAllocationInformation: {
            // AllocationInformation: LARGE_INTEGER (8 bytes) = allocation size
            // On real Windows this pre-allocates disk space. We just truncate/extend.
            if (info_length < 8 || !info_buffer) {
                return nt_status::STATUS_INVALID_PARAMETER;
            }
            u64 alloc_size = 0;
            std::memcpy(&alloc_size, info_buffer, 8);

            auto trunc_result = m_vfs->truncate(vfs_handle, alloc_size);
            if (trunc_result.is_err()) {
                return nt_status::STATUS_ACCESS_DENIED;
            }
            VITREO_TRACE("[ntdll] NtSetInformationFile(Allocation): handle=0x%x size=%llu\n",
                        handle, static_cast<unsigned long long>(alloc_size));
            return nt_status::STATUS_SUCCESS;
        }

        case FilePositionInformation: {
            // PositionInformation: LARGE_INTEGER (8 bytes) = new file position
            if (info_length < 8 || !info_buffer) {
                return nt_status::STATUS_INVALID_PARAMETER;
            }
            i64 new_pos = 0;
            std::memcpy(&new_pos, info_buffer, 8);

            auto seek_result = m_vfs->seek(vfs_handle, new_pos, capsule::SeekOrigin::Begin);
            if (seek_result.is_err()) {
                return nt_status::STATUS_ACCESS_DENIED;
            }
            return nt_status::STATUS_SUCCESS;
        }

        case FileDispositionInformation: {
            // FILE_DISPOSITION_INFORMATION: BOOLEAN DeleteFile (1 byte).
            // Recording the disposition is the whole of the operation — the
            // object is removed when its last handle closes. Reporting success
            // without arming it leaves a caller that re-checks the path
            // spinning against an object that never disappears.
            if (info_length < 1 || info_buffer == nullptr) {
                return nt_status::STATUS_INVALID_PARAMETER;
            }
            const bool delete_on_close = info_buffer[0] != 0;
            // NtCreateFile handles live in this manager; a HANDLE that came
            // from kernel32 CreateFile is equally legal here and belongs to
            // FileManager. Try the owner of this handle, then the other.
            if (!set_delete_disposition(handle, delete_on_close) &&
                (m_file_manager == nullptr ||
                 !m_file_manager->set_delete_disposition(handle,
                                                         delete_on_close))) {
                return nt_status::STATUS_INVALID_HANDLE;
            }
            VITREO_TRACE("[ntdll] NtSetInformationFile(Disposition): "
                         "handle=0x%x delete=%d\n",
                         handle, delete_on_close ? 1 : 0);
            return nt_status::STATUS_SUCCESS;
        }

        default:
            VITREO_TRACE("[ntdll] NtSetInformationFile: unhandled class %u\n", info_class);
            return nt_status::STATUS_NOT_IMPLEMENTED;
    }
}

} // namespace vitreo::winapi
