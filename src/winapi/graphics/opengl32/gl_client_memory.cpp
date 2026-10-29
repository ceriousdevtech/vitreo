// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_client_memory.cpp
/// @brief The client-memory policy of opengl32 and glu32 (gl_client_memory.hpp).

#include "gl_client_memory.hpp"

#include "platform/host_hardware.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace vitreo::winapi::gl {

bool gl_client_range_accessible(const GuestMemory& memory, GuestAddr addr, usize size, MemoryProtection access) {
    if (size == 0) return true;
    u64 last = 0;
    if (__builtin_add_overflow(static_cast<u64>(addr), static_cast<u64>(size) - 1, &last)) return false;
    if (last > kGlLargestUserAddress) return false;
    const GuestAddr ceiling = memory.maximum_user_address();
    if (ceiling != 0 && last > static_cast<u64>(ceiling)) return false;
    // Protection runs one after another; adjacent allocations are crossed.
    u64 cursor = addr;
    while (cursor <= last) {
        GuestAddr base = 0;
        usize run = 0;
        MemoryProtection prot = MemoryProtection::None;
        if (!memory.query_region(static_cast<GuestAddr>(cursor), base, run, prot)) return false;
        if (!has_flag(prot, access) || run == 0) return false;
        u64 end = 0;
        if (__builtin_add_overflow(static_cast<u64>(base), static_cast<u64>(run), &end)) return true;
        if (end <= cursor) return false;
        cursor = end;
    }
    return true;
}

usize gl_working_limit(const GuestMemory* memory) noexcept {
    u64 limit = platform::host_hardware().memory.total_bytes;
    if (limit == 0) limit = std::numeric_limits<u64>::max();
    if (memory != nullptr && memory->maximum_user_address() != 0) {
        limit = std::min<u64>(limit, static_cast<u64>(memory->maximum_user_address()) + 1);
    }
    return static_cast<usize>(std::min<u64>(limit, std::numeric_limits<usize>::max()));
}

bool gl_host_can_allocate(usize bytes) noexcept {
    if (bytes == 0) return true;
    void* probe = ::operator new(bytes, std::nothrow);
    if (probe == nullptr) return false;
    ::operator delete(probe);
    return true;
}

GlClientStatus gl_read_client(const GuestMemory& memory, GuestAddr addr, usize size, MemoryProtection access,
                              std::vector<u8>& storage) {
    storage.clear();
    if (addr == 0) return GlClientStatus::Null;
    if (!gl_client_range_accessible(memory, addr, size, access)) return GlClientStatus::Unreadable;
    // The range is committed guest memory, so its size is one the process
    // could hold; the host may still refuse the copy.
    if (!gl_try_resize(storage, size, &memory)) return GlClientStatus::OutOfMemory;
    if (size != 0 && memory.read_bytes(addr, storage.data(), size).is_err()) {
        storage.clear();
        return GlClientStatus::Unreadable;   // released meanwhile
    }
    return GlClientStatus::Ok;
}

}  // namespace vitreo::winapi::gl
