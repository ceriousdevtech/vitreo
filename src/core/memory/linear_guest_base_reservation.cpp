// SPDX-License-Identifier: LGPL-2.1-only

/// @file guest_memory.cpp
/// @brief Implementation of GuestMemory

#include "guest_memory.hpp"
#include "mem_watch.hpp"
#include "early_low_reservation.hpp"
#include "write_watch_traps.hpp"

#include <vitreo/common.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include <mach-o/loader.h>
#include <crt_externs.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>

/// @file linear_guest_base_reservation.cpp
/// @brief Linear (guest-base) reservation
///
/// Closed cut from guest_memory.cpp — verified with plan_extraction.py.

namespace vitreo {

// =============================================================================
// Linear (guest-base) reservation
// =============================================================================

namespace {
std::atomic<GuestMemory::TeardownHook> g_teardown_hook{nullptr};
std::atomic<GuestMemory::TeardownHook> g_extra_teardown_hooks[4]{};
}  // namespace

void GuestMemory::set_teardown_hook(TeardownHook hook) noexcept {
    g_teardown_hook.store(hook, std::memory_order_release);
}

bool GuestMemory::add_teardown_hook(TeardownHook hook) noexcept {
    for (auto& slot : g_extra_teardown_hooks) {
        TeardownHook expected = nullptr;
        if (slot.load(std::memory_order_acquire) == hook) return true;
        if (slot.compare_exchange_strong(expected, hook,
                                         std::memory_order_acq_rel)) {
            return true;
        }
        if (expected == hook) return true;
    }
    return false;
}

GuestMemory::~GuestMemory() {
    // First, before any page bookkeeping is torn down: a registrant may
    // still release code-page retains into this object.
    if (const TeardownHook hook =
            g_teardown_hook.load(std::memory_order_acquire)) {
        hook(this);
    }
    for (auto& slot : g_extra_teardown_hooks) {
        if (const TeardownHook hook = slot.load(std::memory_order_acquire)) {
            hook(this);
        }
    }
    // The JIT view first: it aliases the reservation's pages and is sized by
    // m_linear_size.
    linear_jit_view_teardown();
    // Borrowed regions (owns()==false) never munmap; the reservation owns the
    // whole address window, so releasing it here frees everything at once.
    if (m_linear_base != nullptr) {
        munmap(m_linear_base, m_linear_size);  // core and extension
        m_linear_base = nullptr;
        m_linear_size = 0;
        m_linear_pow2_size = 0;
        m_linear_ext_materialized.clear();
    }
    if (m_linear_check_map != nullptr) {
        // Before the munmap: a registration arriving after this point must not
        // reach a publisher whose map is gone.
        write_watch::clear_page_publisher(this);
        // The length the allocation used; m_linear_check_count is the number
        // of REAL page entries and would under-free the padded form by a page.
        munmap(m_linear_check_map, m_linear_check_map_bytes);
        m_linear_check_map = nullptr;
        m_linear_check_map_bytes = 0;
        m_linear_check_map_at_va_limit = false;
        if (m_linear_subpage_map != nullptr) {
            munmap(m_linear_subpage_map,
                   m_linear_check_count + kCheckMapPadBytes);
            m_linear_subpage_map = nullptr;
        }
        m_linear_check_count = 0;
    }
}

namespace {

/// The first host mapping at or above @p addr, or @p limit when none.
mach_vm_address_t first_host_occupant_at_or_above(mach_vm_address_t addr,
                                                  mach_vm_address_t limit) {
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count,
                       &object) != KERN_SUCCESS) {
        return limit;
    }
    return std::min<mach_vm_address_t>(a, limit);
}

/// Reserve the extension [base + core, base + span) of a range-by-fault
/// linear reservation and return span, or @p core when nothing could be
/// added. span is the largest multiple of 2^(2*kLinearHostPageShift) (so the
/// check map, one byte per host page, is a whole number of host pages) such
/// that the reservation still ends at or below the map placed to end at the
/// VA limit, less kLinearExtensionHostHeadroom left free for the host:
/// base + span + headroom + span / host_page <= MACH_VM_MAX_ADDRESS. The
/// range must be free -- an existing host mapping is never displaced -- so
/// the span stops at the first host occupant above the core.
///
/// The headroom is the host's: on macOS arm64 anonymous memory cannot be
/// placed between ~64 GiB and the core's base (0x70'0000'0000, where the
/// first 2^46 fit), so without it a 32 GiB host mmap -- or the scattered
/// backing of a guest reservation above the span -- has nowhere to go
/// (measured: mmap(NULL, 32 GiB) fails once the rest is taken). 1 TiB keeps
/// every guest address below 2^47 - ~1.5 TiB on the inline path (~98% of a
/// uniformly random high-half placement) and leaves the host far more than
/// it has been seen to use.
constexpr usize kLinearExtensionHostHeadroom = usize{1} << 40;

usize reserve_linear_extension(u8* base, usize core) {
    const usize hp = usize{1} << kLinearHostPageShift;
    const usize granule = hp * hp;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base);
    if (lo >= MACH_VM_MAX_ADDRESS ||
        MACH_VM_MAX_ADDRESS - lo <= kLinearExtensionHostHeadroom ||
        (core & (granule - 1)) != 0) {
        return core;
    }
    const usize room = static_cast<usize>(MACH_VM_MAX_ADDRESS - lo) -
                       kLinearExtensionHostHeadroom;
    usize span = (room / (hp + 1) * hp) & ~(granule - 1);
    for (int attempt = 0; attempt < 4 && span > core; ++attempt) {
        // A placeholder: maximum protection NONE, no memory object. FIXED
        // without OVERWRITE fails rather than displacing a host mapping.
        // See GuestMemory::m_linear_ext_materialized for why not mmap.
        mach_vm_address_t want =
            reinterpret_cast<mach_vm_address_t>(base + core);
        if (mach_vm_map(mach_task_self(), &want, span - core, 0,
                        VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                        VM_PROT_NONE, VM_PROT_NONE,
                        VM_INHERIT_NONE) == KERN_SUCCESS) {
            return span;
        }
        // Something of the host's already lives above the core: stop below
        // its first mapping instead of displacing it.
        const mach_vm_address_t occupant = first_host_occupant_at_or_above(
            reinterpret_cast<mach_vm_address_t>(base + core),
            reinterpret_cast<mach_vm_address_t>(base + span));
        const usize capped =
            static_cast<usize>(occupant - lo) & ~(granule - 1);
        if (capped >= span) break;  // nothing in the way, yet mmap refused
        span = capped;
    }
    return core;
}

constexpr unsigned kLinearExtensionChunkShift = 30;  // 1 GiB

}  // namespace

bool GuestMemory::linear_extension_materialize(GuestAddr guest_addr,
                                               usize size) {
    if (m_linear_ext_materialized.empty() || size == 0) return true;
    const usize core = m_linear_pow2_size;
    usize lo = static_cast<usize>(guest_addr);
    usize hi = lo + size;
    if (hi < lo || hi <= core) return true;
    lo = std::max(lo, core);
    hi = std::min(hi, m_linear_size);
    if (lo >= hi) return true;
    const usize first = (lo - core) >> kLinearExtensionChunkShift;
    const usize last = (hi - 1 - core) >> kLinearExtensionChunkShift;
    for (usize c = first; c <= last; ++c) {
        u64& word = m_linear_ext_materialized[c >> 6];
        const u64 bit = u64{1} << (c & 63);
        if ((word & bit) != 0) continue;
        const usize chunk_lo = core + (c << kLinearExtensionChunkShift);
        const usize chunk_hi = std::min(
            chunk_lo + (usize{1} << kLinearExtensionChunkShift), m_linear_size);
        // Replaces only our own placeholder: [chunk_lo, chunk_hi) lies inside
        // the extension, which nothing else can map into.
        void* const at = m_linear_base + chunk_lo;
        if (mmap(at, chunk_hi - chunk_lo, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != at) {
            std::fprintf(stderr,
                         "[memory] linear extension chunk 0x%zx could not be "
                         "materialized (errno %d)\n",
                         chunk_lo, errno);
            return false;
        }
        word |= bit;
    }
    return true;
}

bool GuestMemory::enable_linear_reservation(usize size, bool extend_to_va_limit) {
    std::unique_lock lk(m_regions_mutex);
    if (m_linear_base != nullptr) return false;      // already enabled
    if (m_linear_identity) return false;             // identity mapping active
    if (!regions_.empty()) return false;             // must precede any mapping
    if (size == 0) return false;

    void* base = mmap(nullptr, size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        // Non-fatal: caller keeps the scattered model, behavior is identical.
        return false;
    }
    m_linear_base = static_cast<u8*>(base);
    m_linear_size = size;
    m_linear_pow2_size = size;

    // The check-page map belongs to the reservation, not to hardware
    // protection: a store landing on translated code has to reach the
    // invalidating helper under every protection model, and that is the bit
    // that replaced the per-store code-span compare.
    //
    // The map is indexed by a fixed shift so JIT-emitted code can compute the
    // index with one UBFX, so a host page of any other size would silently
    // index the wrong byte. Leave the map absent in that case; the JIT then
    // declines the guest-base path rather than mis-checking.
    if (static_cast<usize>(::getpagesize()) ==
        (usize{1} << kLinearHostPageShift)) {
        const usize host_pages = m_linear_size >> kLinearHostPageShift;
        // One byte of padding past the last real page entry. An access of up
        // to 8 bytes spans at most two ADJACENT host pages, so the JIT's store
        // probe can cover both endpoints with a single halfword load of
        // map[i] and map[i+1] instead of two byte loads plus an index
        // computation and an OR. At the very last page that halfword would
        // otherwise read one byte past the mapping and fault. mmap rounds the
        // length up to a page, but host_pages is itself an exact multiple of
        // the host page size for every power-of-two reservation, so without
        // the explicit +1 there is no slack at all.
        void* map = MAP_FAILED;
        usize map_bytes = 0;
        // Preferred placement: ending exactly at the task's VA limit, with no
        // pad. Then an unmasked index past the last real entry is an address
        // no mapping can ever occupy, and the JIT's range-by-fault guard can
        // drop its per-access reservation test (an out-of-window EA faults on
        // the map load instead; see linear_check_map_ends_at_va_limit). The
        // pad only serves the 32-bit halfword store probe, whose EAs index
        // the first 2^18 entries. VITREO_LINEAR_CHECK_MAP_AT_TOP=0 keeps the
        // ordinary placement.
        static const bool at_top_enabled = [] {
            const char* v = std::getenv("VITREO_LINEAR_CHECK_MAP_AT_TOP");
            return v == nullptr || v[0] != '0';
        }();
        // The extension (x64): the guest's 47-bit user space is twice the
        // 2^46 core, and allocators that pick their own random 47-bit
        // addresses (a fixed lpAddress is honoured exactly) put half their
        // reservations above the core. Backing them at linear_base + addr
        // keeps them on the same inline path. It needs the map at the VA
        // limit (an EA past the extension must fault on the map load), so it
        // is given back below if that placement fails.
        if (extend_to_va_limit && at_top_enabled) {
            m_linear_size = reserve_linear_extension(m_linear_base, size);
        }
        const usize top_pages = m_linear_size >> kLinearHostPageShift;
        if (at_top_enabled && top_pages != 0 &&
            (top_pages % (usize{1} << kLinearHostPageShift)) == 0 &&
            top_pages < MACH_VM_MAX_ADDRESS) {
            mach_vm_address_t at = MACH_VM_MAX_ADDRESS - top_pages;
            if (mach_vm_allocate(mach_task_self(), &at, top_pages,
                                 VM_FLAGS_FIXED) == KERN_SUCCESS) {
                // The proof: nothing can be mapped at the limit itself.
                mach_vm_address_t above = MACH_VM_MAX_ADDRESS;
                const kern_return_t probe = mach_vm_allocate(
                    mach_task_self(), &above,
                    usize{1} << kLinearHostPageShift, VM_FLAGS_FIXED);
                if (probe == KERN_SUCCESS) {
                    // This task's VA extends further: not a fault boundary.
                    (void)mach_vm_deallocate(mach_task_self(), above,
                                             usize{1} << kLinearHostPageShift);
                    (void)mach_vm_deallocate(mach_task_self(), at, top_pages);
                } else {
                    map = reinterpret_cast<void*>(at);
                    map_bytes = top_pages;
                    m_linear_check_map_at_va_limit = true;
                }
            }
        }
        if (map == MAP_FAILED && m_linear_size != size) {
            // No range-by-fault boundary: give the extension back.
            (void)munmap(m_linear_base + size, m_linear_size - size);
            m_linear_size = size;
        }
        if (map == MAP_FAILED && host_pages != 0) {
            map_bytes = host_pages + kCheckMapPadBytes;
            map = mmap(nullptr, map_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        if (map != MAP_FAILED) {
            std::fprintf(stderr,
                         "[memory] linear check map %p..%p (%s)\n", map,
                         static_cast<void*>(static_cast<u8*>(map) + map_bytes),
                         m_linear_check_map_at_va_limit
                             ? "ends at the VA limit: range-by-fault"
                             : "ordinary placement");
            if (m_linear_size != size) {
                const usize chunks =
                    ((m_linear_size - size) +
                     (usize{1} << kLinearExtensionChunkShift) - 1) >>
                    kLinearExtensionChunkShift;
                m_linear_ext_materialized.assign((chunks + 63) / 64, 0);
                std::fprintf(stderr,
                             "[memory] linear reservation extended: guest "
                             "0..0x%llx backed at %p (core 2^%d)\n",
                             static_cast<unsigned long long>(m_linear_size),
                             static_cast<void*>(m_linear_base),
                             __builtin_ctzll(size));
            }
            m_linear_check_map = static_cast<u8*>(map);
            m_linear_check_map_bytes = map_bytes;
            m_linear_check_count = m_linear_size >> kLinearHostPageShift;
            // Same length and indexing. Optional: without it the cold stubs
            // send every mixed-page access to the helper, as before.
            void* sub = mmap(nullptr, m_linear_check_count + kCheckMapPadBytes,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (sub != MAP_FAILED) m_linear_subpage_map = static_cast<u8*>(sub);
            // Make every future MEM_WRITE_WATCH registration publish itself
            // into the map. Installed here rather than by the kernel32 layer so
            // no registration path can exist that forgets to.
            write_watch::set_page_publisher(
                [](void* ctx, u64 base, u64 size, bool set) {
                    auto* self = static_cast<GuestMemory*>(ctx);
                    if (set) {
                        self->linear_watch_range_retain(
                            static_cast<GuestAddr>(base), size);
                    } else {
                        self->linear_watch_range_release(
                            static_cast<GuestAddr>(base), size);
                    }
                },
                this);
        }
    }
    return true;
}

void GuestMemory::linear_check_bits_update(GuestAddr guest_addr, usize size,
                                           u8 bits, bool set) noexcept {
    if (m_linear_check_map == nullptr || size == 0) return;

    const usize first = static_cast<usize>(guest_addr) >> kLinearHostPageShift;
    const usize last = (static_cast<usize>(guest_addr) + size - 1) >>
                       kLinearHostPageShift;
    if (first >= m_linear_check_count) return;
    const usize end = std::min(last + 1, m_linear_check_count);

    // Byte-at-a-time rather than memset: the byte carries several independent
    // reasons and a publisher must never clear another owner's bit.
    //
    // ATOMIC read-modify-write, and not because a JIT thread races the update
    // (it only ever loads the byte). The publishers race EACH OTHER: this is
    // called under m_regions_mutex for kLinearCheckMixedProt and under a
    // code-page shard lock for kLinearCheckHoldsCode, so no single lock
    // serialises the two, and a plain RMW could drop the other owner's bit --
    // dropping kLinearCheckHoldsCode is exactly the direction that lets a
    // direct store run over a live translation.
    for (usize page = first; page < end; ++page) {
        if (set) {
            __atomic_fetch_or(&m_linear_check_map[page], bits, __ATOMIC_RELEASE);
        } else {
            __atomic_fetch_and(&m_linear_check_map[page],
                               static_cast<u8>(~bits), __ATOMIC_RELEASE);
        }
    }
}

void GuestMemory::linear_watch_range_retain(GuestAddr base,
                                            usize size) noexcept {
    if (m_linear_check_map == nullptr || size == 0) return;
    const usize first = static_cast<usize>(base) >> kLinearHostPageShift;
    const usize last = (static_cast<usize>(base) + size - 1) >>
                       kLinearHostPageShift;
    std::lock_guard<std::mutex> lock(m_linear_watch_mutex);
    for (usize page = first; page <= last; ++page) {
        if (++m_linear_watch_host_pages[page] == 1) {
            linear_check_bits_update(
                static_cast<GuestAddr>(page << kLinearHostPageShift),
                usize{1} << kLinearHostPageShift, kLinearCheckWatched,
                /*set=*/true);
        }
    }
    // Publish before the caller registers the region.
    std::atomic_thread_fence(std::memory_order_release);
    // The JIT view drops WRITE now, so a translated store into the watched
    // range faults and takes the recording helper.
    linear_jit_view_refresh(first << kLinearHostPageShift,
                            (last + 1) << kLinearHostPageShift);
}

void GuestMemory::linear_watch_range_release(GuestAddr base,
                                             usize size) noexcept {
    if (m_linear_check_map == nullptr || size == 0) return;
    const usize first = static_cast<usize>(base) >> kLinearHostPageShift;
    const usize last = (static_cast<usize>(base) + size - 1) >>
                       kLinearHostPageShift;
    std::lock_guard<std::mutex> lock(m_linear_watch_mutex);
    for (usize page = first; page <= last; ++page) {
        const auto it = m_linear_watch_host_pages.find(page);
        if (it == m_linear_watch_host_pages.end()) continue;
        if (--it->second != 0) {
            // Another live watched allocation still overlaps this host page.
            continue;
        }
        m_linear_watch_host_pages.erase(it);
        linear_check_bits_update(
            static_cast<GuestAddr>(page << kLinearHostPageShift),
            usize{1} << kLinearHostPageShift, kLinearCheckWatched,
            /*set=*/false);
    }
    // Only after the bits are clear may the JIT view grant WRITE again.
    linear_jit_view_refresh(first << kLinearHostPageShift,
                            (last + 1) << kLinearHostPageShift);
}

void GuestMemory::linear_code_page_retain(JitCodePageShard& shard,
                                          GuestAddr page) noexcept {
    if (m_linear_check_map == nullptr) return;
    const usize host_page = static_cast<usize>(page) >> kLinearHostPageShift;
    const u32 refs = ++shard.linear_host_refs[host_page];
    if (refs == 1) {
        // Publish before the translation is reachable: a racing store must
        // either take the helper or belong to a translation that does not yet
        // exist. The reverse (clearing) is ordered in the release path.
        linear_check_bits_update(page, 1, kLinearCheckHoldsCode, /*set=*/true);
        std::atomic_thread_fence(std::memory_order_release);
        // And the JIT view drops WRITE before the translation is reachable,
        // so a translated store into it faults and reaches the SMC helper.
        linear_jit_view_refresh(host_page << kLinearHostPageShift,
                                (host_page + 1) << kLinearHostPageShift);
    }
}

void GuestMemory::linear_code_page_release(JitCodePageShard& shard,
                                           GuestAddr page) noexcept {
    if (m_linear_check_map == nullptr) return;
    const usize host_page = static_cast<usize>(page) >> kLinearHostPageShift;
    const auto it = shard.linear_host_refs.find(host_page);
    if (it == shard.linear_host_refs.end()) return;
    if (--it->second != 0) {
        // Another 4 KiB guest code page inside this host page still holds a
        // translation, so the bit stays set. This is why the refcount is per
        // HOST page and not per guest page.
        return;
    }
    shard.linear_host_refs.erase(it);
    linear_check_bits_update(page, 1, kLinearCheckHoldsCode, /*set=*/false);
    linear_jit_view_refresh(host_page << kLinearHostPageShift,
                            (host_page + 1) << kLinearHostPageShift);
}

bool GuestMemory::enable_linear_hardware_protection() {
    std::unique_lock lk(m_regions_mutex);
    if (m_linear_identity) return false;   // already applies real protection
    if (m_linear_base == nullptr) return false;
    if (!regions_.empty()) return false;   // must precede any mapping
    if (m_linear_hw_protect) return true;

    // The check-page map is allocated with the reservation and is indexed by a
    // fixed shift, so its absence means the host page size is not the 16 KiB
    // the emitted index arithmetic assumes. Refuse rather than mis-enforce.
    if (m_linear_check_map == nullptr) return false;
    m_linear_hw_protect = true;
    return true;
}

bool GuestMemory::enable_identity_mapping() {
    std::unique_lock lk(m_regions_mutex);
    if (m_linear_base != nullptr) return false;   // linear reservation active
    if (m_linear_identity)        return false;   // already enabled
    if (!regions_.empty())        return false;   // must precede any mapping

    // No reservation mmap here. Under identity mapping the guest address IS
    // the host address, so there is nothing to reserve *relative to* -- the
    // ranges must already have been claimed from the host process before
    // anything else could take them. That claim is a startup concern (and a
    // link-time one), not something
    // GuestMemory can retrofit.
    m_linear_identity = true;
    m_linear_size = 0;   // unbounded: bounded by the reserved holes instead
    return true;
}

namespace {

void insert_reserved_hole(std::vector<std::pair<GuestAddr, GuestAddr>>& holes,
                          GuestAddr lo, GuestAddr hi) {
    if (hi <= lo) return;
    auto it = holes.begin();
    while (it != holes.end() && it->second < lo) ++it;
    while (it != holes.end() && it->first <= hi) {
        lo = std::min(lo, it->first);
        hi = std::max(hi, it->second);
        it = holes.erase(it);
    }
    holes.insert(it, {lo, hi});
}

} // namespace

void GuestMemory::add_reserved_hole(GuestAddr lo, GuestAddr hi) {
    if (hi <= lo) return;
    std::unique_lock lk(m_regions_mutex);
    insert_reserved_hole(m_reserved_holes, lo, hi);
}

bool GuestMemory::overlaps_reserved_hole(GuestAddr addr,
                                         usize size) const noexcept {
    if (size == 0) return false;
    const GuestAddr end = addr + size;
    for (const auto& [lo, hi] : m_reserved_holes) {
        if (lo >= end) break;          // sorted: nothing further can overlap
        if (addr < hi) return true;
    }
    return false;
}

GuestAddr GuestMemory::next_address_clear_of_holes(GuestAddr addr,
                                                   usize size) const noexcept {
    if (size == 0) return addr;
    // Holes are sorted and coalesced, so one forward pass suffices: each time
    // the candidate collides, it can only move to that hole's end.
    for (const auto& [lo, hi] : m_reserved_holes) {
        if (lo >= addr + size) break;
        if (addr < hi && lo < addr + size) addr = hi;
    }
    return addr;
}

std::vector<std::pair<GuestAddr, GuestAddr>>
GuestMemory::reserved_holes() const {
    std::shared_lock lk(m_regions_mutex);
    return m_reserved_holes;
}

namespace {

/// vmsize of the main executable's __PAGEZERO segment.
///
/// This CANNOT be discovered with mach_vm_region: __PAGEZERO is not a VM map
/// entry at all — the kernel raises the task map's minimum offset above it, so
/// the region walk skips straight past and reports the whole range as free.
/// Measured on both backends: on a default-linked binary the first region
/// mach_vm_region returns is already above 4 GiB, yet every address below it
/// returns KERN_INVALID_ADDRESS. Believing the walk would hand the guest 4 GiB
/// of address space that can never be mapped. The Mach-O header is the
/// authority.
u64 main_image_pagezero_size() {
    const auto* mh = _NSGetMachExecuteHeader();
    if (mh == nullptr || mh->magic != MH_MAGIC_64) return 0;

    const auto* header = reinterpret_cast<const mach_header_64*>(mh);
    const auto* lc = reinterpret_cast<const load_command*>(header + 1);
    for (u32 i = 0; i < header->ncmds; ++i) {
        if (lc->cmdsize == 0) break;   // malformed: would not advance
        if (lc->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (std::strncmp(seg->segname, SEG_PAGEZERO,
                             sizeof(seg->segname)) == 0) {
                return seg->vmsize;
            }
        }
        lc = reinterpret_cast<const load_command*>(
            reinterpret_cast<const u8*>(lc) + lc->cmdsize);
    }
    return 0;
}

} // namespace

usize GuestMemory::capture_host_occupancy_below_4gb() {
    constexpr mach_vm_address_t k4GiB = 0x1'0000'0000ull;
    {
        std::shared_lock lk(m_regions_mutex);
        // Only meaningful under identity mapping, and only truthful while the
        // address space is still pristine — mach_vm_region cannot tell a host
        // occupant from a guest region we mapped ourselves.
        if (!m_linear_identity || !regions_.empty()) return 0;
    }

    usize recorded = 0;

    if (const u64 pagezero = main_image_pagezero_size(); pagezero != 0) {
        add_reserved_hole(0, static_cast<GuestAddr>(
                                 std::min<u64>(pagezero, k4GiB)));
        ++recorded;
    }

    mach_vm_address_t addr = 0;
    while (addr < k4GiB) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;

        // mach_vm_region reports the first mapping at OR AFTER *addr, so the
        // gaps between mappings are exactly the free ranges and never need to
        // be probed individually.
        kern_return_t kr = mach_vm_region(
            mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (kr != KERN_SUCCESS) break;   // KERN_INVALID_ADDRESS = nothing left
        if (size == 0) break;            // defensive: would not advance
        if (addr >= k4GiB) break;

        const mach_vm_address_t end = std::min(addr + size, k4GiB);
        add_reserved_hole(static_cast<GuestAddr>(addr),
                          static_cast<GuestAddr>(end));
        ++recorded;
        addr += size;
    }
    return recorded;
}

usize GuestMemory::reserve_identity_space_below_4gb() {
    constexpr mach_vm_address_t k4GiB = 0x1'0000'0000ull;
    {
        std::shared_lock lk(m_regions_mutex);
        if (!m_linear_identity || !regions_.empty()) return 0;
    }

    // The gaps between the reserved holes are exactly the ranges the host does
    // not own, so they are what the guest may claim.
    std::vector<std::pair<GuestAddr, GuestAddr>> gaps;
    {
        std::shared_lock lk(m_regions_mutex);
        GuestAddr cursor = 0;
        for (const auto& [lo, hi] : m_reserved_holes) {
            if (lo >= k4GiB) break;
            if (lo > cursor) gaps.emplace_back(cursor, lo);
            cursor = std::max(cursor, hi);
        }
        if (cursor < k4GiB) gaps.emplace_back(cursor, k4GiB);
    }

    usize reserved = 0;
    for (const auto& [lo, hi] : gaps) {
        mach_vm_address_t addr = lo;
        const mach_vm_size_t size = hi - lo;

        // mach_vm_allocate with VM_FLAGS_FIXED fails with KERN_NO_SPACE when
        // the range is occupied. mmap(MAP_FIXED) would instead UNMAP whatever
        // is there — on a range list assembled from a VM-map walk that could
        // silently destroy dyld or the main thread stack. macOS has no
        // MAP_FIXED_NOREPLACE, so this is the only non-destructive claim.
        const int flags = VM_FLAGS_FIXED |
            VM_MAKE_TAG(static_cast<int>(memory::kGuestIdentityVmTag));
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, size,
                                            flags);
        if (kr != KERN_SUCCESS || addr != lo) {
            if (kr == KERN_SUCCESS) {
                (void)mach_vm_deallocate(mach_task_self(), addr, size);
            }
            std::fprintf(stderr,
                         "[identity] could not claim guest range "
                         "0x%llx-0x%llx (kr=%d); reserving it against the "
                         "guest\n",
                         static_cast<unsigned long long>(lo),
                         static_cast<unsigned long long>(hi),
                         static_cast<int>(kr));
            add_reserved_hole(lo, hi);
            continue;
        }

        // Reserved, not committed: the guest's own map_region() decides what
        // becomes accessible, exactly as under the linear reservation.
        (void)mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(lo)),
                       static_cast<usize>(size), PROT_NONE);
        reserved += static_cast<usize>(size);
    }
    return reserved;
}

// Host page size (16 KiB on Apple Silicon). Linear commit/protect operate at
// this granularity, so sub-page guest protections are coarsened to the host
// page — over-permissive, which is safe for apps that don't rely on their own
// pages faulting (Vitreo models no guard pages). See the doc's 16 KiB caveat.
static usize host_page_size() {
    static const usize s = static_cast<usize>(::getpagesize());
    return s;
}

static bool is_host_owned_vm_tag(unsigned tag) noexcept {
    // Tiny/small/nano zones, their guard pages, and VM_MEMORY_GUARD are never
    // ours. VM_MEMORY_MALLOC (1) is the kernel default for mach_vm_allocate
    // and for anonymous mmap -- tests and the untagged fallback reservation
    // still use it, so it is allowed when the pages are PROT_NONE.
    switch (tag) {
        case VM_MEMORY_MALLOC_SMALL:
        case VM_MEMORY_MALLOC_LARGE:
        case VM_MEMORY_MALLOC_HUGE:
        case VM_MEMORY_MALLOC_TINY:
        case VM_MEMORY_REALLOC:
        case VM_MEMORY_MALLOC_LARGE_REUSABLE:
        case VM_MEMORY_MALLOC_LARGE_REUSED:
        case VM_MEMORY_MALLOC_NANO:
        case VM_MEMORY_GUARD:
            return true;
        default:
            return false;
    }
}

static bool is_legacy_identity_tag(unsigned tag) noexcept {
    return tag == 0 || tag == VM_MEMORY_MALLOC;
}

struct IdentityVmRegion {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    unsigned tag = 0;
    vm_prot_t protection = VM_PROT_NONE;
};

static bool query_identity_region(mach_vm_address_t page, IdentityVmRegion* out) {
    natural_t depth = 0;
    for (int n = 0; n < 16; ++n) {
        mach_vm_address_t q = page;
        mach_vm_size_t sz = 0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        natural_t d = depth;
        kern_return_t kr = mach_vm_region_recurse(
            mach_task_self(), &q, &sz, &d,
            reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        if (kr != KERN_SUCCESS || sz == 0) return false;
        if (info.is_submap) {
            depth = d + 1;
            continue;
        }
        out->addr = q;
        out->size = sz;
        out->tag = static_cast<unsigned>(info.user_tag);
        out->protection = info.protection;
        return true;
    }
    return false;
}

static void log_identity_prepare_fail(const char* why, GuestAddr guest_addr,
                                      usize size, uintptr_t at,
                                      kern_return_t kr) {
    IdentityVmRegion region{};
    const bool have = query_identity_region(at, &region);
    std::fprintf(stderr,
                 "[memory] identity prepare failed (%s) guest=0x%llx size=0x%zx "
                 "at=0x%lx kr=%d errno=%d tag=%u prot=%x mapped=0x%llx-0x%llx\n",
                 why,
                 static_cast<unsigned long long>(guest_addr), size,
                 static_cast<unsigned long>(at), static_cast<int>(kr), errno,
                 have ? region.tag : 0u,
                 have ? static_cast<unsigned>(region.protection) : 0u,
                 have ? static_cast<unsigned long long>(region.addr) : 0ull,
                 have ? static_cast<unsigned long long>(region.addr + region.size)
                      : 0ull);
}

static int identity_mmap_prot(MemoryProtection prot) noexcept {
    // Unlike to_mmap_prot(), identity must honour Execute even on writable
    // pages: natively executed guest code lives at these addresses.
    const auto bits = static_cast<u8>(prot);
    int host_prot = PROT_NONE;
    if (bits & static_cast<u8>(MemoryProtection::Read))    host_prot |= PROT_READ;
    if (bits & static_cast<u8>(MemoryProtection::Write))   host_prot |= PROT_WRITE;
    if (bits & static_cast<u8>(MemoryProtection::Execute)) host_prot |= PROT_EXEC;
    return host_prot;
}

bool GuestMemory::identity_prepare_span(GuestAddr guest_addr, usize size,
                                        MemoryProtection prot,
                                        bool existing_guest_region) {
    if (!m_linear_identity || size == 0) return true;
    if (overlaps_reserved_hole(guest_addr, size)) return false;

    const usize hp = host_page_size();
    uintptr_t lo = static_cast<uintptr_t>(guest_addr) & ~(hp - 1);
    uintptr_t hi = (static_cast<uintptr_t>(guest_addr) + size + hp - 1)
                   & ~(hp - 1);

    const auto refuse_foreign = [&](const IdentityVmRegion& region) -> bool {
        const unsigned tag = region.tag;
        if (tag == memory::kGuestIdentityVmTag) return false;
        if (is_host_owned_vm_tag(tag)) return true;
        // New reservations may adopt leftover PROT_NONE anonymous pages
        // (test mmaps, untagged fallback claims). Anything else is host.
        if (!existing_guest_region &&
            (region.protection != VM_PROT_NONE ||
             !is_legacy_identity_tag(tag))) {
            return true;
        }
        return false;
    };

    auto claim_gap = [&](uintptr_t gap_lo, uintptr_t gap_hi) -> bool {
        if (gap_lo >= gap_hi) return true;
        mach_vm_address_t addr = gap_lo;
        const mach_vm_size_t bytes = gap_hi - gap_lo;
        const int flags = VM_FLAGS_FIXED |
            VM_MAKE_TAG(static_cast<int>(memory::kGuestIdentityVmTag));
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, bytes,
                                            flags);
        if (kr == KERN_SUCCESS && addr == gap_lo) {
            (void)mach_vm_protect(mach_task_self(), gap_lo, bytes, FALSE,
                                  VM_PROT_NONE);
            return true;
        }
        if (kr == KERN_SUCCESS) {
            (void)mach_vm_deallocate(mach_task_self(), addr, bytes);
        }
        // Walk thought this was a gap, but the kernel already has a mapping.
        // Accept it only when every page is ours (or an allowed leftover).
        uintptr_t covered = gap_lo;
        while (covered < gap_hi) {
            IdentityVmRegion region{};
            if (!query_identity_region(covered, &region) ||
                region.addr > covered) {
                log_identity_prepare_fail("claim-gap", guest_addr, size,
                                          covered, kr);
                return false;
            }
            if (refuse_foreign(region)) {
                insert_reserved_hole(
                    m_reserved_holes,
                    static_cast<GuestAddr>(std::max(gap_lo,
                        static_cast<uintptr_t>(region.addr))),
                    static_cast<GuestAddr>(std::min(gap_hi,
                        static_cast<uintptr_t>(region.addr + region.size))));
                log_identity_prepare_fail("host-owned", guest_addr, size,
                                          covered, kr);
                return false;
            }
            const uintptr_t region_end =
                static_cast<uintptr_t>(region.addr + region.size);
            if (region_end <= covered) {
                log_identity_prepare_fail("zero-size", guest_addr, size,
                                          covered, KERN_FAILURE);
                return false;
            }
            covered = region_end;
        }
        return true;
    };

    uintptr_t cursor = lo;
    while (cursor < hi) {
        IdentityVmRegion region{};
        if (!query_identity_region(cursor, &region) || region.addr >= hi) {
            if (!claim_gap(cursor, hi)) return false;
            break;
        }
        if (region.addr > cursor) {
            if (!claim_gap(cursor,
                           std::min(hi, static_cast<uintptr_t>(region.addr)))) {
                return false;
            }
            cursor = static_cast<uintptr_t>(region.addr);
            if (cursor >= hi) break;
        }

        if (refuse_foreign(region)) {
            insert_reserved_hole(
                m_reserved_holes,
                static_cast<GuestAddr>(std::max(
                    lo, static_cast<uintptr_t>(region.addr))),
                static_cast<GuestAddr>(std::min(
                    hi, static_cast<uintptr_t>(region.addr + region.size))));
            log_identity_prepare_fail("host-owned", guest_addr, size, cursor,
                                      KERN_SUCCESS);
            return false;
        }

        const uintptr_t region_end =
            static_cast<uintptr_t>(region.addr + region.size);
        if (region_end <= cursor) {
            log_identity_prepare_fail("zero-size", guest_addr, size, cursor,
                                      KERN_FAILURE);
            return false;
        }
        cursor = region_end;
    }

    const vm_prot_t host_prot =
        static_cast<vm_prot_t>(identity_mmap_prot(prot));
    kern_return_t kr = mach_vm_protect(mach_task_self(), lo, hi - lo, FALSE,
                                       host_prot);
    if (kr == KERN_SUCCESS) return true;

    // Whole-span protect failed: typically a hole the walk missed, or a
    // PROT_NONE page whose max-protection will not accept RW. Refill and
    // replace uncommitted pages; never deallocate a writable mapping.
    for (uintptr_t page = lo; page < hi; page += hp) {
        IdentityVmRegion region{};
        const bool mapped = query_identity_region(page, &region) &&
                            region.addr <= page;
        if (mapped && refuse_foreign(region)) {
            insert_reserved_hole(
                m_reserved_holes,
                static_cast<GuestAddr>(page),
                static_cast<GuestAddr>(page + hp));
            log_identity_prepare_fail("host-owned", guest_addr, size, page,
                                      kr);
            return false;
        }

        const bool prot_none = mapped && region.protection == VM_PROT_NONE;
        if (!mapped || (prot_none && host_prot != VM_PROT_NONE)) {
            if (mapped) {
                (void)mach_vm_deallocate(mach_task_self(), page, hp);
            }
            if (!claim_gap(page, page + hp)) return false;
        }

        const kern_return_t page_kr = mach_vm_protect(
            mach_task_self(), page, hp, FALSE, host_prot);
        if (page_kr != KERN_SUCCESS) {
            log_identity_prepare_fail("protect", guest_addr, size, page,
                                      page_kr);
            return false;
        }
    }
    return true;
}

bool GuestMemory::identity_restore_span(GuestAddr guest_addr, usize size) {
    if (!m_linear_identity || size == 0) return true;

    const usize hp = host_page_size();
    uintptr_t lo = static_cast<uintptr_t>(guest_addr) & ~(hp - 1);
    uintptr_t hi = (static_cast<uintptr_t>(guest_addr) + size + hp - 1)
                   & ~(hp - 1);
    const mach_vm_size_t bytes = hi - lo;
    void* const host = reinterpret_cast<void*>(lo);
    void* const placed = mmap(host, static_cast<usize>(bytes), PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (placed == host) {
        return true;
    }

    // mmap(MAP_FIXED) failed or moved. Record the span as a hole so a later
    // search will not hand it back to the guest as if it were still ours.
    insert_reserved_hole(m_reserved_holes,
                         static_cast<GuestAddr>(lo),
                         static_cast<GuestAddr>(hi));
    std::fprintf(stderr,
                 "[memory] identity restore failed guest=0x%llx size=0x%zx "
                 "errno=%d placed=%p\n",
                 static_cast<unsigned long long>(guest_addr), size, errno,
                 placed == MAP_FAILED ? nullptr : placed);
    return false;
}

bool GuestMemory::identity_host_protect(GuestAddr guest_addr, usize size,
                                        MemoryProtection prot) noexcept {
    if (!m_linear_identity || size == 0) return true;
    return identity_prepare_span(guest_addr, size, prot,
                                 /*existing_guest_region=*/true);
}

MemoryRegion GuestMemory::map_in_reservation(GuestAddr guest_addr,
                                             usize aligned_size,
                                             bool reserve_only) {
    // Identity mapping has no reservation window to bound against; the
    // reserved holes carry that role instead.
    if (m_linear_identity) {
        const MemoryProtection want = reserve_only
            ? MemoryProtection::None
            : MemoryProtection::ReadWrite;
        if (!identity_prepare_span(guest_addr, aligned_size, want,
                                   /*existing_guest_region=*/false)) {
            return MemoryRegion{};
        }
        return MemoryRegion::borrow(
            linear_host(guest_addr), aligned_size, want);
    }
    if (guest_addr > m_linear_size ||
        aligned_size > m_linear_size - guest_addr) {
        return MemoryRegion{};  // invalid → caller errors
    }
    if (!linear_extension_materialize(guest_addr, aligned_size)) {
        return MemoryRegion{};
    }
    // Commit by mprotect within the reservation (not MAP_FIXED): mprotect never
    // remaps, so it neither zeroes nor clobbers neighbouring guest pages that
    // happen to share a 16 KiB host page. reserve_only stays PROT_NONE.
    if (!reserve_only) {
        const usize hp = host_page_size();
        uintptr_t lo = reinterpret_cast<uintptr_t>(linear_host(guest_addr));
        uintptr_t hi = lo + aligned_size;
        lo &= ~(hp - 1);
        hi = (hi + hp - 1) & ~(hp - 1);
        if (mprotect(reinterpret_cast<void*>(lo), hi - lo,
                     PROT_READ | PROT_WRITE) != 0) {
            return MemoryRegion{};
        }
    }
    // Borrow: the reservation owns the address space; the region's dtor must
    // not munmap it.
    return MemoryRegion::borrow(
        linear_host(guest_addr), aligned_size,
        reserve_only ? MemoryProtection::None : MemoryProtection::ReadWrite);
}

namespace {
// Kill switch for A/B measurement: VITREO_MEM_EAGER_ZERO=1 or the marker
// file /tmp/vitreo-mem-eager-zero restores the eager zero-fill of every
// committed page (the pre-guard behaviour).
bool eager_zero_forced() {
    static const bool forced = [] {
        if (const char* e = std::getenv("VITREO_MEM_EAGER_ZERO");
            e != nullptr && e[0] == '1') {
            return true;
        }
        return ::access("/tmp/vitreo-mem-eager-zero", F_OK) == 0;
    }();
    return forced;
}
} // namespace

bool GuestMemory::commit_pages_are_fresh_locked(GuestAddr addr,
                                                usize size) const {
    if (size == 0 || !is_linear() || is_identity() ||
        !linear_backs(addr, size) || eager_zero_forced()) {
        return false;
    }
    const usize hp = host_page_size();
    const usize lo = static_cast<usize>(addr) & ~(hp - 1);
    const usize hi = (static_cast<usize>(addr) + size + hp - 1) & ~(hp - 1);
    std::vector<LinearSpan> spans;
    linear_collect_spans(lo, hi, spans);
    const usize range_lo = static_cast<usize>(addr);
    const usize range_hi = range_lo + size;
    for (const LinearSpan& span : spans) {
        if (!span.committed) continue;
        // A committed run that reaches outside the range shares a host page
        // with live data, so that page was never dropped and may be stale.
        if (span.lo < range_lo || span.hi > range_hi) return false;
    }
    return true;
}

bool GuestMemory::commit_pages_are_fresh(GuestAddr addr, usize size) const {
    std::shared_lock lk(m_regions_mutex);
    return commit_pages_are_fresh_locked(addr, size);
}

void GuestMemory::linear_collect_spans(usize lo, usize hi,
                                       std::vector<LinearSpan>& out) const {
    out.clear();
    const auto push = [&out](usize span_lo, usize span_hi, bool committed,
                             MemoryProtection prot) {
        if (span_hi <= span_lo) return;
        if (!out.empty() && out.back().hi == span_lo &&
            out.back().committed == committed && out.back().prot == prot) {
            out.back().hi = span_hi;
            return;
        }
        out.push_back({span_lo, span_hi, committed, prot});
    };

    usize cursor = lo;
    // regions_ is keyed by base; start at the region that may already cover
    // `lo` (upper_bound then step back), then walk forward.
    auto it = regions_.upper_bound(static_cast<GuestAddr>(lo));
    if (it != regions_.begin()) --it;
    for (; it != regions_.end() && cursor < hi; ++it) {
        const MappedRegion& mapped = it->second;
        const usize base = static_cast<usize>(mapped.base);
        const usize end = base + mapped.requested_size;
        if (end <= cursor) continue;
        if (base >= hi) break;
        // Gap before this region: unmapped, therefore uncommitted.
        push(cursor, std::min(base, hi), false, MemoryProtection::None);
        cursor = std::max(cursor, base);

        // Walk the region's committed and protection runs together. Both are
        // sorted offset->value maps with an implicit entry at offset 0, so the
        // next boundary is the nearer of the two.
        while (cursor < end && cursor < hi) {
            const usize offset = cursor - base;
            auto c = mapped.committed.upper_bound(offset);
            const bool committed = std::prev(c)->second;
            const usize c_end =
                c == mapped.committed.end() ? end : base + c->first;
            const ProtectionSpan p = protection_span(mapped, offset);
            const usize p_end = base + p.end;
            const usize next = std::min({c_end, p_end, end, hi});
            push(cursor, next, committed, p.protection);
            cursor = next;
        }
    }
    push(cursor, hi, false, MemoryProtection::None);
}

namespace {

/// Host protection a single committed guest page with @p prot must carry.
///
/// Guest code is never host-executed in the linear model, so PAGE_EXECUTE*
/// contributes read access and nothing more — PROT_EXEC here would only make
/// a guest's own W^X page host-executable, which nothing consumes.
[[nodiscard]] int linear_guest_page_host_prot(bool committed,
                                              MemoryProtection prot) noexcept {
    if (!committed) return PROT_NONE;
    if (has_flag(prot, MemoryProtection::Write)) return PROT_READ | PROT_WRITE;
    if (has_flag(prot, MemoryProtection::Read) ||
        has_flag(prot, MemoryProtection::Execute)) {
        return PROT_READ;
    }
    return PROT_NONE;  // PAGE_NOACCESS on a committed page
}

/// Whether the host protection above is an EXACT statement of @p prot, so that
/// a hardware fault means a guest access violation and its absence means the
/// guest permits the access.
///
/// Two guest protections have no host equivalent and must keep their software
/// check, or the direct path would silently allow what the guest forbids:
///   * PAGE_EXECUTE with no read — the host page has to be PROT_READ for
///     anything to work, which grants a read the guest denies.
///   * write without read — PROT_WRITE alone is not a thing here either.
[[nodiscard]] bool linear_host_prot_is_exact(bool committed,
                                             MemoryProtection prot) noexcept {
    if (!committed) return true;   // PROT_NONE denies everything, exactly
    if (has_flag(prot, MemoryProtection::Read)) return true;
    // No Read: only PAGE_NOACCESS (nothing set) maps exactly, onto PROT_NONE.
    return !has_flag(prot, MemoryProtection::Execute) &&
           !has_flag(prot, MemoryProtection::Write);
}

}  // namespace

void GuestMemory::linear_reconcile_host_pages(GuestAddr guest_addr, usize size,
                                              bool discard_unowned) {
    if (size == 0) {
        return;
    }
    // Clamp to the reservation: linear_host() of an out-of-window address is
    // outside the mmap, and every caller is expected to pre-filter with
    // linear_backs() — this is the backstop for the one that forgets. Identity
    // (m_linear_base == nullptr) has no window and is exempt.
    if (m_linear_base != nullptr) {
        if (static_cast<usize>(guest_addr) >= m_linear_size) return;
        size = std::min(size, m_linear_size - static_cast<usize>(guest_addr));
    }

    const usize hp = host_page_size();
    const usize guest_lo = static_cast<usize>(guest_addr) & ~(hp - 1);
    const usize range_end = static_cast<usize>(guest_addr) + size;
    const usize guest_hi = (range_end + hp - 1) & ~(hp - 1);

    // The JIT view's decision for these pages is made below from the same
    // spans and bits, and must not interleave with a code-page or write-watch
    // publisher's refresh of the same page (enable_linear_jit_view).
    std::unique_lock<std::mutex> jit_view_lock(m_jit_view_mutex, std::defer_lock);
    if (m_linear_jit_view != nullptr) jit_view_lock.lock();

    std::vector<LinearSpan> spans;
    linear_collect_spans(guest_lo, guest_hi, spans);

    // One decision per 16 KiB host page, coalesced into runs so a multi-
    // gigabyte uniform commit stays a single mprotect.
    struct HostSpan {
        usize lo;
        usize hi;
        int prot;
        bool owned;       // any committed guest page inside
        bool mixed;       // hardware protection is not exact for every guest page
        bool mixed_read;  // ... and the inexactness can let a READ through
    };
    std::vector<HostSpan> host_runs;
    {
        usize si = 0;
        for (usize page = guest_lo; page < guest_hi; page += hp) {
            const usize page_end = page + hp;
            while (si + 1 < spans.size() && spans[si].hi <= page) ++si;

            // Every host page that falls entirely inside ONE span reaches the
            // identical decision, so the run they form can be emitted in a
            // single step. Stepping them 16 KiB at a time is what made this
            // loop the single largest CPU cost on both critical-path threads
            // during startup (~18% of the renderer's main thread): V8 reserves
            // its pointer cage at 2x and releases the padding, so one
            // VirtualFree hands this function a ~1 TB range = 68 million
            // iterations that all agree. linear_collect_spans already
            // coalesces, so a span boundary is the only place the decision can
            // change.
            // A/B gate: VITREO_LINEAR_RECONCILE_PAGE_STEP=1 restores the old
            // one-page-at-a-time walk so the same binary can measure both.
            static const bool page_step = [] {
                const char* v = std::getenv("VITREO_LINEAR_RECONCILE_PAGE_STEP");
                return v != nullptr && v[0] == '1';
            }();
            usize uniform_to = page_end;
            if (!page_step && !spans.empty() && spans[si].lo <= page &&
                spans[si].hi >= page_end) {
                uniform_to = std::max(
                    page_end, std::min(guest_hi, spans[si].hi & ~(hp - 1)));
            }

            bool owned = false;
            bool uniform = true;
            bool have_first = false;
            int first_prot = PROT_NONE;
            int permissive = PROT_NONE;
            // Read-safety is tracked separately from `uniform`: the common
            // disagreement inside a host page is about WRITABILITY, and that
            // cannot make a read unsafe. See kLinearCheckMixedRead.
            bool any_denies_read = false;
            bool read_inexact = false;
            for (usize j = si; j < spans.size() && spans[j].lo < page_end; ++j) {
                if (spans[j].hi <= page) continue;
                owned = owned || spans[j].committed;
                const int p = linear_guest_page_host_prot(spans[j].committed,
                                                          spans[j].prot);
                if (!linear_host_prot_is_exact(spans[j].committed,
                                               spans[j].prot)) {
                    uniform = false;
                    // The only inexactness this helper reports is "the guest
                    // denies read but the host page must grant it" -- execute
                    // -only and write-without-read. That is a read hazard by
                    // definition.
                    read_inexact = true;
                }
                if ((p & PROT_READ) == 0) any_denies_read = true;
                // A run that only clips the page does not by itself make the
                // page mixed; disagreement between runs does.
                if (!have_first) {
                    first_prot = p;
                    have_first = true;
                } else if (p != first_prot) {
                    uniform = false;
                }
                permissive |= p;   // JOIN: NONE < READ < RW
            }

            int prot;
            bool mixed;
            // Reads are unsafe only if hardware would permit one the guest
            // denies: a sub-page that denies read while the page grants it, or
            // a protection whose only inexactness IS "guest denies read".
            bool mixed_read = read_inexact ||
                              (((permissive & PROT_READ) != 0) && any_denies_read);
            // MEM_WRITE_WATCH hardware tracking is the other subsystem that
            // mprotects these pages (arming a clean page PROT_READ). It only
            // runs under identity mapping, which never reaches here, but if it
            // ever did its arming would silently undo this exactness — so
            // concede the whole range to software checking rather than let two
            // owners fight over the same host page.
            if (!m_linear_hw_protect || write_watch::hardware_traps()) {
                // Legacy model: accessibility is software-only, so every
                // committed host page is RW and every access is checked.
                prot = owned ? (PROT_READ | PROT_WRITE) : PROT_NONE;
                mixed = true;
                // Software-only accessibility: reads are unchecked by hardware
                // here too, so they must keep the software check.
                mixed_read = true;
            } else if (uniform) {
                // Hardware states the guest's protection exactly.
                prot = first_prot;
                mixed = false;
                mixed_read = false;
            } else {
                // No host protection can be right for every guest page inside.
                // Most-permissive plus a software check: most-restrictive would
                // instead deny accesses the guest permits, and PE images make
                // that the common case (4 KiB SectionAlignment puts .rdata and
                // .data in one host page), so a hot store would fault forever.
                prot = permissive;
                mixed = true;
            }
            if (!owned) {
                // Nothing committed: PROT_NONE, and eligible for the discard
                // below. A mixed marking would be meaningless.
                prot = PROT_NONE;
                mixed = !m_linear_hw_protect;
                // PROT_NONE denies every read in hardware, so a load cannot
                // wrongly succeed and needs no software check.
                mixed_read = mixed;
            }

            if (!host_runs.empty() && host_runs.back().hi == page &&
                host_runs.back().prot == prot &&
                host_runs.back().owned == owned &&
                host_runs.back().mixed == mixed &&
                host_runs.back().mixed_read == mixed_read) {
                host_runs.back().hi = uniform_to;
            } else {
                host_runs.push_back({page, uniform_to, prot, owned, mixed,
                                     mixed_read});
            }
            page = uniform_to - hp;   // the loop's += hp lands on uniform_to
        }
    }

    // Fail closed while the two states disagree. A host page that is GAINING a
    // software check must have its map byte set before its hardware protection
    // is relaxed, or a concurrent thread would take the direct path against
    // hardware that no longer enforces anything. The reverse transition is
    // handled after the mprotects, below.
    if (m_linear_hw_protect && m_linear_check_map != nullptr) {
        // Each mixed page's per-guest-page read/write bits, before its mixed
        // bit is (re)asserted and before any hardware change: a cold stub
        // that reads them must see this reconcile's state, never one that
        // grants more than the region table now does. Computed per host page
        // -- a run coalesces pages whose DECISION agrees, not whose guest
        // pages agree -- and only for mixed pages, so a terabyte-sized uniform
        // range costs nothing here.
        if (m_linear_subpage_map != nullptr) {
            usize si = 0;
            for (const auto& run : host_runs) {
                if (!run.mixed) continue;
                for (usize page = run.lo; page < run.hi; page += hp) {
                    const usize index = page >> kLinearHostPageShift;
                    if (index >= m_linear_check_count) break;
                    u8 bits = 0;
                    for (usize k = 0; k < (hp >> kLinearGuestPageShift); ++k) {
                        const usize g = page + (k << kLinearGuestPageShift);
                        while (si < spans.size() && spans[si].hi <= g) ++si;
                        if (si >= spans.size() || spans[si].lo > g ||
                            !spans[si].committed) {
                            continue;
                        }
                        if (has_flag(spans[si].prot, MemoryProtection::Read)) {
                            bits |= static_cast<u8>(1u << k);
                        }
                        if (has_flag(spans[si].prot, MemoryProtection::Write)) {
                            bits |= static_cast<u8>(0x10u << k);
                        }
                    }
                    __atomic_store_n(&m_linear_subpage_map[index], bits,
                                     __ATOMIC_RELEASE);
                }
            }
        }
        for (const auto& run : host_runs) {
            if (!run.mixed) continue;
            linear_check_bits_update(
                static_cast<GuestAddr>(run.lo), run.hi - run.lo,
                static_cast<u8>(kLinearCheckMixedProt |
                                (run.mixed_read ? kLinearCheckMixedRead : 0u)),
                /*set=*/true);
            // A page that is mixed for writes but read-safe must have the READ
            // bit CLEARED, not merely left alone: it may carry a stale set bit
            // from an earlier protection state.
            if (!run.mixed_read) {
                linear_check_bits_update(static_cast<GuestAddr>(run.lo),
                                         run.hi - run.lo,
                                         kLinearCheckMixedRead, /*set=*/false);
            }
        }
        std::atomic_thread_fence(std::memory_order_release);
    }

    const auto apply_host_state = [&](usize lo, usize hi, bool committed,
                                      int host_prot) {
        if (lo >= hi) {
            return;
        }
        void* host_range = linear_host(static_cast<GuestAddr>(lo));
        if (mprotect(host_range, hi - lo, host_prot) != 0) {
            std::fprintf(stderr,
                         "[memory] linear host-page reconcile failed "
                         "guest=0x%zx size=0x%zx prot=%s errno=%d\n",
                         lo, hi - lo, committed ? "rw" : "none", errno);
            return;
        }

        // Release the physical pages of an unowned (freed) host-page span and
        // guarantee they read back as zero when the guest address is later
        // re-committed. MADV_FREE alone is wrong here: it is lazy, so a page
        // reused before the kernel reclaims it still holds its old contents —
        // violating Windows' MEM_COMMIT zero-fill guarantee and producing
        // nondeterministic stale-data corruption (garbage high bits in a
        // reused pointer slot). A
        // fresh MAP_FIXED anonymous PROT_NONE mapping drops the old pages
        // without committing new ones (no OOM risk) and forces the kernel to
        // zero-fill on the next commit+touch. This is host-page granular, so
        // it is only applied to fully-unowned spans (never a 16 KiB page still
        // partly owned by a live 4 KiB allocation).
        if (!committed && discard_unowned) {
            // Dropping pages here destroys data if the span is wrong by even
            // one host page, and the symptom (a store that executed but reads
            // back zero) looks exactly like a JIT bug. Log the range so a
            // fault can be correlated against it.
            static const bool log_drops =
                std::getenv("VITREO_LOG_LINEAR_DROP") != nullptr;
            if (log_drops) {
                std::fprintf(stderr,
                             "[linear-drop] guest=0x%zx-0x%zx (0x%zx bytes)\n",
                             lo, hi, hi - lo);
            }
            void* r = mmap(host_range, hi - lo, PROT_NONE,
                           MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (r == MAP_FAILED) {
                // Fall back to the lazy hint rather than leaving stale RW pages.
                madvise(host_range, hi - lo, MADV_FREE);
            } else {
                // New pages in the host view; the JIT view still maps the old
                // ones until it is pointed at these.
                linear_jit_view_realias_locked(lo, hi);
            }
        }
    };

    // Apply owned runs before unowned ones. A lock-free reader of a
    // neighbouring live allocation must never see a transient PROT_NONE
    // window, and only the unowned runs can drop physical pages.
    for (const auto& run : host_runs) {
        if (run.owned) apply_host_state(run.lo, run.hi, true, run.prot);
    }
    for (const auto& run : host_runs) {
        if (run.owned) continue;
        // Nothing committed: the JIT view denies it BEFORE the host view can
        // drop its pages, so translated code never reaches pages being freed.
        linear_jit_view_protect_locked(run.lo, run.hi, PROT_NONE);
        apply_host_state(run.lo, run.hi, false, run.prot);
    }

    // Only now may a host page STOP requiring a software check: its hardware
    // protection is already exact, so a thread that observes the cleared byte
    // and takes the direct path is enforced by the page itself.
    if (m_linear_hw_protect && m_linear_check_map != nullptr) {
        std::atomic_thread_fence(std::memory_order_release);
        for (const auto& run : host_runs) {
            if (run.mixed) continue;
            // Clears only this owner's bit: a host page holding translated
            // code keeps kLinearCheckHoldsCode however exact its protection is.
            linear_check_bits_update(
                static_cast<GuestAddr>(run.lo), run.hi - run.lo,
                static_cast<u8>(kLinearCheckMixedProt | kLinearCheckMixedRead),
                /*set=*/false);
        }
    }

    // The JIT view last, decided from exactly the state just published (the
    // host protection of a uniform page, the subpage byte of a mixed one, the
    // check byte). Unowned runs were denied before their pages dropped, so a
    // freed terabyte costs nothing here.
    if (m_linear_jit_view != nullptr) {
        for (const auto& run : host_runs) {
            if (run.owned) linear_jit_view_refresh_locked(run.lo, run.hi);
        }
        (void)linear_jit_view_verify_after_reconcile(guest_lo, guest_hi);
    }
}

} // namespace vitreo
