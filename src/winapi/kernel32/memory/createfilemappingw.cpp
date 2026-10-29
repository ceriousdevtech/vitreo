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
#include "core/pe/parser/pe_parser.hpp"

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

/// @file createfilemappingw.cpp
/// @brief CreateFileMappingW
///
/// Closed cut from file_mapping.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// CreateFileMappingW
// =============================================================================

capsule::FileHandle FileMappingManager::source_vfs_handle(Handle h_file) const {
    if (m_file_mgr != nullptr) {
        const capsule::FileHandle h = m_file_mgr->vfs_handle_for(h_file);
        if (h != capsule::INVALID_FILE_HANDLE) return h;
    }
    if (m_foreign_files) {
        if (auto foreign = m_foreign_files(h_file)) return foreign->vfs;
    }
    return capsule::INVALID_FILE_HANDLE;
}

std::string FileMappingManager::source_path(Handle h_file) const {
    if (m_file_mgr != nullptr) {
        std::string path = m_file_mgr->path_for_handle(h_file);
        if (!path.empty()) return path;
    }
    if (m_foreign_files) {
        if (auto foreign = m_foreign_files(h_file)) return foreign->path;
    }
    return {};
}

Handle FileMappingManager::create_file_mapping(Handle h_file, u32 protect,
                                                u64 max_size,
                                                const std::string& name,
                                                bool inheritable,
                                                SectionCreateExtras* extras) {
    ipc::ipc_log("create_file_mapping ENTER name='%s' h_file=0x%x size=%llu prot=0x%x",
                 name.c_str(), static_cast<unsigned>(h_file),
                 static_cast<unsigned long long>(max_size), protect);

    const std::string norm_name = normalize_mapping_name(name);
    // Learn: existing named object → handle to the existing object (current
    // size), GetLastError = ERROR_ALREADY_EXISTS. Do not overwrite.
    if (!norm_name.empty()) {
        if (named_object_kind_conflict(norm_name,
                                       NamedObjectKind::FileMapping)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        bool stale_name = false;
        {
            std::lock_guard<std::mutex> tables(m_mappings_mutex);
            auto it = m_named_mappings.find(norm_name);
            if (it != m_named_mappings.end()) {
                if (auto existing = it->second.lock()) {
                    FileMappingObject* raw = existing.get();
                    Handle h = m_handles.allocate(
                        HandleType::FileMapping, raw,
                        inheritable ? HandleFlags::Inherit : 0);
                    m_mappings[h] = std::move(existing);
                    set_last_error(error_codes::ERROR_ALREADY_EXISTS);
                    return h;
                }
                m_named_mappings.erase(it);
                stale_name = true;
            }
        }
        if (stale_name) {
            named_object_unregister(norm_name, NamedObjectKind::FileMapping);
        }
    }

    // Anonymous mapping with zero size is invalid
    if (max_size == 0 && h_file == FILE_MAPPING_INVALID_FILE) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return NULL_HANDLE;
    }

    // Validate page protection + SEC_* attributes (Learn CreateFileMapping).
    {
        constexpr u32 kSecCommit       = 0x08000000u;
        constexpr u32 kSecReserve      = 0x04000000u;
        constexpr u32 kSecImage        = 0x01000000u;
        constexpr u32 kSecImageNoExec  = 0x11000000u;
        constexpr u32 kSecLargePages   = 0x80000000u;
        constexpr u32 kSecNoCache      = 0x10000000u;
        constexpr u32 kSecWriteCombine = 0x40000000u;
        const u32 page = protect & 0xFFu;
        const u32 attrs = protect & ~0xFFu;
        // PAGE_EXECUTE is an NtCreateSection protection only (Learn lists
        // it for neither CreateFileMapping nor its SEC_* table).
        const bool page_ok =
            (page == PAGE_EXECUTE && extras != nullptr) ||
            page == PAGE_READONLY || page == PAGE_READWRITE ||
            page == PAGE_WRITECOPY || page == PAGE_EXECUTE_READ ||
            page == PAGE_EXECUTE_READWRITE || page == PAGE_EXECUTE_WRITECOPY;
        if (!page_ok) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return NULL_HANDLE;
        }
        if ((attrs & kSecCommit) != 0 && (attrs & kSecReserve) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return NULL_HANDLE;
        }
        // SEC_IMAGE_NO_EXECUTE is SEC_IMAGE | 0x10000000, the SEC_NOCACHE
        // bit; that bit is the image kind there, not a cache attribute.
        const bool image_no_exec = (attrs & kSecImageNoExec) == kSecImageNoExec;
        const u32 cache_attrs =
            attrs & (kSecWriteCombine | (image_no_exec ? 0u : kSecNoCache));
        if (cache_attrs != 0 && (attrs & (kSecCommit | kSecReserve)) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return NULL_HANDLE;
        }
        // Learn: SEC_IMAGE accepts no other SEC_* attributes.
        if ((attrs & (kSecImage | kSecImageNoExec)) != 0 &&
            (attrs & ~(kSecImage | kSecImageNoExec)) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return NULL_HANDLE;
        }
        // Learn: SEC_LARGE_PAGES requires SEC_COMMIT; without SeLockMemoryPrivilege
        // Windows fails ERROR_PRIVILEGE_NOT_HELD (Windows sec_attrs_probe).
        if ((attrs & kSecLargePages) != 0) {
            if ((attrs & kSecCommit) == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                return NULL_HANDLE;
            }
            set_last_error(error_codes::ERROR_PRIVILEGE_NOT_HELD);
            return NULL_HANDLE;
        }
    }

    const u32 sec_attrs = protect & ~0xFFu;
    constexpr u32 kSecReserveFlag = 0x04000000u;
    constexpr u32 kSecImageFlag = 0x01000000u;
    constexpr u32 kSecImageNoExecFlag = 0x11000000u;
    const bool want_reserve = (sec_attrs & kSecReserveFlag) != 0;
    const bool want_image_no_exec =
        (sec_attrs & kSecImageNoExecFlag) == kSecImageNoExecFlag;
    const bool want_image =
        want_image_no_exec || (sec_attrs & kSecImageFlag) != 0;

    // Build the mapping object
    auto obj = std::make_shared<FileMappingObject>();
    obj->protection = protect;
    obj->name       = norm_name;
    obj->writable   = is_writable_protect(protect) && !want_image;
    obj->sec_reserve = want_reserve;
    obj->sec_image = want_image;
    obj->sec_image_no_execute = want_image_no_exec;

    if (h_file == FILE_MAPPING_INVALID_FILE) {
        // Anonymous (page-file-backed) mapping.
        //
        // On Windows, CreateFileMapping(INVALID_HANDLE_VALUE, ...) creates a
        // section object whose pages are shareable via DuplicateHandle from the
        // moment of creation. Vitreo must not keep unnamed anonymous sections as
        // private `backing_data` until the first transfer. Unnamed sections
        // are shared from creation, and peers write into those pages before
        // or around DuplicateHandle. A delayed promote can
        // leave the creator on a private snapshot while the peer writes shared
        // pages the creator never sees.
        if (want_image) {
            // Learn: SEC_IMAGE requires an executable image file handle.
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return NULL_HANDLE;
        }
        obj->file_handle = FILE_MAPPING_INVALID_FILE;
        obj->size = max_size;
        // SEC_RESERVE views commit lazily via VirtualAlloc — do not allocate
        // POSIX shm (or zeroed backing) up front; that would commit host pages.
        if (want_reserve) {
            // size already set; MapView reserves VA only.
        } else {
        // POSIX shm makes every MapViewOfFile of this section alias the same
        // zero-fill-on-demand host pages (Learn coherency). A private zeroed
        // buffer would dirty the whole section up front and could not be
        // shared, so shm failure is an allocation failure.
        static std::atomic<u64> unnamed_counter{1};
        const std::string shm_name = !norm_name.empty()
            ? norm_name
            : ("VitreoAnonymousMapping_" + std::to_string(::getpid()) +
               "_" +
               std::to_string(unnamed_counter.fetch_add(
                   1, std::memory_order_relaxed)));
        obj->shared = ipc::SharedMemory::create_section(
            shm_name, max_size, /*create=*/true, protect);
        if (obj->shared) {
            const auto creator_protection = obj->shared->section_protection();
            if (!creator_protection) {
                static std::atomic_flag warned = ATOMIC_FLAG_INIT;
                if (!warned.test_and_set()) std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED kernel32.dll!CreateFileMappingW: section creator metadata unavailable\n");
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                return NULL_HANDLE;
            }
            obj->protection = *creator_protection;
            obj->writable = is_writable_protect(obj->protection);
            obj->size = obj->shared->size();
            obj->transfer_name = shm_name;
        } else {
            ipc::ipc_log(
                "create_file_mapping FAIL name='%s' size=%llu: shm create",
                norm_name.c_str(), static_cast<unsigned long long>(max_size));
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return NULL_HANDLE;
        }
        }
    } else {
        // File-backed mapping
        obj->file_handle = h_file;
        if (!m_file_mgr || m_handles.get_type(h_file) != HandleType::File) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        obj->source_path = source_path(h_file);
        const capsule::FileHandle source_vfs = source_vfs_handle(h_file);

        // Learn: "The file must be opened with access rights that are
        // compatible with the protection flags." Every protection but
        // PAGE_EXECUTE needs read access; PAGE_READWRITE /
        // PAGE_EXECUTE_READWRITE also need write; PAGE_WRITECOPY needs only
        // read; PAGE_EXECUTE* need execute (or GENERIC_ALL). Measured by
        // nt_create_section_probe: image_wc_rd maps a read-only file,
        // image_x_x a FILE_EXECUTE-only one, and image_ro_x / commit_ro_x
        // (PAGE_READONLY over FILE_EXECUTE) are refused.
        // Fail with ERROR_ACCESS_DENIED — do not silently fall back to a
        // private snapshot that never reaches the file.
        {
            const u32 access = m_handles.get_access(h_file);
            constexpr u32 kGenericRead    = 0x80000000u;
            constexpr u32 kGenericWrite   = 0x40000000u;
            constexpr u32 kGenericExecute = 0x20000000u;
            constexpr u32 kGenericAll     = 0x10000000u;
            constexpr u32 kMaximumAllowed = 0x02000000u;
            constexpr u32 kFileReadData   = 0x00000001u;
            constexpr u32 kFileWriteData  = 0x00000002u;
            constexpr u32 kFileExecute    = 0x00000020u;
            const u32 page = protect & 0xFFu;
            const bool needs_read = page != PAGE_EXECUTE;
            const bool needs_write =
                page == PAGE_READWRITE || page == PAGE_EXECUTE_READWRITE;
            const bool needs_execute =
                page == PAGE_EXECUTE || page == PAGE_EXECUTE_READ ||
                page == PAGE_EXECUTE_READWRITE || page == PAGE_EXECUTE_WRITECOPY;
            const u32 any = kGenericAll | kMaximumAllowed;
            const bool has_read = (access & (kGenericRead | kFileReadData | any)) != 0;
            const bool has_write = (access & (kGenericWrite | kFileWriteData | any)) != 0;
            const bool has_execute = (access & (kGenericExecute | kFileExecute | any)) != 0;
            if ((needs_read && !has_read) || (needs_write && !has_write) ||
                (needs_execute && !has_execute)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                return NULL_HANDLE;
            }
        }

        // Derive size from the file when max_size == 0
        u64 file_size = 0;
        if (source_vfs != capsule::INVALID_FILE_HANDLE) {
            // Do not move the file pointer (GetFileSize contract).
            auto info = m_file_mgr->vfs().stat_handle(source_vfs);
            if (info.is_ok()) file_size = info.value().size;
        }

        if (max_size == 0) {
            if (file_size == 0) {
                // Real Windows fails a whole-file mapping of an empty file
                // with ERROR_FILE_INVALID. Reaching this for a non-empty file
                // means get_file_size failed (handle not in m_vfs_handles?) —
                // log it. Callers treat a mapping failure as fatal.
                ipc::ipc_log("create_file_mapping FAIL h_file=0x%x: file size 0/unknown",
                             static_cast<unsigned>(h_file));
                set_last_error(error_codes::ERROR_FILE_INVALID);
                return NULL_HANDLE;
            }
            obj->size = file_size;
        } else {
            obj->size = max_size;
        }

        // Learn: SEC_RESERVE "has no effect" for file-backed sections.
        if (want_reserve) {
            obj->sec_reserve = false;
        }

        // SEC_IMAGE: capture the PE and use SizeOfImage as the section size.
        // MapViewOfFile lays out headers/sections; raw file_mmap is wrong.
        if (want_image) {
            obj->image_pe_bytes.assign(static_cast<usize>(file_size), 0);
            if (file_size > 0) {
                capsule::FileHandle vfs_h = source_vfs;
                if (vfs_h != capsule::INVALID_FILE_HANDLE) {
                    capsule::Vfs& vfs = m_file_mgr->vfs();
                    auto cur = vfs.seek(vfs_h, 0, capsule::SeekOrigin::Current);
                    (void)vfs.seek(vfs_h, 0, capsule::SeekOrigin::Begin);
                    usize total = 0;
                    while (total < obj->image_pe_bytes.size()) {
                        auto rd = vfs.read(
                            vfs_h,
                            std::span<u8>(obj->image_pe_bytes.data() + total,
                                          obj->image_pe_bytes.size() - total));
                        if (rd.is_err() || rd.value() == 0) break;
                        total += rd.value();
                    }
                    if (cur.is_ok()) {
                        (void)vfs.seek(vfs_h, static_cast<i64>(cur.value()),
                                       capsule::SeekOrigin::Begin);
                    }
                }
            }
            auto parsed = pe::parse_pe(obj->image_pe_bytes);
            if (parsed.is_err() || parsed.value().size_of_image == 0) {
                // NtCreateSection: no "MZ" is STATUS_INVALID_IMAGE_NOT_MZ
                // (nt_create_section_probe image_not_pe); any other
                // malformed image is STATUS_INVALID_IMAGE_FORMAT (Learn).
                if (extras != nullptr) {
                    const bool mz = obj->image_pe_bytes.size() >= 2 &&
                                    obj->image_pe_bytes[0] == 'M' &&
                                    obj->image_pe_bytes[1] == 'Z';
                    extras->image_status = mz ? 0xC000007Bu : 0xC000012Fu;
                }
                set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
                return NULL_HANDLE;
            }
            obj->size = parsed.value().size_of_image;
            obj->image_base = parsed.value().image_base;
            // Skip demand-paged raw mmap — image layout is section-aligned.
        }

        if (file_mapping_diag_enabled()) {
            ipc::ipc_log(
                "create_file_mapping file h=0x%x path='%s' file_size=%llu "
                "map_size=%llu writable=%d",
                static_cast<unsigned>(h_file),
                obj->source_path.c_str(),
                static_cast<unsigned long long>(file_size),
                static_cast<unsigned long long>(obj->size),
                obj->writable ? 1 : 0);
        }

        // Learn CreateFileMappingW: creating the mapping object does not map
        // a view or read the file. File-backed pages are demand-paged when
        // the view is accessed. Prefer mmap of the host FD; fall back to an
        // eager private snapshot when the VFS has no native descriptor.
        const usize page = host_mapping_page_size();
        const usize mmap_len =
            align_up(static_cast<usize>(obj->size), page);
        bool mapped_file = want_image;  // SEC_IMAGE uses image_pe_bytes only
        if (!want_image && mmap_len > 0) {
            capsule::FileHandle vfs_h = source_vfs;
            if (vfs_h != capsule::INVALID_FILE_HANDLE) {
                auto fd_result =
                    m_file_mgr->vfs().duplicate_native_fd(vfs_h);
                if (fd_result.is_ok()) {
                    const int fd = fd_result.value();
                    if (obj->writable && obj->size > file_size) {
                        if (::ftruncate(
                                fd, static_cast<off_t>(obj->size)) != 0) {
                            // Learn: cannot grow the file → ERROR_DISK_FULL.
                            ::close(fd);
                            set_last_error(error_codes::ERROR_DISK_FULL);
                            return NULL_HANDLE;
                        }
                    }
                    if (obj->writable || obj->size <= file_size) {
                        const int prot =
                            PROT_READ | (obj->writable ? PROT_WRITE : 0);
                        const int flags =
                            obj->writable ? MAP_SHARED : MAP_PRIVATE;
                        void* mapped = ::mmap(nullptr, mmap_len, prot, flags,
                                              fd, 0);
                        if (mapped != MAP_FAILED) {
                            obj->file_mmap = mapped;
                            obj->file_mmap_len = mmap_len;
                            mapped_file = true;
                        }
                    }
                    ::close(fd);
                }
            }
        }

        if (!mapped_file) {
            // Writable mapping larger than the file with no host FD to grow
            // must not silently become a private zero-padded snapshot.
            if (obj->writable && obj->size > file_size) {
                set_last_error(error_codes::ERROR_DISK_FULL);
                return NULL_HANDLE;
            }
            usize read_len = static_cast<usize>(std::min(obj->size, file_size));
            obj->backing_data.resize(static_cast<usize>(obj->size), 0);

            if (read_len > 0) {
                capsule::FileHandle vfs_h = source_vfs;
                if (vfs_h == capsule::INVALID_FILE_HANDLE) {
                    std::fprintf(stderr,
                        "[file_mapping] CreateFileMappingW skipped file read: "
                        "no VFS handle for 0x%x path='%s' map_size=%llu "
                        "(view would be zeros)\n",
                        static_cast<unsigned>(h_file),
                        obj->source_path.c_str(),
                        static_cast<unsigned long long>(obj->size));
                } else {
                    capsule::Vfs& vfs = m_file_mgr->vfs();
                    auto cur = vfs.seek(vfs_h, 0, capsule::SeekOrigin::Current);
                    (void)vfs.seek(vfs_h, 0, capsule::SeekOrigin::Begin);
                    usize total_read = 0;
                    bool read_failed = false;
                    while (total_read < read_len) {
                        if ((total_read & 0x3FFFFu) == 0) {
                            pump_host_gui();
                        }
                        auto rd = vfs.read(
                            vfs_h,
                            std::span<u8>(obj->backing_data.data() + total_read,
                                          read_len - total_read));
                        if (rd.is_err()) {
                            read_failed = true;
                            break;
                        }
                        if (rd.value() == 0) break;
                        total_read += rd.value();
                    }
                    pump_host_gui();
                    if (cur.is_ok()) {
                        (void)vfs.seek(vfs_h, static_cast<i64>(cur.value()),
                                       capsule::SeekOrigin::Begin);
                    }
                    if (!read_failed) {
                        if (file_mapping_diag_enabled()) {
                            ipc::ipc_log(
                                "create_file_mapping read done h=0x%x bytes=%zu",
                                static_cast<unsigned>(h_file), total_read);
                            fprintf(stderr, "[file_mapping] Read %zu bytes from file handle %u into mapping\n",
                                    total_read, static_cast<unsigned>(h_file));
                        }
                    } else {
                        ipc::ipc_log(
                            "create_file_mapping read FAIL h=0x%x after=%zu",
                            static_cast<unsigned>(h_file), total_read);
                        fprintf(stderr, "[file_mapping] WARNING: Failed to read file handle %u\n",
                                static_cast<unsigned>(h_file));
                    }
                }
            }
        }

        if (file_mapping_diag_enabled()) {
            fprintf(stderr, "[file_mapping] CreateFileMappingW: file_handle=%u, file_size=%llu, "
                    "mapping_size=%llu, writable=%d, name='%s'\n",
                    static_cast<unsigned>(h_file), static_cast<unsigned long long>(file_size),
                    static_cast<unsigned long long>(obj->size), obj->writable, name.c_str());
        }
    }

    // Windows returns a valid handle but sets ERROR_ALREADY_EXISTS when the
    // named object already existed (we attached rather than created the shm).
    const bool already_exists = obj->shared && !obj->shared->is_creator();

    // Allocate a handle
    FileMappingObject* raw = obj.get();
    Handle h = m_handles.allocate(
        HandleType::FileMapping, raw,
        inheritable ? HandleFlags::Inherit : 0,
        extras != nullptr ? extras->granted_access : HandleAccess::All);
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        m_mappings[h] = obj;
    }
    if (ipc::ipc_enabled() && h_file == FILE_MAPPING_INVALID_FILE) {
        ipc::ipc_log(
            "CreateFileMapping '%s' -> h=0x%x shm=%s creator=%d size=%llu "
            "transfer='%s'",
            norm_name.c_str(), static_cast<unsigned>(h),
            obj->shared ? "yes" : "no",
            obj->shared ? (obj->shared->is_creator() ? 1 : 0) : -1,
            static_cast<unsigned long long>(obj->size),
            obj->transfer_name.c_str());
    }

    // Track named mappings for OpenFileMappingW
    if (!norm_name.empty()) {
        if (!named_object_register(norm_name, NamedObjectKind::FileMapping)) {
            // Cross-type race: another kind claimed the name after our check.
            {
                std::lock_guard<std::mutex> tables(m_mappings_mutex);
                m_mappings.erase(h);
            }
            m_handles.close(h);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        m_named_mappings[norm_name] = obj;
    }

    set_last_error(already_exists ? error_codes::ERROR_ALREADY_EXISTS
                                  : error_codes::ERROR_SUCCESS);
    return h;
}


bool FileMappingManager::range_within_mapped_view(GuestAddr addr,
                                                  u64 size) const {
    if (size == 0) return false;
    std::lock_guard<std::recursive_mutex> lock(m_views_mutex);
    for (const auto& [base, view] : m_views) {
        if (view.size == 0) continue;
        // Compare against the GUEST-visible extent. guest_mapped_size is what
        // the guest can actually touch; view.size may be the whole section.
        const u64 extent = view.guest_mapped_size != 0
            ? static_cast<u64>(view.guest_mapped_size)
            : view.size;
        if (addr >= base && addr + size <= base + extent) return true;
    }
    return false;
}

u32 FileMappingManager::mem_reset_view_disposition(GuestAddr addr,
                                                   u64 size) const {
    if (size == 0) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(m_views_mutex);
    for (const auto& [base, view] : m_views) {
        if (view.size == 0 || !view.mapping) {
            continue;
        }
        const u64 extent = view.guest_mapped_size != 0
            ? static_cast<u64>(view.guest_mapped_size)
            : view.size;
        if (addr < base || addr + size > base + extent) {
            continue;
        }
        const FileMappingObject& obj = *view.mapping;
        // Pagefile-backed (CreateFileMapping(INVALID_HANDLE_VALUE)): Learn
        // permits MEM_RESET. File-backed sections must fail.
        const bool file_backed =
            obj.file_handle != FILE_MAPPING_INVALID_FILE ||
            obj.file_mmap != nullptr ||
            !obj.source_path.empty();
        if (file_backed) {
            return error_codes::ERROR_USER_MAPPED_FILE;
        }
        return ~u32{0};  // pagefile-backed view: reset allowed
    }
    return 0;
}

} // namespace vitreo::winapi
