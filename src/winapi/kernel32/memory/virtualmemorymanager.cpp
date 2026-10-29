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

/// @file virtualmemorymanager.cpp
/// @brief VirtualMemoryManager
///
/// Closed cut from memory.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// VirtualMemoryManager
// =============================================================================

VirtualMemoryManager::VirtualMemoryManager(GuestMemory& memory, u64 maximum_address)
    : m_memory(memory)
    , m_maximum_address(maximum_address) {}

VirtualMemoryManager::AllocationIterator
VirtualMemoryManager::find_allocation_locked(u64 address) {
    auto it = m_allocations.upper_bound(address);
    if (it == m_allocations.begin()) {
        return m_allocations.end();
    }
    --it;
    if (address - it->first >= it->second.size) {
        return m_allocations.end();
    }
    return it;
}

VirtualMemoryManager::ConstAllocationIterator
VirtualMemoryManager::find_allocation_locked(u64 address) const {
    auto it = m_allocations.upper_bound(address);
    if (it == m_allocations.begin()) {
        return m_allocations.end();
    }
    --it;
    if (address - it->first >= it->second.size) {
        return m_allocations.end();
    }
    return it;
}

void VirtualMemoryManager::set_page_state(AllocInfo& allocation,
                                          u64 begin_offset,
                                          u64 end_offset,
                                          PageState state) {
    if (begin_offset >= end_offset || end_offset > allocation.size) {
        return;
    }

    const auto state_at = [&](u64 offset) {
        auto it = allocation.page_run_at(offset);
        if (it == allocation.page_states.end()) {
            return PageState{};
        }
        return it->second;
    };
    const PageState state_after = end_offset < allocation.size
                                      ? state_at(end_offset)
                                      : PageState{};

    allocation.page_states[begin_offset] = state;
    auto erase_begin = allocation.page_states.upper_bound(begin_offset);
    auto erase_end = allocation.page_states.lower_bound(end_offset);
    allocation.page_states.erase(erase_begin, erase_end);
    if (end_offset < allocation.size) {
        allocation.page_states[end_offset] = state_after;
    }

    // All unchanged runs are already coalesced. Only the two boundaries
    // modified above can need merging; do not rescan a fragmented reservation.
    const auto merge_previous = [&](auto it) {
        if (it == allocation.page_states.begin()) return;
        const auto previous = std::prev(it);
        if (previous->second.committed == it->second.committed &&
            previous->second.protection == it->second.protection)
            allocation.page_states.erase(it);
    };
    merge_previous(allocation.page_states.find(begin_offset));
    if (end_offset < allocation.size) {
        merge_previous(allocation.page_states.find(end_offset));
    }
}

bool VirtualMemoryManager::range_is_committed(const AllocInfo& allocation,
                                               u64 begin_offset,
                                               u64 end_offset) {
    if (begin_offset >= end_offset || end_offset > allocation.size) {
        return false;
    }

    auto it = allocation.page_run_at(begin_offset);
    if (it == allocation.page_states.end()) {
        return false;
    }
    u64 cursor = begin_offset;
    while (cursor < end_offset) {
        if (!it->second.committed) {
            return false;
        }
        const auto next = std::next(it);
        const u64 run_end = next == allocation.page_states.end()
                                ? allocation.size
                                : next->first;
        cursor = std::min(run_end, end_offset);
        if (cursor < end_offset) {
            it = next;
        }
    }
    return true;
}

} // namespace vitreo::winapi
