// SPDX-License-Identifier: LGPL-2.1-only

/// @file memory.cpp
/// @brief VirtualAlloc / VirtualFree / VirtualProtect implementation (WIN32-020)

#include "core/memory/host_memory_floor.hpp"
#include "vitreo/common.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <vector>

/// @file virtualalloc.cpp
/// @brief VirtualAlloc
///
/// Closed cut from memory.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {

// Bits accepted with MEM_COMMIT|MEM_RESERVE on Windows (Windows bit scan).
// Low 0x3F bits are ignored; MEM_ROTATE is accepted; MEM_PHYSICAL is only
// legal with MEM_RESERVE and no MEM_COMMIT (checked separately).
constexpr u32 kVirtualAllocTypeAllow =
    MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_PHYSICAL |
    MEM_ROTATE | 0x3Fu;

}  // namespace

u32 virtual_alloc_flag_error(u32 allocation_type, u32 protect) noexcept {
    // Standalone MEM_RESET / MEM_RESET_UNDO are validated in their own path.
    if ((allocation_type & (MEM_RESET | MEM_RESET_UNDO)) != 0) {
        return 0;
    }

    // MEM_LARGE_PAGES needs SeLockMemoryPrivilege + host large-page support.
    // Vitreo cannot honour them; refuse like an unprivileged Windows process
    // (ERROR_INVALID_PARAMETER) rather than silently allocating 4 KiB pages.
    if ((allocation_type & MEM_LARGE_PAGES) != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED kernel32!VirtualAlloc: MEM_LARGE_PAGES "
                "(no SeLockMemoryPrivilege / large-page host support)\n");
        }
        return error_codes::ERROR_INVALID_PARAMETER;
    }

    if ((allocation_type & ~kVirtualAllocTypeAllow) != 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    if ((allocation_type & (MEM_COMMIT | MEM_RESERVE)) == 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    // Learn / probe: MEM_WRITE_WATCH requires MEM_RESERVE at allocation time.
    if ((allocation_type & MEM_WRITE_WATCH) != 0 &&
        (allocation_type & MEM_RESERVE) == 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    // Learn: MEM_PHYSICAL with MEM_RESERVE and no MEM_COMMIT; PAGE_READWRITE.
    if ((allocation_type & MEM_PHYSICAL) != 0) {
        if ((allocation_type & MEM_RESERVE) == 0 ||
            (allocation_type & MEM_COMMIT) != 0) {
            return error_codes::ERROR_INVALID_PARAMETER;
        }
        // Windows: PAGE_READONLY → ERROR_NO_SYSTEM_RESOURCES; other non-RW
        // protections → ERROR_INVALID_PARAMETER.
        if (protect == PAGE_READONLY) {
            return error_codes::ERROR_NO_SYSTEM_RESOURCES;
        }
        if (protect != PAGE_READWRITE) {
            return error_codes::ERROR_INVALID_PARAMETER;
        }
    }
    return 0;
}

// -----------------------------------------------------------------------------
// VirtualAlloc(MEM_RESET) / VirtualAlloc(MEM_RESET_UNDO)
// -----------------------------------------------------------------------------

u64 VirtualMemoryManager::virtual_alloc_reset(u64 address, u64 size,
                                              u32 allocation_type,
                                              u32 protect) {
    // Learn (VirtualAlloc): MEM_RESET says the data in the range is no longer
    // of interest -- the pages must not be written to the paging file, but the
    // block stays committed for later reuse. MEM_RESET_UNDO takes that back.
    // Each "cannot be used with any other value", and flProtect is ignored but
    // must still be a valid protection value.
    //
    // Measured on Windows, x86 and x64 alike (tools/oracle_probe/
    // mem_reset_probe{32,64}.expected), through VirtualAlloc and VirtualAlloc2:
    //   - pages only need to be RESERVED: a reset over a reserved NOACCESS
    //     range (Chromium's PartitionAlloc pools) succeeds and leaves state
    //     and protection untouched;
    //   - the base is used as given: NULL or not page aligned is
    //     ERROR_INVALID_ADDRESS, as is any range not inside one allocation
    //     (freed memory, past its end); it is not rounded down;
    //   - flProtect 0 is ERROR_INVALID_PARAMETER, any other invalid value
    //     ERROR_NOT_SUPPORTED.
    m_last_alloc_error = 0;
    const u64 page_mask = static_cast<u64>(GUEST_PAGE_SIZE) - 1;
    if (size == 0 ||
        (allocation_type != MEM_RESET && allocation_type != MEM_RESET_UNDO)) {
        m_last_alloc_error = error_codes::ERROR_INVALID_PARAMETER;
        return 0;
    }
    if (!is_supported_win32_protection(protect)) {
        m_last_alloc_error = protect == 0 ? error_codes::ERROR_INVALID_PARAMETER
                                          : error_codes::ERROR_NOT_SUPPORTED;
        return 0;
    }
    if (address == 0 || (address & page_mask) != 0 ||
        address > m_maximum_address ||
        size - 1 > m_maximum_address - address) {
        m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    const u64 base = address;
    const u64 end = align_up(address + size, static_cast<u64>(GUEST_PAGE_SIZE));
    const u64 span = end - base;

    // The range must lie inside one existing allocation; Windows reports
    // ERROR_INVALID_ADDRESS for a range that is not part of one.
    auto it = find_allocation_locked(base);
    if (it != m_allocations.end() && end - it->first <= it->second.size) {
        // Neither form changes commit state or protection. MEM_RESET offers
        // the writable host pages back without writing them; MEM_RESET_UNDO
        // succeeds only if none of them was reclaimed meanwhile ("if the
        // function fails, at least some of the data ... has been replaced
        // with zeroes"). Learn documents no last-error for that failure.
        if (allocation_type == MEM_RESET) {
            m_memory.reset_region_contents(static_cast<GuestAddr>(base),
                                           static_cast<usize>(span));
            return base;
        }
        if (m_memory.undo_reset_region_contents(static_cast<GuestAddr>(base),
                                                static_cast<usize>(span))) {
            return base;
        }
        static std::once_flag logged;
        std::call_once(logged, [] {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED kernel32!VirtualAlloc: MEM_RESET_UNDO "
                "last-error after host reclaimed reset pages (undocumented; "
                "returning NULL)\n");
        });
        return 0;
    }

    // Mapped views are not in the private-allocation table. Learn: MEM_RESET
    // fails when the range is mapped to a file; pagefile-backed shared views
    // are acceptable (Windows: pf_reset ok, file_reset → ERROR_USER_MAPPED_FILE).
    if (m_file_mappings != nullptr) {
        const u32 disposition = m_file_mappings->mem_reset_view_disposition(
            static_cast<GuestAddr>(base), span);
        if (disposition == ~u32{0}) {
            return base;
        }
        if (disposition != 0) {
            m_last_alloc_error = disposition;
            return 0;
        }
    }
    m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
    return 0;
}

// -----------------------------------------------------------------------------
// VirtualAlloc
// -----------------------------------------------------------------------------

u64 VirtualMemoryManager::virtual_alloc(u64 address, u64 size,
                                        u32 allocation_type, u32 protect) {
    if ((allocation_type & (MEM_RESET | MEM_RESET_UNDO)) != 0) {
        return virtual_alloc_reset(address, size, allocation_type, protect);
    }
    if (size == 0 ||
        virtual_alloc_flag_error(allocation_type, protect) != 0 ||
        !is_supported_win32_protection(protect)) {
        if ((allocation_type & MEM_COMMIT) != 0 &&
            (allocation_type & MEM_RESERVE) == 0 &&
            address != 0) {
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL args addr=0x%llx size=0x%llx "
                "type=0x%x prot=0x%x\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(size),
                allocation_type, protect);
        }
        return 0;
    }

    if (address > m_maximum_address ||
        size - 1 > m_maximum_address - address) {
        if ((allocation_type & MEM_COMMIT) != 0 &&
            (allocation_type & MEM_RESERVE) == 0) {
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL range addr=0x%llx size=0x%llx "
                "max=0x%llx\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(m_maximum_address));
        }
        return 0;
    }

    // A commit Windows would charge against the commit limit fails with
    // ERROR_COMMITMENT_LIMIT once that limit is reached. Windows charges
    // commit against RAM + pagefile; the macOS analogue is exhausted only
    // when the kernel reports critical pressure (host_memory_floor.hpp).
    // Small commits pass -- refusing a stack guard page would only move the
    // failure somewhere undocumented.
    constexpr u64 kFloorCheckedCommit = u64{1} << 20;
    if ((allocation_type & MEM_COMMIT) != 0 && size >= kFloorCheckedCommit &&
        !vitreo::memory::host_can_commit(size, "guest MEM_COMMIT")) {
        m_last_alloc_error = error_codes::ERROR_COMMITMENT_LIMIT;
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    constexpr u64 ALLOCATION_GRANULARITY = 0x10000; // 64 KB

    // Reservations use allocation granularity; commit-only requests use page
    // granularity. Grow the size so the caller's requested end remains covered.
    if (address != 0) {
        const u64 alignment = (allocation_type & MEM_RESERVE)
                                  ? ALLOCATION_GRANULARITY
                                  : static_cast<u64>(GUEST_PAGE_SIZE);
        const u64 offset = address & (alignment - 1);
        if (offset != 0) {
            address -= offset;
            size += offset;
        }
    }

    // Align to guest page granularity (4 KB).  The host mmap may round up
    // further (e.g. 16 KB on ARM64 macOS), but guest address space tracking
    // should use the smaller guest-page-aligned size to avoid exhausting
    // the 4 GB 32-bit address space.
    const u64 aligned_size = align_up(size, static_cast<u64>(GUEST_PAGE_SIZE));
    if (aligned_size == 0) {
        return 0;
    }

    // A non-null MEM_COMMIT request whose range is already reserved commits
    // inside that reservation rather than creating a second allocation.
    //
    // This is limited to MEM_COMMIT without MEM_RESERVE. Microsoft Learn
    // distinguishes the legal operations precisely: MEM_COMMIT may target
    // already-reserved (or already-committed) pages, while a simultaneous
    // MEM_COMMIT|MEM_RESERVE reserves and commits FREE pages. VirtualAlloc
    // cannot reserve a reserved page, so a combined request overlapping an
    // existing reservation must fall through and fail the overlap check.
    //
    // A MEM_RESERVE request that is NOT inside an existing reservation still
    // falls through to the normal reserve path below; only MEM_COMMIT-only
    // must fail outright when nothing is reserved (ERROR_INVALID_ADDRESS).
    // (Interior commits keep the caller size — do not expand to granularity.)
    // Decided up front so MEM_RESERVE can fall through to the normal path
    // whenever the request is not wholly inside an existing reservation.
    const bool commit_inside_reservation = [&] {
        if ((allocation_type & MEM_COMMIT) == 0 || address == 0) return false;
        return (allocation_type & MEM_RESERVE) == 0;
    }();

    if (commit_inside_reservation) {
        auto it = find_allocation_locked(address);
        if (it == m_allocations.end()) {
            // A mapped SEC_RESERVE section view is ALSO a reservation. The
            // section reserves the pages; Microsoft Learn (CreateFileMapping,
            // SEC_RESERVE) says they are then committed by calling
            // VirtualAlloc on them. Such a range is owned by the file-mapping
            // manager and never appears in this allocator's table, so without
            // this probe the commit is rejected as unreserved.
            //
            // MEASURED 2026-09-09: coreclr maps its W^X code heap with
            // MapViewOfFileEx and immediately commits inside it. The rejection
            // surfaced as a C++ exception, a five-frame unwind and
            // "Fatal error 0x80131506" (COR_E_EXECUTIONENGINE) 372,558
            // instructions into PowerShell 7 -- with nothing naming memory.
            if (m_mapped_view_probe &&
                m_mapped_view_probe(static_cast<GuestAddr>(address),
                                    aligned_size)) {
                // SEC_RESERVE views need commit_region + protection_runs
                // update (Windows: State MEM_COMMIT only on committed pages).
                if (m_file_mappings != nullptr &&
                    m_file_mappings->commit_reserved_view_pages(
                        static_cast<GuestAddr>(address), aligned_size,
                        protect)) {
                    VITREO_TRACE(
                        "[vmem] MEM_COMMIT inside SEC_RESERVE view addr=0x%llx "
                        "size=0x%llx prot=0x%x\n",
                        static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(aligned_size), protect);
                    return address;
                }
                (void)m_memory.protect_region(
                    static_cast<GuestAddr>(address),
                    static_cast<usize>(aligned_size),
                    win32_prot_to_native(protect));
                VITREO_TRACE(
                    "[vmem] MEM_COMMIT inside mapped view addr=0x%llx "
                    "size=0x%llx prot=0x%x\n",
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(aligned_size), protect);
                return address;
            }
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL no reservation addr=0x%llx size=0x%llx\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(aligned_size));
            return 0;
        }
        const u64 offset = address - it->first;
        if (aligned_size > it->second.size - offset) {
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL past reservation addr=0x%llx "
                "size=0x%llx base=0x%llx reserved=0x%llx\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(aligned_size),
                static_cast<unsigned long long>(it->first),
                static_cast<unsigned long long>(it->second.size));
            return 0;
        }

        const u32 stored_protect = stored_win32_protection(protect, false);
        const MemoryProtection native_prot =
            (stored_protect & PAGE_GUARD) != 0
                ? MemoryProtection::None
                : win32_prot_to_native(stored_protect);

        // Temporarily make the range writable so newly committed pages can be
        // zeroed even when the final requested protection is read-only or
        // execute-only. Already committed pages retain their contents.
        if (m_memory.commit_region(static_cast<GuestAddr>(address),
                                   static_cast<usize>(aligned_size),
                                   MemoryProtection::ReadWrite).is_err()) {
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL commit_region addr=0x%llx size=0x%llx\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(aligned_size));
            return 0;
        }

        auto run = it->second.page_run_at(offset);
        if (run == it->second.page_states.end()) {
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL no page state addr=0x%llx "
                "size=0x%llx base=0x%llx\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(aligned_size),
                static_cast<unsigned long long>(it->first));
            return 0;
        }
        u64 cursor = offset;
        while (cursor < offset + aligned_size) {
            const auto next = std::next(run);
            const u64 run_end = std::min(
                offset + aligned_size,
                next == it->second.page_states.end()
                    ? it->second.size
                    : next->first);
            if (!run->second.committed) {
                auto* ptr = m_memory.host_ptr_mut(
                    static_cast<GuestAddr>(it->first + cursor),
                    static_cast<usize>(run_end - cursor));
                if (ptr == nullptr) {
                    std::fprintf(stderr,
                        "[vmem] MEM_COMMIT-only FAIL host_ptr addr=0x%llx "
                        "cursor=0x%llx\n",
                        static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(it->first + cursor));
                    return 0;
                }
                // Fresh host pages are already demand-zero; writing zeros
                // into them would make the whole commit resident up front.
                if (!m_memory.commit_pages_are_fresh(
                        static_cast<GuestAddr>(it->first + cursor),
                        static_cast<usize>(run_end - cursor))) {
                    std::memset(ptr, 0, static_cast<usize>(run_end - cursor));
                }
                if (it->second.write_watch &&
                    !it->second.write_watch_dirty.empty()) {
                    const u64 page_size = static_cast<u64>(GUEST_PAGE_SIZE);
                    for (u64 off = cursor; off < run_end; off += page_size) {
                        write_watch::clear_bit(
                            it->second.write_watch_dirty.data(),
                            off / page_size);
                    }
                    write_watch::clear_linear_pages(it->first + cursor,
                                                    it->first + run_end);
                }
            }
            cursor = run_end;
            if (cursor < offset + aligned_size) {
                run = next;
            }
        }

        if (m_memory.protect_region(static_cast<GuestAddr>(address),
                                    static_cast<usize>(aligned_size),
                                    native_prot).is_err()) {
            std::fprintf(stderr,
                "[vmem] MEM_COMMIT-only FAIL protect addr=0x%llx size=0x%llx "
                "prot=0x%x\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(aligned_size),
                protect);
            return 0;
        }
        set_page_state(it->second, offset, offset + aligned_size,
                       PageState{true, stored_protect});
        sync_guard_arm_locked(it->first, it->second, offset,
                              offset + aligned_size, stored_protect);
        if (it->second.write_watch) {
            arm_write_watch_range_locked(it->first, it->second, address,
                                         address + aligned_size);
        }
        return address;
    }

    // Windows allocation granularity: new reservations (without a specific
    // address) must be aligned to 64 KB.  MEM_COMMIT within an existing
    // reservation can stay 4 KB aligned.
    // Choose an address when the caller does not supply one.
    // Learn: a NULL lpAddress creates a new reservation. Reservations
    // are aligned to dwAllocationGranularity (64 KiB), including
    // MEM_COMMIT without MEM_RESERVE (implicit reserve + commit).
    //
    // Size is still page-rounded only (Learn). The remainder of the
    // 64 KiB paragraph is a wasted free hole on Windows — inaccessible,
    // and not a place for a new reservation base. Do not commit that
    // remainder: contour/grid walks that look behind into zeros there
    // invent neighbor tiles and blow out allocation sizes.
    const u64 auto_alignment = ALLOCATION_GRANULARITY;
    const auto fits_process_address_space = [&](u64 candidate) {
        return candidate != 0 &&
               candidate <= m_maximum_address &&
               aligned_size - 1 <= m_maximum_address - candidate;
    };

    // GuestMemory owns the complete mapping table (images, stacks, TEB,
    // thunks, mapped files, and VirtualAlloc regions), so use it to locate
    // gaps. The search itself is aligned to the 64 KiB granularity: the
    // result is the lowest granularity-aligned base >= start whose whole
    // range is free. A page-aligned search followed by rounding up and
    // re-checking finds the same base, but every small reservation leaves a
    // free, misaligned page tail before the next boundary, so it stepped
    // through those tails one region per iteration -- O(regions) per call,
    // and with UE's thousands of small reservations the single largest
    // runtime cost (it also held m_mutex, stalling VirtualFree on other
    // threads). One aligned search also keeps a single stable key in
    // GuestMemory's first-fit floor cache. The process-specific ceiling is
    // enforced here because the generic GuestMemory table also serves 64-bit
    // and large-address-aware processes; fits_process_address_space is
    // monotone, so checking the first fit is checking every later one.
    const auto find_auto_gap = [&](u64 start) {
        const u64 aligned_start = align_up(start, auto_alignment);
        if (aligned_start < start ||
            !fits_process_address_space(aligned_start)) {
            return u64{0};
        }
        const u64 gap = static_cast<u64>(m_memory.find_free_gap(
            static_cast<GuestAddr>(aligned_start),
            static_cast<usize>(aligned_size),
            static_cast<usize>(auto_alignment)));
        if (gap == 0 || gap % auto_alignment != 0 ||
            !fits_process_address_space(gap)) {
            return u64{0};
        }
        return gap;
    };

    // MEM_TOP_DOWN: place at the highest 64 KiB-aligned free base that fits
    // (Learn / Windows). GuestMemory::find_free_gap only walks upward, so
    // build free extents from the same region+hole table dump_no_gap uses.
    //
    // The process ceiling stays the Windows user limit. A linear reservation
    // smaller than that ceiling is the range one host load can cover; an
    // address above it is a real mapping that the JIT has to take through
    // the checked helper. Search the reservation first so an unhinted
    // top-down reservation stays on the inline path, and spill to the full
    // ceiling only when the reservation cannot hold the request. An explicit
    // lpAddress is not affected.
    const auto find_auto_gap_top_down = [&]() -> u64 {
        std::vector<std::pair<u64, u64>> used;
        used.reserve(m_allocations.size() + 8);
        for (const auto& [lo, hi] : m_memory.region_extents()) {
            used.emplace_back(static_cast<u64>(lo), static_cast<u64>(hi));
        }
        for (const auto& [lo, hi] : m_memory.reserved_holes()) {
            used.emplace_back(static_cast<u64>(lo), static_cast<u64>(hi));
        }
        std::sort(used.begin(), used.end());
        // Cap to the same searchable end GuestMemory::find_free_gap uses —
        // VMM's m_maximum_address alone can sit far above the guest linear
        // ceiling (default GuestMemory search_end = 0xFFFF0000).
        const u64 gm_end = m_memory.maximum_user_address() != 0
            ? static_cast<u64>(m_memory.maximum_user_address()) + 1u
            : 0xFFFF0000ULL;
        const u64 full_ceiling = std::min(m_maximum_address + 1u, gm_end);
        const auto search_below = [&](u64 ceiling) -> u64 {
            std::vector<std::pair<u64, u64>> freegaps;
            u64 cursor = 0x10000;
            for (const auto& [lo, hi] : used) {
                if (lo >= ceiling) {
                    break;
                }
                if (lo > cursor) freegaps.emplace_back(cursor, lo);
                if (hi > cursor) cursor = hi;
            }
            if (cursor < ceiling) freegaps.emplace_back(cursor, ceiling);
            for (auto it = freegaps.rbegin(); it != freegaps.rend(); ++it) {
                const u64 lo = it->first;
                const u64 hi = it->second;
                if (hi <= lo || hi - lo < aligned_size) {
                    continue;
                }
                u64 candidate = (hi - aligned_size) & ~(auto_alignment - 1);
                if (candidate < lo ||
                    !fits_process_address_space(candidate)) {
                    continue;
                }
                const u64 confirmed = static_cast<u64>(
                    m_memory.find_free_gap(
                        static_cast<GuestAddr>(candidate),
                        static_cast<usize>(aligned_size)));
                if (confirmed == candidate) {
                    return candidate;
                }
            }
            return u64{0};
        };
        // VITREO_TOP_DOWN_WINDOW=0 restores a search of the full process
        // ceiling, for a same-build comparison against the inline path.
        static const bool prefer_window =
            environment_flag_enabled("VITREO_TOP_DOWN_WINDOW", true);
        const usize linear = m_memory.linear_size();
        if (prefer_window && linear != 0 &&
            static_cast<u64>(linear) < full_ceiling) {
            const u64 in_window = search_below(static_cast<u64>(linear));
            if (in_window != 0) {
                return in_window;
            }
        }
        return search_below(full_ceiling);
    };

    const auto dump_no_gap = [&]() {
        std::fprintf(stderr, "[vmem] VirtualAlloc FAILED: no gap for %llu bytes"
                     " (%llu allocs, total_mapped=%llu, next_alloc=0x%llx)\n",
                     static_cast<unsigned long long>(aligned_size),
                     static_cast<unsigned long long>(m_allocations.size()),
                     static_cast<unsigned long long>(m_memory.total_mapped_size()),
                     static_cast<unsigned long long>(m_next_alloc));

        std::vector<std::pair<u64,u64>> by_size;
        by_size.reserve(m_allocations.size());
        for (const auto& [a, info] : m_allocations)
            by_size.emplace_back(info.size, a);
        std::sort(by_size.rbegin(), by_size.rend());
        // Free extents matter more than allocation sizes when the question
        // is "why did a contiguous request fail" -- 1.8 GB mapped under a
        // 2 GB ceiling can still have no 87 MB run left.
        {
            // Guest allocations are not the only occupants: reserved holes
            // (host-owned ranges under identity mapping) and Vitreo's own
            // thunk/trampoline pages sit in the same space. Counting only
            // m_allocations reports free runs that do not exist.
            std::vector<std::pair<u64,u64>> used;
            used.reserve(m_allocations.size() + 8);
            // GuestMemory's region table, not m_allocations: images, stacks,
            // the TEB and the thunk region occupy guest space too, and a
            // report built from VirtualAlloc's own list invents free runs.
            for (const auto& [lo, hi] : m_memory.region_extents()) {
                used.emplace_back(static_cast<u64>(lo), static_cast<u64>(hi));
            }
            for (const auto& [lo, hi] : m_memory.reserved_holes()) {
                used.emplace_back(static_cast<u64>(lo), static_cast<u64>(hi));
            }
            std::sort(used.begin(), used.end());
            const u64 ceiling = m_memory.maximum_user_address() != 0
                ? static_cast<u64>(m_memory.maximum_user_address()) + 1u
                : 0x80000000ull;
            std::vector<std::pair<u64,u64>> freegaps;
            u64 cursor = 0x10000;
            for (const auto& [lo, hi] : used) {
                if (lo > cursor) freegaps.emplace_back(cursor, lo);
                if (hi > cursor) cursor = hi;
            }
            if (cursor < ceiling) freegaps.emplace_back(cursor, ceiling);
            std::sort(freegaps.begin(), freegaps.end(),
                      [](const auto& a, const auto& b) {
                          return (a.second - a.first) > (b.second - b.first);
                      });
            u64 total_free = 0;
            for (const auto& [lo, hi] : freegaps) total_free += hi - lo;
            std::fprintf(stderr,
                "[vmem] ceiling=0x%llx  free_total=%llu MB in %zu extents; "
                "largest 8:\n",
                static_cast<unsigned long long>(ceiling),
                static_cast<unsigned long long>(total_free / (1024*1024)),
                freegaps.size());
            for (usize i = 0; i < std::min<usize>(freegaps.size(), 8); ++i) {
                std::fprintf(stderr, "  free 0x%llx-0x%llx  %.1f MB\n",
                    static_cast<unsigned long long>(freegaps[i].first),
                    static_cast<unsigned long long>(freegaps[i].second),
                    static_cast<double>(freegaps[i].second - freegaps[i].first)
                        / (1024.0*1024.0));
            }
        }
        {
            // Ask the allocator directly: if a gap exists but VirtualAlloc
            // still failed, the fault is in placement, not in capacity.
            const u64 probe = static_cast<u64>(
                m_memory.find_free_gap(static_cast<GuestAddr>(0x10000),
                                       static_cast<usize>(aligned_size)));
            std::fprintf(stderr,
                "[vmem] find_free_gap(0x10000, %llu) -> 0x%llx\n",
                static_cast<unsigned long long>(aligned_size),
                static_cast<unsigned long long>(probe));
        }
        std::fprintf(stderr, "[vmem] Top allocations:\n");
        for (usize i = 0; i < std::min<usize>(by_size.size(), 10); ++i)
            std::fprintf(stderr, "  #%zu: 0x%llx  %llu bytes (%.1f MB)\n",
                         i + 1,
                         static_cast<unsigned long long>(by_size[i].second),
                         static_cast<unsigned long long>(by_size[i].first),
                         static_cast<double>(by_size[i].first) / (1024.0 * 1024.0));

        usize buckets[6] = {};
        for (const auto& [sz, _] : by_size) {
            if      (sz < 4096)        buckets[0]++;
            else if (sz < 65536)       buckets[1]++;
            else if (sz < 1048576)     buckets[2]++;
            else if (sz < 16777216)    buckets[3]++;
            else if (sz < 268435456)   buckets[4]++;
            else                       buckets[5]++;
        }
        std::fprintf(stderr, "[vmem] Histogram: <4K=%zu <64K=%zu <1M=%zu <16M=%zu <256M=%zu >=256M=%zu\n",
                     buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
    };

    const bool top_down = (allocation_type & MEM_TOP_DOWN) != 0;
    u64 base = address;
    if (base == 0) {
        // Learn: a NULL lpAddress lets the system choose the address.
        // Default is bottom-up first-fit; MEM_TOP_DOWN picks the highest
        // possible address (Windows: hi >> lo for the same size).
        // Reserved pages stay inaccessible until a later MEM_COMMIT.
        if (top_down) {
            base = find_auto_gap_top_down();
        } else {
            base = find_auto_gap(0x00100000);
            if (base == 0) {
                base = find_auto_gap(m_next_alloc);
            }
        }
        if (base == 0) {
            dump_no_gap();
            return 0;
        }
    }

    // A null MEM_COMMIT request implicitly creates its reservation. A
    // reservation-only request occupies the guest address range but remains
    // inaccessible until a later MEM_COMMIT.
    const bool initially_committed = (allocation_type & MEM_COMMIT) != 0;

    // Visibility into large reservations and the failure paths below: a null
    // VirtualAlloc return is a fatal out-of-memory (RaiseException
    // 0xE0000008).
    if (aligned_size >= (64ull * 1024 * 1024)) {
        ipc::ipc_log("VirtualAlloc BIG req size=%llu MB type=0x%x prot=0x%x addr_in=0x%llx -> base=0x%llx",
                     static_cast<unsigned long long>(aligned_size >> 20),
                     allocation_type, protect,
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(base));
    }

    const u32 stored_protect = stored_win32_protection(protect, false);
    const MemoryProtection native_prot =
        initially_committed
            ? ((stored_protect & PAGE_GUARD) != 0
                   ? MemoryProtection::None
                   : win32_prot_to_native(stored_protect))
            : MemoryProtection::None;
    VoidResult result = VoidResult::err(Error::OutOfMemory);
    for (int attempt = 0; attempt < 4096; ++attempt) {
        result = m_memory.map_region(static_cast<GuestAddr>(base),
                                     static_cast<usize>(aligned_size),
                                     native_prot,
                                     initially_committed);
        if (result.is_ok()) {
            break;
        }
        // Common, expected case: caller passed a fixed lpAddress hint that
        // collides with an existing region (a fixed-address multi-megabyte
        // pool probe)
        // or falls outside the linear reservation. Real Windows returns NULL.
        GuestAddr overlap_base = 0;
        usize overlap_size = 0;
        MemoryProtection overlap_prot{};
        const bool overlap = m_memory.query_region(
            static_cast<GuestAddr>(base), overlap_base, overlap_size,
            overlap_prot);
        VITREO_TRACE("[vmem] VirtualAlloc map_region failed at 0x%llx size=%llu\n",
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(aligned_size));
        ipc::ipc_log(
            "VirtualAlloc FAIL: map_region base=0x%llx size=%llu type=0x%x "
            "had_addr=%d err=%d overlap=%d owner=0x%llx owner_size=0x%llx",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(aligned_size),
            allocation_type, address != 0 ? 1 : 0,
            static_cast<int>(result.error()),
            overlap ? 1 : 0,
            static_cast<unsigned long long>(overlap_base),
            static_cast<unsigned long long>(overlap_size));
        // Unhinted: the gap was free when searched, and another thread mapped
        // it before this one could (every first-fit search converges on the
        // same lowest address). Windows fails only when no space is left, so
        // search again against the updated table.
        if (address == 0 && result.error() == Error::AlreadyExists) {
            const u64 again = top_down ? find_auto_gap_top_down()
                                       : find_auto_gap(0x00100000);
            if (again != 0) {
                base = again;
                continue;
            }
        }
        if (address != 0 || !m_memory.is_identity()) {
            if (address == 0) {
                std::fprintf(stderr,
                    "[vmem] VirtualAlloc FAIL unhinted size=%llu type=0x%x\n",
                    static_cast<unsigned long long>(aligned_size),
                    allocation_type);
            }
            return 0;
        }
        // GuestMemory already recorded the host occupant as a reserved hole.
        // Search again rather than turning a single uncommittable span into a
        // guest OOM (ERROR_NOT_ENOUGH_MEMORY).
        const u64 retry = top_down ? find_auto_gap_top_down()
                                   : find_auto_gap(base + auto_alignment);
        if (retry == 0 || retry == base) {
            std::fprintf(stderr,
                "[vmem] VirtualAlloc FAIL unhinted size=%llu type=0x%x "
                "(no identity retry after 0x%llx)\n",
                static_cast<unsigned long long>(aligned_size),
                allocation_type,
                static_cast<unsigned long long>(base));
            dump_no_gap();
            return 0;
        }
        std::fprintf(stderr,
            "[vmem] identity map failed at 0x%llx size=0x%llx, retrying at 0x%llx\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(aligned_size),
            static_cast<unsigned long long>(retry));
        base = retry;
    }
    if (result.is_err()) {
        return 0;
    }
    if (address == 0) {
        m_next_alloc = base + aligned_size;
    }

    // Record the allocation for later free / protect calls.
    AllocInfo allocation{
        aligned_size,
        stored_protect,
        {{0, PageState{initially_committed,
                       initially_committed ? stored_protect : 0}}},
        false,
        false,
        false,
        0,
        (allocation_type & MEM_WRITE_WATCH) != 0,
    };
    if (allocation.write_watch) {
        const usize bytes =
            static_cast<usize>((aligned_size / GUEST_PAGE_SIZE + 7u) / 8u);
        allocation.write_watch_dirty.assign(bytes, 0);
        allocation.write_watch_armed.assign(bytes, 0);
        write_watch::clear_linear_pages(base, base + aligned_size);
    }
    auto [it, inserted] = m_allocations.emplace(base, std::move(allocation));
    (void)inserted;
    if (it->second.write_watch) {
        publish_write_watch_locked(base, it->second);
        if (initially_committed) {
            arm_write_watch_range_locked(base, it->second, base,
                                         base + aligned_size);
        }
    }
    if (initially_committed && (stored_protect & PAGE_GUARD) != 0) {
        sync_guard_arm_locked(base, it->second, 0, aligned_size, stored_protect);
    }
    // Diagnostic only, and only while the helper-exit counters are on.
    // An address at or above the linear reservation is a real mapping the
    // JIT cannot execute as one host load. The first few say which
    // VirtualAlloc shape produced them.
    if (const usize linear = m_memory.linear_size();
        linear != 0 && base >= static_cast<u64>(linear) &&
        std::getenv("VITREO_JIT_MEM_SLOW_STATS") != nullptr) {
        static std::atomic<int> above_window_logs{0};
        if (above_window_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
            std::fprintf(stderr,
                "[vmem] above-window base=0x%llx size=0x%llx type=0x%x "
                "prot=0x%x addr_in=0x%llx window=0x%zx\n",
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(aligned_size),
                allocation_type, protect,
                static_cast<unsigned long long>(address), linear);
        }
    }
    VITREO_TRACE("[vmem] VirtualAlloc(addr=0x%llx, size=%llu, type=0x%x, prot=0x%x) -> 0x%llx\n",
                 static_cast<unsigned long long>(address),
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned>(allocation_type),
                 static_cast<unsigned>(protect),
                 static_cast<unsigned long long>(base));
    // Large NULL commits are useful when diagnosing allocation/fault
    // correlation. Guests issue them continuously. Never put an
    // unconditional stdio write on VirtualAlloc's production path.
    if (address == 0 && size >= 0x100000u) {
        VITREO_TRACE(
            "[vmem] VirtualAlloc(NULL, size=0x%llx, type=0x%x, prot=0x%x) -> 0x%llx "
            "(aligned=0x%llx)\n",
            static_cast<unsigned long long>(size),
            static_cast<unsigned>(allocation_type),
            static_cast<unsigned>(protect),
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(aligned_size));
    }
    if (base >= 0x10000000ull) {

        VITREO_TRACE(
            "[vmem] VirtualAlloc HIGH base=0x%llx size=0x%llx type=0x%x prot=0x%x\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(aligned_size),
            static_cast<unsigned>(allocation_type),
            static_cast<unsigned>(protect));
    }
    // Two rare-but-decisive shapes for the marker-gated trace: a fixed
    // address (an allocator re-taking a range it believes it owns) and
    // executable memory (a JIT's code arena). Both are the allocations that,
    // when two allocators disagree about a range, turn heap data into code.
    if (address != 0 || (protect & 0xF0u) != 0) {
        ipc::ipc_log("VirtualAlloc addr_in=0x%llx size=0x%llx type=0x%x prot=0x%x -> 0x%llx",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(aligned_size),
                     allocation_type, protect,
                     static_cast<unsigned long long>(base));
    }
    return base;
}

} // namespace vitreo::winapi
