// SPDX-License-Identifier: LGPL-2.1-only
// Host-side dirty tracking for MEM_WRITE_WATCH.
//
// Windows tracks stores in the PTE. Guest JIT stores write host memory
// directly, so GetWriteWatch needs a side channel: a 1-byte-per-guest-page
// map that inline stores mark (and C++/interpreter record_store also marks).
//
// That side channel only exists where Vitreo executes the store. Under
// native32 the guest's stores run on the CPU, so the map can never fill; that
// path arms clean pages PROT_READ instead and marks them from the fault
// (enable_hardware_traps). Do NOT arm on Apple Silicon: host pages are 16 KiB
// against 4 KiB guest pages, so one trap dirties four guest pages and a
// GC-style ResetWriteWatch loop pays a SIGBUS per first write. On x86_64 the
// host page IS the guest page, so the arming is exact.

#pragma once

#include "vitreo/types.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace vitreo::write_watch {

inline usize host_page_size() noexcept {
    static const usize s = static_cast<usize>(::getpagesize());
    return s;
}

inline constexpr u64 kGuestPage = 4096;

struct Slot {
    std::atomic<u64> base{0};
    std::atomic<u64> size{0};
    std::atomic<u8*> dirty{nullptr};
    std::atomic<u8*> armed{nullptr};
    /// Whether the region was allocated executable. Arming replaces the host
    /// protection outright, so PROT_EXEC has to be carried through both the
    /// armed and the restored state or the guest's own code in a
    /// PAGE_EXECUTE_READWRITE heap (Mono allocates one) stops being runnable.
    std::atomic<bool> executable{false};
};

inline constexpr int kMaxSlots = 64;
inline Slot& slot(int i) noexcept {
    static Slot slots[kMaxSlots];
    return slots[i];
}
inline std::mutex& table_mutex() noexcept {
    static std::mutex m;
    return m;
}
inline std::atomic<int> g_active_count{0};
inline std::atomic<int> g_table_gen{0};
/// Process-wide map pointer. Null when no MEM_WRITE_WATCH region exists so
/// inline stores skip the dirty mark. JIT loads this through JitContext
/// (pointer-to-pointer) so a later register_region is visible on linked
/// tails that never re-enter execute().
inline std::atomic<u8*> g_published_map{nullptr};
inline std::atomic<int>& active_count() noexcept {
    return g_active_count;
}
/// Host base of the guest linear window, valid only while hardware_traps().
///
/// The dirty map is a software side channel: a store marks its page by calling
/// record_store(), which the interpreter ops, the JIT helpers and host-side
/// writes all do. Native 32-bit execution has no such hook -- the guest's
/// stores run directly on the CPU and are never seen -- so the tracked bits
/// would stay empty however much the guest wrote.
///
/// Neither answer a software-only map can give is acceptable there. An empty
/// set frees live objects (see docs: GetWriteWatch). But answering with the
/// conservative superset of committed pages is not the safe fallback it looks
/// like: WRITE_WATCH_FLAG_RESET then never shrinks the answer, and a collector
/// that drains the buffer with `do { ... } while (count == capacity)` -- which
/// is exactly what Boehm's GC_gww_read_dirty does -- loops forever.
///
/// So native32 must track for real: clean committed pages are armed PROT_READ
/// and the resulting protection fault marks the page and restores write access.
/// Under identity mapping the base is 0, which is why validity is a separate
/// flag rather than a null check.
///
/// Held as an integer, not a pointer: under identity mapping it is 0, and
/// `nullptr + addr` is undefined even where the arithmetic is right.
/// Monotone envelope of every write-watch region ever registered. Widened on
/// register_region, never narrowed on unregister: shrinking it could only ever
/// turn a "watched" answer into "not watched", which is the under-reporting
/// direction. A stale-wide envelope costs one slot scan on a store that already
/// missed the JIT inline path.
inline std::atomic<u64> g_watch_lo{~0ull};
inline std::atomic<u64> g_watch_hi{0};

/// Generation of the slot table. Bumped by every register/unregister, so a
/// consumer holding a cached decision (the JIT's X64MemSlot table) can notice
/// that the decision may no longer hold.
[[nodiscard]] inline int table_generation() noexcept {
    return g_table_gen.load(std::memory_order_acquire);
}

inline std::atomic<bool> g_hw_traps{false};
inline std::atomic<uintptr_t> g_host_base{0};

inline void enable_hardware_traps(uintptr_t host_base) noexcept {
    g_host_base.store(host_base, std::memory_order_release);
    g_hw_traps.store(true, std::memory_order_release);
}

[[nodiscard]] inline bool hardware_traps() noexcept {
    return g_hw_traps.load(std::memory_order_acquire);
}

[[nodiscard]] inline uintptr_t host_base() noexcept {
    return g_host_base.load(std::memory_order_acquire);
}

/// Legacy fallback: answer GetWriteWatch with the conservative superset of
/// committed pages because nothing tracks the stores.
///
/// This is NOT a safe fallback and exists only behind
/// VITREO_WRITE_WATCH_TRAPS=0, to A/B the hardware path against the behaviour
/// that shipped before it. With WRITE_WATCH_FLAG_RESET the answer never
/// shrinks, so a caller that drains the buffer in a loop never terminates.
inline std::atomic<bool> g_stores_untracked{false};

inline void set_stores_untracked(bool value) noexcept {
    g_stores_untracked.store(value, std::memory_order_relaxed);
}

[[nodiscard]] inline bool stores_untracked() noexcept {
    // VITREO_WRITE_WATCH_SUPERSET=1 forces the conservative answer on any
    // architecture, not just native32. It exists to A/B one specific
    // hypothesis: native32 (which works) reports the committed superset,
    // while the JIT path relies on every store marking a page, and a single
    // unmarked store makes a guest collector free a live object with no error
    // anywhere. Turning this on trades that silence for the drain-loop risk
    // described above, so it is a diagnostic, never a default.
    static const bool forced = [] {
        const char* e = std::getenv("VITREO_WRITE_WATCH_SUPERSET");
        return e != nullptr && e[0] == '1';
    }();
    return forced || g_stores_untracked.load(std::memory_order_relaxed);
}

/// Protection faults recovered as write-watch traps. The one number that
/// distinguishes "the guest wrote nothing" from "we are not seeing the
/// writes" -- an under-report is what makes a guest collector free live
/// objects, and it is otherwise silent.
inline std::atomic<u64> g_traps_recovered{0};

[[nodiscard]] inline u64 traps_recovered() noexcept {
    return g_traps_recovered.load(std::memory_order_relaxed);
}

[[nodiscard]] inline int armed_prot(bool executable) noexcept {
    return executable ? (PROT_READ | PROT_EXEC) : PROT_READ;
}

[[nodiscard]] inline int writable_prot(bool executable) noexcept {
    return executable ? (PROT_READ | PROT_WRITE | PROT_EXEC)
                      : (PROT_READ | PROT_WRITE);
}

/// Host page holding @p guest_addr, as a void* for mprotect.
[[nodiscard]] inline void* host_page_for(u64 guest_addr) noexcept {
    const usize hp = host_page_size();
    const u64 aligned = guest_addr & ~(static_cast<u64>(hp) - 1u);
    return reinterpret_cast<void*>(host_base() +
                                   static_cast<uintptr_t>(aligned));
}

[[nodiscard]] inline bool any_active() noexcept {
    return g_active_count.load(std::memory_order_relaxed) != 0;
}

/// True when [begin, end) overlaps any live MEM_WRITE_WATCH region.
///
/// The JIT's inline store paths write host memory with no hook of any kind, so
/// a region that is being watched must never be published into one -- the store
/// would land silently and GetWriteWatch would call the page clean. That is the
/// bug that frees live objects out of a guest GC (docs: getwritewatch_gc_landmine).
/// Callers are slow paths only (a store that already missed the inline path, or
/// block entry), so the scan cost is amortized to nothing.
[[nodiscard]] inline bool range_is_watched(u64 begin, u64 end) noexcept {
    if (!any_active() || end <= begin) return false;
    if (end <= g_watch_lo.load(std::memory_order_relaxed) ||
        begin >= g_watch_hi.load(std::memory_order_relaxed)) {
        return false;
    }
    for (int i = 0; i < kMaxSlots; ++i) {
        const u64 b = slot(i).base.load(std::memory_order_acquire);
        if (b == 0) continue;
        const u64 e = b + slot(i).size.load(std::memory_order_acquire);
        if (begin < e && b < end) return true;
    }
    return false;
}

[[nodiscard]] inline std::atomic<u8*>* published_map_slot() noexcept {
    return &g_published_map;
}

/// Hook by which a linear reservation learns which host pages belong to a live
/// MEM_WRITE_WATCH allocation, so the x64 guest-base store can route those and
/// only those into the recording helper (`kLinearCheckWatched`).
///
/// This lives HERE, on register_region/unregister_region, and not at their
/// caller. Publication has to be inseparable from the act of making a region
/// watched: a caller that registers a region without publishing would leave
/// direct stores going straight to memory, and an unreported dirty page makes a
/// guest GC free a live object. Two tests register regions directly and caught
/// exactly that when the publication lived one level up.
using PagePublisher = void (*)(void* ctx, u64 base, u64 size, bool set);
inline std::atomic<PagePublisher> g_page_publisher{nullptr};
inline std::atomic<void*> g_page_publisher_ctx{nullptr};

inline void set_page_publisher(PagePublisher fn, void* ctx) noexcept {
    g_page_publisher_ctx.store(ctx, std::memory_order_relaxed);
    g_page_publisher.store(fn, std::memory_order_release);
}

/// Uninstall, but only if @p ctx is still the installed owner. A GuestMemory is
/// created and destroyed per test; clearing unconditionally would let a dead
/// instance uninstall a live one's hook.
inline void clear_page_publisher(void* ctx) noexcept {
    if (g_page_publisher_ctx.load(std::memory_order_relaxed) != ctx) return;
    g_page_publisher.store(nullptr, std::memory_order_release);
    g_page_publisher_ctx.store(nullptr, std::memory_order_relaxed);
}

inline void publish_watched_pages(u64 base, u64 size, bool set) noexcept {
    const PagePublisher fn = g_page_publisher.load(std::memory_order_acquire);
    if (fn == nullptr || size == 0) return;
    fn(g_page_publisher_ctx.load(std::memory_order_relaxed), base, size, set);
}

/// 4 GiB of 4 KiB pages. One byte per page so concurrent stores to different
/// pages cannot lose a dirty bit (a packed bitmap would race on the byte).
inline u8* linear_page_dirty_map() noexcept {
    static u8 pages[1u << 20];
    return pages;
}

inline void refresh_published_map() noexcept {
    g_published_map.store(
        any_active() ? linear_page_dirty_map() : nullptr,
        std::memory_order_release);
}

inline void mark_linear_pages(u64 begin, u64 size) noexcept {
    if (size == 0) return;
    u8* map = linear_page_dirty_map();
    const u64 end = begin + size;
    for (u64 page = begin & ~(kGuestPage - 1u); page < end; page += kGuestPage) {
        const u64 idx = page / kGuestPage;
        if (idx < (1u << 20)) {
            map[idx] = 1;
        }
    }
}

inline bool test_linear_page(u64 page) noexcept {
    const u64 idx = page / kGuestPage;
    if (idx >= (1u << 20)) return false;
    return linear_page_dirty_map()[idx] != 0;
}

inline void clear_linear_pages(u64 begin, u64 end) noexcept {
    if (end <= begin) return;
    u8* map = linear_page_dirty_map();
    for (u64 page = begin & ~(kGuestPage - 1u); page < end; page += kGuestPage) {
        const u64 idx = page / kGuestPage;
        if (idx < (1u << 20)) {
            map[idx] = 0;
        }
    }
}

inline void set_bit(u8* bits, u64 index) noexcept {
    __atomic_or_fetch(&bits[index / 8], static_cast<u8>(1u << (index % 8)),
                      __ATOMIC_RELAXED);
}
inline void clear_bit(u8* bits, u64 index) noexcept {
    __atomic_and_fetch(&bits[index / 8],
                       static_cast<u8>(static_cast<u8>(~(1u << (index % 8)))),
                       __ATOMIC_RELAXED);
}
inline bool test_bit(const u8* bits, u64 index) noexcept {
    return (__atomic_load_n(&bits[index / 8], __ATOMIC_RELAXED) &
            (1u << (index % 8))) != 0;
}

inline Slot* find_slot(u64 addr) noexcept {
    struct Last {
        Slot* slot{nullptr};
        u64 base{0};
        u64 size{0};
        u64 miss_page{~0ull};
        int gen{-1};
    };
    static thread_local Last last;
    const int gen = g_table_gen.load(std::memory_order_acquire);
    if (last.gen != gen) {
        last.slot = nullptr;
        last.base = 0;
        last.size = 0;
        last.miss_page = ~0ull;
        last.gen = gen;
    }
    if (last.slot != nullptr && addr >= last.base &&
        addr - last.base < last.size) {
        return last.slot;
    }
    const u64 page = addr & ~(kGuestPage - 1u);
    if (last.slot == nullptr && last.miss_page == page) {
        return nullptr;
    }
    for (int i = 0; i < kMaxSlots; ++i) {
        const u64 base = slot(i).base.load(std::memory_order_acquire);
        if (base == 0) continue;
        const u64 size = slot(i).size.load(std::memory_order_acquire);
        if (addr >= base && addr - base < size) {
            last.slot = &slot(i);
            last.base = base;
            last.size = size;
            last.miss_page = ~0ull;
            return last.slot;
        }
    }
    last.slot = nullptr;
    last.miss_page = page;
    return nullptr;
}

inline void unprotect_host_page(u64 guest_addr, bool executable) noexcept {
    if (!hardware_traps()) return;
    (void)mprotect(host_page_for(guest_addr), host_page_size(),
                   writable_prot(executable));
}

/// Restore RW on every host page overlapping [guest_begin, guest_end).
/// Apple Silicon host pages are 16 KiB; a CRT memset/memcpy can span several.
inline void unprotect_host_span(u64 guest_begin, u64 guest_end,
                                bool executable) noexcept {
    if (!hardware_traps() || guest_end <= guest_begin) return;
    const usize hp = host_page_size();
    u64 aligned = guest_begin & ~(static_cast<u64>(hp) - 1u);
    for (; aligned < guest_end; aligned += hp) {
        (void)mprotect(host_page_for(aligned), hp, writable_prot(executable));
    }
}

/// Discard native32 pages without opening a writable interval in an armed
/// MEM_WRITE_WATCH mapping. MADV_FREE preserves the mapping and its PROT_READ
/// trap while making the old contents disposable. A later read may see the
/// old bytes or zeroes; both satisfy DiscardVirtualMemory's undefined-content
/// contract. Callers use ordinary zeroing when hardware traps are disabled.
[[nodiscard]] inline bool discard_armed_pages(void* contents,
                                              usize size) noexcept {
    if (!hardware_traps() || host_page_size() != kGuestPage || size == 0) {
        return false;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(contents);
    if ((address % host_page_size()) != 0 ||
        (size % host_page_size()) != 0) {
        return false;
    }
    return ::madvise(contents, size, MADV_FREE) == 0;
}

/// Record a store into a write-watch region. Marks guest pages dirty and
/// restores host write permission on every overlapping host page so a
/// subsequent C++ memset/memcpy cannot SIGBUS on a still-armed neighbor.
inline void record_store(GuestAddr addr, usize size) noexcept {
    if (!any_active() || size == 0) {
        return;
    }
    const u64 begin = static_cast<u64>(addr);
    const u64 end = begin + static_cast<u64>(size);
    if (end <= begin) return; // Callers validate guest-address overflow.

    // Learn GetWriteWatch: report every written page, including watched
    // allocations wholly inside a bulk store with unwatched endpoints.
    const auto record_overlap = [begin, end](Slot& entry) {
        const u64 base = entry.base.load(std::memory_order_acquire);
        if (base == 0) return;
        const u64 limit = base + entry.size.load(std::memory_order_relaxed);
        const u64 first = begin > base ? begin : base;
        const u64 last = end < limit ? end : limit;
        if (first >= last) return;
        u8* dirty = entry.dirty.load(std::memory_order_acquire);
        u8* armed = entry.armed.load(std::memory_order_acquire);
        if (dirty == nullptr || armed == nullptr) return;
        mark_linear_pages(first, last - first);
        const bool traps = hardware_traps();
        bool need_unprotect = false;
        for (u64 page = first & ~(kGuestPage - 1u); page < last;
             page += kGuestPage) {
            const u64 index = (page - base) / kGuestPage;
            set_bit(dirty, index);
            if (traps && test_bit(armed, index)) {
                clear_bit(armed, index);
                need_unprotect = true;
            }
        }
        if (need_unprotect) {
            unprotect_host_span(first, last,
                entry.executable.load(std::memory_order_relaxed));
        }
    };

    // Keep the cached lookup fast path for scalar and in-allocation stores.
    Slot* first = find_slot(begin);
    if (first != nullptr) {
        const u64 base = first->base.load(std::memory_order_relaxed);
        if (end <= base + first->size.load(std::memory_order_relaxed)) {
            record_overlap(*first);
            return;
        }
    } else if ((begin / kGuestPage) == ((end - 1) / kGuestPage)) {
        return; // Watched allocations are guest-page aligned.
    }
    for (int i = 0; i < kMaxSlots; ++i) record_overlap(slot(i));
}

/// SIGSEGV/SIGBUS write to a write-watch page: mark dirty, restore RW, retry.
/// Returns true when the fault was a write-watch trap (not a real AV).
///
/// Recover when the page is still armed, or already dirty: record_store may
/// have cleared the armed bit for a whole memset span while leaving a later
/// 16 KiB host page PROT_READ. Dirty + leftover PROT_READ is not a guest AV.
inline bool handle_fault(GuestAddr addr) noexcept {
    if (active_count().load(std::memory_order_relaxed) == 0) {
        return false;
    }
    Slot* s = find_slot(static_cast<u64>(addr));
    if (s == nullptr) return false;
    u8* armed = s->armed.load(std::memory_order_acquire);
    u8* dirty = s->dirty.load(std::memory_order_acquire);
    const u64 base = s->base.load(std::memory_order_relaxed);
    const u64 region_size = s->size.load(std::memory_order_relaxed);
    if (armed == nullptr || dirty == nullptr) return false;
    const u64 page = static_cast<u64>(addr) & ~(kGuestPage - 1u);
    if (page < base || page - base >= region_size) return false;
    const u64 index = (page - base) / kGuestPage;
    const bool was_armed = test_bit(armed, index);
    const bool was_dirty = test_bit(dirty, index);
    if (!was_armed && !was_dirty) return false;
    clear_bit(armed, index);
    set_bit(dirty, index);
    g_traps_recovered.fetch_add(1, std::memory_order_relaxed);
    mark_linear_pages(page, kGuestPage);
    unprotect_host_page(page, s->executable.load(std::memory_order_relaxed));
    return true;
}

inline void register_region(u64 base, u64 size, u8* dirty, u8* armed,
                            bool executable) noexcept {
    std::lock_guard<std::mutex> lock(table_mutex());
    int empty = -1;
    for (int i = 0; i < kMaxSlots; ++i) {
        const u64 slot_base = slot(i).base.load(std::memory_order_relaxed);
        if (slot_base == base) {
            // Re-registration with a new size: publish the new range before
            // retiring the old one, so no host page in the intersection is
            // momentarily unwatched.
            const u64 old_size = slot(i).size.load(std::memory_order_relaxed);
            if (old_size != size) {
                publish_watched_pages(base, size, /*set=*/true);
                publish_watched_pages(base, old_size, /*set=*/false);
            }
            slot(i).size.store(size, std::memory_order_relaxed);
            slot(i).dirty.store(dirty, std::memory_order_relaxed);
            slot(i).armed.store(armed, std::memory_order_relaxed);
            slot(i).executable.store(executable, std::memory_order_relaxed);
            g_table_gen.fetch_add(1, std::memory_order_release);
            return;
        }
        if (empty < 0 && slot_base == 0) {
            empty = i;
        }
    }
    if (empty < 0) return;
    slot(empty).size.store(size, std::memory_order_relaxed);
    slot(empty).dirty.store(dirty, std::memory_order_relaxed);
    slot(empty).armed.store(armed, std::memory_order_relaxed);
    slot(empty).executable.store(executable, std::memory_order_relaxed);
    // Before the slot goes live, so a store can never see the region watched
    // while its host page still says "direct access is fine".
    publish_watched_pages(base, size, /*set=*/true);
    slot(empty).base.store(base, std::memory_order_release);
    // Widen the envelope before publishing "active": a reader that sees
    // any_active() must already see an envelope wide enough to contain this
    // region, or range_is_watched would reject it.
    if (base < g_watch_lo.load(std::memory_order_relaxed)) {
        g_watch_lo.store(base, std::memory_order_relaxed);
    }
    if (base + size > g_watch_hi.load(std::memory_order_relaxed)) {
        g_watch_hi.store(base + size, std::memory_order_relaxed);
    }
    active_count().fetch_add(1, std::memory_order_release);
    refresh_published_map();
    g_table_gen.fetch_add(1, std::memory_order_release);
}

inline void unregister_region(u64 base) noexcept {
    std::lock_guard<std::mutex> lock(table_mutex());
    for (int i = 0; i < kMaxSlots; ++i) {
        if (slot(i).base.load(std::memory_order_relaxed) == base) {
            const u64 size = slot(i).size.load(std::memory_order_relaxed);
            slot(i).base.store(0, std::memory_order_release);
            slot(i).dirty.store(nullptr, std::memory_order_relaxed);
            slot(i).armed.store(nullptr, std::memory_order_relaxed);
            slot(i).size.store(0, std::memory_order_relaxed);
            // After the slot is dead: while the bit lingers a store merely
            // takes the helper, which is always correct.
            publish_watched_pages(base, size, /*set=*/false);
            active_count().fetch_sub(1, std::memory_order_release);
            refresh_published_map();
            g_table_gen.fetch_add(1, std::memory_order_release);
            return;
        }
    }
}

/// Retire every registered region and drop the linear dirty map.
///
/// The registry holds RAW pointers into storage its registrant owns (the
/// `AllocInfo::write_watch_dirty/armed` vectors inside a VirtualMemoryManager).
/// Registration is undone by VirtualFree and by nothing else, so a registrant
/// that dies while a region is still published leaves the slot pointing at
/// freed memory -- and the next `record_store()` anywhere in that guest range
/// atomically ORs a bit into the freed block. In a long-lived guest process the
/// manager outlives everything, but a unit-test binary creates and destroys one
/// per test, which is why the corruption only ever showed up there.
///
/// Callers that tear a registrant down without freeing its regions use this to
/// return the process-wide registry to its initial state.
inline void reset_all() noexcept {
    std::lock_guard<std::mutex> lock(table_mutex());
    for (int i = 0; i < kMaxSlots; ++i) {
        const u64 base = slot(i).base.load(std::memory_order_relaxed);
        if (base == 0) continue;
        const u64 size = slot(i).size.load(std::memory_order_relaxed);
        slot(i).base.store(0, std::memory_order_release);
        slot(i).dirty.store(nullptr, std::memory_order_relaxed);
        slot(i).armed.store(nullptr, std::memory_order_relaxed);
        slot(i).size.store(0, std::memory_order_relaxed);
        slot(i).executable.store(false, std::memory_order_relaxed);
        publish_watched_pages(base, size, /*set=*/false);
    }
    g_active_count.store(0, std::memory_order_release);
    g_watch_lo.store(~0ull, std::memory_order_relaxed);
    g_watch_hi.store(0, std::memory_order_relaxed);
    u8* map = linear_page_dirty_map();
    for (u64 i = 0; i < (1u << 20); ++i) map[i] = 0;
    refresh_published_map();
    g_table_gen.fetch_add(1, std::memory_order_release);
}

/// Arm clean committed pages as PROT_READ so the next store faults.
inline void arm_range(u64 base, u64 begin, u64 end, u8* dirty, u8* armed,
                      bool executable) noexcept {
    if (dirty == nullptr || armed == nullptr || end <= begin) return;
    // Without hardware traps there is nothing to arm. Setting armed bits here
    // would only make record_store test them on every store.
    if (!hardware_traps()) return;
    const u64 region_begin = begin;
    for (u64 page = begin & ~(kGuestPage - 1u); page < end; page += kGuestPage) {
        if (page < base) continue;
        const u64 index = (page - base) / kGuestPage;
        if (test_bit(dirty, index)) continue;
        set_bit(armed, index);
    }
    const usize hp = host_page_size();
    // Only mprotect host pages fully inside the armed range. Rounding down
    // would PROT_READ neighboring non-watch (or uncommitted) 4 KiB pages and
    // turn a C++ store into a fatal SIGBUS.
    const u64 hp64 = static_cast<u64>(hp);
    u64 host = (region_begin + hp64 - 1u) & ~(hp64 - 1u);
    for (; host + hp64 <= end; host += hp64) {
        (void)mprotect(host_page_for(host), hp, armed_prot(executable));
    }
}

}  // namespace vitreo::write_watch
