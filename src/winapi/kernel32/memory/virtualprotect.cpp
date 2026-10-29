// SPDX-License-Identifier: LGPL-2.1-only

/// @file memory.cpp
/// @brief VirtualAlloc / VirtualFree / VirtualProtect implementation (WIN32-020)

#include "core/memory/exec_write_source.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <vector>

/// @file virtualprotect.cpp
/// @brief VirtualProtect
///
/// Closed cut from memory.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// VirtualProtect
// -----------------------------------------------------------------------------

bool VirtualMemoryManager::virtual_protect(u64 address, u64 size,
                                           u32 new_protect, u32* old_protect) {
    m_last_alloc_error = 0;
    if (m_file_mappings != nullptr) {
        const auto mapped = m_file_mappings->virtual_protect_view(
            static_cast<GuestAddr>(address), static_cast<usize>(size),
            new_protect, old_protect);
        if (mapped == ViewProtectResult::Success) return true;
        if (mapped == ViewProtectResult::InvalidProtection ||
            mapped == ViewProtectResult::Unimplemented) {
            m_last_alloc_error = error_codes::ERROR_INVALID_PARAMETER;
            return false;
        }
        if (mapped != ViewProtectResult::NotOwned) {
            m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);

    if (size == 0 || address > std::numeric_limits<u64>::max() - size) {
        m_last_alloc_error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }
    if (!is_supported_win32_protection(new_protect)) {
        // The WRITECOPY forms are rejected only once the memory is known to
        // be private, and that rejection reports old protect PAGE_NOACCESS;
        // malformed masks leave it untouched (writecopy_protect_probe
        // priv wc / virtual_memory_contract_probe vprot prot3).
        if (is_valid_protect_request(new_protect) && old_protect != nullptr) {
            *old_protect = PAGE_NOACCESS;
        }
        m_last_alloc_error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }

    const u32 stored_protect = stored_win32_protection(new_protect, true);
    // PAGE_GUARD pages are host PROT_NONE until the first access trips them.
    const MemoryProtection native_prot =
        (stored_protect & PAGE_GUARD) != 0
            ? MemoryProtection::None
            : win32_prot_to_native(stored_protect);
    const u64 page_base =
        align_down(address, static_cast<u64>(GUEST_PAGE_SIZE));
    const u64 page_end =
        align_up(address + size, static_cast<u64>(GUEST_PAGE_SIZE));
    if (page_end <= page_base) {
        m_last_alloc_error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }

    // VirtualProtect requires every page in the range to be committed and to
    // belong to one reservation. The old protection is that of the first page.
    auto it = find_allocation_locked(page_base);
    if (it != m_allocations.end()) {
        const u64 begin_offset = page_base - it->first;
        const u64 end_offset = page_end - it->first;
        if (end_offset > it->second.size ||
            !range_is_committed(it->second, begin_offset, end_offset)) {
            m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
            return false;
        }

        auto state = it->second.page_run_at(begin_offset);
        if (state == it->second.page_states.end()) {
            m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
            return false;
        }
        if (old_protect != nullptr) {
            u32 old = state->second.protection;
            // VirtualQuery/old protect report PAGE_GUARD only while armed.
            if (guard_page::is_armed(static_cast<GuestAddr>(page_base))) {
                old |= PAGE_GUARD;
            } else {
                old &= ~PAGE_GUARD;
            }
            *old_protect = old;
        }

        // Making a region executable is when generated/patched bytes become
        // callable: protect_region publishes the JIT invalidation for a host
        // execute transition (pairs with FlushInstructionCache).
        std::optional<exec_write_source::Scope> exec_tag;
        if (is_execute_win32_protection(stored_protect)) {
            exec_tag.emplace(exec_write_source::kProtect);
        }
        auto result = m_memory.protect_region(static_cast<GuestAddr>(page_base),
                                              static_cast<usize>(page_end - page_base),
                                              native_prot);
        if (result.is_err()) {
            return false;
        }

        set_page_state(it->second, begin_offset, end_offset,
                       PageState{true, stored_protect});
        sync_guard_arm_locked(it->first, it->second, begin_offset, end_offset,
                              stored_protect);
        // protect_region just replaced the host protection on this range,
        // dropping whatever PROT_READ arming was on it. The pages can no
        // longer fault, so report them written rather than lose stores; this
        // is a one-off per VirtualProtect, so RESET still drains normally.
        if (it->second.write_watch && write_watch::hardware_traps() &&
            !it->second.write_watch_dirty.empty()) {
            for (u64 off = begin_offset; off < end_offset;
                 off += static_cast<u64>(GUEST_PAGE_SIZE)) {
                const u64 index = off / static_cast<u64>(GUEST_PAGE_SIZE);
                write_watch::set_bit(it->second.write_watch_dirty.data(), index);
                write_watch::clear_bit(it->second.write_watch_armed.data(),
                                       index);
            }
        }
        // An executable PAGE_GUARD range is host PROT_NONE, so protect_region
        // saw no execute transition; publish the invalidation here.
        if (is_execute_win32_protection(stored_protect) &&
            !has_flag(native_prot, MemoryProtection::Execute)) {
            m_memory.notify_executable_write(
                static_cast<GuestAddr>(page_base),
                static_cast<usize>(page_end - page_base));
        }
        return true;
    }

    GuestAddr region_base = 0;
    usize region_size = 0;
    MemoryProtection current_prot = MemoryProtection::None;
    if (!m_memory.query_region(static_cast<GuestAddr>(address),
                               region_base,
                               region_size,
                               current_prot)) {
        m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
        return false;
    }

    // Learn VirtualProtect: "All pages in the specified region must be within
    // the same reserved region allocated when calling the VirtualAlloc or
    // VirtualAllocEx function" — for an image or other runtime mapping that
    // is the whole mapping, not the protection run holding lpAddress, so a
    // range may cross section boundaries (virtual_memory_contract_probe:
    // "img protect whole" / "img protect cross" succeed).
    GuestAddr mapping_base = 0;
    usize mapping_size = 0;
    if (!m_memory.query_mapping(static_cast<GuestAddr>(address), mapping_base,
                                mapping_size) ||
        page_base < mapping_base || page_end > mapping_base + mapping_size) {
        m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
        return false;
    }

    if (old_protect != nullptr) {
        *old_protect = native_prot_to_win32(current_prot);
    }

    std::optional<exec_write_source::Scope> exec_tag;
    if (is_execute_win32_protection(new_protect)) {
        exec_tag.emplace(exec_write_source::kProtect);
    }
    auto result = m_memory.protect_region(static_cast<GuestAddr>(page_base),
                                          static_cast<usize>(page_end - page_base),
                                          native_prot);
    if (result.is_err()) {
        m_last_alloc_error = error_codes::ERROR_INVALID_ADDRESS;
        return false;
    }

    if (is_execute_win32_protection(new_protect) &&
        !has_flag(native_prot, MemoryProtection::Execute)) {
        m_memory.notify_executable_write(
            static_cast<GuestAddr>(page_base),
            static_cast<usize>(page_end - page_base));
    }

    VITREO_TRACE("[vmem] VirtualProtect image(addr=0x%llx, size=0x%llx, new=0x%x, old=0x%x)\n",
                 static_cast<unsigned long long>(address),
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned>(new_protect),
                 static_cast<unsigned>(old_protect ? *old_protect : 0));
    return true;
}

bool VirtualMemoryManager::virtual_query(
    u64 address,
    VirtualAllocationQuery& info) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    const u64 page_base =
        align_down(address, static_cast<u64>(GUEST_PAGE_SIZE));
    const auto allocation = find_allocation_locked(page_base);
    if (allocation == m_allocations.end()) {
        return false;
    }

    const u64 offset = page_base - allocation->first;
    auto state = allocation->second.page_run_at(offset);
    if (state == allocation->second.page_states.end()) {
        return false;
    }
    const auto next = std::next(state);
    const u64 run_end = allocation->first +
        (next == allocation->second.page_states.end()
             ? allocation->second.size
             : next->first);

    info = VirtualAllocationQuery{
        page_base,
        allocation->first,
        run_end - page_base,
        allocation->second.allocation_protection,
        state->second.committed ? MEM_COMMIT : MEM_RESERVE,
        state->second.committed ? state->second.protection : 0,
    };
    // PAGE_GUARD is one-shot: report it only while the page is still armed.
    if (info.state == MEM_COMMIT) {
        if (guard_page::is_armed(static_cast<GuestAddr>(page_base))) {
            info.protect |= PAGE_GUARD;
        } else {
            info.protect &= ~PAGE_GUARD;
        }
    }
    return true;
}

u32 VirtualMemoryManager::get_write_watch(u64 base, u64 size,
                                          u64& count_inout,
                                          u32& granularity_out,
                                          std::vector<u64>& pages_out,
                                          u32 flags) {
    std::lock_guard<std::mutex> lock(m_mutex);
    pages_out.clear();
    if (base == 0 || size == 0) {
        return 1;
    }
    const auto allocation = find_allocation_locked(base);
    if (allocation == m_allocations.end() || !allocation->second.write_watch) {
        return 1;
    }
    if (base + size < base ||
        base + size > allocation->first + allocation->second.size) {
        return 1;
    }

    const u64 page_size = static_cast<u64>(GUEST_PAGE_SIZE);
    const u64 capacity = count_inout;
    const u64 query_end = base + size;
    const u64 alloc_base = allocation->first;
    auto& dirty = allocation->second.write_watch_dirty;
    const u64 pages = allocation->second.size / page_size;
    if (dirty.size() != (pages + 7u) / 8u) {
        dirty.assign((pages + 7u) / 8u, 0);
        allocation->second.write_watch_armed.assign(dirty.size(), 0);
        publish_write_watch_locked(alloc_base, allocation->second);
    }

    // Dirty bits first. A GC-style GetWriteWatch walks a large heap and
    // almost every page is clean; a page_states lookup per page is the
    // cost of reporting "nothing changed". Only dirty candidates then
    // check the committed run (Learn: reserved pages are not reported).
    // JIT inline stores mark the linear map; C++/helper stores mark both.
    // Under native32 nothing executes the store in software, so the same bits
    // are set from the PROT_READ protection fault instead -- see
    // write_watch::enable_hardware_traps. Either way the answer is exact: a
    // superset would make WRITE_WATCH_FLAG_RESET a no-op and hang any caller
    // that drains the buffer in a loop.
    u64 run_begin = ~0ull;
    u64 run_end = 0;
    bool run_committed = false;
    // VITREO_WRITE_WATCH_TRAPS=0 only; see write_watch::stores_untracked.
    const bool report_all = write_watch::stores_untracked();
    for (u64 page = align_down(base, page_size); page < query_end;
         page += page_size) {
        const u64 offset = page - alloc_base;
        const u64 index = offset / page_size;
        if (!report_all && !write_watch::test_bit(dirty.data(), index) &&
            !write_watch::test_linear_page(page)) {
            continue;
        }
        if (offset < run_begin || offset >= run_end) {
            auto state = allocation->second.page_run_at(offset);
            if (state == allocation->second.page_states.end()) {
                continue;
            }
            run_begin = state->first;
            const auto next = std::next(state);
            run_end = (next == allocation->second.page_states.end())
                          ? allocation->second.size
                          : next->first;
            run_committed = state->second.committed;
        }
        if (!run_committed) {
            continue;
        }
        if (pages_out.size() >= capacity) {
            // The caller's array cannot hold the answer; reporting a truncated
            // set would look authoritative while hiding written pages.
            pages_out.clear();
            return 1;
        }
        pages_out.push_back(page);
    }

    if ((flags & WRITE_WATCH_FLAG_RESET) != 0) {
        for (u64 page : pages_out) {
            write_watch::clear_bit(dirty.data(), (page - alloc_base) / page_size);
        }
        write_watch::clear_linear_pages(base, query_end);
        arm_write_watch_range_locked(alloc_base, allocation->second, base,
                                     query_end);
    }

    count_inout = static_cast<u64>(pages_out.size());
    granularity_out = static_cast<u32>(GUEST_PAGE_SIZE);
    // VITREO_LOG_WRITE_WATCH=1. Two things are worth seeing here. A FULL
    // buffer is the caller's signal to come back for more, so a reporter that
    // keeps filling it never lets a drain loop terminate -- rare and
    // self-limiting when tracking is exact, a steady stream on one range is
    // the hang. And `traps` must keep climbing: a dirty set that stays empty
    // while it does not means the stores are not being seen at all.
    static const bool log = std::getenv("VITREO_LOG_WRITE_WATCH") != nullptr;
    if (log) {
        static u64 calls = 0;
        const bool full = pages_out.size() == capacity && capacity != 0;
        if (full || (++calls % 200) == 0) {
            std::fprintf(stderr,
                         "[write-watch] %s base=0x%llx size=0x%llx count=%llu "
                         "cap=%llu traps=%llu\n",
                         full ? "FULL" : "call",
                         static_cast<unsigned long long>(base),
                         static_cast<unsigned long long>(size),
                         static_cast<unsigned long long>(pages_out.size()),
                         static_cast<unsigned long long>(capacity),
                         static_cast<unsigned long long>(
                             write_watch::traps_recovered()));
        }
    }
    return 0;
}

u32 VirtualMemoryManager::reset_write_watch(u64 base, u64 size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (base == 0 || size == 0) {
        return 1;
    }
    const auto allocation = find_allocation_locked(base);
    if (allocation == m_allocations.end()) {
        return 1;
    }
    if (!allocation->second.write_watch) {
        return 1;
    }
    if (base + size < base ||
        base + size > allocation->first + allocation->second.size) {
        return 1;
    }
    const u64 page_size = static_cast<u64>(GUEST_PAGE_SIZE);
    auto& dirty = allocation->second.write_watch_dirty;
    const u64 pages = allocation->second.size / page_size;
    if (dirty.size() != (pages + 7u) / 8u) {
        dirty.assign((pages + 7u) / 8u, 0);
        allocation->second.write_watch_armed.assign(dirty.size(), 0);
        publish_write_watch_locked(allocation->first, allocation->second);
    }
    const u64 alloc_base = allocation->first;
    for (u64 page = align_down(base, page_size); page < base + size;
         page += page_size) {
        write_watch::clear_bit(dirty.data(), (page - alloc_base) / page_size);
    }
    write_watch::clear_linear_pages(base, base + size);
    arm_write_watch_range_locked(alloc_base, allocation->second, base,
                                 base + size);
    return 0;
}

void VirtualMemoryManager::publish_write_watch_locked(u64 base,
                                                      AllocInfo& allocation) {
    if (!allocation.write_watch || allocation.write_watch_dirty.empty()) {
        return;
    }
    write_watch::register_region(
        base, allocation.size, allocation.write_watch_dirty.data(),
        allocation.write_watch_armed.data(),
        is_execute_win32_protection(allocation.allocation_protection));
}

void VirtualMemoryManager::unpublish_write_watch_locked(u64 base,
                                                        AllocInfo& allocation) {
    if (!allocation.write_watch) {
        return;
    }
    write_watch::unregister_region(base);
}

void VirtualMemoryManager::arm_write_watch_range_locked(
    u64 alloc_base, AllocInfo& allocation, u64 begin, u64 end) {
    write_watch::arm_range(
        alloc_base, begin, end, allocation.write_watch_dirty.data(),
        allocation.write_watch_armed.data(),
        is_execute_win32_protection(allocation.allocation_protection));
}

void VirtualMemoryManager::ensure_guard_bits_locked(AllocInfo& allocation) {
    const usize bytes =
        static_cast<usize>((allocation.size / GUEST_PAGE_SIZE + 7u) / 8u);
    if (allocation.guard_page_armed.size() != bytes) {
        allocation.guard_page_armed.assign(bytes, 0);
    }
}

void VirtualMemoryManager::sync_guard_arm_locked(u64 alloc_base,
                                                 AllocInfo& allocation,
                                                 u64 begin_offset,
                                                 u64 end_offset,
                                                 u32 stored_protect) {
    ensure_guard_bits_locked(allocation);
    const u32 restore =
        static_cast<u32>(win32_prot_to_native(stored_protect & ~PAGE_GUARD));
    const u64 begin = alloc_base + begin_offset;
    const u64 end = alloc_base + end_offset;
    if ((stored_protect & PAGE_GUARD) != 0) {
        guard_page::arm_range(alloc_base, allocation.size, begin, end,
                              allocation.guard_page_armed.data(), restore);
    } else {
        guard_page::disarm_range(alloc_base, begin, end,
                                 allocation.guard_page_armed.data(), restore);
    }
}

} // namespace vitreo::winapi
