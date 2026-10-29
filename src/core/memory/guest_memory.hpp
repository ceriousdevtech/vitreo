#pragma once

/// @file guest_memory.hpp
/// @brief Guest memory space management
///
/// Manages the guest's virtual address space, mapping guest addresses
/// to host memory regions.

#include "memory_region.hpp"
#include "guest_image_registry.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <core/sync/host_rwlock.hpp>
#include <core/sync/spin_rwlock.hpp>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo {

// =============================================================================
// Memory Access Types
// =============================================================================

/// Result of a memory read operation
template<typename T>
using ReadResult = VResult<T>;

/// Result of a memory write operation
using WriteResult = VoidResult;

/// Guest pages per host page under linear hardware protection. Apple Silicon
/// host pages are 16 KiB; Windows protects at 4 KiB.
inline constexpr usize kLinearHostPageShift = 14;   // 16 KiB
inline constexpr usize kLinearHostPageSize = usize{1} << kLinearHostPageShift;

/// Size of a RUNTIME-chosen guest-visible allocation (a TEB, a TLS array or
/// block, a heap's large block, a mapped D3D resource): rounded up to whole
/// 16 KiB host pages.
///
/// The guest's own VirtualAlloc keeps Windows' 4 KiB sizes exactly. Where the
/// runtime picks the size, nothing in Windows states it, so it should not
/// leave the rest of a host page unmapped. A host page that is part mapped
/// and part free cannot be protected by hardware (the free part must fault,
/// the mapped part must not), so it is "mixed", and every JIT access to the
/// mapped part takes the checked helper. Measured ~1M such
/// accesses per second, nearly all to TEBs, TLS blocks, and heap blocks
/// whose last host page was partly free.
[[nodiscard]] constexpr u64 round_up_host_page(u64 bytes) noexcept {
    return (bytes + kLinearHostPageSize - 1) &
           ~static_cast<u64>(kLinearHostPageSize - 1);
}

/// Padding bytes allocated past the last real entry of the linear check-page
/// map. An access of up to 8 bytes spans at most two ADJACENT host pages, so
/// the JIT's store probe reads map[i] and map[i+1] with one halfword load
/// rather than computing a second index and ORing two byte loads. At the last
/// page that read would otherwise leave the mapping.
inline constexpr usize kCheckMapPadBytes = 1;

/// Largest linear window that can carry a JIT view (see
/// `GuestMemory::enable_linear_jit_view`). Two views of a 2^46 window do not
/// fit in the host's 2^47 user address space beside everything else.
inline constexpr usize kLinearJitViewMaxWindow = usize{1} << 45;

/// PROT_NONE address space, never mapped, on EACH side of the JIT view. A
/// translated access whose base register has passed the window test may add
/// any 32-bit displacement and still land inside the view or a guard, so it
/// faults instead of reaching unrelated host memory.
inline constexpr usize kLinearJitViewGuardBytes = usize{1} << 32;

/// Reasons a 16 KiB host page of the linear reservation cannot be reached by a
/// direct JIT access. One bit each, one byte per host page: see
/// `GuestMemory::linear_check_page_map()`.
///
/// `MixedProt` applies to loads and stores alike; `HoldsCode` is a store-only
/// concern, so a load tests that one bit and a store tests the whole byte.
enum : u8 {
    /// The host page's hardware protection is more permissive than the guest's,
    /// because the 4 KiB guest pages inside it disagree and 16 KiB `mprotect`
    /// cannot state their protections separately. Published by
    /// `linear_reconcile_host_pages` under `m_regions_mutex`.
    kLinearCheckMixedProt = 1u << 0,
    /// The host page contains at least one 4 KiB guest page holding translated
    /// code, so a store into it may be self-modifying code and must reach the
    /// helper that invalidates the translation. Published by
    /// `retain_jit_code_page` / `release_jit_code_page` under the host page's
    /// code-page shard lock, before the translation becomes reachable.
    kLinearCheckHoldsCode = 1u << 1,
    /// The host page belongs to a live `MEM_WRITE_WATCH` allocation, so a store
    /// into it must reach the helper that records the page dirty. Published by
    /// `VirtualMemoryManager::publish_write_watch_locked` over the allocation's
    /// whole range, before the allocation is usable.
    ///
    /// This bit REPLACED a process-global test. The store fast path used to load
    /// a published-map slot and branch on it, which meant one live write-watch
    /// allocation anywhere sent EVERY x64 store in the process through the
    /// helper — and a guest GC is exactly the thing that keeps such an
    /// allocation live for long stretches. Per-page is not merely cheaper, it is
    /// the correct precision: `GetWriteWatch` only ever reports pages inside the
    /// watched allocation, so a store outside it never had anything to record.
    ///
    /// Over-report, never under-report (see the recorded getwritewatch_gc
    /// landmine: an under-reported dirty page makes a guest GC free a live
    /// object). The bit therefore covers whole 16 KiB host pages even when the
    /// allocation's 4 KiB edges only partly fill them, and it is published
    /// before the region is registered and cleared only after it is
    /// unregistered.
    kLinearCheckWatched = 1u << 2,
    /// The host page's protection would let a READ succeed that the guest
    /// denies, so a load must take the checked helper too.
    ///
    /// Split out of `kLinearCheckMixedProt` because that bit is far broader
    /// than reads need. "Mixed" means the 4 KiB guest pages inside disagree,
    /// and the overwhelmingly common disagreement is about WRITABILITY --
    /// `.rdata` beside `.data`, or `.text` beside `.rdata`, which a PE's 4 KiB
    /// SectionAlignment puts in one 16 KiB host page as a matter of course.
    /// Every one of those pages is uniformly READABLE, so gating loads on the
    /// write-oriented bit sent an enormous number of harmless reads to the
    /// helper: measured on `sc_blocksize` rung 1, **686,080,048 helper reads,
    /// 100% of them attributed to `kLinearCheckMixedProt`**, with the compile
    /// -time counter reporting 100% of accesses emitted on the linear path.
    ///
    /// Set only when hardware really could allow a read the guest forbids:
    ///   * some guest page inside denies read while the host page grants it
    ///     (uncommitted, `PAGE_NOACCESS`); or
    ///   * a guest page's protection has no exact host form and the inexactness
    ///     is precisely "guest denies read, host must grant it"
    ///     (`PAGE_EXECUTE` without read, write without read) -- which is what
    ///     `linear_host_prot_is_exact` already tests.
    ///
    /// Over-report, never under-report: an unset bit here lets a load bypass
    /// the software check, so anything not proven read-safe must set it.
    kLinearCheckMixedRead = 1u << 3,
};

/// Bit index of `kLinearCheckMixedProt`, for the emitted single-bit `TBNZ` a
/// load uses. Kept beside the flag so the two cannot drift apart.
inline constexpr u8 kLinearCheckMixedProtBit = 0;
static_assert(kLinearCheckMixedProt == (1u << kLinearCheckMixedProtBit));
/// Bit index of `kLinearCheckMixedRead`, for the emitted single-bit `TBNZ` a
/// LOAD uses. A store keeps testing `kLinearCheckMixedProtBit`.
inline constexpr u8 kLinearCheckMixedReadBit = 3;
static_assert(kLinearCheckMixedRead == (1u << kLinearCheckMixedReadBit));
inline constexpr usize kLinearGuestPageShift = 12;  // 4 KiB

// =============================================================================
// GuestMemory Class
// =============================================================================

/// Manages guest virtual address space
///
/// Maps guest addresses to host memory, handling:
/// - Address translation (guest → host)
/// - Memory protection
/// - Read/write operations with proper endianness
class GuestMemory {
public:
    /// Create an empty guest memory space
    GuestMemory() {
        // Each instance gets a unique starting generation so that
        // thread-local TLB caches from a prior instance at the same
        // address are automatically invalidated.
        // Gap of 1024 ensures that even after many invalidate_tlb() bumps,
        // the next instance's generation cannot collide with a prior one.
        static std::atomic<u64> s_next_gen{1024};
        const u64 initial = s_next_gen.fetch_add(1024, std::memory_order_relaxed);
        m_generation.store(initial, std::memory_order_relaxed);
        m_translation_generation.store(initial, std::memory_order_relaxed);
    }
    
    /// No copy
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    
    /// Move operations (deleted: shared_mutex and atomic are non-movable)
    GuestMemory(GuestMemory&&) = delete;
    GuestMemory& operator=(GuestMemory&&) = delete;
    
    /// Destructor
    ~GuestMemory();

    // -------------------------------------------------------------------------
    // Linear (guest-base) reservation. 32-bit only.
    // -------------------------------------------------------------------------

    /// Reserve @p size bytes of contiguous host address space (PROT_NONE) so
    /// that every subsequently mapped region is backed at
    /// `linear_base() + guest_addr`. Enables the future single-instruction
    /// JIT memory path. Must be called before any region is mapped (32-bit
    /// guests, where @p size == 4 GiB fully covers the address space).
    /// @return true on success; false if a reservation already exists, regions
    ///         are already mapped, or the host mmap failed (caller falls back
    ///         to the scattered-allocation model — behavior is identical).
    ///
    /// @p extend_to_va_limit (x64 only): after reserving the power-of-two
    /// core, also reserve the host range from its end up to just below a
    /// check map placed at the task's VA limit, so guest addresses up to
    /// linear_size() -- nearly the whole 47-bit Windows user space, not just
    /// the core -- are backed at `linear_base() + guest_addr` and reach the
    /// range-by-fault fast path. The core alone holds only 2^46 because the
    /// host's user VA (MACH_VM_MAX_ADDRESS, ~2^47) cannot hold a second 2^46
    /// window; the extension takes what is left above it. Declined (core
    /// only) when the check map cannot end at the VA limit.
    [[nodiscard]] bool enable_linear_reservation(usize size,
                                                 bool extend_to_va_limit = false);

    /// Host base of the linear reservation, or nullptr if not enabled. When
    /// non-null, `linear_base() + guest_addr` is the host pointer for any
    /// mapped guest address — no region lookup needed.
    [[nodiscard]] u8* linear_base() const noexcept { return m_linear_base; }
    /// Guest addresses [0, linear_size()) are backed inside the reservation.
    /// The power-of-two core plus any extension (see
    /// enable_linear_reservation); not necessarily a power of two.
    [[nodiscard]] usize linear_size() const noexcept { return m_linear_size; }
    /// The power-of-two core of the reservation (== linear_size() without an
    /// extension). Emitted code that bounds an EA with a shift (`LSR #bits`)
    /// or masks an index (`UBFX`) must use this, never linear_size().
    [[nodiscard]] usize linear_power_of_two_size() const noexcept {
        return m_linear_pow2_size;
    }
    [[nodiscard]] bool is_linear() const noexcept {
        return m_linear_base != nullptr || m_linear_identity;
    }

    /// True when @p base..@p base+@p size can be backed inside the linear
    /// reservation. The Windows x64 user address space is 128 TiB (ceiling
    /// 0x7FFF'FFFEFFFF) and is never capped to the reservation: callers may
    /// place reservations anywhere in it (fixed lpAddress). The reservation
    /// (core 2^46 plus the extension up to the check map at the host VA
    /// limit, less 1 TiB of host headroom) covers all but its top ~1.1%; a
    /// region above linear_size()
    /// falls back to the scattered model, the region table resolves it, and
    /// the JIT sends every direct access to it to the checked helper.
    ///
    /// Identity mapping has no window (reserved holes bound it instead), so
    /// under identity every range counts as reservation-backed, preserving
    /// its existing behavior.
    [[nodiscard]] bool linear_backs(GuestAddr base, usize size) const noexcept {
        if (!is_linear()) return false;
        if (m_linear_identity) return true;
        return static_cast<usize>(base) <= m_linear_size &&
               size <= m_linear_size - static_cast<usize>(base);
    }

    /// Enable IDENTITY mapping: host address == guest address, no offset.
    ///
    /// Required by a natively-executing backend. Translated backends can put
    /// the guest anywhere and add a base, because every guest address passes
    /// through translation. Natively executed guest code emits absolute
    /// addresses that the CPU consumes directly, so the guest must be mapped
    /// where it believes it is.
    ///
    /// Must precede any mapping, like enable_linear_reservation(), and the two
    /// are mutually exclusive. The caller is responsible for having reserved
    /// the guest ranges from the host. The low 4GB is available only when the
    /// host image is linked so it does not own that range.
    [[nodiscard]] bool enable_identity_mapping();

    [[nodiscard]] bool is_identity() const noexcept { return m_linear_identity; }

    /// Enforce guest protection in HARDWARE inside the linear reservation.
    ///
    /// Off, the reservation carries only committed->RW / uncommitted->PROT_NONE
    /// and the guest-visible R/W/NOACCESS state lives purely in the protection
    /// runs, so the reservation proves an address is *mapped* and never that
    /// the guest may touch it. Every JIT direct access then has to re-check
    /// permission in software (an eleven-instruction slot probe plus an
    /// LDAR-ed protect-generation check, per access).
    ///
    /// On, `linear_reconcile_host_pages` becomes the single owner of host-page
    /// protection. The region table stays authoritative; hardware is an
    /// accelerator layered under it, and the two granularities are reconciled
    /// per 16 KiB host page:
    ///
    ///   * UNIFORM host page — every 4 KiB guest page inside it has the same
    ///     committed state and protection. The host protection is then EXACTLY
    ///     the guest protection, so hardware enforcement is precise and the JIT
    ///     may access it directly with no software check at all.
    ///   * MIXED host page — the guest pages inside disagree. No single host
    ///     protection can be right: the most restrictive one denies accesses
    ///     the guest permits (and PE images guarantee such pages, because
    ///     sections carry different protections at 4 KiB SectionAlignment, so
    ///     a store to the first 4 KiB of .data next to .rdata would fault on
    ///     every iteration), and the most permissive one allows accesses the
    ///     guest forbids. The page is therefore left at the most PERMISSIVE
    ///     protection and `kLinearCheckMixedProt` is set in its byte in
    ///     `linear_check_page_map()`, which routes every JIT access to it
    ///     through the checked helper — the software enforcement that was
    ///     previously applied to all memory.
    ///
    /// So software permission checking never disappears; it stops being paid
    /// on the ~all-uniform majority of pages. Mixed pages are exactly as
    /// correct, and exactly as fast, as the whole world is today.
    ///
    /// Must precede any mapping. Meaningless (and refused) under identity
    /// mapping, which already applies real host protection.
    [[nodiscard]] bool enable_linear_hardware_protection();

    [[nodiscard]] bool linear_hardware_protection() const noexcept {
        return m_linear_hw_protect;
    }

    /// Map the reservation's pages a SECOND time, for translated code only,
    /// with protections that ENFORCE the guest's.
    ///
    /// The reservation itself -- the HOST view -- must stay permissive on three
    /// kinds of 16 KiB host page, because host C++ code reaches guest memory
    /// through it (`linear_base() + addr`): a MIXED page (its 4 KiB guest pages
    /// disagree), a page holding translated code (a store must reach the SMC
    /// helper) and a write-watched page (a store must be recorded). So every
    /// JIT access into it pays a check-map probe. The JIT view carries, per
    /// host page:
    ///
    ///   * NONE when any guest page inside is uncommitted or has no exact
    ///     host form (execute-only, write-only);
    ///   * otherwise the MOST RESTRICTIVE protection of its guest pages;
    ///   * minus WRITE while the page holds translated code or is watched.
    ///
    /// so a translated access that the hardware lets through is one the guest
    /// permits with no helper owed, and any other faults -- the JIT then sends
    /// that one site to its checked path.
    ///
    /// The views alias the same pages (`mach_vm_remap`, no copy). Every
    /// operation that replaces the host view's backing re-aliases the JIT view
    /// over the same range; `linear_jit_view_coherent` proves it.
    ///
    /// Requires a non-identity reservation of at most `kLinearJitViewMaxWindow`,
    /// hardware protection, both check maps, and no mapped region. Returns
    /// false, changing nothing, otherwise or when the host refuses.
    [[nodiscard]] bool enable_linear_jit_view();

    /// Base of the JIT view (`[base, base + linear_size())`), or nullptr.
    [[nodiscard]] u8* linear_jit_view_base() const noexcept {
        return m_linear_jit_view;
    }

    /// True when every VM entry of the host view in [lo, hi) has a JIT-view
    /// twin onto the same VM object at the same offset. Always true when the
    /// view is off. For tests and VITREO_JIT_VIEW_VERIFY.
    [[nodiscard]] bool linear_jit_view_coherent(GuestAddr lo, GuestAddr hi) const;

    /// One byte per 16 KiB host page of the reservation, indexed by
    /// `guest_addr >> kLinearHostPageShift`. A set bit means a direct
    /// JIT access to this host page must instead go through the checked
    /// helper. The reasons are independent and each has its own bit, because
    /// they are published by different owners at different times, but the
    /// emitted fast path only ever asks the one-byte question "is anything
    /// special about this page" — which is why they share a byte rather than
    /// costing a load each.
    ///
    /// Read lock-free by JIT-emitted code with a plain `LDRB`; every publisher
    /// orders itself fail-closed (set the bit before relaxing whatever the bit
    /// guards, clear it only after tightening).
    [[nodiscard]] const u8* linear_check_page_map() const noexcept {
        return m_linear_check_map;
    }
    [[nodiscard]] usize linear_check_page_count() const noexcept {
        return m_linear_check_count;
    }
    /// True when the check map ENDS at the last address this task can map:
    /// map[i] for any i >= linear_check_page_count() is an unmappable address,
    /// so an unmasked `LDRB [map, EA >> kLinearHostPageShift]` faults for
    /// every EA at or above the reservation -- including an EA whose top byte
    /// the hardware would otherwise ignore (TBI). The JIT's range-by-fault
    /// guard relies on exactly this; it was proven at allocation time by
    /// failing to allocate the page above the map.
    [[nodiscard]] bool linear_check_map_ends_at_va_limit() const noexcept {
        return m_linear_check_map_at_va_limit;
    }

    /// Companion to linear_check_page_map(), same indexing: for a MIXED host
    /// page, which of its four 4 KiB guest pages the guest may read (bits
    /// 0-3, bit k for guest page k) and write (bits 4-7). Lets a JIT cold
    /// stub decide a mixed-page access itself -- the bits are the region
    /// table's, so the decision is exact -- and fall back to the checked
    /// helper only when the access is really denied or the page has another
    /// reason (translated code, write watch) to need it. A host page that is
    /// not mixed never sends an access here, so its byte is meaningless and
    /// is left as it was.
    ///
    /// Published by linear_reconcile_host_pages under m_regions_mutex in the
    /// same phase that sets the mixed bit, before any hardware protection is
    /// relaxed. Read lock-free by emitted code with a plain LDRB.
    [[nodiscard]] const u8* linear_subpage_map() const noexcept {
        return m_linear_subpage_map;
    }

    /// Host pointer for @p addr under whichever linear model is active.
    /// Identity mode cannot use `m_linear_base + addr`: pointer arithmetic on
    /// a null pointer is undefined even when the offset makes the result
    /// correct, so the address is materialised directly.
    [[nodiscard]] u8* linear_host(GuestAddr addr) const noexcept {
        return m_linear_identity
                   ? reinterpret_cast<u8*>(static_cast<uintptr_t>(addr))
                   : m_linear_base + addr;
    }

    // -------------------------------------------------------------------------
    // Reserved holes (identity mapping only)
    // -------------------------------------------------------------------------

    /// Mark [lo, hi) as permanently unavailable to the guest.
    ///
    /// Under identity mapping the guest competes with the host process for
    /// address space, and some occupants cannot be moved — notably the main
    /// thread stack, which macOS places low once __PAGEZERO is shrunk and
    /// which `-Wl,-stack_addr` cannot relocate on a modern executable.
    /// Recording the holes lets map_region() refuse honestly instead of
    /// handing back memory the host already owns.
    void add_reserved_hole(GuestAddr lo, GuestAddr hi);

    /// True when [addr, addr+size) intersects any reserved hole. Callers that
    /// can relocate (the PE loader, choosing an image base) should consult
    /// this and pick elsewhere rather than treating a refusal as fatal.
    [[nodiscard]] bool overlaps_reserved_hole(GuestAddr addr,
                                              usize size) const noexcept;

    /// Lowest address >= @p addr where @p size bytes clear every reserved
    /// hole. Does not consider existing regions — it answers "where is this
    /// not structurally forbidden", not "where is this free".
    [[nodiscard]] GuestAddr next_address_clear_of_holes(
        GuestAddr addr, usize size) const noexcept;

    /// Walk the host task's own VM map below 4 GiB and record every occupied
    /// range as a reserved hole.
    ///
    /// Under identity mapping the guest cannot be placed anywhere the host
    /// process already lives, and the occupants below 4 GiB are not knowable
    /// at compile time: dyld's placement, the main thread stack (immovable —
    /// `-Wl,-stack_addr` is rejected on modern executables), and whatever the
    /// dynamic loader mapped before main(). Asking the kernel is the only
    /// honest answer.
    ///
    /// The scan combines two sources, because neither alone is complete: the
    /// task's VM map for real occupants, and the main executable's Mach-O
    /// header for `__PAGEZERO` — which is NOT a VM map entry and which the
    /// region walk silently skips (see the implementation).
    ///
    /// MUST be called before any guest region is mapped: it cannot distinguish
    /// a host occupant from a guest region Vitreo mapped itself, so a late
    /// call would wall off the guest's own memory. Returns 0 and records
    /// nothing if identity mapping is not active or a region already exists.
    ///
    /// @return Number of occupied ranges recorded (before coalescing).
    usize capture_host_occupancy_below_4gb();

    /// Claim every range below 4 GiB that is not a reserved hole, so the guest
    /// owns it before anything else in the process can take it.
    ///
    /// Identity mapping has no reservation to commit inside — the guest
    /// address IS the host address — so without this step map_region() has
    /// nothing to mprotect and every guest mapping fails. It is the startup
    /// half of enable_identity_mapping(), and must follow
    /// capture_host_occupancy_below_4gb() so it does not try to claim ranges
    /// the host already holds.
    ///
    /// Any range that cannot be claimed is recorded as a further reserved
    /// hole rather than silently ignored.
    ///
    /// @return Bytes successfully reserved.
    usize reserve_identity_space_below_4gb();

    /// Reserved holes, lowest first, for diagnostics. Copies under the lock.
    [[nodiscard]] std::vector<std::pair<GuestAddr, GuestAddr>>
    reserved_holes() const;
    
    // -------------------------------------------------------------------------
    // Region Management
    // -------------------------------------------------------------------------
    
    /// Map a region at the specified guest address
    /// @param guest_addr Starting guest address (must be page-aligned)
    /// @param size Size in bytes
    /// @param prot Memory protection
    /// @param committed Whether the range owns committed backing. Pass false
    ///        only for an address-space reservation.
    /// @return Success or error
    [[nodiscard]] VoidResult map_region(
        GuestAddr guest_addr, usize size, MemoryProtection prot,
        bool committed = true);

    /// First-fit placement: find the lowest free gap at or above @p min_start
    /// and map it. The search and the map are separate lock scopes, so
    /// another thread can take the same gap in between (every first-fit
    /// caller converges on the same lowest address); that race is retried
    /// against the updated table instead of reported as out-of-memory.
    /// @return The mapped base, or 0 with @p error set when nothing fits.
    [[nodiscard]] GuestAddr map_first_fit(GuestAddr min_start, usize size,
                                          MemoryProtection prot,
                                          bool committed = true,
                                          usize alignment = 0,
                                          Error* error = nullptr);

    /// Map a region at @p guest_addr BACKED by an externally-owned host buffer
    /// (e.g. shared memory from mmap(MAP_SHARED)). GuestMemory does NOT free
    /// @p host_ptr — the caller owns its lifetime. Contents are NOT zeroed
    /// (shared memory may already hold a peer's data). Used for cross-process
    /// shared-memory views (MapViewOfFile of a named section).
    [[nodiscard]] VoidResult map_region_backed(GuestAddr guest_addr, usize size,
                                               void* host_ptr, MemoryProtection prot);

    /// Replace the host backing of [@p guest_addr, +@p size) with a
    /// MAP_PRIVATE mapping of @p fd at @p file_offset, so pages the guest never
    /// writes stay clean file pages shared with every other mapper of the file.
    /// The range must lie inside ONE committed region that has not been written
    /// yet (its current contents are discarded), and @p guest_addr, @p size and
    /// @p file_offset must be host-page aligned. Only the linear reservation
    /// (non-identity) supports this. Returns false, leaving the range as fresh
    /// zero-filled memory, when any precondition or the host mmap fails.
    [[nodiscard]] bool back_region_with_file(GuestAddr guest_addr, usize size,
                                             int fd, u64 file_offset);

    /// Unmap a region at the specified guest address
    /// @param guest_addr Starting guest address
    /// @param size Size to unmap
    /// @return Success or error
    [[nodiscard]] VoidResult unmap_region(GuestAddr guest_addr, usize size);
    
    /// Change protection of a region
    /// @param guest_addr Starting guest address
    /// @param size Size of region
    /// @param prot New protection
    /// @return Success or error
    [[nodiscard]] VoidResult protect_region(GuestAddr guest_addr, usize size, MemoryProtection prot);

    /// Mark an already mapped range as physically committed and apply its
    /// guest-visible protection. Contents are not initialized; the allocation
    /// owner must zero newly committed pages before exposing them.
    [[nodiscard]] VoidResult commit_region(
        GuestAddr guest_addr, usize size, MemoryProtection prot);

    /// Return an already mapped range to uncommitted backing state and
    /// PAGE_NOACCESS. The caller must erase committed contents first when its
    /// contract requires zero-filled recommit.
    [[nodiscard]] VoidResult decommit_region(
        GuestAddr guest_addr, usize size);
    
    // -------------------------------------------------------------------------
    // Address Translation
    // -------------------------------------------------------------------------
    
    /// Translate guest address to host pointer
    /// @param guest_addr Guest virtual address
    /// @return Host pointer or error
    [[nodiscard]] VResult<Byte*> translate(GuestAddr guest_addr);
    
    /// Translate guest address to const host pointer
    [[nodiscard]] VResult<const Byte*> translate(GuestAddr guest_addr) const;
    
    /// Check if a guest address range is mapped
    [[nodiscard]] bool is_mapped(GuestAddr guest_addr, usize size = 1) const;
    
    /// Check if a guest address range has specific protection
    [[nodiscard]] bool has_protection(GuestAddr guest_addr, usize size, MemoryProtection prot) const;

    /// Hold every mapped region's protection stable for the guard's lifetime.
    ///
    /// `protect_region` takes this lock exclusively across its host mprotect,
    /// so nothing can re-protect a page under a holder of this guard. It
    /// exists for the runtime's own in-place writes into guest *code*
    /// (native64's segment-override translation): those open the host pages
    /// for writing themselves, and a guest VirtualProtect landing between the
    /// open and a store drops the page back to read-only. macOS reports that
    /// as a host SIGBUS -- a protection failure, not an unmapped address --
    /// which kills the emulator instead of faulting the guest.
    ///
    /// Do NOT call other GuestMemory methods while holding it: they take the
    /// same shared_mutex, and a shared_mutex is not recursive.
    using ProtectionPin = std::shared_lock<HostRwLock>;
    [[nodiscard]] ProtectionPin pin_protection() const {
        return ProtectionPin(m_regions_mutex);
    }

    /// Query the mapped region containing a guest address.
    /// @return true if the address is mapped, false otherwise.
    [[nodiscard]] bool query_region(GuestAddr guest_addr,
                                    GuestAddr& base,
                                    usize& requested_size,
                                    MemoryProtection& prot) const;

    /// Query the whole mapping (one map_region call: a PE image, a stack, a
    /// TEB, ...) containing a guest address, ignoring protection runs. This is
    /// the "reserved region" VirtualProtect's range must stay within.
    [[nodiscard]] bool query_mapping(GuestAddr guest_addr,
                                     GuestAddr& base,
                                     usize& requested_size) const;
    
    // -------------------------------------------------------------------------
    // Read Operations (Little-endian, x86 convention)
    // -------------------------------------------------------------------------
    
    /// Read a single byte
    [[nodiscard]] ReadResult<u8> read_u8(GuestAddr addr) const;
    
    /// Read a 16-bit value
    [[nodiscard]] ReadResult<u16> read_u16(GuestAddr addr) const;
    
    /// Read a 32-bit value
    [[nodiscard]] ReadResult<u32> read_u32(GuestAddr addr) const;
    
    /// Read a 64-bit value
    [[nodiscard]] ReadResult<u64> read_u64(GuestAddr addr) const;
    
    /// Read arbitrary bytes into buffer
    [[nodiscard]] VoidResult read_bytes(GuestAddr addr, Byte* buffer, usize size) const;

    /// Fast host pointer for a contiguous range (no Result overhead).
    /// Returns nullptr if not fully mapped.
    [[nodiscard]] const Byte* host_ptr(GuestAddr addr, usize size) const;
    /// Instruction-fetch forms of host_ptr / read_u8: also accept an
    /// execute-only (PAGE_EXECUTE) range, which data reads refuse.
    [[nodiscard]] const Byte* fetch_ptr(GuestAddr addr, usize size) const;
    [[nodiscard]] ReadResult<u8> fetch_u8(GuestAddr addr) const;
    [[nodiscard]] Byte* host_ptr_mut(GuestAddr addr, usize size);

    /// Host pointer for a guest STORE, or nullptr when the guest is not
    /// allowed to write the range.
    ///
    /// `host_ptr_mut` deliberately does not consult protection — it exists for
    /// the runtime's own writes into the guest image (the GS/FS segment
    /// rewrite, CPUID patching), which are not the guest storing. Every
    /// GUEST-semantics writer must use this instead. The LOCK/atomic and
    /// string fast paths take a raw host pointer and write through it with
    /// `std::atomic_ref` or `memcpy`; on a read-only guest page whose host
    /// page is mapped writable that is a silent protection bypass, and on one
    /// mapped read-only it is a host SIGBUS — a guest-triggerable crash of the
    /// emulator.
    ///
    /// Returning nullptr is what makes the fix safe by construction: every one
    /// of those call sites already has a null fallback onto the checked
    /// `read_*`/`write_*` path, which refuses and raises an access violation.
    [[nodiscard]] Byte* host_ptr_store(GuestAddr addr, usize size);

    /// True when the guest may write every byte of [addr, addr+size).
    ///
    /// For instructions that architecturally write their destination even when
    /// they appear not to modify it. `CMPXCHG` is the case that motivated it:
    /// Intel SDM Vol. 2 says the destination is always written back, so a
    /// CMPXCHG against a read-only page faults on real hardware whether or not
    /// the comparison succeeded. Asking permission is the whole fix — the
    /// write-back itself is not performed, so write-watch and cache state are
    /// unchanged.
    [[nodiscard]] bool guest_can_write(GuestAddr addr, usize size);

    /// Discard the contents of a writable range without recording a guest
    /// write, invalidating code, or changing commitment/protection metadata.
    /// Contents are undefined after success, matching DiscardVirtualMemory.
    [[nodiscard]] VoidResult discard_region_contents(
        GuestAddr addr, usize size);

    /// VirtualAlloc(MEM_RESET) on a private range: offer the writable host
    /// pages wholly inside [addr, addr+size) back to the host without writing
    /// them. Other pages keep their bytes (contents are undefined anyway).
    void reset_region_contents(GuestAddr addr, usize size);

    /// VirtualAlloc(MEM_RESET_UNDO): take back pages offered by
    /// reset_region_contents. True only when no page that held data at reset
    /// time was reclaimed by the host, so success never claims lost bytes.
    [[nodiscard]] bool undo_reset_region_contents(GuestAddr addr, usize size);

    /// Offer mapped-view pages back to the host without writing through the
    /// view or changing guest mapping/protection metadata.
    [[nodiscard]] VoidResult discard_mapped_region_contents(
        GuestAddr addr, usize size);
    
    // -------------------------------------------------------------------------
    // Write Operations (Little-endian, x86 convention)
    // -------------------------------------------------------------------------
    
    /// Write a single byte
    [[nodiscard]] WriteResult write_u8(GuestAddr addr, u8 value);
    
    /// Write a 16-bit value
    [[nodiscard]] WriteResult write_u16(GuestAddr addr, u16 value);
    
    /// Write a 32-bit value
    [[nodiscard]] WriteResult write_u32(GuestAddr addr, u32 value);
    
    /// Write a 64-bit value
    [[nodiscard]] WriteResult write_u64(GuestAddr addr, u64 value);
    
    /// Write arbitrary bytes from buffer
    [[nodiscard]] WriteResult write_bytes(GuestAddr addr, const Byte* buffer, usize size);
    
    // -------------------------------------------------------------------------
    // Debug / Inspection
    // -------------------------------------------------------------------------
    
    /// Get total mapped memory size
    [[nodiscard]] usize total_mapped_size() const noexcept { return total_size_; }
    
    /// Get number of mapped regions
    [[nodiscard]] usize region_count() const noexcept { return regions_.size(); }

    /// Snapshot of every mapped region as [base, end). Diagnostics only.
    ///
    /// regions_ is the real occupancy: images, stacks, TEB, thunks, mapped
    /// files and VirtualAlloc reservations alike. A free-space report built
    /// from any one caller's own bookkeeping (e.g. VirtualAlloc's allocation
    /// list) will claim runs that do not exist.
    [[nodiscard]] std::vector<std::pair<GuestAddr, GuestAddr>>
    region_extents() const;

    /// Extents whose guest protection includes Execute. Used to bound a code
    /// scan: rewriting a byte pattern found in data would corrupt the guest,
    /// so a scanner must never look outside these.
    [[nodiscard]] std::vector<std::pair<GuestAddr, GuestAddr>>
    executable_extents() const;
    
    /// Check if any mapped region's host memory overlaps with [start, end)
    bool check_host_overlap(uintptr_t start, uintptr_t end) const;
    /// Find a free gap in the address space starting at or above @p min_start
    /// that can fit @p size bytes (page-aligned).
    /// @param alignment Base alignment (a power of two; values below the guest
    ///        page size mean page alignment). Every candidate the walk tries is
    ///        aligned, so a fragmented range is crossed in one ordered pass
    ///        instead of by caller-side retries that can run out.
    /// @return  Base address with that alignment, or 0 if no gap found.
    [[nodiscard]] GuestAddr find_free_gap(GuestAddr min_start, usize size,
                                          usize alignment = 0) const;

    /// Cap guest VA search for find_free_gap (inclusive). Matches
    /// GetSystemInfo lpMaximumApplicationAddress for the process: 0x7FFEFFFF
    /// for 32-bit non-LARGEADDRESSAWARE images. Zero keeps the legacy 4 GiB
    /// search ceiling used by unit tests.
    void set_maximum_user_address(GuestAddr maximum) noexcept {
        m_maximum_user_address = maximum;
        forget_fit_floors();
    }
    [[nodiscard]] GuestAddr maximum_user_address() const noexcept {
        return m_maximum_user_address;
    }

    /// Find the base address of the next mapped region at or above @p addr.
    /// Returns 0 if no region exists above @p addr.
    [[nodiscard]] GuestAddr next_region_base(GuestAddr addr) const;

    /// Information about a mapped region for JIT inline memory access.
    struct RegionInfo {
        GuestAddr base{0};
        GuestAddr end{0};     // exclusive
        u8* host_ptr{nullptr};
    };

    /// Get the region containing the given guest address (for JIT region caching).
    /// Returns a zero-filled RegionInfo if the address is unmapped.
    [[nodiscard]] RegionInfo get_region_info(GuestAddr addr) const;

    /// Fast variant that checks the thread-local TLB first (lock-free),
    /// falling back to get_region_info() only on TLB miss.
    /// Ideal after a read/write that already populated the TLB.
    [[nodiscard]] RegionInfo get_region_info_cached(GuestAddr addr) const;

    /// Combined access result: host pointer + region info from a single
    /// TLB probe.  Used by JIT helpers so they don't have to call both
    /// resolve_addr() and get_region_info_cached() (each of which costs
    /// a thread_local access via _tlv_get_addr on Darwin).
    struct AccessInfo {
        u8* host_ptr;            // pointer to the requested byte; nullptr on miss/failure
        GuestAddr region_base;   // base of the containing region (0 on miss)
        GuestAddr region_end;    // exclusive end of the containing region (0 on miss)
        u8* region_host_base;    // host-base pointer for the containing region
        bool readable;
        bool writable;
        bool executable;
    };

    /// Single-TLB-probe accessor.  Fills AccessInfo with both the host
    /// pointer for [addr, addr+size) and the bounds of the containing
    /// region.  On TLB miss, falls back to a locked region lookup and
    /// fills the TLB.  Both fields are set together so callers never
    /// need a second lookup to update region caches.
    [[nodiscard]] AccessInfo probe_access(GuestAddr addr, usize size) const;

    /// Validate that [addr, addr+size) is entirely mapped and writable.
    ///
    /// Performs no overflow-prone `addr + size` arithmetic and rejects
    /// addresses beyond the guest architecture limit, so callers can check a
    /// range before mutating any state. Returns false for size == 0 ranges
    /// that would wrap, unmapped or read-only spans.
    [[nodiscard]] bool is_range_writable(GuestAddr addr, usize size,
                                         bool is_64bit) const;

    /// Return true when the write invalidated compiled code (or is the
    /// lost-history sentinel). False means the mapping is executable but no
    /// cached block overlaps — do not publish a generation/log event.
    using ExecutableWriteObserver =
        std::function<bool(GuestAddr, usize, u64)>;

    /// Install the executable-write observer for this memory on the current thread.
    void set_thread_executable_write_observer(ExecutableWriteObserver observer);

    /// Remove the current thread's executable-write observer for this memory.
    void clear_thread_executable_write_observer();

    /// Process-wide teardown hook, invoked with `this` at the start of
    /// ~GuestMemory. A subsystem that retains code pages of this address
    /// space by raw pointer beyond any one thread's lifetime (the JIT's shared
    /// compiled-block table) drops them here, before the pages go away.
    /// Production builds exactly one GuestMemory per process; tests build
    /// many, which is what makes the hook necessary. One registrant.
    using TeardownHook = void (*)(GuestMemory*);
    static void set_teardown_hook(TeardownHook hook) noexcept;
    /// Register a further teardown hook, run after the one above. For other
    /// process-wide subsystems that write into an address space from a host
    /// thread of their own (the KUSER_SHARED_DATA clock source). Idempotent;
    /// returns false only when every slot is taken.
    static bool add_teardown_hook(TeardownHook hook) noexcept;

    /// Notify JIT threads after a write to executable code. The current
    /// thread's observer runs first; a false return (no compiled overlap)
    /// does not publish a generation. Other threads observe published
    /// events at dispatch safe points.
    void notify_executable_write(GuestAddr addr, usize size);

    /// Register that stores to [alias_base, alias_base + size) land in the
    /// same host pages as [target_base, target_base + size). Compiled code is
    /// cached by *guest* address, so a store through a writable alias of an
    /// executable mapping changes code the JIT still holds translations for at
    /// the target address. Writes into a registered alias are therefore
    /// reported as executable writes against the aliased address.
    void add_code_alias(GuestAddr alias_base, GuestAddr target_base,
                        usize size);

    /// Target address a registered code alias maps `addr` onto, or 0 when the
    /// address is not inside any alias.
    [[nodiscard]] GuestAddr code_alias_target(GuestAddr addr,
                                              usize size) const noexcept;

    /// True when any registered code alias overlaps [addr, addr + size).
    [[nodiscard]] bool overlaps_code_alias(GuestAddr addr,
                                           usize size) const noexcept {
        return m_has_code_aliases.load(std::memory_order_relaxed) &&
               code_alias_target(addr, size) != 0;
    }

    /// Register/unregister a page that is represented by at least one block in
    /// a JIT cache for this address space. The returned byte map is shared by
    /// every JIT thread using this GuestMemory, so a direct linear store can
    /// see code owned by another thread before deciding to bypass the checked
    /// write helper.
    void retain_jit_code_page(GuestAddr page);
    void release_jit_code_page(GuestAddr page);
private:
    struct JitCodePageShard;
    /// Retain/release exactly one page; callers hold `shard.lock` exclusively,
    /// where `shard` is jit_code_page_shard(page).
    void retain_jit_code_page_locked(JitCodePageShard& shard, GuestAddr page);
    void release_jit_code_page_locked(JitCodePageShard& shard, GuestAddr page);
    void retain_one_jit_code_page(GuestAddr page);
    void release_one_jit_code_page(GuestAddr page);
    /// Visit the pages that alias `page` through a registered code alias.
    void for_each_code_alias_mirror(
        GuestAddr page, const std::function<void(GuestAddr)>& fn) const;
public:
    [[nodiscard]] u8* jit_code_page_map() noexcept {
        return m_jit_code_page_map.get();
    }
    [[nodiscard]] u32* jit_code_chunk_pages() noexcept {
        return m_jit_code_chunk_pages.get();
    }
    /// Pages of compiled code in the 2 MiB chunk that contains @p addr.
    [[nodiscard]] u32 jit_code_chunk_refs(GuestAddr addr) const noexcept;
    [[nodiscard]] u64 jit_code_generation() const noexcept {
        return m_jit_code_generation.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::atomic<u64>* jit_code_generation_slot() const noexcept {
        return &m_jit_code_generation;
    }
    /// Conservative [lo, hi) of every page currently marked in the process-wide
    /// JIT code map. Linear stores outside this range skip the page-map probe.
    /// The span only grows (same contract as BlockCache). Empty: hi == 0.
    [[nodiscard]] u32 jit_code_span_low() const noexcept {
        return m_jit_code_span_lo.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 jit_code_span_high() const noexcept {
        return m_jit_code_span_hi.load(std::memory_order_acquire);
    }
    /// Full-address conservative span for x64 inline-store SMC rejection.
    /// The low-32 span above remains the envelope for the compact low-4-GiB
    /// page map. An x64 store inside this span at or above 4 GiB must use the
    /// checked helper, which consults the exact sparse high-page index.
    [[nodiscard]] GuestAddr jit_code_span64_low() const noexcept {
        return m_jit_code_span64_lo.load(std::memory_order_acquire);
    }
    [[nodiscard]] GuestAddr jit_code_span64_high() const noexcept {
        return m_jit_code_span64_hi.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::atomic<GuestAddr>*
    jit_code_span64_low_atomic() const noexcept {
        return &m_jit_code_span64_lo;
    }
    [[nodiscard]] const std::atomic<GuestAddr>*
    jit_code_span64_high_atomic() const noexcept {
        return &m_jit_code_span64_hi;
    }
    [[nodiscard]] bool jit_code_pages_overlap(GuestAddr addr,
                                               usize size) const noexcept;
    /// Identity of this address space's JIT code map, unique for the lifetime
    /// of the process. A raw `this` would be reused by a later GuestMemory at
    /// the same address and could match a stale memo entry.
    [[nodiscard]] u64 jit_code_instance_id() const noexcept {
        return m_jit_code_instance_id;
    }

private:
    /// One writable guest window over the host pages of an executable mapping.
    struct CodeAlias {
        GuestAddr alias_base;
        GuestAddr target_base;
        usize size;
    };
    /// Snapshot-published so the write path reads it without a lock; aliases
    /// are established a handful of times, during unpacking.
    std::shared_ptr<const std::vector<CodeAlias>> m_code_aliases;
    std::atomic<bool> m_has_code_aliases{false};
    mutable std::mutex m_code_alias_mutex;

public:

    /// One process-wide executable write / FlushInstructionCache event.
    /// `addr == 0 && size == 0` is the full-process flush sentinel.
    /// `generation` is the latest write folded into this slot; `first_generation`
    /// is when the slot was created, so coalesced updates cannot look like a
    /// dropped history window to a caught-up thread.
    struct ExecutableWriteEvent {
        u64 generation = 0;
        u64 first_generation = 0;
        GuestAddr addr = 0;
        usize size = 0;
        /// Every write folded into this event came from a thread whose JIT
        /// observer already purged the shared compiled-block records for it.
        bool writer_purged = false;
    };

    /// Events with `generation > after_generation`, for per-thread JIT sync.
    /// If the caller lagged past compacted history, returns a single full-flush
    /// sentinel so caches cannot keep stale code.
    [[nodiscard]] std::vector<ExecutableWriteEvent>
    collect_executable_writes_after(u64 after_generation) const;

    /// Process-wide executable-code generation. Unlike the mapping generation,
    /// this changes for byte updates and FlushInstructionCache as well as
    /// map/protect operations.
    /// Loaded PE images whose code is unchanged since load (the JIT
    /// translation store's notion of image code).
    [[nodiscard]] GuestImageRegistry& images() noexcept { return m_images; }
    [[nodiscard]] const GuestImageRegistry& images() const noexcept { return m_images; }

    [[nodiscard]] u64 executable_generation() const noexcept {
        return m_executable_generation.load(std::memory_order_acquire);
    }

    /// Current generation counter (for JIT cache invalidation).
    /// Bumped on backing/lifetime changes such as unmap/commit/decommit.
    /// Non-overlapping insertions and software-only protection changes use
    /// range shootdowns, preserving unrelated TLB/JIT translations.
    [[nodiscard]] u64 generation() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }

    /// The software TLB's epoch: a change discards every thread's whole TLB
    /// at its next host-side guest access. Only a change that can leave a
    /// cached host pointer dangling (a destroyed backing) bumps it; anything
    /// narrower publishes a range shootdown.
    [[nodiscard]] u64 translation_generation() const noexcept {
        return m_translation_generation.load(std::memory_order_acquire);
    }

    /// Protect-range shootdown sequence. VirtualProtect publishes a ranged
    /// invalidation instead of bumping generation(); translators drain this
    /// and drop only overlapping cached spans.
    static constexpr usize kProtectShootdownLog = 256;

    struct ProtectShootdown {
        // Scratch records are filled completely by
        // copy_protect_shootdowns_after() before callers inspect them. Keep
        // default construction trivial: JIT and TLB drains allocate one
        // ring's worth on the stack, and member initializers would clear all
        // 6 KiB even when there are no protection changes to copy.
        GuestAddr addr;
        usize size;
        MemoryProtection protection;
    };
    static_assert(std::is_trivially_default_constructible_v<ProtectShootdown>);

    [[nodiscard]] u64 protect_sequence() const noexcept {
        return m_protect_seq.load(std::memory_order_acquire);
    }

    /// Stable address used by native JIT blocks to detect a protection change
    /// while they remain in a linked/native loop without dispatcher re-entry.
    [[nodiscard]] const std::atomic<u64>* protect_sequence_slot() const noexcept {
        return &m_protect_seq;
    }

    /// Copy protect events after `seen`. `*overflow` means the ring wrapped
    /// past `seen` and every cached translation must be dropped. Returns the
    /// number of events written to `out`.
    usize copy_protect_shootdowns_after(
        u64 seen, ProtectShootdown* out, usize cap, bool* overflow) const;

    /// Hold linear mapping/protection state stable while a proof-bearing JIT
    /// block executes. Returns false when a writer has already announced
    /// revocation; callers must deopt without entering native code.
    [[nodiscard]] bool try_enter_linear_proof_lease() noexcept;
    void exit_linear_proof_lease() noexcept;

private:
    GuestImageRegistry m_images;

    class LinearProofWriteGuard {
    public:
        explicit LinearProofWriteGuard(GuestMemory& memory) noexcept;
        ~LinearProofWriteGuard();
        LinearProofWriteGuard(const LinearProofWriteGuard&) = delete;
        LinearProofWriteGuard& operator=(const LinearProofWriteGuard&) = delete;

    private:
        GuestMemory* m_memory;
    };

    void begin_linear_proof_write() noexcept;
    void end_linear_proof_write() noexcept;

    /// A mapped memory region with its guest base address
    struct MappedRegion {
        GuestAddr base;
        usize requested_size;  // Guest-facing size (may be smaller than region.size())
        MemoryRegion region;
        /// Guest-visible protection runs keyed by byte offset. Host protection
        /// may be coarser (16 KiB on Apple Silicon), so access checks and
        /// VirtualQuery use these runs as the source of truth.
        std::map<usize, MemoryProtection> protections;
        /// Host-backing ownership runs, separate from accessibility:
        /// committed PAGE_NOACCESS pages retain contents, while reservations
        /// are uncommitted even though both use MemoryProtection::None.
        std::map<usize, bool> committed;
    };

    struct ProtectionSpan {
        usize begin;
        usize end;
        MemoryProtection protection;
    };

    [[nodiscard]] static ProtectionSpan protection_span(
        const MappedRegion& mapped, usize offset);
    static void set_protection_range(MappedRegion& mapped, usize offset,
                                     usize size, MemoryProtection protection);
    static void set_committed_range(MappedRegion& mapped, usize offset,
                                    usize size, bool committed);
    [[nodiscard]] static bool has_committed_span(
        const MappedRegion& mapped);
    
    /// Find the region containing a guest address
    [[nodiscard]] MappedRegion* find_region(GuestAddr addr);
    [[nodiscard]] const MappedRegion* find_region(GuestAddr addr) const;
    
    /// Backing generation used by JIT proofs and retired-storage lifetime.
    /// Commit/decommit and hardware-protection reconciliation retain their
    /// existing invalidations independently of software translation caching.
    std::atomic<u64> m_generation{0};
    // The software TLB also drains range shootdowns. Unlike JIT backing
    // proofs, its unrelated translations survive commit/decommit/protect.
    // Storage/lifetime changes and alias changes still flush it globally.
    std::atomic<u64> m_translation_generation{0};

    struct ProtectShootdownSlot {
        std::atomic<u64> seq{0};
        std::atomic<u64> addr{0};
        std::atomic<u64> size{0};
        std::atomic<u32> protection{0};
    };
    ProtectShootdownSlot m_protect_log[kProtectShootdownLog];
    std::atomic<u64> m_protect_seq{0};
    // Proof leases: native proof blocks whose entry validation must remain
    // true until they return, counted per reader shard, and one writer flag
    // announcing a mapping/protection change. Every guest thread takes a
    // lease on every dispatcher entry of such a block, so one shared counter
    // makes every guest thread CAS the same cache line (3.7% of busy CPU
    // across its task workers). A thread always uses the shard
    // its pthread_self() hashes to, so its enter and exit meet; threads
    // sharing a shard only share a line. Readers: increment own shard, then
    // check the flag.
    // Writer: raise the flag, then wait for every shard to drain. Both sides
    // are seq_cst, so at least one sees the other (Dekker).
    struct alignas(128) LinearProofLeaseShard {
        std::atomic<u64> readers{0};
    };
    static constexpr usize kLinearProofLeaseShards = 64;
    LinearProofLeaseShard m_linear_proof_lease_shards[kLinearProofLeaseShards];
    alignas(128) std::atomic<u32> m_linear_proof_writer{0};
    [[nodiscard]] LinearProofLeaseShard& linear_proof_lease_shard() noexcept;
    static constexpr usize kProtectShootdownMask = kProtectShootdownLog - 1;
    static_assert((kProtectShootdownLog & kProtectShootdownMask) == 0,
                  "protect shootdown log capacity must be a power of two");

    void publish_protect_shootdown(GuestAddr addr, usize size,
                                  MemoryProtection protection);

    /// Incremented for every executable-write notification so per-thread JIT
    /// block caches cannot retain code invalidated by another guest thread.
    std::atomic<u64> m_executable_generation{0};

    static constexpr usize kJitCodePageCount = usize{1} << 20;
    static constexpr usize kJitCodeChunkPages = 512;
    static constexpr usize kJitCodeChunkCount =
        kJitCodePageCount / kJitCodeChunkPages;
    std::unique_ptr<u32[]> m_jit_code_page_refs =
        std::make_unique<u32[]>(kJitCodePageCount);
    std::unique_ptr<u8[]> m_jit_code_page_map =
        std::make_unique<u8[]>(kJitCodePageCount);
    std::unique_ptr<u32[]> m_jit_code_chunk_pages =
        std::make_unique<u32[]>(kJitCodeChunkCount);
    std::atomic<u64> m_jit_code_generation{0};
    std::atomic<u32> m_jit_code_span_lo{0xFFFFFFFFu};
    std::atomic<u32> m_jit_code_span_hi{0};
    std::atomic<GuestAddr> m_jit_code_span64_lo{~GuestAddr{0}};
    std::atomic<GuestAddr> m_jit_code_span64_hi{0};

    /// The code-page reference counts are sharded by 16 KiB HOST page, not
    /// guarded by one lock: every compile publishes by retaining its pages,
    /// and a single exclusive lock here serialized every compiling thread
    /// again (compile throughput fell by half from 4+4 to 32+32 threads).
    ///
    /// Keying by host page is what keeps the protection exact. All four 4 KiB
    /// guest pages of a host page share one kLinearCheckHoldsCode bit and one
    /// JIT-view protection, so the 0 -> 1 and 1 -> 0 transitions of that
    /// host page's count, and the bit/view updates they drive, are made by one
    /// shard holder at a time. A guest page's own count
    /// (m_jit_code_page_refs or high_refs) is likewise only touched under its
    /// shard's lock. State shared across shards is only ever widened or
    /// counted atomically: the spans (CAS-widened, never narrowed), the chunk
    /// counters and the generation.
    static constexpr usize kJitCodePageShards = 64;
    static usize jit_code_page_shard_index(GuestAddr page) noexcept {
        const u64 host_page = static_cast<u64>(page) >> kLinearHostPageShift;
        return static_cast<usize>((host_page * 0x9E3779B97F4A7C15ull) >> 58) &
               (kJitCodePageShards - 1);
    }
    struct alignas(128) JitCodePageShard {
        mutable SpinRwLock lock;
        /// Retained guest pages at or above 4 GiB. Ordered so overlap probes
        /// locate the first candidate page with lower_bound().
        std::map<GuestAddr, u32> high_refs;
        /// Host pages whose `kLinearCheckHoldsCode` bit is set, and how many
        /// 4 KiB guest code pages inside each one still hold a translation.
        /// A host page covers four guest pages, so the bit may not be cleared
        /// until the last of them is released.
        std::map<usize, u32> linear_host_refs;
    };
    std::unique_ptr<JitCodePageShard[]> m_jit_code_page_shards =
        std::make_unique<JitCodePageShard[]>(kJitCodePageShards);
    JitCodePageShard& jit_code_page_shard(GuestAddr page) const noexcept {
        return m_jit_code_page_shards[jit_code_page_shard_index(page)];
    }
    /// True when a retained page >= 4 GiB lies in [first, last] (page
    /// addresses, inclusive). Takes the owning shards' locks shared.
    [[nodiscard]] bool jit_high_code_in_range(GuestAddr first,
                                              GuestAddr last) const noexcept;

    /// Lock-free coarse filter over the >=4 GiB half of the address space.
    /// The low half is answered by m_jit_code_chunk_pages / m_jit_code_page_map
    /// without a lock. Without this filter, every x64 guest store, whose
    /// image and heaps sit above 4 GiB, takes the code-page lock and walks
    /// the high-page index. That lock is the dominant contention in a
    /// browser renderer profile.
    ///
    /// One counter per 2 MiB chunk, hashed into a fixed table, so distinct
    /// chunks may share a slot. A slot is incremented BEFORE the page becomes
    /// visible in the map and decremented AFTER it is erased, so a zero slot
    /// proves "no marked page in this chunk" while a non-zero slot only means
    /// "maybe" -- the lock-free reject is never wrong in the unsafe direction.
    static constexpr u32 kJitHighChunkSlots = 8192;
    static u32 jit_high_chunk_slot(GuestAddr page) noexcept {
        const u64 chunk = static_cast<u64>(page) >> 21;
        return static_cast<u32>((chunk ^ (chunk >> 13)) &
                                (kJitHighChunkSlots - 1));
    }
    std::unique_ptr<std::atomic<u32>[]> m_jit_high_chunk_counts =
        std::make_unique<std::atomic<u32>[]>(kJitHighChunkSlots);
    /// Process-unique id for the memo key in jit_code_pages_overlap().
    const u64 m_jit_code_instance_id = next_jit_code_instance_id();
    static u64 next_jit_code_instance_id() noexcept;

    /// The uncached page walk behind jit_code_pages_overlap().
    [[nodiscard]] bool jit_code_pages_overlap_scan(GuestAddr addr,
                                                   usize size) const noexcept;

    /// Bounded ring of executable writes for cross-thread JIT sync.
    /// Same-thread observers invalidate immediately; other threads drain this
    /// at dispatcher safe points. Readers coalesce adjacent same-page records.
    /// Serialize publication and snapshots, including slot reuse on wrap.
    /// The generation is published only after its complete record is visible.
    mutable std::mutex m_exec_write_log_mutex;
    struct ExecutableWriteSlot {
        std::atomic<u64> generation{0};
        std::atomic<u64> addr{0};
        std::atomic<u64> size{0};
        std::atomic<bool> writer_purged{false};
    };
    static constexpr usize kMaxExecWriteLog = 4096;
    static constexpr usize kExecWriteLogMask = kMaxExecWriteLog - 1;
    ExecutableWriteSlot m_exec_write_log[kMaxExecWriteLog];
    static_assert((kMaxExecWriteLog & kExecWriteLogMask) == 0,
                  "executable-write ring capacity must be a power of two");

    /// Bump the generation counter (called under exclusive lock)
    void invalidate_tlb(bool discard_translations = true) noexcept;

    /// Lock-free TLB fast path + locked fallback address resolution.
    [[nodiscard]] const u8* resolve_addr(GuestAddr addr, usize size, bool* writable = nullptr) const;
    [[nodiscard]] u8* resolve_addr_mut(GuestAddr addr, usize size,
                                       bool* writable = nullptr,
                                       bool* executable = nullptr);
    /// Single-pass readable check: TLB fast path then resolve_addr slow path.
    /// On nullptr return, *err is set to InvalidAddress (unmapped) or ProtectionFault.
    [[nodiscard]] const u8* resolve_for_read(GuestAddr addr, usize size, Error* err = nullptr) const;
    /// resolve_for_read that also accepts Execute without Read (fetch).
    [[nodiscard]] const u8* resolve_for_fetch(GuestAddr addr, usize size, Error* err = nullptr) const;
    [[nodiscard]] const u8* resolve_for_access(GuestAddr addr, usize size, Error* err,
                                               bool fetch) const;

    /// Protects regions_ from concurrent modification.
    /// Read operations use lock-free TLB for the fast path; shared lock
    /// only on TLB misses.  Exclusive lock for map / unmap / protect.
    // Reader-scalable (see host_rwlock.hpp): every TLB miss on every guest
    // thread takes this shared, and std::shared_mutex serialised those
    // readers on its internal mutex -- ~1000 blocked samples in 10 s of one
    // frame, with writers (VirtualAlloc/Protect) almost
    // absent.
    mutable HostRwLock m_regions_mutex;

    /// Host pages MEM_RESET offered while they held data (resident or
    /// compressed). Untouched pages are absent: they read as zero whether or
    /// not the host reclaimed anything, so MEM_RESET_UNDO need not check them.
    std::mutex m_reset_pages_mutex;
    std::unordered_set<uintptr_t> m_reset_host_pages;

    /// Deferred region destruction so lock-free TLB readers can safely
    /// finish host memory accesses before the backing allocation is freed.
    struct ZombieRegion { u64 gen; MemoryRegion mem; };
    std::vector<ZombieRegion> m_zombie_regions;
    void cleanup_zombies();

    /// All mapped regions, keyed by guest base address
    std::map<GuestAddr, MappedRegion> regions_;

    /// First-fit floors for find_free_gap. VirtualAlloc places bottom-up
    /// first-fit (Windows' rule), so every call walks every region above
    /// its start. Hundreds of thousands of regions put that
    /// walk at 17% of the main thread. For a (start, size, alignment) search the
    /// floor F records that no fitting base exists in [start, F). Mapping only
    /// removes free space, so floors stay valid; unmap_region lowers every
    /// floor to the freed address, and holes are only ever added. The result
    /// is exactly the address the full walk would return.
    struct FitKey {
        GuestAddr start;
        usize size;
        usize alignment;
        bool operator==(const FitKey&) const = default;
    };
    struct FitKeyHash {
        usize operator()(const FitKey& k) const noexcept {
            return std::hash<u64>{}(k.start ^ (static_cast<u64>(k.size) * 0x9E3779B97F4A7C15ull) ^
                                    (static_cast<u64>(k.alignment) << 7));
        }
    };
    mutable std::mutex m_fit_floor_mutex;
    mutable std::unordered_map<FitKey, GuestAddr, FitKeyHash> m_fit_floors;
    void forget_fit_floors() noexcept;
    void lower_fit_floors(GuestAddr freed) noexcept;

    /// Total mapped size (for statistics)
    usize total_size_ = 0;

    /// Inclusive user-mode VA ceiling for opportunistic gap search (0 = none).
    GuestAddr m_maximum_user_address{0};

    /// Linear (guest-base) reservation. When non-null, regions are backed at
    /// m_linear_base + guest_addr rather than scattered host allocations.
    /// See enable_linear_reservation(). nullptr = classic scattered model.
    u8*   m_linear_base = nullptr;
    usize m_linear_size = 0;        // core + extension, see linear_size()
    usize m_linear_pow2_size = 0;   // the core, see linear_power_of_two_size()
    /// The extension [core, m_linear_size) is reserved as a max-protection
    /// NONE placeholder, which the kernel enters in O(1); an ordinary
    /// PROT_NONE reservation of the same ~63 TiB costs ~70 ms to map and
    /// ~80 ms to unmap per process (page-table bookkeeping proportional to
    /// size, measured). A placeholder page cannot be made accessible, so each
    /// 1 GiB chunk is replaced by an ordinary reservation the first time a
    /// region is placed in it; one bit per chunk, guarded by m_regions_mutex.
    /// A guest access to an unmaterialized chunk faults like any reserved
    /// page of the window.
    std::vector<u64> m_linear_ext_materialized;

    /// Hardware enforcement of guest protection inside the reservation.
    /// See enable_linear_hardware_protection().
    bool  m_linear_hw_protect = false;
    /// One byte per 16 KiB host page of the reservation. Its own mmap so it is
    /// lazily faulted: the map for a 1 TiB reservation is 64 MiB of address
    /// space and only the touched pages of physical memory.
    ///
    /// Allocated with the reservation itself, not with hardware protection:
    /// `kLinearCheckHoldsCode` has to be published whether or not guest
    /// protection is hardware-enforced, because a store that lands on
    /// translated code must reach the invalidating helper either way.
    u8*   m_linear_check_map = nullptr;
    u8*   m_linear_subpage_map = nullptr;   // see linear_subpage_map()
    usize m_linear_check_count = 0;
    /// Bytes mapped for m_linear_check_map: count + kCheckMapPadBytes rounded
    /// by mmap, or exactly count when placed at the VA limit (no pad).
    usize m_linear_check_map_bytes = 0;
    bool  m_linear_check_map_at_va_limit = false;

    /// The JIT view (see enable_linear_jit_view()): m_linear_jit_view is the
    /// alias of guest address 0, inside a mapping of m_linear_size plus
    /// kLinearJitViewGuardBytes on each side.
    u8*   m_linear_jit_view = nullptr;
    /// Set when the host refused a re-alias or an mprotect: the JIT view may
    /// map stale pages or grant too much, so it is held at PROT_NONE for good
    /// and translated code takes the checked path everywhere. Guarded by
    /// m_jit_view_mutex.
    bool  m_linear_jit_view_poisoned = false;
    /// Serializes every JIT-view protection decision with the mprotect that
    /// applies it. A LEAF lock: linear_reconcile_host_pages holds it for its
    /// whole body (under m_regions_mutex), and the code-page and write-watch
    /// publishers take it under their own mutexes, never the reverse.
    mutable std::mutex m_jit_view_mutex;

    /// Host pages whose `kLinearCheckWatched` bit is set, and how many live
    /// MEM_WRITE_WATCH allocations overlap each one. Guarded by its own mutex:
    /// the publishers are in the kernel32 layer and hold that layer's
    /// allocation lock, not a GuestMemory lock.
    std::map<usize, u32> m_linear_watch_host_pages;
    mutable std::mutex m_linear_watch_mutex;

    /// Identity mapping: host address == guest address. Distinct from
    /// m_linear_base, which cannot express it — a base of 0 is exactly what
    /// identity needs, but nullptr already means "no linear model at all".
    bool  m_linear_identity = false;

    /// Address ranges the host owns and the guest can never have, as sorted,
    /// non-overlapping [lo, hi) pairs. Only meaningful under identity mapping.
    std::vector<std::pair<GuestAddr, GuestAddr>> m_reserved_holes;

    /// Make [guest_addr, +size) a host mapping we own, then apply @p prot.
    ///
    /// Unmapped gaps are claimed with our VM tag. libmalloc zone pages,
    /// `VM_MEMORY_GUARD`, and any other non-guest mapping are refused and
    /// recorded as reserved holes so a later VirtualAlloc search skips them.
    /// They used to look like our PROT_NONE reservation, so commit then
    /// mprotect/memset them and the guest saw ERROR_NOT_ENOUGH_MEMORY (or
    /// the host SIGSEGV'd).
    /// @p existing_guest_region is true when GuestMemory already recorded
    /// this span (commit/protect/decommit); RW pages with a foreign tag are
    /// then ours-in-disguise (test mmaps, legacy claims) rather than theft.
    [[nodiscard]] bool identity_prepare_span(GuestAddr guest_addr, usize size,
                                             MemoryProtection prot,
                                             bool existing_guest_region);

    /// Replace [guest_addr, +size) with our PROT_NONE identity reservation.
    ///
    /// Identity unmap used to `munmap`, which hands the VA back to the host.
    /// malloc/Metal then occupy it and the next guest MapView/VirtualAlloc
    /// returns ERROR_NOT_ENOUGH_MEMORY. Caller must hold `m_regions_mutex`.
    bool identity_restore_span(GuestAddr guest_addr, usize size);

    /// Apply @p prot to the host pages backing [guest_addr, +size) under
    /// identity mapping. Fills unmapped gaps first; returns false if the
    /// span is host-owned or cannot be made accessible.
    ///
    /// The linear model needs no equivalent: there, every committed host page
    /// is already RW and guest-visible permissions are enforced by the
    /// protection runs, so host state never has to move. Under identity the
    /// guest address IS the host address, so a guest VirtualProtect has to
    /// reach the real pages or the guest's own W^X transitions would not take
    /// effect.
    [[nodiscard]] bool identity_host_protect(GuestAddr guest_addr, usize size,
                                             MemoryProtection prot) noexcept;

    /// Back a region inside the identity/linear reservation. Returns an
    /// invalid MemoryRegion (is_valid()==false) on failure; caller then errors.
    /// The returned region borrows the reservation (owns()==false) so its dtor
    /// never munmaps — the reservation owns the address space.
    [[nodiscard]] MemoryRegion map_in_reservation(GuestAddr guest_addr,
                                                  usize aligned_size,
                                                  bool reserve_only);

    /// Recompute the host-page protection for every host page touched by the
    /// guest range. regions_ remains the source of truth: a host page stays RW
    /// while any committed guest region overlaps it, and becomes PROT_NONE
    /// only after its final committed owner is gone. Must be called with
    /// m_regions_mutex held exclusively and after the region-table mutation.
    void linear_reconcile_host_pages(GuestAddr guest_addr, usize size,
                                     bool discard_unowned);

    /// Point the JIT view's [lo, hi) (host-page aligned guest offsets) at the
    /// host view's CURRENT pages again, PROT_NONE. Required after anything
    /// replaces the host view's backing there (a MAP_FIXED drop, a
    /// mach_vm_remap of shared memory): the JIT view would otherwise keep the
    /// old pages. Caller holds m_jit_view_mutex and re-protects afterwards.
    void linear_jit_view_realias_locked(usize lo, usize hi);

    /// Set the JIT view's protection over [lo, hi) to @p prot. Caller holds
    /// m_jit_view_mutex.
    void linear_jit_view_protect_locked(usize lo, usize hi, int prot);

    /// The JIT-view protection of every host page in [lo, hi), decided from
    /// published state only (no region table): the host view's VM protection
    /// for a uniform page, the subpage map for a mixed one, the check byte for
    /// code and watch. For the code-page and write-watch publishers, which hold
    /// their own mutexes and so cannot take m_regions_mutex. Takes
    /// m_jit_view_mutex.
    void linear_jit_view_refresh(usize lo, usize hi);
    void linear_jit_view_refresh_locked(usize lo, usize hi);

    /// The JIT-view protection of host page @p index given its
    /// most-restrictive guest protection @p meet (PROT_* bits): minus WRITE
    /// while the page holds code or is watched.
    [[nodiscard]] int linear_jit_view_final_prot(usize index, int meet) const noexcept;

    /// VITREO_JIT_VIEW_VERIFY=1: abort with `[jit-view] INCOHERENT` when the
    /// views no longer alias over [lo, hi). True (and free) otherwise.
    bool linear_jit_view_verify_after_reconcile(usize lo, usize hi) const;

    /// Unmap the JIT view and its guards (destructor).
    void linear_jit_view_teardown() noexcept;

    /// Set or clear @p bits in the check-page byte of every host page the guest
    /// range touches. A no-op when there is no reservation (and therefore no
    /// map), so callers on the shared paths need no model test.
    ///
    /// Ordering is the caller's: publish a bit BEFORE relaxing whatever it
    /// guards, and clear it only AFTER tightening, so a racing thread reads a
    /// stale byte that is conservative rather than one that is permissive.
    void linear_check_bits_update(GuestAddr guest_addr, usize size, u8 bits,
                                  bool set) noexcept;

    /// Make the extension's host pages under [guest_addr, guest_addr + size)
    /// ordinary PROT_NONE reservation (see m_linear_ext_materialized). A
    /// no-op below the core's end or without an extension. Called under
    /// m_regions_mutex by every path that creates a region in the window,
    /// before it commits or remaps host pages there. False: the host refused.
    [[nodiscard]] bool linear_extension_materialize(GuestAddr guest_addr,
                                                    usize size);

public:
    /// Publish / retire `kLinearCheckWatched` over a MEM_WRITE_WATCH
    /// allocation's range, refcounted per 16 KiB host page.
    ///
    /// Refcounted because a host page covers four guest pages and nothing here
    /// may assume two watched allocations never share one. Windows' 64 KiB
    /// allocation granularity probably makes that impossible, but an
    /// under-reported dirty page makes a guest GC free a live object, so the
    /// bit's lifetime is tracked rather than inferred.
    void linear_watch_range_retain(GuestAddr base, usize size) noexcept;
    void linear_watch_range_release(GuestAddr base, usize size) noexcept;

    /// True when every host page overlapping [addr, addr+size) is demand-zero
    /// fresh, so MEM_COMMIT need not memset (Windows zeros on first touch).
    [[nodiscard]] bool commit_pages_are_fresh(GuestAddr addr, usize size) const;

private:

    /// Track one 4 KiB guest code page against the 16 KiB host page containing
    /// it, publishing `kLinearCheckHoldsCode` on the first retain and clearing
    /// it on the last release. Called with `shard.lock` held exclusively,
    /// where `shard` is jit_code_page_shard(page).
    void linear_code_page_retain(JitCodePageShard& shard,
                                 GuestAddr page) noexcept;
    void linear_code_page_release(JitCodePageShard& shard,
                                  GuestAddr page) noexcept;

    /// One (committed, protection) run of the guest range, as the region table
    /// sees it. Gaps between regions and uncommitted runs both appear as
    /// `committed == false`, matching the host-page lifecycle rules.
    struct LinearSpan {
        usize lo;
        usize hi;
        bool committed;
        MemoryProtection prot;
    };

    /// Decompose [lo, hi) into the region table's committed/protection runs.
    /// Caller holds m_regions_mutex. Runs are sorted, non-overlapping, and
    /// cover the whole range; unmapped gaps are emitted as uncommitted runs.
    void linear_collect_spans(usize lo, usize hi,
                              std::vector<LinearSpan>& out) const;
    /// True when every host page overlapping [addr, addr+size) is demand-zero
    /// fresh: the range is linear-backed and no committed guest page OUTSIDE
    /// the range shares one of those host pages. A fully-unowned host page is
    /// always fresh (a reservation's pages are untouched anonymous memory and
    /// decommit re-maps a wholly freed host page), so such a commit needs no
    /// explicit zeroing: Windows' MEM_COMMIT zero-fill is then the kernel's,
    /// page by page on first touch, and untouched committed memory stays
    /// non-resident. Zeroing it eagerly makes resident memory equal committed
    /// memory, and a browser renderer commits far more than it touches.
    /// `_locked` expects the
    /// caller to hold m_regions_mutex.
    [[nodiscard]] bool commit_pages_are_fresh_locked(GuestAddr addr, usize size) const;
};

} // namespace vitreo
