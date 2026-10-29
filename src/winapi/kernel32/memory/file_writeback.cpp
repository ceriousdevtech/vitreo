// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_mapping.cpp
/// @brief Implementation of memory-mapped files (WIN32-066)

#include "file_mapping.hpp"
#include "file_mapping_internal.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/error.hpp"
#include "memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/shared_memory.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

/// @file file_writeback.cpp
/// @brief File Writeback
///
/// Closed cut from file_mapping.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// File Writeback
// =============================================================================

void FileMappingManager::write_back_to_file(FileMappingObject& obj) {
    if (obj.file_mmap != nullptr && obj.file_mmap_len != 0) {
        (void)::msync(obj.file_mmap, obj.file_mmap_len, MS_SYNC);
        return;
    }

    if (!m_file_mgr || obj.file_handle == FILE_MAPPING_INVALID_FILE)
        return;

    capsule::FileHandle vfs_h = source_vfs_handle(obj.file_handle);
    if (vfs_h == capsule::INVALID_FILE_HANDLE)
        return;

    capsule::Vfs& vfs = m_file_mgr->vfs();
    auto cur = vfs.seek(vfs_h, 0, capsule::SeekOrigin::Current);
    (void)vfs.seek(vfs_h, 0, capsule::SeekOrigin::Begin);

    auto wr = vfs.write(vfs_h,
        std::span<const u8>(obj.backing_data.data(), obj.backing_data.size()));

    if (cur.is_ok()) {
        (void)vfs.seek(vfs_h, static_cast<i64>(cur.value()),
                       capsule::SeekOrigin::Begin);
    }

    if (wr.is_ok()) {
        if (file_mapping_diag_enabled()) {
            fprintf(stderr, "[file_mapping] Wrote back %zu bytes to file handle %u\n",
                    wr.value(), static_cast<unsigned>(obj.file_handle));
        }
    }
}

} // namespace vitreo::winapi
