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

/// @file unmapviewoffile.cpp
/// @brief UnmapViewOfFile
///
/// Closed cut from file_mapping.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// UnmapViewOfFile
// =============================================================================

bool FileMappingManager::unmap_view_of_file(GuestAddr base_address) {
    ipc::ipc_log("UnmapViewOfFile view=0x%llx", static_cast<unsigned long long>(base_address));
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    auto it = m_views.find(base_address);
    if (it == m_views.end()) {
        set_last_error(error_codes::ERROR_INVALID_ADDRESS);
        return false;
    }

    MappedView& view = it->second;
    if (view.mapping && !view.copy_on_write) {
        FileMappingObject* obj = view.mapping.get();
        if (view.aliases_section) {
            if (obj->writable && obj->file_mmap != nullptr &&
                obj->file_mmap_len != 0) {
                (void)::msync(obj->file_mmap, obj->file_mmap_len, MS_SYNC);
            }
        } else if (!obj->backing_data.empty()) {
            // Read back view data into backing store (shared section state).
            // FILE_MAP_COPY views must not do this — Learn: private pages are
            // lost on UnmapViewOfFile and never written to the original file.
            usize copy_len = std::min(static_cast<usize>(view.size),
                                       obj->backing_data.size() -
                                           static_cast<usize>(view.offset));
            auto rd = m_memory.read_bytes(
                base_address,
                reinterpret_cast<Byte*>(
                    obj->backing_data.data() + view.offset),
                copy_len);
            (void)rd;

            // Write back to file for file-backed PAGE_READWRITE mappings
            if (obj->writable && obj->file_handle != FILE_MAPPING_INVALID_FILE
                && m_file_mgr) {
                write_back_to_file(*obj);
            }
        }
    }

    // Release exactly the region the view created. A SEC_IMAGE view spans
    // SizeOfImage (SectionMapper's one contiguous region), not the file size
    // in view.size; unmapping by the file size missed the region, so the
    // image stayed mapped and a later MapViewOfFileEx at its base failed
    // with ERROR_INVALID_ADDRESS.
    usize aligned = view.sec_image
        ? static_cast<usize>(view.guest_mapped_size)
        : align_up(static_cast<usize>(view.size), host_mapping_page_size());
    void* shared_host = view.shared_host_ptr;
    usize shared_len = view.shared_host_len;
    if (file_mapping_diag_enabled()) {
        std::fprintf(
            stderr,
            "[file_mapping] UnmapViewOfFile base=0x%llx size=0x%llx "
            "aligned=0x%zx\n",
            static_cast<unsigned long long>(base_address),
            static_cast<unsigned long long>(view.size),
            aligned);
    }

    (void)m_memory.unmap_region(base_address, aligned);

    // For a shared view, GuestMemory held a borrowed (non-owning) region, so
    // unmap_region did not munmap the shm view — release it here.
    if (shared_host != nullptr) {
        ipc::SharedMemory::unmap_view(shared_host, shared_len);
    }

    m_views.erase(it);
    return true;
}

bool FileMappingManager::unmap_view_of_file2(
    GuestAddr base_address, u32 unmap_flags,
    VirtualMemoryManager& virtual_memory) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    constexpr u32 MEM_UNMAP_WITH_TRANSIENT_BOOST = 0x00000001;
    if (unmap_flags != 0 &&
        unmap_flags != MEM_UNMAP_WITH_TRANSIENT_BOOST &&
        unmap_flags != MEM_PRESERVE_PLACEHOLDER) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    auto it = m_views.find(base_address);
    if (it == m_views.end()) {
        set_last_error(error_codes::ERROR_INVALID_ADDRESS);
        return false;
    }

    PlaceholderToken placeholder{};
    if (unmap_flags == MEM_PRESERVE_PLACEHOLDER) {
        if (it->second.placeholder_group == 0) {
            set_last_error(error_codes::ERROR_INVALID_ADDRESS);
            return false;
        }
        placeholder = PlaceholderToken{
            base_address, it->second.size, it->second.placeholder_group};
    }
    if (!unmap_view_of_file(base_address)) {
        return false;
    }
    if (placeholder.group != 0 &&
        !virtual_memory.restore_placeholder(placeholder)) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    VITREO_TRACE("[file_mapping] UnmapViewOfFile2 base=0x%llx flags=0x%x"
                 " preserve=%d\n",
                 static_cast<unsigned long long>(base_address), unmap_flags,
                 placeholder.group != 0 ? 1 : 0);
    ipc::ipc_log(
        "UnmapViewOfFile2 base=0x%llx flags=0x%x preserve=%d -> TRUE",
        static_cast<unsigned long long>(base_address), unmap_flags,
        placeholder.group != 0 ? 1 : 0);
    return true;
}

} // namespace vitreo::winapi
