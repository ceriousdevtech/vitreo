// SPDX-License-Identifier: LGPL-2.1-only

/// @file early_low_reservation.cpp
/// @brief See early_low_reservation.hpp for why this must run before anything.

#include "core/memory/early_low_reservation.hpp"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace vitreo::memory {

namespace {

struct State {
    std::vector<std::pair<GuestAddr, GuestAddr>> holes;
    bool  active  = false;
    usize claimed = 0;
};

// Function-local so a constructor(101) call can use it before C++ globals
// at default priority have run. A namespace-scope std::vector would still be
// unconstructed at that point.
State& state() {
    static State s;
    return s;
}

constexpr mach_vm_address_t kFourGiB = 0x100000000ull;

const char* vm_tag_name(unsigned tag) {
    switch (tag) {
        case VM_MEMORY_MALLOC:        return "malloc";
        case VM_MEMORY_MALLOC_SMALL:  return "malloc_small";
        case VM_MEMORY_MALLOC_LARGE:  return "malloc_large";
        case VM_MEMORY_MALLOC_HUGE:   return "malloc_huge";
        case VM_MEMORY_MALLOC_TINY:   return "malloc_tiny";
        case VM_MEMORY_REALLOC:       return "realloc";
        case VM_MEMORY_STACK:         return "stack";
        case VM_MEMORY_GUARD:         return "guard";
        case VM_MEMORY_SHARED_PMAP:   return "pmap";
        case VM_MEMORY_DYLIB:         return "dylib";
        case VM_MEMORY_OBJC_DISPATCHERS: return "objc";
        case VM_MEMORY_UNSHARED_PMAP: return "uns_pmap";
        default:                      return "other";
    }
}

void describe_hole(GuestAddr lo, GuestAddr hi) {
    mach_vm_address_t addr = lo;
    mach_vm_size_t sz = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(
        mach_task_self(), &addr, &sz, &depth,
        reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
    if (kr == KERN_SUCCESS) {
        std::fprintf(stderr,
                     "[low-reserve] hole 0x%llx-0x%llx (%llu KiB) "
                     "tag=%u (%s) prot=%x mapped=0x%llx-0x%llx\n",
                     static_cast<unsigned long long>(lo),
                     static_cast<unsigned long long>(hi),
                     static_cast<unsigned long long>((hi - lo) / 1024),
                     info.user_tag, vm_tag_name(info.user_tag),
                     static_cast<unsigned>(info.protection),
                     static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(addr + sz));
    } else {
        std::fprintf(stderr,
                     "[low-reserve] hole 0x%llx-0x%llx (%llu KiB) "
                     "(region query kr=%d)\n",
                     static_cast<unsigned long long>(lo),
                     static_cast<unsigned long long>(hi),
                     static_cast<unsigned long long>((hi - lo) / 1024),
                     static_cast<int>(kr));
    }
}

bool range_already_claimed(mach_vm_address_t lo, mach_vm_address_t hi) {
    mach_vm_address_t cursor = lo;
    while (cursor < hi) {
        mach_vm_address_t addr = cursor;
        mach_vm_size_t sz = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(
            mach_task_self(), &addr, &sz, &depth,
            reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        if (kr != KERN_SUCCESS || sz == 0) return false;
        if (addr > cursor) return false;   // gap — not ours
        if (info.protection != VM_PROT_NONE) return false;
        // PROT_NONE is not enough: malloc zones plant guard pages that look
        // identical until you check the tag. Only our own tag is a claim.
        if (static_cast<unsigned>(info.user_tag) != kGuestIdentityVmTag) {
            return false;
        }
        cursor = addr + sz;
    }
    return true;
}

void release_guest_placeholder() {
    mach_vm_address_t addr = kGuestReserveLo;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(
        mach_task_self(), &addr, &sz, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (kr != KERN_SUCCESS || addr != kGuestReserveLo) return;
    // Already our PROT_NONE claim from a previous call: leave it.
    if (info.protection == VM_PROT_NONE) return;
    kr = mach_vm_deallocate(mach_task_self(), kGuestReserveLo, kGuestReserveSize);
    std::fprintf(stderr,
                 "[low-reserve] released __GUEST 0x%llx-0x%llx kr=%d\n",
                 static_cast<unsigned long long>(kGuestReserveLo),
                 static_cast<unsigned long long>(kGuestReserveLo + kGuestReserveSize),
                 static_cast<int>(kr));
}

void claim_or_split(mach_vm_address_t lo, mach_vm_address_t hi, int depth) {
    if (lo >= hi) return;

    auto& st = state();
    mach_vm_address_t addr = lo;
    const int flags = VM_FLAGS_FIXED |
                      VM_MAKE_TAG(static_cast<int>(kGuestIdentityVmTag));
    if (mach_vm_allocate(mach_task_self(), &addr, hi - lo, flags)
            == KERN_SUCCESS && addr == lo) {
        (void)mach_vm_protect(mach_task_self(), lo, hi - lo, FALSE,
                              VM_PROT_NONE);
        st.claimed += static_cast<usize>(hi - lo);
        return;
    }

    // Already mapped PROT_NONE (this function re-entered, or a prior claim).
    // Treat it as ours rather than a host hole.
    if (range_already_claimed(lo, hi)) {
        st.claimed += static_cast<usize>(hi - lo);
        return;
    }

    constexpr mach_vm_size_t kMinSplit = 0x1000;
    if ((hi - lo) <= kMinSplit || depth > 24) {
        if (!st.holes.empty() && st.holes.back().second == static_cast<GuestAddr>(lo)) {
            st.holes.back().second = static_cast<GuestAddr>(hi);
        } else {
            st.holes.emplace_back(static_cast<GuestAddr>(lo),
                                  static_cast<GuestAddr>(hi));
        }
        return;
    }

    const mach_vm_address_t mid = lo + ((hi - lo) / 2 & ~(kMinSplit - 1));
    claim_or_split(lo, mid, depth + 1);
    claim_or_split(mid, hi, depth + 1);
}

} // namespace

usize reserve_low_guest_space() {
#if !defined(VITREO_ARCH_X86_64)
    return 0;
#else
    auto& st = state();
    if (st.active) return st.claimed;
    st.active = true;

    release_guest_placeholder();
    claim_or_split(kLowReserveStart, kFourGiB, 0);

    // Binary split records a hole when allocate fails, but PROT_NONE malloc
    // guards can sit between a tiny zone and our tag-240 claim without being
    // coalesced into that hole. Any mapping below 4GB that we did not tag is
    // a host occupant -- including PROT_NONE -- and must stay off the guest.
    {
        mach_vm_address_t walk = 0;
        while (walk < kFourGiB) {
            mach_vm_size_t sz = 0;
            natural_t depth = 0;
            vm_region_submap_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
            mach_vm_address_t addr = walk;
            kern_return_t kr = mach_vm_region_recurse(
                mach_task_self(), &addr, &sz, &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
            if (kr != KERN_SUCCESS || addr >= kFourGiB) break;
            if (sz == 0) break;
            if (static_cast<unsigned>(info.user_tag) != kGuestIdentityVmTag) {
                st.holes.emplace_back(
                    static_cast<GuestAddr>(addr),
                    static_cast<GuestAddr>(std::min(addr + sz, kFourGiB)));
            }
            walk = addr + sz;
        }
    }

    std::sort(st.holes.begin(), st.holes.end());
    if (st.holes.size() >= 2) {
        std::vector<std::pair<GuestAddr, GuestAddr>> coalesced;
        coalesced.reserve(st.holes.size());
        for (const auto& hole : st.holes) {
            if (coalesced.empty() || coalesced.back().second < hole.first) {
                coalesced.push_back(hole);
            } else {
                coalesced.back().second =
                    std::max(coalesced.back().second, hole.second);
            }
        }
        st.holes.swap(coalesced);
    }

    std::fprintf(stderr,
                 "[low-reserve] claimed %llu MiB, %zu host holes\n",
                 static_cast<unsigned long long>(st.claimed >> 20),
                 st.holes.size());
    for (const auto& [lo, hi] : st.holes) describe_hole(lo, hi);

    std::fprintf(stderr, "[low-reserve] host mappings below 4GB:\n");
    mach_vm_address_t walk = 0;
    while (walk < kFourGiB) {
        mach_vm_size_t sz = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        mach_vm_address_t addr = walk;
        kern_return_t kr = mach_vm_region_recurse(
            mach_task_self(), &addr, &sz, &depth,
            reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        if (kr != KERN_SUCCESS || addr >= kFourGiB) break;
        if (static_cast<unsigned>(info.user_tag) != kGuestIdentityVmTag) {
            std::fprintf(stderr,
                         "  0x%llx-0x%llx %llu KiB tag=%u (%s) prot=%x\n",
                         static_cast<unsigned long long>(addr),
                         static_cast<unsigned long long>(addr + sz),
                         static_cast<unsigned long long>(sz / 1024),
                         info.user_tag, vm_tag_name(info.user_tag),
                         static_cast<unsigned>(info.protection));
        }
        if (sz == 0) break;
        walk = addr + sz;
    }
    std::fflush(stderr);
    return st.claimed;
#endif
}

bool low_reservation_active() noexcept {
    const auto& st = state();
    return st.active && st.claimed != 0;
}

const std::vector<std::pair<GuestAddr, GuestAddr>>&
low_reservation_holes() noexcept {
    return state().holes;
}

} // namespace vitreo::memory

#if defined(VITREO_ARCH_X86_64)
// Drop __GUEST and claim the low 4GB before C++ static constructors malloc.
__attribute__((constructor(101)))
static void vitreo_claim_low_guest_space_before_statics() {
    (void)vitreo::memory::reserve_low_guest_space();
}
#endif
