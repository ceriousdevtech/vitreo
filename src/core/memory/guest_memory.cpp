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
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <thread>

namespace vitreo {

// =============================================================================
// Lock-free per-thread TLB
// =============================================================================

namespace {

struct TlbEntry {
    GuestAddr base{0};
    GuestAddr limit{0};       // base + requested_size
    u8* host_data{nullptr};
    bool writable{false};
    bool readable{false};
    bool executable{false};
};

// 16 slots thrashed on a multi-heap 32-bit walk: every helper read_u32
// missed, took the region lock, and showed up as tlb_find in sample.
// 256 entries is 8 KiB per thread (power of two for the hand mask).
constexpr int TLB_SIZE = 256;

struct ThreadTlb {
    TlbEntry entries[TLB_SIZE]{};
    // Hints into entries, never cached pointers or permissions. Revalidate the
    // selected entry after the normal generation/protection shootdown checks.
    // Stale hints remain safe after eviction, owner changes, and invalidation.
    u8 page_hints[1024]{};
    u8 hand{0};
    u8 last{0};
    // Slots filled since the last whole-TLB discard: they are 0..filled-1,
    // because a discard rewinds `hand` to 0. A miss scans only those.
    u16 filled{0};
    u64 generation{0};
    u64 protect_seq{0};
    const void* owner{nullptr};
};

// initial-exec TLS model: on macOS arm64 the default ("global-dynamic")
// emits a __tlv_get_addr libc call for *every* access to a thread_local.
// Initial-exec resolves the offset at link time and accesses TLS via a
// single LDR off TPIDRRO_EL0.
// Safe here because vitreo is a statically-linked executable (vitreo_memory
// is a STATIC archive linked directly into the main binary).
thread_local __attribute__((tls_model("initial-exec"))) ThreadTlb t_tlb;
// The observer map has a non-trivial destructor, and so does the thread_local
// JitCompiler whose destructor unregisters from it. The order in which two
// thread_locals are destroyed at thread exit is unspecified, so ~JitCompiler
// can run after the map is already gone and fault inside erase(). This flag is
// trivially destructible, so it stays readable for the whole teardown and lets
// every accessor detect that the map is no longer alive.
thread_local __attribute__((tls_model("initial-exec")))
    bool t_executable_write_observers_alive = false;

struct ExecutableWriteObserverMap {
    std::unordered_map<const GuestMemory*, GuestMemory::ExecutableWriteObserver> map;

    ExecutableWriteObserverMap() { t_executable_write_observers_alive = true; }
    ~ExecutableWriteObserverMap() { t_executable_write_observers_alive = false; }
};

thread_local ExecutableWriteObserverMap t_executable_write_observers;

// Single-entry memo over the map above. notify_executable_write runs on every
// guest memory write, and a process almost always has exactly one GuestMemory,
// so the hash lookup resolves to the same bucket every time. Caching the last
// (owner, observer) pair turns it into a pointer compare.
//
// The map stays the source of truth; this only short-circuits the lookup. Any
// mutation of the map clears the memo, so it cannot outlive the entry it
// points at.
thread_local __attribute__((tls_model("initial-exec")))
    const GuestMemory* t_observer_memo_owner = nullptr;
thread_local __attribute__((tls_model("initial-exec")))
    GuestMemory::ExecutableWriteObserver* t_observer_memo = nullptr;

inline void invalidate_observer_memo() noexcept {
    t_observer_memo_owner = nullptr;
    t_observer_memo = nullptr;
}

// Find matching TLB entry, returning a pointer to it (or nullptr on miss).
// Returning a pointer lets the caller dereference it without making another
// thread_local access (one TLV resolution per memory op instead of two).
inline bool tlb_range_overlaps(GuestAddr a0, GuestAddr a1,
                               GuestAddr b0, usize bsize) noexcept {
    if (a1 <= a0 || bsize == 0) {
        return false;
    }
    const GuestAddr b1 = b0 + static_cast<GuestAddr>(bsize);
    if (b1 < b0) {
        return true;
    }
    return a0 < b1 && b0 < a1;
}

[[gnu::noinline]] void tlb_apply_protect_shootdowns_slow(
    const GuestMemory* mem, ThreadTlb& tlb, u64 seq) {
    GuestMemory::ProtectShootdown events[GuestMemory::kProtectShootdownLog];
    bool overflow = false;
    const usize n = mem->copy_protect_shootdowns_after(
        tlb.protect_seq, events, GuestMemory::kProtectShootdownLog, &overflow);
    tlb.protect_seq = seq;
    if (overflow) {
        for (auto& e : tlb.entries) e = {};
        tlb.hand = 0;
        tlb.filled = 0;
        tlb.last = 0;
        return;
    }
    for (usize i = 0; i < n; ++i) {
        for (auto& e : tlb.entries) {
            if (e.host_data &&
                tlb_range_overlaps(e.base, e.limit, events[i].addr,
                                   events[i].size)) {
                e = {};
            }
        }
    }
}

inline void tlb_apply_protect_shootdowns(const GuestMemory* mem, ThreadTlb& tlb) {
    const u64 seq = mem->protect_sequence();
    if (tlb.protect_seq == seq) {
        return;
    }
    // Keep the 256-event replay buffer out of the ordinary TLB-hit frame.
    // Otherwise Clang reserves that several-kilobyte array before this early
    // return and Darwin emits ___chkstk_darwin on every guest memory access.
    tlb_apply_protect_shootdowns_slow(mem, tlb, seq);
}

inline bool tlb_entry_covers(const TlbEntry& e, GuestAddr addr,
                             usize size) noexcept {
    // `addr + size` must not be used here: it wraps for addresses near
    // UINT64_MAX, which made a bogus address satisfy the bound and
    // return host_data + (addr - base) — a wild pointer the caller then
    // wrote through (host SIGBUS rather than a guest fault).
    return e.host_data && addr >= e.base && addr < e.limit &&
           size <= static_cast<usize>(e.limit - addr);
}

inline TlbEntry* tlb_find(const GuestMemory* owner, u64 gen,
                          GuestAddr addr, usize size) {
    ThreadTlb& tlb = t_tlb;
    if (tlb.owner == owner && tlb.generation == gen) {
        tlb_apply_protect_shootdowns(owner, tlb);
        auto& last = tlb.entries[tlb.last];
        if (tlb_entry_covers(last, addr, size)) {
            return &last;
        }
        const GuestAddr page = addr >> 12;
        auto& hint = tlb.page_hints[(page ^ (page >> 10) ^ (page >> 20)) & 1023];
        auto& predicted = tlb.entries[hint];
        if (tlb_entry_covers(predicted, addr, size)) {
            tlb.last = hint;
            return &predicted;
        }
        for (int i = 0; i < tlb.filled; ++i) {
            if (i == tlb.last) {
                continue;
            }
            auto& e = tlb.entries[i];
            if (tlb_entry_covers(e, addr, size)) {
                hint = static_cast<u8>(i);
                tlb.last = hint;
                return &e;
            }
        }
    } else {
        for (auto& e : tlb.entries) e = {};
        tlb.hand = 0;
        tlb.filled = 0;
        tlb.last = 0;
        tlb.generation = gen;
        tlb.protect_seq = owner->protect_sequence();
        tlb.owner = owner;
    }
    return nullptr;
}

// Populate the next TLB slot (round-robin).
inline void tlb_fill(GuestAddr base, usize alloc_size,
                     u8* data, bool writable, bool readable, bool executable) {
    ThreadTlb& tlb = t_tlb;
    auto& slot = tlb.entries[tlb.hand];
    slot.base = base;
    slot.limit = base + alloc_size;
    slot.host_data = data;
    slot.writable = writable;
    slot.readable = readable;
    slot.executable = executable;
    tlb.last = tlb.hand;
    tlb.hand = (tlb.hand + 1) & (TLB_SIZE - 1);
    if (tlb.filled < TLB_SIZE) ++tlb.filled;
}

// Re-sync TLB generation under shared_lock (gen is stable while held).
inline void tlb_resync(const GuestMemory* owner, u64 stable_gen) {
    ThreadTlb& tlb = t_tlb;
    if (tlb.owner != owner || tlb.generation != stable_gen) {
        for (auto& e : tlb.entries) e = {};
        tlb.hand = 0;
        tlb.filled = 0;
        tlb.last = 0;
        tlb.generation = stable_gen;
        tlb.protect_seq = owner->protect_sequence();
        tlb.owner = owner;
    } else {
        tlb_apply_protect_shootdowns(owner, tlb);
    }
}

} // anonymous namespace



// =============================================================================
// Gap Finding
// =============================================================================

std::vector<std::pair<GuestAddr, GuestAddr>>
GuestMemory::region_extents() const {
    std::shared_lock lk(m_regions_mutex);
    std::vector<std::pair<GuestAddr, GuestAddr>> out;
    out.reserve(regions_.size());
    for (const auto& [base, mapped] : regions_) {
        out.emplace_back(base, base + mapped.requested_size);
    }
    return out;
}

std::vector<std::pair<GuestAddr, GuestAddr>>
GuestMemory::executable_extents() const {
    std::shared_lock lk(m_regions_mutex);
    std::vector<std::pair<GuestAddr, GuestAddr>> out;
    for (const auto& [base, mapped] : regions_) {
        // A region carries protection runs keyed by offset; any run with
        // Execute makes that span code.
        for (auto it = mapped.protections.begin();
             it != mapped.protections.end(); ++it) {
            const auto next = std::next(it);
            const usize span_end = (next == mapped.protections.end())
                                       ? mapped.requested_size
                                       : next->first;
            if (span_end <= it->first) continue;
            const auto bits = static_cast<u8>(it->second);
            if ((bits & static_cast<u8>(MemoryProtection::Execute)) == 0) {
                continue;
            }
            out.emplace_back(base + it->first, base + span_end);
        }
    }
    return out;
}

GuestAddr GuestMemory::find_free_gap(GuestAddr min_start, usize size,
                                     usize alignment) const {
    std::shared_lock lk(m_regions_mutex);
    const usize aligned_size = align_up(size, GUEST_PAGE_SIZE);
    const GuestAddr base_align = static_cast<GuestAddr>(
        alignment > GUEST_PAGE_SIZE ? alignment : GUEST_PAGE_SIZE);
    GuestAddr candidate = align_up(min_start, base_align);

    // Exclusive end of the searchable user VA range. Non-LAA 32-bit processes
    // must not receive CRT/D3D Map arenas at 0x80000000+ — guest code treating
    // pointers as signed or capped at 2 GiB then AVs (observed: 0x80001000).
    const GuestAddr search_end = m_maximum_user_address != 0
        ? static_cast<GuestAddr>(m_maximum_user_address) + 1u
        : 0xFFFF0000ULL;
    const auto fits = [&](GuestAddr base) -> bool {
        return base != 0 && aligned_size != 0 && base < search_end &&
               aligned_size <= static_cast<usize>(search_end - base);
    };
    if (!fits(candidate)) {
        return 0;
    }

    // Skip the prefix a previous identical search already proved full.
    const FitKey key{candidate, aligned_size, static_cast<usize>(base_align)};
    {
        std::lock_guard floor_lk(m_fit_floor_mutex);
        const auto floor = m_fit_floors.find(key);
        if (floor != m_fit_floors.end() && floor->second > candidate) {
            const GuestAddr floored = align_up(floor->second, base_align);
            if (!fits(floored)) return 0;  // proved: nothing fits
            candidate = floored;
        }
    }
    // Held under the shared region lock, so no unmap can free space between
    // the walk and recording what it proved.
    const auto remember = [&](GuestAddr result) {
        std::lock_guard floor_lk(m_fit_floor_mutex);
        if (m_fit_floors.size() >= 4096) m_fit_floors.clear();
        m_fit_floors[key] = result != 0 ? result : search_end;
        return result;
    };

    // Two obstacles: reserved holes (address space the host process owns under
    // identity mapping) and regions already mapped. Clearing one can push the
    // candidate into the other, so the scan restarts whenever it moves. The
    // candidate only ever increases and fits() bounds it, so this terminates.
    // With no holes recorded — every translated run — the hole pass is a no-op
    // and this is the original single-pass region scan.
    for (;;) {
        const GuestAddr cleared =
            next_address_clear_of_holes(candidate, aligned_size);
        if (cleared != candidate) {
            candidate = align_up(cleared, base_align);
            if (!fits(candidate)) {
                return remember(0);
            }
            continue;
        }

        // regions_ is a sorted map (by base address), so the scan walks in
        // order, starting at the first region that can overlap `candidate`,
        // never at begin(). Restarting the walk from the front is O(regions^2)
        // per call: the outer loop restarts every time the candidate moves,
        // and the candidate moves once per region it has to step over. A
        // a service creates threads that each replay one virtual_alloc
        // per DLL TLS slot. One guest thread then spends a whole core inside
        // this function, and the cost grows with the region map until it
        // throttles the transfer.
        bool moved = false;
        auto it = regions_.upper_bound(candidate);
        if (it != regions_.begin()) {
            // The greatest base <= candidate is the only earlier region that
            // can still cover it; regions do not overlap.
            --it;
        }
        for (; it != regions_.end(); ++it) {
            const GuestAddr base = it->first;
            if (base >= search_end) {
                break;
            }
            const GuestAddr region_end = base + it->second.requested_size;

            // If the candidate block fits entirely before this region, every
            // later region is further away still.
            if (candidate + aligned_size <= base) {
                break;
            }

            // Otherwise, push candidate past this region if it overlaps.
            if (candidate < region_end) {
                candidate = align_up(region_end, base_align);
                if (!fits(candidate)) {
                    return remember(0);
                }
                moved = true;
            }
        }
        if (!moved) {
            return remember(fits(candidate) ? candidate : 0);
        }
    }
}

GuestAddr GuestMemory::map_first_fit(GuestAddr min_start, usize size,
                                     MemoryProtection prot, bool committed,
                                     usize alignment, Error* error) {
    // Bounded: each retry means another thread mapped the gap we found, so
    // the table strictly grew; thousands of consecutive losses would mean a
    // livelock, reported rather than spun on.
    for (int attempt = 0; attempt < 1024; ++attempt) {
        const GuestAddr base = find_free_gap(min_start, size, alignment);
        if (base == 0) {
            if (error != nullptr) *error = Error::OutOfMemory;
            return 0;
        }
        const VoidResult mapped = map_region(base, size, prot, committed);
        if (mapped.is_ok()) return base;
        if (mapped.error() != Error::AlreadyExists) {
            if (error != nullptr) *error = mapped.error();
            return 0;
        }
    }
    if (error != nullptr) *error = Error::AlreadyExists;
    return 0;
}

void GuestMemory::forget_fit_floors() noexcept {
    std::lock_guard floor_lk(m_fit_floor_mutex);
    m_fit_floors.clear();
}

void GuestMemory::lower_fit_floors(GuestAddr freed) noexcept {
    // Caller holds m_regions_mutex exclusively and has already erased the
    // region. The freed range joins the free gap before it, so a fit can now
    // begin as low as the end of the previous region.
    //
    // The only free space this unmap created is the merged gap
    // [gap_lo, gap_hi) between the neighbouring regions, so a floor F for
    // (start, size, alignment) stays valid unless an aligned base >= start
    // inside that gap can hold `size`. When one can, the lowest such base is
    // the new floor: every lower base either lies wholly in unchanged space
    // (already proved full below F), overlaps the previous region, or is
    // misaligned / below start. Lowering only the floors the gap can serve
    // matters because frees are constant: dropping every floor made each
    // larger request re-walk every region above the freed one. Reserved
    // holes are ignored here; they only remove space, so a floor that is too
    // low is still a valid floor.
    auto next = regions_.lower_bound(freed);
    GuestAddr gap_lo = 0;
    if (next != regions_.begin()) {
        const auto prev = std::prev(next);
        gap_lo = std::min(freed, prev->first + prev->second.requested_size);
    }
    const GuestAddr gap_hi = next != regions_.end()
                                 ? next->first
                                 : std::numeric_limits<GuestAddr>::max();
    std::lock_guard floor_lk(m_fit_floor_mutex);
    for (auto& [key, floor] : m_fit_floors) {
        if (gap_lo >= floor) continue;
        const GuestAddr lowest = std::max(key.start, gap_lo);
        const GuestAddr candidate =
            align_up(lowest, static_cast<GuestAddr>(key.alignment));
        if (candidate < lowest || candidate >= floor) continue;
        if (candidate >= gap_hi || key.size > gap_hi - candidate) continue;
        floor = candidate;
    }
}

GuestAddr GuestMemory::next_region_base(GuestAddr addr) const {
    std::shared_lock lk(m_regions_mutex);
    // upper_bound gives us the first region whose base is strictly > addr.
    auto it = regions_.upper_bound(addr);
    if (it != regions_.end()) {
        return it->first;
    }
    return 0;
}


// =============================================================================
// Read Operations
// =============================================================================

// Single-pass read helpers: resolve_addr() does TLB lookup + region check in one
// shot; the readable flag comes from the TLB entry populated by the slow path.
// This avoids the previous pattern of has_protection() (locked region scan) +
// resolve_addr() (second TLB/region scan) on every interpreter memory access.
const u8* GuestMemory::resolve_for_read(GuestAddr addr, usize size, Error* err) const {
    return resolve_for_access(addr, size, err, false);
}

// Instruction fetch. PAGE_EXECUTE (Learn, Memory Protection Constants)
// "enables execute access" with no read access, so an execute-only page --
// a PE section flagged IMAGE_SCN_MEM_EXECUTE without IMAGE_SCN_MEM_READ, or
// VirtualProtect(PAGE_EXECUTE) -- must fetch. Readable pages keep fetching as
// before. Data reads of such a page still go through resolve_for_read.
const u8* GuestMemory::resolve_for_fetch(GuestAddr addr, usize size, Error* err) const {
    return resolve_for_access(addr, size, err, true);
}

const u8* GuestMemory::resolve_for_access(GuestAddr addr, usize size, Error* err,
                                          bool fetch) const {
    u64 gen = m_translation_generation.load(std::memory_order_acquire);
    if (TlbEntry* e = tlb_find(this, gen, addr, size)) {
        if (!e->readable && !(fetch && e->executable)) {
            if (err) *err = Error::ProtectionFault;
            return nullptr;
        }
        return e->host_data + (addr - e->base);
    }
    // Slow path: fall through to resolve_addr which fills the TLB
    bool writable = false;
    const u8* p = resolve_addr(addr, size, &writable);
    if (!p) {
        // A legal access may cross two differently protected 4 KiB runs
        // (for example RW into RO) while remaining entirely readable. Such a
        // range cannot be represented by one uniform TLB entry, so validate it
        // directly and return the contiguous backing pointer without caching.
        std::shared_lock lk(m_regions_mutex);
        const MappedRegion* mapped = find_region(addr);
        if (mapped == nullptr) {
            if (err) *err = Error::InvalidAddress;
            return nullptr;
        }
        const usize offset = static_cast<usize>(addr - mapped->base);
        if (offset >= mapped->requested_size ||
            size > mapped->requested_size - offset) {
            if (err) *err = Error::InvalidAddress;
            return nullptr;
        }
        const usize end = offset + size;
        usize cursor = offset;
        while (cursor < end) {
            const ProtectionSpan span = protection_span(*mapped, cursor);
            if (!has_flag(span.protection, MemoryProtection::Read) &&
                !(fetch && has_flag(span.protection, MemoryProtection::Execute))) {
                if (err) *err = Error::ProtectionFault;
                return nullptr;
            }
            cursor = std::min(span.end, end);
        }
        return mapped->region.ptr_at(offset);
    }
    // Check readable via the freshly-filled TLB entry
    if (TlbEntry* e2 = tlb_find(this, m_translation_generation.load(std::memory_order_acquire), addr, size); e2 && !e2->readable &&
        !(fetch && e2->executable)) {
        if (err) *err = Error::ProtectionFault;
        return nullptr;
    }
    return p;
}

ReadResult<u8> GuestMemory::read_u8(GuestAddr addr) const {
    Error read_err = Error::ProtectionFault;
    const u8* p = resolve_for_read(addr, 1, &read_err);
    if (!p) return ReadResult<u8>::err(read_err);
    const u8 watched_value = *p;
    if (read_watch_enabled()) read_watch_check(addr, sizeof(u8), watched_value);
    return ReadResult<u8>::ok(watched_value);
}

ReadResult<u16> GuestMemory::read_u16(GuestAddr addr) const {
    Error read_err = Error::ProtectionFault;
    const u8* p = resolve_for_read(addr, sizeof(u16), &read_err);
    if (!p) {
        u16 value = 0;
        const auto result = read_bytes(addr, reinterpret_cast<Byte*>(&value), sizeof(value));
        return result.is_ok() ? ReadResult<u16>::ok(value)
                              : ReadResult<u16>::err(result.error());
    }
    u16 value;
    std::memcpy(&value, p, sizeof(u16));
    const u16 watched_value = value;
    if (read_watch_enabled()) read_watch_check(addr, sizeof(u16), watched_value);
    return ReadResult<u16>::ok(watched_value);
}

ReadResult<u32> GuestMemory::read_u32(GuestAddr addr) const {
    Error read_err = Error::ProtectionFault;
    const u8* p = resolve_for_read(addr, sizeof(u32), &read_err);
    if (!p) {
        u32 value = 0;
        const auto result = read_bytes(addr, reinterpret_cast<Byte*>(&value), sizeof(value));
        return result.is_ok() ? ReadResult<u32>::ok(value)
                              : ReadResult<u32>::err(result.error());
    }
    u32 value;
    std::memcpy(&value, p, sizeof(u32));
    const u32 watched_value = value;
    if (read_watch_enabled()) read_watch_check(addr, sizeof(u32), watched_value);
    return ReadResult<u32>::ok(watched_value);
}

ReadResult<u64> GuestMemory::read_u64(GuestAddr addr) const {
    Error read_err = Error::ProtectionFault;
    const u8* p = resolve_for_read(addr, sizeof(u64), &read_err);
    if (!p) {
        u64 value = 0;
        const auto result = read_bytes(addr, reinterpret_cast<Byte*>(&value), sizeof(value));
        return result.is_ok() ? ReadResult<u64>::ok(value)
                              : ReadResult<u64>::err(result.error());
    }
    u64 value;
    std::memcpy(&value, p, sizeof(u64));
    const u64 watched_value = value;
    if (read_watch_enabled()) read_watch_check(addr, sizeof(u64), watched_value);
    return ReadResult<u64>::ok(watched_value);
}

VoidResult GuestMemory::read_bytes(GuestAddr addr, Byte* buffer, usize size) const {
    if (buffer == nullptr || size == 0) {
        return VoidResult::err(Error::InvalidArgument);
    }
    Error read_err = Error::ProtectionFault;
    const u8* p = resolve_for_read(addr, size, &read_err);
    if (!p) {
        if (size - 1 > std::numeric_limits<GuestAddr>::max() - addr) {
            return VoidResult::err(Error::InvalidAddress);
        }
        struct Chunk { const Byte* host; usize size; };
        std::vector<Chunk> chunks;
        std::shared_lock lock(m_regions_mutex);
        usize done = 0;
        while (done < size) {
            const GuestAddr cursor = addr + done;
            const auto* mapped = find_region(cursor);
            if (!mapped) return VoidResult::err(Error::InvalidAddress);
            const usize offset = static_cast<usize>(cursor - mapped->base);
            if (offset >= mapped->requested_size) {
                return VoidResult::err(Error::InvalidAddress);
            }
            const auto span = protection_span(*mapped, offset);
            if (!has_flag(span.protection, MemoryProtection::Read)) {
                return VoidResult::err(Error::ProtectionFault);
            }
            const usize count = std::min({size - done,
                mapped->requested_size - offset, span.end - offset});
            chunks.push_back({mapped->region.ptr_at(offset), count});
            done += count;
        }
        done = 0;
        for (const auto& chunk : chunks) {
            std::memcpy(buffer + done, chunk.host, chunk.size);
            done += chunk.size;
        }
        return VoidResult::ok();
    }
    std::memcpy(buffer, p, size);
    return VoidResult::ok();
}


// =============================================================================
// Private Helpers
// =============================================================================

GuestMemory::MappedRegion* GuestMemory::find_region(GuestAddr addr) {
    // Find the first region whose base is <= addr
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) {
        return nullptr;
    }
    --it;
    
    MappedRegion& mapped = it->second;
    if (addr >= mapped.base && addr < mapped.base + mapped.requested_size) {
        return &mapped;
    }
    
    return nullptr;
}

const GuestMemory::MappedRegion* GuestMemory::find_region(GuestAddr addr) const {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) {
        return nullptr;
    }
    --it;
    
    const MappedRegion& mapped = it->second;
    if (addr >= mapped.base && addr < mapped.base + mapped.requested_size) {
        return &mapped;
    }
    
    return nullptr;
}

// =============================================================================
// Lock-free address resolution (TLB probe + locked fallback)
// =============================================================================

const u8* GuestMemory::resolve_addr(GuestAddr addr, usize size, bool* writable) const {
    // Lock-free TLB fast path
    u64 gen = m_translation_generation.load(std::memory_order_acquire);
    if (TlbEntry* e = tlb_find(this, gen, addr, size)) {
        if (writable) *writable = e->writable;
        return e->host_data + (addr - e->base);
    }
    // Slow path: locked region lookup + TLB fill
    std::shared_lock lk(m_regions_mutex);
    tlb_resync(this, m_translation_generation.load(std::memory_order_relaxed));
    const MappedRegion* mapped = find_region(addr);
    if (!mapped) return nullptr;
    usize offset = addr - mapped->base;
    // Same overflow hazard as tlb_find(): compare against the remaining bytes
    // instead of forming offset + size.
    if (offset >= mapped->requested_size) return nullptr;
    if (size > mapped->requested_size - offset) return nullptr;
    const ProtectionSpan span = protection_span(*mapped, offset);
    if (size > span.end - offset) return nullptr;
    MemoryProtection prot = span.protection;
    bool w = has_flag(prot, MemoryProtection::Write);
    bool r = has_flag(prot, MemoryProtection::Read);
    bool x = has_flag(prot, MemoryProtection::Execute);
    tlb_fill(
        mapped->base + span.begin, span.end - span.begin,
        const_cast<u8*>(mapped->region.data()) + span.begin, w, r, x);
    if (writable) *writable = w;
    return mapped->region.ptr_at(offset);
}

bool GuestMemory::is_range_writable(GuestAddr addr, usize size,
                                    bool is_64bit) const {
    if (size == 0) return true;
    // Reject anything the guest architecture cannot address, and any range
    // that would wrap. Both are expressed without forming addr + size.
    const u64 arch_max = is_64bit ? std::numeric_limits<u64>::max()
                                  : std::numeric_limits<u32>::max();
    if (addr > arch_max) return false;
    if (static_cast<u64>(size) - 1 > arch_max - addr) return false;

    // Walk the range so a span crossing out of a mapped region is rejected
    // even when its first byte resolves.
    u64 remaining = size;
    u64 cursor = addr;
    while (remaining > 0) {
        bool writable = false;
        const u8* p = resolve_addr(cursor, 1, &writable);
        if (!p || !writable) return false;
        ++cursor;
        --remaining;
    }
    return true;
}

u8* GuestMemory::resolve_addr_mut(GuestAddr addr, usize size, bool* writable,
                                  bool* executable) {
    u64 gen = m_translation_generation.load(std::memory_order_acquire);
    if (TlbEntry* entry = tlb_find(this, gen, addr, size)) {
        if (writable) *writable = entry->writable;
        if (executable) *executable = entry->executable;
        return entry->host_data + (addr - entry->base);
    }

    auto* result = const_cast<u8*>(
        static_cast<const GuestMemory*>(this)->resolve_addr(addr, size, writable));
    if (result == nullptr) {
        // Like the read path, a scalar store may legally straddle protection
        // runs when every byte is writable. Do not publish a uniform TLB entry
        // for that mixed range.
        std::shared_lock lk(m_regions_mutex);
        MappedRegion* mapped = find_region(addr);
        if (mapped != nullptr) {
            const usize offset = static_cast<usize>(addr - mapped->base);
            if (offset < mapped->requested_size &&
                size <= mapped->requested_size - offset) {
                const usize end = offset + size;
                usize cursor = offset;
                bool all_writable = true;
                bool any_executable = false;
                while (cursor < end) {
                    const ProtectionSpan span =
                        protection_span(*mapped, cursor);
                    all_writable &=
                        has_flag(span.protection, MemoryProtection::Write);
                    any_executable |=
                        has_flag(span.protection, MemoryProtection::Execute);
                    cursor = std::min(span.end, end);
                }
                if (writable) {
                    *writable = all_writable;
                }
                if (executable) {
                    *executable =
                        any_executable || overlaps_code_alias(addr, size);
                }
                return mapped->region.ptr_at(offset);
            }
        }
    }
    if (executable) {
        if (TlbEntry* entry = tlb_find(this, m_translation_generation.load(std::memory_order_acquire), addr, size)) {
            *executable = entry->executable;
        } else {
            *executable = false;
        }
    }
    return result;
}

// =============================================================================
// Fast host pointer access
// =============================================================================

const Byte* GuestMemory::host_ptr(GuestAddr addr, usize size) const {
    return resolve_for_read(addr, size);
}

const Byte* GuestMemory::fetch_ptr(GuestAddr addr, usize size) const {
    return resolve_for_fetch(addr, size);
}

ReadResult<u8> GuestMemory::fetch_u8(GuestAddr addr) const {
    Error fetch_err = Error::ProtectionFault;
    const u8* p = resolve_for_fetch(addr, 1, &fetch_err);
    if (!p) return ReadResult<u8>::err(fetch_err);
    return ReadResult<u8>::ok(*p);
}

bool GuestMemory::guest_can_write(GuestAddr addr, usize size) {
    bool writable = false;
    return resolve_addr_mut(addr, size, &writable, nullptr) != nullptr &&
           writable;
}

Byte* GuestMemory::host_ptr_store(GuestAddr addr, usize size) {
    bool writable = false;
    bool executable = false;
    Byte* result = resolve_addr_mut(addr, size, &writable, &executable);
    if (result == nullptr || !writable) return nullptr;
    if (write_watch::any_active()) {
        write_watch::record_store(addr, size);
    }
    if (executable) {
        notify_executable_write(addr, size);
    }
    return result;
}

Byte* GuestMemory::host_ptr_mut(GuestAddr addr, usize size) {
    bool executable = false;
    Byte* result = resolve_addr_mut(addr, size, nullptr, &executable);
    if (result && write_watch::any_active()) {
        write_watch::record_store(addr, size);
    }
    if (result && executable) {
        notify_executable_write(addr, size);
    }
    return result;
}

namespace {

/// Host pages lying wholly inside [contents, contents+size). A partly covered
/// edge page may hold bytes of a neighbouring 4 KiB guest page, so it is left
/// alone; discarded Windows contents are undefined, so keeping them is valid.
bool contained_host_pages(Byte* contents, usize size, Byte*& lo, usize& len) {
    const uintptr_t page = static_cast<uintptr_t>(::getpagesize());
    const uintptr_t begin = reinterpret_cast<uintptr_t>(contents);
    const uintptr_t first = (begin + page - 1) & ~(page - 1);
    const uintptr_t last = (begin + size) & ~(page - 1);
    if (last <= first) return false;
    lo = reinterpret_cast<Byte*>(first);
    len = static_cast<usize>(last - first);
    return true;
}

/// MADV_FREE_REUSABLE drops the pages from the task footprint immediately and
/// lets the kernel reclaim them without paging; the mapping and protection are
/// unchanged. A later guest store without MADV_FREE_REUSE is still safe: the
/// store sets the pmap modified bit, so the page is no longer reclaimable
/// (only its footprint accounting stays reduced, the same trade a
/// discarded-page allocator accepts). Failure is ignored because
/// keeping the old bytes also satisfies the undefined-contents contract.
void offer_host_pages(Byte* contents, usize size) {
    Byte* lo = nullptr;
    usize len = 0;
    if (contained_host_pages(contents, size, lo, len)) {
        (void)::madvise(lo, len, MADV_FREE_REUSABLE);
    }
}

/// Per host page: does it hold data (resident or compressed)? A page that is
/// neither has no data and reads as zero.
bool host_pages_holding_data(Byte* lo, usize len, std::vector<char>& state) {
    const usize page = static_cast<usize>(::getpagesize());
    state.assign(len / page, 0);
    if (::mincore(lo, len, state.data()) != 0) return false;
    for (char& s : state) {
        s = (s & (MINCORE_INCORE | MINCORE_PAGED_OUT)) != 0 ? 1 : 0;
    }
    return true;
}

}  // namespace

VoidResult GuestMemory::discard_region_contents(GuestAddr addr, usize size) {
    if (size == 0) return VoidResult::err(Error::AccessDenied);
    std::shared_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(addr);
    if (mapped == nullptr) return VoidResult::err(Error::AccessDenied);
    const usize offset = static_cast<usize>(addr - mapped->base);
    if (offset >= mapped->requested_size ||
        size > mapped->requested_size - offset) {
        return VoidResult::err(Error::AccessDenied);
    }
    const usize end = offset + size;
    usize cursor = offset;
    while (cursor < end) {
        const ProtectionSpan span = protection_span(*mapped, cursor);
        if (span.protection != MemoryProtection::ReadWrite) {
            return VoidResult::err(Error::AccessDenied);
        }
        cursor = std::min(span.end, end);
    }

    Byte* contents = mapped->region.ptr_at(offset);
    if (write_watch::hardware_traps()) {
        if (!write_watch::discard_armed_pages(contents, size)) {
            return VoidResult::err(Error::AccessDenied);
        }
    } else {
        offer_host_pages(contents, size);
    }
    return VoidResult::ok();
}

void GuestMemory::reset_region_contents(GuestAddr addr, usize size) {
    if (size == 0) return;
    std::shared_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(addr);
    if (mapped == nullptr) return;
    const usize offset = static_cast<usize>(addr - mapped->base);
    if (offset >= mapped->requested_size) return;
    const usize end = offset + std::min(size, mapped->requested_size - offset);
    for (usize cursor = offset; cursor < end;) {
        const ProtectionSpan span = protection_span(*mapped, cursor);
        const usize span_end = std::min(span.end, end);
        Byte* lo = nullptr;
        usize len = 0;
        if (span.protection == MemoryProtection::ReadWrite &&
            contained_host_pages(mapped->region.ptr_at(cursor),
                                 span_end - cursor, lo, len)) {
            std::vector<char> holding;
            if (host_pages_holding_data(lo, len, holding)) {
                const usize page = static_cast<usize>(::getpagesize());
                std::lock_guard pages_lock(m_reset_pages_mutex);
                for (usize i = 0; i < holding.size(); ++i) {
                    const uintptr_t host =
                        reinterpret_cast<uintptr_t>(lo) + i * page;
                    if (holding[i] != 0) {
                        m_reset_host_pages.insert(host);
                    } else {
                        m_reset_host_pages.erase(host);
                    }
                }
                (void)::madvise(lo, len, MADV_FREE_REUSABLE);
            }
        }
        cursor = span_end;
    }
}

bool GuestMemory::undo_reset_region_contents(GuestAddr addr, usize size) {
    if (size == 0) return false;
    std::shared_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(addr);
    if (mapped == nullptr) return false;
    const usize offset = static_cast<usize>(addr - mapped->base);
    if (offset >= mapped->requested_size) return false;
    const usize end = offset + std::min(size, mapped->requested_size - offset);
    bool intact = true;
    for (usize cursor = offset; cursor < end;) {
        const ProtectionSpan span = protection_span(*mapped, cursor);
        const usize span_end = std::min(span.end, end);
        Byte* lo = nullptr;
        usize len = 0;
        if (span.protection == MemoryProtection::ReadWrite &&
            contained_host_pages(mapped->region.ptr_at(cursor),
                                 span_end - cursor, lo, len)) {
            // REUSE first: from here on the host can no longer reclaim any
            // page, so the residency read below is final.
            std::vector<char> holding;
            if (::madvise(lo, len, MADV_FREE_REUSE) != 0 ||
                !host_pages_holding_data(lo, len, holding)) {
                intact = false;
                holding.clear();
            }
            const usize page = static_cast<usize>(::getpagesize());
            std::lock_guard pages_lock(m_reset_pages_mutex);
            for (usize i = 0; i < len / page; ++i) {
                const uintptr_t host =
                    reinterpret_cast<uintptr_t>(lo) + i * page;
                if (m_reset_host_pages.erase(host) != 0 &&
                    (holding.empty() || holding[i] == 0)) {
                    intact = false;
                }
            }
        }
        cursor = span_end;
    }
    return intact;
}

VoidResult GuestMemory::discard_mapped_region_contents(GuestAddr addr,
                                                       usize size) {
    if (size == 0) return VoidResult::err(Error::AccessDenied);
    std::shared_lock lk(m_regions_mutex);
    MappedRegion* mapped = find_region(addr);
    if (mapped == nullptr) return VoidResult::err(Error::AccessDenied);
    const usize offset = static_cast<usize>(addr - mapped->base);
    if (offset >= mapped->requested_size ||
        size > mapped->requested_size - offset) {
        return VoidResult::err(Error::AccessDenied);
    }
    const usize end = offset + size;
    usize cursor = offset;
    while (cursor < end) {
        const ProtectionSpan span = protection_span(*mapped, cursor);
        if (span.protection != MemoryProtection::ReadWrite) {
            return VoidResult::err(Error::AccessDenied);
        }
        cursor = std::min(span.end, end);
    }

    // Never write through a mapped view: that would alter shared section or
    // file backing. MADV_DONTNEED is a non-writing host hint. When a 4 KiB
    // Windows range cannot be represented independently on a 16 KiB host
    // page, retaining the old bytes is permitted because post-discard content
    // is undefined; commitment and mapping remain unchanged.
    Byte* contents = mapped->region.ptr_at(offset);
    const usize host_page = static_cast<usize>(::getpagesize());
    if ((reinterpret_cast<uintptr_t>(contents) % host_page) == 0 &&
        (size % host_page) == 0 &&
        ::madvise(contents, size, MADV_DONTNEED) != 0) {
        return VoidResult::err(Error::AccessDenied);
    }
    return VoidResult::ok();
}

GuestMemory::RegionInfo GuestMemory::get_region_info(GuestAddr addr) const {
    std::shared_lock lk(m_regions_mutex);
    const MappedRegion* mapped = find_region(addr);
    if (!mapped) return {};
    const usize offset = static_cast<usize>(addr - mapped->base);
    const ProtectionSpan span = protection_span(*mapped, offset);
    if (span.protection == MemoryProtection::None) {
        return {};
    }
    return {
        mapped->base + span.begin,
        static_cast<GuestAddr>(mapped->base + span.end),
        const_cast<u8*>(mapped->region.data()) + span.begin,
    };
}

GuestMemory::RegionInfo GuestMemory::get_region_info_cached(GuestAddr addr) const {
    // Fast path: check the thread-local TLB (no lock, no atomic beyond gen load).
    // Typically the preceding read/write just populated this entry.
    u64 gen = m_translation_generation.load(std::memory_order_acquire);
    if (TlbEntry* e = tlb_find(this, gen, addr, 1)) {
        if (!e->readable && !e->writable && !e->executable) {
            return {};
        }
        return { e->base, e->limit, e->host_data };
    }
    // TLB miss — fall back to the locked region lookup.
    return get_region_info(addr);
}

GuestMemory::AccessInfo GuestMemory::probe_access(GuestAddr addr, usize size) const {
    // Single TLB probe → host pointer + region info in one TLS access.
    // (On macOS, every thread_local touch is a _tlv_get_addr call, so
    // halving the number of probes doubles the throughput of the inner loop.)
    u64 gen = m_translation_generation.load(std::memory_order_acquire);
    if (TlbEntry* e = tlb_find(this, gen, addr, size)) {
        return AccessInfo{
            e->host_data + (addr - e->base),
            e->base, e->limit, e->host_data,
            e->readable, e->writable, e->executable,
        };
    }
    // Slow path: locked region lookup + TLB fill, then read it back from the
    // freshly-filled entry so the caller still gets region info for free.
    std::shared_lock lk(m_regions_mutex);
    tlb_resync(this, m_translation_generation.load(std::memory_order_relaxed));
    const MappedRegion* mapped = find_region(addr);
    if (!mapped) return AccessInfo{};
    usize offset = addr - mapped->base;
    if (offset >= mapped->requested_size ||
        size > mapped->requested_size - offset) return AccessInfo{};
    const ProtectionSpan span = protection_span(*mapped, offset);
    if (size > span.end - offset) {
        const usize access_end = offset + size;
        usize cursor = offset;
        bool readable = true;
        bool writable = true;
        bool executable = false;
        while (cursor < access_end) {
            const ProtectionSpan current =
                protection_span(*mapped, cursor);
            readable &=
                has_flag(current.protection, MemoryProtection::Read);
            writable &=
                has_flag(current.protection, MemoryProtection::Write);
            executable |=
                has_flag(current.protection, MemoryProtection::Execute);
            cursor = std::min(current.end, access_end);
        }
        executable |= overlaps_code_alias(addr, size);
        auto* host_ptr =
            const_cast<u8*>(mapped->region.data()) + offset;
        return AccessInfo{
            host_ptr,
            addr,
            addr + size,
            host_ptr,
            readable,
            writable,
            executable,
        };
    }
    MemoryProtection prot = span.protection;
    bool w = has_flag(prot, MemoryProtection::Write);
    bool r = has_flag(prot, MemoryProtection::Read);
    bool x = has_flag(prot, MemoryProtection::Execute) ||
             overlaps_code_alias(mapped->base + span.begin,
                                 span.end - span.begin);
    u8* host_base =
        const_cast<u8*>(mapped->region.data()) + span.begin;
    GuestAddr base = mapped->base + span.begin;
    GuestAddr end = mapped->base + span.end;
    tlb_fill(base, span.end - span.begin, host_base, w, r, x);
    return AccessInfo{
        const_cast<u8*>(mapped->region.data()) + offset,
        base, end, host_base,
        r, w, x,
    };
}

void GuestMemory::set_thread_executable_write_observer(
    ExecutableWriteObserver observer) {
    // Touching the map constructs it, which sets the alive flag.
    // Rehashing on insert can move the mapped value, so the memo must go.
    invalidate_observer_memo();
    t_executable_write_observers.map[this] = std::move(observer);
}

void GuestMemory::clear_thread_executable_write_observer() {
    // May run from a thread_local destructor after the map itself was
    // destroyed; the registration is already gone in that case.
    if (!t_executable_write_observers_alive) return;
    invalidate_observer_memo();
    t_executable_write_observers.map.erase(this);
}

u64 GuestMemory::next_jit_code_instance_id() noexcept {
    static std::atomic<u64> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

u32 GuestMemory::jit_code_chunk_refs(GuestAddr addr) const noexcept {
    if (addr >= (GuestAddr{1} << 32)) return 0;
    const u32 idx = static_cast<u32>(addr) >> 12;
    const u32 chunk = idx / kJitCodeChunkPages;
    return __atomic_load_n(&m_jit_code_chunk_pages[chunk], __ATOMIC_ACQUIRE);
}

void GuestMemory::retain_one_jit_code_page(GuestAddr page) {
    JitCodePageShard& shard = jit_code_page_shard(page);
    std::lock_guard<SpinRwLock> lock(shard.lock);
    retain_jit_code_page_locked(shard, page);
}

void GuestMemory::release_one_jit_code_page(GuestAddr page) {
    JitCodePageShard& shard = jit_code_page_shard(page);
    std::lock_guard<SpinRwLock> lock(shard.lock);
    release_jit_code_page_locked(shard, page);
}

void GuestMemory::retain_jit_code_page(GuestAddr page) {
    page &= ~GuestAddr{0xFFF};
    retain_one_jit_code_page(page);
    // A writable alias of this page writes the same bytes at a different guest
    // address. The inline store fast path is keyed by page, so the alias page
    // must be marked as code too or those stores never reach the helper that
    // invalidates this translation. Each page's count is independent, so the
    // mirror is retained under its own shard's lock; both are marked before
    // this returns, which is before the caller makes the translation
    // reachable.
    for_each_code_alias_mirror(
        page, [this](GuestAddr mirror) { retain_one_jit_code_page(mirror); });
}

namespace {

template <typename T>
void atomic_lower_to(std::atomic<T>& slot, T value) noexcept {
    T seen = slot.load(std::memory_order_relaxed);
    while (value < seen &&
           !slot.compare_exchange_weak(seen, value, std::memory_order_release,
                                       std::memory_order_relaxed)) {
    }
}

template <typename T>
void atomic_raise_to(std::atomic<T>& slot, T value) noexcept {
    T seen = slot.load(std::memory_order_relaxed);
    while (value > seen &&
           !slot.compare_exchange_weak(seen, value, std::memory_order_release,
                                       std::memory_order_relaxed)) {
    }
}

}  // namespace

void GuestMemory::for_each_code_alias_mirror(
    GuestAddr page, const std::function<void(GuestAddr)>& fn) const {
    if (!m_has_code_aliases.load(std::memory_order_acquire)) return;
    const auto aliases = std::atomic_load(&m_code_aliases);
    if (!aliases) return;
    for (const auto& alias : *aliases) {
        if (page >= alias.target_base &&
            page < alias.target_base + alias.size) {
            const GuestAddr mirror =
                alias.alias_base + (page - alias.target_base);
            if (mirror != page) fn(mirror);
        } else if (page >= alias.alias_base &&
                   page < alias.alias_base + alias.size) {
            const GuestAddr mirror =
                alias.target_base + (page - alias.alias_base);
            if (mirror != page) fn(mirror);
        }
    }
}

void GuestMemory::retain_jit_code_page_locked(JitCodePageShard& shard,
                                              GuestAddr page) {
    const GuestAddr page_hi = page + 0x1000;
    // Shards widen concurrently, so a plain compare-then-store could let a
    // smaller widening overwrite a larger one; a CAS loop only ever widens.
    auto widen_full_span = [&] {
        atomic_lower_to(m_jit_code_span64_lo, page);
        atomic_raise_to(m_jit_code_span64_hi, page_hi);
    };
    if (page >= (GuestAddr{1} << 32)) {
        u32& refs = shard.high_refs[page];
        if (refs++ == 0) {
            // Publish the coarse chunk slot before anything else: a reader that
            // sees a zero slot skips the lock entirely, so the slot must never
            // lag the page becoming marked. Widening early is the same
            // discipline widen_full_span() uses.
            m_jit_high_chunk_counts[jit_high_chunk_slot(page)].fetch_add(
                1, std::memory_order_release);
            widen_full_span();
            linear_code_page_retain(shard, page);
            m_jit_code_generation.fetch_add(1, std::memory_order_release);
        }
        return;
    }

    const u32 idx = static_cast<u32>(page) >> 12;
    if (m_jit_code_page_refs[idx]++ == 0) {
        // Widen the coarse span BEFORE publishing the page-map byte.
        // jit_code_pages_overlap() rejects a range outside the span without
        // touching the map, so the span must never be narrower than the set of
        // marked pages -- publishing the byte first would open a window in
        // which a reader sees the byte's page as outside the span and
        // under-reports code (the direction that lets the JIT run a stale
        // translation of rewritten bytes).
        const u32 page_lo = static_cast<u32>(page);
        const u32 page_hi32 = page_lo + 0x1000u;
        atomic_lower_to(m_jit_code_span_lo, page_lo);
        atomic_raise_to(m_jit_code_span_hi, page_hi32);
        widen_full_span();
        linear_code_page_retain(shard, page);
        __atomic_store_n(&m_jit_code_page_map[idx], u8{1}, __ATOMIC_RELEASE);
        __atomic_add_fetch(&m_jit_code_chunk_pages[idx / kJitCodeChunkPages],
                           u32{1}, __ATOMIC_RELEASE);
        m_jit_code_generation.fetch_add(1, std::memory_order_release);
    }
}

void GuestMemory::release_jit_code_page(GuestAddr page) {
    page &= ~GuestAddr{0xFFF};
    release_one_jit_code_page(page);
    for_each_code_alias_mirror(page, [this](GuestAddr mirror) {
        release_one_jit_code_page(mirror);
    });
}

void GuestMemory::release_jit_code_page_locked(JitCodePageShard& shard,
                                               GuestAddr page) {
    if (page >= (GuestAddr{1} << 32)) {
        auto it = shard.high_refs.find(page);
        if (it == shard.high_refs.end()) return;
        if (--it->second == 0) {
            shard.high_refs.erase(it);
            linear_code_page_release(shard, page);
            // Drop the coarse slot last: while it is still set a racing store
            // merely takes the lock and consults the map, which is always
            // correct. Dropping it early is the one unsafe direction.
            m_jit_high_chunk_counts[jit_high_chunk_slot(page)].fetch_sub(
                1, std::memory_order_release);
            m_jit_code_generation.fetch_add(1, std::memory_order_release);
        }
        return;
    }
    const u32 idx = static_cast<u32>(page) >> 12;
    if (m_jit_code_page_refs[idx] == 0) {
        return;
    }
    if (--m_jit_code_page_refs[idx] == 0) {
        __atomic_store_n(&m_jit_code_page_map[idx], u8{0}, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&m_jit_code_chunk_pages[idx / kJitCodeChunkPages],
                           u32{1}, __ATOMIC_RELEASE);
        // Last, so the bit is dropped only after every other record of this
        // page holding code is gone: while the bit is still set a racing store
        // merely takes the helper, which is always correct.
        linear_code_page_release(shard, page);
        m_jit_code_generation.fetch_add(1, std::memory_order_release);
    }
}

namespace {

/// Repeat-query memo for jit_code_pages_overlap(). The store helpers ask about
/// the *whole* published region on every store that misses the inline fast
/// path, so a region that can never be published (it holds compiled code, or
/// its slot keeps getting evicted) is re-walked page by page on every single
/// store. The page map cannot change without bumping the owner's JIT code
/// generation, so a same-generation repeat of the same query is answerable
/// from the previous result. Thread-local: no synchronization, and each JIT
/// thread has its own store stream.
///
/// A SINGLE entry is not enough. One store-helper call issues two queries with
/// different keys: the exact store address (does this write hit compiled code?)
/// and the whole enclosing region (may the region be published into the inline
/// store table?). With one slot the two evict each other, so the hit rate is
/// zero and the region walk runs on literally every helper store. The table is
/// therefore direct-mapped over a few entries, and single-page queries bypass
/// it entirely — for those the scan is already two atomic loads, cheaper than
/// the lookup, and keeping them out stops them from evicting region answers.
struct JitCodeOverlapMemo {
    u64 owner_id{0};       // 0 = empty; GuestMemory::jit_code_instance_id()
    u64 generation{0};
    GuestAddr addr{0};
    usize size{0};
    bool result{false};
};
constexpr usize kJitCodeOverlapMemoSlots = 8;  // power of two
thread_local JitCodeOverlapMemo
    t_jit_code_overlap_memo[kJitCodeOverlapMemoSlots];

constexpr usize jit_code_overlap_memo_index(GuestAddr addr,
                                            usize size) noexcept {
    // Region bases are page- and usually 64-KiB-aligned, so the low bits of
    // addr>>12 are zero for exactly the keys that matter and a shift-and-mask
    // index would collide every published region into one slot. Multiply-shift
    // so the index comes from the *high* bits of a full-width mix instead.
    const u64 mixed = (static_cast<u64>(addr) ^ (static_cast<u64>(size) << 24))
                      * 0x9E3779B97F4A7C15ull;
    return static_cast<usize>(mixed >> 58) & (kJitCodeOverlapMemoSlots - 1);
}

}  // namespace

bool GuestMemory::jit_code_pages_overlap(GuestAddr addr,
                                         usize size) const noexcept {
    if (size == 0) return false;

    // A query confined to one guest page is answered by the span reject plus a
    // single chunk/page-map load. Memoizing it would cost more than it saves,
    // and it must not displace the multi-page region answers that do amortize.
    if (size <= 0x1000 &&
        ((addr ^ (addr + (size - 1))) & ~GuestAddr{0xFFF}) == 0) {
        return jit_code_pages_overlap_scan(addr, size);
    }

    // Sample the generation BEFORE anything is read. If a page is retained
    // while this query runs, the memo is written under the *old* generation
    // and a later query misses it instead of caching a stale "no code".
    const u64 generation =
        m_jit_code_generation.load(std::memory_order_acquire);
    JitCodeOverlapMemo& memo =
        t_jit_code_overlap_memo[jit_code_overlap_memo_index(addr, size)];
    if (memo.owner_id == m_jit_code_instance_id &&
        memo.generation == generation && memo.addr == addr &&
        memo.size == size) {
        return memo.result;
    }

    const bool result = jit_code_pages_overlap_scan(addr, size);
    memo.owner_id = m_jit_code_instance_id;
    memo.generation = generation;
    memo.addr = addr;
    memo.size = size;
    memo.result = result;
    return result;
}

bool GuestMemory::jit_code_pages_overlap_scan(GuestAddr addr,
                                              usize size) const noexcept {
    // Coarse monotone envelope, the same shape BlockCache::range_is_code_free()
    // and write_watch::range_is_watched() already use. The span only widens,
    // and retain_jit_code_page() widens it before publishing the map byte, so
    // "outside the span" proves "no marked page" -- never the other way round.
    const GuestAddr span_hi =
        m_jit_code_span64_hi.load(std::memory_order_acquire);
    if (span_hi == 0) return false;  // no compiled page anywhere, ever
    const GuestAddr span_lo =
        m_jit_code_span64_lo.load(std::memory_order_acquire);

    const GuestAddr first = addr & ~GuestAddr{0xFFF};
    const GuestAddr extent = static_cast<GuestAddr>(size - 1);
    const GuestAddr last_byte = extent > ~GuestAddr{0} - addr
                              ? ~GuestAddr{0} : addr + extent;
    const GuestAddr last = last_byte & ~GuestAddr{0xFFF};
    if (last < span_lo || first >= span_hi) return false;

    for (GuestAddr page = first;; page += 0x1000) {
        if (page >= (GuestAddr{1} << 32)) {
            // Lock-free coarse reject first, per 2 MiB chunk. Almost every
            // store an x64 guest makes is to a high data page in a chunk that
            // holds no code, and taking a code-page lock for each one
            // serializes every thread of a browser renderer against every other.
            // Only the chunks the filter cannot clear are looked up.
            for (GuestAddr chunk = page & ~GuestAddr{0x1FFFFF};
                 chunk <= last; chunk += 0x200000) {
                if (m_jit_high_chunk_counts[jit_high_chunk_slot(chunk)].load(
                        std::memory_order_acquire) != 0) {
                    const GuestAddr lo = std::max(chunk, page);
                    const GuestAddr hi = std::min(chunk + 0x1FF000, last);
                    if (jit_high_code_in_range(lo, hi)) return true;
                }
                if (chunk > ~GuestAddr{0} - 0x200000) break;  // no wrap
            }
            return false;
        }
        const u32 idx = static_cast<u32>(page) >> 12;
        const u32 chunk = idx / kJitCodeChunkPages;
        if (__atomic_load_n(&m_jit_code_chunk_pages[chunk],
                            __ATOMIC_ACQUIRE) == 0) {
            const GuestAddr chunk_last =
                (page & ~GuestAddr{0x1FFFFF}) + 0x1FF000;
            if (chunk_last >= last) break;
            page = chunk_last;
            continue;
        }
        if (__atomic_load_n(&m_jit_code_page_map[idx], __ATOMIC_ACQUIRE) != 0) {
            return true;
        }
        if (page == last) break;
    }
    return false;
}

bool GuestMemory::jit_high_code_in_range(GuestAddr first,
                                         GuestAddr last) const noexcept {
    const auto shard_has = [&](const JitCodePageShard& shard, GuestAddr lo,
                               GuestAddr hi) {
        std::shared_lock<SpinRwLock> lock(shard.lock);
        const auto candidate = shard.high_refs.lower_bound(lo);
        return candidate != shard.high_refs.end() && candidate->first <= hi &&
               candidate->second != 0;
    };
    const u64 first_host = static_cast<u64>(first) >> kLinearHostPageShift;
    const u64 last_host = static_cast<u64>(last) >> kLinearHostPageShift;
    if (last_host - first_host >= kJitCodePageShards) {
        // Wider than one host page per shard: every shard is visited anyway.
        for (usize i = 0; i < kJitCodePageShards; ++i) {
            if (shard_has(m_jit_code_page_shards[i], first, last)) return true;
        }
        return false;
    }
    for (u64 host = first_host;; ++host) {
        const GuestAddr host_lo = static_cast<GuestAddr>(host)
                                  << kLinearHostPageShift;
        const GuestAddr host_last =
            host_lo + ((GuestAddr{1} << kLinearHostPageShift) - 0x1000);
        if (shard_has(jit_code_page_shard(host_lo), std::max(host_lo, first),
                      std::min(host_last, last))) {
            return true;
        }
        if (host == last_host) break;
    }
    return false;
}

void GuestMemory::add_code_alias(GuestAddr alias_base, GuestAddr target_base,
                                 usize size) {
    if (size == 0) return;
    std::lock_guard<std::mutex> lock(m_code_alias_mutex);
    auto next = std::make_shared<std::vector<CodeAlias>>();
    if (auto current = std::atomic_load(&m_code_aliases)) {
        *next = *current;
    }
    next->push_back(CodeAlias{alias_base, target_base, size});
    std::atomic_store(&m_code_aliases, std::shared_ptr<const std::vector<
                                           CodeAlias>>(std::move(next)));
    m_has_code_aliases.store(true, std::memory_order_release);
    // Cached translations formed before the alias existed still carry the
    // alias window's old executable bit.
    invalidate_tlb();
}

GuestAddr GuestMemory::code_alias_target(GuestAddr addr,
                                         usize size) const noexcept {
    if (!m_has_code_aliases.load(std::memory_order_acquire)) return 0;
    const auto aliases = std::atomic_load(&m_code_aliases);
    if (!aliases) return 0;
    const GuestAddr end = addr + (size == 0 ? 1 : size);
    for (const auto& alias : *aliases) {
        if (end > alias.alias_base &&
            addr < alias.alias_base + alias.size) {
            return alias.target_base + (addr - alias.alias_base);
        }
    }
    return 0;
}

void GuestMemory::notify_executable_write(GuestAddr addr, usize size) {
    // Writes to an executable mapping are not SMC unless they overlap a
    // compiled block. Logging every RWX data store overflows the 4096-slot
    // ring; collect_executable_writes_after then emits the (0, 0) lost-history
    // sentinel and the JIT wipes every translation. Ask the observer first
    // and only publish when it reports a real invalidation — or when this
    // thread has no observer (foreign-thread FlushInstructionCache).
    const bool sentinel = (addr == 0 && size == 0);
    m_images.note_modified(addr, size);
    // A store through a writable alias of executable pages rewrites code the
    // JIT caches under the *aliased* address; report it there.
    if (!sentinel && m_has_code_aliases.load(std::memory_order_acquire)) {
        if (const GuestAddr target = code_alias_target(addr, size);
            target != 0) {
            addr = target;
        }
    }
    ExecutableWriteObserver* observer = nullptr;
    if (t_executable_write_observers_alive) {
        if (t_observer_memo_owner == this) {
            observer = t_observer_memo;
        } else {
            auto& observers = t_executable_write_observers.map;
            auto it = observers.find(this);
            if (it != observers.end()) {
                t_observer_memo_owner = this;
                t_observer_memo = &it->second;
                observer = t_observer_memo;
            }
        }
    }

    // An observer that reports an invalidation has also purged the shared
    // compiled-block records, so other threads need not repeat that purge.
    bool writer_purged = false;
    if (observer != nullptr && !sentinel) {
        const u64 preview =
            m_executable_generation.load(std::memory_order_acquire);
        writer_purged = (*observer)(addr, size, preview);
        if (!writer_purged && !jit_code_pages_overlap(addr, size)) {
            return;
        }
    }

    u64 generation;
    {
        // Reserving a generation before its record was ready made a reader
        // mistake a preempted writer for lost history and flush its whole JIT
        // cache. The same lock prevents a wrapping writer from changing a
        // record halfway through a reader's snapshot. Observers stay outside
        // this lock: they may themselves drain executable-write history.
        std::lock_guard<std::mutex> lock(m_exec_write_log_mutex);
        generation = m_executable_generation.load(std::memory_order_relaxed) + 1;
        auto& slot = m_exec_write_log[generation & kExecWriteLogMask];
        slot.addr.store(addr, std::memory_order_relaxed);
        slot.size.store(size, std::memory_order_relaxed);
        slot.writer_purged.store(writer_purged, std::memory_order_relaxed);
        slot.generation.store(generation, std::memory_order_relaxed);
        m_executable_generation.store(generation, std::memory_order_release);
    }

    if (observer != nullptr && sentinel) {
        (*observer)(addr, size, generation);
    }
}

std::vector<GuestMemory::ExecutableWriteEvent>
GuestMemory::collect_executable_writes_after(u64 after_generation) const {
    std::lock_guard<std::mutex> lock(m_exec_write_log_mutex);
    const u64 now =
        m_executable_generation.load(std::memory_order_acquire);
    if (after_generation == now) {
        return {};
    }
    if (now < after_generation ||
        now - after_generation >= kMaxExecWriteLog) {
        return {ExecutableWriteEvent{now, now, 0, 0}};
    }

    std::vector<ExecutableWriteEvent> out;
    out.reserve(static_cast<usize>(now - after_generation));
    constexpr GuestAddr kPageMask = ~GuestAddr{0xFFF};
    for (u64 generation = after_generation + 1;
         generation <= now; ++generation) {
        const auto& slot =
            m_exec_write_log[generation & kExecWriteLogMask];
        const u64 published = slot.generation.load(std::memory_order_relaxed);
        if (published != generation) {
            return {ExecutableWriteEvent{now, now, 0, 0}};
        }
        const GuestAddr addr =
            slot.addr.load(std::memory_order_relaxed);
        const usize size = static_cast<usize>(
            slot.size.load(std::memory_order_relaxed));
        const bool writer_purged =
            slot.writer_purged.load(std::memory_order_relaxed);
        if (addr == 0 && size == 0) {
            out.clear();
            out.push_back({generation, generation, 0, 0});
            continue;
        }
        if (!out.empty() && out.back().addr != 0 &&
            (out.back().addr & kPageMask) == (addr & kPageMask)) {
            auto& last = out.back();
            const GuestAddr begin = std::min(last.addr, addr);
            const GuestAddr end =
                std::max(last.addr + last.size, addr + size);
            last.addr = begin;
            last.size = static_cast<usize>(end - begin);
            last.generation = generation;
            last.writer_purged = last.writer_purged && writer_purged;
            continue;
        }
        out.push_back({generation, generation, addr, size, writer_purged});
    }
    return out;
}

void GuestMemory::publish_protect_shootdown(GuestAddr addr, usize size,
                                           MemoryProtection protection) {
    const u64 seq =
        m_protect_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& slot = m_protect_log[seq & kProtectShootdownMask];
    slot.addr.store(addr, std::memory_order_relaxed);
    slot.size.store(size, std::memory_order_relaxed);
    slot.protection.store(static_cast<u32>(protection),
                          std::memory_order_relaxed);
    slot.seq.store(seq, std::memory_order_release);
}

usize GuestMemory::copy_protect_shootdowns_after(
    u64 seen, ProtectShootdown* out, usize cap, bool* overflow) const {
    if (overflow) {
        *overflow = false;
    }
    const u64 seq = m_protect_seq.load(std::memory_order_acquire);
    if (seen == seq) {
        return 0;
    }
    if (seq < seen || (seq - seen) >= kProtectShootdownLog) {
        if (overflow) {
            *overflow = true;
        }
        return 0;
    }
    usize n = 0;
    for (u64 s = seen + 1; s <= seq; ++s) {
        const auto& slot = m_protect_log[s & kProtectShootdownMask];
        u64 got = slot.seq.load(std::memory_order_acquire);
        if (got != s) {
            for (int spin = 0; spin < 64 && got != s; ++spin) {
                got = slot.seq.load(std::memory_order_acquire);
            }
            if (got != s) {
                if (overflow) {
                    *overflow = true;
                }
                return 0;
            }
        }
        if (out != nullptr && n < cap) {
            out[n].addr = slot.addr.load(std::memory_order_relaxed);
            out[n].size = static_cast<usize>(
                slot.size.load(std::memory_order_relaxed));
            out[n].protection = static_cast<MemoryProtection>(
                slot.protection.load(std::memory_order_relaxed));
            ++n;
        } else if (out == nullptr || n >= cap) {
            if (overflow) {
                *overflow = true;
            }
            return 0;
        }
    }
    // A publisher announces its sequence before touching a slot. If any
    // publisher raced this copy, reject all records: a wrapped slot may have
    // changed payload while its old slot sequence was still visible. The
    // fence keeps the relaxed payload loads above from being satisfied after
    // this re-check (an acquire load alone orders only what follows it).
    std::atomic_thread_fence(std::memory_order_acquire);
    if (m_protect_seq.load(std::memory_order_acquire) != seq) {
        if (overflow) *overflow = true;
        return 0;
    }
    return n;
}

bool GuestMemory::check_host_overlap(uintptr_t start, uintptr_t end) const {
    std::shared_lock lk(m_regions_mutex);
    bool found = false;
    for (const auto& [base, mapped] : regions_) {
        auto rstart = reinterpret_cast<uintptr_t>(mapped.region.data());
        auto rend = rstart + mapped.region.size();
        if (rstart < end && rend > start) {
            fprintf(stderr, "[overlap] guest=0x%08x size=0x%zx(req)/0x%zx(alloc) "
                    "host=0x%lx-0x%lx OVERLAPS 0x%lx-0x%lx\n",
                    static_cast<unsigned>(base),
                    mapped.requested_size, mapped.region.size(),
                    rstart, rend, start, end);
            found = true;
        }
    }
    return found;
}

} // namespace vitreo
