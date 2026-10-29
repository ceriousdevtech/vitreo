// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_mapping.cpp
/// @brief Implementation of memory-mapped files (WIN32-066)

#include "file_mapping.hpp"
#include "file_mapping_internal.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/named_object_directory.hpp"
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

/// @file flushviewoffile.cpp
/// @brief FlushViewOfFile
///
/// Closed cut from file_mapping.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// FlushViewOfFile
// =============================================================================

bool FileMappingManager::flush_view_of_file(GuestAddr base_address,
                                             u64 num_bytes) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    // Learn: lpBaseAddress may be any address inside a mapped view.
    auto vit = m_views.find(base_address);
    if (vit == m_views.end()) {
        for (auto it = m_views.begin(); it != m_views.end(); ++it) {
            const u64 extent = it->second.guest_mapped_size != 0
                ? static_cast<u64>(it->second.guest_mapped_size)
                : it->second.size;
            if (base_address >= it->first &&
                base_address < it->first + extent) {
                vit = it;
                break;
            }
        }
    }
    if (vit == m_views.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    MappedView& view = vit->second;
    const u64 offset_in_view = base_address - vit->first;
    // FILE_MAP_COPY: nothing durable to push.
    if (!view.mapping || view.copy_on_write) {
        return true;
    }
    FileMappingObject* obj = view.mapping.get();

    if (!obj->backing_data.empty()) {
        usize copy_len = std::min(static_cast<usize>(view.size),
                                   obj->backing_data.size() -
                                       static_cast<usize>(view.offset));
        if (num_bytes != 0) {
            copy_len = std::min(copy_len, static_cast<usize>(num_bytes));
        }
        const usize skip = static_cast<usize>(offset_in_view);
        if (skip < copy_len) {
            copy_len -= skip;
            (void)m_memory.read_bytes(
                base_address,
                reinterpret_cast<Byte*>(
                    obj->backing_data.data() + view.offset + skip),
                copy_len);
        }
    } else if (view.aliases_section && obj->file_mmap != nullptr &&
               obj->file_mmap_len != 0 && obj->writable) {
        usize flush_len = obj->file_mmap_len;
        if (num_bytes != 0) {
            flush_len = std::min(flush_len, static_cast<usize>(num_bytes));
        }
        const usize page = static_cast<usize>(::getpagesize());
        const usize mmap_off =
            static_cast<usize>(view.offset + offset_in_view);
        flush_len = (flush_len + page - 1) & ~(page - 1);
        if (mmap_off < obj->file_mmap_len) {
            if (mmap_off + flush_len > obj->file_mmap_len) {
                flush_len = obj->file_mmap_len - mmap_off;
            }
            (void)::msync(static_cast<Byte*>(obj->file_mmap) + mmap_off,
                          flush_len, MS_SYNC);
        }
    }

    if (view.shared_host_ptr != nullptr && view.shared_host_len > 0) {
        usize flush_len = view.shared_host_len;
        if (num_bytes != 0) {
            flush_len = std::min(flush_len, static_cast<usize>(num_bytes));
        }
        const usize page = static_cast<usize>(::getpagesize());
        const usize host_off = static_cast<usize>(offset_in_view);
        flush_len = (flush_len + page - 1) & ~(page - 1);
        if (host_off < view.shared_host_len) {
            if (host_off + flush_len > view.shared_host_len) {
                flush_len = view.shared_host_len - host_off;
            }
            if (::msync(static_cast<Byte*>(view.shared_host_ptr) + host_off,
                        flush_len, MS_SYNC) != 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                return false;
            }
        }
    }

    if (obj->writable && obj->file_handle != FILE_MAPPING_INVALID_FILE &&
        m_file_mgr) {
        write_back_to_file(*obj);
    }
    return true;
}

bool FileMappingManager::on_handle_close(Handle handle, HandleType type) noexcept {
    if (type != HandleType::FileMapping) return false;
    std::shared_ptr<FileMappingObject> obj;
    bool last_named_reference = false;
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        auto it = m_mappings.find(handle);
        if (it == m_mappings.end()) return false;
        obj = std::move(it->second);
        m_mappings.erase(it);
        if (obj && !obj->name.empty() && obj.use_count() == 1) {
            m_named_mappings.erase(obj->name);
            last_named_reference = true;
        }
    }
    if (last_named_reference) {
        named_object_unregister(obj->name, NamedObjectKind::FileMapping);
    }
    return true;
}

bool FileMappingManager::blocks_shrink_end_of_file(Handle file_handle,
                                                   u64 new_size) const {
    if (file_handle == 0 || file_handle == FILE_MAPPING_INVALID_FILE) {
        return false;
    }
    std::string path;
    if (m_file_mgr) {
        path = source_path(file_handle);
    }
    auto references = [&](const std::shared_ptr<FileMappingObject>& obj) {
        if (!obj || obj->file_handle == FILE_MAPPING_INVALID_FILE) {
            return false;
        }
        if (obj->file_handle == file_handle) return true;
        return !path.empty() && obj->source_path == path;
    };
    auto blocks = [&](const std::shared_ptr<FileMappingObject>& obj) {
        return references(obj) && new_size < obj->size;
    };

    std::lock_guard<std::recursive_mutex> lock(m_views_mutex);
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        for (const auto& [h, obj] : m_mappings) {
            (void)h;
            if (blocks(obj)) return true;
        }
    }
    for (const auto& [addr, view] : m_views) {
        (void)addr;
        if (blocks(view.mapping)) return true;
    }
    return false;
}

} // namespace vitreo::winapi
