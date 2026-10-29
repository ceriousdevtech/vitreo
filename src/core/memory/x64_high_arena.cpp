// SPDX-License-Identifier: LGPL-2.1-only

/// @file x64_high_arena.cpp
/// @brief Host probe behind x64_high_arena_base().

#include "core/memory/x64_high_arena.hpp"

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <atomic>
#include <cstdio>

namespace vitreo::memory {

namespace {

/// True when no host mapping intersects [base, base + span).
///
/// mach_vm_region_recurse reports the first mapping at OR AFTER the address it
/// is given, so a single query answers the whole window: either it finds
/// nothing (the rest of the map is empty), or the mapping it finds starts
/// beyond the window's end.
[[nodiscard]] bool host_window_free(u64 base, u64 span) noexcept {
    mach_vm_address_t addr = base;
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;

    const kern_return_t kr = mach_vm_region_recurse(
        mach_task_self(), &addr, &size, &depth,
        reinterpret_cast<vm_region_recurse_info_t>(&info), &count);

    if (kr != KERN_SUCCESS) {
        // No mapping at or above `base`: the window is past everything the
        // task owns.
        return true;
    }
    return addr >= base + span;
}

u64 probe_anchor() noexcept {
    for (u64 candidate = kX64HighArenaDefaultBase;
         candidate >= kX64HighArenaFloor;
         candidate -= kX64HighArenaStride) {
        if (!host_window_free(candidate, kX64HighArenaSpan)) continue;

        if (candidate != kX64HighArenaDefaultBase) {
            std::fprintf(stderr,
                         "[x64-arena] host owns 0x%llx; guest high arenas "
                         "relocated to 0x%llx (%llu GiB span)\n",
                         static_cast<unsigned long long>(
                             kX64HighArenaDefaultBase),
                         static_cast<unsigned long long>(candidate),
                         static_cast<unsigned long long>(
                             kX64HighArenaSpan >> 30));
        }
        return candidate;
    }

    // Nothing free down to the floor. Return the default so the resulting
    // claim failure is the honest, familiar one rather than a layout no
    // caller expected.
    std::fprintf(stderr,
                 "[x64-arena] no free %llu GiB window between 0x%llx and "
                 "0x%llx; keeping the default anchor\n",
                 static_cast<unsigned long long>(kX64HighArenaSpan >> 30),
                 static_cast<unsigned long long>(kX64HighArenaFloor),
                 static_cast<unsigned long long>(kX64HighArenaDefaultBase));
    return kX64HighArenaDefaultBase;
}

} // namespace

namespace {
std::atomic<u64> g_arena_window{0};
} // namespace

void set_x64_high_arena_window(u64 window_bytes) noexcept {
    g_arena_window.store(window_bytes, std::memory_order_release);
}

u64 x64_high_arena_anchor_for(u64 window_bytes) noexcept {
    // Inside the reservation the guest address is not a host address, so the
    // host map is irrelevant: the reservation itself owns the range.
    if (window_bytes >= kX64HighArenaMinWindow) {
        return (window_bytes - kX64HighArenaWindowHeadroom) &
               ~(kX64HighArenaStride - 1);
    }
    return probe_anchor();
}

u64 x64_high_arena_base() noexcept {
    // Placement happens during process construction and every later consumer
    // (thunk region, dynamic loader, synthetic modules) must agree with it, so
    // the probe runs once and the answer is frozen for the process lifetime.
    static const u64 anchor = x64_high_arena_anchor_for(
        g_arena_window.load(std::memory_order_acquire));
    return anchor;
}

} // namespace vitreo::memory
