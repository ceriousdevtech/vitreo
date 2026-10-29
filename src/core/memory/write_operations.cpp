// SPDX-License-Identifier: LGPL-2.1-only

/// @file guest_memory.cpp
/// @brief Implementation of GuestMemory

#include "guest_memory.hpp"
#include "mem_watch.hpp"
#include "write_watch_traps.hpp"

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

/// @file write_operations.cpp
/// @brief Write Operations
///
/// Closed cut from guest_memory.cpp — verified with plan_extraction.py.

namespace vitreo {

// =============================================================================
// Write Operations
// =============================================================================

WriteResult GuestMemory::write_u8(GuestAddr addr, u8 value) {
    bool writable = false;
    bool executable = false;
    auto* p = resolve_addr_mut(addr, 1, &writable, &executable);
    if (!p) return WriteResult::err(Error::InvalidAddress);
    if (!writable) return WriteResult::err(Error::ProtectionFault);
    if (write_watch::any_active()) {
        write_watch::record_store(addr, 1);
    }
    if (mem_watch_enabled() || store_ring_enabled()) {
        mem_watch_check(addr, 1, value, "interp.u8");
    }
    *p = value;
    if (executable) {
        notify_executable_write(addr, 1);
    }
    return WriteResult::ok();
}

WriteResult GuestMemory::write_u16(GuestAddr addr, u16 value) {
    bool writable = false;
    bool executable = false;
    auto* p = resolve_addr_mut(addr, sizeof(u16), &writable, &executable);
    if (!p) return write_bytes(addr, reinterpret_cast<const Byte*>(&value), sizeof(value));
    if (!writable) return WriteResult::err(Error::ProtectionFault);
    if (write_watch::any_active()) {
        write_watch::record_store(addr, sizeof(u16));
    }
    if (mem_watch_enabled() || store_ring_enabled()) {
        mem_watch_check(addr, sizeof(u16), value, "interp.u16");
    }
    std::memcpy(p, &value, sizeof(u16));
    if (executable) {
        notify_executable_write(addr, sizeof(u16));
    }
    return WriteResult::ok();
}

WriteResult GuestMemory::write_u32(GuestAddr addr, u32 value) {
    bool writable = false;
    bool executable = false;
    auto* p = resolve_addr_mut(addr, sizeof(u32), &writable, &executable);
    if (!p) return write_bytes(addr, reinterpret_cast<const Byte*>(&value), sizeof(value));
    if (!writable) return WriteResult::err(Error::ProtectionFault);
    if (write_watch::any_active()) {
        write_watch::record_store(addr, sizeof(u32));
    }
    if (mem_watch_enabled() || store_ring_enabled()) {
        mem_watch_check(addr, sizeof(u32), value, "interp.u32");
    }
    std::memcpy(p, &value, sizeof(u32));
    if (executable) {
        notify_executable_write(addr, sizeof(u32));
    }
    return WriteResult::ok();
}

WriteResult GuestMemory::write_u64(GuestAddr addr, u64 value) {
    bool writable = false;
    bool executable = false;
    auto* p = resolve_addr_mut(addr, sizeof(u64), &writable, &executable);
    if (!p) return write_bytes(addr, reinterpret_cast<const Byte*>(&value), sizeof(value));
    if (!writable) return WriteResult::err(Error::ProtectionFault);
    if (write_watch::any_active()) {
        write_watch::record_store(addr, sizeof(u64));
    }
    if (mem_watch_enabled() || store_ring_enabled()) {
        mem_watch_check(addr, sizeof(u64), value, "interp.u64");
    }
    std::memcpy(p, &value, sizeof(u64));
    if (executable) {
        notify_executable_write(addr, sizeof(u64));
    }
    return WriteResult::ok();
}

WriteResult GuestMemory::write_bytes(GuestAddr addr, const Byte* buffer, usize size) {
    if (buffer == nullptr || size == 0) {
        return WriteResult::err(Error::InvalidArgument);
    }

    // Same TLB resolve as write_u32 / read_bytes. The previous locked
    // find_region walk serialized every JIT SSE m128 store miss (and the
    // interpreter's write_xmm_mem) on m_regions_mutex. Guest-visible
    // outcomes stay: InvalidArgument / InvalidAddress / ProtectionFault,
    // write-watch, and executable-write notify.
    bool writable = false;
    bool executable = false;
    auto* p = resolve_addr_mut(addr, size, &writable, &executable);
    if (!p) {
        if (size - 1 > std::numeric_limits<GuestAddr>::max() - addr) {
            return WriteResult::err(Error::InvalidAddress);
        }
        // A guest buffer may span adjacent allocations whose host backing is
        // not contiguous. Validate every span before modifying any byte.
        struct Chunk { Byte* host; usize size; };
        std::vector<Chunk> chunks;
        {
            std::shared_lock lock(m_regions_mutex);
            usize done = 0;
            while (done < size) {
                const GuestAddr cursor = addr + done;
                auto* mapped = find_region(cursor);
                if (!mapped) return WriteResult::err(Error::InvalidAddress);
                const usize offset = static_cast<usize>(cursor - mapped->base);
                if (offset >= mapped->requested_size) {
                    return WriteResult::err(Error::InvalidAddress);
                }
                const auto span = protection_span(*mapped, offset);
                if (!has_flag(span.protection, MemoryProtection::Write)) {
                    return WriteResult::err(Error::ProtectionFault);
                }
                const usize count = std::min({size - done,
                    mapped->requested_size - offset, span.end - offset});
                chunks.push_back({mapped->region.ptr_at(offset), count});
                executable |= has_flag(span.protection, MemoryProtection::Execute);
                done += count;
            }
            if (write_watch::any_active()) write_watch::record_store(addr, size);
            done = 0;
            for (const auto& chunk : chunks) {
                std::memcpy(chunk.host, buffer + done, chunk.size);
                done += chunk.size;
            }
        }
        if (mem_watch_enabled() || store_ring_enabled()) mem_watch_bytes(addr, buffer, size, "interp.bytes");
        if (executable) notify_executable_write(addr, size);
        return WriteResult::ok();
    }
    if (!writable) return WriteResult::err(Error::ProtectionFault);
    if (write_watch::any_active()) {
        write_watch::record_store(addr, size);
    }
    std::memcpy(p, buffer, size);
    if (mem_watch_enabled() || store_ring_enabled()) {
        mem_watch_bytes(addr, buffer, size, "interp.bytes");
    }
    if (executable) {
        notify_executable_write(addr, size);
    }
    return WriteResult::ok();
}

} // namespace vitreo
