// SPDX-License-Identifier: LGPL-2.1-only

/// @file memory.cpp
/// @brief VirtualAlloc / VirtualFree / VirtualProtect implementation (WIN32-020)

#include "core/memory/exec_write_source.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

/// @file virtualfree.cpp
/// @brief VirtualFree
///
/// Closed cut from memory.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// VirtualFree
// -----------------------------------------------------------------------------

bool VirtualMemoryManager::virtual_free(u64 address, u64 size, u32 free_type) {
    const bool ok = virtual_free_locked(address, size, free_type);
    ipc::ipc_log("VirtualFree addr=0x%llx size=0x%llx type=0x%x -> %d",
                 static_cast<unsigned long long>(address),
                 static_cast<unsigned long long>(size), free_type, ok ? 1 : 0);
    return ok;
}

bool VirtualMemoryManager::virtual_free_locked(u64 address, u64 size, u32 free_type) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // VirtualFree accepts exactly one operation. Treating unknown or combined
    // flags as one of the known operations can release an allocation even
    // though Windows would reject the request.
    if (free_type != MEM_RELEASE && free_type != MEM_DECOMMIT &&
        free_type != (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
        free_type != (MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        return false;
    }

    if (free_type == (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        // A private allocation created with MEM_REPLACE_PLACEHOLDER is freed
        // back to its placeholder by naming that exact replaced range. This
        // differs from plain MEM_RELEASE, whose size must be zero.
        auto replacement = m_allocations.find(address);
        if (replacement != m_allocations.end() &&
            replacement->second.replaced_placeholder) {
            if (replacement->second.internal ||
                size != replacement->second.size) {
                return false;
            }
            const u64 allocation_size = replacement->second.size;
            const u64 group = replacement->second.placeholder_group;
            if (m_memory.unmap_region(static_cast<GuestAddr>(address),
                                      static_cast<usize>(allocation_size)).is_err() ||
                m_memory.map_region(static_cast<GuestAddr>(address),
                                    static_cast<usize>(allocation_size),
                                    MemoryProtection::None,
                                    /*committed=*/false).is_err()) {
                return false;
            }
            replacement->second = AllocInfo{
                allocation_size,
                PAGE_NOACCESS,
                {{0, PageState{false, 0}}},
                false,
            };
            replacement->second.placeholder = true;
            replacement->second.placeholder_group = group;
            return true;
        }

        // A non-zero request splits one placeholder into exactly two pieces.
        // The specified range must be a prefix or suffix; a middle range would
        // create three placeholders and is not the documented split operation.
        if ((address % GUEST_PAGE_SIZE) != 0 ||
            (size % GUEST_PAGE_SIZE) != 0) {
            return false;
        }
        auto it = find_allocation_locked(address);
        if (it == m_allocations.end() || !it->second.placeholder ||
            it->second.internal || size >= it->second.size) {
            return false;
        }
        const u64 original_base = it->first;
        const u64 original_size = it->second.size;
        if (address != original_base && address + size != original_base + original_size) {
            return false;
        }
        const u64 group = it->second.placeholder_group;
        const u64 split = address == original_base ? address + size : address;
        const u64 first_size = split - original_base;
        const u64 second_size = original_size - first_size;
        if (m_memory.unmap_region(static_cast<GuestAddr>(original_base),
                                  static_cast<usize>(original_size)).is_err()) {
            return false;
        }
        if (m_memory.map_region(static_cast<GuestAddr>(original_base),
                                static_cast<usize>(first_size),
                                MemoryProtection::None, false).is_err() ||
            m_memory.map_region(static_cast<GuestAddr>(split),
                                static_cast<usize>(second_size),
                                MemoryProtection::None, false).is_err()) {
            return false;
        }
        m_allocations.erase(it);
        AllocInfo first{
            first_size, PAGE_NOACCESS,
            {{0, PageState{false, 0}}}, false,
        };
        first.placeholder = true;
        first.placeholder_group = group;
        AllocInfo second{
            second_size, PAGE_NOACCESS,
            {{0, PageState{false, 0}}}, false,
        };
        second.placeholder = true;
        second.placeholder_group = group;
        m_allocations.emplace(original_base, std::move(first));
        m_allocations.emplace(split, std::move(second));
        return true;
    }

    if (free_type == (MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        if (size == 0 || (address % GUEST_PAGE_SIZE) != 0 ||
            (size % GUEST_PAGE_SIZE) != 0 ||
            address > std::numeric_limits<u64>::max() - size) {
            return false;
        }
        const u64 end = address + size;
        auto first = m_allocations.find(address);
        if (first == m_allocations.end() || !first->second.placeholder) {
            return false;
        }
        const u64 group = first->second.placeholder_group;
        u64 cursor = address;
        usize count = 0;
        auto it = first;
        while (cursor < end && it != m_allocations.end() &&
               it->first == cursor && it->second.placeholder &&
               it->second.placeholder_group == group) {
            cursor += it->second.size;
            ++count;
            ++it;
        }
        if (cursor != end || count < 2) {
            return false;
        }
        auto erase_it = first;
        for (usize index = 0; index < count; ++index) {
            if (m_memory.unmap_region(
                    static_cast<GuestAddr>(erase_it->first),
                    static_cast<usize>(erase_it->second.size)).is_err()) {
                return false;
            }
            ++erase_it;
        }
        m_allocations.erase(first, erase_it);
        if (m_memory.map_region(static_cast<GuestAddr>(address),
                                static_cast<usize>(size),
                                MemoryProtection::None, false).is_err()) {
            return false;
        }
        AllocInfo combined{
            size, PAGE_NOACCESS,
            {{0, PageState{false, 0}}}, false,
        };
        combined.placeholder = true;
        combined.placeholder_group = group;
        m_allocations.emplace(address, std::move(combined));
        return true;
    }

    if (free_type == MEM_RELEASE) {
        // MEM_RELEASE: size must be 0; release the entire allocation.
        if (size != 0) {
            return false;
        }
        auto it = m_allocations.find(address);
        if (it == m_allocations.end()) {
            return false;
        }

        // A Win32 heap segment is not a caller-visible VirtualAlloc region, so
        // Windows fails this call rather than unmapping the segment. Vitreo's
        // heap slabs live in this same map and a slab's first block is handed
        // to the guest at the slab base, so honoring the release here would
        // destroy every other live block in the slab.
        if (it->second.internal) {
            VITREO_TRACE("[vmem] VirtualFree(addr=0x%llx, MEM_RELEASE) refused:"
                         " address is a heap segment, not a VirtualAlloc region\n",
                         static_cast<unsigned long long>(address));
            return false;
        }

        auto result = m_memory.unmap_region(static_cast<GuestAddr>(address),
                                            static_cast<usize>(it->second.size));
        if (result.is_err()) {
            return false;
        }

        VITREO_TRACE("[vmem] VirtualFree(addr=0x%llx, type=0x%x, size=%llu)\n",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned>(free_type),
                     static_cast<unsigned long long>(it->second.size));
        if (address >= 0x10000000ull) {
            VITREO_TRACE(
                "[vmem] VirtualFree HIGH addr=0x%llx size=0x%llx type=0x%x\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(it->second.size),
                static_cast<unsigned>(free_type));
        }
        unpublish_write_watch_locked(address, it->second);
        guard_page::unregister_region(address);
        m_allocations.erase(it);
        // Prefer the newly released low gap for the next automatic allocation.
        // Besides limiting fragmentation, this prevents short-lived scratch
        // reservations from walking monotonically to the 32-bit user/kernel
        // boundary, as Storm does once per rendered frame.
        m_next_alloc = std::min(m_next_alloc, address);
        return true;
    }

    if (free_type == MEM_DECOMMIT) {
        if (size == 0) {
            // Microsoft VirtualFree: MEM_DECOMMIT with size zero decommits the
            // entire allocation only when address names its allocation base.
            auto whole = m_allocations.find(address);
            if (whole == m_allocations.end() || whole->second.placeholder) {
                return false;
            }
            size = whole->second.size;
        }
        if (address > std::numeric_limits<u64>::max() - size) {
            return false;
        }
        const u64 page_base =
            align_down(address, static_cast<u64>(GUEST_PAGE_SIZE));
        const u64 page_end =
            align_up(address + size, static_cast<u64>(GUEST_PAGE_SIZE));
        if (page_end <= page_base) {
            return false;
        }

        // A caller decommits subranges inside large reservations. The entire page-rounded range must belong to one
        // reservation; Windows does not silently clamp an oversized request.
        auto it = find_allocation_locked(page_base);
        if (it == m_allocations.end() ||
            page_end - it->first > it->second.size) {
            return false;
        }

        const u64 begin_offset = page_base - it->first;
        const u64 decommit_size = page_end - page_base;
        auto run = it->second.page_run_at(begin_offset);
        if (run == it->second.page_states.end()) {
            return false;
        }
        u64 cursor = begin_offset;
        while (cursor < begin_offset + decommit_size) {
            const auto next = std::next(run);
            const u64 run_end = std::min(
                begin_offset + decommit_size,
                next == it->second.page_states.end()
                    ? it->second.size
                    : next->first);
            if (run->second.committed) {
                const GuestAddr segment =
                    static_cast<GuestAddr>(it->first + cursor);
                const usize segment_size =
                    static_cast<usize>(run_end - cursor);
                if (m_memory.protect_region(
                        segment, segment_size,
                        MemoryProtection::ReadWrite).is_err()) {
                    return false;
                }
                auto* ptr = m_memory.host_ptr_mut(segment, segment_size);
                if (ptr == nullptr) {
                    return false;
                }
                std::memset(ptr, 0, segment_size);
            }
            cursor = run_end;
            if (cursor < begin_offset + decommit_size) {
                run = next;
            }
        }

        if (m_memory.decommit_region(
                static_cast<GuestAddr>(page_base),
                static_cast<usize>(decommit_size)).is_err()) {
            return false;
        }

        // The temporary ReadWrite protection used to clear committed pages
        // suppresses the normal executable-write hook. Decommit still makes
        // every translation covering the range stale, even though the guest
        // can no longer access those pages until it recommits them.
        exec_write_source::Scope tag(exec_write_source::kDecommit);
        m_memory.notify_executable_write(
            static_cast<GuestAddr>(page_base),
            static_cast<usize>(decommit_size));

        set_page_state(it->second, begin_offset,
                       begin_offset + decommit_size,
                       PageState{false, 0});
        if (it->second.write_watch && !it->second.write_watch_dirty.empty()) {
            const u64 page_size = static_cast<u64>(GUEST_PAGE_SIZE);
            for (u64 off = begin_offset; off < begin_offset + decommit_size;
                 off += page_size) {
                write_watch::clear_bit(it->second.write_watch_dirty.data(),
                                       off / page_size);
                write_watch::clear_bit(it->second.write_watch_armed.data(),
                                       off / page_size);
            }
        }

        VITREO_TRACE("[vmem] VirtualFree DECOMMIT(addr=0x%llx, size=%llu)"
                     " — pages reserved and inaccessible\n",
                     static_cast<unsigned long long>(page_base),
                     static_cast<unsigned long long>(decommit_size));
        return true;
    }

    return false;
}

bool VirtualMemoryManager::discard_virtual_memory(u64 address, u64 size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (size == 0 || (address % GUEST_PAGE_SIZE) != 0 ||
        (size % GUEST_PAGE_SIZE) != 0 ||
        address > std::numeric_limits<u64>::max() - size) {
        return false;
    }

    auto allocation = find_allocation_locked(address);
    if (allocation != m_allocations.end()) {
        if (allocation->second.placeholder ||
            address + size > allocation->first + allocation->second.size) {
            return false;
        }
        const u64 begin_offset = address - allocation->first;
        const u64 end_offset = begin_offset + size;
        auto run = allocation->second.page_run_at(begin_offset);
        if (run == allocation->second.page_states.end()) return false;
        u64 cursor = begin_offset;
        while (cursor < end_offset) {
            if (!run->second.committed ||
                run->second.protection != PAGE_READWRITE) {
                return false;
            }
            const auto next = std::next(run);
            cursor = std::min(
                end_offset,
                next == allocation->second.page_states.end()
                    ? allocation->second.size
                    : next->first);
            if (cursor < end_offset) run = next;
        }
    } else {
        // GuestMemory's native protection collapses PAGE_READWRITE and
        // PAGE_WRITECOPY. Until mapped views retain exact Win32 protection and
        // backing ownership, only VirtualAlloc-owned private memory can meet
        // DiscardVirtualMemory's exact PAGE_READWRITE precondition safely.
        return false;
    }

    return m_memory.discard_region_contents(
        static_cast<GuestAddr>(address), static_cast<usize>(size)).is_ok();
}

bool VirtualMemoryManager::take_placeholder(u64 address, u64 size,
                                            PlaceholderToken& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_allocations.find(address);
    if (it == m_allocations.end() || !it->second.placeholder ||
        it->second.size != size || it->second.internal) {
        return false;
    }
    if (m_memory.unmap_region(static_cast<GuestAddr>(address),
                              static_cast<usize>(size)).is_err()) {
        return false;
    }
    token = PlaceholderToken{address, size, it->second.placeholder_group};
    m_allocations.erase(it);
    return true;
}

bool VirtualMemoryManager::restore_placeholder(const PlaceholderToken& token) {
    if (token.base == 0 || token.size == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_allocations.contains(token.base) ||
        m_memory.map_region(static_cast<GuestAddr>(token.base),
                            static_cast<usize>(token.size),
                            MemoryProtection::None,
                            /*committed=*/false).is_err()) {
        return false;
    }
    AllocInfo placeholder{
        token.size, PAGE_NOACCESS,
        {{0, PageState{false, 0}}}, false,
    };
    placeholder.placeholder = true;
    placeholder.placeholder_group =
        token.group != 0 ? token.group : m_next_placeholder_group++;
    m_allocations.emplace(token.base, std::move(placeholder));
    return true;
}

} // namespace vitreo::winapi
