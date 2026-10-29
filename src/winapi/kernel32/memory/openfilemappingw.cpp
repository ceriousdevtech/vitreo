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

/// @file openfilemappingw.cpp
/// @brief OpenFileMappingW
///
/// Closed cut from file_mapping.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// OpenFileMappingW
// =============================================================================

Handle FileMappingManager::open_file_mapping(u32 desired_access,
                                              const std::string& name,
                                              bool inheritable) {
    if (!valid_file_mapping_access(desired_access)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return NULL_HANDLE;
    }

    // SDK FILE_MAP_EXECUTE is translated to native SECTION_MAP_EXECUTE;
    // COPY alone is the documented read-access alias. Other combinations
    // retain SECTION_QUERY rather than silently granting READ.
    if (desired_access == FILE_MAP_COPY) desired_access = FILE_MAP_READ;
    if (desired_access & FILE_MAP_EXECUTE)
        desired_access = (desired_access & ~FILE_MAP_EXECUTE) | SECTION_MAP_EXECUTE;

    const std::string norm_name = normalize_mapping_name(name);
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        auto it = m_named_mappings.find(norm_name);
        if (it != m_named_mappings.end()) {
            if (auto obj = it->second.lock()) {
                Handle h = m_handles.allocate(
                    HandleType::FileMapping, obj.get(),
                    inheritable ? HandleFlags::Inherit : 0, desired_access);
                m_mappings[h] = std::move(obj);
                return h;
            }
            m_named_mappings.erase(it);
        }
    }

    // Cross-process: a peer may have created the named shm in another host
    // process. Attach to it by name (create=false).
    if (ipc::ipc_enabled() && !norm_name.empty()) {
        auto shared = ipc::SharedMemory::create_or_open(norm_name, 0, /*create=*/false);
        ipc::ipc_log("OpenFileMapping '%s' -> %s", norm_name.c_str(),
                     shared ? "attached" : "miss");
        if (shared) {
            auto obj = std::make_shared<FileMappingObject>();
            obj->name = norm_name;
            obj->size = shared->size();
            const auto creator_protection = shared->section_protection();
            if (!creator_protection) {
                static std::atomic_flag warned = ATOMIC_FLAG_INIT;
                if (!warned.test_and_set()) std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED kernel32.dll!OpenFileMappingW: section creator metadata unavailable\n");
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                return NULL_HANDLE;
            }
            if ((*creator_protection & (SDK_SEC_IMAGE | SDK_SEC_RESERVE)) != 0) {
                static std::atomic_flag warned = ATOMIC_FLAG_INIT;
                if (!warned.test_and_set()) std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED kernel32.dll!OpenFileMappingW: remote image/reserved section transport\n");
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                return NULL_HANDLE;
            }
            obj->protection = *creator_protection;
            obj->writable = is_writable_protect(obj->protection);
            obj->shared = std::move(shared);
            FileMappingObject* raw = obj.get();
            Handle h = m_handles.allocate(
                HandleType::FileMapping, raw,
                inheritable ? HandleFlags::Inherit : 0, desired_access);
            std::lock_guard<std::mutex> tables(m_mappings_mutex);
            m_mappings[h] = obj;
            m_named_mappings[norm_name] = obj;
            return h;
        }
    }

    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    return NULL_HANDLE;
}

Handle FileMappingManager::duplicate_handle(Handle source, u32 flags,
                                            u32 access) {
    std::lock_guard<std::mutex> tables(m_mappings_mutex);
    auto it = m_mappings.find(source);
    if (it == m_mappings.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }
    // Handles created by CreateFileMapping carry HandleAccess::All as the
    // table's internal unrestricted-access sentinel. Preserve it for
    // DUPLICATE_SAME_ACCESS rather than treating it as a guest access mask.
    if (access != HandleAccess::All &&
        !valid_file_mapping_access(access)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return NULL_HANDLE;
    }

    // A restricted section handle cannot be used to manufacture a handle
    // with rights it was not granted. A caller distinguishes a read-only
    // shared-memory handle by attempting a FILE_MAP_WRITE duplicate and
    // expects ERROR_ACCESS_DENIED when the source
    // handle only carries FILE_MAP_READ | SECTION_QUERY.
    if (access != HandleAccess::All) access &= SECTION_ALL_ACCESS;
    const u32 source_access = m_handles.get_access(source);
    if (source_access != HandleAccess::All &&
        access != HandleAccess::All &&
        (access & ~source_access) != 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return NULL_HANDLE;
    }

    const auto& mapping = it->second;
    Handle duplicate = m_handles.allocate(
        HandleType::FileMapping, mapping.get(), flags, access);
    m_mappings.emplace(duplicate, mapping);
    return duplicate;
}

bool FileMappingManager::make_mapping_transferable(
    const std::shared_ptr<FileMappingObject>& mapping) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    if (!mapping || mapping->size == 0) {
        ipc::ipc_log(
            "promote file mapping FAIL stage=validate mapping=%p size=%llu",
            static_cast<const void*>(mapping.get()),
            static_cast<unsigned long long>(mapping ? mapping->size : 0));
        return false;
    }
    if (mapping->shared) {
        if (mapping->transfer_name.empty()) {
            mapping->transfer_name = mapping->name;
        }
        return !mapping->transfer_name.empty();
    }

    static std::atomic<u64> transfer_counter{1};
    mapping->transfer_name =
        "VitreoInheritedMapping_" + std::to_string(::getpid()) + "_" +
        std::to_string(transfer_counter.fetch_add(1));
    auto shared = ipc::SharedMemory::create_section(
        mapping->transfer_name, mapping->size, /*create=*/true, mapping->protection);
    if (!shared) {
        ipc::ipc_log(
            "promote file mapping FAIL stage=create-shared size=%llu "
            "transfer='%s'",
            static_cast<unsigned long long>(mapping->size),
            mapping->transfer_name.c_str());
        return false;
    }

    const usize host_page = host_mapping_page_size();
    const usize whole_len =
        align_up(static_cast<usize>(mapping->size), host_page);
    Byte* whole = static_cast<Byte*>(shared->map_view(0, whole_len));
    if (whole == nullptr) {
        ipc::ipc_log(
            "promote file mapping FAIL stage=map-copy size=%llu "
            "transfer='%s' errno=%d",
            static_cast<unsigned long long>(mapping->size),
            mapping->transfer_name.c_str(), errno);
        return false;
    }
    struct WholeView {
        Byte* p;
        usize len;
        ~WholeView() { ipc::SharedMemory::unmap_view(p, len); }
    } whole_guard{whole, whole_len};

    // The new shm starts as zero-fill-on-demand pages, which is exactly the
    // Learn contract for unwritten pagefile-backed section pages. Write only
    // host pages that carry non-zero data so promotion never dirties pages
    // the guest has not written.
    const auto holds_data = [host_page](const void* p) {
        const auto page_addr = reinterpret_cast<std::uintptr_t>(p) &
                               ~static_cast<std::uintptr_t>(host_page - 1);
        char state = 0;
        if (::mincore(reinterpret_cast<void*>(page_addr), host_page,
                      &state) != 0) {
            return true;
        }
        return (state & (MINCORE_INCORE | MINCORE_PAGED_OUT)) != 0;
    };
    const auto all_zero = [](const Byte* p, usize n) {
        return std::all_of(p, p + n, [](Byte b) { return b == 0; });
    };

    // Section storage first: file bytes (never probed with mincore — a
    // non-resident file page still holds data) or a private snapshot.
    if (const Byte* section = mapping->section_bytes()) {
        const bool anonymous_section = mapping->file_mmap == nullptr;
        const usize len = std::min(static_cast<usize>(mapping->size),
                                   mapping->section_byte_count());
        for (usize off = 0; off < len; off += host_page) {
            const usize n = std::min(host_page, len - off);
            if (anonymous_section && !holds_data(section + off)) continue;
            if (all_zero(section + off, n)) continue;
            std::memcpy(whole + off, section + off, n);
        }
    }

    // Views of a local section hold the authoritative bytes. Aliased file
    // views already share the section pages; FILE_MAP_COPY views are private.
    // A view page the host never materialized was never written: it is zero.
    std::vector<Byte> page_buf(host_page);
    for (const auto& [base, view] : m_views) {
        if (view.mapping != mapping || view.shared_host_ptr != nullptr) continue;
        if (view.copy_on_write || view.aliases_section) continue;
        if (view.offset >= mapping->size) continue;
        const usize copy_len = static_cast<usize>(std::min(
            view.size, mapping->size - view.offset));
        Byte* dst = whole + static_cast<usize>(view.offset);
        for (usize off = 0; off < copy_len; off += host_page) {
            const usize n = std::min(host_page, copy_len - off);
            const GuestAddr at = base + static_cast<GuestAddr>(off);
            const Byte* host = m_memory.host_ptr(at, n);
            bool data = host == nullptr || holds_data(host);
            if (data && m_memory.read_bytes(at, page_buf.data(), n).is_err()) {
                // Partly committed (SEC_RESERVE) or no-access guest pages:
                // read what is readable; the rest holds no section data.
                for (usize sub = 0; sub < n; sub += GUEST_PAGE_SIZE) {
                    const usize m = std::min<usize>(GUEST_PAGE_SIZE, n - sub);
                    if (m_memory.read_bytes(at + static_cast<GuestAddr>(sub),
                                            page_buf.data() + sub, m)
                            .is_err()) {
                        std::memset(page_buf.data() + sub, 0, m);
                    }
                }
            }
            data = data && !all_zero(page_buf.data(), n);
            if (data) {
                std::memcpy(dst + off, page_buf.data(), n);
            } else if (holds_data(dst + off) && !all_zero(dst + off, n)) {
                std::memset(dst + off, 0, n);
            }
        }
    }

    // Rebind existing parent views so writes after CreateProcess remain visible
    // to the child, as they are for a real inherited section object.
    for (auto& [base, view] : m_views) {
        if (view.mapping != mapping || view.shared_host_ptr != nullptr) continue;
        const usize aligned_size = align_up(
            static_cast<usize>(view.size),
            host_mapping_page_size());
        void* shared_host = shared->map_view(view.offset, aligned_size);
        if (shared_host == nullptr) {
            ipc::ipc_log(
                "promote file mapping FAIL stage=map-parent-view "
                "base=0x%llx offset=%llu size=%zu transfer='%s' errno=%d",
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(view.offset), aligned_size,
                mapping->transfer_name.c_str(), errno);
            return false;
        }

        (void)m_memory.unmap_region(base, aligned_size);
        auto rebound = m_memory.map_region_backed(
            base, aligned_size, shared_host, MemoryProtection::ReadWrite);
        if (rebound.is_err()) {
            ipc::SharedMemory::unmap_view(shared_host, aligned_size);
            // Preserve the parent's usable mapping even though inheritance
            // cannot be completed.
            if (m_memory.map_region(
                    base, aligned_size,
                    MemoryProtection::ReadWrite).is_ok()) {
                const usize copy_len = static_cast<usize>(std::min(
                    view.size, mapping->size - view.offset));
                (void)m_memory.write_bytes(
                    base, whole + static_cast<usize>(view.offset), copy_len);
            }
            ipc::ipc_log(
                "promote file mapping FAIL stage=rebind-parent-view "
                "base=0x%llx offset=%llu size=%zu transfer='%s'",
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(view.offset), aligned_size,
                mapping->transfer_name.c_str());
            return false;
        }
        view.shared_host_ptr = shared_host;
        view.shared_host_len = aligned_size;
        view.aliases_section = false;
    }

    mapping->shared = std::move(shared);
    if (mapping->file_handle == FILE_MAPPING_INVALID_FILE) {
        // The shm is now the section's only storage; a stale private copy
        // would be resident memory nothing reads.
        std::vector<Byte>().swap(mapping->backing_data);
    }
    if (mapping->file_mmap != nullptr && mapping->file_mmap_len != 0) {
        ::munmap(mapping->file_mmap, mapping->file_mmap_len);
        mapping->file_mmap = nullptr;
        mapping->file_mmap_len = 0;
    }
    ipc::ipc_log(
        "promoted inherited file mapping size=%llu transfer='%s'",
        static_cast<unsigned long long>(mapping->size),
        mapping->transfer_name.c_str());
    return true;
}

std::optional<std::string>
FileMappingManager::prepare_inherited_handles() {
    return prepare_inherited_handles(
        m_handles.handles_with_flags(HandleFlags::Inherit));
}

std::optional<std::string>
FileMappingManager::prepare_inherited_handles(
    std::span<const Handle> selected_handles) {
    std::string descriptors;
    for (const Handle handle : selected_handles) {
        if ((m_handles.get_flags(handle) & HandleFlags::Inherit) == 0 ||
            m_handles.get_type(handle) != HandleType::FileMapping) {
            continue;
        }
        std::shared_ptr<FileMappingObject> mapping;
        {
            std::lock_guard<std::mutex> tables(m_mappings_mutex);
            const auto found = m_mappings.find(handle);
            if (found != m_mappings.end()) mapping = found->second;
        }
        if (!mapping || !make_mapping_transferable(mapping)) {
            ipc::ipc_log(
                "failed to prepare inherited file mapping handle=0x%x",
                static_cast<unsigned>(handle));
            return std::nullopt;
        }
        if (!descriptors.empty()) descriptors.push_back(';');
        descriptors += "M," + std::to_string(handle) + "," +
                       std::to_string(mapping->protection) + "," +
                       std::to_string(mapping->writable ? 1 : 0) + "," +
                       std::to_string(m_handles.get_access(handle)) + "," +
                       hex_encode(mapping->transfer_name) + "," +
                       hex_encode(mapping->name);
    }
    return descriptors;
}

bool FileMappingManager::import_inherited_handles(
    std::string_view descriptors) {
    usize offset = 0;
    while (offset < descriptors.size()) {
        const usize end = descriptors.find(';', offset);
        const std::string_view item = descriptors.substr(
            offset, end == std::string_view::npos
                        ? descriptors.size() - offset
                        : end - offset);

        std::array<std::string_view, 7> fields{};
        usize field_start = 0;
        for (usize index = 0; index < fields.size(); ++index) {
            const usize comma = item.find(',', field_start);
            if (index + 1 == fields.size()) {
                if (comma != std::string_view::npos) return false;
                fields[index] = item.substr(field_start);
            } else {
                if (comma == std::string_view::npos) return false;
                fields[index] =
                    item.substr(field_start, comma - field_start);
                field_start = comma + 1;
            }
        }
        if (fields[0] != "M") return false;

        const auto handle_value = parse_u32(fields[1]);
        const auto protection = parse_u32(fields[2]);
        const auto writable_value = parse_u32(fields[3]);
        const auto access = parse_u32(fields[4]);
        const auto transfer_name = hex_decode(fields[5]);
        const auto visible_name = hex_decode(fields[6]);
        if (!handle_value || !protection || !writable_value || !access ||
            !transfer_name || transfer_name->empty() || !visible_name) {
            return false;
        }
        const Handle handle = static_cast<Handle>(*handle_value);
        auto shared = ipc::SharedMemory::create_or_open(
            *transfer_name, 0, /*create=*/false);
        if (!shared) return false;

        auto mapping = std::make_shared<FileMappingObject>();
        mapping->size = shared->size();
        mapping->protection = *protection;
        mapping->name = *visible_name;
        mapping->transfer_name = *transfer_name;
        mapping->file_handle = FILE_MAPPING_INVALID_FILE;
        mapping->writable = *writable_value != 0;
        mapping->shared = std::move(shared);
        if (!m_handles.allocate_at(
                handle, HandleType::FileMapping, mapping.get(),
                HandleFlags::Inherit, *access)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> tables(m_mappings_mutex);
            m_mappings[handle] = mapping;
            if (!mapping->name.empty()) {
                m_named_mappings[mapping->name] = mapping;
            }
        }
        ipc::ipc_log(
            "inherited M handle 0x%x size=%llu transfer='%s'",
            static_cast<unsigned>(handle),
            static_cast<unsigned long long>(mapping->size),
            mapping->transfer_name.c_str());

        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return true;
}

std::optional<std::string>
FileMappingManager::prepare_transferred_handle(
    Handle source, u32 access, bool inheritable) {
    std::shared_ptr<FileMappingObject> source_mapping;
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        const auto found = m_mappings.find(source);
        if (found != m_mappings.end()) source_mapping = found->second;
    }
    if (!source_mapping) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    if (access != HandleAccess::All && !valid_file_mapping_access(access)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return std::nullopt;
    }
    const u32 source_access = m_handles.get_access(source);
    if (source_access != HandleAccess::All &&
        access != HandleAccess::All &&
        (access & ~source_access) != 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return std::nullopt;
    }
    if (!make_mapping_transferable(source_mapping)) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return std::nullopt;
    }
    const auto& mapping = source_mapping;
    const u32 flags = inheritable ? HandleFlags::Inherit : 0;
    return "M," + std::to_string(mapping->size) + "," +
           std::to_string(mapping->protection) + "," +
           std::to_string(mapping->writable ? 1 : 0) + "," +
           std::to_string(access) + "," + std::to_string(flags) + "," +
           hex_encode(mapping->transfer_name) + "," +
           hex_encode(mapping->name);
}

Handle FileMappingManager::import_transferred_handle(
    std::string_view descriptor) {
    std::array<std::string_view, 8> fields{};
    usize field_start = 0;
    for (usize index = 0; index < fields.size(); ++index) {
        const usize comma = descriptor.find(',', field_start);
        if (index + 1 == fields.size()) {
            if (comma != std::string_view::npos) {
                ipc::ipc_log(
                    "import runtime M FAIL stage=parse-extra-field "
                    "descriptor='%.*s'",
                    static_cast<int>(std::min<usize>(descriptor.size(), 96)),
                    descriptor.data());
                return NULL_HANDLE;
            }
            fields[index] = descriptor.substr(field_start);
        } else {
            if (comma == std::string_view::npos) {
                ipc::ipc_log(
                    "import runtime M FAIL stage=parse-fields "
                    "descriptor='%.*s'",
                    static_cast<int>(std::min<usize>(descriptor.size(), 96)),
                    descriptor.data());
                return NULL_HANDLE;
            }
            fields[index] =
                descriptor.substr(field_start, comma - field_start);
            field_start = comma + 1;
        }
    }
    if (fields[0] != "M") {
        ipc::ipc_log("import runtime M FAIL stage=prefix");
        return NULL_HANDLE;
    }
    u64 logical_size = 0;
    const auto size_result = std::from_chars(
        fields[1].data(), fields[1].data() + fields[1].size(),
        logical_size);
    const auto protection = parse_u32(fields[2]);
    const auto writable_value = parse_u32(fields[3]);
    const auto access = parse_u32(fields[4]);
    const auto flags = parse_u32(fields[5]);
    const auto transfer_name = hex_decode(fields[6]);
    const auto visible_name = hex_decode(fields[7]);
    if (size_result.ec != std::errc{} ||
        size_result.ptr != fields[1].data() + fields[1].size() ||
        logical_size == 0 ||
        !protection || !writable_value || !access || !flags ||
        !transfer_name || transfer_name->empty() || !visible_name ||
        (*access != HandleAccess::All &&
         !valid_file_mapping_access(*access))) {
        ipc::ipc_log(
            "import runtime M FAIL stage=validate size=%llu prot=%d "
            "writable=%d access=%d flags=%d transfer_ok=%d",
            static_cast<unsigned long long>(logical_size),
            protection.has_value() ? 1 : 0,
            writable_value.has_value() ? 1 : 0,
            access.has_value() ? 1 : 0, flags.has_value() ? 1 : 0,
            (transfer_name && !transfer_name->empty()) ? 1 : 0);
        return NULL_HANDLE;
    }
    auto shared = ipc::SharedMemory::create_or_open(
        *transfer_name, 0, /*create=*/false);
    if (!shared) {
        ipc::ipc_log(
            "import runtime M FAIL stage=open-shared transfer='%s' "
            "size=%llu",
            transfer_name->c_str(),
            static_cast<unsigned long long>(logical_size));
        return NULL_HANDLE;
    }

    auto mapping = std::make_shared<FileMappingObject>();
    if (logical_size > shared->size()) {
        ipc::ipc_log(
            "import runtime M FAIL stage=size-mismatch logical=%llu "
            "shared=%llu transfer='%s'",
            static_cast<unsigned long long>(logical_size),
            static_cast<unsigned long long>(shared->size()),
            transfer_name->c_str());
        return NULL_HANDLE;
    }
    mapping->size = logical_size;
    mapping->protection = *protection;
    mapping->name = *visible_name;
    mapping->transfer_name = *transfer_name;
    mapping->file_handle = FILE_MAPPING_INVALID_FILE;
    mapping->writable = *writable_value != 0;
    mapping->shared = std::move(shared);
    const Handle handle = m_handles.allocate(
        HandleType::FileMapping, mapping.get(), *flags, *access);
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        m_mappings[handle] = mapping;
        if (!mapping->name.empty()) m_named_mappings[mapping->name] = mapping;
    }
    ipc::ipc_log(
        "imported runtime M handle=0x%x size=%llu transfer='%s'",
        static_cast<unsigned>(handle),
        static_cast<unsigned long long>(mapping->size),
        mapping->transfer_name.c_str());
    return handle;
}

} // namespace vitreo::winapi
