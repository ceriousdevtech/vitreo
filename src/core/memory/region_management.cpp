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
#include <vitreo/ab_cycle.hpp>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <thread>
#include <pthread.h>

/// @file region_management.cpp
/// @brief Region Management
///
/// Closed cut from guest_memory.cpp — verified with plan_extraction.py.

namespace vitreo {

void GuestMemory::invalidate_tlb(bool discard_translations) noexcept {
    if (discard_translations) {
        m_translation_generation.fetch_add(1, std::memory_order_release);
    }
    // Preserve every existing JIT/backing invalidation, including partial
    // host-page reconciliation. Only the software TLB uses the split epoch.
    m_generation.fetch_add(1, std::memory_order_release);
}

GuestMemory::LinearProofWriteGuard::LinearProofWriteGuard(
    GuestMemory& memory) noexcept
    : m_memory(&memory) {
    m_memory->begin_linear_proof_write();
}

GuestMemory::LinearProofWriteGuard::~LinearProofWriteGuard() {
    m_memory->end_linear_proof_write();
}

GuestMemory::LinearProofLeaseShard&
GuestMemory::linear_proof_lease_shard() noexcept {
    // pthread_self() is a register read on Darwin (no TLV call). Its pointer
    // is page-aligned per thread; fold the page number into a shard index.
    const auto self = reinterpret_cast<uintptr_t>(pthread_self());
    const u64 h = (static_cast<u64>(self) >> 12) * 0x9E3779B97F4A7C15ull;
    static_assert(kLinearProofLeaseShards == 64,
                  "the shard index is the top 6 bits of the hash");
    return m_linear_proof_lease_shards[h >> (64 - 6)];
}

bool GuestMemory::try_enter_linear_proof_lease() noexcept {
    LinearProofLeaseShard& shard = linear_proof_lease_shard();
    shard.readers.fetch_add(1, std::memory_order_seq_cst);
    if (m_linear_proof_writer.load(std::memory_order_seq_cst) != 0) {
        shard.readers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

void GuestMemory::exit_linear_proof_lease() noexcept {
    linear_proof_lease_shard().readers.fetch_sub(1, std::memory_order_release);
}

void GuestMemory::begin_linear_proof_write() noexcept {
    // One writer at a time; a second waits for the first to finish.
    u32 expected = 0;
    while (!m_linear_proof_writer.compare_exchange_weak(
               expected, 1u, std::memory_order_seq_cst,
               std::memory_order_relaxed)) {
        expected = 0;
        std::this_thread::yield();
    }
    // New leases now fail; wait out the ones already entered.
    for (auto& shard : m_linear_proof_lease_shards) {
        while (shard.readers.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
    }
}

void GuestMemory::end_linear_proof_write() noexcept {
    m_linear_proof_writer.store(0, std::memory_order_release);
}

// =============================================================================
// Region Management
// =============================================================================

VoidResult GuestMemory::map_region(
    GuestAddr guest_addr, usize size, MemoryProtection prot,
    bool committed) {
    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    cleanup_zombies();
    // Validate alignment
    if (!is_aligned(static_cast<usize>(guest_addr), GUEST_PAGE_SIZE)) {
        return VoidResult::err(Error::InvalidArgument);
    }
    
    if (size == 0) {
        return VoidResult::err(Error::InvalidArgument);
    }
    
    // Check for overlap with existing regions
    const usize aligned_size = align_up(size, GUEST_PAGE_SIZE);
    const GuestAddr end_addr = guest_addr + aligned_size;
    
    // Existing regions are disjoint and ordered. Only the successor and
    // predecessor can overlap an insertion; scanning every region under the
    // exclusive lock stalls concurrent guest-memory readers unnecessarily.
    const auto next = regions_.lower_bound(guest_addr);
    if (next != regions_.end() && next->first < end_addr) {
        return VoidResult::err(Error::AlreadyExists);
    }
    if (next != regions_.begin()) {
        const auto previous = std::prev(next);
        if (guest_addr < previous->first + previous->second.requested_size) {
            return VoidResult::err(Error::AlreadyExists);
        }
    }

    publish_protect_shootdown(guest_addr, aligned_size, prot);
    
    // Reservations and committed PAGE_NOACCESS mappings both begin as
    // PROT_NONE host ranges. Their backing ownership remains distinct in the
    // committed interval map: a reservation may be discarded cheaply, while a
    // committed no-access page must retain contents across VirtualProtect.
    // Avoiding a ReadWrite memset for reservations is load-bearing for x64
    // apps that reserve multi-gigabyte pools and commit them lazily.
    const bool reserve_only =
        !committed || prot == MemoryProtection::None;

    // Linear model: back the region inside the reservation at its guest
    // offset. Scattered model: an independent host allocation (as before).
    // Either way the region is ReadWrite-and-zeroed (or PROT_NONE reserved)
    // here, and the shared code below applies the final protection.
    VResult<MemoryRegion> region_result = VResult<MemoryRegion>::err(Error::OutOfMemory);
    if (linear_backs(guest_addr, aligned_size)) {
        MemoryRegion r = map_in_reservation(guest_addr, aligned_size, reserve_only);
        if (!r.is_valid()) {
            return VoidResult::err(Error::OutOfMemory);
        }
        region_result = VResult<MemoryRegion>::ok(std::move(r));
    } else {
        region_result = MemoryRegion::allocate(
            aligned_size,
            reserve_only ? MemoryProtection::None : MemoryProtection::ReadWrite);
    }
    if (region_result.is_err()) {
        return VoidResult::err(region_result.error());
    }

    if (!reserve_only) {
        // A scattered region is a fresh anonymous mapping and a linear one
        // is fresh unless a live neighbour shares a host page; only then is
        // an explicit zero needed. Writing zeros into fresh pages would make
        // every committed byte resident before the guest touches it.
        const bool fresh = linear_backs(guest_addr, aligned_size)
            ? commit_pages_are_fresh_locked(guest_addr, aligned_size)
            : !is_identity();
        if (!fresh) {
            std::memset(region_result.value().data(), 0, aligned_size);
        }

        // Set the requested protection (if different from ReadWrite).
        if (prot != MemoryProtection::ReadWrite) {
            if (is_identity()) {
                // Native execution consumes these pages as code. Apply the
                // real host protection, including PROT_EXEC for RX.
                auto prot_result = region_result.value().set_protection(prot);
                if (prot_result.is_err()) {
                    return VoidResult::err(prot_result.error());
                }
            } else if (linear_backs(guest_addr, aligned_size)) {
                // Linear model: host pages are 16 KiB, so sub-page protection
                // can't be represented and a downgrade to RO/RX would EINVAL or
                // wrongly de-protect a RW neighbour sharing the host page. Keep
                // every committed page RW (over-permissive — guest code is
                // never host-executed, and the linear store path's code-span
                // check still drives SMC) and only record the requested flag
                // for VirtualQuery. Unmapped/reserved pages stay PROT_NONE.
                region_result.value().set_protection_flag_only(prot);
            } else {
                auto prot_result = region_result.value().set_protection(prot);
                if (prot_result.is_err()) {
                    return VoidResult::err(prot_result.error());
                }
            }
        }
    }
    
    // Store the mapping
    const usize requested = aligned_size;  // Store the requested size before rounding to host page
    MappedRegion mapped{
        guest_addr,
        requested,
        std::move(region_result.value()),
        {{0, prot}},
        {{0, committed}},
    };
    total_size_ += mapped.requested_size;
    regions_.emplace(guest_addr, std::move(mapped));
    // With hardware enforcement the freshly-backed span is RW from
    // map_in_reservation; reconcile applies the requested protection (and the
    // most-restrictive meet with whatever shares its 16 KiB host pages).
    if (linear_backs(guest_addr, aligned_size) && m_linear_hw_protect) {
        linear_reconcile_host_pages(guest_addr, aligned_size,
                                    /*discard_unowned=*/false);
    }
    // A non-overlapping insertion cannot invalidate an existing translation.
    // Misses are not cached. The range shootdown above publishes the new
    // mapping to permission/JIT caches without discarding unrelated regions.

    return VoidResult::ok();
}

VoidResult GuestMemory::map_region_backed(GuestAddr guest_addr, usize size,
                                          void* host_ptr, MemoryProtection prot) {
    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    cleanup_zombies();
    if (host_ptr == nullptr) {
        return VoidResult::err(Error::InvalidArgument);
    }
    if (!is_aligned(static_cast<usize>(guest_addr), GUEST_PAGE_SIZE)) {
        return VoidResult::err(Error::InvalidArgument);
    }
    if (size == 0) {
        return VoidResult::err(Error::InvalidArgument);
    }

    const usize aligned_size = align_up(size, GUEST_PAGE_SIZE);
    const GuestAddr end_addr = guest_addr + aligned_size;

    // Existing regions are disjoint and ordered. Only the successor and
    // predecessor can overlap an insertion; scanning every region under the
    // exclusive lock stalls concurrent guest-memory readers unnecessarily.
    const auto next = regions_.lower_bound(guest_addr);
    if (next != regions_.end() && next->first < end_addr) {
        return VoidResult::err(Error::AlreadyExists);
    }
    if (next != regions_.begin()) {
        const auto previous = std::prev(next);
        if (guest_addr < previous->first + previous->second.requested_size) {
            return VoidResult::err(Error::AlreadyExists);
        }
    }

    publish_protect_shootdown(guest_addr, aligned_size, prot);

    // Linear model: the shared pages live at host_ptr, but the guest (and the
    // JIT) reach this address at linear_base+guest_addr. Alias the same
    // physical pages into the reservation there with mach_vm_remap (copy=FALSE
    // ⇒ genuine sharing: writes through either VA are mutually visible). The
    // region then borrows the reservation address, keeping resolve_addr and the
    // JIT's host_base+addr consistent.
    Byte* backing = static_cast<Byte*>(host_ptr);
    // An out-of-window backed region skips the reservation remap and borrows
    // the caller's pages directly, exactly as the scattered model does; the
    // region table resolves it and the JIT never touches it directly.
    if (linear_backs(guest_addr, aligned_size)) {
        if (m_linear_identity) {
            // Own the span first (record host occupants as holes). Remap
            // OVERWRITE onto a mixed host/guest range would destroy malloc.
            if (!identity_prepare_span(guest_addr, aligned_size, prot,
                                       /*existing_guest_region=*/false)) {
                return VoidResult::err(Error::OutOfMemory);
            }
        } else if (guest_addr > m_linear_size ||
                   aligned_size > m_linear_size - guest_addr) {
            return VoidResult::err(Error::InvalidArgument);
        } else if (!linear_extension_materialize(guest_addr, aligned_size)) {
            return VoidResult::err(Error::OutOfMemory);
        }
        mach_vm_address_t expected =
            reinterpret_cast<mach_vm_address_t>(linear_host(guest_addr));
        mach_vm_address_t target = expected;
        vm_prot_t cur_prot = 0, max_prot = 0;
        kern_return_t kr = mach_vm_remap(
            mach_task_self(), &target, aligned_size, 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
            reinterpret_cast<mach_vm_address_t>(host_ptr), /*copy=*/FALSE,
            &cur_prot, &max_prot, VM_INHERIT_SHARE);
        if (kr != KERN_SUCCESS || target != expected) {
            if (m_linear_identity) {
                // PROT_NONE placeholders sometimes reject OVERWRITE. Drop
                // the reservation and remap into the vacated VA.
                (void)mach_vm_deallocate(mach_task_self(), expected,
                                         aligned_size);
                target = expected;
                cur_prot = 0;
                max_prot = 0;
                kr = mach_vm_remap(
                    mach_task_self(), &target, aligned_size, 0,
                    VM_FLAGS_FIXED, mach_task_self(),
                    reinterpret_cast<mach_vm_address_t>(host_ptr),
                    /*copy=*/FALSE, &cur_prot, &max_prot, VM_INHERIT_SHARE);
            }
        }
        if (kr != KERN_SUCCESS || target != expected) {
            if (m_linear_identity) {
                std::fprintf(stderr,
                             "[memory] identity remap failed guest=0x%llx "
                             "size=0x%zx kr=%d\n",
                             static_cast<unsigned long long>(guest_addr),
                             aligned_size, static_cast<int>(kr));
                (void)identity_restore_span(guest_addr, aligned_size);
            }
            return VoidResult::err(Error::OutOfMemory);
        }
        backing = linear_host(guest_addr);
        if (m_linear_jit_view != nullptr) {
            // The reservation now maps the shared pages; point the JIT view at
            // them too (the reconcile below sets its protection).
            const usize hp = usize{1} << kLinearHostPageShift;
            const usize lo = static_cast<usize>(guest_addr) & ~(hp - 1);
            const usize hi = (static_cast<usize>(guest_addr) + aligned_size +
                              hp - 1) & ~(hp - 1);
            std::lock_guard view_lock(m_jit_view_mutex);
            linear_jit_view_realias_locked(lo, hi);
        }
    }

    // Wrap the externally-owned buffer WITHOUT taking ownership. Do NOT zero
    // (shared memory may already contain a peer's data) and do NOT mprotect
    // (the caller's mmap already set host-page protection).
    MemoryRegion region = MemoryRegion::borrow(backing, aligned_size, prot);
    MappedRegion mapped{
        guest_addr,
        aligned_size,
        std::move(region),
        {{0, prot}},
        {{0, true}},
    };
    total_size_ += mapped.requested_size;
    regions_.emplace(guest_addr, std::move(mapped));
    // The remap gave us our own VM entry onto the shared pages, so mprotect
    // here narrows only this view — the peer keeps its own protection.
    if (linear_backs(guest_addr, aligned_size) && m_linear_hw_protect) {
        linear_reconcile_host_pages(guest_addr, aligned_size,
                                    /*discard_unowned=*/false);
    }
    // A non-overlapping insertion cannot invalidate an existing translation.
    // Misses are not cached. The range shootdown above publishes the new
    // mapping to permission/JIT caches without discarding unrelated regions.

    return VoidResult::ok();
}

bool GuestMemory::back_region_with_file(GuestAddr guest_addr, usize size,
                                        int fd, u64 file_offset) {
    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    const usize hp = usize{1} << kLinearHostPageShift;
    if (fd < 0 || size == 0 || m_linear_base == nullptr || m_linear_identity ||
        !linear_backs(guest_addr, size) ||
        (static_cast<usize>(guest_addr) & (hp - 1)) != 0 ||
        (size & (hp - 1)) != 0 || (file_offset & (hp - 1)) != 0) {
        return false;
    }
    auto it = regions_.upper_bound(guest_addr);
    if (it == regions_.begin()) return false;
    --it;
    const MappedRegion& owner = it->second;
    if (guest_addr < owner.base ||
        guest_addr + size > owner.base + owner.requested_size ||
        owner.committed.size() != 1 || !owner.committed.begin()->second) {
        return false;
    }

    void* const host = linear_host(guest_addr);
    void* placed = mmap(host, size, PROT_READ | PROT_WRITE,
                        MAP_FIXED | MAP_PRIVATE, fd,
                        static_cast<off_t>(file_offset));
    const bool ok = placed == host;
    if (!ok) {
        // MAP_FIXED may have released the old entry; give the range fresh
        // zero pages, which is what an unwritten committed region holds.
        placed = mmap(host, size, PROT_READ | PROT_WRITE,
                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (placed != host) {
            std::fprintf(stderr,
                         "[memory] file-backing fallback failed guest=0x%llx "
                         "size=0x%zx errno=%d\n",
                         static_cast<unsigned long long>(guest_addr), size,
                         errno);
        }
    }
    if (m_linear_jit_view != nullptr) {
        std::lock_guard view_lock(m_jit_view_mutex);
        linear_jit_view_realias_locked(static_cast<usize>(guest_addr),
                                       static_cast<usize>(guest_addr) + size);
    }
    if (m_linear_hw_protect) {
        linear_reconcile_host_pages(guest_addr, size, /*discard_unowned=*/false);
    }
    return ok;
}

VoidResult GuestMemory::unmap_region(GuestAddr guest_addr, usize size) {
    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    // For simplicity, we require exact match on the start address
    auto it = regions_.find(guest_addr);
    if (it == regions_.end()) {
        return VoidResult::err(Error::NotFound);
    }
    
    // Match against requested size, not actual allocated size
    if (size != it->second.requested_size) {
        return VoidResult::err(Error::NotSupported);
    }

    VITREO_TRACE("[vmem] unmap_region base=0x%llx size=0x%llx\n",
                 static_cast<unsigned long long>(guest_addr),
                 static_cast<unsigned long long>(size));
    publish_protect_shootdown(guest_addr, size, MemoryProtection::None);
    m_images.unregister_overlapping(guest_addr, it->second.requested_size);

    total_size_ -= it->second.requested_size;

    if (is_identity()) {
        const usize sz = it->second.requested_size;
        regions_.erase(it);
        lower_fit_floors(guest_addr);
        // Keep the VA. munmap hands it to malloc/Metal; the next guest
        // VirtualAlloc/MapView then hits a host occupant and returns
        // ERROR_NOT_ENOUGH_MEMORY.
        (void)identity_restore_span(guest_addr, sz);
        invalidate_tlb();
        return VoidResult::ok();
    }

    // Linear model: the region borrows the reservation, so its dtor won't free
    // anything. Decommit the sub-range back to PROT_NONE — this both releases
    // the physical pages and restores the reservation placeholder so the guest
    // address can be re-mapped later. (A subsequent access faults, matching an
    // unmapped page.) The requested (guest-facing) size is what was committed.
    if (linear_backs(guest_addr, it->second.requested_size)) {
        // Remove the region-table owner first, then derive each affected host
        // page's state from the remaining mappings. Apple Silicon host pages
        // are 16 KiB, so adjacent 4 KiB guest allocations can share one. The
        // shared page must remain RW (with software permission checks enforcing
        // the guest boundary) until its final committed owner is removed.
        const usize sz = it->second.requested_size;
        regions_.erase(it);
        lower_fit_floors(guest_addr);
        linear_reconcile_host_pages(guest_addr, sz, /*discard_unowned=*/true);
        // Every cached host pointer stays valid memory here (the reservation
        // outlives the region), so the range shootdown published above, under
        // this exclusive lock, is enough: each thread drops the translations
        // overlapping the range at its next access, and a refill can only see
        // the table without the region. Discarding every thread's whole TLB
        // instead costs ~1.7k TLB flushes and ~50k region-table misses a second
        // while the guest issues one VirtualFree at a time.
        // (VITREO_AB_CYCLE_S: the off arm discards, for an in-process A/B.)
        invalidate_tlb(
            /*discard_translations=*/diagnostics::ab_cycle_off("tlb-unmap"));
        return VoidResult::ok();
    }

    // Defer destruction of the MemoryRegion so lock-free TLB readers
    // that resolved a host pointer before the generation bump can
    // safely complete their access.
    u64 cur_gen = m_generation.load(std::memory_order_relaxed);
    m_zombie_regions.push_back({cur_gen, std::move(it->second.region)});

    regions_.erase(it);
    lower_fit_floors(guest_addr);
    invalidate_tlb();
    cleanup_zombies();

    return VoidResult::ok();
}

void GuestMemory::cleanup_zombies() {
    // Called under unique_lock.  Free zombies from old generations.
    u64 cur_gen = m_generation.load(std::memory_order_relaxed);
    constexpr u64 kZombieGap = 32;
    if (cur_gen > kZombieGap) {
        u64 threshold = cur_gen - kZombieGap;
        auto it = std::remove_if(
            m_zombie_regions.begin(), m_zombie_regions.end(),
            [threshold](const ZombieRegion& z) { return z.gen <= threshold; });
        m_zombie_regions.erase(it, m_zombie_regions.end());
    }
}

GuestMemory::ProtectionSpan GuestMemory::protection_span(
    const MappedRegion& mapped, usize offset) {
    auto it = mapped.protections.upper_bound(offset);
    if (it == mapped.protections.begin()) {
        return {0, 0, MemoryProtection::None};
    }
    --it;
    const auto next = std::next(it);
    return {
        it->first,
        next == mapped.protections.end()
            ? mapped.requested_size
            : next->first,
        it->second,
    };
}

void GuestMemory::set_protection_range(
    MappedRegion& mapped, usize offset, usize size,
    MemoryProtection protection) {
    if (size == 0) return;
    const usize end = offset + size;
    const MemoryProtection end_protection =
        end < mapped.requested_size
            ? protection_span(mapped, end).protection
            : MemoryProtection::None;
    mapped.protections[offset] = protection;
    auto erase_begin = mapped.protections.upper_bound(offset);
    auto erase_end = mapped.protections.lower_bound(end);
    mapped.protections.erase(erase_begin, erase_end);
    if (end < mapped.requested_size) {
        mapped.protections[end] = end_protection;
    }

    // The map was coalesced before this update. Only its two changed
    // boundaries can now have the same value as their predecessor.
    auto first = mapped.protections.find(offset);
    if (first != mapped.protections.begin() &&
        std::prev(first)->second == first->second) {
        mapped.protections.erase(first);
    }
    if (end < mapped.requested_size) {
        auto last = mapped.protections.find(end);
        if (last != mapped.protections.begin() &&
            std::prev(last)->second == last->second) {
            mapped.protections.erase(last);
        }
    }
}

void GuestMemory::set_committed_range(
    MappedRegion& mapped, usize offset, usize size, bool committed) {
    if (size == 0) return;
    const usize end = offset + size;
    const auto committed_at = [&](usize query_offset) {
        auto it = mapped.committed.upper_bound(query_offset);
        --it;
        return it->second;
    };
    const bool committed_after =
        end < mapped.requested_size ? committed_at(end) : false;

    mapped.committed[offset] = committed;
    auto erase_begin = mapped.committed.upper_bound(offset);
    auto erase_end = mapped.committed.lower_bound(end);
    mapped.committed.erase(erase_begin, erase_end);
    if (end < mapped.requested_size) {
        mapped.committed[end] = committed_after;
    }

    // The map was coalesced before this update. Only its two changed
    // boundaries can now have the same value as their predecessor.
    auto first = mapped.committed.find(offset);
    if (first != mapped.committed.begin() &&
        std::prev(first)->second == first->second) {
        mapped.committed.erase(first);
    }
    if (end < mapped.requested_size) {
        auto last = mapped.committed.find(end);
        if (last != mapped.committed.begin() &&
            std::prev(last)->second == last->second) {
            mapped.committed.erase(last);
        }
    }
}

bool GuestMemory::has_committed_span(const MappedRegion& mapped) {
    return std::any_of(
        mapped.committed.begin(), mapped.committed.end(),
        [](const auto& entry) {
            return entry.second;
        });
}

VoidResult GuestMemory::protect_region(GuestAddr guest_addr, usize size, MemoryProtection prot) {
    // A range becoming executable is the moment generated or patched bytes
    // become callable, so it is the publish point for block-cache
    // invalidation -- exactly the rule kernel32's VirtualProtect already
    // applies to guest-driven transitions (winapi/kernel32/memory/
    // virtualprotect.cpp). Vitreo's own patchers do the identical dance and
    // were invisible to it: guest_accel.cpp drops a thunk sled page to RW,
    // writes `jmp rel32`, and restores RX, so write_bytes() sees a
    // non-executable page and skips notify_executable_write(); native32 and
    // native64_prepare go further and rewrite every thunk stub through a raw
    // linear_host() pointer, which never notifies at all. Catching the
    // transition here covers those, the PE loader's relocate/IAT pass (which
    // protects whole sections RW and back), and any future patcher, without
    // asking each site to remember.
    //
    // Ordering today makes the gap latent rather than reachable: both
    // accelerator installs run before any JitCompiler exists. This is
    // therefore hygiene against a future re-patch, and against any future
    // change that publishes blocks at exactly these sled addresses.
    //
    // Declared BEFORE the lock so it runs after ~unique_lock and
    // ~LinearProofWriteGuard: the observer reaches into the JIT block cache
    // and must never be entered while m_regions_mutex is held. Armed up front
    // rather than per success path -- a notify after a *failed* protect costs
    // at most one spurious invalidation, while getting the arming wrong on one
    // of several exits would cost correctness.
    struct ExecutableTransitionNotify {
        GuestMemory* self;
        GuestAddr addr;
        usize size;
        bool armed;
        ~ExecutableTransitionNotify() {
            if (armed) self->notify_executable_write(addr, size);
        }
    } exec_notify{this, guest_addr, size,
                  size != 0 && has_flag(prot, MemoryProtection::Execute)};
    m_images.note_modified(guest_addr, size);

    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    // Find the region
    MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return VoidResult::err(Error::NotFound);
    }
    
    // Verify the range is within the region
    const usize offset = guest_addr - mapped->base;
    if (!mapped->region.contains(offset, size)) {
        return VoidResult::err(Error::InvalidArgument);
    }

    publish_protect_shootdown(guest_addr, size, prot);

    if (is_identity()) {
        if (!identity_host_protect(guest_addr, size, prot)) {
            return VoidResult::err(Error::AccessDenied);
        }
        set_protection_range(*mapped, offset, size, prot);
        return VoidResult::ok();
    }

    set_protection_range(*mapped, offset, size, prot);
    // VirtualProtect changes accessibility, not backing ownership. With
    // hardware enforcement the reservation carries the new accessibility too,
    // composed most-restrictively per 16 KiB host page; without it, every
    // committed host page stays RW and the protection runs above are the only
    // enforcement (which is why every JIT direct access has to re-check).
    if (linear_backs(guest_addr, size)) {
        if (m_linear_hw_protect) {
            linear_reconcile_host_pages(guest_addr, size,
                                        /*discard_unowned=*/false);
            invalidate_tlb(false);
        }
        return VoidResult::ok();
    }

    const MemoryProtection host_protection =
        has_committed_span(*mapped)
            ? MemoryProtection::ReadWrite
            : MemoryProtection::None;
    if (mapped->region.protection() == host_protection) {
        return VoidResult::ok();
    }
    return mapped->region.set_protection(host_protection);
}

VoidResult GuestMemory::commit_region(
    GuestAddr guest_addr, usize size, MemoryProtection prot) {
    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return VoidResult::err(Error::NotFound);
    }

    const usize offset = guest_addr - mapped->base;
    if (!mapped->region.contains(offset, size)) {
        return VoidResult::err(Error::InvalidArgument);
    }

    publish_protect_shootdown(guest_addr, size, prot);

    if (is_identity()) {
        if (!identity_host_protect(guest_addr, size, prot)) {
            return VoidResult::err(Error::AccessDenied);
        }
        set_committed_range(*mapped, offset, size, true);
        set_protection_range(*mapped, offset, size, prot);
        invalidate_tlb(false);
        return VoidResult::ok();
    }

    set_committed_range(*mapped, offset, size, true);
    set_protection_range(*mapped, offset, size, prot);
    if (linear_backs(guest_addr, size)) {
        linear_reconcile_host_pages(
            guest_addr, size, /*discard_unowned=*/false);
        invalidate_tlb(false);
        return VoidResult::ok();
    }

    auto result = mapped->region.set_protection(MemoryProtection::ReadWrite);
    if (result.is_ok()) {
        invalidate_tlb(false);
    }
    return result;
}

VoidResult GuestMemory::decommit_region(GuestAddr guest_addr, usize size) {
    m_images.note_modified(guest_addr, size);
    LinearProofWriteGuard proof_write{*this};
    std::unique_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return VoidResult::err(Error::NotFound);
    }

    const usize offset = guest_addr - mapped->base;
    if (!mapped->region.contains(offset, size)) {
        return VoidResult::err(Error::InvalidArgument);
    }

    publish_protect_shootdown(
        guest_addr, size, MemoryProtection::None);

    if (is_identity()) {
        if (!identity_host_protect(guest_addr, size, MemoryProtection::None)) {
            return VoidResult::err(Error::AccessDenied);
        }
        set_protection_range(*mapped, offset, size, MemoryProtection::None);
        set_committed_range(*mapped, offset, size, false);
        invalidate_tlb(false);
        return VoidResult::ok();
    }

    set_protection_range(*mapped, offset, size, MemoryProtection::None);
    set_committed_range(*mapped, offset, size, false);
    if (linear_backs(guest_addr, size)) {
        linear_reconcile_host_pages(
            guest_addr, size, /*discard_unowned=*/true);
        invalidate_tlb(false);
        return VoidResult::ok();
    }

    const MemoryProtection host_protection =
        has_committed_span(*mapped)
            ? MemoryProtection::ReadWrite
            : MemoryProtection::None;
    auto result = mapped->region.set_protection(host_protection);
    if (result.is_ok()) {
        invalidate_tlb(false);
    }
    return result;
}

bool GuestMemory::query_region(GuestAddr guest_addr,
                               GuestAddr& base,
                               usize& requested_size,
                               MemoryProtection& prot) const {
    std::shared_lock lk(m_regions_mutex);
    const MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return false;
    }

    const usize offset = static_cast<usize>(guest_addr - mapped->base);
    const ProtectionSpan span = protection_span(*mapped, offset);
    base = mapped->base + span.begin;
    requested_size = span.end - span.begin;
    prot = span.protection;
    return true;
}

bool GuestMemory::query_mapping(GuestAddr guest_addr, GuestAddr& base,
                                usize& requested_size) const {
    std::shared_lock lk(m_regions_mutex);
    const MappedRegion* mapped = find_region(guest_addr);
    if (mapped == nullptr) {
        return false;
    }
    base = mapped->base;
    requested_size = mapped->requested_size;
    return true;
}

} // namespace vitreo
