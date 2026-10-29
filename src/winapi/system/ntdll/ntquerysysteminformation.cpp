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

/// @file ntquerysysteminformation.cpp
/// @brief NtQuerySystemInformation
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtQuerySystemInformation
// =============================================================================

u32 NtdllExtManager::query_system_info(u32 info_class, void* buffer,
                                        u32 length, u32* return_length) const {
    switch (info_class) {
        case 0: {
            // SystemBasicInformation
            constexpr u32 REQUIRED = sizeof(SystemBasicInfo);
            if (return_length != nullptr) {
                *return_length = REQUIRED;
            }
            if (length < REQUIRED) {
                return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            }
            if (buffer == nullptr) {
                return nt_status::STATUS_INVALID_PARAMETER;
            }

            auto* info          = static_cast<SystemBasicInfo*>(buffer);
            info->reserved      = 0;
            info->timer_resolution           = 156250;   // ~15.6 ms in 100ns
            info->page_size                  = 4096;
            info->number_of_physical_pages   = 2097152;  // ~8 GB
            info->lowest_physical_page_number  = 1;
            info->highest_physical_page_number = 2097152;
            info->allocation_granularity     = 65536;
            info->minimum_user_mode_address  = 0x10000;
            info->maximum_user_mode_address  = 0x7FFEFFFF;
            info->active_processors_affinity_mask = 0xF; // 4 cores
            info->number_of_processors       = 4;

            return nt_status::STATUS_SUCCESS;
        }
        case 1: {
            // SystemProcessorInformation (simplified)
            struct ProcessorInfo {
                u16 processor_architecture;
                u16 processor_level;
                u16 processor_revision;
                u16 maximum_processors;
                u32 processor_feature_bits;
            };
            constexpr u32 REQUIRED = sizeof(ProcessorInfo);
            if (return_length != nullptr) {
                *return_length = REQUIRED;
            }
            if (length < REQUIRED) {
                return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            }
            if (buffer == nullptr) {
                return nt_status::STATUS_INVALID_PARAMETER;
            }

            auto* info = static_cast<ProcessorInfo*>(buffer);
            info->processor_architecture = 9;  // PROCESSOR_ARCHITECTURE_AMD64
            info->processor_level        = 6;
            info->processor_revision     = 0x5E03;
            info->maximum_processors     = 64;
            info->processor_feature_bits = 0;

            return nt_status::STATUS_SUCCESS;
        }
        default:
            if (return_length != nullptr) {
                *return_length = 0;
            }
            return nt_status::STATUS_INVALID_INFO_CLASS;
    }
}

} // namespace vitreo::winapi
