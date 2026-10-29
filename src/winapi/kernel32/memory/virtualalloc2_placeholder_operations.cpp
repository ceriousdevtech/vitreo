// SPDX-License-Identifier: LGPL-2.1-only

/// @file memory.cpp
/// @brief VirtualAlloc / VirtualFree / VirtualProtect implementation (WIN32-020)

#include "winapi/kernel32/memory/memory.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

/// @file virtualalloc2_placeholder_operations.cpp
/// @brief VirtualAlloc2 placeholder operations
///
/// Closed cut from memory.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// VirtualAlloc2 placeholder operations
// -----------------------------------------------------------------------------

u64 VirtualMemoryManager::virtual_alloc2(u64 address, u64 size,
                                         u32 allocation_type, u32 protect) {
    // Only reservations need an allocation-granularity base, and only
    // placeholder ranges must be whole pages; a plain reserve/commit size is
    // rounded up like VirtualAlloc (mem_reset_probe "VirtualAlloc2 types").
    constexpr u64 kAllocationGranularity = 0x10000;
    const bool placeholder_op =
        (allocation_type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)) != 0;
    if (size == 0 || (placeholder_op && (size % GUEST_PAGE_SIZE) != 0) ||
        (address != 0 && (allocation_type & MEM_RESERVE) != 0 &&
         (address % kAllocationGranularity) != 0)) {
        return 0;
    }

    if ((allocation_type & ~MEM_TOP_DOWN) ==
        (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)) {
        // Microsoft Learn: placeholder reservations require PAGE_NOACCESS.
        if (protect != PAGE_NOACCESS) {
            return 0;
        }
        const u64 base = virtual_alloc(
            address, size, MEM_RESERVE | (allocation_type & MEM_TOP_DOWN),
            PAGE_NOACCESS);
        if (base == 0) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_allocations.find(base);
        if (it == m_allocations.end()) {
            return 0;
        }
        it->second.placeholder = true;
        it->second.placeholder_group = m_next_placeholder_group++;
        return base;
    }

    const bool replace_placeholder =
        allocation_type == (MEM_RESERVE | MEM_REPLACE_PLACEHOLDER) ||
        allocation_type ==
            (MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER);
    if (replace_placeholder) {
        if (address == 0 || !is_supported_win32_protection(protect)) {
            return 0;
        }
        const bool commit = (allocation_type & MEM_COMMIT) != 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_allocations.find(address);
        if (it == m_allocations.end() || !it->second.placeholder ||
            it->second.size != size || it->second.internal) {
            return 0;
        }

        const u64 group = it->second.placeholder_group;
        if (m_memory.unmap_region(static_cast<GuestAddr>(address),
                                  static_cast<usize>(size)).is_err()) {
            return 0;
        }
        if (m_memory.map_region(static_cast<GuestAddr>(address),
                                static_cast<usize>(size),
                                commit ? win32_prot_to_native(protect)
                                       : MemoryProtection::None,
                                commit).is_err()) {
            // Preserve the original address-space reservation on a host-side
            // binding failure; Windows placeholder replacement is atomic.
            (void)m_memory.map_region(static_cast<GuestAddr>(address),
                                      static_cast<usize>(size),
                                      MemoryProtection::None,
                                      /*committed=*/false);
            return 0;
        }

        it->second = AllocInfo{
            size,
            protect,
            {{0, PageState{commit, commit ? protect : 0}}},
            false,
        };
        it->second.replaced_placeholder = true;
        it->second.placeholder_group = group;
        return address;
    }

    // Without placeholders and extended parameters, VirtualAlloc2 is the same
    // local reserve/commit contract as VirtualAlloc, including MEM_TOP_DOWN,
    // MEM_WRITE_WATCH and MEM_PHYSICAL (validated by the thunk).
    if (placeholder_op) {
        return 0;
    }
    return virtual_alloc(address, size, allocation_type, protect);
}

} // namespace vitreo::winapi
