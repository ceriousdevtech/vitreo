// SPDX-License-Identifier: LGPL-2.1-only

/// @file guest_memory.cpp
/// @brief Implementation of GuestMemory

#include "guest_memory.hpp"
#include "mem_watch.hpp"

#include <vitreo/common.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

/// @file address_translation.cpp
/// @brief Address Translation
///
/// Closed cut from guest_memory.cpp — verified with plan_extraction.py.

namespace vitreo {

// =============================================================================
// Address Translation
// =============================================================================

VResult<Byte*> GuestMemory::translate(GuestAddr guest_addr) {
    std::shared_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return VResult<Byte*>::err(Error::InvalidAddress);
    }
    
    const usize offset = guest_addr - mapped->base;
    return VResult<Byte*>::ok(mapped->region.ptr_at(offset));
}

VResult<const Byte*> GuestMemory::translate(GuestAddr guest_addr) const {
    std::shared_lock lk(m_regions_mutex);
    const MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return VResult<const Byte*>::err(Error::InvalidAddress);
    }
    
    const usize offset = guest_addr - mapped->base;
    return VResult<const Byte*>::ok(mapped->region.ptr_at(offset));
}

bool GuestMemory::is_mapped(GuestAddr guest_addr, usize size) const {
    std::shared_lock lk(m_regions_mutex);
    const MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return false;
    }
    
    const usize offset = guest_addr - mapped->base;
    const usize accessible = mapped->requested_size;
    return offset < accessible && size <= accessible - offset;
}

bool GuestMemory::has_protection(GuestAddr guest_addr, usize size, MemoryProtection prot) const {
    std::shared_lock lk(m_regions_mutex);
    const MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return false;
    }
    
    const usize offset = guest_addr - mapped->base;
    const usize accessible = mapped->requested_size;
    if (offset >= accessible || size > accessible - offset) {
        return false;
    }

    const usize end = offset + size;
    usize cursor = offset;
    while (cursor < end) {
        const ProtectionSpan span = protection_span(*mapped, cursor);
        if (!has_flag(span.protection, prot)) {
            return false;
        }
        cursor = std::min(span.end, end);
    }
    return true;
}

} // namespace vitreo
