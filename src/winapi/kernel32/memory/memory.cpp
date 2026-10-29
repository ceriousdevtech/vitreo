/// @file memory.cpp
/// @brief VirtualAlloc / VirtualFree / VirtualProtect implementation (WIN32-020)

#include "winapi/kernel32/memory/memory.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

namespace vitreo::winapi {





// -----------------------------------------------------------------------------
// virtual_alloc_internal
// -----------------------------------------------------------------------------

u64 VirtualMemoryManager::virtual_alloc_internal(u64 size, u32 protect) {
    // The runtime's own arenas (heap slabs and large blocks) own whole host
    // pages; see round_up_host_page.
    const u64 base = virtual_alloc(0, round_up_host_page(size),
                                   MEM_COMMIT | MEM_RESERVE, protect);
    if (base == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_allocations.find(base);
    if (it != m_allocations.end()) {
        it->second.internal = true;
    }
    return base;
}

bool VirtualMemoryManager::virtual_free_internal(u64 address) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_allocations.find(address);
        if (it == m_allocations.end() || !it->second.internal) {
            return false;
        }
        // Drop the internal mark so the shared release path below accepts it.
        it->second.internal = false;
    }
    return virtual_free(address, 0, MEM_RELEASE);
}


} // namespace vitreo::winapi
