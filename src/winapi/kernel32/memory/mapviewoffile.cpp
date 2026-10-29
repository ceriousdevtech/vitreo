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
#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/mapper/section_mapper.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

/// @file mapviewoffile.cpp
/// @brief MapViewOfFile
///
/// Closed cut from file_mapping.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {
/// A protected DLL that maps its own section over its live image needs the
/// section's other views to alias that image. Honour it unless disabled.
[[nodiscard]] bool section_image_alias_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_NO_SECTION_IMAGE_ALIAS") == nullptr;
    return enabled;
}

[[nodiscard]] u32 view_protection_from_access(u32 desired_access) noexcept {
    const bool executable = (desired_access & FILE_MAP_EXECUTE) != 0;
    if (is_copy_on_write_access(desired_access)) {
        return executable ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
    }
    if ((desired_access & FILE_MAP_WRITE) != 0) {
        return executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    }
    if ((desired_access & FILE_MAP_READ) != 0) {
        return executable ? PAGE_EXECUTE_READ : PAGE_READONLY;
    }
    // No read, write or copy right (dwDesiredAccess 0, or FILE_MAP_EXECUTE
    // alone): the data view is mapped PAGE_NOACCESS
    // (mapview_access_zero_probe; an image view keeps its PE protections).
    return PAGE_NOACCESS;
}

/// Copy-on-write host snapshot of a FILE_MAP_COPY view's map-time bytes:
/// a VM remap shares physical pages until the view writes, with a heap copy
/// when the host range cannot be remapped.
[[nodiscard]] std::shared_ptr<const CopyViewTemplate> capture_copy_template(
    const Byte* src, usize size) {
    if (src == nullptr || size == 0) return nullptr;
    auto snapshot = std::make_shared<CopyViewTemplate>();
    snapshot->size = size;
    const mach_vm_size_t page = static_cast<mach_vm_size_t>(vm_page_size);
    const auto src_addr = reinterpret_cast<mach_vm_address_t>(src);
    if (page != 0 && (src_addr % page) == 0) {
        const mach_vm_size_t rounded = (size + page - 1) & ~(page - 1);
        mach_vm_address_t target = 0;
        vm_prot_t cur_prot = VM_PROT_NONE;
        vm_prot_t max_prot = VM_PROT_NONE;
        if (mach_vm_remap(mach_task_self(), &target, rounded, 0,
                          VM_FLAGS_ANYWHERE, mach_task_self(), src_addr,
                          /*copy=*/TRUE, &cur_prot, &max_prot,
                          VM_INHERIT_NONE) == KERN_SUCCESS) {
            if ((cur_prot & VM_PROT_READ) != 0 ||
                ((max_prot & VM_PROT_READ) != 0 &&
                 mach_vm_protect(mach_task_self(), target, rounded, FALSE,
                                 cur_prot | VM_PROT_READ) == KERN_SUCCESS)) {
                snapshot->data = reinterpret_cast<const Byte*>(target);
                snapshot->vm_size = static_cast<usize>(rounded);
                return snapshot;
            }
            (void)mach_vm_deallocate(mach_task_self(), target, rounded);
        }
    }
    auto* bytes = new (std::nothrow) Byte[size];
    if (bytes == nullptr) return nullptr;
    std::memcpy(bytes, src, size);
    snapshot->data = bytes;
    return snapshot;
}
}  // namespace

CopyViewTemplate::~CopyViewTemplate() {
    if (data == nullptr) return;
    if (vm_size != 0) {
        (void)mach_vm_deallocate(mach_task_self(),
                                 reinterpret_cast<mach_vm_address_t>(data),
                                 vm_size);
    } else {
        delete[] data;
    }
}

u32 FileMappingManager::report_view_protect(const MappedView& view,
                                            u64 page_offset,
                                            u32 stored) const noexcept {
    const u32 base = stored & 0xFFu;
    if (base != PAGE_WRITECOPY && base != PAGE_EXECUTE_WRITECOPY) {
        return stored;
    }
    const CopyViewTemplate* tmpl = view.copy_template.get();
    if (tmpl == nullptr || page_offset >= tmpl->size) return stored;
    const usize len = std::min<usize>(
        GUEST_PAGE_SIZE, tmpl->size - static_cast<usize>(page_offset));
    const Byte* cur = m_memory.host_ptr(
        view.base_addr + static_cast<GuestAddr>(page_offset), len);
    if (cur == nullptr ||
        std::memcmp(cur, tmpl->data + page_offset, len) == 0) {
        return stored;
    }
    return (stored & ~0xFFu) |
           (base == PAGE_WRITECOPY ? PAGE_READWRITE : PAGE_EXECUTE_READWRITE);
}


// =============================================================================
// MapViewOfFile
// =============================================================================

GuestAddr FileMappingManager::map_view_of_file(Handle h_mapping,
                                                u32 desired_access,
                                                u64 offset,
                                                u64 bytes_to_map) {
    // A successful view leaves the caller's last error alone
    // (view_region_lasterror_probe: err=57005 after every mapped view).
    const u32 entry_error = get_last_error();
    const u32 protection = view_protection_from_access(desired_access);
    const GuestAddr view = map_view_impl(h_mapping, desired_access, offset,
                                         bytes_to_map, /*fixed_base=*/0,
                                         win32_prot_to_native(protection),
                                         protection, /*placeholder_group=*/0);
    if (view != 0) set_last_error(entry_error);
    return view;
}

GuestAddr FileMappingManager::map_view_of_file_ex(
    Handle h_mapping, u32 desired_access, u64 offset, u64 bytes_to_map,
    GuestAddr base_address) {
    // Microsoft Learn MapViewOfFileEx: lpBaseAddress must be allocation-
    // granularity aligned, and a non-null address succeeds only when that
    // exact region is available. NULL is equivalent to MapViewOfFile.
    if (base_address != 0 &&
        (base_address % kWindowsAllocationGranularity) != 0) {
        set_last_error(error_codes::ERROR_MAPPED_ALIGNMENT);
        return 0;
    }
    const u32 entry_error = get_last_error();
    const u32 protection = view_protection_from_access(desired_access);
    const GuestAddr view = map_view_impl(h_mapping, desired_access, offset,
                                         bytes_to_map, base_address,
                                         win32_prot_to_native(protection),
                                         protection, /*placeholder_group=*/0);
    if (view != 0) set_last_error(entry_error);
    return view;
}

GuestAddr FileMappingManager::map_view_impl(
    Handle h_mapping, u32 desired_access, u64 offset, u64 bytes_to_map,
    GuestAddr fixed_base, MemoryProtection native_protection,
    u32 win32_protection, u64 placeholder_group) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    if (file_mapping_diag_enabled()) {
        ipc::ipc_log(
            "MapViewOfFile ENTER h=0x%x access=0x%x offset=0x%llx bytes=%llu "
            "fixed=0x%llx",
            static_cast<unsigned>(h_mapping),
            static_cast<unsigned>(desired_access),
            static_cast<unsigned long long>(offset),
            static_cast<unsigned long long>(bytes_to_map),
            static_cast<unsigned long long>(fixed_base));
    }
    // Own a reference for the whole call: another thread's CloseHandle may
    // erase the table entry while this view is being placed.
    std::shared_ptr<FileMappingObject> mapping;
    {
        std::lock_guard<std::mutex> tables(m_mappings_mutex);
        auto it = m_mappings.find(h_mapping);
        if (it != m_mappings.end()) mapping = it->second;
    }
    if (!mapping) {
        ipc::ipc_log("MapViewOfFile FAIL h=0x%x invalid handle",
                     static_cast<unsigned>(h_mapping));
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    FileMappingObject* obj = mapping.get();

    // dwDesiredAccess 0 is valid (mapview_access_zero_probe: every section
    // kind maps; the CLR maps SEC_IMAGE assemblies that way).
    if (!valid_file_mapping_access(desired_access)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    // A view request selects data intent, not the standard/object rights
    // embedded in FILE_MAP_ALL_ACCESS. Windows gives WRITE precedence over
    // READ and COPY; the latter two require SECTION_MAP_READ.
    const u32 granted_access = m_handles.get_access(h_mapping);
    const u32 view_protection = view_protection_from_access(desired_access);
    u32 need = (desired_access & FILE_MAP_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    const bool executable_view = view_protection == PAGE_EXECUTE_READ ||
        view_protection == PAGE_EXECUTE_READWRITE ||
        view_protection == PAGE_EXECUTE_WRITECOPY;
    if (executable_view) need |= SECTION_MAP_EXECUTE;
    if (granted_access != HandleAccess::All && (need & ~granted_access) != 0) {
        ipc::ipc_log("MapViewOfFile rights denial h=0x%x desired=0x%x need=0x%x granted=0x%x protection=0x%x writable=%u",
                     static_cast<unsigned>(h_mapping), desired_access, need,
                     granted_access, obj->protection, obj->writable ? 1u : 0u);
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return 0;
    }
    const u32 section_protection = obj->protection & 0xFFu;
    const bool section_executable =
        section_protection == PAGE_EXECUTE || section_protection == PAGE_EXECUTE_READ ||
        section_protection == PAGE_EXECUTE_READWRITE ||
        section_protection == PAGE_EXECUTE_WRITECOPY;
    if (!obj->sec_image &&
        (((desired_access & FILE_MAP_WRITE) && !obj->writable) ||
         (executable_view && !section_executable))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // Learn MapViewOfFile: file offset must be a multiple of the system
    // allocation granularity (ERROR_MAPPED_ALIGNMENT otherwise).
    const u64 required_offset_alignment =
        placeholder_group != 0 ? static_cast<u64>(GUEST_PAGE_SIZE)
                               : static_cast<u64>(kWindowsAllocationGranularity);
    if ((offset % required_offset_alignment) != 0) {
        set_last_error(error_codes::ERROR_MAPPED_ALIGNMENT);
        return 0;
    }

    // If bytes_to_map is 0, map the full size from offset
    u64 view_size = bytes_to_map;
    if (view_size == 0) {
        if (offset >= obj->size) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return 0;
        }
        view_size = obj->size - offset;
    }

    // Range check
    if (offset > obj->size || view_size > obj->size - offset) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return 0;
    }

    // -----------------------------------------------------------------------
    // SEC_RESERVE: MapView reserves VA (MEM_RESERVE, Protect=0, Type=MEM_MAPPED).
    // Pages become committed only via a later VirtualAlloc MEM_COMMIT.
    // -----------------------------------------------------------------------
    if (obj->sec_reserve) {
        usize aligned_size = align_up(static_cast<usize>(view_size),
                                      host_mapping_page_size());
        const GuestAddr base_align = view_base_alignment();
        const auto view_fits = [&](GuestAddr gap) -> bool {
            return gap != 0 && gap <= m_maximum_address &&
                   aligned_size - 1 <= m_maximum_address - gap;
        };
        GuestAddr base = fixed_base;
        if (base == 0) {
            if (m_next_view_addr == 0 || m_next_view_addr > m_maximum_address) {
                m_next_view_addr = 0x30000000;
            }
            GuestAddr candidate = align_up(m_next_view_addr, base_align);
            // The search returns aligned gaps; a retry is only needed when a
            // concurrent mapping takes the gap between search and map.
            for (int attempt = 0; attempt < 64; ++attempt) {
                GuestAddr gap = m_memory.find_free_gap(candidate, aligned_size,
                                                       base_align);
                if (gap == 0 || !view_fits(gap)) {
                    set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                    return 0;
                }
                if (m_memory.map_region(gap, aligned_size,
                                        MemoryProtection::None,
                                        /*committed=*/false).is_ok()) {
                    base = gap;
                    break;
                }
                candidate = gap + static_cast<GuestAddr>(base_align);
            }
            if (base == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
        } else {
            if (!view_fits(fixed_base) ||
                m_memory.map_region(fixed_base, aligned_size,
                                    MemoryProtection::None,
                                    /*committed=*/false).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_ADDRESS);
                return 0;
            }
            base = fixed_base;
        }

        MappedView view;
        view.base_addr = base;
        view.offset = offset;
        view.size = view_size;
        view.guest_mapped_size =
            align_up(static_cast<usize>(view_size), GUEST_PAGE_SIZE);
        view.desired_access = desired_access;
        view.allocation_protection = section_protection;
        // Protect=0 runs mark still-reserved pages (Windows SEC_RESERVE).
        view.protection_runs.emplace(0, 0);
        view.mapping = mapping;
        view.sec_reserve = true;
        m_views[base] = std::move(view);
        if (fixed_base == 0) {
            m_next_view_addr = align_up(
                base + static_cast<GuestAddr>(aligned_size), base_align);
        }
        return base;
    }

    // -----------------------------------------------------------------------
    // SEC_IMAGE / SEC_IMAGE_NO_EXECUTE: section-aligned PE layout, MEM_IMAGE.
    // -----------------------------------------------------------------------
    if (obj->sec_image) {
        if (obj->image_pe_bytes.empty() || offset != 0 ||
            (bytes_to_map != 0 && bytes_to_map != obj->size)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        auto parsed = pe::parse_pe(obj->image_pe_bytes);
        if (parsed.is_err()) {
            set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
            return 0;
        }
        const pe::ParsedPe& pe = parsed.value();
        const u64 preferred = pe.image_base;
        GuestAddr load_at = fixed_base;
        if (load_at == 0) {
            // Prefer ImageBase when free (Windows SEC_IMAGE places there).
            const usize need = align_up(static_cast<usize>(obj->size),
                                        GUEST_PAGE_SIZE);
            if (preferred != 0 && preferred <= m_maximum_address &&
                need - 1 <= m_maximum_address - preferred &&
                m_memory.find_free_gap(preferred, need) == preferred) {
                load_at = preferred;
            } else {
                load_at = 0;  // SectionMapper / map_pe picks a free base via 0
            }
        }
        pe::SectionMapper mapper(m_memory);
        if (load_at == 0) {
            // Preferred base busy (or unusable): a free gap at allocation
            // granularity, as every Windows view base is (the
            // nt_create_section_probe ntdll copy maps 64K-aligned).
            const usize need = align_up(static_cast<usize>(obj->size), GUEST_PAGE_SIZE);
            const GuestAddr hint = align_up(
                m_next_view_addr != 0 && m_next_view_addr <= m_maximum_address
                    ? m_next_view_addr : GuestAddr{0x10000000},
                view_base_alignment());
            const GuestAddr gap = m_memory.find_free_gap(hint, need, view_base_alignment());
            if (gap == 0 || gap > m_maximum_address || need - 1 > m_maximum_address - gap) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            load_at = gap;
        }
        auto mapped = mapper.map(pe, obj->image_pe_bytes, load_at);
        if (mapped.is_err()) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        const pe::MappedImage& image = mapped.value();
        const GuestAddr base = image.base_address;

        if (obj->sec_image_no_execute) {
            (void)m_memory.protect_region(
                base, image.image_size, MemoryProtection::Read);
        }

        MappedView view;
        view.base_addr = base;
        view.offset = 0;
        view.size = obj->size;
        view.guest_mapped_size = image.image_size;
        view.desired_access = desired_access;
        // SEC_IMAGE AllocationProtect is PAGE_EXECUTE_WRITECOPY; NO_EXECUTE
        // stays PAGE_READONLY (Windows sec_attrs_probe).
        view.allocation_protection =
            obj->sec_image_no_execute ? PAGE_READONLY
                                      : PAGE_EXECUTE_WRITECOPY;
        view.protection_runs.clear();
        if (obj->sec_image_no_execute) {
            view.protection_runs.emplace(0, PAGE_READONLY);
        } else {
            view.protection_runs.emplace(0, PAGE_READONLY);  // headers
            for (const auto& sec : image.sections) {
                const u64 off = sec.guest_address - base;
                view.protection_runs[off] = sec.win32_protection;
            }
        }
        view.mapping = mapping;
        view.sec_image = true;
        view.aliases_section = true;
        m_views[base] = std::move(view);
        if (fixed_base == 0) {
            m_next_view_addr = align_up(
                base + static_cast<GuestAddr>(image.image_size),
                view_base_alignment());
        }
        return base;
    }

    // Allocate address in guest space. Host mmap length stays host-page
    // aligned; the guest base uses Windows allocation granularity so
    // UnmapViewOfFile(align_down(ptr, dwAllocationGranularity)) matches.
    usize aligned_size = align_up(static_cast<usize>(view_size),
                                   host_mapping_page_size());
    const GuestAddr base_align = view_base_alignment();
    const auto view_fits = [&](GuestAddr gap) -> bool {
        return gap != 0 && gap <= m_maximum_address &&
               aligned_size - 1 <= m_maximum_address - gap;
    };
    // The aligned walk crosses any amount of fragmentation. The previous
    // caller-side loop gave up after 16 misaligned gaps; a process whose low
    // VA is diced into 4 KiB per-thread TLS blocks runs out of attempts and
    // fails MapViewOfFile while the address space still has room.
    const auto find_host_aligned_gap =
        [&](GuestAddr min_start) -> GuestAddr {
        const GuestAddr gap =
            m_memory.find_free_gap(min_start, aligned_size, base_align);
        return view_fits(gap) ? gap : 0;
    };
    if (fixed_base == 0 &&
        (m_next_view_addr == 0 || m_next_view_addr > m_maximum_address)) {
        m_next_view_addr = 0x30000000;
    }
    // Identity mapping records a host occupant as a hole on each failed
    // bind, then the next find_free_gap skips just that hole. Eight tries
    // is not enough when titles map many views against a fragmented low
    // 4GB; VirtualAlloc already retries 4096 times for the same reason.
    const int map_attempts = fixed_base != 0
                                 ? 1
                                 : (m_memory.is_identity() ? 4096 : 8);
    GuestAddr base = fixed_base != 0
                         ? fixed_base
                         : align_up(m_next_view_addr, base_align);
    if (fixed_base != 0 && !view_fits(fixed_base)) {
        ipc::ipc_log(
            "MapViewOfFile FAIL h=0x%x fixed=0x%llx above user limit 0x%llx",
            static_cast<unsigned>(h_mapping),
            static_cast<unsigned long long>(fixed_base),
            static_cast<unsigned long long>(m_maximum_address));
        set_last_error(error_codes::ERROR_INVALID_ADDRESS);
        return 0;
    }

    // Learn FILE_MAP_COPY: private pages backed by the paging file — never
    // shared with the section object or peers, discarded on UnmapViewOfFile.
    // Always take the private-copy path for COW, even when the section has
    // POSIX shm (anonymous IPC mappings).
    const bool cow = is_copy_on_write_access(desired_access);

    // Prefer VA below the TEB/PEB island at 0x7FFE0000. A high hint of
    // 0x79000000 cannot hold a ~150 MiB view before that island, so
    // find_free_gap skips to 0x7FFF0000 — above TEB and across 2 GiB —
    // which breaks payload walkers that use 32-bit-sized pointer math on
    // the view base.
    constexpr GuestAddr kTebIslandFloor = 0x7F000000ull;
    auto pick_unfixed_base = [&](GuestAddr search_start) -> GuestAddr {
        const bool high_hint_too_small =
            search_start < kTebIslandFloor &&
            aligned_size > static_cast<usize>(kTebIslandFloor - search_start);
        if (search_start >= kTebIslandFloor || high_hint_too_small) {
            for (GuestAddr low_start :
                 {GuestAddr{0x30000000}, GuestAddr{0x10000000},
                  GuestAddr{0x60000000}}) {
                const GuestAddr low = find_host_aligned_gap(low_start);
                if (low != 0 && low < kTebIslandFloor) {
                    return low;
                }
            }
        }
        GuestAddr gap = find_host_aligned_gap(search_start);
        if (gap != 0 && gap < kTebIslandFloor) {
            return gap;
        }
        for (GuestAddr low_start :
             {GuestAddr{0x30000000}, GuestAddr{0x10000000},
              GuestAddr{0x60000000}}) {
            const GuestAddr low = find_host_aligned_gap(low_start);
            if (low != 0 && low < kTebIslandFloor) {
                return low;
            }
        }
        return gap;
    };

    void* shared_host = nullptr;
    bool aliases_section = false;
    // FILE_MAP_COPY over host-backed section pages (shm or a file mmap): take
    // a kernel copy-on-write snapshot of the view range. Only pages the guest
    // writes are copied; no section page is touched up front. Darwin rejects
    // mmap(MAP_PRIVATE) of a POSIX shm fd, so the snapshot is a
    // mach_vm_remap(copy=TRUE) of a MAP_SHARED view.
    bool cow_host_copy = false;
    if (cow) {
        void* temp_view = nullptr;
        const void* cow_source = nullptr;
        if (obj->shared) {
            temp_view = obj->shared->map_view(offset, aligned_size);
            if (temp_view == nullptr) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            cow_source = temp_view;
        } else if (obj->file_mmap != nullptr &&
                   offset + static_cast<u64>(aligned_size) <=
                       static_cast<u64>(obj->file_mmap_len)) {
            cow_source = static_cast<const Byte*>(obj->file_mmap) +
                         static_cast<usize>(offset);
        }
        if (cow_source != nullptr) {
            mach_vm_address_t copy = 0;
            vm_prot_t cur_prot = 0;
            vm_prot_t max_prot = 0;
            kern_return_t kr = mach_vm_remap(
                mach_task_self(), &copy, aligned_size, 0, VM_FLAGS_ANYWHERE,
                mach_task_self(),
                reinterpret_cast<mach_vm_address_t>(cow_source),
                /*copy=*/TRUE, &cur_prot, &max_prot, VM_INHERIT_COPY);
            if (temp_view != nullptr) {
                ipc::SharedMemory::unmap_view(temp_view, aligned_size);
            }
            if (kr == KERN_SUCCESS &&
                ::mprotect(reinterpret_cast<void*>(copy), aligned_size,
                           PROT_READ | PROT_WRITE) != 0) {
                (void)mach_vm_deallocate(mach_task_self(), copy,
                                         aligned_size);
                kr = KERN_PROTECTION_FAILURE;
            }
            if (kr != KERN_SUCCESS) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            shared_host = reinterpret_cast<void*>(copy);
            cow_host_copy = true;
        }
    }
    if (cow_host_copy || (obj->shared && !cow)) {
        // Cross-process: mmap a MAP_SHARED view of the shm and bind it into the
        // guest arena (no copy) so writes are visible to peer processes. The
        // offset must be page-aligned (mmap requirement); the common case is 0.
        if (!cow_host_copy) {
            shared_host = obj->shared->map_view(offset, aligned_size);
        }
        if (shared_host == nullptr) {
            ipc::ipc_log("map_view FAIL name='%s' shm mmap returned null",
                         obj->name.c_str());
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        bool bound = false;
        for (int attempt = 0; attempt < map_attempts && !bound; ++attempt) {
            if (fixed_base == 0) {
                base = pick_unfixed_base(m_next_view_addr);
            }
            if (base == 0) {
                ipc::ipc_log("map_view FAIL name='%s' no free gap size=%zu",
                             obj->name.c_str(), aligned_size);
                ipc::SharedMemory::unmap_view(shared_host, aligned_size);
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            auto res = m_memory.map_region_backed(base, aligned_size, shared_host,
                                                  native_protection);
            if (res.is_ok()) {
                bound = true;
            } else {
                // Gap taken by a peer thread; advance past it and rescan.
                if (fixed_base == 0) {
                    m_next_view_addr =
                        align_up(base + static_cast<GuestAddr>(aligned_size),
                                 base_align);
                    base = 0;
                }
            }
        }
        if (!bound && !cow && fixed_base != 0 &&
            section_image_alias_enabled()) {
            // A protected DLL maps its own section over its live image so that
            // writes through the section's writable view land in the code it is
            // executing from. The image cannot be unmapped -- that would delete
            // the running code -- so establish the aliasing from the other
            // side: repoint this section's existing views at the image's host
            // pages. Vitreo maps a PE as one contiguous host block (per-section
            // region entries only carve up its protections), which is what
            // makes a single contiguous alias possible; verify that, never
            // assume it.
            const GuestAddr module_base =
                fixed_base - static_cast<GuestAddr>(offset);
            const usize image_span = static_cast<usize>(obj->size);
            Byte* image_host =
                m_memory.host_ptr_mut(module_base, GUEST_PAGE_SIZE);
            Byte* image_tail =
                image_span >= GUEST_PAGE_SIZE
                    ? m_memory.host_ptr_mut(
                          module_base + static_cast<GuestAddr>(
                                            image_span - GUEST_PAGE_SIZE),
                          GUEST_PAGE_SIZE)
                    : nullptr;
            if (image_host != nullptr && image_tail != nullptr &&
                image_tail == image_host + image_span - GUEST_PAGE_SIZE) {
                // Aliasing only means something when this section already has
                // another view for the guest to write through. Without one there
                // is nothing to alias, and an exact-base request over an
                // occupied range must still fail (Learn MapViewOfFileEx).
                usize aliased = 0;
                for (auto& [vbase, view] : m_views) {
                    if (view.mapping.get() != obj || view.copy_on_write) {
                        continue;
                    }
                    // A view already repointed has no shm of its own left.
                    if (view.shared_host_ptr == nullptr) {
                        if (view.aliases_section) ++aliased;
                        continue;
                    }
                    if (view.offset + view.size > image_span) continue;
                    const usize vlen = align_up(static_cast<usize>(view.size),
                                                host_mapping_page_size());
                    void* old_host = view.shared_host_ptr;
                    const usize old_len = view.shared_host_len;
                    (void)m_memory.unmap_region(vbase, vlen);
                    if (m_memory.map_region_backed(
                            vbase, vlen,
                            image_host + static_cast<usize>(view.offset),
                            MemoryProtection::ReadWrite).is_ok()) {
                        ipc::SharedMemory::unmap_view(old_host, old_len);
                        view.shared_host_ptr = nullptr;
                        view.shared_host_len = 0;
                        view.aliases_section = true;
                        ++aliased;
                        // The view now writes straight into the image's host
                        // pages, including its executable sections. Compiled
                        // code is cached by guest address, so tell the memory
                        // system these stores rewrite code at the image
                        // address; without it the JIT keeps running
                        // translations of the still-encrypted bytes.
                        m_memory.add_code_alias(
                            vbase,
                            module_base + static_cast<GuestAddr>(view.offset),
                            vlen);
                        VITREO_TRACE(
                            "[file_mapping] section view 0x%llx now aliases "
                            "image 0x%llx+0x%llx\n",
                            static_cast<unsigned long long>(vbase),
                            static_cast<unsigned long long>(module_base),
                            static_cast<unsigned long long>(view.offset));
                    }
                }
                if (aliased != 0) {
                    // The image is now the section's storage, so the mapping
                    // the caller asked for is in effect at the address it
                    // requested.
                    ipc::SharedMemory::unmap_view(shared_host, aligned_size);
                    return fixed_base;
                }
            }
        }
        if (!bound) {
            ipc::ipc_log("map_view FAIL name='%s' map_region_backed contended size=%zu",
                         obj->name.c_str(), aligned_size);
            ipc::SharedMemory::unmap_view(shared_host, aligned_size);
            set_last_error(fixed_base != 0
                               ? error_codes::ERROR_INVALID_ADDRESS
                               : error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        ipc::ipc_log(
            "map_view OK h=0x%x name='%s' transfer='%s' base=0x%llx host=%p "
            "size=%zu offset=%llu",
            static_cast<unsigned>(h_mapping), obj->name.c_str(),
            obj->transfer_name.c_str(),
            static_cast<unsigned long long>(base), shared_host, aligned_size,
            static_cast<unsigned long long>(offset));
    } else if (!cow && obj->file_mmap != nullptr &&
               offset + static_cast<u64>(aligned_size) <=
                   static_cast<u64>(obj->file_mmap_len)) {
        // Learn MapViewOfFile: a file view exposes section pages; it does not
        // copy the file into a newly zeroed private region.
        void* host = static_cast<Byte*>(obj->file_mmap) +
                     static_cast<usize>(offset);
        bool bound = false;
        for (int attempt = 0; attempt < map_attempts && !bound; ++attempt) {
            if (fixed_base == 0) {
                base = pick_unfixed_base(m_next_view_addr);
            }
            if (base == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            auto res = m_memory.map_region_backed(base, aligned_size, host,
                                                  native_protection);
            if (res.is_ok()) {
                bound = true;
            } else if (fixed_base != 0) {
                break;
            } else {
                m_next_view_addr =
                    align_up(base + static_cast<GuestAddr>(aligned_size),
                             base_align);
                base = 0;
            }
        }
        if (!bound) {
            // Learn MapViewOfFileEx: a non-NULL lpBaseAddress is exact, and an
            // unavailable range is ERROR_INVALID_ADDRESS, not an allocation
            // failure. Packers that alias a section over their own image
            // branch on this to pick an in-place fallback, so reporting
            // "out of memory" sends them down the fatal path instead.
            set_last_error(fixed_base != 0
                               ? error_codes::ERROR_INVALID_ADDRESS
                               : error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        aliases_section = true;
    } else {
        // A section view owns its guest virtual-address range. Reusing an
        // arbitrary existing mapping would overwrite unrelated memory and
        // later prevent this private view from being rebound to MAP_SHARED
        // when DuplicateHandle transfers the section to another process.
        bool mapped = false;
        for (int attempt = 0; attempt < map_attempts && !mapped; ++attempt) {
            if (base != 0 && base < kTebIslandFloor &&
                m_memory.map_region(
                    base, aligned_size,
                    MemoryProtection::ReadWrite).is_ok()) {
                mapped = true;
                break;
            }
            if (fixed_base != 0) {
                if (base != 0 &&
                    m_memory.map_region(
                        base, aligned_size,
                        MemoryProtection::ReadWrite).is_ok()) {
                    mapped = true;
                }
                break;
            }
            const GuestAddr search_start =
                base == 0 ? m_next_view_addr : base + base_align;
            base = pick_unfixed_base(search_start);
            if (base != 0 &&
                m_memory.map_region(
                    base, aligned_size,
                    MemoryProtection::ReadWrite).is_ok()) {
                mapped = true;
                break;
            }
            if (base != 0) {
                m_next_view_addr =
                    align_up(base + static_cast<GuestAddr>(aligned_size),
                             base_align);
                base = 0;
            }
        }
        if (!mapped) {
            ipc::ipc_log(
                "MapViewOfFile FAIL h=0x%x no_gap size=%zu fixed=0x%llx",
                static_cast<unsigned>(h_mapping), aligned_size,
                static_cast<unsigned long long>(fixed_base));
            // See above: an exact-base request that cannot be satisfied is
            // ERROR_INVALID_ADDRESS on Windows.
            set_last_error(fixed_base != 0
                               ? error_codes::ERROR_INVALID_ADDRESS
                               : error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }

        // Snapshot section contents into the private view.
        auto fill_view = [&](const Byte* src, usize copy_len) -> bool {
            if (copy_len == 0) return true;
            if (copy_len >= 64u * 1024u) {
                pump_host_gui();
            }
            Byte* dst = m_memory.host_ptr_mut(base, copy_len);
            if (dst != nullptr) {
                std::memcpy(dst, src, copy_len);
                return true;
            }
            return m_memory.write_bytes(base, src, copy_len).is_ok();
        };
        if (const Byte* src = obj->section_bytes();
            src != nullptr && offset < obj->section_byte_count()) {
            usize copy_len = std::min(
                static_cast<usize>(view_size),
                obj->section_byte_count() - static_cast<usize>(offset));
            if (!fill_view(src + static_cast<usize>(offset), copy_len)) {
                (void)m_memory.unmap_region(base, aligned_size);
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
        }
        if (native_protection != MemoryProtection::ReadWrite &&
            m_memory.protect_region(base, aligned_size,
                                    native_protection).is_err()) {
            (void)m_memory.unmap_region(base, aligned_size);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
    }

    // Record the view
    MappedView view;
    view.base_addr       = base;
    view.offset          = offset;
    view.size            = view_size;
    view.guest_mapped_size = align_up(
        static_cast<usize>(view_size), GUEST_PAGE_SIZE);
    view.desired_access = desired_access;
    view.allocation_protection = win32_protection;
    view.protection_runs.emplace(0, win32_protection);
    view.mapping         = mapping;
    view.shared_host_ptr = shared_host;
    view.shared_host_len = shared_host ? aligned_size : 0;
    view.placeholder_group = placeholder_group;
    view.copy_on_write   = is_copy_on_write_access(desired_access);
    view.aliases_section = aliases_section;
    if (view.copy_on_write) {
        view.copy_template = capture_copy_template(
            m_memory.host_ptr(base, view.guest_mapped_size),
            view.guest_mapped_size);
    }
    m_views[base] = view;
    if (file_mapping_diag_enabled()) {
        std::fprintf(stderr,
            "[file_mapping] MapViewOfFile base=0x%llx size=0x%llx "
            "offset=0x%llx name='%s'\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(view_size),
            static_cast<unsigned long long>(offset),
            obj->name.c_str());
    }

    // Advance past this view; next base is rounded up to allocation
    // granularity inside find_host_aligned_gap / align_up.
    if (fixed_base == 0) {
        m_next_view_addr =
            align_up(base + static_cast<GuestAddr>(aligned_size), base_align);
        if (m_next_view_addr == 0 || m_next_view_addr > m_maximum_address) {
            m_next_view_addr = 0x30000000;
        }
    }

    if (file_mapping_diag_enabled()) {
        fprintf(stderr, "[file_mapping] MapViewOfFile: handle=%u, offset=%llu, view_size=%llu, "
                "guest_addr=0x%08x, file_backed=%s\n",
                static_cast<unsigned>(h_mapping),
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(view_size),
                static_cast<unsigned>(base),
                (obj->file_handle != FILE_MAPPING_INVALID_FILE) ? "yes" : "no");
        ipc::ipc_log(
            "MapViewOfFile OK h=0x%x base=0x%llx size=%llu offset=0x%llx",
            static_cast<unsigned>(h_mapping),
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(view_size),
            static_cast<unsigned long long>(offset));
    }

    ipc::ipc_log("MapViewOfFile h=0x%x -> view=0x%llx", static_cast<unsigned>(h_mapping), static_cast<unsigned long long>(base));
    return base;
}

GuestAddr FileMappingManager::map_view_of_file3(
    Handle h_mapping, GuestAddr base_address, u64 offset, u64 view_size,
    u32 allocation_type, u32 page_protection,
    VirtualMemoryManager& virtual_memory) {
    if (allocation_type != 0 &&
        allocation_type != MEM_REPLACE_PLACEHOLDER) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (!is_supported_win32_protection(page_protection) ||
        (view_size != 0 && (view_size % GUEST_PAGE_SIZE) != 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    u32 desired_access = FILE_MAP_READ;
    const u32 base_protection = page_protection & 0xFFu;
    switch (base_protection) {
    case PAGE_READONLY:
        break;
    case PAGE_READWRITE:
        desired_access |= FILE_MAP_WRITE;
        break;
    case PAGE_WRITECOPY:
        desired_access = FILE_MAP_COPY;
        break;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
        desired_access |= FILE_MAP_EXECUTE;
        break;
    case PAGE_EXECUTE_READWRITE:
        desired_access |= FILE_MAP_WRITE | FILE_MAP_EXECUTE;
        break;
    case PAGE_EXECUTE_WRITECOPY:
        desired_access = FILE_MAP_COPY | FILE_MAP_EXECUTE;
        break;
    default:
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    PlaceholderToken placeholder{};
    if (allocation_type == MEM_REPLACE_PLACEHOLDER) {
        if (base_address == 0 || view_size == 0 ||
            !virtual_memory.take_placeholder(
                base_address, view_size, placeholder)) {
            set_last_error(error_codes::ERROR_INVALID_ADDRESS);
            return 0;
        }
    } else if (base_address != 0) {
        // Microsoft documents the non-placeholder desired address as rounded
        // down to the Windows allocation granularity.
        base_address = align_down(base_address, kWindowsAllocationGranularity);
    }

    const GuestAddr result = map_view_impl(
        h_mapping, desired_access, offset, view_size, base_address,
        win32_prot_to_native(page_protection), page_protection,
        placeholder.group);
    if (result == 0 && placeholder.group != 0) {
        (void)virtual_memory.restore_placeholder(placeholder);
    }
    if (result != 0) {
        VITREO_TRACE("[file_mapping] MapViewOfFile3 h=0x%x base=0x%llx"
                     " offset=0x%llx size=0x%llx type=0x%x prot=0x%x"
                     " -> 0x%llx\n",
                     static_cast<unsigned>(h_mapping),
                     static_cast<unsigned long long>(base_address),
                     static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(view_size),
                     allocation_type, page_protection,
                     static_cast<unsigned long long>(result));
        ipc::ipc_log(
            "MapViewOfFile3 h=0x%x base=0x%llx offset=0x%llx size=0x%llx "
            "type=0x%x protect=0x%x placeholder=%d -> 0x%llx",
            static_cast<unsigned>(h_mapping),
            static_cast<unsigned long long>(base_address),
            static_cast<unsigned long long>(offset),
            static_cast<unsigned long long>(view_size), allocation_type,
            page_protection, placeholder.group != 0 ? 1 : 0,
            static_cast<unsigned long long>(result));
    }
    return result;
}

std::string FileMappingManager::mapped_file_path(GuestAddr address) const {
    std::lock_guard<std::recursive_mutex> lock(m_views_mutex);
    for (const auto& [base, view] : m_views) {
        if (address >= base && address - base < view.guest_mapped_size && view.mapping)
            return view.mapping->source_path;
    }
    return {};
}

bool FileMappingManager::query_view(GuestAddr address, GuestAddr& view_base,
                                    u64& view_size, u32* protection,
                                    u64* protection_run_end,
                                    u32* allocation_protection,
                                    bool* sec_image,
                                    bool* page_reserved) const noexcept {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    // Views are keyed by base; scan for containment. Mapped views are few
    // enough that linear search is fine.
    for (const auto& [base, view] : m_views) {
        if (address < base) continue;
        const u64 end = static_cast<u64>(base) + view.guest_mapped_size;
        if (static_cast<u64>(address) >= end) continue;
        view_base = base;
        view_size = view.guest_mapped_size;
        const u64 offset = address - base;
        const u64 page_off =
            align_down(offset, static_cast<u64>(GUEST_PAGE_SIZE));
        const auto run = std::prev(view.protection_runs.upper_bound(offset));
        const u32 reported = report_view_protect(view, page_off, run->second);
        if (protection != nullptr) *protection = reported;
        if (allocation_protection != nullptr) {
            *allocation_protection = view.allocation_protection;
        }
        if (protection_run_end != nullptr) {
            // VirtualQuery's region is every following page with the same
            // attributes, across protection-run (section) boundaries: a
            // SEC_IMAGE view of a PE whose adjacent sections share a
            // protection is one region on Windows
            // (sec_image_foreign_probe). A privately written page splits a
            // WRITECOPY run; a reserved (0) run never merges with a
            // committed one because its stored value differs.
            const u64 view_end = static_cast<u64>(view.guest_mapped_size);
            u64 end_off = view_end;
            for (auto r = run; r != view.protection_runs.end(); ++r) {
                const auto next = std::next(r);
                const u64 r_end = next == view.protection_runs.end()
                                      ? view_end
                                      : std::min<u64>(next->first, view_end);
                u64 off = r == run ? page_off + GUEST_PAGE_SIZE : r->first;
                if (view.copy_template == nullptr) {
                    if (off < r_end && r->second != reported) {
                        end_off = off;
                        break;
                    }
                    continue;
                }
                for (; off < r_end; off += GUEST_PAGE_SIZE) {
                    if (report_view_protect(view, off, r->second) !=
                        reported) {
                        break;
                    }
                }
                if (off < r_end) {
                    end_off = off;
                    break;
                }
            }
            *protection_run_end = base + end_off;
        }
        if (sec_image != nullptr) *sec_image = view.sec_image;
        if (page_reserved != nullptr) {
            *page_reserved = view.sec_reserve && run->second == 0;
        }
        return true;
    }
    return false;
}

bool FileMappingManager::commit_reserved_view_pages(GuestAddr address,
                                                    u64 size, u32 protect) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    if (size == 0) return false;
    for (auto& [base, view] : m_views) {
        if (!view.sec_reserve) continue;
        if (address < base) continue;
        const u64 offset = address - base;
        if (offset >= view.guest_mapped_size) continue;
        if (size > view.guest_mapped_size - offset) return false;

        if (m_memory.commit_region(address, static_cast<usize>(size),
                                   MemoryProtection::ReadWrite).is_err()) {
            return false;
        }
        // Zero newly committed pages (Learn VirtualAlloc of reserved pages).
        if (Byte* p = m_memory.host_ptr_mut(address,
                                            static_cast<usize>(size))) {
            std::memset(p, 0, static_cast<usize>(size));
        }
        if (m_memory.protect_region(address, static_cast<usize>(size),
                                    win32_prot_to_native(protect)).is_err()) {
            return false;
        }

        const u64 end_offset = offset + size;
        const u32 after =
            std::prev(view.protection_runs.upper_bound(end_offset - 1))
                ->second;
        auto erase_begin = view.protection_runs.lower_bound(offset);
        auto erase_end = view.protection_runs.lower_bound(end_offset);
        view.protection_runs.erase(erase_begin, erase_end);
        view.protection_runs[offset] = protect;
        if (end_offset < view.guest_mapped_size) {
            const auto existing =
                view.protection_runs.find(end_offset);
            if (existing == view.protection_runs.end()) {
                view.protection_runs[end_offset] = after;
            }
        }
        return true;
    }
    return false;
}

ViewDiscardResult FileMappingManager::discard_virtual_memory(
    GuestAddr address, u64 size) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    if (size == 0 || (address % GUEST_PAGE_SIZE) != 0 ||
        (size % GUEST_PAGE_SIZE) != 0 ||
        address > std::numeric_limits<u64>::max() - size) {
        return ViewDiscardResult::InvalidRange;
    }
    for (const auto& [base, view] : m_views) {
        if (address < base) continue;
        const u64 offset = address - base;
        if (offset >= view.guest_mapped_size) continue;
        if (size > view.guest_mapped_size - offset) {
            return ViewDiscardResult::InvalidRange;
        }
        const u64 end_offset = offset + size;
        auto run = std::prev(view.protection_runs.upper_bound(offset));
        u64 cursor = offset;
        while (cursor < end_offset) {
            if (run->second != PAGE_READWRITE) {
                return ViewDiscardResult::InvalidRange;
            }
            const auto next = std::next(run);
            cursor = std::min(
                end_offset,
                next == view.protection_runs.end()
                    ? static_cast<u64>(view.guest_mapped_size)
                    : next->first);
            if (cursor < end_offset) run = next;
        }
        return m_memory.discard_mapped_region_contents(address, size).is_ok()
            ? ViewDiscardResult::Success
            : ViewDiscardResult::HostFailure;
    }
    return ViewDiscardResult::NotOwned;
}

ViewProtectResult FileMappingManager::virtual_protect_view(
    GuestAddr address, usize size, u32 new_protect, u32* old_protect) {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    if (size == 0 || address > std::numeric_limits<u64>::max() - size) {
        return ViewProtectResult::InvalidRange;
    }
    if (!is_valid_protect_request(new_protect)) {
        return ViewProtectResult::InvalidProtection;
    }
    const GuestAddr page_base = align_down(
        address, static_cast<GuestAddr>(GUEST_PAGE_SIZE));
    const GuestAddr page_end = align_up(
        address + size, static_cast<GuestAddr>(GUEST_PAGE_SIZE));
    for (auto& [base, view] : m_views) {
        if (page_base < base) continue;
        const u64 begin_offset = page_base - base;
        if (begin_offset >= view.guest_mapped_size) continue;
        const u64 protect_size = page_end - page_base;
        if (protect_size > view.guest_mapped_size - begin_offset) {
            return ViewProtectResult::InvalidRange;
        }

        const u32 page_protect = new_protect & 0xFFu;
        const bool wants_write = page_protect == PAGE_READWRITE ||
                                 page_protect == PAGE_EXECUTE_READWRITE;
        const bool wants_copy = page_protect == PAGE_WRITECOPY ||
                                page_protect == PAGE_EXECUTE_WRITECOPY;
        const bool wants_execute = page_protect == PAGE_EXECUTE ||
                                   page_protect == PAGE_EXECUTE_READ ||
                                   page_protect == PAGE_EXECUTE_READWRITE ||
                                   page_protect == PAGE_EXECUTE_WRITECOPY;
        // writecopy_protect_probe map ...: PAGE_READWRITE forms need a
        // FILE_MAP_WRITE view (copy and read views reject them), execute
        // forms need FILE_MAP_EXECUTE, and the WRITECOPY forms need only read
        // access, so every view accepts them.
        if ((wants_write && (view.desired_access & FILE_MAP_WRITE) == 0) ||
            (wants_execute &&
             (view.desired_access & FILE_MAP_EXECUTE) == 0)) {
            if (old_protect != nullptr) *old_protect = PAGE_NOACCESS;
            return ViewProtectResult::InvalidProtection;
        }
        // On a shared view, WRITECOPY makes the next write to each page a
        // private copy the section never sees. Shared views alias section
        // pages for their whole length, so that per-page split is not
        // available here.
        if (wants_copy && !view.copy_on_write) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED kernel32!VirtualProtect: "
                    "PAGE_*WRITECOPY on a shared (non-FILE_MAP_COPY) mapped "
                    "view needs per-page copy-on-write\n");
            }
            return ViewProtectResult::Unimplemented;
        }

        auto current = std::prev(
            view.protection_runs.upper_bound(begin_offset));
        if (old_protect != nullptr) {
            *old_protect = report_view_protect(
                view, begin_offset, current->second);
        }
        if (m_memory.protect_region(
                page_base, static_cast<usize>(protect_size),
                win32_prot_to_native(new_protect)).is_err()) {
            return ViewProtectResult::HostFailure;
        }

        const u64 end_offset = begin_offset + protect_size;
        const u32 after = end_offset < view.guest_mapped_size
            ? std::prev(view.protection_runs.upper_bound(end_offset))->second
            : 0u;
        auto erase_begin = view.protection_runs.lower_bound(begin_offset);
        auto erase_end = view.protection_runs.lower_bound(end_offset);
        view.protection_runs.erase(erase_begin, erase_end);
        view.protection_runs[begin_offset] = new_protect;
        if (end_offset < view.guest_mapped_size) {
            view.protection_runs[end_offset] = after;
        }
        auto inserted = view.protection_runs.find(begin_offset);
        if (inserted != view.protection_runs.begin()) {
            auto previous = std::prev(inserted);
            if (previous->second == inserted->second) {
                view.protection_runs.erase(inserted);
            }
        }
        auto boundary = view.protection_runs.find(end_offset);
        if (boundary != view.protection_runs.end() &&
            boundary != view.protection_runs.begin()) {
            auto previous = std::prev(boundary);
            if (previous->second == boundary->second) {
                view.protection_runs.erase(boundary);
            }
        }
        return ViewProtectResult::Success;
    }
    return ViewProtectResult::NotOwned;
}

void FileMappingManager::dump_ipcz_router_link_state_candidates(
    const char* reason) const {
    std::lock_guard<std::recursive_mutex> views_lock(m_views_mutex);
    // Primary shared buffer is 128 KiB. Layout (v0):
    //   [0,256)   reserved header (next_buffer_id / next_sublink_id at front)
    //   [256,1024) initial link-state records, 12 of them, 64 B each
    //   [1024,...) 64-byte block-allocator region (dynamic records, e.g. offset 1088)
    // Layout of each link-state record: ref_count (i32 @0) + status (u32 @4).
    constexpr u64 kPrimarySize = 128ull * 1024ull;
    constexpr u64 kInitialLinkStateBase = 256;
    constexpr usize kLinkStateSize = 64;
    constexpr u64 kDynamicProbeOff = 1088; // first transferred-portal RLS
    constexpr usize kInitialSlotsToDump = 3; // ConnectNode portals 0..2

    const auto load_u32 = [](const std::array<u8, kLinkStateSize>& b,
                             usize off) -> u32 {
        return static_cast<u32>(b[off]) |
               (static_cast<u32>(b[off + 1]) << 8) |
               (static_cast<u32>(b[off + 2]) << 16) |
               (static_cast<u32>(b[off + 3]) << 24);
    };
    const auto dump_slot = [&](GuestAddr base, const MappedView& view,
                               u64 link_off, const char* slot_tag) {
        if (view.size < link_off + kLinkStateSize) return;
        std::array<u8, kLinkStateSize> from_guest{};
        std::array<u8, kLinkStateSize> from_host{};
        const bool guest_ok =
            m_memory
                .read_bytes(base + static_cast<GuestAddr>(link_off),
                            from_guest.data(), kLinkStateSize)
                .is_ok();
        bool host_ok = false;
        if (view.shared_host_ptr != nullptr &&
            view.shared_host_len >= link_off + kLinkStateSize) {
            std::memcpy(from_host.data(),
                        static_cast<const u8*>(view.shared_host_ptr) + link_off,
                        kLinkStateSize);
            host_ok = true;
        }
        const int refcount =
            guest_ok ? static_cast<int>(load_u32(from_guest, 0)) : -1;
        const u32 status =
            host_ok ? load_u32(from_host, 4)
                    : (guest_ok ? load_u32(from_guest, 4) : 0u);
        // Skip completely untouched zero slots to cut noise.
        if (refcount == 0 && status == 0) return;
        ipc::ipc_log(
            "ipcz-rls %s %s off=%llu transfer='%s' guest_ok=%d host_ok=%d "
            "refcount=%d status=0x%x (kStable=0x3 kWaitingA=0x5)",
            reason != nullptr ? reason : "?", slot_tag,
            static_cast<unsigned long long>(link_off),
            view.mapping->transfer_name.c_str(), guest_ok ? 1 : 0,
            host_ok ? 1 : 0, refcount, status);
    };

    for (const auto& [base, view] : m_views) {
        if (!view.mapping || !view.mapping->shared) continue;
        if (view.mapping->size < kPrimarySize) continue;
        if (view.offset != 0) continue;
        if (view.size < kDynamicProbeOff + kLinkStateSize) continue;

        for (usize i = 0; i < kInitialSlotsToDump; ++i) {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "init[%zu]", i);
            dump_slot(base, view,
                      kInitialLinkStateBase +
                          static_cast<u64>(i) * kLinkStateSize,
                      tag);
        }
        dump_slot(base, view, kDynamicProbeOff, "dyn@1088");
    }
}

} // namespace vitreo::winapi
